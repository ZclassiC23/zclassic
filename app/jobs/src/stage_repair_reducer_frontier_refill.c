/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_refill — refill forward-only reducer stages
 * after a hole is discovered below their cursor.
 */

#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"
#include "stage_repair_reducer_frontier_internal.h"

#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stddef.h>

static bool find_lowest_validate_headers_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    if (out_height)
        *out_height = -1;
    if (!db || !out_height)
        return false;
    if (start_height > end_height)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT h.height "
            "FROM header_admit_log h "
            "LEFT JOIN validate_headers_log v ON v.height = h.height "
            "WHERE h.height >= ? AND h.height <= ? "
            "AND v.height IS NULL "
            "ORDER BY h.height ASC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate_headers refill scan prepare "
                 "failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(st, 1, start_height);
    sqlite3_bind_int(st, 2, end_height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate_headers refill scan failed "
                 "rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool find_lowest_validate_headers_hash_split_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    if (out_height)
        *out_height = -1;
    if (!db || !out_height)
        return false;
    if (start_height > end_height)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT v.height "
            "FROM validate_headers_log v "
            "JOIN script_validate_log s ON s.height = v.height "
            "WHERE v.height >= ? AND v.height <= ? "
            "AND v.ok = 1 AND s.ok = 1 "
            "AND length(v.hash) = 32 AND length(s.block_hash) = 32 "
            "AND v.hash <> s.block_hash "
            "ORDER BY v.height ASC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate_headers hash-split scan "
                 "prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(st, 1, start_height);
    sqlite3_bind_int(st, 2, end_height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate_headers hash-split scan "
                 "failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool find_lowest_body_fetch_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    if (out_height)
        *out_height = -1;
    if (!db || !out_height)
        return false;
    if (start_height > end_height)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT v.height "
            "FROM validate_headers_log v "
            "LEFT JOIN body_fetch_log b ON b.height = v.height "
            "WHERE v.height >= ? AND v.height <= ? "
            "AND v.ok = 1 AND b.height IS NULL "
            "ORDER BY v.height ASC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch refill scan prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(st, 1, start_height);
    sqlite3_bind_int(st, 2, end_height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch refill scan failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool find_lowest_body_persist_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    if (out_height)
        *out_height = -1;
    if (!db || !out_height)
        return false;
    if (start_height > end_height)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT b.height "
            "FROM body_fetch_log b "
            "LEFT JOIN body_persist_log p ON p.height = b.height "
            "WHERE b.height >= ? AND b.height <= ? "
            "AND b.ok = 1 AND p.height IS NULL "
            "ORDER BY b.height ASC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_persist refill scan prepare "
                 "failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(st, 1, start_height);
    sqlite3_bind_int(st, 2, end_height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_persist refill scan failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool body_persist_log_present_unlocked(sqlite3 *db,
                                              int height,
                                              bool *present)
{
    if (present)
        *present = false;
    if (!db || !present)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM body_persist_log WHERE height = ? LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_persist presence prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *present = true;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_persist presence failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool reconcile_validate_headers_refill_holes(
    sqlite3 *db,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->validate_headers_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->validate_headers_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    if (!find_lowest_validate_headers_refill_hole_unlocked(
            db, out->hstar + 1, end_height,
            &out->lowest_validate_headers_refill_hole))
        return false;
    return find_lowest_validate_headers_hash_split_unlocked(
        db, out->hstar + 1, end_height,
        &out->lowest_validate_headers_hash_split);
}

static bool reconcile_body_fetch_refill_holes(
    sqlite3 *db,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->body_fetch_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->body_fetch_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    return find_lowest_body_fetch_refill_hole_unlocked(
        db, out->hstar + 1, end_height,
        &out->lowest_body_fetch_refill_hole);
}

static bool reconcile_body_persist_refill_holes(
    sqlite3 *db,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->body_persist_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->body_persist_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    return find_lowest_body_persist_refill_hole_unlocked(
        db, out->hstar + 1, end_height,
        &out->lowest_body_persist_refill_hole);
}

bool stage_reducer_frontier_force_stage_cursor_in_tx(
    sqlite3 *db,
    const char *stage_name,
    const char *label,
    int target)
{
    progress_store_tx_lock();

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier %s BEGIN failed: %s",
                 label, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    if (!stage_repair_force_stage_cursor(db, stage_name, target)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier %s COMMIT failed: %s",
                 label, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    progress_store_tx_unlock();
    return true;
}

bool stage_reducer_frontier_reconcile_validate_hash_split_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->validate_headers_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->validate_headers_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    if (!find_lowest_validate_headers_hash_split_unlocked(
            db, out->hstar + 1, end_height,
            &out->lowest_validate_headers_hash_split))
        return false;
    if (out->lowest_validate_headers_hash_split < 0)
        return true;

    if (out->validate_headers_cursor_before <=
        out->lowest_validate_headers_hash_split) {
        out->validate_headers_cursor_after =
            out->validate_headers_cursor_before;
        return true;
    }

    out->clamped_validate_headers = true;
    out->validate_headers_cursor_after =
        out->lowest_validate_headers_hash_split;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "validate_headers", "validate_headers-hash-split",
        out->lowest_validate_headers_hash_split);
}

static int lower_refill_target(int target, int candidate)
{
    if (candidate >= 0 && (target < 0 || candidate < target))
        return candidate;
    return target;
}

static bool reconcile_validate_headers_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_validate_headers_refill_hole;
    target = lower_refill_target(target,
                                 out->lowest_validate_headers_hash_split);
    if (target < 0) {
        out->validate_headers_cursor_after =
            out->validate_headers_cursor_before;
        return true;
    }

    if (out->validate_headers_cursor_before <= target) {
        out->validate_headers_cursor_after =
            out->validate_headers_cursor_before;
        return true;
    }

    out->clamped_validate_headers = true;
    out->validate_headers_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "validate_headers", "validate_headers", target);
}

static bool reconcile_body_fetch_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_have_data_cleared;
    target = lower_refill_target(target,
                                 out->lowest_validate_headers_refill_hole);
    target = lower_refill_target(target, out->lowest_body_fetch_refill_hole);
    if (target < 0) {
        out->body_fetch_cursor_after = out->body_fetch_cursor_before;
        return true;
    }

    if (out->body_fetch_cursor_before <= target) {
        out->body_fetch_cursor_after = out->body_fetch_cursor_before;
        return true;
    }

    out->clamped_body_fetch = true;
    out->body_fetch_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "body_fetch", "body_fetch", target);
}

static bool reconcile_body_persist_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_body_persist_refill_hole;
    int upstream_targets[] = {
        out->lowest_validate_headers_refill_hole,
        out->lowest_body_fetch_refill_hole,
    };
    for (size_t i = 0;
         i < sizeof(upstream_targets) / sizeof(upstream_targets[0]);
         i++) {
        int candidate = upstream_targets[i];
        if (candidate < 0)
            continue;
        bool present = false;
        if (!body_persist_log_present_unlocked(db, candidate, &present))
            return false;
        if (!present)
            target = lower_refill_target(target, candidate);
    }
    if (target < 0) {
        out->body_persist_cursor_after = out->body_persist_cursor_before;
        return true;
    }

    if (out->body_persist_cursor_before <= target) {
        out->body_persist_cursor_after = out->body_persist_cursor_before;
        return true;
    }

    out->clamped_body_persist = true;
    out->body_persist_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "body_persist", "body_persist", target);
}

bool stage_reducer_frontier_reconcile_refill_cursors(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!reconcile_validate_headers_refill_holes(db, out))
        return false;

    if (!reconcile_body_fetch_refill_holes(db, out))
        return false;

    if (!reconcile_body_persist_refill_holes(db, out))
        return false;

    if (!reconcile_validate_headers_cursor(db, apply, out))
        return false;

    if (!reconcile_body_fetch_cursor(db, apply, out))
        return false;

    if (!reconcile_body_persist_cursor(db, apply, out))
        return false;

    return true;
}
