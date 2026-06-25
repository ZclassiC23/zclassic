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

#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "models/database.h"
#include "services/block_index_loader.h"
#include "controllers/sync_controller.h"
#include "core/serialize.h"

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

    /* (3) find the highest-work block in the map and install it as the active
     * tip so active_chain_extend_window can densify the [..tip] window; then
     * confirm the seed-height slot resolves. */
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
        return 1;
    }
    ms.pindex_best_header = best;
    if (!active_chain_install_tip_slot(&ms.chain_active, best) ||
        !active_chain_extend_window(&ms.chain_active, best)) {
        fprintf(stderr, "failed to install/extend the active-chain window to "
                "the header tip h=%d\n", best->nHeight);
        return 1;
    }

    const struct block_index *seed_bi =
        active_chain_at(&ms.chain_active, seed_h);
    if (!seed_bi) {
        fprintf(stderr, "no block_index at seed height h=%d (header tip h=%d)\n",
                seed_h, best->nHeight);
        return 1;
    }
    uint8_t anchor_hash[32];
    memcpy(anchor_hash, seed_bi->hashBlock.data, 32);

    /* Move the visible active tip DOWN to the seed height so
     * sapling_tree_rebuild's endpoint resolves to seed_h (it caps to the
     * coins-applied frontier when that is lower; the explicit window tip here
     * ensures active_chain_height == seed_h). */
    struct block_index *seed_bi_mut =
        block_map_find(&ms.map_block_index, &seed_bi->hashBlock);
    if (seed_bi_mut && !active_chain_move_window_tip(&ms.chain_active, seed_bi_mut))
        fprintf(stderr, "WARN: could not move window tip to seed h=%d "
                "(rebuild caps to coins-applied frontier)\n", seed_h);

    /* (4) node_state home. */
    char ndb_path[1100];
    snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", datadir);
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, ndb_path)) {
        fprintf(stderr, "node_db_open(%s) failed\n", ndb_path);
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
