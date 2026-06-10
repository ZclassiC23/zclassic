/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/reducer_frontier_reconcile_light.h"

#include "framework/condition.h"
#include "jobs/stage_repair.h"
#include "net/connman.h"
#include "services/sync_monitor.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic int g_local_height_at_detect = -1;
static _Atomic int g_hstar_at_detect = -1;
static _Atomic int g_sweep_top_at_detect = -1;
static _Atomic int g_validate_headers_cursor_at_detect = -1;
static _Atomic int g_body_fetch_cursor_at_detect = -1;
static _Atomic int g_body_persist_cursor_at_detect = -1;
static _Atomic int g_script_validate_cursor_at_detect = -1;
static _Atomic int g_proof_validate_cursor_at_detect = -1;
static _Atomic int g_utxo_apply_cursor_at_detect = -1;
static _Atomic int g_tip_finalize_cursor_at_detect = -1;
/* Coin-backfill scan record snapshot: -1 = no detect snapshot yet,
 * 0 = record absent at detect, 1 = record present at detect. */
static _Atomic int g_coin_backfill_scan_present_at_detect = -1;
static _Atomic int g_coin_backfill_scan_next_at_detect = -1;
static _Atomic int g_remedy_calls = 0;

static bool read_reducer_cursor(sqlite3 *db, const char *name, int *out)
{
    if (out)
        *out = -1;
    if (!db || !name || !name[0] || !out)
        return false;

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] cursor "
                 "prepare failed stage=%s: %s",
                 name, sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] cursor "
                 "step failed stage=%s rc=%d: %s",
                 name, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        progress_store_tx_unlock();
        return false;
    }

    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return true;
}

static void snapshot_reducer_cursors(sqlite3 *db)
{
    int cursor = -1;

    if (read_reducer_cursor(db, "validate_headers", &cursor))
        atomic_store(&g_validate_headers_cursor_at_detect, cursor);
    if (read_reducer_cursor(db, "body_fetch", &cursor))
        atomic_store(&g_body_fetch_cursor_at_detect, cursor);
    if (read_reducer_cursor(db, "body_persist", &cursor))
        atomic_store(&g_body_persist_cursor_at_detect, cursor);
    if (read_reducer_cursor(db, "script_validate", &cursor))
        atomic_store(&g_script_validate_cursor_at_detect, cursor);
    if (read_reducer_cursor(db, "proof_validate", &cursor))
        atomic_store(&g_proof_validate_cursor_at_detect, cursor);
    if (read_reducer_cursor(db, "utxo_apply", &cursor))
        atomic_store(&g_utxo_apply_cursor_at_detect, cursor);
    if (read_reducer_cursor(db, "tip_finalize", &cursor))
        atomic_store(&g_tip_finalize_cursor_at_detect, cursor);
}

static bool cursor_changed(sqlite3 *db, const char *name,
                           const _Atomic int *captured)
{
    int before = atomic_load(captured);
    if (before < 0)
        return false;

    int now = -1;
    if (!read_reducer_cursor(db, name, &now))
        return false;
    return now >= 0 && now != before;
}

static bool reducer_cursor_witnessed(sqlite3 *db)
{
    return cursor_changed(db, "validate_headers",
                          &g_validate_headers_cursor_at_detect) ||
           cursor_changed(db, "body_fetch",
                          &g_body_fetch_cursor_at_detect) ||
           cursor_changed(db, "body_persist",
                          &g_body_persist_cursor_at_detect) ||
           cursor_changed(db, "script_validate",
                          &g_script_validate_cursor_at_detect) ||
           cursor_changed(db, "proof_validate",
                          &g_proof_validate_cursor_at_detect) ||
           cursor_changed(db, "utxo_apply",
                          &g_utxo_apply_cursor_at_detect) ||
           cursor_changed(db, "tip_finalize",
                          &g_tip_finalize_cursor_at_detect);
}

/* The coin-backfill no-spend scan persists its resumable cursor as a
 * progress_meta record keyed coin_backfill.scan.<H>.<holehash> whose value
 * begins [next_height i32 LE] (jobs/stage_repair_coin_backfill.h). At most
 * one hole is active (single-frontier discipline), so the first record in
 * key order is the live scan. Distinguishes record ABSENT — a valid,
 * witnessable state — from a read failure (returns false). */
static bool read_coin_backfill_scan_cursor(sqlite3 *db, bool *present,
                                           int *next_height)
{
    *present = false;
    *next_height = -1;
    if (!db)
        return false;

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM progress_meta "
            "WHERE key GLOB 'coin_backfill.scan.*' "
            "ORDER BY key LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] coin_backfill "
                 "scan record prepare failed: %s",
                 sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return false;
    }

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *present = true;
        const uint8_t *v = sqlite3_column_blob(st, 0);
        if (v && sqlite3_column_bytes(st, 0) >= 4)
            *next_height = (int)((uint32_t)v[0] | (uint32_t)v[1] << 8 |
                                 (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] coin_backfill "
                 "scan record step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        progress_store_tx_unlock();
        return false;
    }

    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return true;
}

static void snapshot_coin_backfill_scan(sqlite3 *db)
{
    bool present = false;
    int next = -1;
    if (!read_coin_backfill_scan_cursor(db, &present, &next))
        return;
    atomic_store(&g_coin_backfill_scan_present_at_detect, present ? 1 : 0);
    atomic_store(&g_coin_backfill_scan_next_at_detect, next);
}

/* Unlike cursor_changed — whose before<0 convention means "no snapshot,
 * never witness" — an ABSENT scan record at detect is itself a valid
 * baseline: the record being CREATED (absent->present, scan started) and
 * CONSUMED (present->absent, scan completed and applied by the insert tx)
 * are both durable backfill progress and must reset the attempt budget.
 * Only present_before == -1 (no detect snapshot at all) refuses. */
static bool coin_backfill_scan_witnessed(sqlite3 *db)
{
    int present_before =
        atomic_load(&g_coin_backfill_scan_present_at_detect);
    if (present_before < 0)
        return false;
    int next_before = atomic_load(&g_coin_backfill_scan_next_at_detect);

    bool present_now = false;
    int next_now = -1;
    if (!read_coin_backfill_scan_cursor(db, &present_now, &next_now))
        return false;

    if ((present_before != 0) != present_now)
        return true;
    return present_now && next_now != next_before;
}

static bool peer_lag_allows_repair(struct main_state *ms)
{
    struct connman *cm = sync_monitor_connman();
    if (!cm)
        return true;

    int local = ms ? active_chain_height(&ms->chain_active) : -1;
    if (local < 0)
        return false;

    int peer_max = connman_max_peer_height(cm);
    if (peer_max > local)
        return true;

    /* A zero-peer copy still needs the local flag/cursor repair so it can hold
     * in repairing state and resume when a body source appears. Non-zero peers
     * that are not ahead are not useful evidence of a stale local tip. */
    return connman_get_node_count(cm) == 0;
}

static bool detect_reducer_frontier_reconcile_light(void)
{
    int64_t tip_age = sync_monitor_tip_advance_age();
    if (tip_age >= 0 && tip_age < 60)
        return false;

    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms)
        return false;
    if (!peer_lag_allows_repair(ms))
        return false;

    struct stage_reducer_frontier_reconcile_result rr;
    if (!stage_reducer_frontier_reconcile_light_needed(db, ms, &rr))
        return false;
    if (rr.refused_coin_unknown)
        return false;

    atomic_store(&g_local_height_at_detect,
                 active_chain_height(&ms->chain_active));
    atomic_store(&g_hstar_at_detect, rr.hstar);
    atomic_store(&g_sweep_top_at_detect, rr.sweep_top);
    snapshot_reducer_cursors(db);
    snapshot_coin_backfill_scan(db);
    return rr.refused_coin_tear || rr.repaired;
}

static enum condition_remedy_result remedy_reducer_frontier_reconcile_light(void)
{
    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);

    struct stage_reducer_frontier_reconcile_result rr;
    if (!stage_reducer_frontier_reconcile_light(db, ms, &rr))
        return COND_REMEDY_FAILED;
    if (rr.refused_coin_unknown) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "coins_applied_height absent");
        return COND_REMEDY_SKIP;
    }
    if (rr.refused_coin_tear) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "coins_applied_height=%d hstar=%d",
                 rr.coins_applied_height, rr.hstar);
        return COND_REMEDY_FAILED;
    }
    if (rr.value_overflow_repair_owner_refused) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "value_overflow repair h=%d: owner ack missing",
                 rr.value_overflow_repair_height);
        return COND_REMEDY_FAILED;
    }
    /* Belt-and-suspenders engine accounting only: the backfill Job pages the
     * operator directly (typed blocker + EV_OPERATOR_NEEDED) on every refusal
     * status; this surfacing must never be the paging path. */
    if (rr.coin_backfill_owner_refused) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "coin backfill h=%d: owner ack missing",
                 rr.coin_backfill_hole_height);
        return COND_REMEDY_FAILED;
    }
    if (!rr.repaired)
        return COND_REMEDY_SKIP;

    LOG_WARN("condition",
             "[condition:reducer_frontier_reconcile_light] hstar=%d "
             "coins_applied=%d sweep_top=%d validate_headers=%d->%d "
             "body_fetch=%d->%d body_persist=%d->%d "
             "tip_finalize=%d->%d scripts_set=%d have_data_set=%d "
             "have_data_cleared=%d validate_hash_split=%d "
             "failed_mask_cleared=%d",
             rr.hstar, rr.coins_applied_height, rr.sweep_top,
             rr.validate_headers_cursor_before,
             rr.validate_headers_cursor_after,
             rr.body_fetch_cursor_before, rr.body_fetch_cursor_after,
             rr.body_persist_cursor_before, rr.body_persist_cursor_after,
             rr.tip_finalize_cursor_before, rr.tip_finalize_cursor_after,
             rr.scripts_set, rr.have_data_set, rr.have_data_cleared,
             rr.lowest_validate_headers_hash_split,
             rr.failed_mask_cleared);
    return COND_REMEDY_OK;
}

static bool witness_reducer_frontier_reconcile_light(int64_t target_at_detect)
{
    (void)target_at_detect;

    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        return false;

    int before = atomic_load(&g_local_height_at_detect);
    if (before < 0)
        return false;

    int now = active_chain_height(&ms->chain_active);
    if (now > before)
        return true;

    sqlite3 *db = progress_store_db();
    if (reducer_cursor_witnessed(db))
        return true;
    return coin_backfill_scan_witnessed(db);
}

static struct condition c_reducer_frontier_reconcile_light = {
    .name = "reducer_frontier_reconcile_light",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_reducer_frontier_reconcile_light,
    .remedy = remedy_reducer_frontier_reconcile_light,
    .witness = witness_reducer_frontier_reconcile_light,
    .witness_window_secs = 60,
};

void register_reducer_frontier_reconcile_light(void)
{
    (void)condition_register(&c_reducer_frontier_reconcile_light);
}

#ifdef ZCL_TESTING
void reducer_frontier_reconcile_light_test_reset(void)
{
    struct condition_state *s = &c_reducer_frontier_reconcile_light.state;
    atomic_store(&g_local_height_at_detect, -1);
    atomic_store(&g_hstar_at_detect, -1);
    atomic_store(&g_sweep_top_at_detect, -1);
    atomic_store(&g_validate_headers_cursor_at_detect, -1);
    atomic_store(&g_body_fetch_cursor_at_detect, -1);
    atomic_store(&g_body_persist_cursor_at_detect, -1);
    atomic_store(&g_script_validate_cursor_at_detect, -1);
    atomic_store(&g_proof_validate_cursor_at_detect, -1);
    atomic_store(&g_utxo_apply_cursor_at_detect, -1);
    atomic_store(&g_tip_finalize_cursor_at_detect, -1);
    atomic_store(&g_coin_backfill_scan_present_at_detect, -1);
    atomic_store(&g_coin_backfill_scan_next_at_detect, -1);
    atomic_store(&g_remedy_calls, 0);
    condition_reset_state(&c_reducer_frontier_reconcile_light);
    atomic_store(&s->last_remedy_unix, (int64_t)0);
    atomic_store(&s->last_operator_needed_unix, (int64_t)0);
}

void reducer_frontier_reconcile_light_test_clear_backoff(void);
void reducer_frontier_reconcile_light_test_clear_backoff(void)
{
    struct condition_state *s = &c_reducer_frontier_reconcile_light.state;
    atomic_store(&s->last_remedy_unix, (int64_t)0);
}

int reducer_frontier_reconcile_light_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}
#endif
