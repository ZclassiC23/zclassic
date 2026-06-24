/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Unit tests for Wave S S-6 script_validate stage. */

#include "test/test_helpers.h"

#include "bloom/merkle.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "jobs/created_outputs_index.h"
#include "jobs/script_validate_stage.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SV_CHECK(name, expr) do { \
    printf("script_validate: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

struct synth_chain_sv {
    struct block_index *blocks;
    struct uint256     *hashes;
    struct block       *bodies;
    struct uint256     *prev_hashes;
    struct tx_out      *prevouts;
    int                 n;
    int                 invalid_height;
    int                 missing_prevout_height;
    int                 no_data_height;
};

static int mkdir_p_sv(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void make_prevout(struct tx_out *out, bool valid)
{
    tx_out_set_null(out);
    out->value = 100000000;
    script_init(&out->script_pub_key);
    script_push_op(&out->script_pub_key, valid ? OP_TRUE : OP_FALSE);
}

static bool make_body(struct synth_chain_sv *sc, int h)
{
    struct block *b = &sc->bodies[h];
    block_init(b);
    b->header.nVersion = 4;
    b->header.nTime = (uint32_t)(1700000000u + (uint32_t)h);
    b->header.nBits = 0x1f07ffff;
    b->num_vtx = 2;
    b->vtx = zcl_calloc(2, sizeof(struct transaction), "sv_tx");
    if (!b->vtx) return false;

    transaction_init(&b->vtx[0]);
    if (!transaction_alloc(&b->vtx[0], 1, 1)) return false;
    outpoint_set_null(&b->vtx[0].vin[0].prevout);
    script_push_data(&b->vtx[0].vin[0].script_sig, (const unsigned char *)&h,
                     sizeof(h));
    b->vtx[0].vout[0] = sc->prevouts[h];
    transaction_compute_hash(&b->vtx[0]);
    sc->prev_hashes[h] = b->vtx[0].hash;

    transaction_init(&b->vtx[1]);
    if (!transaction_alloc(&b->vtx[1], 1, 1)) return false;
    b->vtx[1].vin[0].prevout.hash = sc->prev_hashes[h];
    b->vtx[1].vin[0].prevout.n = 0;
    script_init(&b->vtx[1].vin[0].script_sig);
    b->vtx[1].vout[0].value = 99900000;
    script_init(&b->vtx[1].vout[0].script_pub_key);
    script_push_op(&b->vtx[1].vout[0].script_pub_key, OP_TRUE);
    transaction_compute_hash(&b->vtx[1]);

    struct uint256 txids[2] = { b->vtx[0].hash, b->vtx[1].hash };
    b->header.hashMerkleRoot = compute_merkle_root(txids, 2);
    return true;
}

static bool synth_chain_sv_build(struct synth_chain_sv *sc, int n)
{
    memset(sc, 0, sizeof(*sc));
    sc->invalid_height = -1;
    sc->missing_prevout_height = -1;
    sc->no_data_height = -1;
    sc->blocks = zcl_calloc((size_t)n, sizeof(struct block_index),
                            "sv_blocks");
    sc->hashes = zcl_calloc((size_t)n, sizeof(struct uint256),
                            "sv_hashes");
    sc->bodies = zcl_calloc((size_t)n, sizeof(struct block),
                            "sv_bodies");
    sc->prev_hashes = zcl_calloc((size_t)n, sizeof(struct uint256),
                                 "sv_prev_hashes");
    sc->prevouts = zcl_calloc((size_t)n, sizeof(struct tx_out),
                              "sv_prevouts");
    if (!sc->blocks || !sc->hashes || !sc->bodies ||
        !sc->prev_hashes || !sc->prevouts)
        return false;
    for (int i = 0; i < n; i++) {
        make_prevout(&sc->prevouts[i], true);
        if (!make_body(sc, i)) return false;
        block_header_get_hash(&sc->bodies[i].header, &sc->hashes[i]);
        block_index_init(&sc->blocks[i]);
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].hashMerkleRoot = sc->bodies[i].header.hashMerkleRoot;
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = sc->bodies[i].header.nVersion;
        sc->blocks[i].nTime = sc->bodies[i].header.nTime;
        sc->blocks[i].nBits = sc->bodies[i].header.nBits;
        sc->blocks[i].nStatus = BLOCK_HAVE_DATA;
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void synth_chain_sv_free(struct synth_chain_sv *sc)
{
    if (sc->bodies) {
        for (int i = 0; i < sc->n; i++)
            block_free(&sc->bodies[i]);
    }
    free(sc->blocks);
    free(sc->hashes);
    free(sc->bodies);
    free(sc->prev_hashes);
    free(sc->prevouts);
    memset(sc, 0, sizeof(*sc));
}

static bool fake_reader(struct block *out, const struct block_index *bi,
                        const char *datadir, void *user)
{
    (void)datadir;
    struct synth_chain_sv *sc = user;
    if (!out || !bi || !sc || bi->nHeight < 0 || bi->nHeight >= sc->n)
        return false;
    return test_block_copy(out, &sc->bodies[bi->nHeight], "sv_tx_copy");
}

static bool fake_prevout(const struct outpoint *prevout, struct tx_out *out,
                         void *user)
{
    struct synth_chain_sv *sc = user;
    if (!prevout || !out || !sc) return false;
    for (int h = 0; h < sc->n; h++) {
        if (h == sc->missing_prevout_height)
            continue;
        if (uint256_eq(&prevout->hash, &sc->prev_hashes[h]) &&
            prevout->n == 0) {
            *out = sc->prevouts[h];
            if (h == sc->invalid_height) {
                script_init(&out->script_pub_key);
                script_push_op(&out->script_pub_key, OP_FALSE);
            }
            return true;
        }
    }
    return false;
}

static bool retarget_spend(struct synth_chain_sv *sc, int spend_h,
                           int created_h)
{
    if (!sc || spend_h < 0 || spend_h >= sc->n ||
        created_h < 0 || created_h >= sc->n)
        return false;
    struct block *b = &sc->bodies[spend_h];
    b->vtx[1].vin[0].prevout.hash = sc->prev_hashes[created_h];
    b->vtx[1].vin[0].prevout.n = 0;
    transaction_compute_hash(&b->vtx[1]);
    struct uint256 txids[2] = { b->vtx[0].hash, b->vtx[1].hash };
    b->header.hashMerkleRoot = compute_merkle_root(txids, 2);
    block_header_get_hash(&b->header, &sc->hashes[spend_h]);
    sc->blocks[spend_h].hashMerkleRoot = b->header.hashMerkleRoot;
    return true;
}

static bool seed_coins_frontier(sqlite3 *db, int32_t frontier)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    bool ok = coins_kv_set_applied_height_in_tx(db, frontier);
    if (ok)
        ok = sqlite3_exec(db, "COMMIT", NULL, NULL, &err) == SQLITE_OK;
    if (!ok)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (err) sqlite3_free(err);
    return ok;
}

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

static bool seed_body_persist(sqlite3 *db, int n, int upstream_fail_height)
{
    if (!exec_sql(db,
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "  height       INTEGER PRIMARY KEY,"
        "  source       TEXT    NOT NULL,"
        "  ok           INTEGER NOT NULL,"
        "  persisted_at INTEGER NOT NULL"
        ")"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO body_persist_log "
        "(height, source, ok, persisted_at) VALUES (?, ?, ?, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h < n; h++) {
        int ok = (h == upstream_fail_height) ? 0 : 1;
        sqlite3_bind_int(st, 1, h);
        sqlite3_bind_text(st, 2, ok ? "verified" : "merkle_mismatch",
                          -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, ok);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return false;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
        "VALUES('body_persist', ?, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, n);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool log_row_at(sqlite3 *db, int height, int *out_ok,
                       char *out_status, size_t status_size,
                       int *out_vin)
{
    *out_ok = -1;
    *out_vin = -2;
    if (out_status && status_size) out_status[0] = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, status, first_failure_vin FROM script_validate_log "
        "WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt && out_status && status_size)
            snprintf(out_status, status_size, "%s", (const char *)txt);
        *out_vin = sqlite3_column_type(st, 2) == SQLITE_NULL
            ? -1 : sqlite3_column_int(st, 2);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

static int sv_setup(const char *tag, int n, int upstream_fail_height,
                    char *dir_out, size_t dir_out_size,
                    struct main_state *ms, struct synth_chain_sv *sc)
{
    test_fmt_tmpdir(dir_out, dir_out_size, "script_validate", tag);
    mkdir_p_sv("./test-tmp");
    mkdir_p_sv(dir_out);
    if (!progress_store_open(dir_out)) return 1;

    memset(ms, 0, sizeof(*ms));
    active_chain_init(&ms->chain_active);
    if (!synth_chain_sv_build(sc, n)) return 2;
    active_chain_move_window_tip(&ms->chain_active, &sc->blocks[n - 1]);

    if (!seed_body_persist(progress_store_db(), n, upstream_fail_height))
        return 3;
    if (!script_validate_stage_init(ms)) return 4;
    script_validate_stage_set_reader(fake_reader, sc);
    script_validate_stage_set_prevout_resolver(fake_prevout, sc);
    return 0;
}

static void sv_teardown(const char *dir, struct main_state *ms,
                        struct synth_chain_sv *sc)
{
    script_validate_stage_shutdown();
    active_chain_free(&ms->chain_active);
    synth_chain_sv_free(sc);
    progress_store_close();
    test_cleanup_tmpdir(dir);
}

int test_script_validate_stage(void);
int test_script_validate_stage(void)
{
    printf("\n=== script_validate_stage tests ===\n");
    int failures = 0;

    blocker_module_init();

    {
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("happy: setup",
                 sv_setup("happy", 3, -1, dir, sizeof(dir), &ms, &sc) == 0);
        SV_CHECK("happy: drains 3", script_validate_stage_drain(100) == 3);
        SV_CHECK("happy: cursor at 3", script_validate_stage_cursor() == 3);
        SV_CHECK("happy: verified_total == 3",
                 script_validate_stage_verified_total() == 3);
        SV_CHECK("happy: inputs_verified_total == 3",
                 script_validate_stage_inputs_verified_total() == 3);
        for (int h = 0; h < 3; h++) {
            int ok = -1, vin = -2; char status[32];
            log_row_at(progress_store_db(), h, &ok, status, sizeof(status),
                       &vin);
            SV_CHECK("happy: row ok=1", ok == 1);
            SV_CHECK("happy: row status verified",
                     strcmp(status, "verified") == 0);
            SV_CHECK("happy: failure vin null", vin == -1);
        }
        SV_CHECK("happy: next step IDLE",
                 script_validate_stage_step_once() == JOB_IDLE);
        sv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("script_invalid: setup",
                 sv_setup("invalid", 3, -1, dir, sizeof(dir), &ms, &sc) == 0);
        sc.invalid_height = 1;
        SV_CHECK("script_invalid: drains 3",
                 script_validate_stage_drain(100) == 3);
        SV_CHECK("script_invalid: counter == 1",
                 script_validate_stage_script_invalid_total() == 1);
        int ok = -1, vin = -2; char status[32];
        log_row_at(progress_store_db(), 1, &ok, status, sizeof(status),
                   &vin);
        SV_CHECK("script_invalid: h=1 ok=0", ok == 0);
        SV_CHECK("script_invalid: h=1 status",
                 strcmp(status, "script_invalid") == 0);
        SV_CHECK("script_invalid: first vin 0", vin == 0);
        sv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("upstream_failed: setup",
                 sv_setup("upstream", 3, 2, dir, sizeof(dir), &ms, &sc) == 0);
        SV_CHECK("upstream_failed: drains 3",
                 script_validate_stage_drain(100) == 3);
        SV_CHECK("upstream_failed: counter == 1",
                 script_validate_stage_upstream_failed_total() == 1);
        int ok = -1, vin = -2; char status[32];
        log_row_at(progress_store_db(), 2, &ok, status, sizeof(status),
                   &vin);
        SV_CHECK("upstream_failed: h=2 ok=0", ok == 0);
        SV_CHECK("upstream_failed: h=2 status",
                 strcmp(status, "upstream_failed") == 0);
        sv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("internal_error: setup",
                 sv_setup("internal", 3, -1, dir, sizeof(dir), &ms, &sc) == 0);
        sc.missing_prevout_height = 1;
        SV_CHECK("internal_error: drains 3",
                 script_validate_stage_drain(100) == 3);
        SV_CHECK("internal_error: counter == 1",
                 script_validate_stage_internal_error_total() == 1);
        int ok = -1, vin = -2; char status[32];
        log_row_at(progress_store_db(), 1, &ok, status, sizeof(status),
                   &vin);
        SV_CHECK("internal_error: h=1 ok=0", ok == 0);
        SV_CHECK("internal_error: h=1 status",
                 strcmp(status, "prevout_unresolved") == 0);
        sv_teardown(dir, &ms, &sc);
    }

    {
        /* Regression for wedge-retry: a transient internal_error dry-run
         * (missing prevout / sapling-ctx race) must report
         * dry.internal_error == true so the stale-script repair treats it
         * as retriable rather than a permanent "genuinely invalid" verdict.
         * If this flag were false, the repair would refuse to rewind and H*
         * would terminate on the first transient ok=0 row forever. */
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("internal_error_dry_run: setup",
                 sv_setup("internal_dry", 3, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        sc.missing_prevout_height = 1;
        struct script_validate_dry_run_report dry;
        SV_CHECK("internal_error_dry_run: dry-run flags internal_error",
                 script_validate_stage_dry_run_block(&sc.bodies[1], 1,
                                                     &dry) &&
                 !dry.ok && dry.internal_error);
        sv_teardown(dir, &ms, &sc);
    }

    /* FIX A regression pin (wedge-retry, stage_repair_reducer_frontier_coin.c
     * maybe_repair_stale_script): the repair discriminates a TRANSIENT
     * internal_error (ok=0 + internal_error=true → one-shot rewind, retriable)
     * from a GENUINE consensus failure (ok=0 + internal_error=false →
     * stale_script_repair_genuinely_invalid refusal) on exactly this dry-run
     * contract. The block above pins the transient→rewind side; this block
     * pins the complementary genuine-invalid side: a block whose prevout
     * resolves to a failing script (OP_FALSE) must report internal_error=FALSE
     * so the repair takes the genuinely_invalid refusal branch, NOT the rewind.
     * If a regression ever set internal_error on a real script failure, the
     * repair would wrongly rewind a permanently-invalid block forever,
     * re-introducing the silent wedge class this fix deleted. An end-to-end
     * on-disk rewind fixture is not constructible in this unit-test infra
     * (stage_repair_read_active_block_checked preads real block files), so the
     * load-bearing dry-run contract that DRIVES the branch is pinned instead. */
    {
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("script_invalid_dry_run: setup",
                 sv_setup("invalid_dry", 3, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        sc.invalid_height = 1;   /* prevout resolves to OP_FALSE */
        struct script_validate_dry_run_report dry;
        SV_CHECK("script_invalid_dry_run: dry-run is NOT ok",
                 script_validate_stage_dry_run_block(&sc.bodies[1], 1,
                                                     &dry) &&
                 !dry.ok);
        SV_CHECK("script_invalid_dry_run: internal_error FALSE (genuine)",
                 !dry.internal_error);
        sv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct synth_chain_sv sc;
        test_fmt_tmpdir(dir, sizeof(dir), "script_validate",
                        "bounded_created");
        mkdir_p_sv("./test-tmp");
        mkdir_p_sv(dir);
        SV_CHECK("bounded_created: progress opens",
                 progress_store_open(dir));
        SV_CHECK("bounded_created: chain builds",
                 synth_chain_sv_build(&sc, 3));
        SV_CHECK("bounded_created: block2 spends block1 output",
                 retarget_spend(&sc, 2, 1));
        SV_CHECK("bounded_created: index schema",
                 created_outputs_index_ensure_schema(progress_store_db()));
        SV_CHECK("bounded_created: index block1",
                 created_outputs_index_put_block(progress_store_db(),
                                                 &sc.bodies[1], 1));
        SV_CHECK("bounded_created: seed frontier",
                 seed_coins_frontier(progress_store_db(), 1));
        struct script_validate_dry_run_report dry;
        SV_CHECK("bounded_created: dry-run resolves above-frontier coin",
                 script_validate_stage_dry_run_block(&sc.bodies[2], 2,
                                                     &dry) &&
                 dry.ok && strcmp(dry.status, "verified") == 0);
        synth_chain_sv_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    {
        char dir[256]; struct synth_chain_sv sc;
        test_fmt_tmpdir(dir, sizeof(dir), "script_validate",
                        "bounded_future_coin");
        mkdir_p_sv("./test-tmp");
        mkdir_p_sv(dir);
        SV_CHECK("bounded_future_coin: progress opens",
                 progress_store_open(dir));
        SV_CHECK("bounded_future_coin: chain builds",
                 synth_chain_sv_build(&sc, 3));
        struct uint256 future_txid;
        uint256_set_null(&future_txid);
        future_txid.data[0] = 0x77;
        future_txid.data[31] = 0x88;
        sc.bodies[2].vtx[1].vin[0].prevout.hash = future_txid;
        sc.bodies[2].vtx[1].vin[0].prevout.n = 0;
        transaction_compute_hash(&sc.bodies[2].vtx[1]);
        struct uint256 txids[2] = {
            sc.bodies[2].vtx[0].hash,
            sc.bodies[2].vtx[1].hash,
        };
        sc.bodies[2].header.hashMerkleRoot = compute_merkle_root(txids, 2);
        SV_CHECK("bounded_future_coin: index schema",
                 created_outputs_index_ensure_schema(progress_store_db()));
        SV_CHECK("bounded_future_coin: coin schema",
                 coins_kv_ensure_schema(progress_store_db()));
        unsigned char sc_true[1] = { OP_TRUE };
        SV_CHECK("bounded_future_coin: seed live future coin",
                 coins_kv_add(progress_store_db(), future_txid.data, 0,
                              100000000, 3, false, sc_true,
                              sizeof(sc_true)));
        SV_CHECK("bounded_future_coin: seed frontier ahead of block",
                 seed_coins_frontier(progress_store_db(), 4));
        struct script_validate_dry_run_report dry;
        SV_CHECK("bounded_future_coin: dry-run rejects future coin",
                 script_validate_stage_dry_run_block(&sc.bodies[2], 2,
                                                     &dry) &&
                 !dry.ok &&
                 strcmp(dry.status, "prevout_unresolved") == 0);
        synth_chain_sv_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("idle: setup",
                 sv_setup("idle", 3, -1, dir, sizeof(dir), &ms, &sc) == 0);
        sqlite3_exec(progress_store_db(),
            "UPDATE stage_cursor SET cursor=1 WHERE name='body_persist'",
            NULL, NULL, NULL);
        SV_CHECK("idle: advances one",
                 script_validate_stage_drain(100) == 1);
        SV_CHECK("idle: next step IDLE",
                 script_validate_stage_step_once() == JOB_IDLE);
        SV_CHECK("idle: cursor stays 1",
                 script_validate_stage_cursor() == 1);
        sv_teardown(dir, &ms, &sc);
    }

    {
        SV_CHECK("guard: step_once with no init returns IDLE",
                 script_validate_stage_step_once() == JOB_IDLE);
        SV_CHECK("guard: init(NULL) rejected",
                 !script_validate_stage_init(NULL));
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_sv sc;
        SV_CHECK("dump: setup",
                 sv_setup("dump", 2, -1, dir, sizeof(dir), &ms, &sc) == 0);
        script_validate_stage_drain(100);
        struct json_value v;
        json_init(&v);
        SV_CHECK("dump: returns true",
                 script_validate_dump_state_json(&v, NULL));
        char buf[1024];
        size_t n = json_write(&v, buf, sizeof(buf));
        SV_CHECK("dump: serializes", n > 0 && n < sizeof(buf));
        SV_CHECK("dump: stage_name",
                 strstr(buf, "\"stage_name\":\"script_validate\"") != NULL);
        SV_CHECK("dump: cursor=2",
                 strstr(buf, "\"cursor\":2") != NULL);
        SV_CHECK("dump: verified_total=2",
                 strstr(buf, "\"verified_total\":2") != NULL);
        json_free(&v);
        sv_teardown(dir, &ms, &sc);
    }

    printf("script_validate_stage tests: %s\n",
           failures ? "FAILED" : "PASSED");
    return failures;
}
