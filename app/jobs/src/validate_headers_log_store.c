/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_log_store — implementation. See validate_headers_log_store.h. */

#include "validate_headers_log_store.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>

bool validate_headers_log_ensure_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS validate_headers_log ("
        "  height       INTEGER PRIMARY KEY,"
        "  hash         BLOB    NOT NULL,"
        "  ok           INTEGER NOT NULL,"
        "  fail_reason  TEXT,"
        "  validated_at INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("validate_headers", "[validate_headers] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool validate_headers_log_insert(sqlite3 *db, int height,
                                 const struct uint256 *hash, bool ok,
                                 const char *reason)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO validate_headers_log "
        "(height, hash, ok, fail_reason, validated_at) VALUES (?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("validate_headers", "[validate_headers] prepare insert failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_blob (stmt, 2, hash->data, 32, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    if (!ok && reason && reason[0])
        sqlite3_bind_text(stmt, 4, reason, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)platform_time_wall_unix());

    rc = sqlite3_step(stmt);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("validate_headers", "[validate_headers] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}
