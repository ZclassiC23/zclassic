/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Flush policy + coins/Sapling persistence helpers for process_block.
 *
 * Contents:
 *   - g_flush_policy + tunable setter
 *   - g_coins_sqlite_ptr, g_sapling_tree_for_flush, g_sapling_ckpt_path
 *   - sapling_checkpoint_maybe_flush
 *   - sapling_tree_persist_once
 *   - flush_coins_if_needed (the per-block coins flush gate)
 *   - ZCL_TESTING hooks */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "config/runtime.h"
#include "validation/process_block.h"
#include "validation/main_logic.h"
#include "validation/connect_block.h"
#include "coins/utxo_commitment.h"
#include "core/serialize.h"
#include "core/utiltime.h"
#include "event/event.h"
#include "storage/coins_view_sqlite.h"
#include "util/log_macros.h"

#include "process_block_internal.h"

/* `g_sapling_tree_rebuilding` is defined in app/controllers/sync_controller.c
 * and declared in controllers/sync_controller.h (outside ZCL_TESTING),
 * but lib/ shouldn't pull the full controller header in. Mirror the
 * local-extern pattern already used in
 * app/controllers/src/blockchain_controller_chain.c. Real fix is to
 * promote the global into a lib/-owned header — out of scope. */
extern _Atomic bool g_sapling_tree_rebuilding;

/* ── Flush policy ────────────────────────────────────────────
 * Controls when the in-memory UTXO cache writes to LevelDB.
 * Batching multiple blocks into one LevelDB write improves
 * throughput during IBD by 10-50x.
 *
 * Short-term (hot) layer: coins_view_cache accumulates changes.
 * Long-term (cold) layer: LevelDB receives batched writes.
 * The batch_block_interval controls how many blocks accumulate
 * before flushing — the bridge between the two layers. */
struct flush_policy {
    int64_t interval_secs;     /* max seconds between flushes (default 3600) */
    size_t  max_entries;       /* flush if cache exceeds this (default 500000) */
    int     block_interval;    /* flush every N blocks; 0 = disabled (default) */
};

static struct flush_policy g_flush_policy = {
    .interval_secs  = 3600,
    .max_entries    = 500000,
    .block_interval = 0,
};
static _Atomic int64_t g_last_coins_flush = 0;
static _Atomic int64_t g_blocks_since_flush = 0;

/* Sapling tree checkpoint — persist every N blocks so SIGKILL only
 * loses ~N blocks of tree state (seconds to rebuild) instead of the
 * full tree (5+ minutes). */
#define SAPLING_TREE_CHECKPOINT_INTERVAL 1000
static _Atomic int64_t g_blocks_since_sapling_save = 0;
static _Atomic int g_sapling_persist_fail_count = 0;
static _Atomic int g_sapling_persist_test_force_fail = 0;

/* SQLite handle for persisting UTXO commitment alongside flushes.
 * Set by boot.c via set_coins_sqlite_for_commitment(). */
static struct coins_view_sqlite *g_coins_sqlite_ptr = NULL;

void set_coins_sqlite_for_commitment(struct coins_view_sqlite *cvs)
{
    g_coins_sqlite_ptr = cvs;
}

/* Accessor used by core.c (process_block_get_coins_sqlite). */
struct coins_view_sqlite *process_block_coins_sqlite_ptr(void)
{
    return g_coins_sqlite_ptr;
}

/* Sapling tree pointer for persistence during flush.
 * Set by boot.c after loading tree from node_state. */
static struct incremental_merkle_tree *g_sapling_tree_for_flush = NULL;

void set_sapling_tree_for_flush(struct incremental_merkle_tree *tree)
{
    g_sapling_tree_for_flush = tree;
}

/* ── Flat-file sapling checkpoint ───────────────────────
 *
 * Optional path set by boot.c once datadir is known. When populated
 * and the sapling tree pointer above is also set, every
 * SAPLING_CHECKPOINT_BLOCK_INTERVAL blocks we flush a self-contained
 * checkpoint to `<datadir>/sapling_tree_ckpt.dat`. Boot.c loads this
 * file before the node_state-backed rebuild path fires, so a healthy
 * checkpoint skips the 2.6M-block replay entirely. */
#define SAPLING_CHECKPOINT_BLOCK_INTERVAL 10000
static char g_sapling_ckpt_path[512] = {0};

void set_sapling_checkpoint_datadir(const char *datadir)
{
    if (!datadir || datadir[0] == '\0') {
        g_sapling_ckpt_path[0] = '\0';
        return;
    }
    int n = snprintf(g_sapling_ckpt_path, sizeof(g_sapling_ckpt_path),
                     "%s/sapling_tree_ckpt.dat", datadir);
    if (n < 0 || (size_t)n >= sizeof(g_sapling_ckpt_path))
        g_sapling_ckpt_path[0] = '\0';
}

void sapling_checkpoint_maybe_flush(int height)
{
    if (!g_sapling_tree_for_flush || g_sapling_ckpt_path[0] == '\0')
        return;
    if (height < 0)
        return;
    if ((height % SAPLING_CHECKPOINT_BLOCK_INTERVAL) != 0)
        return;
    /* Best-effort: a failed flush is not fatal — the next interval
     * will retry, and the node_state-backed persist path still runs
     * as a secondary belt. */
    (void)sapling_tree_flush_checkpoint(g_sapling_tree_for_flush,
                                        (int64_t)height,
                                        g_sapling_ckpt_path);
}

bool sapling_tree_persist_once(void)
{
    struct node_db *ndb = process_block_node_db_internal();
    if (!app_runtime_node_db_handle_open(ndb) || !g_sapling_tree_for_flush)
        return false;

    struct byte_stream ts;
    stream_init(&ts, 4096);
    bool serialized = incremental_tree_serialize(g_sapling_tree_for_flush, &ts);
    if (!serialized) {
        stream_free(&ts);
        return false;
    }

    bool persist_ok;
    int force_fail = atomic_fetch_sub(&g_sapling_persist_test_force_fail, 1);
    if (force_fail > 0) {
        persist_ok = false;
    } else {
        persist_ok = app_runtime_node_db_state_set(
            ndb, "sapling_tree", ts.data, ts.size);
        atomic_store(&g_sapling_persist_test_force_fail, 0);
    }
    stream_free(&ts);

    if (!persist_ok) {
        int fails = atomic_fetch_add(&g_sapling_persist_fail_count, 1) + 1;
        event_emitf(EV_SAPLING_PERSIST_FAIL, 0, "fails=%d", fails);
        if (fails >= 3) {
            atomic_store(&g_sapling_tree_rebuilding, true);
            atomic_store(&g_sapling_persist_fail_count, 0);
            return false;
        }
        return true;
    }

    atomic_store(&g_sapling_persist_fail_count, 0);
    g_blocks_since_sapling_save = 0;
    return true;
}

void set_flush_policy(int64_t interval_secs, size_t max_entries,
                      int block_interval)
{
    g_flush_policy.interval_secs = interval_secs;
    g_flush_policy.max_entries = max_entries;
    g_flush_policy.block_interval = block_interval;
    printf("flush_policy: interval=%llds max_entries=%zu block_interval=%d\n",
           (long long)interval_secs, max_entries, block_interval);
}

bool flush_coins_if_needed(struct coins_view_cache *coins_tip,
                           bool force)
{
    int64_t now = GetTime();
    if (g_last_coins_flush == 0)
        g_last_coins_flush = now;

    g_blocks_since_flush++;
    g_blocks_since_sapling_save++;

    bool time_flush = (now - g_last_coins_flush) > g_flush_policy.interval_secs;
    bool size_flush = coins_tip->cache_coins.size > g_flush_policy.max_entries;
    bool block_flush = g_flush_policy.block_interval > 0 &&
                       g_blocks_since_flush >= g_flush_policy.block_interval;

    if (!force && !time_flush && !size_flush && !block_flush) {
        /* Even without a full coins flush, periodically persist the
         * Sapling commitment tree so SIGKILL only loses ~1000 blocks
         * of tree state (seconds to rebuild) instead of all of it. */
        if (g_sapling_tree_for_flush &&
            g_blocks_since_sapling_save >= SAPLING_TREE_CHECKPOINT_INTERVAL) {
            struct node_db *sap_ndb = process_block_node_db_internal();
            if (app_runtime_node_db_handle_open(sap_ndb)) {
                struct byte_stream ts;
                stream_init(&ts, 4096);
                if (incremental_tree_serialize(g_sapling_tree_for_flush, &ts)) {
                    if (app_runtime_node_db_state_set(
                            sap_ndb, "sapling_tree", ts.data, ts.size)) {
                        g_blocks_since_sapling_save = 0;
                        (void)app_runtime_node_db_wal_checkpoint_passive(
                            sap_ndb);
                    }
                }
                stream_free(&ts);
            }
        }
        return true;
    }


    /* Flush node_db batch first — coins_flush needs the write lock. */
    struct node_db *ndb = process_block_node_db_internal();
    app_runtime_node_db_sync_flush_if_needed(ndb);

    size_t batched = (size_t)g_blocks_since_flush;

    /* Use batch_write_ex to write UTXO commitment atomically inside
     * the same SAVEPOINT transaction as the coins flush. This eliminates
     * the race window where a concurrent reader could block the next
     * SAVEPOINT between flush and commitment write. */
    bool ok;
    if (g_coins_sqlite_ptr) {
        ok = coins_view_sqlite_batch_write_ex( // one-write-path-ok:process-block-flush-policy
            g_coins_sqlite_ptr, &coins_tip->cache_coins,
            &coins_tip->hash_block, &coins_tip->commitment);
        if (ok) {
            coins_map_free(&coins_tip->cache_coins);
            coins_map_init(&coins_tip->cache_coins);
        } else {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "WARNING: coins cache flush FAILED — retaining "
                    "%zu dirty entries for retry\n",
                    coins_tip->cache_coins.size);
        }
    } else {
#ifdef ZCL_TESTING
        ok = coins_view_cache_flush_for_testing(
            coins_tip);
#else
        LOG_FAIL("flush",
                 "coins SQLite writer is not configured; refusing legacy "
                 "coins_view_cache_flush fallback");
#endif
    }

    if (ok) {
        g_last_coins_flush = now;
        g_blocks_since_flush = 0;

        /* Persist Sapling commitment tree state */
        if (app_runtime_node_db_handle_open(ndb) &&
            g_sapling_tree_for_flush) {
            ok = sapling_tree_persist_once() && ok;
        }

        const char *trigger = force ? "forced" : time_flush ? "periodic" :
                              size_flush ? "cache-full" : "block-interval";
        event_emitf(EV_COINS_FLUSH, 0, "%s batched=%zu entries=%zu",
                    trigger, batched, coins_tip->cache_coins.size);
        if (batched >= 100)
            printf("flush_coins: wrote %s (%zu blocks batched)\n",
                   trigger, batched);

        /* WAL checkpoint after every successful flush — prevents WAL
         * from growing to multi-GB during sustained block processing.
         * A 6GB WAL was observed in production, blocking all operations.
         * Checkpointing on every flush keeps WAL size bounded. */
        if (app_runtime_node_db_handle_open(ndb))
            (void)app_runtime_node_db_wal_checkpoint(ndb);
    } else {
        event_emitf(EV_COINS_FLUSH_FAILED, 0, "flush returned false");
        /* If there's nothing dirty in the cache, the flush "failure" is
         * harmless — nothing was lost. This happens when force-flush
         * triggers before any blocks are connected (SQLITE_BUSY from
         * background threads). */
        if (coins_tip->cache_coins.size == 0 && batched <= 1) {
            /* Empty cache — nothing to flush, not fatal */
            return true;
        }
        /* During IBD (many blocks since last flush), a flush failure
         * is likely SQLITE_BUSY from lock contention. Don't treat as
         * fatal — retry on next interval. The coins stay in memory. */
        if (batched > 10 && coins_tip->cache_coins.size < 2000000) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "flush_coins: BUSY — coins cached in memory, "
                    "will retry (%zu blocks batched, %zu entries)\n",
                   batched, coins_tip->cache_coins.size);
            return true; /* non-fatal during IBD */
        }
        if (coins_tip->cache_coins.size >= 2000000)
            LOG_FAIL("flush",
                     "CRITICAL — cache has %zu entries and flush keeps failing; "
                     "halting to prevent OOM",
                     coins_tip->cache_coins.size);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "flush_coins: FAILED to flush coins cache to disk "
                "(%zu blocks batched, %zu entries)\n",
                batched, coins_tip->cache_coins.size);
    }
    return ok;
}

#ifdef ZCL_TESTING
void process_block_test_fail_next_sapling_persists(int n)
{
    atomic_store(&g_sapling_persist_test_force_fail, n);
}

bool process_block_test_persist_sapling_tree(bool force)
{
    (void)force;
    return sapling_tree_persist_once();
}
#endif
