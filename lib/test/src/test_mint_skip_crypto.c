/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_mint_skip_crypto — proves the OFFLINE FAST-MINT state-only fold yields
 * the IDENTICAL coins_kv set (same SHA3 commitment + count) as the
 * full-validated fold, on a small synthetic chain.
 *
 * It runs the SAME three-stage tail of the staged pipeline — script_validate ->
 * proof_validate -> utxo_apply — TWICE on one synthetic chain, in two isolated
 * datadirs:
 *
 *   Run A (full validation): mint_skip_crypto OFF (the default). script_validate
 *     runs the REAL per-input verify_script (the OP_TRUE prevouts pass);
 *     proof_validate runs the REAL proof verifier (the blocks carry no shielded
 *     proofs, so it verifies vacuously). utxo_apply folds the bodies.
 *   Run B (state-only): mint_skip_crypto ON. script_validate SKIPS verify_script
 *     and writes the verified row directly; proof_validate SKIPS the proof
 *     verifier and writes the verified row directly. utxo_apply folds the SAME
 *     bodies — UNCHANGED.
 *
 * EXPECT: coins_kv_commitment(A) == coins_kv_commitment(B) AND count(A) ==
 * count(B). This is the load-bearing fact: state-only == validated for the UTXO
 * set, so the mint's SHA3==checkpoint hard-assert is satisfied by the exact same
 * set either way.
 *
 * EQUIVALENCE FLOOR (the toggle is the only difference): Run A actually ran
 * crypto (inputs_verified_total > 0); Run B skipped it (inputs_verified_total
 * == 0). This proves the OFF path runs real verify_script and the ON path does
 * not — so a normal boot (toggle unset) runs full crypto. */

#include "test/test_helpers.h"

#include "bloom/merkle.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "jobs/mint_skip_crypto.h"
#include "jobs/refold_progress.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "chain/checkpoints.h"
#include "config/boot.h"
#include "config/mint_anchor_progress.h"
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

#define MSC_CHECK(name, expr) do { \
    printf("mint_skip_crypto: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* One synthetic chain shared by both runs, modeled on test_utxo_apply_stage so
 * the utxo_apply STATE fold ACCEPTS every block (no coinbase-protect, no
 * subsidy-ceiling reject) — otherwise both runs would reject identically and
 * the equivalence would be vacuous. Each block has:
 *   vtx[0] coinbase paying a tiny OP_TRUE output (50+h, well under subsidy);
 *   vtx[1] spends an EXTERNAL OP_TRUE prevout (ext[h], value 1000+h) with an
 *          empty scriptSig (valid against OP_TRUE) into a 900+h OP_TRUE output.
 * The external prevout is resolved by BOTH the script_validate prevout resolver
 * (so verify_script passes in the full run) AND the utxo_apply lookup (so the
 * state fold resolves the spent value), exactly as test_utxo_apply_stage does. */
struct external_utxo {
    struct uint256 txid;
    uint32_t       vout;
    int64_t        value;
};
struct synth_chain {
    struct block_index   *blocks;
    struct uint256       *hashes;
    struct block         *bodies;
    struct external_utxo *ext;       /* the external prevout spent at each h */
    int                   n;
};

static void synthetic_ext_txid(struct uint256 *out, int h)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0x80 + h);
    out->data[1] = 0x09;
}

static int mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static bool make_body(struct synth_chain *sc, int h)
{
    struct block *b = &sc->bodies[h];
    block_init(b);
    b->header.nVersion = 4;
    b->header.nTime = (uint32_t)(1700000000u + (uint32_t)h);
    b->header.nBits = 0x1f07ffff;
    b->num_vtx = 2;
    b->vtx = zcl_calloc(2, sizeof(struct transaction), "msc_tx");
    if (!b->vtx) return false;

    /* The external prevout this block's spend consumes (OP_TRUE, 1000+h). */
    synthetic_ext_txid(&sc->ext[h].txid, h);
    sc->ext[h].vout  = 0;
    sc->ext[h].value = 1000 + h;

    /* vtx[0]: coinbase, one OP_TRUE output worth 50+h (well under subsidy). */
    transaction_init(&b->vtx[0]);
    if (!transaction_alloc(&b->vtx[0], 1, 1)) return false;
    outpoint_set_null(&b->vtx[0].vin[0].prevout);
    script_push_data(&b->vtx[0].vin[0].script_sig, (const unsigned char *)&h,
                     sizeof(h));
    b->vtx[0].vout[0].value = 50 + h;
    script_init(&b->vtx[0].vout[0].script_pub_key);
    script_push_op(&b->vtx[0].vout[0].script_pub_key, OP_TRUE);
    transaction_compute_hash(&b->vtx[0]);

    /* vtx[1]: spend the EXTERNAL OP_TRUE prevout with empty scriptSig (passes
     * OP_TRUE) into a 900+h OP_TRUE output (input 1000+h leaves a fee). */
    transaction_init(&b->vtx[1]);
    if (!transaction_alloc(&b->vtx[1], 1, 1)) return false;
    b->vtx[1].vin[0].prevout.hash = sc->ext[h].txid;
    b->vtx[1].vin[0].prevout.n = sc->ext[h].vout;
    script_init(&b->vtx[1].vin[0].script_sig);
    b->vtx[1].vout[0].value = 900 + h;
    script_init(&b->vtx[1].vout[0].script_pub_key);
    script_push_op(&b->vtx[1].vout[0].script_pub_key, OP_TRUE);
    transaction_compute_hash(&b->vtx[1]);

    struct uint256 txids[2] = { b->vtx[0].hash, b->vtx[1].hash };
    b->header.hashMerkleRoot = compute_merkle_root(txids, 2);
    return true;
}

static bool synth_chain_build(struct synth_chain *sc, int n)
{
    memset(sc, 0, sizeof(*sc));
    sc->blocks = zcl_calloc((size_t)n, sizeof(struct block_index), "msc_bi");
    sc->hashes = zcl_calloc((size_t)n, sizeof(struct uint256), "msc_h");
    sc->bodies = zcl_calloc((size_t)n, sizeof(struct block), "msc_b");
    sc->ext    = zcl_calloc((size_t)n, sizeof(struct external_utxo), "msc_ext");
    if (!sc->blocks || !sc->hashes || !sc->bodies || !sc->ext)
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

static void synth_chain_free(struct synth_chain *sc)
{
    if (sc->bodies)
        for (int i = 0; i < sc->n; i++)
            block_free(&sc->bodies[i]);
    free(sc->blocks);
    free(sc->hashes);
    free(sc->bodies);
    free(sc->ext);
    memset(sc, 0, sizeof(*sc));
}

static bool fake_reader(struct block *out, const struct block_index *bi,
                        const char *datadir, void *user)
{
    (void)datadir;
    struct synth_chain *sc = user;
    if (!out || !bi || !sc || bi->nHeight < 0 || bi->nHeight >= sc->n)
        return false;
    return test_block_copy(out, &sc->bodies[bi->nHeight], "msc_copy");
}

/* script_validate prevout resolver: the spend's prevout is the EXTERNAL
 * OP_TRUE coin ext[h] (so verify_script passes against an empty scriptSig). */
static bool fake_prevout(const struct outpoint *prevout, struct tx_out *out,
                         void *user)
{
    struct synth_chain *sc = user;
    if (!prevout || !out || !sc) return false;
    for (int h = 0; h < sc->n; h++) {
        if (uint256_eq(&prevout->hash, &sc->ext[h].txid) &&
            prevout->n == sc->ext[h].vout) {
            tx_out_set_null(out);
            out->value = sc->ext[h].value;
            script_init(&out->script_pub_key);
            script_push_op(&out->script_pub_key, OP_TRUE);
            return true;
        }
    }
    return false;
}

/* utxo_apply lookup: resolve the EXTERNAL spent prevout's value/script so the
 * state fold accepts the spend (the coin's pre-image for the inverse delta). */
static bool fake_lookup(const struct uint256 *txid, uint32_t vout,
                        struct utxo_apply_lookup *out, void *user)
{
    struct synth_chain *sc = user;
    memset(out, 0, sizeof(*out));
    if (!sc) return true;
    for (int h = 0; h < sc->n; h++) {
        if (uint256_eq(&sc->ext[h].txid, txid) && sc->ext[h].vout == vout) {
            out->found = true;
            out->value = sc->ext[h].value;
            return true;
        }
    }
    return true;
}

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

/* Seed body_persist_log + its cursor so script_validate sees an ok=1 upstream
 * for every height (the stages it feeds are the ones under test). */
static bool seed_body_persist(sqlite3 *db, int n)
{
    if (!exec_sql(db,
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "  height INTEGER PRIMARY KEY, source TEXT NOT NULL, "
        "  ok INTEGER NOT NULL, persisted_at INTEGER NOT NULL)"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO body_persist_log "
        "(height, source, ok, persisted_at) VALUES (?, 'verified', 1, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h < n; h++) {
        sqlite3_bind_int(st, 1, h);
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return false; }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
        "VALUES('body_persist', ?, 1)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, n);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Run the script_validate -> proof_validate -> utxo_apply tail over `n` heights
 * in a fresh datadir. `skip` selects full-validation (false) vs state-only
 * (true). On success writes the resulting commitment + count + whether real
 * script crypto ran into the out params. Returns the count of script_validate
 * heights drained (== n on a clean fold). */
static int run_fold(const char *tag, struct synth_chain *sc, bool skip,
                    uint8_t commit_out[32], int64_t *count_out,
                    uint64_t *inputs_verified_out)
{
    char dir[256];
    struct main_state ms;
    test_fmt_tmpdir(dir, sizeof(dir), "mint_skip_crypto", tag);
    mkdir_p("./test-tmp");
    mkdir_p(dir);
    int drained = -1;
    if (!progress_store_open(dir)) return -1;

    memset(&ms, 0, sizeof(ms));
    active_chain_init(&ms.chain_active);
    active_chain_move_window_tip(&ms.chain_active, &sc->blocks[sc->n - 1]);

    /* The toggle is process-global; set it BEFORE init/drive and clear after. */
    mint_skip_crypto_set(skip);

    if (!seed_body_persist(progress_store_db(), sc->n)) goto done;

    if (!script_validate_stage_init(&ms)) goto done;
    script_validate_stage_set_reader(fake_reader, sc);
    script_validate_stage_set_prevout_resolver(fake_prevout, sc);
    if (!proof_validate_stage_init(&ms)) goto done;
    proof_validate_stage_set_reader(fake_reader, sc);
    if (!utxo_apply_stage_init(&ms)) goto done;
    utxo_apply_stage_set_reader(fake_reader, sc);
    utxo_apply_stage_set_lookup(fake_lookup, sc);

    drained = script_validate_stage_drain(1000);
    (void)proof_validate_stage_drain(1000);
    (void)utxo_apply_stage_drain(1000);

    if (inputs_verified_out)
        *inputs_verified_out = script_validate_stage_inputs_verified_total();
    if (count_out)
        *count_out = coins_kv_count(progress_store_db());
    if (commit_out)
        (void)coins_kv_commitment(progress_store_db(), commit_out);

done:
    utxo_apply_stage_shutdown();
    proof_validate_stage_shutdown();
    script_validate_stage_shutdown();
    mint_skip_crypto_set(false);        /* never leak the toggle to other tests */
    active_chain_free(&ms.chain_active);
    progress_store_close();
    test_cleanup_tmpdir(dir);
    return drained;
}

static char *read_source_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = zcl_malloc((size_t)len + 1, "msc_source");
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

static bool msc_set_applied_height(sqlite3 *db, int32_t h)
{
    char *err = NULL;
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) == SQLITE_OK;
    if (ok)
        ok = coins_kv_set_applied_height_in_tx(db, h);
    if (ok)
        ok = sqlite3_exec(db, "COMMIT", NULL, NULL, &err) == SQLITE_OK;
    if (!ok)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (err)
        sqlite3_free(err);
    return ok;
}

static struct sha3_utxo_checkpoint msc_checkpoint(int32_t height,
                                                  uint64_t count,
                                                  uint8_t salt)
{
    struct sha3_utxo_checkpoint cp;
    memset(&cp, 0, sizeof(cp));
    cp.height = height;
    cp.utxo_count = count;
    cp.total_supply = 123456;
    for (int i = 0; i < 32; i++) {
        cp.sha3_hash[i] = (uint8_t)(salt + i);
        cp.block_hash[i] = (uint8_t)(0x80 + salt + i);
    }
    return cp;
}

static void msc_clear_refold_progress(sqlite3 *db)
{
    if (db) {
        (void)progress_meta_delete(db, REFOLD_IN_PROGRESS_KEY);
        (void)progress_meta_delete(db, REFOLD_FROM_ANCHOR_KEY);
        (void)progress_meta_delete(db, REFOLD_FROM_ANCHOR_TARGET_KEY);
        (void)refold_progress_refresh(db);
    }
    refold_progress_test_set_cached(false);
}

static int test_mint_anchor_progress_resume(void)
{
    int failures = 0;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "mint_skip_crypto", "resume_marker");
    mkdir_p("./test-tmp");
    mkdir_p(dir);

    struct sha3_utxo_checkpoint cp = msc_checkpoint(200, 9, 0x11);
    struct sha3_utxo_checkpoint other = msc_checkpoint(200, 9, 0x22);

    bool opened = progress_store_open(dir);
    sqlite3 *db = opened ? progress_store_db() : NULL;
    MSC_CHECK("mint progress: progress store opens", opened && db);
    if (!opened || !db)
        return failures + 1;

    int32_t through = -99;
    bool legacy = true;
    MSC_CHECK("mint progress: no marker does not resume",
              !mint_anchor_progress_can_resume(db, &cp, &through, &legacy));

    MSC_CHECK("mint progress: seed applied frontier",
              msc_set_applied_height(db, 43));
    MSC_CHECK("mint progress: mark in progress",
              mint_anchor_progress_mark(db, &cp));
    through = -99;
    legacy = true;
    MSC_CHECK("mint progress: matching marker resumes",
              mint_anchor_progress_can_resume(db, &cp, &through, &legacy) &&
              through == 42 && !legacy);
    MSC_CHECK("mint progress: checkpoint mismatch refuses resume",
              !mint_anchor_progress_can_resume(db, &other, NULL, NULL));

    MSC_CHECK("mint progress: clear marker",
              mint_anchor_progress_clear(db));
    (void)progress_meta_delete(db, REFOLD_IN_PROGRESS_KEY);
    (void)refold_progress_refresh(db);
    MSC_CHECK("mint progress: cleared marker no longer resumes",
              !mint_anchor_progress_can_resume(db, &cp, NULL, NULL));

    MSC_CHECK("mint progress: legacy refold signal armed",
              refold_progress_mark_started(db));
    through = -99;
    legacy = false;
    MSC_CHECK("mint progress: legacy interrupted fold adopted",
              mint_anchor_progress_can_resume(db, &cp, &through, &legacy) &&
              through == 42 && legacy);
    through = -99;
    legacy = true;
    MSC_CHECK("mint progress: adopted marker resumes normally",
              mint_anchor_progress_can_resume(db, &cp, &through, &legacy) &&
              through == 42 && !legacy);

    MSC_CHECK("mint progress: frontier past anchor recorded",
              msc_set_applied_height(db, cp.height + 2));
    MSC_CHECK("mint progress: marker refuses past-anchor frontier",
              !mint_anchor_progress_can_resume(db, &cp, NULL, NULL));

    msc_clear_refold_progress(db);
    progress_store_close();
    refold_progress_test_set_cached(false);
    test_cleanup_tmpdir(dir);
    return failures;
}

int test_mint_skip_crypto(void);
int test_mint_skip_crypto(void)
{
    int failures = 0;
    printf("\n=== test_mint_skip_crypto: state-only fold == validated fold ===\n");

    blocker_module_init();

    const int N = 5;
    struct synth_chain sc;
    if (!synth_chain_build(&sc, N)) {
        printf("mint_skip_crypto: FAILED to build synthetic chain\n");
        return 1;
    }

    uint8_t commit_full[32] = {0}, commit_skip[32] = {0};
    int64_t count_full = -1, count_skip = -2;
    uint64_t inputs_full = 0, inputs_skip = 999;

    int drained_full = run_fold("full", &sc, /*skip=*/false,
                                commit_full, &count_full, &inputs_full);
    int drained_skip = run_fold("skip", &sc, /*skip=*/true,
                                commit_skip, &count_skip, &inputs_skip);

    /* Both folds walked every height. */
    MSC_CHECK("full validation drains all heights", drained_full == N);
    MSC_CHECK("state-only fold drains all heights", drained_skip == N);

    /* The load-bearing equivalence: the UTXO set is identical. */
    MSC_CHECK("count(full) == count(state-only)",
              count_full == count_skip && count_full > 0);
    MSC_CHECK("coins_kv commitment identical",
              memcmp(commit_full, commit_skip, 32) == 0);

    /* Equivalence floor — the toggle is the ONLY difference: full validation
     * actually ran verify_script (inputs verified > 0); the state-only fold
     * skipped it (0). Proves the OFF path (a normal boot) runs real crypto. */
    MSC_CHECK("full validation ran verify_script (inputs_verified > 0)",
              inputs_full > 0);
    MSC_CHECK("state-only fold SKIPPED verify_script (inputs_verified == 0)",
              inputs_skip == 0);

    /* Sanity: a non-empty set actually folded. Each block creates 2 outputs
     * (coinbase + spend) and spends an EXTERNAL coin (not in this set), so 2
     * coins survive per block → 2*N coins. A non-trivial, non-empty fold makes
     * the commitment-equality above load-bearing (not a vacuous empty==empty). */
    MSC_CHECK("folded a non-trivial UTXO set", count_full == (int64_t)(2 * N));

    char *boot_src = read_source_file("config/src/boot.c");
    char *main_src = read_source_file("src/main.c");
    const char *offline_marker = boot_src
        ? strstr(boot_src, "-mint-anchor: offline reducer stages initialized")
        : NULL;
    const char *services_start = boot_src
        ? strstr(boot_src, "app_init_services(ctx, params, &g_svc)")
        : NULL;
    MSC_CHECK("mint-anchor app_init exits before app_init_services",
              offline_marker && services_start && offline_marker < services_start);

    const char *mint_branch = main_src
        ? strstr(main_src, "if (ctx.mint_anchor) {")
        : NULL;
    const char *offline_shutdown = mint_branch
        ? strstr(mint_branch, "app_shutdown_offline();")
        : NULL;
    const char *mint_return = mint_branch
        ? strstr(mint_branch, "return minted ? 0 : 1;")
        : NULL;
    const char *full_shutdown = mint_branch
        ? strstr(mint_branch, "app_shutdown();")
        : NULL;
    MSC_CHECK("mint-anchor uses offline shutdown",
              offline_shutdown && mint_return && offline_shutdown < mint_return);
    MSC_CHECK("mint-anchor does not call full app_shutdown",
              mint_return && (!full_shutdown || full_shutdown > mint_return));

    const char *offline_shutdown_fn = boot_src
        ? strstr(boot_src, "void app_shutdown_offline(void)")
        : NULL;
    const char *wallet_flush = offline_shutdown_fn
        ? strstr(offline_shutdown_fn, "wallet_sqlite_flush_r(&g_wallet_sqlite")
        : NULL;
    const char *wallet_close = offline_shutdown_fn
        ? strstr(offline_shutdown_fn, "wallet_sqlite_close(&g_wallet_sqlite)")
        : NULL;
    const char *wallet_free_call = offline_shutdown_fn
        ? strstr(offline_shutdown_fn, "wallet_free(&g_wallet)")
        : NULL;
    MSC_CHECK("offline shutdown flushes wallet sqlite before wallet_free",
              wallet_flush && wallet_free_call && wallet_flush < wallet_free_call);
    MSC_CHECK("offline shutdown closes wallet sqlite before wallet_free",
              wallet_close && wallet_free_call && wallet_close < wallet_free_call);

    MSC_CHECK("mint-anchor emits 10k progress heartbeats",
              !boot_mint_anchor_should_log_progress(9999, 3056758) &&
              boot_mint_anchor_should_log_progress(10000, 3056758) &&
              !boot_mint_anchor_should_log_progress(10001, 3056758) &&
              boot_mint_anchor_should_log_progress(20000, 3056758));
    MSC_CHECK("mint-anchor emits final-anchor tail heartbeats",
              !boot_mint_anchor_should_log_progress(3056741, 3056758) &&
              boot_mint_anchor_should_log_progress(3056742, 3056758) &&
              boot_mint_anchor_should_log_progress(3056758, 3056758));
    failures += test_mint_anchor_progress_resume();
    free(boot_src);
    free(main_src);

    synth_chain_free(&sc);

    printf("=== test_mint_skip_crypto complete: %d failure(s) ===\n", failures);
    return failures;
}
