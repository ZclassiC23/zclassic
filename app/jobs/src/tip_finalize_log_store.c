/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_log_store — implementation. See tip_finalize_log_store.h. */

#include "tip_finalize_log_store.h"

#include "platform/time_compat.h"
#include "core/arith_uint256.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

bool ensure_log_schema(sqlite3 *db)
{
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "  height           INTEGER PRIMARY KEY,"
        "  status           TEXT    NOT NULL,"
        "  ok               INTEGER NOT NULL,"
        "  work_delta_high  INTEGER NOT NULL,"
        "  work_delta_low   INTEGER NOT NULL,"
        "  utxo_size_after  INTEGER NOT NULL,"
        "  reorg_depth      INTEGER NOT NULL,"
        "  finalized_at     INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("tip_finalize", "[tip_finalize] schema ensure failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(db,
        "ALTER TABLE tip_finalize_log ADD COLUMN tip_hash BLOB",
        NULL, NULL, &err) != SQLITE_OK) {
        if (!err || strstr(err, "duplicate column name") == NULL) {
            LOG_WARN("tip_finalize", "[tip_finalize] schema alter failed: %s", err ? err : "(no message)");
            if (err) sqlite3_free(err);
            return false;
        }
        sqlite3_free(err);
    }
    return true;
}

int utxo_apply_log_at(sqlite3 *db, int height,
                      struct utxo_apply_row *out)
{
    memset(out, 0, sizeof(*out));
    out->ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, spent_count, added_count "
        "FROM utxo_apply_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("tip_finalize", "[tip_finalize] utxo_apply_log prepare failed: %s", sqlite3_errmsg(db));
        return -1;  // raw-return-ok:logged-above
    }
    sqlite3_bind_int(st, 1, height);
    int found = 0;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        out->ok = sqlite3_column_int(st, 0);
        out->spent_count = sqlite3_column_int64(st, 1);
        out->added_count = sqlite3_column_int64(st, 2);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

bool utxo_apply_sums_through(sqlite3 *db, int height,
                             int64_t *spent_out,
                             int64_t *added_out)
{
    *spent_out = 0;
    *added_out = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT COALESCE(SUM(spent_count),0), "
        "       COALESCE(SUM(added_count),0) "
        "FROM utxo_apply_log WHERE height <= ? AND ok = 1",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("tip_finalize", "[tip_finalize] utxo_apply_log sum prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        *spent_out = sqlite3_column_int64(st, 0);
        *added_out = sqlite3_column_int64(st, 1);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool log_insert(sqlite3 *db, int height, const char *status, bool ok,
                const struct arith_uint256 *work_delta,
                int64_t utxo_size_after, int reorg_depth,
                const struct uint256 *tip_hash)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO tip_finalize_log "
        "(height, status, ok, work_delta_high, work_delta_low, "
        " utxo_size_after, reorg_depth, finalized_at, tip_hash) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("tip_finalize", "[tip_finalize] prepare insert failed: %s", sqlite3_errmsg(db));
        return false;
    }

    uint64_t hi = 0, lo = 0;
    if (work_delta) {
        lo = arith_uint256_get_low64(work_delta);
        hi = ((uint64_t)work_delta->pn[3] << 32) | work_delta->pn[2];
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)height);
    sqlite3_bind_text (stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 3, ok ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)hi);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)lo);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)utxo_size_after);
    sqlite3_bind_int  (stmt, 7, reorg_depth);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)platform_time_wall_unix());
    if (tip_hash)
        sqlite3_bind_blob(stmt, 9, tip_hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 9);
    rc = sqlite3_step(stmt);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_WARN("tip_finalize", "[tip_finalize] insert height=%d rc=%d", height, rc);
        return false;
    }
    return true;
}

bool finalized_tip_row_at(sqlite3 *db, int height,
                          struct finalized_tip_row *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, tip_hash, status FROM tip_finalize_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("tip_finalize", "[tip_finalize] finalized row prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        out->found = true;
        out->ok = sqlite3_column_int(st, 0) != 0;
        const void *blob = sqlite3_column_blob(st, 1);
        int n = sqlite3_column_bytes(st, 1);
        if (blob && n == 32) {
            memcpy(out->tip_hash.data, blob, 32);
            out->has_tip_hash = true;
        }
        const unsigned char *status = sqlite3_column_text(st, 2);
        out->is_anchor = (status && strcmp((const char *)status, "anchor") == 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("tip_finalize", "[tip_finalize] finalized row step failed rc=%d: %s", rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}
