/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_frontier_dump - zcl_state diagnostics for the reducer L0 authority.
 * Read-only: every SQLite statement is a SELECT over progress.kv. */

#include "jobs/reducer_frontier.h"

#include "json/json.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"

#include <sqlite3.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct dump_log {
    const char *stage;
    const char *log_table;
    const char *cursor_name;
    bool served_tip_cursor;
};

struct dump_frontier {
    bool cursor_ok;
    bool frontier_ok;
    int64_t raw_cursor;
    int64_t next_cursor;
    int32_t contiguous_frontier;
};

struct hstar_blocker {
    bool found;
    bool pending_edge;
    const char *stage;
    const char *log_table;
    const char *pending_stage;
    const char *pending_log_table;
    int32_t height;
    const char *kind;
    char reason[128];
    char pending_reason[128];
};

static const struct dump_log k_logs[] = {
    { "validate_headers", "validate_headers_log", "validate_headers", false },
    { "script_validate",  "script_validate_log",  "script_validate",  false },
    { "body_persist",     "body_persist_log",     "body_persist",     false },
    { "proof_validate",   "proof_validate_log",   "proof_validate",   false },
    { "utxo_apply",       "utxo_apply_log",       "utxo_apply",       false },
    { "tip_finalize",     "tip_finalize_log",     "tip_finalize",     true  },
};

static const char *const k_stage_cursors[] = {
    "header_admit",
    "validate_headers",
    "body_fetch",
    "body_persist",
    "script_validate",
    "proof_validate",
    "utxo_apply",
    "tip_finalize",
};

static int64_t next_cursor_for_dump(const struct dump_log *log, int64_t cursor)
{
    return log && log->served_tip_cursor && cursor > 0 ? cursor + 1 : cursor;
}

/* Map a first-H*-blocker to the subsystem that owns its repair. Kind-keyed
 * with a reason refinement, and NEVER empty: the reason-only table this
 * replaced had no entry for kind=log_hole (missing-success-row), so the
 * 3166989 script_validate_log hole dumped repair_owner="" and the 3 h stall
 * surfaced with zero named owners. kind=ok0_failure keeps the two stored-
 * header reasons with stale_validate_headers_repair; log_hole (refill from
 * the on-disk body), hash_split (one-shot script re-derivation via
 * maybe_repair_validate_script_hash_split in try_replay_repairs), and every
 * other ok=0 reason are driven by the reducer_frontier_reconcile_light
 * condition. */
static const char *diagnostic_repair_hint(const char *kind, const char *reason)
{
    if (kind && strcmp(kind, "ok0_failure") == 0 && reason &&
        (strcmp(reason, "no-header-solution-backfill-required") == 0 ||
         strcmp(reason, "header-source-hash-mismatch") == 0))
        return "stale_validate_headers_repair";
    return "reducer_frontier_reconcile_light";
}

static bool table_exists(sqlite3 *db, const char *name, bool *out)
{
    *out = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master "
            "WHERE type = 'table' AND name = ? LIMIT 1",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "dump table_exists prepare failed: %s",
                 sqlite3_errmsg(db));
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = true;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "dump table_exists step failed table=%s: %s",
                 name, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool schema_ready(sqlite3 *db, char *missing, size_t missing_sz)
{
    if (missing && missing_sz > 0)
        missing[0] = '\0';

    bool present = false;
    if (!table_exists(db, "stage_cursor", &present))
        return false;
    if (!present) {
        if (missing && missing_sz > 0)
            snprintf(missing, missing_sz, "%s", "stage_cursor");
        return true;
    }
    if (!table_exists(db, "progress_meta", &present))
        return false;
    if (!present) {
        if (missing && missing_sz > 0)
            snprintf(missing, missing_sz, "%s", "progress_meta");
        return true;
    }
    for (size_t i = 0; i < sizeof(k_logs) / sizeof(k_logs[0]); i++) {
        if (!table_exists(db, k_logs[i].log_table, &present))
            return false;
        if (!present) {
            if (missing && missing_sz > 0)
                snprintf(missing, missing_sz, "%s", k_logs[i].log_table);
            return true;
        }
    }
    return true;
}

static bool cursor_at(sqlite3 *db, const char *name, int64_t *out)
{
    *out = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name = ?",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "dump cursor prepare failed: %s",
                 sqlite3_errmsg(db));
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "dump cursor step failed stage=%s: %s",
                 name, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool first_validate_failure(sqlite3 *db,
                                   int32_t min_exclusive,
                                   int32_t max_inclusive,
                                   bool *found,
                                   int32_t *height,
                                   char *reason,
                                   size_t reason_sz)
{
    *found = false;
    *height = -1;
    if (reason && reason_sz > 0)
        reason[0] = '\0';

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height, COALESCE(fail_reason, '') "
            "FROM validate_headers_log "
            "WHERE height > ? AND height <= ? AND ok = 0 "
            "ORDER BY height ASC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "dump first validate failure prepare failed: %s",
                 sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, min_exclusive);
    sqlite3_bind_int(st, 2, max_inclusive);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found = true;
        *height = (int32_t)sqlite3_column_int64(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (reason && reason_sz > 0)
            snprintf(reason, reason_sz, "%s", txt ? (const char *)txt : "");
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "dump first validate failure step failed: %s",
                 sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static const char *blocker_row_sql(const struct dump_log *log)
{
    if (!log)
        return NULL;
    if (strcmp(log->log_table, "validate_headers_log") == 0)
        return "SELECT ok, COALESCE(fail_reason, '') "
               "FROM validate_headers_log WHERE height = ?";
    if (strcmp(log->log_table, "script_validate_log") == 0)
        return "SELECT ok, COALESCE(status, '') "
               "FROM script_validate_log WHERE height = ?";
    if (strcmp(log->log_table, "body_persist_log") == 0)
        return "SELECT ok, COALESCE(source, '') "
               "FROM body_persist_log WHERE height = ?";
    if (strcmp(log->log_table, "proof_validate_log") == 0)
        return "SELECT ok, '' FROM proof_validate_log WHERE height = ?";
    if (strcmp(log->log_table, "utxo_apply_log") == 0)
        return "SELECT ok, '' FROM utxo_apply_log WHERE height = ?";
    if (strcmp(log->log_table, "tip_finalize_log") == 0)
        return "SELECT ok, COALESCE(status, '') "
               "FROM tip_finalize_log WHERE height = ?";
    return NULL;
}

static bool trusted_base_height_for_dump(sqlite3 *db, int32_t *out,
                                         bool *found)
{
    *out = -1;
    *found = false;
    uint8_t blob[8] = {0};
    size_t n = 0;
    bool present = false;
    if (!progress_meta_get(db, REDUCER_TRUSTED_BASE_HEIGHT_KEY,
                           blob, sizeof(blob), &n, &present))
        return false;
    if (!present)
        return true;
    if (n != sizeof(blob)) {
        LOG_WARN("reducer",
                 "dump trusted_base blob malformed (len=%zu)", n);
        return false;
    }
    int64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | blob[i];
    *out = (int32_t)v;
    *found = true;
    return true;
}

static bool blocker_row_at(sqlite3 *db,
                           const struct dump_log *log,
                           int32_t height,
                           bool *found,
                           bool *ok,
                           char *reason,
                           size_t reason_sz)
{
    *found = false;
    *ok = false;
    if (reason && reason_sz > 0)
        reason[0] = '\0';

    const char *sql = blocker_row_sql(log);
    if (!sql)
        LOG_FAIL("reducer", "dump blocker row has unknown log table");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "dump blocker row prepare failed table=%s: %s",
                 log->log_table, sqlite3_errmsg(db));
    sqlite3_bind_int64(st, 1, (sqlite3_int64)height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found = true;
        *ok = sqlite3_column_int(st, 0) != 0;
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (reason && reason_sz > 0)
            snprintf(reason, reason_sz, "%s", txt ? (const char *)txt : "");
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "dump blocker row step failed table=%s h=%d: %s",
                 log->log_table, height, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool hash_split_at(sqlite3 *db, int32_t height, bool *split)
{
    *split = false;
    uint8_t validate_hash[32] = {0};
    uint8_t script_hash[32] = {0};
    bool validate_found = false;
    bool script_found = false;

    if (!reducer_frontier_log_hash_at(db, "validate_headers_log", "hash",
                                      height, validate_hash, &validate_found))
        return false;
    if (!reducer_frontier_log_hash_at(db, "script_validate_log", "block_hash",
                                      height, script_hash, &script_found))
        return false;

    *split = validate_found && script_found &&
        memcmp(validate_hash, script_hash, sizeof(validate_hash)) != 0;
    return true;
}

static bool tip_finalize_pending_edge(const struct dump_log *log,
                                      const struct dump_frontier *fr,
                                      int64_t block_height)
{
    /* tip_finalize's raw cursor is the served tip. A finalized row at H proves
     * H, then the cursor advances to H+1 while the H+1 -> H+2 transition is
     * still pending. That expected frontier edge has no row at the raw cursor
     * yet and is not a repairable log hole. Older holes below the raw cursor
     * still report as blockers. */
    return log && fr && log->served_tip_cursor &&
           fr->raw_cursor == block_height &&
           fr->contiguous_frontier + 1 == block_height;
}

static bool first_hstar_blocker(sqlite3 *db,
                                int32_t hstar,
                                const struct dump_frontier *frontiers,
                                size_t frontier_count,
                                struct hstar_blocker *out)
{
    if (!out)
        LOG_FAIL("reducer", "dump hstar blocker missing out param");
    memset(out, 0, sizeof(*out));
    out->height = -1;
    out->kind = "";
    out->stage = "";
    out->log_table = "";
    out->pending_stage = "";
    out->pending_log_table = "";

    int64_t block_height = (int64_t)hstar + 1;
    if (block_height < 0 || block_height > INT32_MAX)
        return true;
    out->height = (int32_t)block_height;

    size_t log_count = sizeof(k_logs) / sizeof(k_logs[0]);
    if (frontier_count < log_count)
        LOG_FAIL("reducer", "dump hstar blocker frontier count too small");
    for (size_t i = 0; i < log_count; i++) {
        const struct dump_frontier *fr = &frontiers[i];
        if (!fr->cursor_ok || !fr->frontier_ok)
            continue;
        if (fr->contiguous_frontier != hstar)
            continue;
        if (fr->next_cursor <= block_height)
            continue;

        bool row_found = false;
        bool row_ok = false;
        char reason[128] = "";
        if (!blocker_row_at(db, &k_logs[i], out->height, &row_found, &row_ok,
                            reason, sizeof(reason)))
            return false;
        if (!row_found) {
            if (tip_finalize_pending_edge(&k_logs[i], fr, block_height)) {
                out->pending_edge = true;
                out->pending_stage = k_logs[i].stage;
                out->pending_log_table = k_logs[i].log_table;
                snprintf(out->pending_reason, sizeof(out->pending_reason),
                         "%s", "tip-finalize-edge-pending");
                continue;
            }
            out->found = true;
            out->stage = k_logs[i].stage;
            out->log_table = k_logs[i].log_table;
            out->kind = "log_hole";
            snprintf(out->reason, sizeof(out->reason),
                     "%s", "missing-success-row");
            return true;
        }
        if (!row_ok) {
            out->found = true;
            out->stage = k_logs[i].stage;
            out->log_table = k_logs[i].log_table;
            out->kind = "ok0_failure";
            snprintf(out->reason, sizeof(out->reason),
                     "%s", reason[0] ? reason : "ok=0");
            return true;
        }
    }

    bool split = false;
    if (!hash_split_at(db, out->height, &split))
        return false;
    if (split) {
        out->found = true;
        out->stage = "script_validate";
        out->log_table = "script_validate_log";
        out->kind = "hash_split";
        snprintf(out->reason, sizeof(out->reason),
                 "%s", "validate-script-hash-mismatch");
        return true;
    }

    out->height = -1;
    return true;
}

bool reducer_frontier_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    json_push_kv_bool(out, "open", db != NULL);
    json_push_kv_str(out, "authority", "reducer_frontier_hstar");
    json_push_kv_int(out, "floor", reducer_frontier_floor());
    json_push_kv_int(out, "cached_provable_tip",
                     reducer_frontier_provable_tip_cached());
    if (!db) {
        json_push_kv_bool(out, "schema_ready", false);
        json_push_kv_str(out, "schema_missing", "progress_store");
        return true;
    }

    progress_store_tx_lock();

    char missing[64] = "";
    bool ready = schema_ready(db, missing, sizeof(missing));
    if (!ready) {
        progress_store_tx_unlock();
        return false;
    }
    json_push_kv_bool(out, "schema_ready", missing[0] == '\0');
    json_push_kv_str(out, "schema_missing", missing);
    if (missing[0] != '\0') {
        progress_store_tx_unlock();
        return true;
    }

    int32_t hstar = -1;
    int32_t served_floor = -1;
    if (!reducer_frontier_compute_hstar(db, &hstar, &served_floor)) {
        progress_store_tx_unlock();
        return false;
    }
    json_push_kv_int(out, "hstar", hstar);
    json_push_kv_int(out, "served_floor", served_floor);
    json_push_kv_int(out, "served_gap",
                     served_floor > hstar ? (int64_t)served_floor - hstar : 0);
    json_push_kv_bool(out, "served_above_hstar", served_floor > hstar);

    int32_t trusted_base_height = -1;
    bool trusted_base_found = false;
    bool trusted_base_ok =
        trusted_base_height_for_dump(db, &trusted_base_height,
                                     &trusted_base_found);
    json_push_kv_bool(out, "trusted_base_read_ok", trusted_base_ok);
    json_push_kv_bool(out, "trusted_base_found",
                      trusted_base_ok && trusted_base_found);
    json_push_kv_int(out, "trusted_base_height",
                     (trusted_base_ok && trusted_base_found)
                         ? trusted_base_height
                         : -1);
    json_push_kv_bool(out, "trusted_base_above_hstar",
                      trusted_base_ok && trusted_base_found &&
                      trusted_base_height > hstar);
    json_push_kv_bool(out, "trusted_base_accepted",
                      trusted_base_ok && trusted_base_found &&
                      trusted_base_height <= hstar);

    int32_t coins_applied = -1;
    bool coins_found = false;
    bool coins_ok = coins_kv_get_applied_height(db, &coins_applied,
                                                &coins_found);
    json_push_kv_bool(out, "coins_applied_read_ok", coins_ok);
    json_push_kv_bool(out, "coins_applied_found", coins_ok && coins_found);
    json_push_kv_int(out, "coins_applied_height",
                     (coins_ok && coins_found) ? coins_applied : -1);
    json_push_kv_int(out, "coins_best_height",
                     (coins_ok && coins_found) ? coins_applied - 1 : -1);
    json_push_kv_bool(out, "coins_best_above_hstar",
                      coins_ok && coins_found && coins_applied - 1 > hstar);

    struct json_value cursors = {0};
    json_set_array(&cursors);
    for (size_t i = 0;
         i < sizeof(k_stage_cursors) / sizeof(k_stage_cursors[0]);
         i++) {
        int64_t cursor = 0;
        bool ok = cursor_at(db, k_stage_cursors[i], &cursor);
        struct json_value obj = {0};
        json_set_object(&obj);
        json_push_kv_str(&obj, "stage", k_stage_cursors[i]);
        json_push_kv_bool(&obj, "read_ok", ok);
        json_push_kv_int(&obj, "cursor", ok ? cursor : -1);
        json_push_back(&cursors, &obj);
        json_free(&obj);
    }
    json_push_kv(out, "stage_cursors", &cursors);
    json_free(&cursors);

    struct json_value frontiers = {0};
    json_set_array(&frontiers);
    int32_t validate_frontier = hstar;
    int32_t validate_cursor_next = INT32_MAX;
    struct dump_frontier frontier_state[
        sizeof(k_logs) / sizeof(k_logs[0])
    ];
    memset(frontier_state, 0, sizeof(frontier_state));
    for (size_t i = 0; i < sizeof(k_logs) / sizeof(k_logs[0]); i++) {
        int64_t raw_cursor = 0;
        bool cursor_ok = cursor_at(db, k_logs[i].cursor_name, &raw_cursor);
        int64_t next_cursor = cursor_ok
            ? next_cursor_for_dump(&k_logs[i], raw_cursor)
            : -1;
        int32_t frontier = -1;
        bool frontier_ok = reducer_frontier_log_frontier(
            db, k_logs[i].log_table, k_logs[i].cursor_name, &frontier);
        frontier_state[i].cursor_ok = cursor_ok;
        frontier_state[i].frontier_ok = frontier_ok;
        frontier_state[i].raw_cursor = raw_cursor;
        frontier_state[i].next_cursor = next_cursor;
        frontier_state[i].contiguous_frontier = frontier;
        if (strcmp(k_logs[i].log_table, "validate_headers_log") == 0) {
            validate_frontier = frontier_ok ? frontier : hstar;
            validate_cursor_next = (next_cursor > INT32_MAX)
                ? INT32_MAX
                : (int32_t)next_cursor;
        }

        struct json_value obj = {0};
        json_set_object(&obj);
        json_push_kv_str(&obj, "stage", k_logs[i].stage);
        json_push_kv_str(&obj, "log_table", k_logs[i].log_table);
        json_push_kv_int(&obj, "cursor", cursor_ok ? raw_cursor : -1);
        json_push_kv_int(&obj, "next_cursor", next_cursor);
        json_push_kv_bool(&obj, "served_tip_cursor",
                          k_logs[i].served_tip_cursor);
        json_push_kv_bool(&obj, "frontier_read_ok", frontier_ok);
        json_push_kv_int(&obj, "contiguous_frontier",
                         frontier_ok ? frontier : -1);
        json_push_back(&frontiers, &obj);
        json_free(&obj);
    }
    json_push_kv(out, "success_checked_frontiers", &frontiers);
    json_free(&frontiers);

    struct hstar_blocker blocker;
    if (!first_hstar_blocker(db, hstar, frontier_state,
                             sizeof(frontier_state) /
                             sizeof(frontier_state[0]),
                             &blocker)) {
        progress_store_tx_unlock();
        return false;
    }
    int64_t hstar_next = (hstar < INT32_MAX) ? (int64_t)hstar + 1 : -1;
    json_push_kv_int(out, "hstar_next_height", hstar_next);
    json_push_kv_bool(out, "hstar_next_blocked", blocker.found);
    json_push_kv_str(out, "hstar_next_primary_kind",
                     blocker.found ? blocker.kind : "none");
    json_push_kv_str(out, "hstar_next_primary_stage",
                     blocker.found ? blocker.stage : "");
    json_push_kv_str(out, "hstar_next_primary_log_table",
                     blocker.found ? blocker.log_table : "");
    json_push_kv_str(out, "hstar_next_primary_detail",
                     blocker.found ? blocker.reason : "");
    json_push_kv_str(out, "hstar_next_primary_repair_owner",
                     blocker.found
                         ? diagnostic_repair_hint(blocker.kind,
                                                  blocker.reason)
                         : "");
    json_push_kv_bool(out, "hstar_next_pending_edge",
                      blocker.pending_edge);
    json_push_kv_str(out, "hstar_next_pending_stage",
                     blocker.pending_edge ? blocker.pending_stage : "");
    json_push_kv_str(out, "hstar_next_pending_log_table",
                     blocker.pending_edge ? blocker.pending_log_table : "");
    json_push_kv_str(out, "hstar_next_pending_detail",
                     blocker.pending_edge ? blocker.pending_reason : "");
    json_push_kv_int(out, "hstar_next_blocker_count",
                     blocker.found ? 1 : 0);
    json_push_kv_bool(out, "first_hstar_blocker_found", blocker.found);
    json_push_kv_str(out, "first_hstar_blocker_stage",
                     blocker.found ? blocker.stage : "");
    json_push_kv_str(out, "first_hstar_blocker_log_table",
                     blocker.found ? blocker.log_table : "");
    json_push_kv_int(out, "first_hstar_blocker_height",
                     blocker.found ? blocker.height : -1);
    json_push_kv_str(out, "first_hstar_blocker_kind",
                     blocker.found ? blocker.kind : "");
    json_push_kv_str(out, "first_hstar_blocker_reason",
                     blocker.found ? blocker.reason : "");
    json_push_kv_str(out, "first_hstar_blocker_repair_owner",
                     blocker.found
                         ? diagnostic_repair_hint(blocker.kind,
                                                  blocker.reason)
                         : "");

    bool fail_found = false;
    int32_t fail_height = -1;
    char fail_reason[128] = "";
    int32_t max_validate = validate_cursor_next > 0
        ? validate_cursor_next - 1
        : INT32_MAX;
    if (!first_validate_failure(db, validate_frontier, max_validate,
                                &fail_found, &fail_height,
                                fail_reason, sizeof(fail_reason))) {
        progress_store_tx_unlock();
        return false;
    }
    json_push_kv_bool(out, "first_validate_failure_found", fail_found);
    json_push_kv_int(out, "first_validate_failure_height",
                     fail_found ? fail_height : -1);
    json_push_kv_str(out, "first_validate_failure_reason",
                     fail_found ? fail_reason : "");
    json_push_kv_str(out, "first_validate_failure_repair_owner",
                     fail_found
                         ? diagnostic_repair_hint("ok0_failure", fail_reason)
                         : "");

    /* S5: active_chain_extend_window_have_data fast (best-header ancestry,
     * O(log n)) vs slow (full-map scan + pprev-walk, O(map)) path hit
     * counts. A live climb on window_extend_slow is a silent regression
     * back to the fixed ~9s/block full-map-scan pathology — see
     * chainstate.h / chainstate.c. */
    json_push_kv_int(out, "window_extend_fast",
                     (int64_t)active_chain_extend_window_have_data_fast_count());
    json_push_kv_int(out, "window_extend_slow",
                     (int64_t)active_chain_extend_window_have_data_slow_count());

    progress_store_tx_unlock();
    return true;
}
