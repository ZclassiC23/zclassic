/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/reducer_frontier_reconcile_light.h"

#include "platform/time_compat.h"
#include "framework/condition.h"
#include "jobs/refold_progress.h"
#include "jobs/stage_repair.h"
#include "jobs/stage_repair_coin_backfill.h"
#include "net/connman.h"
#include "services/sync_monitor.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/log_throttle.h"
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
/* tipfin backfill progress record snapshot, same convention. */
static _Atomic int g_tipfin_backfill_present_at_detect = -1;
static _Atomic int g_tipfin_backfill_progress_at_detect = -1;
static _Atomic int g_remedy_calls = 0;
static _Atomic int g_tear_bypass_warn_total = 0;

#define RFRL_RR_FIELD_LIST(X)                                             \
    X(REPAIRED, "last_reconcile_repaired", true)                         \
    X(CLAMPED_TIP_FINALIZE, "last_reconcile_clamped_tip_finalize", true) \
    X(REFUSED_COIN_TEAR, "last_reconcile_refused_coin_tear", true)       \
    X(REFUSED_COIN_UNKNOWN, "last_reconcile_refused_coin_unknown", true) \
    X(COINS_APPLIED_FOUND, "last_reconcile_coins_applied_found", true)   \
    X(HSTAR, "last_reconcile_hstar", false)                              \
    X(SERVED_FLOOR, "last_reconcile_served_floor", false)                \
    X(COINS_APPLIED_HEIGHT, "last_reconcile_coins_applied_height", false)\
    X(CLAMPED_VALIDATE_HEADERS,                                           \
      "last_reconcile_clamped_validate_headers", true)                   \
    X(VALIDATE_HEADERS_CURSOR_BEFORE,                                     \
      "last_reconcile_validate_headers_cursor_before", false)            \
    X(VALIDATE_HEADERS_CURSOR_AFTER,                                      \
      "last_reconcile_validate_headers_cursor_after", false)             \
    X(CLAMPED_BODY_FETCH, "last_reconcile_clamped_body_fetch", true)     \
    X(BODY_FETCH_CURSOR_BEFORE,                                           \
      "last_reconcile_body_fetch_cursor_before", false)                  \
    X(BODY_FETCH_CURSOR_AFTER,                                            \
      "last_reconcile_body_fetch_cursor_after", false)                   \
    X(CLAMPED_BODY_PERSIST,                                               \
      "last_reconcile_clamped_body_persist", true)                       \
    X(BODY_PERSIST_CURSOR_BEFORE,                                         \
      "last_reconcile_body_persist_cursor_before", false)                \
    X(BODY_PERSIST_CURSOR_AFTER,                                          \
      "last_reconcile_body_persist_cursor_after", false)                 \
    X(TIP_FINALIZE_CURSOR_BEFORE,                                         \
      "last_reconcile_tip_finalize_cursor_before", false)                \
    X(TIP_FINALIZE_CURSOR_AFTER,                                          \
      "last_reconcile_tip_finalize_cursor_after", false)                 \
    X(SWEEP_TOP, "last_reconcile_sweep_top", false)                      \
    X(LOWEST_HAVE_DATA_CLEARED,                                           \
      "last_reconcile_lowest_have_data_cleared", false)                  \
    X(LOWEST_VALIDATE_HEADERS_REFILL_HOLE,                                \
      "last_reconcile_lowest_validate_headers_refill_hole", false)       \
    X(LOWEST_VALIDATE_HEADERS_HASH_SPLIT,                                 \
      "last_reconcile_lowest_validate_headers_hash_split", false)        \
    X(LOWEST_BODY_FETCH_REFILL_HOLE,                                      \
      "last_reconcile_lowest_body_fetch_refill_hole", false)             \
    X(LOWEST_BODY_PERSIST_REFILL_HOLE,                                    \
      "last_reconcile_lowest_body_persist_refill_hole", false)           \
    X(SCRIPTS_SET, "last_reconcile_scripts_set", false)                  \
    X(HAVE_DATA_SET, "last_reconcile_have_data_set", false)              \
    X(HAVE_DATA_CLEARED, "last_reconcile_have_data_cleared", false)      \
    X(FAILED_MASK_CLEARED, "last_reconcile_failed_mask_cleared", false)  \
    X(HEADER_EVENTS_EMITTED, "last_reconcile_header_events_emitted", false)\
    X(STALE_SCRIPT_REPAIR_ATTEMPTED,                                      \
      "last_reconcile_stale_script_repair_attempted", true)              \
    X(STALE_SCRIPT_REPAIRED, "last_reconcile_stale_script_repaired", true)\
    X(STALE_SCRIPT_REPAIR_MARKER_SEEN,                                    \
      "last_reconcile_stale_script_repair_marker_seen", true)            \
    X(STALE_SCRIPT_REPAIR_GENUINELY_INVALID,                              \
      "last_reconcile_stale_script_repair_genuinely_invalid", true)      \
    X(STALE_SCRIPT_REPAIR_HEIGHT,                                         \
      "last_reconcile_stale_script_repair_height", false)                \
    X(STALE_SCRIPT_CURSOR_BEFORE,                                         \
      "last_reconcile_stale_script_cursor_before", false)                \
    X(STALE_SCRIPT_CURSOR_AFTER,                                          \
      "last_reconcile_stale_script_cursor_after", false)                 \
    X(STALE_SCRIPT_BACKFILL_FIRST,                                        \
      "last_reconcile_stale_script_backfill_first", false)               \
    X(STALE_SCRIPT_BACKFILL_LAST,                                         \
      "last_reconcile_stale_script_backfill_last", false)                \
    X(STALE_SCRIPT_UTXO_CURSOR_BEFORE,                                    \
      "last_reconcile_stale_script_utxo_cursor_before", false)           \
    X(STALE_SCRIPT_TIP_CURSOR_BEFORE,                                     \
      "last_reconcile_stale_script_tip_cursor_before", false)            \
    X(COIN_BACKFILL_ATTEMPTED,                                            \
      "last_reconcile_coin_backfill_attempted", true)                    \
    X(COIN_BACKFILL_STATUS, "last_reconcile_coin_backfill_status", false)\
    X(COIN_BACKFILL_HOLE_HEIGHT,                                          \
      "last_reconcile_coin_backfill_hole_height", false)                 \
    X(COIN_BACKFILL_UNRESOLVED,                                           \
      "last_reconcile_coin_backfill_unresolved", false)                  \
    X(COIN_BACKFILL_INSERTED,                                             \
      "last_reconcile_coin_backfill_inserted", false)                    \
    X(COIN_BACKFILL_SCAN_NEXT,                                            \
      "last_reconcile_coin_backfill_scan_next", false)                   \
    X(COIN_BACKFILL_OWNER_REFUSED,                                        \
      "last_reconcile_coin_backfill_owner_refused", true)                \
    X(COIN_BACKFILL_GENUINELY_INVALID,                                    \
      "last_reconcile_coin_backfill_genuinely_invalid", true)            \
    X(TIPFIN_BACKFILL_HEIGHT,                                             \
      "last_reconcile_tipfin_backfill_height", false)                    \
    X(TIPFIN_BACKFILL_COUNT,                                              \
      "last_reconcile_tipfin_backfill_count", false)                     \
    X(TIPFIN_BACKFILL_MARKER_SEEN,                                        \
      "last_reconcile_tipfin_backfill_marker_seen", true)                \
    X(TIPFIN_BACKFILL_REFUSED_REASON,                                     \
      "last_reconcile_tipfin_backfill_refused_reason", false)            \
    X(TIPFIN_BACKFILL_REFUSED_HEIGHT,                                     \
      "last_reconcile_tipfin_backfill_refused_height", false)            \
    X(TIPFIN_BACKFILL_REFUSED_LOG,                                        \
      "last_reconcile_tipfin_backfill_refused_log", false)               \
    X(NONCANONICAL_FOUND, "last_reconcile_noncanonical_found", false)    \
    X(NONCANONICAL_PURGED, "last_reconcile_noncanonical_purged", false)  \
    X(LOWEST_NONCANONICAL, "last_reconcile_lowest_noncanonical", false)  \
    X(REORG_RESIDUE_TIPFIN_FOUND,                                         \
      "last_reconcile_reorg_residue_tipfin_found", false)                \
    X(REORG_RESIDUE_TIPFIN_REPLACED,                                      \
      "last_reconcile_reorg_residue_tipfin_replaced", false)             \
    X(LOWEST_REORG_RESIDUE_TIPFIN,                                        \
      "last_reconcile_lowest_reorg_residue_tipfin", false)

enum rfrl_rr_field {
#define RFRL_RR_ENUM(id, key, is_bool) RFRL_RR_##id,
    RFRL_RR_FIELD_LIST(RFRL_RR_ENUM)
#undef RFRL_RR_ENUM
    RFRL_RR_FIELD_N
};

enum rfrl_rr_phase {
    RFRL_RR_PHASE_NONE = 0,
    RFRL_RR_PHASE_DETECT = 1,
    RFRL_RR_PHASE_REMEDY = 2,
};

struct rfrl_rr_field_meta {
    const char *key;
    bool is_bool;
};

static const struct rfrl_rr_field_meta
    k_rfrl_rr_fields[RFRL_RR_FIELD_N] = {
#define RFRL_RR_META(id, key, is_bool) [RFRL_RR_##id] = { key, is_bool },
        RFRL_RR_FIELD_LIST(RFRL_RR_META)
#undef RFRL_RR_META
};

static _Atomic int g_last_rr_seen = 0;
static _Atomic int g_last_rr_phase = RFRL_RR_PHASE_NONE;
static _Atomic int g_last_rr_values[RFRL_RR_FIELD_N];

/* Peer-gate visibility memos. detect runs only on the serial condition-engine
 * tick thread; the active bit is atomic because diagnostics can read it. The
 * gate-suppress WARN is the shared log_throttle de-storm primitive, keyed on a
 * single "active" token: log_throttle_reset() re-arms it when the suppression
 * ends (so the next engagement emits immediately, reps=0), and a same-key
 * keep-alive fires every 300 s carrying the suppressed-tick count. */
static _Atomic bool g_tear_bypass_active;
#define GATE_SUPPRESS_ACTIVE_KEY ((uint64_t)1)
static struct log_throttle g_gate_suppress = LOG_THROTTLE_INIT;

#ifdef ZCL_TESTING
/* Test-only post-remedy hook: simulates the TIPFIN backfill bumping its
 * tipfin_backfill.progress record DURING the remedy (the production writer
 * lives in the TIPFIN package; this Condition only snapshots/witnesses the
 * record). Lets the T10 harness prove the witness channel through the real
 * engine without that package. */
static void (*g_test_post_remedy_hook)(void);
#endif

static const char *coin_backfill_status_label(int status)
{
    switch ((enum coin_backfill_status)status) {
    case COIN_BACKFILL_NOT_APPLICABLE:     return "not_applicable";
    case COIN_BACKFILL_SCANNING:           return "scanning";
    case COIN_BACKFILL_REPAIRED:           return "repaired";
    case COIN_BACKFILL_OWNER_REFUSED:      return "owner_refused";
    case COIN_BACKFILL_REFUSED_SPENT:      return "refused_spent";
    case COIN_BACKFILL_REFUSED_UNPROVABLE: return "refused_unprovable";
    case COIN_BACKFILL_MARKER_SEEN:        return "marker_seen";
    }
    return "unknown";
}

static const char *rfrl_rr_phase_label(int phase)
{
    switch ((enum rfrl_rr_phase)phase) {
    case RFRL_RR_PHASE_NONE:   return "none";
    case RFRL_RR_PHASE_DETECT: return "detect";
    case RFRL_RR_PHASE_REMEDY: return "remedy";
    }
    return "unknown";
}

#define RFRL_RR_SET_BOOL(id, value) \
    atomic_store(&g_last_rr_values[RFRL_RR_##id], (value) ? 1 : 0)
#define RFRL_RR_SET_INT(id, value) \
    atomic_store(&g_last_rr_values[RFRL_RR_##id], (value))

static void rfrl_snapshot_reconcile_result(
    enum rfrl_rr_phase phase,
    const struct stage_reducer_frontier_reconcile_result *rr)
{
    if (!rr)
        return;

    RFRL_RR_SET_BOOL(REPAIRED, rr->repaired);
    RFRL_RR_SET_BOOL(CLAMPED_TIP_FINALIZE, rr->clamped_tip_finalize);
    RFRL_RR_SET_BOOL(REFUSED_COIN_TEAR, rr->refused_coin_tear);
    RFRL_RR_SET_BOOL(REFUSED_COIN_UNKNOWN, rr->refused_coin_unknown);
    RFRL_RR_SET_BOOL(COINS_APPLIED_FOUND, rr->coins_applied_found);
    RFRL_RR_SET_INT(HSTAR, rr->hstar);
    RFRL_RR_SET_INT(SERVED_FLOOR, rr->served_floor);
    RFRL_RR_SET_INT(COINS_APPLIED_HEIGHT, rr->coins_applied_height);
    RFRL_RR_SET_BOOL(CLAMPED_VALIDATE_HEADERS,
                     rr->clamped_validate_headers);
    RFRL_RR_SET_INT(VALIDATE_HEADERS_CURSOR_BEFORE,
                    rr->validate_headers_cursor_before);
    RFRL_RR_SET_INT(VALIDATE_HEADERS_CURSOR_AFTER,
                    rr->validate_headers_cursor_after);
    RFRL_RR_SET_BOOL(CLAMPED_BODY_FETCH, rr->clamped_body_fetch);
    RFRL_RR_SET_INT(BODY_FETCH_CURSOR_BEFORE,
                    rr->body_fetch_cursor_before);
    RFRL_RR_SET_INT(BODY_FETCH_CURSOR_AFTER,
                    rr->body_fetch_cursor_after);
    RFRL_RR_SET_BOOL(CLAMPED_BODY_PERSIST, rr->clamped_body_persist);
    RFRL_RR_SET_INT(BODY_PERSIST_CURSOR_BEFORE,
                    rr->body_persist_cursor_before);
    RFRL_RR_SET_INT(BODY_PERSIST_CURSOR_AFTER,
                    rr->body_persist_cursor_after);
    RFRL_RR_SET_INT(TIP_FINALIZE_CURSOR_BEFORE,
                    rr->tip_finalize_cursor_before);
    RFRL_RR_SET_INT(TIP_FINALIZE_CURSOR_AFTER,
                    rr->tip_finalize_cursor_after);
    RFRL_RR_SET_INT(SWEEP_TOP, rr->sweep_top);
    RFRL_RR_SET_INT(LOWEST_HAVE_DATA_CLEARED,
                    rr->lowest_have_data_cleared);
    RFRL_RR_SET_INT(LOWEST_VALIDATE_HEADERS_REFILL_HOLE,
                    rr->lowest_validate_headers_refill_hole);
    RFRL_RR_SET_INT(LOWEST_VALIDATE_HEADERS_HASH_SPLIT,
                    rr->lowest_validate_headers_hash_split);
    RFRL_RR_SET_INT(LOWEST_BODY_FETCH_REFILL_HOLE,
                    rr->lowest_body_fetch_refill_hole);
    RFRL_RR_SET_INT(LOWEST_BODY_PERSIST_REFILL_HOLE,
                    rr->lowest_body_persist_refill_hole);
    RFRL_RR_SET_INT(SCRIPTS_SET, rr->scripts_set);
    RFRL_RR_SET_INT(HAVE_DATA_SET, rr->have_data_set);
    RFRL_RR_SET_INT(HAVE_DATA_CLEARED, rr->have_data_cleared);
    RFRL_RR_SET_INT(FAILED_MASK_CLEARED, rr->failed_mask_cleared);
    RFRL_RR_SET_INT(HEADER_EVENTS_EMITTED, rr->header_events_emitted);
    RFRL_RR_SET_BOOL(STALE_SCRIPT_REPAIR_ATTEMPTED,
                     rr->stale_script_repair_attempted);
    RFRL_RR_SET_BOOL(STALE_SCRIPT_REPAIRED,
                     rr->stale_script_repaired);
    RFRL_RR_SET_BOOL(STALE_SCRIPT_REPAIR_MARKER_SEEN,
                     rr->stale_script_repair_marker_seen);
    RFRL_RR_SET_BOOL(STALE_SCRIPT_REPAIR_GENUINELY_INVALID,
                     rr->stale_script_repair_genuinely_invalid);
    RFRL_RR_SET_INT(STALE_SCRIPT_REPAIR_HEIGHT,
                    rr->stale_script_repair_height);
    RFRL_RR_SET_INT(STALE_SCRIPT_CURSOR_BEFORE,
                    rr->stale_script_cursor_before);
    RFRL_RR_SET_INT(STALE_SCRIPT_CURSOR_AFTER,
                    rr->stale_script_cursor_after);
    RFRL_RR_SET_INT(STALE_SCRIPT_BACKFILL_FIRST,
                    rr->stale_script_backfill_first);
    RFRL_RR_SET_INT(STALE_SCRIPT_BACKFILL_LAST,
                    rr->stale_script_backfill_last);
    RFRL_RR_SET_INT(STALE_SCRIPT_UTXO_CURSOR_BEFORE,
                    rr->stale_script_utxo_cursor_before);
    RFRL_RR_SET_INT(STALE_SCRIPT_TIP_CURSOR_BEFORE,
                    rr->stale_script_tip_cursor_before);
    RFRL_RR_SET_BOOL(COIN_BACKFILL_ATTEMPTED,
                     rr->coin_backfill_attempted);
    RFRL_RR_SET_INT(COIN_BACKFILL_STATUS, rr->coin_backfill_status);
    RFRL_RR_SET_INT(COIN_BACKFILL_HOLE_HEIGHT,
                    rr->coin_backfill_hole_height);
    RFRL_RR_SET_INT(COIN_BACKFILL_UNRESOLVED,
                    rr->coin_backfill_unresolved);
    RFRL_RR_SET_INT(COIN_BACKFILL_INSERTED,
                    rr->coin_backfill_inserted);
    RFRL_RR_SET_INT(COIN_BACKFILL_SCAN_NEXT,
                    rr->coin_backfill_scan_next);
    RFRL_RR_SET_BOOL(COIN_BACKFILL_OWNER_REFUSED,
                     rr->coin_backfill_owner_refused);
    RFRL_RR_SET_BOOL(COIN_BACKFILL_GENUINELY_INVALID,
                     rr->coin_backfill_genuinely_invalid);
    RFRL_RR_SET_INT(TIPFIN_BACKFILL_HEIGHT,
                    rr->tipfin_backfill_height);
    RFRL_RR_SET_INT(TIPFIN_BACKFILL_COUNT, rr->tipfin_backfill_count);
    RFRL_RR_SET_BOOL(TIPFIN_BACKFILL_MARKER_SEEN,
                     rr->tipfin_backfill_marker_seen);
    RFRL_RR_SET_INT(TIPFIN_BACKFILL_REFUSED_REASON,
                    rr->tipfin_backfill_refused_reason);
    RFRL_RR_SET_INT(TIPFIN_BACKFILL_REFUSED_HEIGHT,
                    rr->tipfin_backfill_refused_height);
    RFRL_RR_SET_INT(TIPFIN_BACKFILL_REFUSED_LOG,
                    rr->tipfin_backfill_refused_log);
    RFRL_RR_SET_INT(NONCANONICAL_FOUND, rr->noncanonical_found);
    RFRL_RR_SET_INT(NONCANONICAL_PURGED, rr->noncanonical_purged);
    RFRL_RR_SET_INT(LOWEST_NONCANONICAL, rr->lowest_noncanonical);
    RFRL_RR_SET_INT(REORG_RESIDUE_TIPFIN_FOUND,
                    rr->reorg_residue_tipfin_found);
    RFRL_RR_SET_INT(REORG_RESIDUE_TIPFIN_REPLACED,
                    rr->reorg_residue_tipfin_replaced);
    RFRL_RR_SET_INT(LOWEST_REORG_RESIDUE_TIPFIN,
                    rr->lowest_reorg_residue_tipfin);

    atomic_store(&g_last_rr_phase, (int)phase);
    atomic_store(&g_last_rr_seen, 1);
}

#undef RFRL_RR_SET_BOOL
#undef RFRL_RR_SET_INT

static bool rfrl_push_reconcile_snapshot(struct json_value *out)
{
    bool ok = true;
    int phase = atomic_load(&g_last_rr_phase);
    int coin_status = atomic_load(&g_last_rr_values[RFRL_RR_COIN_BACKFILL_STATUS]);

    ok = ok && json_push_kv_bool(out, "last_reconcile_seen",
                                 atomic_load(&g_last_rr_seen) != 0);
    ok = ok && json_push_kv_str(out, "last_reconcile_phase",
                                rfrl_rr_phase_label(phase));
    for (int i = 0; i < RFRL_RR_FIELD_N; i++) {
        int value = atomic_load(&g_last_rr_values[i]);
        if (k_rfrl_rr_fields[i].is_bool) {
            ok = ok && json_push_kv_bool(out, k_rfrl_rr_fields[i].key,
                                         value != 0);
        } else {
            ok = ok && json_push_kv_int(out, k_rfrl_rr_fields[i].key,
                                        value);
        }
    }
    ok = ok && json_push_kv_str(
        out, "last_reconcile_coin_backfill_status_label",
        coin_backfill_status_label(coin_status));
    return ok;
}

#ifdef ZCL_TESTING
/* Test-only: the sole caller (the reset hook at the ZCL_TESTING block below)
 * is compiled out of the production binary, so the definition must be too, or
 * -Werror=unused-function breaks the prod whole-program build. */
static void rfrl_reconcile_snapshot_reset(void)
{
    atomic_store(&g_last_rr_seen, 0);
    atomic_store(&g_last_rr_phase, RFRL_RR_PHASE_NONE);
    for (int i = 0; i < RFRL_RR_FIELD_N; i++)
        atomic_store(&g_last_rr_values[i], 0);
}
#endif

static bool coin_backfill_refused_reconcile(
    const struct stage_reducer_frontier_reconcile_result *rr)
{
    if (!rr || !rr->coin_backfill_attempted)
        return false;
    return rr->coin_backfill_status != COIN_BACKFILL_NOT_APPLICABLE &&
           rr->coin_backfill_status != COIN_BACKFILL_SCANNING &&
           rr->coin_backfill_status != COIN_BACKFILL_REPAIRED &&
           rr->coin_backfill_status != COIN_BACKFILL_OWNER_REFUSED;
}

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

/* ── tipfin backfill witness channel ────────────────────────────────────
 * The TIPFIN no-spend backfill persists a progress_meta record keyed
 * tipfin_backfill.progress whose value begins [progress i32 LE], bumped
 * after every repaired batch (that package owns the writes/deletes; this
 * Condition only snapshots/witnesses). Mirrors the coin_backfill.scan
 * channel exactly, INCLUDING the absent->present (backfill started) and
 * present->absent (backfill completed and consumed) transitions: a
 * row-only backfill moves NO stage cursor and NO chain height, so it is
 * structurally unwitnessable through the channels above — without this one
 * the attempt budget freezes at max_attempts and pages the operator while
 * the repair is genuinely progressing (attempt-budget starvation). */
static bool read_tipfin_backfill_progress(sqlite3 *db, bool *present,
                                          int *progress)
{
    *present = false;
    *progress = -1;
    if (!db)
        return false;

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM progress_meta "
            "WHERE key = 'tipfin_backfill.progress'",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] tipfin_backfill "
                 "progress record prepare failed: %s",
                 sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return false;
    }

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *present = true;
        const uint8_t *v = sqlite3_column_blob(st, 0);
        if (v && sqlite3_column_bytes(st, 0) >= 4)
            *progress = (int)((uint32_t)v[0] | (uint32_t)v[1] << 8 |
                              (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] tipfin_backfill "
                 "progress record step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        progress_store_tx_unlock();
        return false;
    }

    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return true;
}

static void snapshot_tipfin_backfill(sqlite3 *db)
{
    bool present = false;
    int progress = -1;
    if (!read_tipfin_backfill_progress(db, &present, &progress))
        return;
    atomic_store(&g_tipfin_backfill_present_at_detect, present ? 1 : 0);
    atomic_store(&g_tipfin_backfill_progress_at_detect, progress);
}

/* Same baseline semantics as coin_backfill_scan_witnessed: an ABSENT record
 * at detect is a valid baseline; only "no detect snapshot at all" refuses. */
static bool tipfin_backfill_witnessed(sqlite3 *db)
{
    int present_before = atomic_load(&g_tipfin_backfill_present_at_detect);
    if (present_before < 0)
        return false;
    int progress_before = atomic_load(&g_tipfin_backfill_progress_at_detect);

    bool present_now = false;
    int progress_now = -1;
    if (!read_tipfin_backfill_progress(db, &present_now, &progress_now))
        return false;

    if ((present_before != 0) != present_now)
        return true;
    return present_now && progress_now != progress_before;
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

/* Peer-gate BYPASS for a pending refused_coin_tear: the gate's
 * purpose — refusing repairs without peer-staleness evidence — does not
 * apply here, because coins_applied_height sitting above H*+1 is durable
 * internal-store evidence of damage, independent of any peer's height
 * (guards against the peer gate silently idling). Transition-logged: one WARN
 * when the bypass engages, re-armed when the tear state ends. */
static void note_tear_bypass(
    const struct stage_reducer_frontier_reconcile_result *rr)
{
    if (atomic_load(&g_tear_bypass_active))
        return;
    atomic_store(&g_tear_bypass_active, true);
    atomic_fetch_add(&g_tear_bypass_warn_total, 1);
    LOG_WARN("condition",
             "[condition:reducer_frontier_reconcile_light] peer-gate BYPASS: "
             "refused_coin_tear pending (coins_applied_height=%d hstar=%d) — "
             "a durable internal-state tear is peer-independent evidence, "
             "detect proceeds with no peer ahead",
             rr->coins_applied_height, rr->hstar);
}

/* True when the dry-run reports any refusal/backfill-pending signal, or a
 * backfill progress record (coin or tipfin) is live on disk. Used only to
 * decide whether a gate suppression must be LOUD. */
static bool repair_evidence_pending(
    sqlite3 *db,
    const struct stage_reducer_frontier_reconcile_result *rr)
{
    if (rr->value_overflow_repair_owner_refused ||
        rr->value_overflow_repair_attempted ||
        rr->coin_backfill_owner_refused ||
        rr->coin_backfill_attempted ||
        rr->stale_script_repair_attempted ||
        rr->tipfin_backfill_count > 0 ||
        rr->tipfin_backfill_refused_reason ||
        rr->reorg_residue_tipfin_found > 0)
        return true;

    bool present = false;
    int v = -1;
    if (read_coin_backfill_scan_cursor(db, &present, &v) && present)
        return true;
    if (read_tipfin_backfill_progress(db, &present, &v) && present)
        return true;
    return false;
}

/* The peer gate can suppress an actionable detect, silently idling the whole
 * L1 layer; this logging keeps such suppressions visible. Stay quiet only for
 * the plain cursor-churn class with no other repair evidence (the gate's
 * intended job); otherwise WARN on the transition plus a 300 s keep-alive
 * carrying the suppressed-tick count (storm-safe: first occurrence never
 * suppressed). */
static void note_gate_suppressed(
    sqlite3 *db,
    const struct stage_reducer_frontier_reconcile_result *rr)
{
    if (!repair_evidence_pending(db, rr)) {
        log_throttle_reset(&g_gate_suppress);
        return;
    }
    int64_t now = platform_time_wall_unix();
    uint64_t reps = 0;
    if (!log_throttle_should_emit(&g_gate_suppress, GATE_SUPPRESS_ACTIVE_KEY,
                                  now, 300, &reps))
        return;
    LOG_WARN("condition",
             "[condition:reducer_frontier_reconcile_light] peer gate "
             "suppressed an actionable detect (repaired=%d "
             "vo_owner_refused=%d vo_attempted=%d cb_owner_refused=%d "
             "cb_attempted=%d stale_script_attempted=%d) while "
             "repair/backfill evidence is pending and no peer is ahead "
             "(suppressed_ticks=%llu)",
             rr->repaired, rr->value_overflow_repair_owner_refused,
             rr->value_overflow_repair_attempted,
             rr->coin_backfill_owner_refused, rr->coin_backfill_attempted,
             rr->stale_script_repair_attempted,
             (unsigned long long)reps);
}

static bool detect_reducer_frontier_reconcile_light(void)
{
    /* SUSPENDED during a from-genesis staged refold (jobs/refold_progress.h):
     * this self-repair drags below-anchor cursors back UP to the trusted
     * anchor, which is exactly what a refold must NOT do — the fold is
     * legitimately re-walking the frozen prefix from genesis. With the floor
     * lowered to 0 the L0 frontier reports the true folded height; this gate
     * stops L1 from fighting it. The condition is GATED, not deleted — a
     * normal boot (refold_in_progress()==false) runs the standard L1 path.
     *
     * This serial condition-engine tick is also the off-the-drive owner of the
     * CLEAR edge: once the fold's utxo_apply cursor reaches/passes the clear
     * target, the matching clear helper drops the durable signal and the very
     * next tick falls through to the normal self-repair. (Both clear helpers are
     * cheap no-ops when not refolding or still below their target.)
     *
     * B2 — a from-ANCHOR refold (refold_from_anchor_active()) clears when the
     * fold's utxo_apply cursor reaches the durable RESUME TARGET (the tip it is
     * climbing to), NOT the trusted anchor (the fold STARTS at the anchor). A
     * from-genesis refold keeps the original anchor-crossing clear edge. */
    if (refold_in_progress()) {
        sqlite3 *db = progress_store_db();
        if (db) {
            int ua = -1;
            if (read_reducer_cursor(db, "utxo_apply", &ua) && ua >= 0) {
                if (refold_from_anchor_active()) {
                    /* CUTOVER DEFECT 1 — the from-anchor resume target is
                     * captured ONCE at boot, but the chain advances during a
                     * multi-hour fold. Re-write the durable target to
                     * MAX(stored, live tip) BEFORE the clear so the clear edge
                     * keys on the CURRENT tip, not the frozen boot height —
                     * otherwise utxo_apply crossing the stale boot target
                     * un-suspends below-anchor self-repair while the fold is
                     * still climbing to the real tip (the re-wedge surface).
                     * Touches ONLY progress.kv — no csr->lock, no evidence
                     * machinery (lock-order-safe on the reducer-drive path). */
                    struct main_state *ms = sync_monitor_main_state();
                    if (ms) {
                        int live_tip =
                            active_chain_height(&ms->chain_active);
                        if (live_tip >= 0)
                            (void)refold_progress_bump_target(
                                db, (int32_t)live_tip);
                    }
                    (void)refold_progress_clear_if_reached(db, (int32_t)ua, -1);
                } else {
                    (void)refold_progress_clear_if_crossed(db, (int32_t)ua);
                }
            }
        }
        if (refold_in_progress())
            return false;
    }

    int64_t tip_age = sync_monitor_tip_advance_age();
    if (tip_age >= 0 && tip_age < 60)
        return false;

    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms)
        return false;

    /* The read-only dry-run runs BEFORE the peer gate so a durable
     * internal tear can bypass it. No new steady-state cost: it already ran
     * on every detect tick whenever a peer was ahead. */
    struct stage_reducer_frontier_reconcile_result rr;
    if (!stage_reducer_frontier_reconcile_light_needed(db, ms, &rr))
        return false;
    rfrl_snapshot_reconcile_result(RFRL_RR_PHASE_DETECT, &rr);
    if (rr.refused_coin_unknown)
        return false;
    if (stage_repair_tipfin_refusal_is_pending_forward(&rr)) {
        atomic_store(&g_tear_bypass_active, false);
        log_throttle_reset(&g_gate_suppress);
        return false;
    }
    if (!rr.refused_coin_tear && !rr.repaired &&
        rr.noncanonical_found == 0 &&
        rr.reorg_residue_tipfin_found == 0) {
        /* Nothing actionable: both transition memos re-arm.
         * noncanonical_found counts relabel/reorg-residue rows the
         * dry-run judged stale — the apply purge is the remedy.
         * reorg_residue_tipfin_found counts stale ok=0 reorg_detected
         * tip_finalize verdicts the apply path replaces in place. */
        atomic_store(&g_tear_bypass_active, false);
        log_throttle_reset(&g_gate_suppress);
        return false;
    }

    if (!peer_lag_allows_repair(ms)) {
        if (rr.refused_coin_tear) {
            note_tear_bypass(&rr);
        } else {
            /* Gate KEPT for the plain cursor-churn repair class: peers that
             * exist but are not ahead are no staleness evidence. The tear
             * state (if any) ended, so the bypass memo re-arms. */
            atomic_store(&g_tear_bypass_active, false);
            note_gate_suppressed(db, &rr);
            return false;
        }
    } else {
        atomic_store(&g_tear_bypass_active, false);
    }
    log_throttle_reset(&g_gate_suppress);

    atomic_store(&g_local_height_at_detect,
                 active_chain_height(&ms->chain_active));
    atomic_store(&g_hstar_at_detect, rr.hstar);
    atomic_store(&g_sweep_top_at_detect, rr.sweep_top);
    snapshot_reducer_cursors(db);
    snapshot_coin_backfill_scan(db);
    snapshot_tipfin_backfill(db);
    return true;
}

static enum condition_remedy_result remedy_reducer_frontier_reconcile_light(void)
{
    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);
#ifdef ZCL_TESTING
    /* Stands in for the TIPFIN backfill's in-remedy progress-record bump
     * (see the hook's declaration comment). */
    if (g_test_post_remedy_hook)
        g_test_post_remedy_hook();
#endif

    struct stage_reducer_frontier_reconcile_result rr;
    if (!stage_reducer_frontier_reconcile_light(db, ms, &rr))
        return COND_REMEDY_FAILED;
    rfrl_snapshot_reconcile_result(RFRL_RR_PHASE_REMEDY, &rr);
    if (rr.refused_coin_unknown) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "coins_applied_height absent");
        return COND_REMEDY_SKIP;
    }
    if (stage_repair_tipfin_refusal_is_pending_forward(&rr)) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] pending "
                 "tipfin backfill reason=%s binding_log=%s h=%d "
                 "coins_applied=%d hstar=%d",
                 stage_repair_tipfin_refused_reason_label(
                     rr.tipfin_backfill_refused_reason),
                 stage_repair_tipfin_refused_log_label(
                     rr.tipfin_backfill_refused_log),
                 rr.tipfin_backfill_refused_height,
                 rr.coins_applied_height, rr.hstar);
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
    if (coin_backfill_refused_reconcile(&rr)) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "coin backfill h=%d status=%s unresolved=%d inserted=%d "
                 "scan_next=%d",
                 rr.coin_backfill_hole_height,
                 coin_backfill_status_label(rr.coin_backfill_status),
                 rr.coin_backfill_unresolved, rr.coin_backfill_inserted,
                 rr.coin_backfill_scan_next);
        return COND_REMEDY_FAILED;
    }
    if (rr.stale_script_repair_genuinely_invalid) {
        LOG_WARN("condition",
                 "[condition:reducer_frontier_reconcile_light] refused "
                 "stale script replay h=%d: dry-run still invalid",
                 rr.stale_script_repair_height);
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
    if (coin_backfill_scan_witnessed(db))
        return true;
    return tipfin_backfill_witnessed(db);
}

static bool detail_reducer_frontier_reconcile_light(struct json_value *out)
{
    if (!out)
        return false;

    bool ok = true;
    ok = ok && json_push_kv_int(out, "local_height_at_detect",
                                atomic_load(&g_local_height_at_detect));
    ok = ok && json_push_kv_int(out, "hstar_at_detect",
                                atomic_load(&g_hstar_at_detect));
    ok = ok && json_push_kv_int(out, "sweep_top_at_detect",
                                atomic_load(&g_sweep_top_at_detect));
    ok = ok && json_push_kv_int(
        out, "validate_headers_cursor_at_detect",
        atomic_load(&g_validate_headers_cursor_at_detect));
    ok = ok && json_push_kv_int(
        out, "body_fetch_cursor_at_detect",
        atomic_load(&g_body_fetch_cursor_at_detect));
    ok = ok && json_push_kv_int(
        out, "body_persist_cursor_at_detect",
        atomic_load(&g_body_persist_cursor_at_detect));
    ok = ok && json_push_kv_int(
        out, "script_validate_cursor_at_detect",
        atomic_load(&g_script_validate_cursor_at_detect));
    ok = ok && json_push_kv_int(
        out, "proof_validate_cursor_at_detect",
        atomic_load(&g_proof_validate_cursor_at_detect));
    ok = ok && json_push_kv_int(
        out, "utxo_apply_cursor_at_detect",
        atomic_load(&g_utxo_apply_cursor_at_detect));
    ok = ok && json_push_kv_int(
        out, "tip_finalize_cursor_at_detect",
        atomic_load(&g_tip_finalize_cursor_at_detect));
    ok = ok && json_push_kv_int(
        out, "coin_backfill_scan_present_at_detect",
        atomic_load(&g_coin_backfill_scan_present_at_detect));
    ok = ok && json_push_kv_int(
        out, "coin_backfill_scan_next_at_detect",
        atomic_load(&g_coin_backfill_scan_next_at_detect));
    ok = ok && json_push_kv_int(
        out, "tipfin_backfill_present_at_detect",
        atomic_load(&g_tipfin_backfill_present_at_detect));
    ok = ok && json_push_kv_int(
        out, "tipfin_backfill_progress_at_detect",
        atomic_load(&g_tipfin_backfill_progress_at_detect));
    ok = ok && json_push_kv_int(out, "remedy_calls",
                                atomic_load(&g_remedy_calls));
    ok = ok && json_push_kv_bool(out, "tear_bypass_active",
                                 atomic_load(&g_tear_bypass_active));
    ok = ok && json_push_kv_int(out, "tear_bypass_warn_total",
                                atomic_load(&g_tear_bypass_warn_total));
    ok = ok && rfrl_push_reconcile_snapshot(out);
    return ok;
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
    .detail = detail_reducer_frontier_reconcile_light,
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
    atomic_store(&g_tipfin_backfill_present_at_detect, -1);
    atomic_store(&g_tipfin_backfill_progress_at_detect, -1);
    atomic_store(&g_remedy_calls, 0);
    atomic_store(&g_tear_bypass_warn_total, 0);
    atomic_store(&g_tear_bypass_active, false);
    rfrl_reconcile_snapshot_reset();
    log_throttle_reset(&g_gate_suppress);
    g_test_post_remedy_hook = NULL;
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

/* src-private test hooks (mirrored by test_reducer_reconcile_witness.c, the
 * test_utxo_apply_stage.c delta-internal pattern — not in the public
 * header). */
void reducer_frontier_reconcile_light_test_set_post_remedy_hook(
    void (*fn)(void));
void reducer_frontier_reconcile_light_test_set_post_remedy_hook(
    void (*fn)(void))
{
    g_test_post_remedy_hook = fn;
}

int reducer_frontier_reconcile_light_test_bypass_warns(void);
int reducer_frontier_reconcile_light_test_bypass_warns(void)
{
    return atomic_load(&g_tear_bypass_warn_total);
}
#endif
