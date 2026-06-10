/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the FIX-1 tip_finalize_log hole backfill (WP-B / TIPFIN):
 * stage_repair_reducer_frontier_tipfin.c. Covers T2 (lifecycle: write,
 * witness create/bump/delete, zero deletions), T3 (real coin tear refuses
 * byte-identically), T4 (adversarial G2/G3 refusals) and T9 (the
 * panel-required exact live state: FIX-2a clamp -> simulated refill ->
 * span backfill -> arithmetic convergence -> tip_finalize clamps to C). */

#include "test/test_helpers.h"

#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* WP-B exports — declared in app/jobs/src/stage_repair_reducer_frontier_
 * internal.h, which is src-local (not on the test include path); these
 * prototypes mirror it exactly. Both TUs link into the test binary. */
bool stage_reducer_frontier_try_tipfin_backfill(
    sqlite3 *db, bool apply,
    struct stage_reducer_frontier_reconcile_result *out, bool *handled);
bool stage_reducer_frontier_try_unapplied_hole_clamp(
    sqlite3 *db, bool apply,
    struct stage_reducer_frontier_reconcile_result *out, bool *handled);

/* Mirror of enum tipfin_refused_reason in
 * stage_repair_reducer_frontier_tipfin.c (the result field carries an int
 * code, "0 = none"). Keep the values in sync with that TU. */
enum {
    T_REFUSED_NONE = 0,
    T_REFUSED_G2_EVIDENCE_ROW = 2,
    T_REFUSED_G3_MISSING_EVIDENCE = 4,
    T_REFUSED_G5_BINDER_MISSING = 6,
};

#define TIPFIN_CHECK(name, expr) do { \
    printf("stage_repair_tipfin_backfill: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

#define A REDUCER_FRONTIER_TRUSTED_ANCHOR
#define TIPFIN_WITNESS_KEY "tipfin_backfill.progress"

/* Distinct deterministic hashes per height (rfrl fixture convention). */
static void mk_hash(struct uint256 *h, int height)
{
    memset(h, 0, sizeof(*h));
    h->data[0] = (uint8_t)(height & 0xff);
    h->data[1] = (uint8_t)((height >> 8) & 0xff);
    h->data[2] = (uint8_t)((height >> 16) & 0xff);
    h->data[31] = 0x7c;
}

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("SQL failed: %s\n", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

/* Production-shaped schema: tip_finalize_log carries the FULL column set so
 * the production log_insert (work_delta/finalized_at columns) round-trips. */
static bool seed_schema(sqlite3 *db)
{
    return
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS header_admit_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL,"
            "parent_hash BLOB, admitted_at INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS validate_headers_log ("
            "height INTEGER PRIMARY KEY, hash BLOB, ok INTEGER NOT NULL,"
            "fail_reason TEXT, validated_at INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS script_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
            "block_hash BLOB)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS body_persist_log ("
            "height INTEGER PRIMARY KEY, source TEXT, ok INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS body_fetch_log ("
            "height INTEGER PRIMARY KEY, hash BLOB, source TEXT,"
            "bytes INTEGER, fetched_at INTEGER, ok INTEGER,"
            "fail_reason TEXT)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS proof_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
            "height INTEGER PRIMARY KEY, status TEXT NOT NULL,"
            "ok INTEGER NOT NULL, work_delta_high INTEGER NOT NULL,"
            "work_delta_low INTEGER NOT NULL, utxo_size_after INTEGER NOT NULL,"
            "reorg_depth INTEGER NOT NULL, finalized_at INTEGER NOT NULL,"
            "tip_hash BLOB)");
}

static bool seed_cursor(sqlite3 *db, const char *name, int cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at) "
            "VALUES(?,?,1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool seed_all_cursors(sqlite3 *db, int cursor)
{
    return seed_cursor(db, "validate_headers", cursor) &&
           seed_cursor(db, "body_fetch", cursor) &&
           seed_cursor(db, "body_persist", cursor) &&
           seed_cursor(db, "script_validate", cursor) &&
           seed_cursor(db, "proof_validate", cursor) &&
           seed_cursor(db, "utxo_apply", cursor) &&
           seed_cursor(db, "tip_finalize", cursor);
}

static bool put_validate(sqlite3 *db, int height, int ok_flag,
                         const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO validate_headers_log(height,hash,ok) "
            "VALUES(?,?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    if (hash)
        sqlite3_bind_blob(st, 2, hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 2);
    sqlite3_bind_int(st, 3, ok_flag);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_script(sqlite3 *db, int height, int ok_flag,
                       const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO script_validate_log"
            "(height,status,ok,block_hash) VALUES(?,'verified',?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    if (hash)
        sqlite3_bind_blob(st, 3, hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 3);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_simple(sqlite3 *db, const char *table, int height, int ok_flag)
{
    char sql[160];
    if (strcmp(table, "body_persist_log") == 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,source,ok) "
                 "VALUES(?,'fixture',?)",
                 table);
    } else {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,status,ok) "
                 "VALUES(?,'verified',?)",
                 table);
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_tip(sqlite3 *db, int height, const char *status, int ok_flag,
                    const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO tip_finalize_log"
            "(height,status,ok,work_delta_high,work_delta_low,"
            " utxo_size_after,reorg_depth,finalized_at,tip_hash) "
            "VALUES(?,?,?,0,0,0,0,1,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_text(st, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, ok_flag);
    if (hash)
        sqlite3_bind_blob(st, 4, hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 4);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* All five upstream logs ok=1 at `height` (validate + script bound to hash). */
static bool put_upstream_ok(sqlite3 *db, int height,
                            const struct uint256 *hash)
{
    return put_validate(db, height, 1, hash) &&
           put_script(db, height, 1, hash) &&
           put_simple(db, "body_persist_log", height, 1) &&
           put_simple(db, "proof_validate_log", height, 1) &&
           put_simple(db, "utxo_apply_log", height, 1);
}

static bool seed_coins_applied(sqlite3 *db, int64_t height)
{
    uint8_t blob[8];
    for (int i = 0; i < 8; i++)
        blob[i] = (uint8_t)((uint64_t)height >> (8 * i));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, "coins_applied_height", -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, blob, sizeof(blob), SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static int cursor_value(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            value = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

/* (count, sum(height), sum(ok)) digest per log table + every stage cursor:
 * the "cursors and logs byte-identical" witness for refusal cases. */
#define TIPFIN_DIGEST_N (6 * 3 + 7)

static bool digest_all(sqlite3 *db, long long out[TIPFIN_DIGEST_N])
{
    static const char *const logs[] = {
        "validate_headers_log", "script_validate_log", "body_persist_log",
        "proof_validate_log",   "utxo_apply_log",      "tip_finalize_log",
    };
    static const char *const stages[] = {
        "validate_headers", "body_fetch", "body_persist", "script_validate",
        "proof_validate",   "utxo_apply", "tip_finalize",
    };
    int k = 0;
    for (size_t i = 0; i < sizeof(logs) / sizeof(logs[0]); i++) {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*), COALESCE(SUM(height),0), "
                 "COALESCE(SUM(ok),0) FROM %s", logs[i]);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            return false;
        if (sqlite3_step(st) != SQLITE_ROW) {
            sqlite3_finalize(st);
            return false;
        }
        out[k++] = sqlite3_column_int64(st, 0);
        out[k++] = sqlite3_column_int64(st, 1);
        out[k++] = sqlite3_column_int64(st, 2);
        sqlite3_finalize(st);
    }
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
        out[k++] = cursor_value(db, stages[i]);
    return k == TIPFIN_DIGEST_N;
}

struct tip_row_view {
    bool found;
    int ok;
    char status[32];
    bool has_hash;
    struct uint256 hash;
};

static bool tip_row_at(sqlite3 *db, int height, struct tip_row_view *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok, status, tip_hash FROM tip_finalize_log "
            "WHERE height=?",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->found = true;
        out->ok = sqlite3_column_int(st, 0);
        const unsigned char *s = sqlite3_column_text(st, 1);
        snprintf(out->status, sizeof(out->status), "%s",
                 s ? (const char *)s : "");
        const void *blob = sqlite3_column_blob(st, 2);
        if (blob && sqlite3_column_bytes(st, 2) == 32) {
            memcpy(out->hash.data, blob, 32);
            out->has_hash = true;
        }
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static long long tip_row_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    long long n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tip_finalize_log",
                           -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static bool witness_at(sqlite3 *db, bool *found, int32_t *last,
                       uint32_t *total)
{
    *found = false;
    *last = -1;
    *total = 0;
    uint8_t buf[8] = {0};
    size_t n = 0;
    if (!progress_meta_get(db, TIPFIN_WITNESS_KEY, buf, sizeof(buf), &n,
                           found))
        return false;
    if (!*found)
        return true;
    if (n != sizeof(buf))
        return false;
    uint32_t l = 0, t = 0;
    for (int i = 0; i < 4; i++)
        l |= (uint32_t)buf[i] << (8 * i);
    for (int i = 0; i < 4; i++)
        t |= (uint32_t)buf[4 + i] << (8 * i);
    *last = (int32_t)l;
    *total = t;
    return true;
}

static long long marker_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    long long n = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM progress_meta WHERE key LIKE "
            "'reducer_frontier.tipfin_backfill_repair.%'",
            -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* The read_frontier_snapshot subset the WP-B repairs consume (the tests
 * drive the exports directly, like the production reconcile would). */
static bool snapshot(sqlite3 *db,
                     struct stage_reducer_frontier_reconcile_result *out)
{
    static const char *const stages[] = {
        "validate_headers", "body_fetch", "body_persist", "script_validate",
        "proof_validate",   "utxo_apply", "tip_finalize",
    };
    memset(out, 0, sizeof(*out));
    progress_store_tx_lock();
    int32_t hstar = 0, floor = 0;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &floor);
    int32_t coins = 0;
    bool found = false;
    ok = ok && coins_kv_get_applied_height(db, &coins, &found);
    progress_store_tx_unlock();
    if (!ok)
        return false;
    out->hstar = hstar;
    out->served_floor = floor;
    out->sweep_top = floor;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        int cursor = cursor_value(db, stages[i]);
        if (cursor > 0 && cursor - 1 > out->sweep_top)
            out->sweep_top = cursor - 1;
    }
    out->coins_applied_found = found;
    out->coins_applied_height = found ? coins : -1;
    if (!found)
        out->refused_coin_unknown = true;
    else if (coins > hstar + 1)
        out->refused_coin_tear = true;
    return true;
}

static bool open_fixture(char *dir, size_t dir_n, const char *tag,
                         int cursor_top)
{
    test_make_tmpdir(dir, dir_n, "stage_repair_tipfin_backfill", tag);
    if (!progress_store_open(dir))
        return false;
    return seed_schema(progress_store_db()) &&
           seed_all_cursors(progress_store_db(), cursor_top);
}

int test_stage_repair_tipfin_backfill(void);
int test_stage_repair_tipfin_backfill(void)
{
    printf("\n=== stage_repair_tipfin_backfill tests ===\n");
    int failures = 0;

    /* ── T2 — lifecycle: write, witness create/bump/delete, no deletes ── */
    {
        char dir[256];
        TIPFIN_CHECK("T2 setup", open_fixture(dir, sizeof(dir), "t2", A + 7));
        sqlite3 *db = progress_store_db();

        struct uint256 h[8];
        for (int i = 1; i <= 7; i++)
            mk_hash(&h[i], A + i);

        bool seeded = true;
        for (int i = 1; i <= 6; i++)
            seeded = seeded && put_upstream_ok(db, A + i, &h[i]);
        /* Binder gap mid-span: the script hash at A+4 is NULL until pass 3,
         * so G5 blocks p=A+3 and the witness record is observable. */
        seeded = seeded && put_script(db, A + 4, 1, NULL);
        seeded = seeded && put_tip(db, A + 1, "finalized", 1, &h[2]) &&
                 put_tip(db, A + 5, "finalized", 1, &h[6]) &&
                 put_tip(db, A + 6, "finalized", 1, &h[7]) &&
                 seed_coins_applied(db, A + 6);
        TIPFIN_CHECK("T2 seed", seeded);

        struct stage_reducer_frontier_reconcile_result rr;
        bool handled = false;
        TIPFIN_CHECK("T2 snapshot torn",
                     snapshot(db, &rr) && rr.hstar == A + 1 &&
                     rr.served_floor == A + 6 && rr.refused_coin_tear);

        /* Dry run: reports the repair, mutates nothing. */
        TIPFIN_CHECK("T2 dry-run reports without mutating",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, false, &rr, &handled) &&
                     handled && rr.repaired &&
                     rr.tipfin_backfill_height == A + 2 &&
                     tip_row_count(db) == 3 && marker_count(db) == 0);

        /* Pass 1: backfills A+2, blocks at the A+4 binder gap. */
        bool found = false;
        int32_t wlast = -1;
        uint32_t wtotal = 0;
        TIPFIN_CHECK("T2 snapshot pass1", snapshot(db, &rr));
        handled = false;
        TIPFIN_CHECK("T2 pass1 backfills first hole",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     handled && rr.repaired && !rr.refused_coin_tear &&
                     rr.tipfin_backfill_count == 1 &&
                     rr.tipfin_backfill_height == A + 2);
        struct tip_row_view row;
        TIPFIN_CHECK("T2 pass1 row hash-bound to h(p+1)",
                     tip_row_at(db, A + 2, &row) && row.found &&
                     row.ok == 1 &&
                     strcmp(row.status, "finalize_backfill") == 0 &&
                     row.has_hash &&
                     memcmp(row.hash.data, h[3].data, 32) == 0);
        TIPFIN_CHECK("T2 pass1 witness created + marker recorded",
                     witness_at(db, &found, &wlast, &wtotal) && found &&
                     wlast == A + 2 && wtotal == 1 && marker_count(db) == 1);

        /* Pass 2: binder still missing at A+4 — zero progress, named G5. */
        TIPFIN_CHECK("T2 snapshot pass2",
                     snapshot(db, &rr) && rr.hstar == A + 2 &&
                     rr.refused_coin_tear);
        handled = false;
        TIPFIN_CHECK("T2 pass2 blocked on missing binder",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     !handled && rr.tipfin_backfill_count == 0 &&
                     rr.tipfin_backfill_refused_reason ==
                         T_REFUSED_G5_BINDER_MISSING);
        TIPFIN_CHECK("T2 pass2 witness untouched",
                     witness_at(db, &found, &wlast, &wtotal) && found &&
                     wlast == A + 2 && wtotal == 1 &&
                     tip_row_count(db) == 4);

        /* Pass 3: binder restored — span completes, witness deleted. */
        TIPFIN_CHECK("T2 restore binder", put_script(db, A + 4, 1, &h[4]));
        TIPFIN_CHECK("T2 snapshot pass3",
                     snapshot(db, &rr) && rr.hstar == A + 2);
        handled = false;
        TIPFIN_CHECK("T2 pass3 completes span",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     handled && rr.tipfin_backfill_count == 2 &&
                     rr.tipfin_backfill_height == A + 4);
        TIPFIN_CHECK("T2 pass3 rows hash-bound",
                     tip_row_at(db, A + 3, &row) && row.found && row.ok == 1 &&
                     memcmp(row.hash.data, h[4].data, 32) == 0 &&
                     tip_row_at(db, A + 4, &row) && row.found && row.ok == 1 &&
                     memcmp(row.hash.data, h[5].data, 32) == 0);
        TIPFIN_CHECK("T2 pass3 witness deleted on span completion",
                     witness_at(db, &found, &wlast, &wtotal) && !found &&
                     marker_count(db) == 1);

        /* Zero deletions: 3 original rows + 3 backfilled, originals intact,
         * and the repair wrote no cursor. */
        TIPFIN_CHECK("T2 zero deletions, originals + cursors intact",
                     tip_row_count(db) == 6 &&
                     tip_row_at(db, A + 1, &row) && row.found &&
                     row.ok == 1 && strcmp(row.status, "finalized") == 0 &&
                     tip_row_at(db, A + 5, &row) && row.found &&
                     row.ok == 1 && strcmp(row.status, "finalized") == 0 &&
                     cursor_value(db, "tip_finalize") == A + 7 &&
                     cursor_value(db, "script_validate") == A + 7);

        /* Healed: the coin-tear refusal clears arithmetically. */
        TIPFIN_CHECK("T2 post-heal pass reports no tear",
                     snapshot(db, &rr) && rr.hstar == A + 6 &&
                     !rr.refused_coin_tear);
        handled = false;
        TIPFIN_CHECK("T2 post-heal backfill not applicable",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     !handled &&
                     rr.tipfin_backfill_refused_reason == T_REFUSED_NONE);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── T3 — real coin tear (hole strictly below coins): refuse ── */
    {
        char dir[256];
        TIPFIN_CHECK("T3 setup", open_fixture(dir, sizeof(dir), "t3", A + 6));
        sqlite3 *db = progress_store_db();

        struct uint256 h[7];
        for (int i = 1; i <= 6; i++)
            mk_hash(&h[i], A + i);

        /* The REAL tear: the script row is missing at A+3, strictly below
         * the coins frontier (A+5). Everything else is ok=1. */
        bool seeded = true;
        for (int i = 1; i <= 5; i++) {
            seeded = seeded && put_validate(db, A + i, 1, &h[i]) &&
                     put_simple(db, "body_persist_log", A + i, 1) &&
                     put_simple(db, "proof_validate_log", A + i, 1) &&
                     put_simple(db, "utxo_apply_log", A + i, 1);
            if (i != 3)
                seeded = seeded && put_script(db, A + i, 1, &h[i]);
        }
        seeded = seeded &&
                 put_tip(db, A + 1, "finalized", 1, &h[2]) &&
                 put_tip(db, A + 2, "finalized", 1, &h[3]) &&
                 seed_coins_applied(db, A + 5);
        TIPFIN_CHECK("T3 seed", seeded);

        struct stage_reducer_frontier_reconcile_result rr;
        TIPFIN_CHECK("T3 snapshot pins at the tear",
                     snapshot(db, &rr) && rr.hstar == A + 2 &&
                     rr.refused_coin_tear);

        long long before[TIPFIN_DIGEST_N], after[TIPFIN_DIGEST_N];
        TIPFIN_CHECK("T3 digest before", digest_all(db, before));

        bool handled = false;
        TIPFIN_CHECK("T3 backfill refuses, names the binding log",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     !handled &&
                     rr.tipfin_backfill_refused_reason ==
                         T_REFUSED_G3_MISSING_EVIDENCE);

        /* FIX-2a must NOT clamp: the hole is strictly below the coins
         * frontier — the unapplied-hole clamp floor. */
        struct stage_reducer_frontier_reconcile_result rr2;
        bool handled2 = false;
        TIPFIN_CHECK("T3 snapshot for clamp", snapshot(db, &rr2));
        TIPFIN_CHECK("T3 FIX-2a does not clamp below the frontier",
                     stage_reducer_frontier_try_unapplied_hole_clamp(
                         db, true, &rr2, &handled2) &&
                     cursor_value(db, "script_validate") == A + 6 &&
                     cursor_value(db, "proof_validate") == A + 6);

        TIPFIN_CHECK("T3 cursors and logs byte-identical",
                     digest_all(db, after) &&
                     memcmp(before, after, sizeof(before)) == 0);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── T4 — adversarial: G3 (proof ok=0) and G2 (evidence row) ── */
    {
        char dir[256];
        TIPFIN_CHECK("T4a setup",
                     open_fixture(dir, sizeof(dir), "t4a", A + 5));
        sqlite3 *db = progress_store_db();

        struct uint256 h[6];
        for (int i = 1; i <= 5; i++)
            mk_hash(&h[i], A + i);

        bool seeded = true;
        for (int i = 1; i <= 4; i++)
            seeded = seeded && put_upstream_ok(db, A + i, &h[i]);
        seeded = seeded &&
                 put_simple(db, "proof_validate_log", A + 2, 0) &&
                 put_tip(db, A + 1, "finalized", 1, &h[2]) &&
                 put_tip(db, A + 4, "finalized", 1, &h[5]) &&
                 seed_coins_applied(db, A + 4);
        TIPFIN_CHECK("T4a seed", seeded);

        struct stage_reducer_frontier_reconcile_result rr;
        TIPFIN_CHECK("T4a snapshot",
                     snapshot(db, &rr) && rr.hstar == A + 1 &&
                     rr.refused_coin_tear);

        long long before[TIPFIN_DIGEST_N], after[TIPFIN_DIGEST_N];
        TIPFIN_CHECK("T4a digest before", digest_all(db, before));
        bool handled = false;
        struct tip_row_view row;
        TIPFIN_CHECK("T4a G3 refuses on proof ok=0",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     !handled &&
                     rr.tipfin_backfill_refused_reason ==
                         T_REFUSED_G3_MISSING_EVIDENCE &&
                     tip_row_at(db, A + 2, &row) && !row.found);
        TIPFIN_CHECK("T4a byte-identical",
                     digest_all(db, after) &&
                     memcmp(before, after, sizeof(before)) == 0);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }
    {
        char dir[256];
        TIPFIN_CHECK("T4b setup",
                     open_fixture(dir, sizeof(dir), "t4b", A + 5));
        sqlite3 *db = progress_store_db();

        struct uint256 h[6];
        for (int i = 1; i <= 5; i++)
            mk_hash(&h[i], A + i);

        bool seeded = true;
        for (int i = 1; i <= 4; i++)
            seeded = seeded && put_upstream_ok(db, A + i, &h[i]);
        seeded = seeded &&
                 put_tip(db, A + 1, "finalized", 1, &h[2]) &&
                 put_tip(db, A + 2, "utxo_count_diverged", 0, NULL) &&
                 put_tip(db, A + 4, "finalized", 1, &h[5]) &&
                 seed_coins_applied(db, A + 4);
        TIPFIN_CHECK("T4b seed", seeded);

        struct stage_reducer_frontier_reconcile_result rr;
        TIPFIN_CHECK("T4b snapshot",
                     snapshot(db, &rr) && rr.hstar == A + 1 &&
                     rr.refused_coin_tear);

        long long before[TIPFIN_DIGEST_N], after[TIPFIN_DIGEST_N];
        TIPFIN_CHECK("T4b digest before", digest_all(db, before));
        bool handled = false;
        struct tip_row_view row;
        TIPFIN_CHECK("T4b G2 refuses on the ok=0 evidence row",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     !handled &&
                     rr.tipfin_backfill_refused_reason ==
                         T_REFUSED_G2_EVIDENCE_ROW);
        TIPFIN_CHECK("T4b evidence row never overwritten",
                     tip_row_at(db, A + 2, &row) && row.found &&
                     row.ok == 0 &&
                     strcmp(row.status, "utxo_count_diverged") == 0);
        TIPFIN_CHECK("T4b byte-identical",
                     digest_all(db, after) &&
                     memcmp(before, after, sizeof(before)) == 0);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── T9 — panel-required exact live state ──
     * tip_finalize rowless span [S..C-1] + anchor ok=1 at C; script+proof
     * rowless at C == coins_applied; utxo rows through C-1; cursors at
     * C+2001. FIX-2a clamps script/proof; after a simulated refill at C the
     * backfill heals the span to C-1 (boundary written with both binders),
     * H* converges to C-1, the tear clears arithmetically, and the existing
     * reconcile clamps tip_finalize to C. S = A+3, C = A+8. */
    {
        const int C = A + 8;
        char dir[256];
        TIPFIN_CHECK("T9 setup",
                     open_fixture(dir, sizeof(dir), "t9", C + 2001));
        sqlite3 *db = progress_store_db();

        struct uint256 h[10];
        for (int i = 1; i <= 9; i++)
            mk_hash(&h[i], A + i);

        bool seeded = true;
        for (int i = 1; i <= 7; i++)  /* A+1 .. C-1: all five logs ok=1 */
            seeded = seeded && put_upstream_ok(db, A + i, &h[i]);
        /* At C: validate + body_persist present, script/proof/utxo rowless. */
        seeded = seeded && put_validate(db, C, 1, &h[8]) &&
                 put_simple(db, "body_persist_log", C, 1);
        /* tip_finalize: rows through S-1, rowless span, anchor ok=1 at C. */
        seeded = seeded &&
                 put_tip(db, A + 1, "finalized", 1, &h[2]) &&
                 put_tip(db, A + 2, "finalized", 1, &h[3]) &&
                 put_tip(db, C, "anchor", 1, &h[8]) &&
                 seed_coins_applied(db, C);
        TIPFIN_CHECK("T9 seed", seeded);

        struct stage_reducer_frontier_reconcile_result rr;
        TIPFIN_CHECK("T9 snapshot matches the live pin",
                     snapshot(db, &rr) && rr.hstar == A + 2 &&
                     rr.served_floor == C &&
                     rr.coins_applied_height == C && rr.refused_coin_tear);

        /* Step 1 — FIX-2a clamps script/proof to the unapplied hole at C. */
        bool handled = false;
        TIPFIN_CHECK("T9 FIX-2a clamps script/proof to C",
                     stage_reducer_frontier_try_unapplied_hole_clamp(
                         db, true, &rr, &handled) &&
                     cursor_value(db, "script_validate") == C &&
                     cursor_value(db, "proof_validate") == C);

        /* Step 2 — boundary ordering: before the refill at C, the backfill
         * stops at C-2 (G5 needs the script binder at C). */
        TIPFIN_CHECK("T9 snapshot pre-refill",
                     snapshot(db, &rr) && rr.hstar == A + 2 &&
                     rr.refused_coin_tear);
        handled = false;
        TIPFIN_CHECK("T9 pre-refill pass stops below the boundary",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     handled && rr.tipfin_backfill_count == 4 &&
                     rr.tipfin_backfill_height == C - 2);

        /* Step 3 — simulated forward script/proof refill at C. */
        TIPFIN_CHECK("T9 simulated refill at C",
                     put_script(db, C, 1, &h[8]) &&
                     put_simple(db, "proof_validate_log", C, 1));

        /* Step 4 — the boundary p=C-1 is written with both binders. */
        TIPFIN_CHECK("T9 snapshot at boundary",
                     snapshot(db, &rr) && rr.hstar == C - 2 &&
                     rr.refused_coin_tear);
        handled = false;
        TIPFIN_CHECK("T9 boundary backfilled after refill",
                     stage_reducer_frontier_try_tipfin_backfill(
                         db, true, &rr, &handled) &&
                     handled && rr.tipfin_backfill_count == 1 &&
                     rr.tipfin_backfill_height == C - 1);
        struct tip_row_view row;
        TIPFIN_CHECK("T9 boundary row carries the dual binder hash",
                     tip_row_at(db, C - 1, &row) && row.found &&
                     row.ok == 1 &&
                     strcmp(row.status, "finalize_backfill") == 0 &&
                     row.has_hash &&
                     memcmp(row.hash.data, h[8].data, 32) == 0);
        bool found = false;
        int32_t wlast = -1;
        uint32_t wtotal = 0;
        TIPFIN_CHECK("T9 witness deleted when the span exhausts",
                     witness_at(db, &found, &wlast, &wtotal) && !found);

        /* Step 5 — H* converges to C-1; the tear clears ARITHMETICALLY. */
        TIPFIN_CHECK("T9 hstar converges, tear clears",
                     snapshot(db, &rr) && rr.hstar == C - 1 &&
                     !rr.refused_coin_tear);

        /* Step 6 — the existing reconcile clamps tip_finalize to C. */
        struct main_state ms;
        main_state_init(&ms);
        struct stage_reducer_frontier_reconcile_result fin;
        TIPFIN_CHECK("T9 reconcile clamps tip_finalize to C",
                     stage_reducer_frontier_reconcile_light(db, &ms, &fin) &&
                     !fin.refused_coin_tear &&
                     cursor_value(db, "tip_finalize") == C);
        main_state_free(&ms);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    printf("stage_repair_tipfin_backfill: %d failures\n", failures);
    return failures;
}
