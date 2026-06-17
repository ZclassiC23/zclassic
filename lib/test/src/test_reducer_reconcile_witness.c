/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * T10/T11 (WP-F / FIX-5): reducer_frontier_reconcile_light condition-layer
 * witness channel + peer-gate visibility, proven through the REAL condition
 * engine (condition_engine_tick), never by calling detect/remedy directly.
 *
 * T10 — tipfin backfill witness channel (panel-required): a row-only
 *   backfill moves no stage cursor and no chain height, so it is
 *   structurally unwitnessable through the pre-existing channels; the
 *   tipfin_backfill.progress record (including absent<->present
 *   transitions) is the mandatory channel. With the record bumping every
 *   remedy round the attempt budget must NEVER freeze (no operator page
 *   across >5 rounds); with the record artificially frozen the budget must
 *   still exhaust at max_attempts=5 and page (EV_OPERATOR_NEEDED path
 *   intact).
 *
 * T11 — peer-gate bypass: peers present but none ahead used to idle the
 *   ENTIRE L1 layer silently. A pending refused_coin_tear (durable internal
 *   evidence) must now bypass the gate with ONE transition-logged WARN;
 *   the same peer state with no tear must suppress exactly as today.
 *
 * Fixture is a trimmed copy of test_reducer_frontier_reconcile_light.c's. */

#include "test/test_helpers.h"

#include "conditions/reducer_frontier_reconcile_light.h"
#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "net/connman.h"
#include "net/net.h"
#include "services/sync_monitor.h"
#include "storage/progress_store.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Mirrors app/conditions/src/reducer_frontier_reconcile_light.c ZCL_TESTING
 * hooks that are src-private (the test_utxo_apply_stage.c delta-internal
 * mirror pattern). The post-remedy hook stands in for the TIPFIN backfill
 * bumping its progress record during the remedy. */
void reducer_frontier_reconcile_light_test_set_post_remedy_hook(
    void (*fn)(void));
int reducer_frontier_reconcile_light_test_bypass_warns(void);

#define RRW_CHECK(name, expr) do { \
    printf("reducer_reconcile_witness: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

#define A REDUCER_FRONTIER_TRUSTED_ANCHOR
#define COND_NAME "reducer_frontier_reconcile_light"

struct rrw_fixture {
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

/* Seed a REAL coin tear: poke an ok=0 row into utxo_apply_log at height K.
 * The coin-tear test now compares coins_applied against utxo_apply's OWN
 * contiguous ok=1 log prefix (reducer_frontier_log_frontier on
 * "utxo_apply_log"/"utxo_apply"), so an ok=0 row at K (overwriting the ok=1
 * row setup_fixture wrote) terminates that prefix at K-1. With coins_applied
 * set above K, `coins_applied > utxo_apply_contig + 1` is TRUE — a genuine
 * tear of coins above the solid applied log, NOT a tip_finalize-lag false
 * positive. Status 'verified' (default) is intentional: it must NOT be
 * 'value_overflow' (the maybe_repair_value_overflow trigger), so this remains
 * a plain applied-log hole that no pre-refusal repair claims, and the L1 flow
 * reaches the terminal coin-tear refusal that drives the witness machinery. */
static bool poke_utxo_apply_hole(sqlite3 *db, int height)
{
    return put_simple_log(db, "utxo_apply_log", height, 0);
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

/* Write/bump the tipfin_backfill.progress record exactly as the TIPFIN
 * backfill does: value begins [progress i32 LE]. */
static bool set_tipfin_progress(sqlite3 *db, int v)
{
    uint8_t blob[4] = {
        (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)
    };
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, "tipfin_backfill.progress", -1, SQLITE_STATIC);
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

static bool setup_fixture(struct rrw_fixture *fx, const char *tag)
{
    memset(fx, 0, sizeof(*fx));
    test_make_tmpdir(fx->dir, sizeof(fx->dir),
                     "reducer_reconcile_witness", tag);
    if (!progress_store_open(fx->dir))
        return false;
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

static void teardown_fixture(struct rrw_fixture *fx)
{
    main_state_free(&fx->ms);
    progress_store_close();
    test_cleanup_tmpdir(fx->dir);
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

/* T10 post-remedy hook: the TIPFIN backfill repairs a batch DURING the
 * remedy and bumps its progress record; the engine's immediate post-remedy
 * witness re-check then sees the bump. */
static int g_bump_progress = 100;
static void bump_tipfin_record(void)
{
    g_bump_progress++;
    (void)set_tipfin_progress(progress_store_db(), g_bump_progress);
}

int test_reducer_reconcile_witness(void);
int test_reducer_reconcile_witness(void)
{
    test_reset_shared_globals();   /* monolith isolation: see test_helpers.c */
    printf("\n=== reducer_reconcile_witness (FIX-5) tests ===\n");
    int failures = 0;

    /* ── T10a: bumping record keeps the attempt budget alive ─────────── */
    {
        struct rrw_fixture fx;
        RRW_CHECK("T10a: setup tear fixture", setup_fixture(&fx, "t10_bump"));
        sqlite3 *db = progress_store_db();
        /* REAL coin tear: the tear is now measured against utxo_apply's OWN
         * contiguous ok=1 log prefix, so seed a real utxo_apply hole at K=A+2
         * with coins_applied=A+3 > K — `coins_applied > utxo_apply_contig + 1`
         * is TRUE so the tear (and the bypass/witness machinery) genuinely
         * fires. Remedy refuses (COND_REMEDY_FAILED) every round — the ONLY
         * thing moving is the backfill record the hook bumps. */
        RRW_CHECK("T10a: poke real utxo_apply hole at K=A+2",
                  poke_utxo_apply_hole(db, A + 2));
        RRW_CHECK("T10a: seed coins_applied above the hole (tear)",
                  seed_coins_applied(db, A + 3));
        RRW_CHECK("T10a: backfill record present before first tick",
                  set_tipfin_progress(db, 100));

        struct connman cm;
        memset(&cm, 0, sizeof(cm));
        net_manager_init(&cm.manager);

        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        sync_monitor_init();
        sync_monitor_set_context(&cm, NULL, &fx.ms);
        sync_monitor_test_set_tip_advance_ts(1);
        register_reducer_frontier_reconcile_light();
        g_bump_progress = 100;
        reducer_frontier_reconcile_light_test_set_post_remedy_hook(
            bump_tipfin_record);

        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(COND_NAME, &snap);
        int cleared_before = got ? snap.cleared_count : -1;

        /* >5 remedy rounds: each tick detects (snapshot), remedies (hook
         * bumps the record, impl refuses the tear), and the immediate
         * post-remedy witness sees the bump -> cleared -> attempts reset.
         * No backoff-clearing needed: condition_mark_cleared zeroes
         * last_remedy_unix. */
        for (int i = 0; i < 7; i++)
            condition_engine_tick();

        got = condition_engine_get_registered_snapshot(COND_NAME, &snap);
        RRW_CHECK("T10a: 7 remedy rounds ran",
                  got &&
                  reducer_frontier_reconcile_light_test_remedy_calls() == 7);
        RRW_CHECK("T10a: every round witnessed (cleared 7x, budget reset)",
                  got && cleared_before >= 0 &&
                  snap.cleared_count - cleared_before == 7 &&
                  snap.attempts == 0 &&
                  !snap.currently_active);
        RRW_CHECK("T10a: attempts never hit max — no operator page",
                  got && !snap.operator_needed_emitted);

        reducer_frontier_reconcile_light_test_set_post_remedy_hook(NULL);
        sync_monitor_set_context(NULL, NULL, NULL);
        sync_monitor_test_set_tip_advance_ts(0);
        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        net_manager_free(&cm.manager);
        teardown_fixture(&fx);
    }

    /* ── T10b: frozen record exhausts the budget and pages ───────────── */
    {
        struct rrw_fixture fx;
        RRW_CHECK("T10b: setup tear fixture",
                  setup_fixture(&fx, "t10_frozen"));
        sqlite3 *db = progress_store_db();
        /* REAL coin tear: the tear is now measured against utxo_apply's OWN
         * contiguous ok=1 log prefix, so seed a real utxo_apply hole at K=A+2
         * with coins_applied=A+3 > K — `coins_applied > utxo_apply_contig + 1`
         * is TRUE so the tear genuinely fires (and, the record being frozen,
         * the budget exhausts and pages). */
        RRW_CHECK("T10b: poke real utxo_apply hole at K=A+2",
                  poke_utxo_apply_hole(db, A + 2));
        RRW_CHECK("T10b: seed coins_applied above the hole (tear)",
                  seed_coins_applied(db, A + 3));
        /* Record present but FROZEN: presence alone must never witness —
         * only movement (or an absent<->present transition) does. */
        RRW_CHECK("T10b: frozen backfill record present",
                  set_tipfin_progress(db, 42));

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
        bool got = condition_engine_get_registered_snapshot(COND_NAME, &snap);
        RRW_CHECK("T10b: frozen record -> attempts hit max and page "
                  "(EV_OPERATOR_NEEDED path intact)",
                  got &&
                  reducer_frontier_reconcile_light_test_remedy_calls() == 5 &&
                  snap.currently_active &&
                  snap.attempts >= 5 &&
                  snap.last_outcome == COND_REMEDY_FAILED &&
                  snap.operator_needed_emitted);

        sync_monitor_set_context(NULL, NULL, NULL);
        sync_monitor_test_set_tip_advance_ts(0);
        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        net_manager_free(&cm.manager);
        teardown_fixture(&fx);
    }

    /* ── T11a: peers-not-ahead + pending tear -> gate BYPASS ─────────── */
    {
        struct rrw_fixture fx;
        RRW_CHECK("T11a: setup tear fixture",
                  setup_fixture(&fx, "t11_bypass"));
        sqlite3 *db = progress_store_db();
        /* REAL coin tear: the tear is now measured against utxo_apply's OWN
         * contiguous ok=1 log prefix, so seed a real utxo_apply hole at K=A+2
         * with coins_applied=A+3 > K — `coins_applied > utxo_apply_contig + 1`
         * is TRUE so the tear (and the peer-gate bypass) genuinely fires. */
        RRW_CHECK("T11a: poke real utxo_apply hole at K=A+2",
                  poke_utxo_apply_hole(db, A + 2));
        RRW_CHECK("T11a: seed coins_applied above the hole (tear)",
                  seed_coins_applied(db, A + 3));

        /* One peer, NOT ahead: starting_height == local finalized height
         * (A+1). peer_lag_allows_repair refuses this — pre-FIX-5 the whole
         * L1 layer idled here silently. */
        struct connman cm;
        struct p2p_node p1;
        struct p2p_node *peers[1];
        memset(&cm, 0, sizeof(cm));
        net_manager_init(&cm.manager);
        memset(&p1, 0, sizeof(p1));
        p1.id = 1;
        p1.starting_height = A + 1;
        p1.state = PEER_ACTIVE;
        peers[0] = &p1;
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;

        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        sync_monitor_init();
        sync_monitor_set_context(&cm, NULL, &fx.ms);
        sync_monitor_test_set_tip_advance_ts(1);
        register_reducer_frontier_reconcile_light();

        condition_engine_tick();

        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(COND_NAME, &snap);
        RRW_CHECK("T11a: detect activates via bypass and remedy runs",
                  got &&
                  reducer_frontier_reconcile_light_test_remedy_calls() == 1 &&
                  snap.currently_active &&
                  snap.last_outcome == COND_REMEDY_FAILED);
        RRW_CHECK("T11a: bypass WARN emitted once",
                  reducer_frontier_reconcile_light_test_bypass_warns() == 1);

        /* Second tick: still bypassing, but the WARN is transition-logged —
         * no repeat. */
        reducer_frontier_reconcile_light_test_clear_backoff();
        condition_engine_tick();
        RRW_CHECK("T11a: second tick keeps detecting",
                  reducer_frontier_reconcile_light_test_remedy_calls() == 2);
        RRW_CHECK("T11a: bypass WARN still exactly once",
                  reducer_frontier_reconcile_light_test_bypass_warns() == 1);

        cm.manager.nodes = NULL;
        cm.manager.num_nodes = 0;
        sync_monitor_set_context(NULL, NULL, NULL);
        sync_monitor_test_set_tip_advance_ts(0);
        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        net_manager_free(&cm.manager);
        teardown_fixture(&fx);
    }

    /* ── T11b: same peer state, NO tear -> gate suppresses as today ──── */
    {
        struct rrw_fixture fx;
        RRW_CHECK("T11b: setup plain repair fixture",
                  setup_fixture(&fx, "t11_suppress"));
        sqlite3 *db = progress_store_db();

        struct connman cm;
        struct p2p_node p1;
        struct p2p_node *peers[1];
        memset(&cm, 0, sizeof(cm));
        net_manager_init(&cm.manager);
        memset(&p1, 0, sizeof(p1));
        p1.id = 1;
        p1.starting_height = A + 1;
        p1.state = PEER_ACTIVE;
        peers[0] = &p1;
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;

        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        sync_monitor_init();
        sync_monitor_set_context(&cm, NULL, &fx.ms);
        sync_monitor_test_set_tip_advance_ts(1);
        register_reducer_frontier_reconcile_light();

        condition_engine_tick();
        reducer_frontier_reconcile_light_test_clear_backoff();
        condition_engine_tick();

        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(COND_NAME, &snap);
        RRW_CHECK("T11b: gate suppresses the plain cursor-churn class",
                  got &&
                  reducer_frontier_reconcile_light_test_remedy_calls() == 0 &&
                  !snap.currently_active &&
                  reducer_frontier_reconcile_light_test_bypass_warns() == 0);
        RRW_CHECK("T11b: no repair mutation while suppressed",
                  cursor_value(db, "body_fetch") == A + 4 &&
                  cursor_value(db, "tip_finalize") == A + 4);

        cm.manager.nodes = NULL;
        cm.manager.num_nodes = 0;
        sync_monitor_set_context(NULL, NULL, NULL);
        sync_monitor_test_set_tip_advance_ts(0);
        condition_engine_reset_for_testing();
        reducer_frontier_reconcile_light_test_reset();
        net_manager_free(&cm.manager);
        teardown_fixture(&fx);
    }

    printf("reducer_reconcile_witness: %d failures\n", failures);
    return failures;
}
