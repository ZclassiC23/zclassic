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
#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "services/sticky_escalator.h"
#include "services/sync_monitor.h"
#include "storage/boot_auto_reindex.h"
#include "storage/boot_auto_refold.h"
#include "storage/progress_store.h"
#include "storage/seal_kv.h"
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
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
            "block_hash BLOB)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_delta ("
            "height INTEGER PRIMARY KEY, branch_hash BLOB NOT NULL,"
            "spent_blob BLOB NOT NULL, added_blob BLOB NOT NULL)") &&
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

static bool put_utxo_log(sqlite3 *db, int height, int ok_flag,
                         const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO utxo_apply_log(height,status,ok) "
            "VALUES(?,'verified',?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_int(st, 2, ok_flag);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    if (!ok)
        return false;

    st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO utxo_apply_delta"
            "(height,branch_hash,spent_blob,added_blob) "
            "VALUES(?,?,x'',x'')",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_blob(st, 2, hash->data, 32, SQLITE_STATIC);
    ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
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
           put_hash_log(db, "proof_validate_log", "block_hash", height, 1,
                        hash) &&
           put_utxo_log(db, height, 1, hash);
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

/* Seed a self-valid RATIFIED seal at grid point g into the fixture's seal ring
 * (mirrors test_seal_kv.c's insert+ratify), so the resnapshot rung's
 * nearest-verified-base probe (seal_kv_newest_ratified) finds a base. */
static bool seed_ratified_seal(sqlite3 *db, int32_t g)
{
    if (!seal_kv_ensure_schema(db))
        return false;
    struct seal_record r;
    memset(&r, 0, sizeof(r));
    r.height = g;
    for (int i = 0; i < 32; i++) {
        r.block_hash[i]         = (uint8_t)(g + i + 1);
        r.coins_sha3[i]         = (uint8_t)(g + i + 0x40);
        r.anchor_window_sha3[i] = (uint8_t)(g + i + 0x80);
    }
    r.utxo_count = (int64_t)g * 7 + 11;
    r.supply     = (int64_t)g * 1000000 + 333;
    r.sealed_at  = 1700000000 + g;

    progress_store_tx_lock();
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    if (ok) ok = seal_kv_insert_candidate_in_tx(db, &r);
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    progress_store_tx_unlock();
    if (!ok)
        return false;

    struct seal_record at;
    bool found = false;
    int slot = -1;
    if (!seal_kv_get_at_height(db, g, &at, &found, &slot) || !found)
        return false;
    progress_store_tx_lock();
    bool mok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    if (mok) mok = seal_kv_mark_ratified_in_tx(db, slot, &at);
    sqlite3_exec(db, mok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    progress_store_tx_unlock();
    return mok;
}

/* ── T7 fixture: a synthetic never-clearing condition ────────────────────
 * detect() always fires, witness() never clears — the shape of a condition
 * that stays "active" forever under its own remedy/cooldown schedule
 * (e.g. download_queue_starved), used to prove the belt-and-suspenders
 * auto-arm severity gate below. */
static bool se_t7_detect(void)
{
    return true;
}

static enum condition_remedy_result se_t7_remedy(void)
{
    return COND_REMEDY_OK;
}

static bool se_t7_witness(int64_t target_at_detect)
{
    (void)target_at_detect;
    return false;
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

    /* T4 — episode clears via tip progress with a PENDING (non-terminal)
     * reindex marker whose anchor the tip has progressed PAST:
     * withdraw_stale_reindex_request() (called from clear_episode) must
     * remove it so it does not outlive its episode and force a needless
     * reindex-chainstate rebuild on the next boot (live 2026-07-09: this
     * exact residue blocked `make deploy-dev` with "pending crash-only
     * auto-reindex request anchor=3175394" after the stall had already
     * self-resolved). No progress-store fixture is needed: the cached H*
     * (reducer_frontier_provable_tip_set) drives observe_tip(), so the clear
     * fires on the very first drive call, before any rung dispatch. */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "sticky_escalator", "t4_withdraw");
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        reducer_frontier_provable_tip_set(1000);
        sticky_escalator_set_datadir(dir);

        SE_CHECK("T4: plant a pending (non-terminal) reindex request",
                 boot_auto_reindex_request(dir, 900) == 1 &&
                 boot_auto_reindex_pending(dir));

        sticky_escalator_note_stall("test_t4_withdraw");
        int64_t t4 = (int64_t)platform_time_wall_time_t();
        /* tip_at_rung was stamped from the cached H*=1000 at arm time; inject
         * a tip 2 past it (STICKY_PROGRESS_MARGIN) so THIS drive call clears
         * the episode. */
        SE_CHECK("T4: tip progress clears the episode",
                 sticky_escalator_test_drive(1002, t4 + 1) ==
                     STICKY_RUNG_RETRY &&
                 !sticky_escalator_test_armed());
        SE_CHECK("T4: stale reindex marker withdrawn (tip 1002 > anchor 900)",
                 !boot_auto_reindex_pending(dir) &&
                 !boot_auto_reindex_is_terminal(dir));

        sticky_escalator_set_datadir(NULL);
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        test_cleanup_tmpdir(dir);
    }

    /* T5 — episode clears via tip progress, but the tip has NOT progressed
     * past the pending marker's anchor: the marker must be left alone (the
     * request may still describe a real anchor the next boot legitimately
     * needs to consume). */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "sticky_escalator", "t5_keep");
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        reducer_frontier_provable_tip_set(1000);
        sticky_escalator_set_datadir(dir);

        SE_CHECK("T5: plant a pending reindex request ABOVE the clearing tip",
                 boot_auto_reindex_request(dir, 1500) == 1 &&
                 boot_auto_reindex_pending(dir));

        sticky_escalator_note_stall("test_t5_keep");
        int64_t t5 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T5: tip progress still clears the episode (H* climbed)",
                 sticky_escalator_test_drive(1002, t5 + 1) ==
                     STICKY_RUNG_RETRY &&
                 !sticky_escalator_test_armed());
        SE_CHECK("T5: marker retained (tip 1002 has not passed anchor 1500)",
                 boot_auto_reindex_pending(dir));

        sticky_escalator_set_datadir(NULL);
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        test_cleanup_tmpdir(dir);
    }

    /* T6 — a TERMINAL marker (budget exhausted, operator already paged) must
     * never be touched by episode clearing — only the operator or a fresh,
     * strictly-higher-anchor episode may replace it (PRESERVE the
     * cross-boot budget / terminal-state semantics documented on
     * boot_auto_reindex_request / boot_auto_reindex_mark_terminal). */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "sticky_escalator", "t6_terminal");
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        reducer_frontier_provable_tip_set(1000);
        sticky_escalator_set_datadir(dir);

        SE_CHECK("T6: plant a TERMINAL reindex marker",
                 boot_auto_reindex_mark_terminal(dir, 900) &&
                 boot_auto_reindex_is_terminal(dir) &&
                 !boot_auto_reindex_pending(dir));

        sticky_escalator_note_stall("test_t6_terminal");
        int64_t t6 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T6: tip progress clears the episode",
                 sticky_escalator_test_drive(1002, t6 + 1) ==
                     STICKY_RUNG_RETRY &&
                 !sticky_escalator_test_armed());
        SE_CHECK("T6: terminal marker left untouched",
                 boot_auto_reindex_is_terminal(dir));

        sticky_escalator_set_datadir(NULL);
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        test_cleanup_tmpdir(dir);
    }

    /* T7 — belt-and-suspenders auto-arm (apply_drive's own
     * condition_engine_get_unresolved_critical_count() check, reached with NO
     * explicit sticky_escalator_note_stall) must only respond to an
     * unresolved CRITICAL condition, never a WARN-severity one that owns its
     * own bounded remedy/cooldown. Live 2026-07-09: download_queue_starved
     * (COND_WARN) stayed active 8+ hours on a healthy, tip-synced node and
     * kept silently re-arming this exact path every few minutes. */
    {
        static struct condition c_t7_warn = {
            .name = "t7_warn_only",
            .severity = COND_WARN,
            .poll_secs = 1,
            .backoff_secs = 0,
            .max_attempts = 1,
            .detect = se_t7_detect,
            .remedy = se_t7_remedy,
            .witness = se_t7_witness,
            .witness_window_secs = 60,
        };
        condition_engine_reset_for_testing();
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();

        SE_CHECK("T7: register a WARN-only unresolved condition",
                 condition_register(&c_t7_warn));
        condition_engine_tick();
        condition_engine_tick(); /* attempts reaches max_attempts=1: exhausted */
        SE_CHECK("T7: plain unresolved count sees the WARN backlog",
                 condition_engine_get_unresolved_count() == 1);
        SE_CHECK("T7: CRITICAL-scoped count does not",
                 condition_engine_get_unresolved_critical_count() == 0);

        int64_t t7 = (int64_t)platform_time_wall_time_t();
        sticky_escalator_test_drive(0, t7);
        SE_CHECK("T7: a WARN-only backlog must NOT auto-arm the ladder",
                 !sticky_escalator_test_armed());

        condition_engine_reset_for_testing();
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
    }

    /* T8 — the same belt-and-suspenders path DOES auto-arm for an unresolved
     * CRITICAL condition (the class it was built for — e.g.
     * reducer_frontier_reconcile_light). Confirms T7 is a severity filter, not
     * an accidental full disablement of the auto-arm path. */
    {
        static struct condition c_t8_critical = {
            .name = "t8_critical",
            .severity = COND_CRITICAL,
            .poll_secs = 1,
            .backoff_secs = 0,
            .max_attempts = 1,
            .detect = se_t7_detect,
            .remedy = se_t7_remedy,
            .witness = se_t7_witness,
            .witness_window_secs = 60,
        };
        condition_engine_reset_for_testing();
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();

        SE_CHECK("T8: register a CRITICAL unresolved condition",
                 condition_register(&c_t8_critical));
        condition_engine_tick();
        condition_engine_tick();
        SE_CHECK("T8: CRITICAL-scoped count sees it",
                 condition_engine_get_unresolved_critical_count() == 1);

        int64_t t8 = (int64_t)platform_time_wall_time_t();
        sticky_escalator_test_drive(0, t8);
        SE_CHECK("T8: an unresolved CRITICAL backlog DOES auto-arm the ladder",
                 sticky_escalator_test_armed());

        condition_engine_reset_for_testing();
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
    }

    /* ── T9/T10: the widen_peers rung ───────────────────────────────────────
     * A hermetic connman with directly-injected p2p_node entries (never
     * started — no sockets/threads), mirroring
     * test_connman_addnode_fallback.c's add_test_peer, so
     * connman_outbound_healthy_count() reflects exactly the peers we
     * inject. g_connect_only forces connman_kick_seed_discovery /
     * connman_kick_onion_seeds to no-op (both check it — lib/net/src/
     * connman.c) so the rung's real dispatch never touches DNS/onion
     * network I/O in-test; the dispatch itself is observed via the
     * sticky_escalator_test_widen_kicks() counter, not its side effects. */

    /* T9 — peers below the widen-peers floor: driving the real ladder all
     * the way down (retry -> targeted_rederive -> resnapshot -> reindex ->
     * self_mint_refold -> widen_peers, the same unexecutable-reindex shape
     * as T3) must NOT report NOT_IMPLEMENTED at widen_peers: it dispatches
     * connman_kick_seed_discovery (+ onion, zero outbound) and HOLDS the
     * rung for its witness window, same shape as T1's real curative rung. */
    {
        extern bool g_connect_only; /* lib/net/src/connman.c */
        bool saved_connect_only = g_connect_only;
        g_connect_only = true; /* force kick_* no-ops: no DNS/onion I/O */

        chain_params_select(CHAIN_MAIN);
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        SE_CHECK("T9: connman_init",
                 connman_init(&cm, chain_params_get(), &sigs));
        /* One healthy outbound peer — below STICKY_WIDEN_PEERS_MIN_HEALTHY
         * (3), but NOT zero, so the seed kick fires without the onion
         * peer-of-last-resort kick. */
        cm.manager.nodes = zcl_calloc(4, sizeof(*cm.manager.nodes),
                                      "test_sticky_widen_nodes");
        cm.manager.nodes_cap = 4;
        struct net_address addr;
        memset(&addr, 0, sizeof(addr));
        net_address_init(&addr);
        addr.svc.addr.ip[10] = 0xff;
        addr.svc.addr.ip[11] = 0xff;
        addr.svc.addr.ip[12] = 203;
        addr.svc.addr.ip[13] = 0;
        addr.svc.addr.ip[14] = 113;
        addr.svc.addr.ip[15] = 5;
        addr.svc.port = 8033;
        struct p2p_node *peer = p2p_node_create(
            &cm.manager, ZCL_INVALID_SOCKET, &addr, "widen-test", false);
        SE_CHECK("T9: inject one healthy outbound peer", peer != NULL);
        if (peer) {
            peer->state = PEER_HANDSHAKE_COMPLETE;
            peer->disconnect = false;
            peer->services = NODE_NETWORK;
            peer->starting_height = 100;
            cm.manager.nodes[cm.manager.num_nodes++] = peer;
        }
        SE_CHECK("T9: exactly one healthy outbound peer counted",
                 connman_outbound_healthy_count(&cm) == 1);

        struct se_fixture fx;
        SE_CHECK("T9: setup fixture", setup_fixture(&fx, "t9_widen"));
        sqlite3 *db = progress_store_db();
        SE_CHECK("T9: make rederive rung an honest no-op",
                 put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                 put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                 seed_coins_applied(db, A + 4) &&
                 seed_cursor(db, "tip_finalize", A + 3));

        sync_monitor_set_context(&cm, NULL, &fx.ms);
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();
        sticky_escalator_set_datadir(fx.dir); /* unexecutable -> escalates */

        sticky_escalator_note_stall("test_widen_below_floor");
        int64_t t9 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T9: retry -> targeted_rederive",
                 sticky_escalator_test_drive(0, t9 + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T9: no-op rederive -> resnapshot",
                 sticky_escalator_test_drive(0, t9 + 32) ==
                     STICKY_RUNG_RESNAPSHOT);
        SE_CHECK("T9: resnapshot stub -> reindex",
                 sticky_escalator_test_drive(0, t9 + 33) ==
                     STICKY_RUNG_REINDEX);
        SE_CHECK("T9: unexecutable reindex -> self_mint_refold",
                 sticky_escalator_test_drive(0, t9 + 34) ==
                     STICKY_RUNG_SELF_MINT_REFOLD);
        /* Entering the widen_peers rung does not dispatch it — the rung
         * action runs on the NEXT drive. So no kick has fired yet here. */
        SE_CHECK("T9: self_mint_refold stub -> widen_peers (not yet dispatched)",
                 sticky_escalator_test_drive(0, t9 + 35) ==
                     STICKY_RUNG_WIDEN_PEERS &&
                 sticky_escalator_test_widen_kicks() == 0);
        /* First dispatch of the rung: it kicks seed discovery and HOLDS the
         * rung within its witness window — never NOT_IMPLEMENTED (which would
         * have advanced instead of holding). */
        SE_CHECK("T9: widen_peers dispatches the real kick and HOLDS",
                 sticky_escalator_test_drive(0, t9 + 36) ==
                     STICKY_RUNG_WIDEN_PEERS &&
                 sticky_escalator_test_widen_kicks() == 1);
        /* Thrash guard: re-driving within the kick cooldown holds the rung
         * WITHOUT re-dispatching the discovery kick. */
        SE_CHECK("T9: thrash guard: re-drive within cooldown did NOT re-kick",
                 sticky_escalator_test_drive(0, t9 + 50) ==
                     STICKY_RUNG_WIDEN_PEERS &&
                 sticky_escalator_test_widen_kicks() == 1);

        sticky_escalator_set_datadir(NULL);
        teardown_fixture(&fx);
        connman_free(&cm);
        g_connect_only = saved_connect_only;
    }

    /* T10 — peers already at/above the floor when the ladder reaches
     * widen_peers: nothing for THIS rung to widen, so it must honestly
     * FAIL (not hold, not NOT_IMPLEMENTED) and advance straight to
     * rebootstrap without dispatching any kick. */
    {
        extern bool g_connect_only;
        bool saved_connect_only = g_connect_only;
        g_connect_only = true;

        chain_params_select(CHAIN_MAIN);
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        SE_CHECK("T10: connman_init",
                 connman_init(&cm, chain_params_get(), &sigs));
        cm.manager.nodes = zcl_calloc(4, sizeof(*cm.manager.nodes),
                                      "test_sticky_widen_healthy_nodes");
        cm.manager.nodes_cap = 4;
        for (uint8_t i = 0; i < 3; i++) {
            struct net_address addr;
            memset(&addr, 0, sizeof(addr));
            net_address_init(&addr);
            addr.svc.addr.ip[10] = 0xff;
            addr.svc.addr.ip[11] = 0xff;
            addr.svc.addr.ip[12] = 203;
            addr.svc.addr.ip[13] = 0;
            addr.svc.addr.ip[14] = 113;
            addr.svc.addr.ip[15] = (uint8_t)(10 + i);
            addr.svc.port = 8033;
            struct p2p_node *peer = p2p_node_create(
                &cm.manager, ZCL_INVALID_SOCKET, &addr, "widen-test", false);
            if (peer) {
                peer->state = PEER_HANDSHAKE_COMPLETE;
                peer->disconnect = false;
                peer->services = NODE_NETWORK;
                peer->starting_height = 100;
                cm.manager.nodes[cm.manager.num_nodes++] = peer;
            }
        }
        SE_CHECK("T10: three healthy outbound peers counted (at floor)",
                 connman_outbound_healthy_count(&cm) == 3);

        struct se_fixture fx;
        SE_CHECK("T10: setup fixture", setup_fixture(&fx, "t10_healthy"));
        sqlite3 *db = progress_store_db();
        SE_CHECK("T10: make rederive rung an honest no-op",
                 put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                 put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                 seed_coins_applied(db, A + 4) &&
                 seed_cursor(db, "tip_finalize", A + 3));

        sync_monitor_set_context(&cm, NULL, &fx.ms);
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();
        sticky_escalator_set_datadir(fx.dir);

        sticky_escalator_note_stall("test_widen_already_healthy");
        int64_t t10 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T10: retry -> targeted_rederive",
                 sticky_escalator_test_drive(0, t10 + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T10: no-op rederive -> resnapshot",
                 sticky_escalator_test_drive(0, t10 + 32) ==
                     STICKY_RUNG_RESNAPSHOT);
        SE_CHECK("T10: resnapshot stub -> reindex",
                 sticky_escalator_test_drive(0, t10 + 33) ==
                     STICKY_RUNG_REINDEX);
        SE_CHECK("T10: unexecutable reindex -> self_mint_refold",
                 sticky_escalator_test_drive(0, t10 + 34) ==
                     STICKY_RUNG_SELF_MINT_REFOLD);
        SE_CHECK("T10: self_mint_refold stub -> widen_peers",
                 sticky_escalator_test_drive(0, t10 + 35) ==
                     STICKY_RUNG_WIDEN_PEERS);
        SE_CHECK("T10: already-healthy widen_peers advances immediately "
                 "to rebootstrap (FAILED, not a hold)",
                 sticky_escalator_test_drive(0, t10 + 36) ==
                     STICKY_RUNG_REBOOTSTRAP);
        SE_CHECK("T10: no kick dispatched — nothing to widen",
                 sticky_escalator_test_widen_kicks() == 0);

        sticky_escalator_set_datadir(NULL);
        teardown_fixture(&fx);
        connman_free(&cm);
        g_connect_only = saved_connect_only;
    }

    /* ── T11: self_mint_refold ARMS the from-anchor refold; the TERMINAL rung
     * triggers the (suppressed) self-respawn that consumes it ────────────────
     * The stub rungs are now real: self_mint_refold arms boot_auto_refold
     * (witnessed via boot_auto_refold_pending) WITHOUT respawning, so the
     * cheaper widen_peers/rebootstrap rungs still get a turn; the terminal
     * refold_from_anchor rung then pulls the restart trigger. The anchor-artifact
     * gate is forced present and the real shutdown/respawn syscalls suppressed. */
    {
        struct se_fixture fx;
        SE_CHECK("T11: setup fixture", setup_fixture(&fx, "t11_refold_arm"));
        sqlite3 *db = progress_store_db();
        SE_CHECK("T11: make rederive rung an honest no-op",
                 put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                 put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                 seed_coins_applied(db, A + 4) &&
                 seed_cursor(db, "tip_finalize", A + 3));

        sync_monitor_set_context(NULL, NULL, &fx.ms);
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();
        reducer_frontier_provable_tip_reset();
        reducer_frontier_provable_tip_set(1000);
        sticky_escalator_set_datadir(fx.dir);
        sticky_escalator_test_set_refold_artifact_available(1);
        sticky_escalator_test_set_suppress_refold_restart(true);

        SE_CHECK("T11: no refold armed before the ladder runs",
                 !boot_auto_refold_pending(fx.dir));

        sticky_escalator_note_stall("test_refold_arm");
        int64_t t = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T11: retry -> targeted_rederive",
                 sticky_escalator_test_drive(0, t + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T11: no-op rederive -> resnapshot",
                 sticky_escalator_test_drive(0, t + 32) ==
                     STICKY_RUNG_RESNAPSHOT);
        SE_CHECK("T11: resnapshot (no base) -> reindex",
                 sticky_escalator_test_drive(0, t + 33) == STICKY_RUNG_REINDEX);
        SE_CHECK("T11: unexecutable reindex -> self_mint_refold",
                 sticky_escalator_test_drive(0, t + 34) ==
                     STICKY_RUNG_SELF_MINT_REFOLD);
        /* self_mint_refold arms the durable refold and advances (arm persists). */
        SE_CHECK("T11: self_mint_refold arms the refold + advances to widen_peers",
                 sticky_escalator_test_drive(0, t + 35) ==
                     STICKY_RUNG_WIDEN_PEERS &&
                 boot_auto_refold_pending(fx.dir));
        SE_CHECK("T11: widen_peers (no connman) -> rebootstrap",
                 sticky_escalator_test_drive(0, t + 36) ==
                     STICKY_RUNG_REBOOTSTRAP);
        SE_CHECK("T11: rebootstrap -> terminal refold_from_anchor",
                 sticky_escalator_test_drive(0, t + 37) ==
                     STICKY_RUNG_REFOLD_FROM_ANCHOR);
        /* Terminal rung sees the pending arm, requests the (suppressed) respawn,
         * and HOLDS within its witness window — the refold stays armed and the
         * ladder is still driving (never-give-up). */
        SE_CHECK("T11: terminal refold requests respawn and holds (armed)",
                 sticky_escalator_test_drive(0, t + 38) ==
                     STICKY_RUNG_REFOLD_FROM_ANCHOR &&
                 boot_auto_refold_pending(fx.dir) &&
                 sticky_escalator_test_armed());

        sticky_escalator_set_datadir(NULL);
        reducer_frontier_provable_tip_reset();
        teardown_fixture(&fx);
    }

    /* ── T12: resnapshot detects the nearest SELF-VERIFIED rewind base ───────
     * With NO base it names the typed blocker resnapshot_no_base; with a
     * ratified seal reachable it names resnapshot_no_consumer (base found, but
     * no in-process stage_rederive_range consumer linked in this build). Either
     * way it advances — never a borrowed-state snapshot pull, never a faked
     * "done". Witnessed via the typed blocker registry. */
    {
        struct se_fixture fx;
        SE_CHECK("T12: setup fixture", setup_fixture(&fx, "t12_resnapshot_base"));
        sqlite3 *db = progress_store_db();
        SE_CHECK("T12: make rederive rung an honest no-op",
                 put_tip_log(db, A + 2, 1, &fx.hashes[2]) &&
                 put_tip_log(db, A + 3, 1, &fx.hashes[3]) &&
                 seed_coins_applied(db, A + 4) &&
                 seed_cursor(db, "tip_finalize", A + 3));

        sync_monitor_set_context(NULL, NULL, &fx.ms);

        /* Sub-case A: no seal, no artifact -> resnapshot names no_base. */
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();
        blocker_clear("sticky_escalator.resnapshot_no_base");
        blocker_clear("sticky_escalator.resnapshot_no_consumer");
        sticky_escalator_note_stall("test_resnapshot_no_base");
        int64_t t = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T12A: retry -> targeted_rederive",
                 sticky_escalator_test_drive(0, t + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T12A: no-op rederive -> resnapshot",
                 sticky_escalator_test_drive(0, t + 32) ==
                     STICKY_RUNG_RESNAPSHOT);
        SE_CHECK("T12A: resnapshot (no base) names no_base + advances to reindex",
                 sticky_escalator_test_drive(0, t + 33) == STICKY_RUNG_REINDEX &&
                 blocker_exists("sticky_escalator.resnapshot_no_base") &&
                 !blocker_exists("sticky_escalator.resnapshot_no_consumer"));

        /* Sub-case B: seed a ratified seal -> resnapshot finds the base and
         * names no_consumer (stage_rederive_range weak symbol is NULL here). */
        SE_CHECK("T12B: seed a ratified seal at a grid point",
                 seed_ratified_seal(db, 1000));
        sticky_escalator_test_reset();
        stage_reducer_frontier_reset_detect_memo_for_testing();
        blocker_clear("sticky_escalator.resnapshot_no_base");
        blocker_clear("sticky_escalator.resnapshot_no_consumer");
        sticky_escalator_note_stall("test_resnapshot_base_found");
        int64_t t2 = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T12B: retry -> targeted_rederive",
                 sticky_escalator_test_drive(0, t2 + 31) ==
                     STICKY_RUNG_TARGETED_REDERIVE);
        SE_CHECK("T12B: no-op rederive -> resnapshot",
                 sticky_escalator_test_drive(0, t2 + 32) ==
                     STICKY_RUNG_RESNAPSHOT);
        SE_CHECK("T12B: resnapshot finds the seal base, names no_consumer, advances",
                 sticky_escalator_test_drive(0, t2 + 33) == STICKY_RUNG_REINDEX &&
                 blocker_exists("sticky_escalator.resnapshot_no_consumer") &&
                 !blocker_exists("sticky_escalator.resnapshot_no_base"));

        blocker_clear("sticky_escalator.resnapshot_no_base");
        blocker_clear("sticky_escalator.resnapshot_no_consumer");
        teardown_fixture(&fx);
    }

    /* ── T13: episode clear withdraws a stale armed refold ───────────────────
     * A pending (non-terminal) auto_refold_request whose anchor the tip has
     * progressed past is withdrawn on episode clear (mirrors T4 for the reindex
     * marker) so a self-resolved stall does not force a needless from-anchor
     * refold on the next boot — the auto-terminating-remedy invariant. */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "sticky_escalator", "t13_refold_withdraw");
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        reducer_frontier_provable_tip_set(1000);
        sticky_escalator_set_datadir(dir);

        SE_CHECK("T13: plant a pending (non-terminal) refold request",
                 boot_auto_refold_request(dir, 900) == 1 &&
                 boot_auto_refold_pending(dir));

        sticky_escalator_note_stall("test_t13_refold_withdraw");
        int64_t t = (int64_t)platform_time_wall_time_t();
        SE_CHECK("T13: tip progress clears the episode",
                 sticky_escalator_test_drive(1002, t + 1) == STICKY_RUNG_RETRY &&
                 !sticky_escalator_test_armed());
        SE_CHECK("T13: stale refold marker withdrawn (tip 1002 > anchor 900)",
                 !boot_auto_refold_pending(dir) &&
                 !boot_auto_refold_is_terminal(dir));

        sticky_escalator_set_datadir(NULL);
        sticky_escalator_test_reset();
        reducer_frontier_provable_tip_reset();
        test_cleanup_tmpdir(dir);
    }

    reducer_frontier_test_set_compiled_anchor(-1); /* restore production floor */

    printf("sticky_escalator: %d failures\n", failures);
    return failures;
}
