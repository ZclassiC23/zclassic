/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * proof_validate_log_store — implementation. See proof_validate_log_store.h. */

#include "proof_validate_log_store.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

static bool add_block_hash_if_missing(sqlite3 *db)
{
    char *error = NULL;
    if (sqlite3_exec(db,
            "ALTER TABLE proof_validate_log ADD COLUMN block_hash BLOB",
            NULL, NULL, &error) == SQLITE_OK)
        return true;
    bool duplicate = error &&
        strstr(error, "duplicate column name") != NULL;
    if (!duplicate)
        LOG_WARN("proof_validate",
                 "[proof_validate] block_hash migration failed: %s",
                 error ? error : "(no message)");
    if (error) sqlite3_free(error);
    return duplicate;
}

bool proof_validate_log_ensure_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "  height                  INTEGER PRIMARY KEY,"
        "  status                  TEXT    NOT NULL,"
        "  ok                      INTEGER NOT NULL,"
        "  sapling_spends_total    INTEGER NOT NULL,"
        "  sapling_outputs_total   INTEGER NOT NULL,"
        "  sprout_joinsplits_total INTEGER NOT NULL,"
        "  block_hash              BLOB,"
        "  first_failure_txid      BLOB,"
        "  first_failure_proof_type TEXT,"
        "  validated_at            INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("proof_validate", "[proof_validate] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return add_block_hash_if_missing(db);
}

int proof_validate_script_validate_log_at(sqlite3 *db, int height,
                                          struct script_validate_row *out)
{
    memset(out, 0, sizeof(*out));
    out->ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, block_hash FROM script_validate_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("proof_validate", "[proof_validate] script_validate_log prepare failed: %s", sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        out->ok = sqlite3_column_type(st, 0) == SQLITE_INTEGER
                    ? sqlite3_column_int(st, 0) : -1;
        const void *hash = sqlite3_column_blob(st, 1);
        int hash_size = sqlite3_column_bytes(st, 1);
        if (sqlite3_column_type(st, 1) == SQLITE_BLOB && hash &&
            hash_size == 32) {
            memcpy(out->block_hash.data, hash, 32);
            out->has_block_hash = true;
        }
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

bool proof_validate_log_insert(sqlite3 *db, int height,
                               const char *status, bool ok,
                               size_t sapling_spends_total,
                               size_t sapling_outputs_total,
                               size_t sprout_joinsplits_total,
                               const struct uint256 *block_hash,
                               const struct uint256 *first_failure_txid,
                               const char *first_failure_proof_type)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO proof_validate_log "
        "(height, status, ok, sapling_spends_total, "
        " sapling_outputs_total, sprout_joinsplits_total, "
        " block_hash, first_failure_txid, first_failure_proof_type, "
        " validated_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("proof_validate", "[proof_validate] prepare insert failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_text (stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)sapling_spends_total);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)sapling_outputs_total);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)sprout_joinsplits_total);
    if (block_hash)
        sqlite3_bind_blob(stmt, 7, block_hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 7);
    if (first_failure_txid)
        sqlite3_bind_blob(stmt, 8, first_failure_txid->data, 32,
                          SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 8);
    if (first_failure_proof_type)
        sqlite3_bind_text(stmt, 9, first_failure_proof_type, -1,
                          SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 9);
    sqlite3_bind_int64(stmt, 10, (sqlite3_int64)platform_time_wall_unix());
    rc = sqlite3_step(stmt);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("proof_validate", "[proof_validate] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}
