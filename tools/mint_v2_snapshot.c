/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * mint_v2_snapshot — write a VERSION-2 ZCLUTXO snapshot that embeds the
 * Sapling commitment-tree frontier at the seed height, so a FRESH node can
 * seed coins_kv AND install a verified Sapling tree WITHOUT the multi-GB
 * blocks/ directory.
 *
 * It reuses the node's own machinery (load_block_index_flat +
 * sapling_tree_rebuild) on a datadir that HAS blocks/ + block_index.bin:
 *
 *   1. progress_store_open(datadir)          — the coins_kv home.
 *   2. load_block_index_flat(datadir, &ms)   — in-memory block index map.
 *   3. install the active tip at the seed height + extend the window so
 *      active_chain_at(seed_h) resolves the PoW-proven block_index.
 *   4. node_db_open(<datadir>/node.db)        — node_state["sapling_tree"] home.
 *   5. clear any stale sapling_tree resume markers, then drive
 *      sapling_tree_rebuild(): it replays note commitments from Sapling
 *      activation up to the (capped) endpoint and VERIFIES the final root
 *      against hashFinalSaplingRoot at the seed height, persisting the
 *      serialized frontier into node_state["sapling_tree"].
 *   6. read that frontier blob back + write the v2 snapshot via
 *      coins_kv_snapshot_write_v2 (UTXO records + [u32 len][frontier]).
 *
 * The seed height must be <= the durable coins-applied frontier
 * (coins_applied_height - 1) so the UTXO set and the frontier are coherent at
 * the SAME height. sapling_tree_rebuild already caps its endpoint to the
 * coins-applied frontier; we pass the requested height and let it cap.
 *
 * Usage:
 *   mint_v2_snapshot <datadir> <seed_height> <out_path>
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>

#include <sqlite3.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "models/database.h"
#include "services/block_index_loader.h"
#include "controllers/sync_controller.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "chain/chainparams.h"

/* Read the REAL hashFinalSaplingRoot from a block body on disk (node.db's
 * blocks.sapling_root is a zeroed projection artifact, so we cannot trust it).
 * Returns true and fills root32 on success. */
static bool read_body_sapling_root(const char *datadir, int file_num,
                                   uint32_t data_pos, uint8_t root32[32])
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat", datadir, file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || (off_t)data_pos >= st.st_size) {
        close(fd);
        return false;
    }
    void *mp = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mp == MAP_FAILED) return false;
    bool ok = false;
    struct block blk;
    block_init(&blk);
    struct byte_stream s;
    stream_init_from_data(&s, (uint8_t *)mp + data_pos,
                          (size_t)st.st_size - data_pos);
    if (block_deserialize(&blk, &s)) {
        memcpy(root32, blk.header.hashFinalSaplingRoot.data, 32);
        ok = true;
    }
    block_free(&blk);
    munmap(mp, (size_t)st.st_size);
    return ok;
}

/* Top up the in-memory block-index map by walking the CHILD chain FORWARD from
 * `start` (the flat block-index tip) up to height `to_h`, using node.db `blocks`
 * (the stale flat block_index.bin does not reach the live coins frontier).
 *
 * The contested wedge region makes node.db's stored `height` labels unreliable
 * (a row at stored height H may have a parent that is NOT the row at stored
 * H-1), so we follow prev_hash linkage instead: at each step find the child row
 * whose prev_hash == the current block's hash, insert it with pprev linked + the
 * REAL hashFinalSaplingRoot read from the body (node.db's is zeroed), and assign
 * a sequential height. Returns the count inserted, -1 on error. */
static long topup_forward_from_node_db(struct main_state *ms, struct node_db *ndb,
                                       const char *datadir,
                                       struct block_index *start, int to_h)
{
    if (!start || start->nHeight >= to_h) return 0;

    sqlite3_stmt *child = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT hash, file_num, data_pos, status, num_tx "
            "FROM blocks WHERE prev_hash = ? LIMIT 1",
            -1, &child, NULL) != SQLITE_OK) {
        fprintf(stderr, "topup: prepare failed: %s\n", sqlite3_errmsg(ndb->db));
        return -1;
    }

    long inserted = 0;
    struct block_index *cur = start;
    int next_h = start->nHeight + 1;
    while (next_h <= to_h) {
        sqlite3_reset(child);
        sqlite3_clear_bindings(child);
        sqlite3_bind_blob(child, 1, cur->hashBlock.data, 32, SQLITE_STATIC);
        if (sqlite3_step(child) != SQLITE_ROW) {
            fprintf(stderr, "topup: no child of h=%d (hash chain ends before "
                    "seed h=%d)\n", cur->nHeight, to_h);
            inserted = -1;
            break;
        }
        const void *hb = sqlite3_column_blob(child, 0);
        if (!hb || sqlite3_column_bytes(child, 0) < 32) { inserted = -1; break; }
        int file_num = sqlite3_column_int(child, 1);
        uint32_t data_pos = (uint32_t)sqlite3_column_int64(child, 2);
        int status = sqlite3_column_int(child, 3);
        int num_tx = sqlite3_column_int(child, 4);

        struct uint256 hash;
        memcpy(hash.data, hb, 32);
        struct block_index *bi =
            chainstate_insert_block_index((struct chainstate *)ms, &hash);
        if (!bi) { inserted = -1; break; }
        bi->nHeight = next_h;
        bi->nFile = file_num;
        bi->nDataPos = data_pos;
        bi->nStatus = (uint32_t)status;
        bi->nTx = num_tx;
        bi->pprev = cur;
        uint8_t real_root[32];
        if (read_body_sapling_root(datadir, file_num, data_pos, real_root))
            memcpy(bi->hashFinalSaplingRoot.data, real_root, 32);

        cur = bi;
        next_h++;
        inserted++;
    }
    sqlite3_finalize(child);
    return inserted;
}

/* Provided by the node binary's main.c; this standalone tool defines its own
 * so the shared object set links. */
volatile sig_atomic_t g_shutdown_requested = 0;

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <datadir> <seed_height> <out_path>\n", argv[0]);
        return 2;
    }
    const char *datadir = argv[1];
    int32_t seed_h = (int32_t)strtol(argv[2], NULL, 10);
    const char *out_path = argv[3];

    if (seed_h <= 0) {
        fprintf(stderr, "seed_height must be > 0 (got %d)\n", seed_h);
        return 2;
    }

    /* (1) coins_kv home. */
    if (!progress_store_open(datadir)) {
        fprintf(stderr, "progress_store_open(%s) failed\n", datadir);
        return 1;
    }
    sqlite3 *pdb = progress_store_db();
    if (!pdb) {
        fprintf(stderr, "progress_store_db() returned NULL\n");
        return 1;
    }

    int64_t num_txs = 0, count = 0, supply = 0;
    if (coins_kv_setinfo(pdb, &num_txs, &count, &supply))
        fprintf(stderr, "coins_kv: count=%lld supply=%lld\n",
                (long long)count, (long long)supply);

    /* (2) in-memory block index. */
    struct main_state ms;
    main_state_init(&ms);
    if (!load_block_index_flat(datadir, &ms)) {
        fprintf(stderr, "load_block_index_flat(%s) failed — need a "
                "block_index.bin (run --importblockindex first)\n", datadir);
        return 1;
    }

    /* node_state home (opened early — the topup below reads node.db `blocks`). */
    char ndb_path[1100];
    snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", datadir);
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, ndb_path)) {
        fprintf(stderr, "node_db_open(%s) failed\n", ndb_path);
        return 1;
    }

    /* (3) the stale flat block_index.bin may stop below the coins frontier.
     * Top up the [flat_tip+1 .. seed_h] window from node.db `blocks` (reading
     * the REAL hashFinalSaplingRoot from each body) so the active chain reaches
     * the seed height with verifiable roots. */
    int flat_tip = -1;
    struct block_index *flat_tip_bi = NULL;
    {
        size_t it = 0;
        struct block_index *p = NULL;
        while (block_map_next(&ms.map_block_index, &it, NULL, &p)) {
            if (!p || !p->phashBlock) continue;
            if (p->nHeight > flat_tip) { flat_tip = p->nHeight; flat_tip_bi = p; }
        }
    }
    if (flat_tip < seed_h) {
        fprintf(stderr, "[mint-v2] flat block_index tip h=%d < seed h=%d — "
                "topping up FORWARD by prev_hash from node.db `blocks`...\n",
                flat_tip, seed_h);
        long up = topup_forward_from_node_db(&ms, &ndb, datadir,
                                             flat_tip_bi, seed_h);
        if (up < 0) {
            fprintf(stderr, "topup failed\n");
            node_db_close(&ndb);
            return 1;
        }
        fprintf(stderr, "[mint-v2] topped up %ld block-index entries\n", up);
    }

    /* find the highest block in the map and install it as the active tip so
     * active_chain_extend_window can densify the [..tip] window. */
    struct block_index *best = NULL;
    {
        size_t it = 0;
        struct block_index *p = NULL;
        while (block_map_next(&ms.map_block_index, &it, NULL, &p)) {
            if (!p || !p->phashBlock) continue;
            if (!best || p->nHeight > best->nHeight) best = p;
        }
    }
    if (!best) {
        fprintf(stderr, "block index map empty after load\n");
        node_db_close(&ndb);
        return 1;
    }
    ms.pindex_best_header = best;

    /* Diagnostic: walk pprev from the tip and report the first gap (a NULL
     * pprev or a non-contiguous height step) so a densify failure is legible. */
    {
        struct block_index *w = best;
        int prev_h = best->nHeight + 1;
        while (w) {
            if (w->nHeight != prev_h - 1) {
                fprintf(stderr, "[mint-v2] pprev height step gap: %d -> %d\n",
                        prev_h, w->nHeight);
                break;
            }
            if (!w->pprev) {
                if (w->nHeight != 0)
                    fprintf(stderr, "[mint-v2] pprev chain breaks at h=%d "
                            "(NULL pprev, not genesis)\n", w->nHeight);
                break;
            }
            prev_h = w->nHeight;
            w = w->pprev;
        }
    }

    /* Resolve the seed-height block by walking pprev from the map tip (no
     * active-chain window needed yet). */
    struct block_index *seed_bi_mut = best;
    while (seed_bi_mut && seed_bi_mut->nHeight > seed_h)
        seed_bi_mut = seed_bi_mut->pprev;
    if (!seed_bi_mut || seed_bi_mut->nHeight != seed_h) {
        fprintf(stderr, "no block_index at seed height h=%d (map tip h=%d)\n",
                seed_h, best->nHeight);
        node_db_close(&ndb);
        return 1;
    }
    uint8_t anchor_hash[32];
    memcpy(anchor_hash, seed_bi_mut->hashBlock.data, 32);

    /* Set the active tip AT the seed height and densify the full [0..seed_h]
     * window (active_chain_move_window_tip -> active_chain_fill_window walks
     * pprev to genesis). This is what sapling_tree_rebuild's per-height
     * active_chain_at() reads, and it caps the rebuild endpoint to seed_h. */
    if (!active_chain_move_window_tip(&ms.chain_active, seed_bi_mut)) {
        fprintf(stderr, "failed to install+densify the active-chain window to "
                "the seed height h=%d\n", seed_h);
        node_db_close(&ndb);
        return 1;
    }
    /* Sanity: a few historical slots must resolve (else the bodies would be
     * skipped and the rebuild would produce a wrong root). */
    if (!active_chain_at(&ms.chain_active, 476969) ||
        !active_chain_at(&ms.chain_active, seed_h)) {
        fprintf(stderr, "active-chain window did not densify (slot 476969 or "
                "seed missing) — pprev chain incomplete\n");
        node_db_close(&ndb);
        return 1;
    }

    /* (5) clear stale resume markers + drive the consensus-validating rebuild.
     * It replays note commitments from Sapling activation to the capped
     * endpoint, verifying the final root against hashFinalSaplingRoot, and
     * persists the serialized frontier into node_state["sapling_tree"]. */
    (void)node_db_state_set(&ndb, "sapling_tree", NULL, 0);
    (void)node_db_state_set(&ndb, "sapling_tree_rescan_height", NULL, 0);
    (void)node_db_state_set(&ndb, "sapling_tree_rebuild_height", NULL, 0);

    fprintf(stderr, "[mint-v2] driving sapling_tree_rebuild to h=%d "
            "(this replays note commitments from blocks/ — minutes)...\n",
            seed_h);
    int appended = sapling_tree_rebuild(&ndb, &ms.chain_active, datadir);
    if (appended < 0) {
        fprintf(stderr, "sapling_tree_rebuild FAILED (rc=%d)\n", appended);
        node_db_close(&ndb);
        return 1;
    }

    /* The rebuild may have capped its endpoint to the coins-applied frontier;
     * read the ACTUAL height it persisted so we stamp the snapshot coherently. */
    int64_t built_h = 0;
    if (!node_db_state_get_int(&ndb, "sapling_tree_rebuild_height", &built_h) ||
        built_h <= 0) {
        fprintf(stderr, "sapling_tree_rebuild did not persist a rebuild height\n");
        node_db_close(&ndb);
        return 1;
    }
    if ((int32_t)built_h != seed_h) {
        fprintf(stderr, "[mint-v2] NOTE: rebuild endpoint capped to h=%lld "
                "(requested seed h=%d). Stamping the snapshot at h=%lld so the "
                "UTXO set and the Sapling frontier are coherent.\n",
                (long long)built_h, seed_h, (long long)built_h);
        seed_h = (int32_t)built_h;
        const struct block_index *bh = active_chain_at(&ms.chain_active, seed_h);
        if (bh) {
            memcpy(anchor_hash, bh->hashBlock.data, 32);
        } else {
            fprintf(stderr, "could not resolve block_index at capped h=%d "
                    "for the anchor hash\n", seed_h);
            node_db_close(&ndb);
            return 1;
        }
    }

    /* (6) read the frontier blob + write the v2 snapshot. */
    uint8_t frontier[8192];
    size_t flen = 0;
    if (!node_db_state_get(&ndb, "sapling_tree", frontier, sizeof(frontier),
                           &flen) || flen == 0) {
        fprintf(stderr, "could not read back node_state[sapling_tree]\n");
        node_db_close(&ndb);
        return 1;
    }
    fprintf(stderr, "[mint-v2] sapling frontier built: %zu bytes at h=%d\n",
            flen, seed_h);

    uint8_t got_sha3[32] = {0};
    uint64_t got_count = 0;
    int64_t got_supply = 0;
    if (!coins_kv_snapshot_write_v2(pdb, out_path, seed_h, anchor_hash,
                                    frontier, (uint32_t)flen,
                                    got_sha3, &got_count, &got_supply)) {
        fprintf(stderr, "coins_kv_snapshot_write_v2 FAILED\n");
        node_db_close(&ndb);
        return 1;
    }
    node_db_close(&ndb);

    char sha3hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha3hex + i * 2, 3, "%02x", got_sha3[i]);
    fprintf(stderr,
            "WROTE v2 %s height=%d count=%llu supply=%lld frontier=%zuB sha3=%s\n",
            out_path, seed_h, (unsigned long long)got_count,
            (long long)got_supply, flen, sha3hex);
    return 0;
}
