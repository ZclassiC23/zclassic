/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Unit tests for Wave S S-7 proof_validate stage. */

#include "test/test_helpers.h"

#include "bloom/merkle.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "jobs/proof_validate_stage.h"
#include "sapling/params_init.h"
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

#define PV_CHECK(name, expr) do { \
    printf("proof_validate: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

enum pv_fail_kind {
    PV_FAIL_NONE = 0,
    PV_FAIL_SAPLING_SPEND,
    PV_FAIL_SAPLING_OUTPUT,
    PV_FAIL_SPROUT_GROTH16,
    PV_FAIL_SPROUT_PHGR13,
    PV_FAIL_BINDING_SIG,
    PV_FAIL_INTERNAL,
};

struct synth_chain_pv {
    struct block_index *blocks;
    struct uint256     *hashes;
    struct block       *bodies;
    int                 n;
    int                 fail_height;
    enum pv_fail_kind   fail_kind;
};

static int mkdir_p_pv(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static bool make_shielded_tx(struct transaction *tx, int h)
{
    transaction_init(tx);
    tx->overwintered = true;
    tx->version = SAPLING_TX_VERSION;
    tx->version_group_id = SAPLING_VERSION_GROUP_ID;
    tx->num_shielded_spend = 1;
    tx->v_shielded_spend = zcl_calloc(1, sizeof(struct spend_description),
                                      "pv_spend");
    tx->num_shielded_output = 1;
    tx->v_shielded_output = zcl_calloc(1, sizeof(struct output_description),
                                       "pv_output");
    tx->num_joinsplit = 2;
    tx->v_joinsplit = zcl_calloc(2, sizeof(struct js_description),
                                 "pv_joinsplit");
    if (!tx->v_shielded_spend || !tx->v_shielded_output || !tx->v_joinsplit)
        return false;

    tx->v_joinsplit[0].use_groth = true;
    tx->v_joinsplit[1].use_groth = false;
    memset(tx->joinsplit_pubkey.data, h, 32);
    memset(tx->binding_sig, 0x42, 64);
    transaction_compute_hash(tx);
    return true;
}

static bool make_body(struct synth_chain_pv *sc, int h)
{
    struct block *b = &sc->bodies[h];
    block_init(b);
    b->header.nVersion = 4;
    b->header.nTime = (uint32_t)(1700001000u + (uint32_t)h);
    b->header.nBits = 0x1f07ffff;
    b->num_vtx = 1;
    b->vtx = zcl_calloc(1, sizeof(struct transaction), "pv_tx");
    if (!b->vtx) return false;
    if (!make_shielded_tx(&b->vtx[0], h)) return false;
    struct uint256 txids[1] = { b->vtx[0].hash };
    b->header.hashMerkleRoot = compute_merkle_root(txids, 1);
    return true;
}

static bool synth_chain_pv_build(struct synth_chain_pv *sc, int n)
{
    memset(sc, 0, sizeof(*sc));
    sc->fail_height = -1;
    sc->fail_kind = PV_FAIL_NONE;
    sc->blocks = zcl_calloc((size_t)n, sizeof(struct block_index),
                            "pv_blocks");
    sc->hashes = zcl_calloc((size_t)n, sizeof(struct uint256),
                            "pv_hashes");
    sc->bodies = zcl_calloc((size_t)n, sizeof(struct block),
                            "pv_bodies");
    if (!sc->blocks || !sc->hashes || !sc->bodies)
        return false;
    for (int i = 0; i < n; i++) {
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

static void synth_chain_pv_free(struct synth_chain_pv *sc)
{
    if (sc->bodies) {
        for (int i = 0; i < sc->n; i++)
            block_free(&sc->bodies[i]);
    }
    free(sc->blocks);
    free(sc->hashes);
    free(sc->bodies);
    memset(sc, 0, sizeof(*sc));
}

static bool fake_reader(struct block *out, const struct block_index *bi,
                        const char *datadir, void *user)
{
    (void)datadir;
    struct synth_chain_pv *sc = user;
    if (!out || !bi || !sc || bi->nHeight < 0 || bi->nHeight >= sc->n)
        return false;
    return test_block_copy(out, &sc->bodies[bi->nHeight], "pv_tx_copy");
}

static const char *fail_kind_name(enum pv_fail_kind k)
{
    switch (k) {
    case PV_FAIL_SAPLING_SPEND:   return "sapling_spend";
    case PV_FAIL_SAPLING_OUTPUT:  return "sapling_output";
    case PV_FAIL_SPROUT_GROTH16:  return "sprout_groth16";
    case PV_FAIL_SPROUT_PHGR13:   return "sprout_phgr13";
    case PV_FAIL_BINDING_SIG:     return "binding_sig";
    case PV_FAIL_INTERNAL:        return "sapling_ctx";
    case PV_FAIL_NONE:
    default:                      return NULL;
    }
}

static bool fake_tx_verifier(const struct transaction *tx, int height,
                             struct proof_validate_tx_report *out,
                             void *user)
{
    struct synth_chain_pv *sc = user;
    memset(out, 0, sizeof(*out));
    out->ok = true;
    out->sapling_spends_total = tx ? tx->num_shielded_spend : 0;
    out->sapling_outputs_total = tx ? tx->num_shielded_output : 0;
    out->sprout_joinsplits_total = tx ? tx->num_joinsplit : 0;
    if (!sc || height != sc->fail_height || sc->fail_kind == PV_FAIL_NONE)
        return true;
    out->ok = false;
    out->internal_error = (sc->fail_kind == PV_FAIL_INTERNAL);
    out->first_failure_proof_type = fail_kind_name(sc->fail_kind);
    return true;
}

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

static bool ensure_params_loaded_pv(void)
{
    if (sapling_params_loaded())
        return true;
    const char *home = getenv("HOME");
    char params_dir[512];
    snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
             home ? home : ".");
    return sapling_init_params(params_dir);
}

static bool seed_script_validate(sqlite3 *db,
                                 const struct synth_chain_pv *sc,
                                 int upstream_fail_height)
{
    if (!exec_sql(db,
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height             INTEGER PRIMARY KEY,"
        "  status             TEXT    NOT NULL,"
        "  ok                 INTEGER NOT NULL,"
        "  tx_count           INTEGER NOT NULL,"
        "  input_count        INTEGER NOT NULL,"
        "  first_failure_txid BLOB,"
        "  first_failure_vin  INTEGER,"
        "  block_hash         BLOB,"
        "  validated_at       INTEGER NOT NULL"
        ")"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO script_validate_log "
        "(height, status, ok, tx_count, input_count, block_hash, validated_at) "
        "VALUES (?, ?, ?, 1, 1, ?, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h < sc->n; h++) {
        int ok = (h == upstream_fail_height) ? 0 : 1;
        sqlite3_bind_int(st, 1, h);
        sqlite3_bind_text(st, 2, ok ? "verified" : "script_invalid",
                          -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, ok);
        sqlite3_bind_blob(st, 4, sc->hashes[h].data, 32, SQLITE_STATIC);
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
        "VALUES('script_validate', ?, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, sc->n);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool set_script_validate_hash(sqlite3 *db, int height,
                                     const struct uint256 *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE script_validate_log SET block_hash=? WHERE height=?",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    int bind_rc = hash ? sqlite3_bind_blob(st, 1, hash->data, 32,
                                           SQLITE_STATIC)
                       : sqlite3_bind_null(st, 1);
    bool ok = bind_rc == SQLITE_OK &&
              sqlite3_bind_int(st, 2, height) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool log_row_at(sqlite3 *db, int height, int *out_ok,
                       char *out_status, size_t status_size,
                       char *out_type, size_t type_size)
{
    *out_ok = -1;
    if (out_status && status_size) out_status[0] = 0;
    if (out_type && type_size) out_type[0] = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, status, first_failure_proof_type "
        "FROM proof_validate_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt && out_status && status_size)
            snprintf(out_status, status_size, "%s", (const char *)txt);
        const unsigned char *typ = sqlite3_column_text(st, 2);
        if (typ && out_type && type_size)
            snprintf(out_type, type_size, "%s", (const char *)typ);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

static int pv_setup(const char *tag, int n, int upstream_fail_height,
                    char *dir_out, size_t dir_out_size,
                    struct main_state *ms, struct synth_chain_pv *sc)
{
    test_fmt_tmpdir(dir_out, dir_out_size, "proof_validate", tag);
    mkdir_p_pv("./test-tmp");
    mkdir_p_pv(dir_out);
    if (!progress_store_open(dir_out)) return 1;

    memset(ms, 0, sizeof(*ms));
    active_chain_init(&ms->chain_active);
    if (!synth_chain_pv_build(sc, n)) return 2;
    active_chain_move_window_tip(&ms->chain_active, &sc->blocks[n - 1]);

    if (!seed_script_validate(progress_store_db(), sc, upstream_fail_height))
        return 3;
    if (!proof_validate_stage_init(ms)) return 4;
    proof_validate_stage_set_reader(fake_reader, sc);
    proof_validate_stage_set_tx_verifier(fake_tx_verifier, sc);
    return 0;
}

static void pv_teardown(const char *dir, struct main_state *ms,
                        struct synth_chain_pv *sc)
{
    proof_validate_stage_shutdown();
    active_chain_free(&ms->chain_active);
    synth_chain_pv_free(sc);
    progress_store_close();
    test_cleanup_tmpdir(dir);
}

static void run_invalid_case(int *failures_out, enum pv_fail_kind kind,
                             const char *expected_type,
                             uint64_t (*counter)(void))
{
    int failures = 0;
    char dir[256]; struct main_state ms; struct synth_chain_pv sc;
    PV_CHECK("invalid: setup",
             pv_setup(expected_type, 3, -1, dir, sizeof(dir), &ms, &sc) == 0);
    sc.fail_height = 1;
    sc.fail_kind = kind;
    PV_CHECK("invalid: drains 3", proof_validate_stage_drain(100) == 3);
    PV_CHECK("invalid: proof_invalid_total == 1",
             proof_validate_stage_proof_invalid_total() == 1);
    PV_CHECK("invalid: type counter == 1", counter() == 1);
    int ok = -1; char status[32]; char type[32];
    log_row_at(progress_store_db(), 1, &ok, status, sizeof(status),
               type, sizeof(type));
    PV_CHECK("invalid: h=1 ok=0", ok == 0);
    PV_CHECK("invalid: h=1 status", strcmp(status, "proof_invalid") == 0);
    PV_CHECK("invalid: failure type", strcmp(type, expected_type) == 0);
    pv_teardown(dir, &ms, &sc);
    *failures_out += failures;
}

int test_proof_validate_stage(void);
int test_proof_validate_stage(void)
{
    printf("\n=== proof_validate_stage tests ===\n");
    int failures = 0;

    blocker_module_init();

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        PV_CHECK("params_missing: setup",
                 pv_setup("params_missing", 1, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        sapling_free_params();
        proof_validate_stage_set_tx_verifier(NULL, NULL);
        /* CS-PROOF-TRANSIENT: during the NORMAL background param-load window the
         * LOADER has not named the permanent blocker, so proof_validate must HOLD
         * (JOB_IDLE) and re-derive next tick — it must NOT name a permanent
         * blocker of its own (that would wedge a transient load window). */
        blocker_clear("params_missing");
        PV_CHECK("params_missing: transient load window holds (JOB_IDLE)",
                 proof_validate_stage_step_once() == JOB_IDLE);
        PV_CHECK("params_missing: cursor stays 0 (idle)",
                 proof_validate_stage_cursor() == 0);
        int ok = -1; char status[32]; char type[32];
        PV_CHECK("params_missing: no poisoned row (idle)",
                 !log_row_at(progress_store_db(), 0, &ok, status,
                             sizeof(status), type, sizeof(type)));
        /* The LOADER is the authority: once it declares the PERMANENT
         * params_missing blocker (a genuine corrupt/parse failure),
         * proof_validate RE-SURFACES it as JOB_BLOCKED — still no poisoned row. */
        struct blocker_record rec;
        PV_CHECK("params_missing: loader names blocker",
                 blocker_init(&rec, "params_missing", "crypto.params",
                              BLOCKER_PERMANENT, "test: params corrupt") &&
                 blocker_set(&rec) == 0);
        PV_CHECK("params_missing: shielded block blocks (named blocker)",
                 proof_validate_stage_step_once() == JOB_BLOCKED);
        PV_CHECK("params_missing: cursor stays 0 (blocked)",
                 proof_validate_stage_cursor() == 0);
        PV_CHECK("params_missing: still no poisoned row",
                 !log_row_at(progress_store_db(), 0, &ok, status,
                             sizeof(status), type, sizeof(type)));
        blocker_clear("params_missing");
        pv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        blocker_clear(PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID);
        PV_CHECK("stale_hash: setup",
                 pv_setup("stale_hash", 2, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        PV_CHECK("stale_hash: malformed verdict fails closed and named",
                 exec_sql(progress_store_db(),
                     "UPDATE script_validate_log SET ok=2 WHERE height=0") &&
                     proof_validate_stage_step_once() == JOB_BLOCKED &&
                     proof_validate_stage_cursor() == 0 &&
                     blocker_exists(
                         PROOF_VALIDATE_INVALID_UPSTREAM_BLOCKER_ID));
        PV_CHECK("stale_hash: restore canonical verdict",
                 exec_sql(progress_store_db(),
                     "UPDATE script_validate_log SET ok=1 WHERE height=0"));
        PV_CHECK("stale_hash: text-typed hash fails closed",
                 exec_sql(progress_store_db(),
                     "UPDATE script_validate_log SET block_hash="
                     "'12345678901234567890123456789012' WHERE height=0") &&
                     proof_validate_stage_step_once() == JOB_IDLE &&
                     proof_validate_stage_cursor() == 0 &&
                     blocker_exists(
                         PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID));
        PV_CHECK("stale_hash: hashless receipt fails closed",
                 set_script_validate_hash(progress_store_db(), 0, NULL) &&
                     proof_validate_stage_step_once() == JOB_IDLE &&
                     proof_validate_stage_cursor() == 0 &&
                     blocker_exists(
                         PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID));
        struct uint256 foreign = sc.hashes[0];
        foreign.data[0] ^= 0xff;
        PV_CHECK("stale_hash: seed foreign script receipt",
                 set_script_validate_hash(progress_store_db(), 0, &foreign));
        PV_CHECK("stale_hash: proof stage parks without reading branch",
                 proof_validate_stage_step_once() == JOB_IDLE &&
                     proof_validate_stage_cursor() == 0);
        PV_CHECK("stale_hash: dependency is named",
                 blocker_exists(
                     PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID) &&
                 blocker_class_for(
                     PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID) ==
                     BLOCKER_DEPENDENCY);
        PV_CHECK("stale_hash: bind script receipt to selected branch",
                 set_script_validate_hash(progress_store_db(), 0,
                                          &sc.hashes[0]));
        PV_CHECK("stale_hash: rebound receipt advances",
                 proof_validate_stage_step_once() == JOB_ADVANCED &&
                     proof_validate_stage_cursor() == 1);
        PV_CHECK("stale_hash: dependency clears on rebind",
                 !blocker_exists(
                     PROOF_VALIDATE_STALE_UPSTREAM_HASH_BLOCKER_ID));
        pv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        PV_CHECK("happy: setup",
                 pv_setup("happy", 2, -1, dir, sizeof(dir), &ms, &sc) == 0);
        PV_CHECK("happy: drains 2", proof_validate_stage_drain(100) == 2);
        PV_CHECK("happy: cursor at 2", proof_validate_stage_cursor() == 2);
        PV_CHECK("happy: verified_total == 2",
                 proof_validate_stage_verified_total() == 2);
        PV_CHECK("happy: sapling spends verified == 2",
                 proof_validate_stage_sapling_spends_verified_total() == 2);
        PV_CHECK("happy: sapling outputs verified == 2",
                 proof_validate_stage_sapling_outputs_verified_total() == 2);
        PV_CHECK("happy: sprout groth16 verified == 2",
                 proof_validate_stage_sprout_groth16_verified_total() == 2);
        PV_CHECK("happy: sprout phgr13 verified == 2",
                 proof_validate_stage_sprout_phgr13_verified_total() == 2);
        PV_CHECK("happy: binding sig verified == 2",
                 proof_validate_stage_binding_sig_verified_total() == 2);
        for (int h = 0; h < 2; h++) {
            int ok = -1; char status[32]; char type[32];
            log_row_at(progress_store_db(), h, &ok, status, sizeof(status),
                       type, sizeof(type));
            PV_CHECK("happy: row ok=1", ok == 1);
            PV_CHECK("happy: row status verified",
                     strcmp(status, "verified") == 0);
            PV_CHECK("happy: failure type null", type[0] == 0);
        }
        PV_CHECK("happy: next step IDLE",
                 proof_validate_stage_step_once() == JOB_IDLE);
        pv_teardown(dir, &ms, &sc);
    }

    run_invalid_case(&failures, PV_FAIL_SAPLING_SPEND, "sapling_spend",
                     proof_validate_stage_sapling_spends_failed_total);
    run_invalid_case(&failures, PV_FAIL_SAPLING_OUTPUT, "sapling_output",
                     proof_validate_stage_sapling_outputs_failed_total);
    run_invalid_case(&failures, PV_FAIL_SPROUT_GROTH16, "sprout_groth16",
                     proof_validate_stage_sprout_groth16_failed_total);
    run_invalid_case(&failures, PV_FAIL_SPROUT_PHGR13, "sprout_phgr13",
                     proof_validate_stage_sprout_phgr13_failed_total);
    run_invalid_case(&failures, PV_FAIL_BINDING_SIG, "binding_sig",
                     proof_validate_stage_binding_sig_failed_total);

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        PV_CHECK("joinsplit_sig: setup",
                 pv_setup("joinsplit_sig", 1, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        proof_validate_stage_set_tx_verifier(NULL, NULL);
        PV_CHECK("joinsplit_sig: params loaded", ensure_params_loaded_pv());
        PV_CHECK("joinsplit_sig: drains 1",
                 proof_validate_stage_drain(100) == 1);
        PV_CHECK("joinsplit_sig: proof_invalid_total == 1",
                 proof_validate_stage_proof_invalid_total() == 1);
        int ok = -1; char status[32]; char type[32];
        log_row_at(progress_store_db(), 0, &ok, status, sizeof(status),
                   type, sizeof(type));
        PV_CHECK("joinsplit_sig: h=0 ok=0", ok == 0);
        PV_CHECK("joinsplit_sig: h=0 status",
                 strcmp(status, "proof_invalid") == 0);
        PV_CHECK("joinsplit_sig: failure type",
                 strcmp(type, "joinsplit_sig") == 0);
        pv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        PV_CHECK("upstream_failed: setup",
                 pv_setup("upstream", 3, 2, dir, sizeof(dir), &ms, &sc) == 0);
        PV_CHECK("upstream_failed: drains 3",
                 proof_validate_stage_drain(100) == 3);
        PV_CHECK("upstream_failed: counter == 1",
                 proof_validate_stage_upstream_failed_total() == 1);
        int ok = -1; char status[32]; char type[32];
        log_row_at(progress_store_db(), 2, &ok, status, sizeof(status),
                   type, sizeof(type));
        PV_CHECK("upstream_failed: h=2 ok=0", ok == 0);
        PV_CHECK("upstream_failed: h=2 status",
                 strcmp(status, "upstream_failed") == 0);
        pv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        PV_CHECK("internal_error: setup",
                 pv_setup("internal", 3, -1, dir, sizeof(dir), &ms, &sc) == 0);
        sc.fail_height = 1;
        sc.fail_kind = PV_FAIL_INTERNAL;
        /* TL-2: a transient internal_error (e.g. a sapling_ctx allocation failure
         * under memory pressure) is NOT a permanent reject. The stage HOLDS the
         * cursor at the hole — no terminal ok=0 row, no advance — and re-derives
         * next tick. So the drain stops at the hole (only h=0 advances) and NO row
         * is written at h=1. */
        PV_CHECK("internal_error: drains only up to the hole",
                 proof_validate_stage_drain(100) == 1);
        PV_CHECK("internal_error: cursor held at the hole (1)",
                 proof_validate_stage_cursor() == 1);
        PV_CHECK("internal_error: counter == 1 (one held height)",
                 proof_validate_stage_internal_error_total() == 1);
        int ok = -1; char status[32]; char type[32];
        bool row = log_row_at(progress_store_db(), 1, &ok, status,
                              sizeof(status), type, sizeof(type));
        PV_CHECK("internal_error: no terminal row written at the hole",
                 !row && ok == -1);
        /* Re-tick within budget: still HOLDS (JOB_IDLE), never advances, never
         * writes a row, counter stays 1 (paged once per held height). */
        PV_CHECK("internal_error: re-tick still holds (JOB_IDLE)",
                 proof_validate_stage_step_once() == JOB_IDLE);
        PV_CHECK("internal_error: counter still 1 (same held height)",
                 proof_validate_stage_internal_error_total() == 1);
        pv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        PV_CHECK("idle: setup",
                 pv_setup("idle", 3, -1, dir, sizeof(dir), &ms, &sc) == 0);
        sqlite3_exec(progress_store_db(),
            "UPDATE stage_cursor SET cursor=1 WHERE name='script_validate'",
            NULL, NULL, NULL);
        PV_CHECK("idle: advances one", proof_validate_stage_drain(100) == 1);
        PV_CHECK("idle: next step IDLE",
                 proof_validate_stage_step_once() == JOB_IDLE);
        PV_CHECK("idle: cursor stays 1",
                 proof_validate_stage_cursor() == 1);
        pv_teardown(dir, &ms, &sc);
    }

    {
        PV_CHECK("guard: step_once with no init returns IDLE",
                 proof_validate_stage_step_once() == JOB_IDLE);
        PV_CHECK("guard: init(NULL) rejected",
                 !proof_validate_stage_init(NULL));
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_pv sc;
        PV_CHECK("dump: setup",
                 pv_setup("dump", 2, -1, dir, sizeof(dir), &ms, &sc) == 0);
        proof_validate_stage_drain(100);
        struct json_value v;
        json_init(&v);
        PV_CHECK("dump: returns true",
                 proof_validate_dump_state_json(&v, NULL));
        char buf[1024];
        size_t n = json_write(&v, buf, sizeof(buf));
        PV_CHECK("dump: serializes", n > 0 && n < sizeof(buf));
        PV_CHECK("dump: stage_name",
                 strstr(buf, "\"stage_name\":\"proof_validate\"") != NULL);
        PV_CHECK("dump: cursor=2", strstr(buf, "\"cursor\":2") != NULL);
        PV_CHECK("dump: verified_total=2",
                 strstr(buf, "\"verified_total\":2") != NULL);
        json_free(&v);
        pv_teardown(dir, &ms, &sc);
    }

    printf("proof_validate_stage tests: %s\n",
           failures ? "FAILED" : "PASSED");
    return failures;
}
