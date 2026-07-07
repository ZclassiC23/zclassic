/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_sticky_escalator — the targeted_rederive rung's real curative surface.
 *
 * The rung calls the reducer-frontier reconcile APPLY pass
 * (stage_reducer_frontier_reconcile_light) directly — the same entry the
 * reducer_frontier_reconcile_light Condition's remedy uses, WITHOUT the
 * Condition's peer gate (connman_max_peer_height reads static handshake
 * starting_height, so near tip it reads "no peer ahead" forever and the
 * recomputed repair is discarded — the 2026-07-02 stall at H*=3166988 with a
 * rowless script/proof hole at 3166989). Proven here through the REAL ladder
 * (note_stall -> retry -> targeted_rederive) over a synthetic progress.kv:
 *
 *   T1 — actionable rowless script+proof hole at coins_applied: the rung
 *        reports repaired, clamps script/proof cursors to the hole and
 *        tip_finalize to H*, deletes no log rows, and HOLDS the rung
 *        (PROGRESSING + repair-hold memo) so the ladder does not cascade
 *        into the reindex rung while the stages consume the clamp.
 *   T2 — fully-consistent store: the rung reports no-op (repaired=0) and the
 *        ladder honestly advances to the next rung, zero cursor writes.
 *
 * Fixture shape mirrors test_stage_repair_script_refill.c Part A (synthetic
 * rows at the mainnet trusted anchor A). */

#include "platform/time_compat.h"
#include "test/test_helpers.h"

#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "services/sticky_escalator.h"
#include "services/sync_monitor.h"
#include "storage/boot_auto_reindex.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SE_CHECK(name, expr) do { \
    printf("sticky_escalator: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

#define A REDUCER_FRONTIER_TRUSTED_ANCHOR

/* The compiled-anchor floor is network-derived; this fixture seeds rows at
 * MAINNET heights (A+1..) so the floor must be pinned to the mainnet anchor A
 * for compute_hstar (the test_stage_repair_script_refill.c pattern). Restored
 * to -1 (production default) before return. */
void reducer_frontier_test_set_compiled_anchor(int32_t height);

/* ── Fixture: synthetic progress.kv at the mainnet trusted anchor ───────── */

struct se_fixture {
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
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

static bool delete_height(sqlite3 *db, const char *table, int height)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE height=?", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    if (!ok) return false;
    /* Stamp full coins_kv proven-authority so compute_hstar honors the baked
     * TRUSTED_ANCHOR floor (the test_stage_repair_script_refill.c fixture
     * models a seeded datadir whose H* clamps at the anchor). */
    char *err = NULL;
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS coins(k BLOB PRIMARY KEY, v BLOB);"
            "INSERT OR IGNORE INTO coins(k,v) VALUES(x'00', x'00');",
            NULL, NULL, &err) != SQLITE_OK) {  // raw-sql-ok:test-seed
        sqlite3_free(err);
        return false;
    }
    uint8_t one = 1;
    st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, "coins_kv_migration_complete", -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, &one, 1, SQLITE_STATIC);
    ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
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
        if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:test-readback
            value = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

/* Total rows across every reducer log — the "no log row is ever deleted"
 * invariant witness. */
static int64_t total_log_rows(sqlite3 *db)
{
    static const char *const tables[] = {
        "header_admit_log", "validate_headers_log", "body_fetch_log",
        "body_persist_log", "script_validate_log", "proof_validate_log",
        "utxo_apply_log", "tip_finalize_log",
    };
    int64_t total = 0;
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", tables[i]);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            return -1;
        if (sqlite3_step(st) != SQLITE_ROW) {  // raw-sql-ok:test-readback
            sqlite3_finalize(st);
            return -1;
        }
        total += sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return total;
}

static struct block_index *insert_index(struct main_state *ms,
                                        struct uint256 *hash,
                                        int height,
                                        struct block_index *prev)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(height & 0xff);
    hash->data[1] = (uint8_t)((height >> 8) & 0xff);
    hash->data[2] = (uint8_t)((height >> 16) & 0xff);
    hash->data[31] = 0x5e;

    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!bi)
        return NULL;
    bi->nHeight = height;
    bi->pprev = prev;
    /* VALID_SCRIPTS and no HAVE_DATA: the block-index flag reconcile pass
     * has nothing to set or clear, so cursor assertions stay isolated. */
    bi->nStatus = BLOCK_VALID_SCRIPTS;
    bi->nFile = -1;
    bi->nDataPos = 0;
    bi->nTx = 1;
    bi->nChainTx = prev ? prev->nChainTx + 1 : 1;
    arith_uint256_set_u64(&bi->nChainWork, (uint64_t)(height - A + 1));
    return bi;
}

static bool setup_fixture(struct se_fixture *fx, const char *tag)
{
    memset(fx, 0, sizeof(*fx));
    test_make_tmpdir(fx->dir, sizeof(fx->dir), "sticky_escalator", tag);
    if (!progress_store_open(fx->dir))
        return false;
    sqlite3 *db = progress_store_db();
    if (!seed_schema(db))
        return false;
    if (!seed_all_cursors(db, A + 4))
        return false;

    main_state_init(&fx->ms);
    fx->idx[1] = insert_index(&fx->ms, &fx->hashes[1], A + 1, NULL);
    fx->idx[2] = insert_index(&fx->ms, &fx->hashes[2], A + 2, fx->idx[1]);
    fx->idx[3] = insert_index(&fx->ms, &fx->hashes[3], A + 3, fx->idx[2]);
    if (!fx->idx[1] || !fx->idx[2] || !fx->idx[3])
        return false;

    if (!put_header_admit(db, A + 1, &fx->hashes[1], NULL) ||
        !put_header_admit(db, A + 2, &fx->hashes[2], &fx->hashes[1]) ||
        !put_header_admit(db, A + 3, &fx->hashes[3], &fx->hashes[2]))
        return false;

    for (int i = 1; i <= 3; i++) {
        if (!put_upstream_ok(db, A + i, &fx->hashes[i]) ||
            !put_body_fetch_ok(db, A + i, &fx->hashes[i]))
            return false;
    }
    if (!put_tip_log(db, A + 1, 1, &fx->hashes[1]))
        return false;
    if (!seed_coins_applied(db, A + 2))
        return false;
    return true;
}

static void teardown_fixture(struct se_fixture *fx)
{
    sync_monitor_set_context(NULL, NULL, NULL);
    sticky_escalator_test_reset();
    stage_reducer_frontier_reset_detect_memo_for_testing();
    main_state_free(&fx->ms);
    progress_store_close();
    test_cleanup_tmpdir(fx->dir);
}

int test_sticky_escalator(void);
int test_sticky_escalator(void)
{
    printf("\n=== sticky_escalator tests ===\n");
    int failures = 0;

    blocker_module_init();
    /* Pin the network-derived compiled-anchor floor to the mainnet anchor A:
     * the fixtures seed rows at A+1.. (see the refill test's rationale). */
    reducer_frontier_test_set_compiled_anchor(A);

    /* T1 — actionable rowless script+proof hole at coins_applied: driving the
     * real ladder into the targeted_rederive rung applies the reconcile clamp
     * (no peer gate) and holds the rung while the stages would consume it. */
    {
        struct se_fixture fx;
        SE_CHECK("T1: setup fixture", setup_fixture(&fx, "t1_hole"));
        sqlite3 *db = progress_store_db();

        /* The live 3166989 shape: purge left script/proof rowless at the
         * coins frontier while their cursors sit above it. */
        SE_CHECK("T1: punch rowless hole at h0 = coins_applied",
                 delete_height(db, "script_validate_log", A + 2) &&
                 delete_height(db, "proof_validate_log", A + 2) &&
                 delete_height(db, "utxo_apply_log", A + 2) &&
                 delete_height(db, "utxo_apply_log", A + 3) &&
                 seed_cursor(db, "utxo_apply", A + 2));

        int64_t rows_before = total_log_rows(db);

        sync_monitor_set_context(NULL, NULL, &fx.ms);
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();

        sticky_escalator_note_stall("test_rowless_hole");
        SE_CHECK("T1: ladder armed by note_stall",
                 sticky_escalator_test_armed());

        /* note_stall stamps the rung-entered clock with the REAL wall time,
         * so the injected drive times are anchored on it. Injected tip 0
         * never satisfies the progress margin (entry is -1 or a real H*). */
        int64_t t0 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T1: retry rung holds within its window",
                 sticky_escalator_test_drive(0, t0 + 1) == STICKY_RUNG_RETRY);
        SE_CHECK("T1: retry window lapse advances to targeted_rederive",
                 sticky_escalator_test_drive(0, t0 + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T1: targeted_rederive applies the reconcile and holds",
                 sticky_escalator_test_drive(0, t0 + 32) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T1: script/proof cursors clamped to the hole, tip to hstar",
                 cursor_value(db, "script_validate") == A + 2 &&
                 cursor_value(db, "proof_validate") == A + 2 &&
                 cursor_value(db, "tip_finalize") == A + 1);
        SE_CHECK("T1: utxo cursor untouched + no log rows deleted",
                 cursor_value(db, "utxo_apply") == A + 2 &&
                 total_log_rows(db) == rows_before);
        /* The next pass finds nothing NEW actionable (cursors already at the
         * hole); the repair-hold memo keeps the rung instead of cascading
         * into the reindex rung on the very next tick. */
        SE_CHECK("T1: repair-hold memo keeps the rung after the clamp",
                 sticky_escalator_test_drive(0, t0 + 40) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T1: ladder still armed (episode clears only on H* climb)",
                 sticky_escalator_test_armed());

        teardown_fixture(&fx);
    }

    /* T2 — fully-consistent store: nothing actionable. The rung reports the
     * honest no-op and the ladder advances to the next rung, zero writes. */
    {
        struct se_fixture fx;
        SE_CHECK("T2: setup fixture", setup_fixture(&fx, "t2_clean"));
        sqlite3 *db = progress_store_db();

        /* Complete the tip column, move coins one above H*, and park the
         * OWN-frame tip_finalize cursor AT H* (A+3, the served tip) so every
         * cursor already equals its reconcile target: no clamp, no purge, no
         * backfill — the honest no-op shape. */
        SE_CHECK("T2: complete tip rows + coins frontier + served tip cursor",
                 put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                 put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                 seed_coins_applied(db, A + 4) &&
                 seed_cursor(db, "tip_finalize", A + 3));

        int64_t rows_before = total_log_rows(db);

        sync_monitor_set_context(NULL, NULL, &fx.ms);
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();

        sticky_escalator_note_stall("test_clean_store");
        int64_t t1 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T2: retry rung holds within its window",
                 sticky_escalator_test_drive(0, t1 + 1) == STICKY_RUNG_RETRY);
        SE_CHECK("T2: retry window lapse advances to targeted_rederive",
                 sticky_escalator_test_drive(0, t1 + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T2: no-op rederive honestly advances the ladder",
                 sticky_escalator_test_drive(0, t1 + 32) ==
                     STICKY_RUNG_RESNAPSHOT);
        SE_CHECK("T2: zero cursor writes + no log rows deleted",
                 cursor_value(db, "script_validate") == A + 4 &&
                 cursor_value(db, "proof_validate") == A + 4 &&
                 cursor_value(db, "utxo_apply") == A + 4 &&
                 cursor_value(db, "tip_finalize") == A + 3 &&
                 total_log_rows(db) == rows_before);

        teardown_fixture(&fx);
    }

    /* T3 — cold-import window: active-chain state exists, but genesis-side
     * block data is not readable from this datadir. Runtime escalation must
     * NOT arm auto_reindex_request, because the next boot would only consume
     * and refuse the impossible replay-from-blocks verb. */
    {
        struct se_fixture fx;
        SE_CHECK("T3: setup fixture", setup_fixture(&fx, "t3_cold_import"));
        sqlite3 *db = progress_store_db();

        SE_CHECK("T3: make rederive rung an honest no-op",
                 put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                 put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                 seed_coins_applied(db, A + 4) &&
                 seed_cursor(db, "tip_finalize", A + 3));

        sync_monitor_set_context(NULL, NULL, &fx.ms);
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();
        sticky_escalator_set_datadir(fx.dir);

        sticky_escalator_note_stall("test_cold_import_window");
        int64_t t2 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T3: retry window advances to targeted_rederive",
                 sticky_escalator_test_drive(0, t2 + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T3: no-op rederive advances to resnapshot",
                 sticky_escalator_test_drive(0, t2 + 32) ==
                     STICKY_RUNG_RESNAPSHOT);
        SE_CHECK("T3: resnapshot stub advances to reindex",
                 sticky_escalator_test_drive(0, t2 + 33) ==
                     STICKY_RUNG_REINDEX);
        SE_CHECK("T3: unexecutable reindex escalates deeper",
                 sticky_escalator_test_drive(0, t2 + 34) ==
                     STICKY_RUNG_SELF_MINT_REFOLD);
        SE_CHECK("T3: no auto-reindex marker written",
                 !boot_auto_reindex_pending(fx.dir));

        sticky_escalator_set_datadir(NULL);
        teardown_fixture(&fx);
    }

    reducer_frontier_test_set_compiled_anchor(-1); /* restore production floor */

    printf("sticky_escalator: %d failures\n", failures);
    return failures;
}
