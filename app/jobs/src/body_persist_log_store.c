/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_persist_log_store — implementation. See body_persist_log_store.h. */

#include "body_persist_log_store.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool body_persist_log_ensure_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "  height       INTEGER PRIMARY KEY,"
        "  source       TEXT    NOT NULL,"
        "  ok           INTEGER NOT NULL,"
        "  persisted_at INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("body_persist", "[body_persist] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

int body_persist_body_fetch_log_at(sqlite3 *db, int height,
                                   struct body_fetch_row *out)
{
    memset(out, 0, sizeof(*out));
    out->ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT source, ok FROM body_fetch_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("body_persist", "[body_persist] body_fetch_log prepare failed: %s", sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const unsigned char *src = sqlite3_column_text(st, 0);
        if (src)
            snprintf(out->source, sizeof(out->source), "%s",
                     (const char *)src);
        out->ok = sqlite3_column_int(st, 1);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

bool body_persist_log_insert(sqlite3 *db, int height, const char *source, bool ok)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO body_persist_log "
        "(height, source, ok, persisted_at) VALUES (?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("body_persist", "[body_persist] prepare insert failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_text (stmt, 2, source, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)platform_time_wall_unix());
    rc = sqlite3_step(stmt);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("body_persist", "[body_persist] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}
