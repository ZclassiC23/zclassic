/* Forward creation index for transparent prevout resolution (P0 §2.1).
 * See jobs/created_outputs_index.h for the design rationale. Raw SQLite over
 * the shared progress.kv DB, mirroring the *_log table idiom of the reducer
 * stages (prepare / bind / step / finalize, loud on error). */

#include "jobs/created_outputs_index.h"

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <string.h>

bool created_outputs_index_ensure_schema(sqlite3 *db)
{
    if (!db)
        LOG_FAIL("created_outputs", "ensure_schema: NULL db");
    static const char *const sql =
        "CREATE TABLE IF NOT EXISTS created_outputs ("
        "  txid   BLOB    NOT NULL,"
        "  vout   INTEGER NOT NULL,"
        "  value  INTEGER NOT NULL,"
        "  script BLOB    NOT NULL,"
        "  height INTEGER NOT NULL,"
        "  PRIMARY KEY (txid, vout)"
        ") WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS created_outputs_height "
        "  ON created_outputs(height);";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("created_outputs", "[created_outputs] schema ensure failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool created_outputs_index_put_block(sqlite3 *db, const struct block *blk,
                                     int height)
{
    if (!db || !blk)
        LOG_FAIL("created_outputs", "put_block: NULL db/blk");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO created_outputs "
        "(txid, vout, value, script, height) VALUES (?,?,?,?,?)",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("created_outputs", "[created_outputs] put prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    bool ok = true;
    for (size_t ti = 0; ok && ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        for (size_t vo = 0; vo < tx->num_vout; vo++) {
            const struct tx_out *o = &tx->vout[vo];
            sqlite3_reset(st);
            sqlite3_bind_blob (st, 1, tx->hash.data, 32, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)vo);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)o->value);
            sqlite3_bind_blob (st, 4, o->script_pub_key.data,
                               (int)o->script_pub_key.size, SQLITE_STATIC);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)height);
            int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
            if (rc != SQLITE_DONE) {
                LOG_WARN("created_outputs",
                         "[created_outputs] put height=%d tx=%zu vout=%zu rc=%d",
                         height, ti, vo, rc);
                ok = false;
                break;
            }
        }
    }
    sqlite3_finalize(st);
    return ok;
}

bool created_outputs_index_get(sqlite3 *db, const uint8_t txid[32],
                               uint32_t vout, int64_t *value_out,
                               uint8_t *script_out, size_t script_cap,
                               size_t *script_len_out)
{
    if (!db || !txid)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT value, script FROM created_outputs WHERE txid = ? AND vout = ?",
        -1, &st, NULL) != SQLITE_OK) {
        /* Missing table (body_persist not yet run) or other error: treat as
         * not-found. The caller (script_validate) FAILS LOUD on a true miss. */
        return false;
    }
    sqlite3_bind_blob (st, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)vout);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        if (value_out)
            *value_out = (int64_t)sqlite3_column_int64(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int blen = sqlite3_column_bytes(st, 1);
        size_t n = (blen < 0) ? 0 : (size_t)blen;
        if (script_len_out)
            *script_len_out = n;
        if (script_out && script_cap) {
            size_t copy = n < script_cap ? n : script_cap;
            if (blob && copy)
                memcpy(script_out, blob, copy);
        }
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool created_outputs_index_get_bounded(sqlite3 *db, const uint8_t txid[32],
                                       uint32_t vout, int min_height,
                                       int max_height, int64_t *value_out,
                                       uint8_t *script_out, size_t script_cap,
                                       size_t *script_len_out,
                                       int *height_out)
{
    if (!db || !txid || min_height > max_height)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT value, script, height FROM created_outputs "
        "WHERE txid = ? AND vout = ? "
        "  AND height >= ? AND height <= ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("created_outputs",
                 "[created_outputs] bounded get prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_blob (st, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)vout);
    sqlite3_bind_int  (st, 3, min_height);
    sqlite3_bind_int  (st, 4, max_height);
    bool found = false;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        if (value_out)
            *value_out = (int64_t)sqlite3_column_int64(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int blen = sqlite3_column_bytes(st, 1);
        size_t n = (blen < 0) ? 0 : (size_t)blen;
        if (script_len_out)
            *script_len_out = n;
        if (script_out && script_cap) {
            size_t copy = n < script_cap ? n : script_cap;
            if (blob && copy)
                memcpy(script_out, blob, copy);
        }
        if (height_out)
            *height_out = sqlite3_column_int(st, 2);
        found = true;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("created_outputs",
                 "[created_outputs] bounded get step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return found;
}

bool created_outputs_index_prune_below(sqlite3 *db, int floor)
{
    if (!db)
        LOG_FAIL("created_outputs", "prune_below: NULL db");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM created_outputs WHERE height < ?", -1, &st, NULL)
        != SQLITE_OK) {
        LOG_WARN("created_outputs", "[created_outputs] prune prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, floor);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("created_outputs", "[created_outputs] prune floor=%d rc=%d",
                 floor, rc);
        return false;
    }
    return true;
}

bool created_outputs_index_prune_below_limited(sqlite3 *db, int floor,
                                               int max_heights,
                                               int *rows_deleted_out)
{
    if (rows_deleted_out)
        *rows_deleted_out = 0;
    if (!db)
        LOG_FAIL("created_outputs", "prune_below_limited: NULL db");
    if (max_heights <= 0)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            db,
            "DELETE FROM created_outputs "
            "WHERE height < ? "
            "  AND height <= COALESCE(("
            "    SELECT MAX(height) FROM ("
            "      SELECT DISTINCT height FROM created_outputs "
            "      WHERE height < ? "
            "      ORDER BY height "
            "      LIMIT ?"
            "    )"
            "  ), -1)",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("created_outputs",
                 "[created_outputs] limited prune prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, floor);
    sqlite3_bind_int(st, 2, floor);
    sqlite3_bind_int(st, 3, max_heights);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_DONE && rows_deleted_out)
        *rows_deleted_out = sqlite3_changes(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("created_outputs",
                 "[created_outputs] limited prune floor=%d max_heights=%d "
                 "rc=%d: %s",
                 floor, max_heights, rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}
