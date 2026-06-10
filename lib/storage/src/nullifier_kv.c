/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * nullifier_kv — implementation. See storage/nullifier_kv.h for the contract
 * (pool namespaces, the rewind invariant, permanence).
 *
 * Raw sqlite3_step calls carry // raw-sql-ok:progress-kv-kernel-store, the
 * sanctioned hatch for the kernel store (same convention as coins_kv.c /
 * progress_store.c). The nullifier set sits BELOW the AR lifecycle — it is
 * reducer consensus state, not an AR model. */
#include "storage/nullifier_kv.h"

#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>

bool nullifier_kv_ensure_schema(sqlite3 *db)
{
    if (!db) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] ensure_schema: NULL db");
        return false;
    }
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS nullifiers ("
        "  nf     BLOB    NOT NULL,"
        "  pool   INTEGER NOT NULL,"
        "  height INTEGER NOT NULL,"
        "  PRIMARY KEY (nf, pool)"
        ") WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS idx_nullifiers_height "
        "ON nullifiers(height)",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] schema ensure failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool nullifier_kv_table_exists(sqlite3 *db)
{
    if (!db) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='nullifiers'",
        -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] table_exists prepare: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    bool found = sqlite3_step(s) == SQLITE_ROW;  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(s);
    return found;
}

bool nullifier_kv_get(sqlite3 *db, const uint8_t nf[32], int pool,
                      bool *found, int64_t *height_out)
{
    if (found) *found = false;
    if (!db || !nf || !found) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] get: NULL arg");
        return false;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT height FROM nullifiers WHERE nf=? AND pool=?",
        -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] get prepare: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_blob(s, 1, nf, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, 2, pool);
    bool ok = true;
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found = true;
        if (height_out) *height_out = sqlite3_column_int64(s, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] get step rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        ok = false;
    }
    sqlite3_finalize(s);
    return ok;
}

bool nullifier_kv_add(sqlite3 *db, const uint8_t nf[32], int pool,
                      int64_t height)
{
    if (!db || !nf) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] add: NULL arg");
        return false;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO nullifiers(nf,pool,height) VALUES(?,?,?)",
        -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] add prepare: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_blob (s, 1, nf, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int  (s, 2, pool);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)height);
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] add step rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

bool nullifier_kv_delete_range(sqlite3 *db, int64_t first_h, int64_t last_h)
{
    if (!db) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] delete_range: NULL db");
        return false;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM nullifiers WHERE height >= ? AND height <= ?",
        -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] delete_range prepare: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(s, 1, (sqlite3_int64)first_h);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)last_h);
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        LOG_WARN("nullifier_kv", "[nullifier_kv] delete_range step rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}
