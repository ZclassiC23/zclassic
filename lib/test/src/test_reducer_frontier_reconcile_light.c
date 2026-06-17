/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "conditions/reducer_frontier_reconcile_light.h"
#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "net/net.h"
#include "services/sync_monitor.h"
#include "storage/progress_store.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RFRL_CHECK(name, expr) do { \
    printf("reducer_frontier_reconcile_light: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

#define A REDUCER_FRONTIER_TRUSTED_ANCHOR

struct rfrl_fixture {
    char dir[256];
    struct main_state ms;
    struct uint256 hashes[4];
    struct block_index *idx[4];
};

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

static bool seed_schema(sqlite3 *db)
{
    return
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS header_admit_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL,"
            "parent_hash BLOB, admitted_at INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS validate_headers_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL,"
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
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
            "tip_hash BLOB)");
}

static bool put_body_fetch_ok(sqlite3 *db, int height,
                              const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO body_fetch_log"
            "(height,hash,source,bytes,fetched_at,ok,fail_reason) "
            "VALUES(?,?,'disk',0,1,1,NULL)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_blob(st, 2, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_header_admit(sqlite3 *db, int height,
                             const struct uint256 *hash,
                             const struct uint256 *parent_hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO header_admit_log"
            "(height,hash,parent_hash,admitted_at) VALUES(?,?,?,1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_blob(st, 2, hash->data, 32, SQLITE_STATIC);
    if (parent_hash)
        sqlite3_bind_blob(st, 3, parent_hash->data, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 3);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
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

static bool put_hash_log(sqlite3 *db, const char *table, const char *hash_col,
                         int height, int ok_flag, const struct uint256 *hash)
{
    char sql[192];
    if (strcmp(table, "validate_headers_log") == 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,ok,%s) VALUES(?,?,?)",
                 table, hash_col);
    } else {
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO %s(height,status,ok,%s) "
                 "VALUES(?,'verified',?,?)",
                 table, hash_col);
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    sqlite3_bind_blob(st, 3, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_script_status(sqlite3 *db, int height, int ok_flag,
                              const char *status,
                              const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO script_validate_log"
            "(height,status,ok,block_hash) VALUES(?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_text(st, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, ok_flag);
    sqlite3_bind_blob(st, 4, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_simple_log(sqlite3 *db, const char *table, int height,
                           int ok_flag)
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

static bool delete_height(sqlite3 *db, const char *table, int height)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE height=?", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_tip_log(sqlite3 *db, int height, int ok_flag,
                        const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO tip_finalize_log"
            "(height,status,ok,tip_hash) VALUES(?,'finalized',?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    sqlite3_bind_blob(st, 3, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_upstream_ok(sqlite3 *db, int height,
                            const struct uint256 *hash)
{
    return put_hash_log(db, "validate_headers_log", "hash", height, 1, hash) &&
           put_hash_log(db, "script_validate_log", "block_hash", height, 1,
                        hash) &&
           put_simple_log(db, "body_persist_log", height, 1) &&
           put_simple_log(db, "proof_validate_log", height, 1) &&
           put_simple_log(db, "utxo_apply_log", height, 1);
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

static struct block_index *insert_index(struct main_state *ms,
                                        struct uint256 *hash,
                                        int height,
                                        struct block_index *prev,
                                        unsigned status)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(height & 0xff);
    hash->data[1] = (uint8_t)((height >> 8) & 0xff);
    hash->data[2] = (uint8_t)((height >> 16) & 0xff);
    hash->data[31] = 0x7b;

    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!bi)
        return NULL;
    bi->nHeight = height;
    bi->pprev = prev;
    bi->nStatus = status;
    bi->nFile = -1;
    bi->nDataPos = 0;
    bi->nTx = 1;
    bi->nChainTx = prev ? prev->nChainTx + 1 : 1;
    arith_uint256_set_u64(&bi->nChainWork, (uint64_t)(height - A + 1));
    return bi;
}

static bool setup_fixture(struct rfrl_fixture *fx, const char *tag)
{
    memset(fx, 0, sizeof(*fx));
    test_make_tmpdir(fx->dir, sizeof(fx->dir),
                     "reducer_frontier_reconcile_light", tag);
    if (!progress_store_open(fx->dir))
        return false;
    /* progress.kv is closed+reopened per fixture; drop the dry-run detect memo
     * so a reused db pointer + reset total_changes cannot wrongly hit a prior
     * fixture's cached result (production never reopens, so this is test-only). */
    stage_reducer_frontier_reset_detect_memo_for_testing();
    if (!seed_schema(progress_store_db()))
        return false;
    if (!seed_all_cursors(progress_store_db(), A + 4))
        return false;

    main_state_init(&fx->ms);
    fx->idx[1] = insert_index(&fx->ms, &fx->hashes[1], A + 1, NULL,
                              BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
    fx->idx[2] = insert_index(&fx->ms, &fx->hashes[2], A + 2,
                              fx->idx[1], BLOCK_HAVE_DATA);
    fx->idx[3] = insert_index(&fx->ms, &fx->hashes[3], A + 3,
                              fx->idx[2],
                              BLOCK_VALID_TREE | BLOCK_HAVE_DATA |
                              BLOCK_FAILED_VALID);
    if (!fx->idx[1] || !fx->idx[2] || !fx->idx[3])
        return false;

    if (!put_header_admit(progress_store_db(), A + 1, &fx->hashes[1], NULL) ||
        !put_header_admit(progress_store_db(), A + 2, &fx->hashes[2],
                          &fx->hashes[1]) ||
        !put_header_admit(progress_store_db(), A + 3, &fx->hashes[3],
                          &fx->hashes[2]))
        return false;

    if (!put_upstream_ok(progress_store_db(), A + 1, &fx->hashes[1]) ||
        !put_upstream_ok(progress_store_db(), A + 2, &fx->hashes[2]) ||
        !put_upstream_ok(progress_store_db(), A + 3, &fx->hashes[3]))
        return false;
    if (!put_tip_log(progress_store_db(), A + 1, 1, &fx->hashes[1]))
        return false;
    if (!seed_coins_applied(progress_store_db(), A + 2))
        return false;
    return true;
}

static void teardown_fixture(struct rfrl_fixture *fx)
{
    main_state_free(&fx->ms);
    progress_store_close();
    test_cleanup_tmpdir(fx->dir);
}

/* Cross-thread lock probe: acquires + releases the progress lock from a
 * second thread. A refusal path that leaks the (recursive) lock is invisible
 * to the calling thread; the probe's join hangs the test binary instead. */
static void *progress_lock_probe(void *arg)
{
    progress_store_tx_lock();
    progress_store_tx_unlock();
    *(bool *)arg = true;
    return NULL;
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

int test_reducer_frontier_reconcile_light(void);
int test_reducer_frontier_reconcile_light(void)
{
    test_reset_shared_globals();   /* monolith isolation: see test_helpers.c */
    printf("\n=== reducer_frontier_reconcile_light tests ===\n");
    int failures = 0;

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup dry-run/apply fixture",
                   setup_fixture(&fx, "apply"));

        sqlite3 *db = progress_store_db();
        unsigned before2 = fx.idx[2]->nStatus;
        unsigned before3 = fx.idx[3]->nStatus;

        struct stage_reducer_frontier_reconcile_result dry;
        RFRL_CHECK("dry-run succeeds",
                   stage_reducer_frontier_reconcile_light_needed(
                       db, &fx.ms, &dry));
        RFRL_CHECK("dry-run reports repair",
                   dry.repaired && dry.hstar == A + 1 &&
                   dry.sweep_top == A + 3 &&
                   dry.lowest_have_data_cleared == A + 2 &&
                   dry.lowest_validate_headers_refill_hole == -1 &&
                   dry.lowest_body_fetch_refill_hole == A + 2 &&
                   dry.lowest_body_persist_refill_hole == -1 &&
                   dry.validate_headers_cursor_before == A + 4 &&
                   dry.validate_headers_cursor_after == A + 4 &&
                   dry.body_fetch_cursor_before == A + 4 &&
                   dry.body_fetch_cursor_after == A + 2 &&
                   dry.clamped_body_fetch &&
                   !dry.clamped_body_persist &&
                   /* OWN-frame (task #31): clamp band [hstar, hstar+1]
                    * capped at coins_applied-1 = min(A+2, A+1) = A+1 —
                    * the served tip, no longer the next transition. */
                   dry.tip_finalize_cursor_after == A + 1);
        RFRL_CHECK("dry-run does not mutate",
                   fx.idx[2]->nStatus == before2 &&
                   fx.idx[3]->nStatus == before3 &&
                   cursor_value(db, "validate_headers") == A + 4 &&
                   cursor_value(db, "body_fetch") == A + 4 &&
                   cursor_value(db, "body_persist") == A + 4 &&
                   cursor_value(db, "tip_finalize") == A + 4);

        struct stage_reducer_frontier_reconcile_result applied;
        RFRL_CHECK("apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &applied));
        RFRL_CHECK("apply clamps body_fetch and tip_finalize",
                   cursor_value(db, "tip_finalize") == A + 1 &&
                   cursor_value(db, "validate_headers") == A + 4 &&
                   cursor_value(db, "body_fetch") == A + 2 &&
                   cursor_value(db, "body_persist") == A + 4 &&
                   cursor_value(db, "utxo_apply") == A + 4 &&
                   applied.clamped_body_fetch &&
                   !applied.clamped_body_persist &&
                   applied.clamped_tip_finalize);
        RFRL_CHECK("script bits restored",
                   (fx.idx[2]->nStatus & BLOCK_VALID_MASK) ==
                       BLOCK_VALID_SCRIPTS &&
                   (fx.idx[3]->nStatus & BLOCK_VALID_MASK) ==
                       BLOCK_VALID_SCRIPTS &&
                   applied.scripts_set == 2);
        RFRL_CHECK("unreadable HAVE_DATA cleared",
                   (fx.idx[2]->nStatus & BLOCK_HAVE_DATA) == 0 &&
                   (fx.idx[3]->nStatus & BLOCK_HAVE_DATA) == 0 &&
                   applied.have_data_cleared == 2);
        RFRL_CHECK("proved stale failure mask cleared",
                   (fx.idx[3]->nStatus & BLOCK_FAILED_MASK) == 0 &&
                   applied.failed_mask_cleared == 1);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup validate-refill-hole fixture",
                   setup_fixture(&fx, "validate_refill_hole"));
        sqlite3 *db = progress_store_db();

        fx.idx[2]->nStatus = BLOCK_VALID_SCRIPTS;
        fx.idx[3]->nStatus = BLOCK_VALID_SCRIPTS;
        RFRL_CHECK("validate-refill-hole: seed later body_fetch row",
                   put_body_fetch_ok(db, A + 3, &fx.hashes[3]));
        RFRL_CHECK("validate-refill-hole: delete reducer rows at hole",
                   delete_height(db, "validate_headers_log", A + 2) &&
                   delete_height(db, "body_fetch_log", A + 2) &&
                   delete_height(db, "body_persist_log", A + 2) &&
                   delete_height(db, "script_validate_log", A + 2) &&
                   delete_height(db, "proof_validate_log", A + 2) &&
                   delete_height(db, "utxo_apply_log", A + 2));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("validate-refill-hole: apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("validate-refill-hole: clamps upstream refill cursors",
                   rr.lowest_validate_headers_refill_hole == A + 2 &&
                   rr.lowest_body_fetch_refill_hole == -1 &&
                   rr.lowest_body_persist_refill_hole == -1 &&
                   rr.clamped_validate_headers &&
                   rr.clamped_body_fetch &&
                   rr.clamped_body_persist &&
                   cursor_value(db, "validate_headers") == A + 2 &&
                   cursor_value(db, "body_fetch") == A + 2 &&
                   cursor_value(db, "body_persist") == A + 2 &&
                   /* tip_finalize is OWN-frame: served tip = A+1 (coins
                    * applied through A+1), one below the refill cursors. */
                   cursor_value(db, "tip_finalize") == A + 1);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup validate-hash-split fixture",
                   setup_fixture(&fx, "validate_hash_split"));
        sqlite3 *db = progress_store_db();

        struct uint256 stale = fx.hashes[2];
        stale.data[0] ^= 0x5a;
        RFRL_CHECK("validate-hash-split: seed stale validate hash",
                   put_hash_log(db, "validate_headers_log", "hash",
                                A + 2, 1, &stale));
        RFRL_CHECK("validate-hash-split: seed coins above split hstar",
                   seed_coins_applied(db, A + 3));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("validate-hash-split: apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        /* New coin-tear semantics: a stale validate hash with utxo_apply SOLID
         * is NOT a coin tear (coins track utxo_apply's own log, not the
         * hash-split-pinned H*). The split still caps H* and must be healed,
         * but now via the downstream refill rather than the dead tear-gated
         * pre-refusal clamp — it re-walks validate_headers AND its dependent
         * cursors (body_fetch, tip_finalize) back to the split height A+2 to
         * re-derive the column; body_persist already holds its rows so it is
         * not clamped. No coin-tear refusal is involved. */
        RFRL_CHECK("validate-hash-split: downstream refill heals the split",
                   rr.repaired &&
                   !rr.refused_coin_tear &&
                   rr.lowest_validate_headers_hash_split == A + 2 &&
                   rr.lowest_validate_headers_refill_hole == -1 &&
                   rr.clamped_validate_headers &&
                   cursor_value(db, "validate_headers") == A + 2 &&
                   cursor_value(db, "body_fetch") == A + 2 &&
                   cursor_value(db, "body_persist") == A + 4 &&
                   cursor_value(db, "tip_finalize") == A + 2);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup body-refill-hole fixture",
                   setup_fixture(&fx, "body_refill_hole"));
        sqlite3 *db = progress_store_db();

        fx.idx[2]->nStatus = BLOCK_VALID_SCRIPTS;
        fx.idx[3]->nStatus = BLOCK_VALID_SCRIPTS;
        RFRL_CHECK("body-refill-hole: seed body_fetch rows around hole",
                   put_body_fetch_ok(db, A + 1, &fx.hashes[1]) &&
                   put_body_fetch_ok(db, A + 3, &fx.hashes[3]));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("body-refill-hole: apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("body-refill-hole: clamps to missing body_fetch row",
                   rr.lowest_have_data_cleared == -1 &&
                   rr.lowest_validate_headers_refill_hole == -1 &&
                   rr.lowest_body_fetch_refill_hole == A + 2 &&
                   rr.lowest_body_persist_refill_hole == -1 &&
                   rr.clamped_body_fetch &&
                   !rr.clamped_body_persist &&
                   cursor_value(db, "body_fetch") == A + 2);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup coin-tear fixture",
                   setup_fixture(&fx, "cointear"));
        sqlite3 *db = progress_store_db();
        /* REAL coin tear: a HOLE in utxo_apply's OWN ok=1 log below the coins
         * frontier. Mark utxo_apply ok=0 at A+2 (status='verified', so neither
         * the value_overflow nor stale_script replays — which key on
         * status='value_overflow'/'internal_error' in script_validate — engage)
         * so utxo_apply's contiguous prefix stops at A+1, then seed coins above
         * it at A+3. coins_applied(A+3) > utxo_apply_contig(A+1)+1 is a genuine
         * tear: coins applied above utxo_apply's own solid log. (Pre-fix this
         * test relied on tip_finalize lagging at A+1 to push coins past the
         * global MIN H* — a FALSE positive the new ua_contig compare ignores.)
         * tipfin_backfill's G3 refuses on the ok=0 utxo_apply row, so the tear
         * survives every pre-refusal repair and the L1 refusal stands. */
        RFRL_CHECK("seed real utxo_apply hole below coins frontier",
                   put_simple_log(db, "utxo_apply_log", A + 2, 0) &&
                   seed_coins_applied(db, A + 3));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("coin-tear detect call succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("coin-tear refused without mutation",
                   rr.refused_coin_tear &&
                   cursor_value(db, "tip_finalize") == A + 4 &&
                   (fx.idx[2]->nStatus & BLOCK_VALID_MASK) == 0);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup served-floor fixture",
                   setup_fixture(&fx, "served_floor"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("seed served floor above hstar without contiguous prefix",
                   put_tip_log(db, A + 3, 1, &fx.hashes[3]));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("served-floor apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("served-floor cannot override coins cap",
                   rr.hstar == A + 1 &&
                   rr.served_floor == A + 3 &&
                   rr.coins_applied_height == A + 2 &&
                   /* OWN-frame: served tip capped at coins applied-through
                    * (coins_applied A+2 is NEXT-frame => through A+1). */
                   rr.tip_finalize_cursor_after == A + 1 &&
                   cursor_value(db, "tip_finalize") == A + 1 &&
                   rr.clamped_tip_finalize);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup coin-lag fixture",
                   setup_fixture(&fx, "coin_lag"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("seed contiguous hstar above coins_applied",
                   put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                   put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                   seed_coins_applied(db, A + 3));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("coin-lag apply succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("coin-lag caps tip_finalize at coins applied-through",
                   rr.hstar == A + 3 &&
                   rr.coins_applied_height == A + 3 &&
                   /* OWN-frame: hstar allows served A+3..A+4 but coins
                    * (NEXT-frame A+3 => applied through A+2) cap the
                    * served-tip claim at A+2. */
                   rr.tip_finalize_cursor_after == A + 2 &&
                   cursor_value(db, "tip_finalize") == A + 2 &&
                   rr.clamped_tip_finalize);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup unknown-coin fixture",
                   setup_fixture(&fx, "unknown"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("delete coins_applied frontier",
                   exec_sql(db, "DELETE FROM progress_meta "
                                "WHERE key='coins_applied_height'"));

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("unknown-coin call succeeds",
                   stage_reducer_frontier_reconcile_light(
                       db, &fx.ms, &rr));
        RFRL_CHECK("unknown-coin refused without mutation",
                   rr.refused_coin_unknown &&
                   cursor_value(db, "tip_finalize") == A + 4 &&
                   (fx.idx[2]->nStatus & BLOCK_VALID_MASK) == 0);

        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup zero-peer condition fixture",
                   setup_fixture(&fx, "zero_peer_condition"));

        struct connman cm;
        memset(&cm, 0, sizeof(cm));
        net_manager_init(&cm.manager);

        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        sync_monitor_init();
        sync_monitor_set_context(&cm, NULL, &fx.ms);
        sync_monitor_test_set_tip_advance_ts(1);
        register_reducer_frontier_reconcile_light();

        condition_engine_tick();

        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(
            "reducer_frontier_reconcile_light", &snap);
        sqlite3 *db = progress_store_db();
        bool ok = got &&
                  reducer_frontier_reconcile_light_test_remedy_calls() == 1 &&
                  cursor_value(db, "body_fetch") == A + 2 &&
                  cursor_value(db, "tip_finalize") == A + 1 &&
                  !snap.currently_active &&
                  snap.attempts == 0 &&
                  snap.last_outcome == COND_REMEDY_SKIP &&
                  snap.cleared_count == 1 &&
                  !snap.operator_needed_emitted;

        condition_engine_tick();
        got = condition_engine_get_registered_snapshot(
            "reducer_frontier_reconcile_light", &snap);
        ok = ok && got &&
             reducer_frontier_reconcile_light_test_remedy_calls() == 1 &&
             !snap.currently_active &&
             snap.attempts == 0 &&
             snap.cleared_count == 1 &&
             !snap.operator_needed_emitted;
        RFRL_CHECK("zero-peer condition witnesses cursor repair", ok);

        sync_monitor_set_context(NULL, NULL, NULL);
        sync_monitor_test_set_tip_advance_ts(0);
        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        net_manager_free(&cm.manager);
        teardown_fixture(&fx);
    }

    {
        struct rfrl_fixture fx;
        RFRL_CHECK("setup coin-tear condition fixture",
                   setup_fixture(&fx, "cointear_condition"));
        sqlite3 *db = progress_store_db();
        /* REAL coin tear (same shape as the detect-path coin-tear case above):
         * a utxo_apply ok=0 hole at A+2 caps utxo_apply's own contiguous prefix
         * at A+1 while coins_applied sits at A+3, so
         * coins_applied > utxo_apply_contig+1 holds. The earlier seed leaned on
         * tip_finalize lagging at A+1 (a FALSE tear the new ua_contig compare
         * no longer escalates); this is a genuine hole below the cursor that
         * survives every pre-refusal repair, so the Condition still escalates
         * to operator_needed without mutating any cursor or block flag. */
        RFRL_CHECK("coin-tear condition: seed real utxo_apply hole",
                   put_simple_log(db, "utxo_apply_log", A + 2, 0) &&
                   seed_coins_applied(db, A + 3));

        struct connman cm;
        memset(&cm, 0, sizeof(cm));
        net_manager_init(&cm.manager);

        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        sync_monitor_init();
        sync_monitor_set_context(&cm, NULL, &fx.ms);
        sync_monitor_test_set_tip_advance_ts(1);
        register_reducer_frontier_reconcile_light();

        for (int i = 0; i < 5; i++) {
            reducer_frontier_reconcile_light_test_clear_backoff();
            condition_engine_tick();
        }

        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(
            "reducer_frontier_reconcile_light", &snap);
        bool ok = got &&
                  reducer_frontier_reconcile_light_test_remedy_calls() == 5 &&
                  snap.currently_active &&
                  snap.attempts >= 5 &&
                  snap.last_outcome == COND_REMEDY_FAILED &&
                  snap.operator_needed_emitted &&
                  cursor_value(db, "tip_finalize") == A + 4 &&
                  cursor_value(db, "body_fetch") == A + 4 &&
                  (fx.idx[2]->nStatus & BLOCK_VALID_MASK) == 0;

        RFRL_CHECK("coin-tear condition escalates without mutation", ok);

        sync_monitor_set_context(NULL, NULL, NULL);
        sync_monitor_test_set_tip_advance_ts(0);
        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        net_manager_free(&cm.manager);
        teardown_fixture(&fx);
    }

    {
        /* Stale-script replay refusal (TOCTOU fix): the hole's preconditions
         * pass but the block is unreadable (fixture nFile == -1), so the
         * replay refuses AFTER the cursor snapshot — on a path that now runs
         * under the progress lock held from snapshot to rewind COMMIT. The
         * probe thread proves every traversed refusal path released it. */
        struct rfrl_fixture fx;
        RFRL_CHECK("setup stale-script fixture",
                   setup_fixture(&fx, "stale_script"));
        sqlite3 *db = progress_store_db();
        RFRL_CHECK("stale-script: seed internal_error hole below cursor",
                   put_script_status(db, A + 2, 0, "internal_error",
                                     &fx.hashes[2]));

        struct stage_reducer_frontier_reconcile_result dry;
        RFRL_CHECK("stale-script: dry-run succeeds",
                   stage_reducer_frontier_reconcile_light_needed(
                       db, &fx.ms, &dry));
        RFRL_CHECK("stale-script: dry-run reports the hole without mutation",
                   dry.repaired &&
                   dry.stale_script_repair_height == A + 2 &&
                   !dry.stale_script_repaired &&
                   cursor_value(db, "script_validate") == A + 4 &&
                   cursor_value(db, "utxo_apply") == A + 4);

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("stale-script: apply succeeds",
                   stage_reducer_frontier_reconcile_light(db, &fx.ms, &rr));
        RFRL_CHECK("stale-script: unreadable block refuses without rewind",
                   rr.stale_script_repair_height == A + 2 &&
                   !rr.stale_script_repaired &&
                   cursor_value(db, "script_validate") == A + 4 &&
                   cursor_value(db, "proof_validate") == A + 4 &&
                   cursor_value(db, "utxo_apply") == A + 4);

        bool probed = false;
        pthread_t probe;
        RFRL_CHECK("stale-script: probe thread starts",
                   pthread_create(&probe, NULL, progress_lock_probe,
                                  &probed) == 0);
        pthread_join(probe, NULL);
        RFRL_CHECK("stale-script: progress lock released on refusal", probed);

        teardown_fixture(&fx);
    }

    /* ── non-canonical residue purge (the 2026-06-10 -2 relabel class) ──
     * Rows recorded for the WRONG block at their height (hash != the
     * canonical active-chain block) must be purged — including the false
     * ok=0 bad-cb-height verdicts no other repair touches — while a
     * GENUINE consensus reject (ok=0 with the canonical hash) survives. */
    {
        struct rfrl_fixture fx;
        RFRL_CHECK("noncanon: setup fixture", setup_fixture(&fx, "noncanon"));
        sqlite3 *db = progress_store_db();

        RFRL_CHECK("noncanon: active chain installs",
                   active_chain_move_window_tip(&fx.ms.chain_active,
                                                fx.idx[3]));

        /* Stale row at A+2: recorded with A+3's hash (a relabel wrote the
         * wrong block's verdict here). False ok=0, like bad-cb-height. */
        RFRL_CHECK("noncanon: seed stale script row",
                   put_script_status(db, A + 2, 0, "contextual_invalid",
                                     &fx.hashes[3]));
        /* Genuine reject at A+3: ok=0 but hash IS canonical — kept. */
        RFRL_CHECK("noncanon: seed genuine reject",
                   put_script_status(db, A + 3, 0, "contextual_invalid",
                                     &fx.hashes[3]));

        struct stage_reducer_frontier_reconcile_result dry;
        RFRL_CHECK("noncanon: dry-run succeeds",
                   stage_reducer_frontier_reconcile_light_needed(
                       db, &fx.ms, &dry));
        RFRL_CHECK("noncanon: dry-run finds, does not purge",
                   dry.noncanonical_found >= 1 &&
                   dry.noncanonical_purged == 0 &&
                   dry.lowest_noncanonical == A + 2);

        struct stage_reducer_frontier_reconcile_result rr;
        RFRL_CHECK("noncanon: apply succeeds",
                   stage_reducer_frontier_reconcile_light(db, &fx.ms, &rr));
        RFRL_CHECK("noncanon: apply purges the stale row",
                   rr.noncanonical_purged >= 1 && rr.repaired);

        sqlite3_stmt *st = NULL;
        int stale_left = -1, genuine_left = -1, vh_left = -1;
        if (sqlite3_prepare_v2(db,
                "SELECT "
                " (SELECT COUNT(*) FROM script_validate_log WHERE height=?),"
                " (SELECT COUNT(*) FROM script_validate_log WHERE height=?),"
                " (SELECT COUNT(*) FROM validate_headers_log WHERE height=?)",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, A + 2);
            sqlite3_bind_int(st, 2, A + 3);
            sqlite3_bind_int(st, 3, A + 2);
            if (sqlite3_step(st) == SQLITE_ROW) {
                stale_left = sqlite3_column_int(st, 0);
                genuine_left = sqlite3_column_int(st, 1);
                vh_left = sqlite3_column_int(st, 2);
            }
        }
        sqlite3_finalize(st);
        RFRL_CHECK("noncanon: stale gone, genuine + canonical rows kept",
                   stale_left == 0 && genuine_left == 1 && vh_left == 1);

        int dep_left = -1;
        if (sqlite3_prepare_v2(db,
                "SELECT (SELECT COUNT(*) FROM proof_validate_log "
                "WHERE height=?) + (SELECT COUNT(*) FROM body_persist_log "
                "WHERE height=?)",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, A + 2);
            sqlite3_bind_int(st, 2, A + 2);
            if (sqlite3_step(st) == SQLITE_ROW)
                dep_left = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        RFRL_CHECK("noncanon: hashless downstream rows purged transitively",
                   dep_left == 0);

        teardown_fixture(&fx);
    }

    printf("reducer_frontier_reconcile_light: %d failures\n", failures);
    return failures;
}
