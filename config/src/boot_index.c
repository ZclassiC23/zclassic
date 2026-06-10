/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Block index management: the chainstate rebuild/reindex core.
 *
 * Part of the boot composition root (config/src/). Owns the consensus
 * UTXO-replay surface — fast_rebuild_chainstate (a precheck), the full
 * reindex_chainstate replay plus its private DB-mode / coins-flush
 * helpers, boot_index_clear_coins_state, and the nChainTx/nChainWork
 * propagation pass (propagate_nchaintx).
 *
 * The background explorer address backfill lives in
 * config/src/boot_address_backfill.c; the on-disk block-file scan lives
 * in config/src/boot_block_file_scan.c. Block index flat-file save/load
 * and SQLite cache functions live in app/services/src/block_index_loader.c. */

#include "platform/time_compat.h"
#include "config/boot_internal.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "validation/connect_block.h"
#include "storage/block_index_db.h"
#include "storage/disk_block_io.h"
#include "coins/coins_view.h"
#include "event/event.h"
#include "primitives/block.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <malloc.h>
#include <sqlite3.h>

static struct db_service *boot_index_db_service_for(struct node_db *ndb)
{
    struct db_service *dbsvc = app_runtime_db_service();

    if (!ndb || !dbsvc)
        return NULL;
    return db_service_node_db(dbsvc) == ndb ? dbsvc : NULL;
}

static bool boot_index_enter_turbo_mode(struct node_db *ndb)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_ibd_turbo_mode(dbsvc);
    return node_db_ibd_turbo_mode(ndb);
}

static bool boot_index_restore_normal_mode(struct node_db *ndb)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_normal_mode(dbsvc);
    return node_db_normal_mode(ndb);
}

static bool boot_index_set_sync_batch_size(struct node_db *ndb, int batch_size)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_set_sync_batch_size(dbsvc, batch_size);
    node_db_set_sync_batch_size(ndb, batch_size);
    return true;
}

static bool boot_index_flush_write(struct node_db *ndb)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_flush_write(dbsvc);
    return node_db_sync_flush(ndb);
}

/* Block index flat file save/load, SQLite cache, and LevelDB loading
 * have been extracted to app/services/src/block_index_loader.c.
 * See services/block_index_loader.h for the public API. */


/* ── Fast chainstate rebuild from SQLite UTXOs ─────────────── */

bool fast_rebuild_chainstate(struct coins_view_sqlite *cvs,
                              struct coins_view_cache *cvtip,
                              const char *datadir)
{
    (void)cvtip;
    (void)datadir;
    if (!cvs->db) return false;

    sqlite3_stmt *cnt = NULL;
    if (sqlite3_prepare_v2(cvs->db, "SELECT count(*) FROM utxos",
                           -1, &cnt, NULL) != SQLITE_OK || !cnt) {
        fprintf(stderr, "fast_rebuild_chainstate: count prepare failed: %s\n",
                sqlite3_errmsg(cvs->db));
        return false;
    }
    int64_t total = 0;
    if (sqlite3_step(cnt) == SQLITE_ROW)
        total = sqlite3_column_int64(cnt, 0);
    sqlite3_finalize(cnt);

    if (total == 0) return false;

    printf("SQLite UTXO set: %lld UTXOs (canonical)\n", (long long)total);

    struct uint256 best;
    if (!coins_view_sqlite_get_best_block(cvs, &best) ||
        uint256_is_null(&best)) {
        fprintf(stderr,
                "fast_rebuild_chainstate: coins_best_block missing; "
                "refusing legacy tip_hash fallback\n");
        return false;
    }

    return true;
}

/* ── Full chainstate reindex: replay all blocks ────────────── */

static bool boot_index_flush_reindex_coins(struct coins_view_sqlite *cvs,
                                           struct coins_view_cache *cvtip)
{
    if (!cvs || !cvtip)
        LOG_FAIL("boot_index",
                 "reindex coins flush: NULL arg cvs=%p cvtip=%p",
                 (void *)cvs, (void *)cvtip);

    bool ok = coins_view_sqlite_batch_write_ex( // one-write-path-ok:boot-reindex-single-writer
        cvs, &cvtip->cache_coins, &cvtip->hash_block, &cvtip->commitment);
    if (!ok) {
        fprintf(stderr, // obs-ok:helper-context-logged
                "reindex-chainstate: coins flush failed; retaining %zu "
                "dirty entries\n",
                cvtip->cache_coins.size);
        return false;
    }

    coins_map_free(&cvtip->cache_coins);
    coins_map_init(&cvtip->cache_coins);
    utxo_commitment_init(&cvtip->commitment);
    return true;
}

/* Clear the persisted coins state (UTXO set + anchor + commitments) so the set
 * can be rebuilt from block data. Shared by reindex_chainstate and the
 * pre-integrity-gate path in app_init: a torn coins anchor would otherwise
 * FATAL the boot integrity gate before the operator's -reindex-chainstate
 * rebuild can run. Idempotent; node_db_exec logs each failing statement. */
bool boot_index_clear_coins_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        return false;
    bool ok = node_db_exec(ndb, "DELETE FROM utxos");
    /* coins_best_block (the anchor the gate checks), utxo_commitment (the XOR
     * checkpoint) and utxo_sha3 (the self-heal commitment) are ALL stale once
     * the set is rebuilt — clear them together so nothing stale survives to
     * mis-seed the rebuild or the boot self-heal. */
    if (!node_db_exec(ndb,
            "DELETE FROM node_state WHERE key IN "
            "('coins_best_block','utxo_commitment','utxo_sha3')"))
        ok = false;
    return ok;
}

bool reindex_chainstate(struct main_state *ms,
                          struct coins_view_sqlite *cvs,
                          struct coins_view_cache *cvtip,
                          struct node_db *ndb,
                          const char *datadir)
{
    int tip_height = active_chain_height(&ms->chain_active);
    if (tip_height < 0) {
        fprintf(stderr, "reindex-chainstate: no active chain\n");
        return false;
    }

    printf("reindex-chainstate: rebuilding UTXO set (%d blocks)...\n",
           tip_height + 1);
    event_emitf(EV_SYNC_STATE_CHANGE, 0, "reindex start blocks=%d",
                tip_height + 1);

    mallopt(M_MMAP_THRESHOLD, 32768);

    if (!boot_index_flush_reindex_coins(cvs, cvtip))
        return false;
    coins_view_cache_free(cvtip);

    if (ndb->open) {
        if (boot_index_clear_coins_state(ndb))
            printf("reindex-chainstate: wiped SQLite UTXO set\n");
    }

    coins_view_cache_init(cvtip, &cvs->view);

    set_flush_policy(3600, 1000000, 500);
    if (ndb->open) {
        if (!boot_index_enter_turbo_mode(ndb))
            fprintf(stderr, "reindex-chainstate: failed to enter turbo mode\n");
        if (!boot_index_set_sync_batch_size(ndb, 1000))
            fprintf(stderr, "reindex-chainstate: failed to set sync batch size\n");
    }

    extern _Atomic bool g_utxo_commitment_skip;
    atomic_store(&g_utxo_commitment_skip, true);

    if (ndb->open) {
        node_db_state_set(ndb, "sapling_tree", NULL, 0);
        node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);
    }

    const struct chain_params *cparams = chain_params_get();
    int64_t t_start = (int64_t)platform_time_wall_time_t();
    int errors = 0;

    for (int h = 0; h <= tip_height; h++) {
        struct block_index *pindex = active_chain_at(
            &ms->chain_active, h);
        if (!pindex) {
            printf("reindex-chainstate: missing block_index at height %d\n", h);
            errors++;
            break;
        }

        struct block blk;
        if (!read_block_from_disk_index(&blk, pindex, datadir)) {
            fprintf(stderr, "reindex-chainstate: failed to read block at "
                    "height %d — stopping to prevent UTXO corruption\n", h);
            errors++;
            break; /* Can't skip blocks during UTXO replay */
        }

        struct validation_state state;
        validation_state_init(&state);
        if (!connect_block(&blk, &state, pindex, cvtip, cparams, false)) {
            fprintf(stderr, "reindex-chainstate: connect_block FATAL at "
                    "height %d: %s — stopping to prevent UTXO corruption\n",
                    h, state.reject_reason);
            block_free(&blk);
            errors++;
            break; /* MUST stop — continuing would skip this block's UTXOs */
        }

        block_free(&blk);

        bool need_flush = (h % 10000 == 0) ||
                          (cvtip->cache_coins.size > 200000);
        if (need_flush) {
            if (!boot_index_flush_reindex_coins(cvs, cvtip)) {
                errors++;
                break;
            }
            malloc_trim(0);
            if (h % 1000 == 0) {
                int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
                double rate = elapsed > 0 ? (double)h / (double)elapsed : 0;
                int eta = rate > 0 ? (int)((tip_height - h) / rate) : 0;
                printf("  height %d/%d (%.0f blk/s, ETA %dm%ds, cache %zu)\n",
                       h, tip_height, rate, eta / 60, eta % 60,
                       cvtip->cache_coins.size);
            }
        }
    }

    if (!boot_index_flush_reindex_coins(cvs, cvtip))
        errors++;

    atomic_store(&g_utxo_commitment_skip, false);

    /* Restore normal mode — flush every 500 blocks */
    set_flush_policy(3600, 500000, 500);
    if (ndb->open) {
        if (!boot_index_flush_write(ndb))
            fprintf(stderr, "reindex-chainstate: flush failed\n");
        if (!boot_index_restore_normal_mode(ndb))
            fprintf(stderr, "reindex-chainstate: failed to restore normal mode\n");
        if (!boot_index_set_sync_batch_size(ndb, 1))
            fprintf(stderr, "reindex-chainstate: failed to reset sync batch size\n");
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    printf("reindex-chainstate: complete in %lldm%llds (%d errors)\n",
           (long long)(elapsed / 60), (long long)(elapsed % 60), errors);
    event_emitf(EV_SYNC_STATE_CHANGE, 0, "reindex complete %dm%ds errors=%d",
                (int)(elapsed / 60), (int)(elapsed % 60), errors);

    return errors == 0;
}

/* Address backfill (backfill_addresses_thread) moved to
 * config/src/boot_address_backfill.c. */

/* Block-file scan (scan_block_files_mark_data + helpers) moved to
 * config/src/boot_block_file_scan.c. */

/* ── Reusable nChainTx + nChainWork propagation ────────────────
 * Called after scan_block_files_mark_data or any operation that
 * sets BLOCK_HAVE_DATA on blocks. Propagates nChainTx (cumulative
 * tx count) and nChainWork so find_most_work_chain can consider
 * these blocks as chain tip candidates.
 * Returns the number of blocks whose nChainTx was updated. */
int propagate_nchaintx(struct main_state *ms)
{
    if (!ms) return 0;

    size_t total = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(total * sizeof(struct block_index *), "boot.index.propagate_sorted");
    if (!sorted) return 0;

    size_t n = 0;
    size_t iter = 0;
    struct block_index *bi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (bi && (bi->pprev || (bi->nStatus & BLOCK_HAVE_DATA)))
            sorted[n++] = bi;
    }

    qsort(sorted, n, sizeof(struct block_index *), block_index_cmp_height);

    int total_propagated = 0;
    for (int pass = 0; pass < 50; pass++) {
        int propagated = 0;
        for (size_t i = 0; i < n; i++) {
            struct block_index *b = sorted[i];
            if (b->nHeight == 0) {
                if (b->nChainTx == 0) {
                    b->nChainTx = b->nTx > 0 ? b->nTx : 1;
                    propagated++;
                }
                if (arith_uint256_is_zero(&b->nChainWork)) {
                    b->nChainWork = GetBlockProof(b);
                    propagated++;
                }
            } else if (b->pprev && b->pprev->nChainTx > 0) {
                unsigned int ntx = b->nTx > 0 ? b->nTx : 1;
                unsigned int expected = b->pprev->nChainTx + ntx;
                if (b->nChainTx != expected) {
                    b->nChainTx = expected;
                    propagated++;
                }
            } else if (b->pprev && b->pprev->nChainTx == 0) {
                unsigned int ntx = b->pprev->nTx > 0 ? b->pprev->nTx : 1;
                b->pprev->nChainTx = b->pprev->nHeight > 0 ?
                    (unsigned)(b->pprev->nHeight) : ntx;
                unsigned int btx = b->nTx > 0 ? b->nTx : 1;
                b->nChainTx = b->pprev->nChainTx + btx;
                if (arith_uint256_is_zero(&b->pprev->nChainWork)) {
                    b->pprev->nChainWork = GetBlockProof(b->pprev);
                    if (b->pprev->pprev &&
                        !arith_uint256_is_zero(&b->pprev->pprev->nChainWork))
                        arith_uint256_add(&b->pprev->nChainWork,
                            &b->pprev->pprev->nChainWork,
                            &b->pprev->nChainWork);
                }
                propagated += 2;
            }
            /* Propagate nChainWork alongside nChainTx */
            if (b->pprev && !arith_uint256_is_zero(&b->pprev->nChainWork) &&
                arith_uint256_is_zero(&b->nChainWork)) {
                struct arith_uint256 proof = GetBlockProof(b);
                arith_uint256_add(&b->nChainWork,
                                  &b->pprev->nChainWork, &proof);
                propagated++;
            }
        }
        total_propagated += propagated;
        if (propagated == 0) break;
    }

    free(sorted);
    return total_propagated;
}
