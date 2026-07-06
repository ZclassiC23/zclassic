/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/agent_controller.h"

#include "chain/checkpoints.h"
#include "controllers/strong_params.h"
#include "json/json.h"
#include "util/clientversion.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define ANCHOR_MARKER_LEN 48

struct anchor_stage_view {
    const char *name;
    const char *log_table;
    int64_t cursor;
    int64_t log_count;
    int64_t min_height;
    int64_t max_height;
    bool cursor_present;
    bool log_present;
};

static uint32_t ale32_read(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ale64_read(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

static bool anchor_stat_file(const char *path, int64_t *size_out,
                             int64_t *mtime_out)
{
    if (size_out)
        *size_out = 0;
    if (mtime_out)
        *mtime_out = 0;
    if (!path || !path[0])
        return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    if (size_out)
        *size_out = (int64_t)st.st_size;
    if (mtime_out)
        *mtime_out = (int64_t)st.st_mtime;
    return true;
}

static void anchor_path_join(char *out, size_t cap,
                             const char *datadir, const char *name)
{
    if (!out || cap == 0)
        return;
    snprintf(out, cap, "%s/%s", datadir && datadir[0] ? datadir : ".", name);
}

static bool anchor_sql_i64(sqlite3 *db, const char *sql, int64_t bind0,
                           bool bind, int64_t *out, bool *found_out)
{
    if (out)
        *out = 0;
    if (found_out)
        *found_out = false;
    if (!db || !sql)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) // raw-controller-sql-ok:anchorstatus-progress-kv-readonly
        return false;
    if (bind)
        sqlite3_bind_int64(st, 1, (sqlite3_int64)bind0);
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-diagnostic-cli
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        if (out)
            *out = sqlite3_column_int64(st, 0);
        if (found_out)
            *found_out = true;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static bool anchor_meta_blob(sqlite3 *db, const char *key,
                             uint8_t *buf, size_t cap, size_t *len_out,
                             bool *found_out)
{
    if (len_out)
        *len_out = 0;
    if (found_out)
        *found_out = false;
    if (!db || !key)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, // raw-controller-sql-ok:anchorstatus-progress-kv-readonly
            "SELECT value FROM progress_meta WHERE key=?1",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-diagnostic-cli
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (found_out)
            *found_out = true;
        if (len_out)
            *len_out = n > 0 ? (size_t)n : 0;
        if (blob && n > 0 && buf && cap > 0) {
            size_t copy = (size_t)n < cap ? (size_t)n : cap;
            memcpy(buf, blob, copy);
        }
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static bool anchor_stage_cursor(sqlite3 *db, const char *name,
                                int64_t *cursor_out, bool *present_out)
{
    if (cursor_out)
        *cursor_out = 0;
    if (present_out)
        *present_out = false;
    if (!db || !name)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, // raw-controller-sql-ok:anchorstatus-progress-kv-readonly
            "SELECT cursor FROM stage_cursor WHERE name=?1",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-diagnostic-cli
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        if (cursor_out)
            *cursor_out = sqlite3_column_int64(st, 0);
        if (present_out)
            *present_out = true;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static bool anchor_table_stats(sqlite3 *db, const char *table,
                               int64_t *count_out, int64_t *min_out,
                               int64_t *max_out, bool *present_out)
{
    if (count_out)
        *count_out = 0;
    if (min_out)
        *min_out = -1;
    if (max_out)
        *max_out = -1;
    if (present_out)
        *present_out = false;
    if (!db || !table)
        return false;

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT count(*), min(height), max(height) FROM %s", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) // raw-controller-sql-ok:anchorstatus-progress-kv-readonly
        return true; /* older fixture without this log table */
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-diagnostic-cli
    if (rc == SQLITE_ROW) {
        if (present_out)
            *present_out = true;
        if (count_out)
            *count_out = sqlite3_column_int64(st, 0);
        if (min_out && sqlite3_column_type(st, 1) != SQLITE_NULL)
            *min_out = sqlite3_column_int64(st, 1);
        if (max_out && sqlite3_column_type(st, 2) != SQLITE_NULL)
            *max_out = sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static bool anchor_marker_matches(const uint8_t *marker, size_t len,
                                  const struct sha3_utxo_checkpoint *cp)
{
    if (!marker || len != ANCHOR_MARKER_LEN || !cp)
        return false;
    if (memcmp(marker, "ZAM1", 4) != 0)
        return false;
    if ((int32_t)ale32_read(marker + 4) != cp->height)
        return false;
    if (ale64_read(marker + 8) != cp->utxo_count)
        return false;
    return memcmp(marker + 16, cp->sha3_hash, 32) == 0;
}

static void anchor_push_stage_json(struct json_value *arr,
                                   const struct anchor_stage_view *s)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "name", s->name);
    json_push_kv_int(&obj, "cursor", s->cursor);
    json_push_kv_bool(&obj, "cursor_present", s->cursor_present);
    json_push_kv_str(&obj, "log_table", s->log_table);
    json_push_kv_bool(&obj, "log_present", s->log_present);
    json_push_kv_int(&obj, "log_count", s->log_count);
    json_push_kv_int(&obj, "log_min_height", s->min_height);
    json_push_kv_int(&obj, "log_max_height", s->max_height);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static const char *anchor_summary(bool progress_present, bool snapshot_present,
                                  bool marker_present, bool marker_matches,
                                  int64_t durable_frontier,
                                  int64_t anchor_height,
                                  int64_t validated_backlog,
                                  int64_t stale_above_anchor)
{
    if (snapshot_present)
        return "anchor_snapshot_present";
    if (!progress_present)
        return "progress_store_missing";
    if (!marker_present)
        return "mint_not_marked_in_progress";
    if (!marker_matches)
        return "mint_marker_checkpoint_mismatch";
    if (durable_frontier >= anchor_height)
        return "anchor_reached_snapshot_pending";
    if (validated_backlog > 100000)
        return "mint_utxo_apply_far_behind_validated_backlog";
    if (stale_above_anchor > 0)
        return "mint_has_stale_rows_above_anchor";
    return "mint_in_progress";
}

static const char *anchor_next_action(const char *summary)
{
    if (!summary)
        return "inspect_anchorstatus";
    if (strcmp(summary, "anchor_snapshot_present") == 0)
        return "copy_prove_refold_from_anchor_then_cutover";
    if (strcmp(summary, "progress_store_missing") == 0)
        return "start_anchor_mint_on_full_history_datadir";
    if (strcmp(summary, "mint_not_marked_in_progress") == 0)
        return "restart_anchor_mint_with_checkpoint_resume_marker";
    if (strcmp(summary, "mint_marker_checkpoint_mismatch") == 0)
        return "stop_and_reseed_anchor_mint_on_a_copy_before_resume";
    if (strcmp(summary, "anchor_reached_snapshot_pending") == 0)
        return "check_snapshot_write_and_sha3_assert_logs";
    if (strcmp(summary, "mint_utxo_apply_far_behind_validated_backlog") == 0)
        return "inspect_utxo_apply_idle_reason_before_waiting_more";
    if (strcmp(summary, "mint_has_stale_rows_above_anchor") == 0)
        return "copy_probe_progress_store_for_stale_rows_before_trusting_resume";
    return "observe_progress_or_drill_into_stage_cursors";
}

bool rpc_agent_anchor_status(const struct json_value *params, bool help,
                             struct json_value *result)
{
    RPC_HELP(help, result,
        "anchorstatus [datadir]\n"
        "\nRead a mint-anchor datadir's progress.kv directly and return the\n"
        "sovereign-anchor producer state. This is a no-cookie static command\n"
        "for offline/transient anchor mint services.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    const char *ctx_datadir = agent_runtime_context_datadir();
    const char *datadir = rpc_permit_str(&p, 0, "datadir",
                                         ctx_datadir && ctx_datadir[0]
                                             ? ctx_datadir
                                             : ".");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }

    char progress_path[1100];
    char snapshot_path[1100];
    anchor_path_join(progress_path, sizeof(progress_path), datadir,
                     "progress.kv");
    anchor_path_join(snapshot_path, sizeof(snapshot_path), datadir,
                     "utxo-anchor.snapshot");

    int64_t progress_size = 0, progress_mtime = 0;
    int64_t snapshot_size = 0, snapshot_mtime = 0;
    bool progress_present = anchor_stat_file(progress_path, &progress_size,
                                             &progress_mtime);
    bool snapshot_present = anchor_stat_file(snapshot_path, &snapshot_size,
                                             &snapshot_mtime);

    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.anchor_mint_status.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_str(result, "datadir", datadir);
    json_push_kv_str(result, "progress_path", progress_path);
    json_push_kv_bool(result, "progress_present", progress_present);
    json_push_kv_int(result, "progress_size_bytes", progress_size);
    json_push_kv_int(result, "progress_mtime_unix", progress_mtime);
    json_push_kv_str(result, "snapshot_path", snapshot_path);
    json_push_kv_bool(result, "snapshot_present", snapshot_present);
    json_push_kv_int(result, "snapshot_size_bytes", snapshot_size);
    json_push_kv_int(result, "snapshot_mtime_unix", snapshot_mtime);

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    int64_t anchor_height = cp ? cp->height : -1;
    json_push_kv_int(result, "anchor_height", anchor_height);
    json_push_kv_int(result, "anchor_utxo_count",
                     cp ? (int64_t)cp->utxo_count : 0);

    if (!progress_present) {
        json_push_kv_str(result, "status", "missing");
        const char *summary = anchor_summary(false, snapshot_present, false,
                                             false, -1, anchor_height, 0, 0);
        json_push_kv_str(result, "summary", summary);
        json_push_kv_str(result, "agent_next_action",
                         anchor_next_action(summary));
        return true;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(progress_path, &db, SQLITE_OPEN_READONLY, NULL) !=
        SQLITE_OK) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "summary", "progress_store_open_failed");
        json_push_kv_str(result, "agent_next_action",
                         "inspect_progress_store_permissions_or_locking");
        json_push_kv_str(result, "sqlite_error", db ? sqlite3_errmsg(db)
                                                     : "out of memory");
        sqlite3_close(db);
        return true;
    }
    sqlite3_busy_timeout(db, 2000);
    json_push_kv_str(result, "status", "ok");

    struct anchor_stage_view stages[] = {
        { "header_admit",     "header_admit_log",     0, 0, -1, -1, false, false },
        { "validate_headers", "validate_headers_log", 0, 0, -1, -1, false, false },
        { "body_fetch",       "body_fetch_log",       0, 0, -1, -1, false, false },
        { "body_persist",     "body_persist_log",     0, 0, -1, -1, false, false },
        { "script_validate",  "script_validate_log",  0, 0, -1, -1, false, false },
        { "proof_validate",   "proof_validate_log",   0, 0, -1, -1, false, false },
        { "utxo_apply",       "utxo_apply_log",       0, 0, -1, -1, false, false },
        { "tip_finalize",     "tip_finalize_log",     0, 0, -1, -1, false, false },
    };

    int64_t min_cursor = -1, max_cursor = -1;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        (void)anchor_stage_cursor(db, stages[i].name, &stages[i].cursor,
                                  &stages[i].cursor_present);
        (void)anchor_table_stats(db, stages[i].log_table,
                                 &stages[i].log_count,
                                 &stages[i].min_height,
                                 &stages[i].max_height,
                                 &stages[i].log_present);
        if (stages[i].cursor_present) {
            if (min_cursor < 0 || stages[i].cursor < min_cursor)
                min_cursor = stages[i].cursor;
            if (max_cursor < 0 || stages[i].cursor > max_cursor)
                max_cursor = stages[i].cursor;
        }
    }

    uint8_t blob[ANCHOR_MARKER_LEN] = {0};
    size_t blob_len = 0;
    bool marker_present = false;
    (void)anchor_meta_blob(db, "mint_anchor_in_progress_v1",
                           blob, sizeof(blob), &blob_len, &marker_present);
    bool marker_matches = anchor_marker_matches(blob, blob_len, cp);

    uint8_t height_blob[8] = {0};
    size_t height_len = 0;
    bool coins_height_present = false;
    (void)anchor_meta_blob(db, "coins_applied_height",
                           height_blob, sizeof(height_blob), &height_len,
                           &coins_height_present);
    int64_t coins_applied_height =
        (coins_height_present && height_len == 8)
            ? (int64_t)ale64_read(height_blob) : -1;
    int64_t durable_frontier = coins_applied_height >= 0
        ? coins_applied_height - 1 : -1;

    uint8_t flush_blob[8] = {0};
    size_t flush_len = 0;
    bool flush_present = false;
    (void)anchor_meta_blob(db, "coins_ram_flushed_height",
                           flush_blob, sizeof(flush_blob), &flush_len,
                           &flush_present);
    int64_t coins_ram_flushed_height =
        (flush_present && flush_len == 8) ? (int64_t)ale64_read(flush_blob) : -1;

    int64_t refold_flag = 0;
    bool refold_present = false;
    (void)anchor_sql_i64(db,
        "SELECT length(value) FROM progress_meta WHERE key='refold_in_progress'",
        0, false, &refold_flag, &refold_present);

    int64_t stale_above_anchor = 0;
    bool stale_found = false;
    (void)anchor_sql_i64(db,
        "SELECT count(*) FROM header_admit_log WHERE height>?1",
        anchor_height, true, &stale_above_anchor, &stale_found);

    sqlite3_close(db);

    int64_t proof_cursor = stages[5].cursor;
    int64_t utxo_cursor = stages[6].cursor;
    int64_t tip_cursor = stages[7].cursor;
    int64_t validated_backlog = stages[5].cursor_present &&
        stages[6].cursor_present && proof_cursor > utxo_cursor
            ? proof_cursor - utxo_cursor : 0;
    int64_t tip_finalize_gap = stages[6].cursor_present &&
        stages[7].cursor_present && utxo_cursor > tip_cursor
            ? utxo_cursor - tip_cursor : 0;
    int64_t anchor_gap = anchor_height >= 0 && durable_frontier >= 0
        ? anchor_height - durable_frontier : -1;
    if (anchor_gap < 0 && durable_frontier >= anchor_height)
        anchor_gap = 0;

    struct json_value stage_arr;
    json_init(&stage_arr);
    json_set_array(&stage_arr);
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
        anchor_push_stage_json(&stage_arr, &stages[i]);
    json_push_kv(result, "stages", &stage_arr);
    json_free(&stage_arr);

    json_push_kv_bool(result, "mint_marker_present", marker_present);
    json_push_kv_bool(result, "mint_marker_matches_checkpoint",
                      marker_matches);
    json_push_kv_bool(result, "refold_in_progress_present",
                      refold_present);
    json_push_kv_int(result, "coins_applied_height",
                     coins_applied_height);
    json_push_kv_int(result, "durable_applied_through_height",
                     durable_frontier);
    json_push_kv_int(result, "coins_ram_flushed_height",
                     coins_ram_flushed_height);
    json_push_kv_int(result, "anchor_gap_blocks", anchor_gap);
    json_push_kv_int(result, "min_stage_cursor", min_cursor);
    json_push_kv_int(result, "max_stage_cursor", max_cursor);
    json_push_kv_int(result, "validated_backlog_blocks",
                     validated_backlog);
    json_push_kv_int(result, "tip_finalize_gap_blocks",
                     tip_finalize_gap);
    json_push_kv_int(result, "stale_header_rows_above_anchor",
                     stale_above_anchor);
    json_push_kv_bool(result, "stale_rows_above_anchor",
                      stale_above_anchor > 0);
    json_push_kv_str(result, "semantics",
                     "durable_applied_through_height is the persisted coins_kv/flush frontier; in-RAM overlay work is only trusted after a flush or final SHA3 snapshot assert");

    const char *summary = anchor_summary(progress_present, snapshot_present,
                                         marker_present, marker_matches,
                                         durable_frontier, anchor_height,
                                         validated_backlog,
                                         stale_above_anchor);
    json_push_kv_str(result, "summary", summary);
    json_push_kv_str(result, "agent_next_action",
                     anchor_next_action(summary));
    return true;
}
