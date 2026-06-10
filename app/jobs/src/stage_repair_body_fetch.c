/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_body_fetch — body-fetch candidacy detection. When the
 * validate_headers cursor has advanced past the body_fetch cursor but the
 * frontier header validated ok with no body_fetch_log row, a body is missing
 * its HAVE_DATA observation; the body_fetch_missing_have_data Condition uses
 * these read-only predicates to find that gap. This TU also owns the two small
 * progress.kv read helpers (validate-log row, stage cursor) shared with the
 * rewind and clamp TUs via stage_repair_internal.h. Read-only — no rewinds. */

#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"

#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

bool stage_repair_read_validate_row(sqlite3 *db, int height,
                                    struct validate_row *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok, COALESCE(fail_reason,'') "
            "FROM validate_headers_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate row prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        out->found = true;
        out->ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt)
            snprintf(out->fail_reason, sizeof(out->fail_reason),
                     "%s", (const char *)txt);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] validate row step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

bool stage_repair_cursor_at_unlocked(sqlite3 *db, const char *name, int *out)
{
    *out = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] cursor read prepare failed stage=%s: %s",
                 name, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] cursor read step failed stage=%s rc=%d: %s",
                 name, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool body_fetch_row_observed_unlocked(sqlite3 *db, int height,
                                             bool *found, bool *observed)
{
    *found = false;
    *observed = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM body_fetch_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch observed prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found = true;
        *observed = sqlite3_column_int(st, 0) == 1;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] body_fetch observed step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

bool stage_repair_body_fetch_observed(sqlite3 *db, int height)
{
    if (!db || height < 0)
        return false;
    progress_store_tx_lock();
    bool found = false;
    bool observed = false;
    bool ok = body_fetch_row_observed_unlocked(db, height, &found,
                                               &observed);
    progress_store_tx_unlock();
    return ok && found && observed;
}

bool stage_repair_body_fetch_missing_have_data_candidate(
    sqlite3 *db,
    int height,
    struct stage_repair_body_fetch_gap *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db || height < 0)
        return false;

    progress_store_tx_lock();
    int validate_cursor = -1;
    int body_fetch_cursor = -1;
    bool ok = stage_repair_cursor_at_unlocked(db, "validate_headers",
                                              &validate_cursor) &&
              stage_repair_cursor_at_unlocked(db, "body_fetch",
                                              &body_fetch_cursor);
    if (!ok) {
        progress_store_tx_unlock();
        return false;
    }

    struct validate_row vh;
    if (!stage_repair_read_validate_row(db, height, &vh)) {
        progress_store_tx_unlock();
        return false;
    }

    bool body_row_found = false;
    bool body_observed = false;
    if (!body_fetch_row_observed_unlocked(db, height, &body_row_found,
                                          &body_observed)) {
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();

    bool ready = body_fetch_cursor == height &&
                 validate_cursor > height &&
                 vh.found && vh.ok == 1 &&
                 !body_row_found;
    if (out) {
        out->ready = ready;
        out->body_observed = body_row_found && body_observed;
        out->target_height = height;
        out->validate_cursor = validate_cursor;
        out->body_fetch_cursor = body_fetch_cursor;
    }
    return ready;
}

bool stage_repair_body_fetch_missing_have_data_frontier_candidate(
    sqlite3 *db,
    struct stage_repair_body_fetch_gap *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db)
        return false;

    progress_store_tx_lock();
    int body_fetch_cursor = -1;
    bool ok = stage_repair_cursor_at_unlocked(db, "body_fetch",
                                              &body_fetch_cursor);
    progress_store_tx_unlock();
    if (!ok || body_fetch_cursor < 0)
        return false;

    return stage_repair_body_fetch_missing_have_data_candidate(
        db, body_fetch_cursor, out);
}
