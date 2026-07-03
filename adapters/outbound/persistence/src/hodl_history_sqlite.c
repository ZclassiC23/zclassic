/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * hodl_history_sqlite — sqlite implementation of hodl_history_port.
 *
 * The five methods below carry the EXACT SQL text and binding order of the
 * hodl-history queries, so the persisted snapshots and the explorer/MCP
 * surface stay bit-for-bit identical.
 */

#include "adapters/outbound/persistence/hodl_history_sqlite.h"

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

#include <string.h>

/* `self` aliases the sqlite3* directly — there is no wrapper struct. */
static inline sqlite3 *db_of(void *self) { return (sqlite3 *)self; }

#define HODL_HISTORY_SOURCE_TIP_KEY "sync_projection_tip_height"

static int64_t hh_projection_tip_height(sqlite3 *db, int64_t tableless_fallback)
{
    if (!db)
        return 0;

    sqlite3_stmt *s = NULL;
    const char *sql =
        "SELECT value FROM node_state WHERE key='" HODL_HISTORY_SOURCE_TIP_KEY "'";
    int rc = sqlite3_prepare_v2(db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK || !s) {
        const char *err = sqlite3_errmsg(db);
        if (err && strstr(err, "no such table") != NULL)
            return tableless_fallback;
        LOG_WARN("hodl_history",
                 "prepare source-tip SQL failed: %s", err ? err : "(null)");
        return 0;
    }

    int64_t h = 0;
    rc = AR_STEP_ROW_READONLY(s);
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        if (blob && len == (int)sizeof(int64_t)) {
            memcpy(&h, blob, sizeof(h));
        } else if (blob && len == (int)sizeof(int32_t)) {
            int32_t h32 = 0;
            memcpy(&h32, blob, sizeof(h32));
            h = h32;
        } else {
            LOG_WARN("hodl_history",
                     "source-tip value has unexpected length %d", len);
        }
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        LOG_WARN("hodl_history",
                 "source-tip step rc=%d: %s", rc, sqlite3_errmsg(db));
        return 0;
    }
    return h > 0 ? h : 0;
}

static bool hh_block_time(void *self, int64_t height, int64_t *out_time)
{
    sqlite3 *db = db_of(self);
    if (!db || !out_time)
        return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT time FROM blocks WHERE height = ?",
                           -1, &s, NULL) != SQLITE_OK || !s)
        return false;
    sqlite3_bind_int64(s, 1, height);
    bool got = false;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        *out_time = sqlite3_column_int64(s, 0);
        got = true;
    }
    sqlite3_finalize(s);
    return got;
}

static bool hh_compute_snapshot(void *self,
                                int64_t height,
                                int64_t cutoff_time,
                                int64_t *out_total,
                                int64_t *out_older)
{
    sqlite3 *db = db_of(self);
    if (!db || !out_total || !out_older)
        return false;

    /* Compute total + older-than-1y in a single pass.
     *   o "alive at H": LEFT JOIN tx_inputs filtered to spends <= H,
     *                   keep only rows where no such spend exists.
     *   "older than 1y": creation-block time <= block_time - 1y. */
    const char *sql =
        "SELECT "
        "  COALESCE(SUM(o.value), 0) AS total_zat,"
        "  COALESCE(SUM(CASE WHEN b.time <= ?1 THEN o.value ELSE 0 END), 0) "
        "    AS older_zat "
        "FROM tx_outputs o "
        "JOIN blocks b ON b.height = o.block_height "
        "LEFT JOIN tx_inputs i "
        "  ON i.prev_txid = o.txid AND i.prev_vout = o.vout "
        "     AND i.block_height <= ?2 "
        "WHERE o.block_height <= ?2 AND i.txid IS NULL";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s) {
        LOG_FAIL("hodl_history",
                 "prepare snapshot SQL failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(s, 1, cutoff_time);
    sqlite3_bind_int64(s, 2, height);
    int64_t total = 0, older = 0;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        total = sqlite3_column_int64(s, 0);
        older = sqlite3_column_int64(s, 1);
    }
    sqlite3_finalize(s);
    *out_total = total;
    *out_older = older;
    return true;
}

static bool hh_upsert_snapshot(void *self,
                               const struct hodl_history_snapshot *row)
{
    sqlite3 *db = db_of(self);
    if (!db || !row)
        return false;
    sqlite3_stmt *ins = NULL;
    int64_t source_tip = hh_projection_tip_height(db, row->height);
    const char *ins_sql =
        "INSERT OR REPLACE INTO hodl_history "
        "(height, time, total_zat, older_1y_zat, older_1y_pct, "
        " calc_version, source_tip_height) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";
    if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) != SQLITE_OK || !ins) {
        LOG_FAIL("hodl_history",
                 "prepare INSERT failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(ins, 1, row->height);
    sqlite3_bind_int64(ins, 2, row->time);
    sqlite3_bind_int64(ins, 3, row->total_zat);
    sqlite3_bind_int64(ins, 4, row->older_1y_zat);
    sqlite3_bind_double(ins, 5, row->older_1y_pct);
    sqlite3_bind_int(ins, 6, HODL_HISTORY_SNAPSHOT_CALC_VERSION);
    sqlite3_bind_int64(ins, 7, source_tip);
    int rc = AR_STEP_WRITE(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        LOG_FAIL("hodl_history",
                 "INSERT step rc=%d: %s", rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static int64_t hh_max_filled_height(void *self)
{
    sqlite3 *db = db_of(self);
    if (!db)
        return 0;
    sqlite3_stmt *s = NULL;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(MAX(height), 0) FROM hodl_history",
            -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
            v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

static bool hh_next_fill_height(void *self,
                                int64_t stride,
                                int64_t target,
                                int64_t *out_height)
{
    sqlite3 *db = db_of(self);
    if (!db || !out_height || stride <= 0 || target < stride)
        return false;

    *out_height = 0;
    int64_t source_tip = hh_projection_tip_height(db, target);
    if (source_tip > target)
        source_tip = target;
    if (source_tip < stride)
        return true;

    sqlite3_stmt *s = NULL;
    const char *sql =
        "WITH RECURSIVE expected(h) AS ("
        "  SELECT ?1"
        "  UNION ALL SELECT h + ?1 FROM expected WHERE h + ?1 <= ?2"
        ") "
        "SELECT e.h "
        "FROM expected e "
        "LEFT JOIN hodl_history hh ON hh.height = e.h "
        "WHERE e.h <= ?3 AND ("
        "   hh.height IS NULL "
        "   OR hh.calc_version < ?4 "
        "   OR hh.source_tip_height < e.h"
        ") "
        "ORDER BY e.h LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s) {
        LOG_FAIL("hodl_history",
                 "prepare next-fill SQL failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(s, 1, stride);
    sqlite3_bind_int64(s, 2, target);
    sqlite3_bind_int64(s, 3, source_tip);
    sqlite3_bind_int(s, 4, HODL_HISTORY_SNAPSHOT_CALC_VERSION);
    int rc = AR_STEP_ROW_READONLY(s);
    if (rc == SQLITE_ROW)
        *out_height = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        LOG_FAIL("hodl_history",
                 "next-fill step rc=%d: %s", rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static int hh_load_all(void *self,
                       struct hodl_history_snapshot *out,
                       int max_rows)
{
    sqlite3 *db = db_of(self);
    if (!db || !out || max_rows <= 0)
        return 0;
    const char *sql =
        "SELECT height, time, total_zat, older_1y_zat, older_1y_pct "
        "FROM ("
        "  SELECT height, time, total_zat, older_1y_zat, older_1y_pct "
        "  FROM hodl_history ORDER BY height DESC LIMIT ?"
        ") ORDER BY height ASC";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s)
        return 0;
    sqlite3_bind_int(s, 1, max_rows);
    int n = 0;
    while (n < max_rows && AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        out[n].height       = sqlite3_column_int64(s, 0);
        out[n].time         = sqlite3_column_int64(s, 1);
        out[n].total_zat    = sqlite3_column_int64(s, 2);
        out[n].older_1y_zat = sqlite3_column_int64(s, 3);
        out[n].older_1y_pct = sqlite3_column_double(s, 4);
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

bool hodl_history_sqlite_bind(sqlite3 *db, struct hodl_history_port *out_port)
{
    if (!db || !out_port)
        return false;
    *out_port = (struct hodl_history_port){
        .self              = db,
        .block_time        = hh_block_time,
        .compute_snapshot  = hh_compute_snapshot,
        .upsert_snapshot   = hh_upsert_snapshot,
        .max_filled_height = hh_max_filled_height,
        .next_fill_height  = hh_next_fill_height,
        .load_all          = hh_load_all,
    };
    return true;
}
