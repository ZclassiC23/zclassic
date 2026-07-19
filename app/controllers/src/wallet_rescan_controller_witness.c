/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * rescanwitnesses RPC: rebuild Sapling Merkle witnesses for unspent notes. */

#include "controllers/wallet_rescan_controller_internal.h"

static bool uint256_is_zero_local(const struct uint256 *v)
{
    static const uint8_t zero[32] = {0};
    return !v || memcmp(v->data, zero, sizeof(v->data)) == 0;
}

bool rescan_result_consensus_valid(const struct uint256 *our_root,
                                   const struct uint256 *header_root,
                                   int witness_mismatches)
{
    if (!our_root || !header_root)
        return false;
    if (witness_mismatches != 0)
        return false;
    if (uint256_is_zero_local(header_root))
        return false;
    return memcmp(our_root->data, header_root->data,
                  sizeof(our_root->data)) == 0;
}

bool rpc_rescanwitnesses(const struct json_value *params, bool help,
	                                  struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "rescanwitnesses\n"
        "Rebuild Sapling Merkle witnesses for all unspent shielded notes.\n"
        "Required before spending z→z or z→t. Replays the commitment tree\n"
        "from the Sapling activation height to tip.");

    ENSURE_WALLET(result);
    if (!ctx->main_state) {
        json_set_str(result, "Main state not available");
        return false;
    }
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Node database not available");
        return false;
    }
    if (!ctx->datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }

    /* Load ALL unspent notes that need witnesses (no fixed cap — a 256-cap
     * here meant the recovery path could not rebuild witnesses for notes ranked
     * beyond #256, leaving them permanently unspendable). */
    struct db_sapling_note *notes = NULL;
    int n_notes = db_sapling_note_list_unspent_alloc(ctx->node_db, &notes);
    if (n_notes < 0) {
        json_set_str(result, "Failed to load unspent notes");
        return false;
    }
    if (n_notes == 0) {
        free(notes);
        json_set_object(result);
        json_push_kv_int(result, "notes_updated", 0);
        json_push_kv_str(result, "status", "no unspent notes");
        return true;
    }

    printf("rescanwitnesses: building witnesses for %d notes...\n", n_notes);
    fflush(stdout);

    /* Prevent sync_controller from overwriting Sapling tree during rescan */
    extern _Atomic bool g_sapling_rescan_active;
    atomic_store(&g_sapling_rescan_active, true);

    int chain_tip = active_chain_height(&ctx->main_state->chain_active);
    int sapling_start = 476969; /* Sapling activation on ZClassic mainnet */

    /* Initialize empty tree and per-note witness state */
    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);

    struct incremental_witness *witnesses = zcl_calloc((size_t)n_notes,
        sizeof(struct incremental_witness), "rescan witnesses");
    bool *witness_active = zcl_calloc((size_t)n_notes, sizeof(bool), "rescan witness active");
    if (!witnesses || !witness_active) {
        /* zcl_calloc already logged the OOM. Match the function's existing
         * cleanup convention (see the diverged-tree refusal path): free all
         * three arrays, release the global rescan latch, and fail the RPC. */
        free(witnesses);
        free(witness_active);
        free(notes);
        atomic_store(&g_sapling_rescan_active, false);
        json_set_str(result, "Out of memory building witness arrays");
        return false;
    }
    int witnesses_built = 0;

    /* mmap cache */
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    int64_t t_start = (int64_t)platform_time_wall_time_t();
    int blocks_scanned = 0;
    size_t total_commitments = 0;

    /* Stop at the immutable height to avoid reading blocks the C++ node
     * may still be writing to shared blk*.dat files. The remaining
     * blocks will be handled by normal connect_block processing. */
    int safe_tip = zcl_immutable_height(chain_tip);
    if (safe_tip < sapling_start) safe_tip = chain_tip;

    for (int h = sapling_start; h <= safe_tip; h++) {
        const struct block_index *pindex =
            active_chain_at(&ctx->main_state->chain_active, h);
        if (!pindex) continue;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) continue;

        /* mmap block file */
        if (pindex->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            char path[512];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     ctx->datadir, pindex->nFile);
            int fd = open(path, O_RDONLY);
            if (fd < 0) { cached_data = NULL; cached_file = -1; continue; }
            struct stat fst;
            if (fstat(fd, &fst) != 0) { close(fd); continue; }
            cached_size = (size_t)fst.st_size;
            cached_data = mmap(NULL, cached_size,
                               PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (cached_data == MAP_FAILED) {
                cached_data = NULL; cached_file = -1; continue;
            }
            /* Advise kernel: sequential read, prefetch entire file */
            posix_madvise(cached_data, cached_size,
                          POSIX_MADV_SEQUENTIAL);
            posix_madvise(cached_data, cached_size,
                          POSIX_MADV_WILLNEED);
            cached_file = pindex->nFile;
        }
        if (!cached_data || pindex->nDataPos >= cached_size) continue;

        /* Fast-scan: extract Sapling commitments without full
         * block deserialization. 1000x faster — skips scriptSig parsing
         * for blocks with thousands of inputs. */
        uint8_t block_cms[4096][32];
        size_t block_data_len = cached_size - pindex->nDataPos;
        int n_cms = fast_scan_sapling_commitments(
            cached_data + pindex->nDataPos, block_data_len,
            block_cms, 4096);

        for (int ci = 0; ci < n_cms; ci++) {
            struct uint256 cm;
            memcpy(cm.data, block_cms[ci], 32);

            /* Advance all active witnesses */
            for (int ni = 0; ni < n_notes; ni++) {
                if (witness_active[ni])
                    incremental_witness_append(&witnesses[ni], &cm);
            }

            /* Append to tree */
            incremental_tree_append(&tree, &cm);
            total_commitments++;

            /* Check if this cm matches any note's commitment */
            for (int ni = 0; ni < n_notes; ni++) {
                if (witness_active[ni]) continue;
                if (memcmp(cm.data, notes[ni].cm, 32) == 0) {
                    incremental_witness_init(&witnesses[ni], &tree);
                    witness_active[ni] = true;
                    witnesses_built++;
                }
            }
        }
        blocks_scanned++;

        /* Checkpoint: compare our tree root vs block header.
         * Every 100K blocks normally, every 1000 heights in last 10K. */
        bool do_ckpt = (blocks_scanned % 100000 == 0) ||
                       (h > safe_tip - 10000 && h % 1000 == 0);
        if (do_ckpt) {
            int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
            printf("rescanwitnesses: %d blocks (height %d), "
                   "%zu cms, %d/%d witnesses, %llds",
                   blocks_scanned, h, total_commitments,
                   witnesses_built, n_notes, (long long)elapsed);

            struct uint256 our_root;
            incremental_tree_root(&tree, &our_root);
            if (memcmp(our_root.data,
                       pindex->hashFinalSaplingRoot.data, 32) == 0) {
                printf(" [tree OK]\n");
            } else {
                char oh[65], bh[65];
                uint256_get_hex(&our_root, oh);
                uint256_get_hex(&pindex->hashFinalSaplingRoot, bh);
                printf(" [TREE DIVERGED!]\n"
                       "  our root:   %s (size=%zu)\n"
                       "  block root: %s\n",
                       oh, incremental_tree_size(&tree), bh);
            }
            fflush(stdout);
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    /* Binary search for divergence point: check tree root at last checkpoint
     * that passed (3036968) vs block header. We know tree matches there.
     * Log our total commitment count at save height for comparison. */
    printf("rescanwitnesses: total commitments: %zu at save height %d\n",
           total_commitments, safe_tip);
    fflush(stdout);

    struct uint256 final_tree_root;
    incremental_tree_root(&tree, &final_tree_root);
    struct uint256 final_header_root;
    memset(&final_header_root, 0, sizeof(final_header_root));

    /* Verify tree root matches block header at save height */
    {
        const struct block_index *save_block =
            active_chain_at(&ctx->main_state->chain_active, safe_tip);
        if (save_block) {
            final_header_root = save_block->hashFinalSaplingRoot;
            char oh[65], bh[65];
            uint256_get_hex(&final_tree_root, oh);
            uint256_get_hex(&final_header_root, bh);
            if (memcmp(final_tree_root.data, final_header_root.data, 32) == 0) {
                printf("rescanwitnesses: FINAL tree root matches block header at height %d ✓\n", safe_tip);
            } else {
                printf("rescanwitnesses: FINAL tree root DOES NOT match block header at height %d!\n"
                       "  our root:   %s (size=%zu)\n"
                       "  block root: %s\n",
                       safe_tip, oh, incremental_tree_size(&tree), bh);
            }
            fflush(stdout);
        } else {
            printf("rescanwitnesses: FINAL tree root cannot be checked at height %d (missing active-chain block)\n",
                   safe_tip);
            fflush(stdout);
        }
    }

    /* Verify witness roots match tree root BEFORE saving */
    int witness_root_mismatches = 0;
    {
        char tr_hex[65]; uint256_get_hex(&final_tree_root, tr_hex);
        for (int ni = 0; ni < n_notes; ni++) {
            if (!witness_active[ni]) continue;
            struct uint256 wr;
            incremental_witness_root(&witnesses[ni], &wr);
            char wr_hex[65]; uint256_get_hex(&wr, wr_hex);
            if (memcmp(wr.data, final_tree_root.data, 32) != 0) {
                witness_root_mismatches++;
                printf("rescanwitnesses: WITNESS ROOT MISMATCH for note %d!\n"
                    "  tree root:    %s (size=%zu)\n"
                    "  witness root: %s (fills=%zu)\n",
                    ni, tr_hex, incremental_tree_size(&tree),
                    wr_hex, witnesses[ni].num_filled);
            } else {
                printf("rescanwitnesses: note %d witness root MATCHES tree ✓\n", ni);
            }
        }
        fflush(stdout);
    }

    if (!rescan_result_consensus_valid(&final_tree_root, &final_header_root,
                                       witness_root_mismatches)) {
        char our_hex[65], header_hex[65];
        uint256_get_hex(&final_tree_root, our_hex);
        uint256_get_hex(&final_header_root, header_hex);
        printf("rescanwitnesses: refusing to save diverged Sapling tree "
               "(height=%d mismatches=%d our=%s header=%s)\n",
               safe_tip, witness_root_mismatches, our_hex, header_hex);
        fflush(stdout);

        free(witnesses);
        free(witness_active);
        free(notes);
        atomic_store(&g_sapling_rescan_active, false);

        json_set_object(result);
        json_push_kv_str(result, "status", "diverged");
        json_push_kv_str(result, "message",
                         "Sapling tree diverged from consensus header root; refusing to save rescan tree or witnesses");
        json_push_kv_int(result, "height", safe_tip);
        json_push_kv_str(result, "our_root", our_hex);
        json_push_kv_str(result, "header_root", header_hex);
        json_push_kv_int(result, "witness_root_mismatches",
                         witness_root_mismatches);
        return false;
    }

    /* Save the authoritative tree state to node_state.
     * This replaces any incomplete tree from catchup.
     * Tree is saved at safe_tip height — subsequent connect_block
     * calls will load it and extend naturally for remaining blocks. */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(&tree, &ts);
        /* Save to the normal key + "sapling_tree_rebuild_height" as ONE
         * atomic pair (sapling_tree_persist_pair, lane/sapling-tree-persist)
         * — this is the SAME height key config/src/boot.c's loader and
         * sapling_tree_rebuild() trust to resume/fold-forward from, and the
         * consensus check just above (rescan_result_consensus_valid) already
         * proved final_tree_root == the real header root at safe_tip, so
         * safe_tip is a legitimate saved-height binding. Also save to the
         * rescan-specific key (can't be overwritten by connect_block). */
        sapling_tree_persist_pair(ctx->node_db, ts.data, ts.size,
                                  (int64_t)safe_tip);
        node_db_state_set(ctx->node_db, "sapling_tree_rescan", ts.data, ts.size);

        printf("rescanwitnesses: tree saved (%zu bytes, %zu cms)\n",
               ts.size, incremental_tree_size(&tree));
        fflush(stdout);
        stream_free(&ts);

        char height_str[16];
        snprintf(height_str, sizeof(height_str), "%d", safe_tip);
        node_db_state_set(ctx->node_db, "sapling_tree_height",
                          (uint8_t *)height_str, strlen(height_str));
        node_db_state_set(ctx->node_db, "sapling_tree_rescan_height",
                          (uint8_t *)height_str, strlen(height_str));
    }

    /* Serialize and save witnesses (BEFORE releasing the rescan lock) */
    int saved = 0;
    for (int ni = 0; ni < n_notes; ni++) {
        if (!witness_active[ni]) continue;

        struct byte_stream ws;
        stream_init(&ws, 4096);
        if (incremental_witness_serialize(&witnesses[ni], &ws)) {
            db_sapling_note_save_witness(ctx->node_db,
                notes[ni].txid, notes[ni].output_index,
                ws.data, ws.size, safe_tip);
            saved++;
        }
        stream_free(&ws);
    }

    free(witnesses);
    free(witness_active);
    free(notes);

    /* NOW release the rescan lock — tree and witnesses are all saved */
    atomic_store(&g_sapling_rescan_active, false);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    printf("rescanwitnesses: done in %llds — %zu cms, %d/%d witnesses, "
           "%d saved\n",
           (long long)elapsed, total_commitments, witnesses_built,
           n_notes, saved);
    fflush(stdout);

    json_set_object(result);
    json_push_kv_int(result, "blocks_scanned", blocks_scanned);
    json_push_kv_int(result, "notes_total", n_notes);
    json_push_kv_int(result, "witnesses_built", witnesses_built);
    json_push_kv_int(result, "witnesses_saved", saved);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}
