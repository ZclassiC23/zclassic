/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * script_validate_log_store — implementation. See script_validate_log_store.h. */

#include "script_validate_log_store.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Idempotent ADD COLUMN: tolerate "duplicate column name" (already migrated),
 * fail loud on any other error. */
static bool add_column_if_missing(sqlite3 *db, const char *alter_sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, alter_sql, NULL, NULL, &err) == SQLITE_OK)
        return true;
    bool dup = err && strstr(err, "duplicate column name") != NULL;
    if (!dup)
        LOG_WARN("script_validate",
                 "[script_validate] schema alter failed: %s",
                 err ? err : "(no message)");
    if (err) sqlite3_free(err);
    return dup;
}

bool script_validate_log_ensure_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height             INTEGER PRIMARY KEY,"
        "  status             TEXT    NOT NULL,"
        "  ok                 INTEGER NOT NULL,"
        "  tx_count           INTEGER NOT NULL,"
        "  input_count        INTEGER NOT NULL,"
        "  first_failure_txid BLOB,"
        "  first_failure_vin  INTEGER,"
        "  first_failure_serror INTEGER,"
        "  validated_at       INTEGER NOT NULL,"
        "  block_hash         BLOB"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("script_validate", "[script_validate] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    /* Idempotent migrations. block_hash binds an ok=1 row to its block so
     * tip_finalize can reject a stale height-keyed orphan row (different hash). */
    if (!add_column_if_missing(db,
            "ALTER TABLE script_validate_log ADD COLUMN first_failure_serror INTEGER"))
        return false;
    if (!add_column_if_missing(db,
            "ALTER TABLE script_validate_log ADD COLUMN block_hash BLOB"))
        return false;
    return true;
}

int script_validate_body_persist_log_at(sqlite3 *db, int height,
                                        struct body_persist_row *out)
{
    memset(out, 0, sizeof(*out));
    out->ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT source, ok FROM body_persist_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("script_validate", "[script_validate] body_persist_log prepare failed: %s", sqlite3_errmsg(db));
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

int script_validate_log_verdict_at(sqlite3 *db, int height,
                                   struct script_validate_verdict_row *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, block_hash FROM script_validate_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("script_validate",
                 "[script_validate] verdict_at prepare failed: %s",
                 sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    if (sqlite3_bind_int(st, 1, height) != SQLITE_OK) {
        LOG_WARN("script_validate",
                 "[script_validate] verdict_at bind failed height=%d", height);
        sqlite3_finalize(st);
        return -1;  // raw-return-ok:logged-above
    }
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        out->ok = sqlite3_column_int(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        if (blob && sqlite3_column_bytes(st, 1) == 32) {
            memcpy(out->block_hash.data, blob, 32);
            out->has_block_hash = true;
        }
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

bool script_validate_log_insert(sqlite3 *db, int height,
                                const char *status, bool ok,
                                size_t tx_count, size_t input_count,
                                const struct uint256 *first_failure_txid,
                                int first_failure_vin,
                                ScriptError first_failure_serror,
                                const struct uint256 *block_hash)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO script_validate_log "
        "(height, status, ok, tx_count, input_count, first_failure_txid, "
        " first_failure_vin, first_failure_serror, validated_at, block_hash) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("script_validate", "[script_validate] prepare insert failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_text (stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)tx_count);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)input_count);
    if (first_failure_txid)
        sqlite3_bind_blob(stmt, 6, first_failure_txid->data, 32,
                          SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);
    if (first_failure_vin >= 0)
        sqlite3_bind_int(stmt, 7, first_failure_vin);
    else
        sqlite3_bind_null(stmt, 7);
    /* SCRIPT_ERR_OK persists as NULL so the column is never misread. */
    if (first_failure_serror != SCRIPT_ERR_OK)
        sqlite3_bind_int(stmt, 8, (int)first_failure_serror);
    else
        sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int64(stmt, 9, (sqlite3_int64)platform_time_wall_unix());
    /* Bind the block's own hash so tip_finalize can prove an ok=1 row is THIS
     * block's verdict (reorg-safe); NULL before the candidate is resolved. */
    if (block_hash)
        sqlite3_bind_blob(stmt, 10, block_hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 10);
    rc = sqlite3_step(stmt);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("script_validate", "[script_validate] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}
