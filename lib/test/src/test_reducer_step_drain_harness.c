/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_reducer_step_drain_harness — a DETERMINISTIC, single-stepped drive of the
 * eight-stage reducer pipeline for ONE self-mined regtest block with NO
 * successor (exactly the `generate 1` scenario). It calls each stage's step_once
 * ONE AT A TIME and asserts that stage's progress.kv log row + cursor, so the
 * exact stage that records a wrong ok=0 is pinpointed in-process — turning the
 * live, run-to-run-contradicting node diagnostics into a reproducible unit test.
 *
 * It runs the PRODUCTION step bodies with NO stubs: real Equihash mining
 * (mine_block_pow, regtest 48,5), real body write/read to disk, the identical
 * *_stage_step_once functions reducer_drain_all_stages calls. The only
 * difference from reducer_ingest_block is single-stepping with an assertion
 * between each stage instead of summing advance counts.
 *
 * Scaffolding (setup / block builder / genesis seed) is lifted from
 * test_reducer_forward_progress_gate.c. Process-globals → opt-in isolated run:
 *   make t ONLY=reducer_step_drain_harness
 */

#include "test/test_helpers.h"

#include "bloom/merkle.h"
#include "chain/chainparams.h"
#include "chain/chainparamsbase.h"
#include "chain/pow.h"
#include "chain/subsidy.h"
#include "consensus/validation.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "domain/consensus/coinbase.h"
#include "mining/miner.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "services/chain_activation_service.h"
#include "services/header_admit_inbox.h"
#include "storage/disk_block_io.h"
#include "storage/event_log.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_helpers.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"
#include "util/util.h"
#include "validation/accept_block_header.h"
#include "validation/chainstate.h"
#include "validation/check_block.h"
#include "validation/main_constants.h"
#include "validation/main_state.h"

#include <errno.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SD_CHECK(name, expr) do {                              \
    printf("reducer_step_drain_harness: %s... ", (name));      \
    if ((expr)) printf("OK\n");                                \
    else { printf("FAIL\n"); failures++; }                     \
} while (0)

static int sd_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* One-coinbase regtest block, real merkle root, regtest powLimit nBits.
 * Identical shape to rfp_build_regtest_block. */
static bool sd_build_regtest_block(struct block *blk, int height,
                                   const struct uint256 *prev_hash,
                                   const struct chain_params *cp,
                                   int64_t *out_coinbase_value)
{
    block_init(blk);
    blk->vtx = zcl_calloc(1, sizeof(struct transaction), "sd_vtx");
    if (!blk->vtx)
        return false;
    blk->num_vtx = 1;

    struct transaction *coinbase = &blk->vtx[0];
    transaction_init(coinbase);
    if (!transaction_alloc(coinbase, 1, 1))
        return false;

    struct script miner_script;
    script_init(&miner_script);
    miner_script.data[0] = 0x76; /* OP_DUP        */
    miner_script.data[1] = 0xa9; /* OP_HASH160    */
    miner_script.data[2] = 0x14; /* push 20       */
    for (int i = 0; i < 20; i++)
        miner_script.data[3 + i] = (unsigned char)(0x10 + i);
    miner_script.data[23] = 0x88; /* OP_EQUALVERIFY */
    miner_script.data[24] = 0xac; /* OP_CHECKSIG    */
    miner_script.size = 25;

    int64_t subsidy = get_block_subsidy(height, &cp->consensus);
    struct domain_consensus_coinbase_inputs cb_in = {
        .n_height     = height,
        .subsidy      = subsidy,
        .total_fees   = 0,
        .miner_script = &miner_script,
        .params       = &cp->consensus,
    };
    struct zcl_result r = domain_consensus_coinbase_build(&cb_in, coinbase);
    if (!r.ok)
        return false;
    if (out_coinbase_value)
        *out_coinbase_value = subsidy;

    struct uint256 txid = blk->vtx[0].hash;
    blk->header.hashMerkleRoot = compute_merkle_root(&txid, 1);
    blk->header.hashPrevBlock = *prev_hash;
    uint256_set_null(&blk->header.hashFinalSaplingRoot);
    blk->header.nTime = 1600000000u + (uint32_t)height;

    struct arith_uint256 pow_limit;
    uint256_to_arith(&pow_limit, &cp->consensus.powLimit);
    blk->header.nBits = arith_uint256_get_compact(&pow_limit, false);
    return true;
}

static bool sd_seed_genesis_utxo_apply_row(sqlite3 *db)
{
    if (!db) return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxo_apply_log "
        "(height,status,ok,spent_count,added_count,total_value_delta,applied_at) "
        "VALUES(0,'verified',1,0,0,0,1)", -1, &st, NULL) != SQLITE_OK)
        return false;
    bool ok = (sqlite3_step(st) == SQLITE_DONE);  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
}

/* Generic "SELECT <col> FROM <table> WHERE height=?" -> int. Returns false if
 * no row / error. Idiom = tf_log_status_at (test_reducer_ingest_e2e.c). */
static bool sd_log_int(sqlite3 *db, const char *table, const char *col,
                       int height, int64_t *out)
{
    if (!db) return false;
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE height=?", col, table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;  // raw-sql-ok:test-readback
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {   // raw-sql-ok:test-readback
        *out = sqlite3_column_int64(st, 0);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

/* Read a TEXT column (status/source) into buf. */
static bool sd_log_text(sqlite3 *db, const char *table, const char *col,
                        int height, char *buf, size_t buflen)
{
    if (!db) return false;
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE height=?", col, table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;  // raw-sql-ok:test-readback
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {   // raw-sql-ok:test-readback
        const unsigned char *t = sqlite3_column_text(st, 0);
        snprintf(buf, buflen, "%s", t ? (const char *)t : "");
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

int test_reducer_step_drain_harness(void);
int test_reducer_step_drain_harness(void)
{
    int failures = 0;

    blocker_module_init();
    chain_params_select(CHAIN_REGTEST);
    const struct chain_params *cp = chain_params_get();

    /* ── hermetic datadir + stores ─────────────────────────────────────── */
    char dir[256];
    sd_mkdir_p("./test-tmp");
    test_fmt_tmpdir(dir, sizeof(dir), "reducer_step_drain_harness", "main");
    sd_mkdir_p(dir);

    SetDataDir(dir);
    ClearDataDirCache();
    char netdir[512];
    GetDataDir(true, netdir, sizeof(netdir));
    sd_mkdir_p(netdir);
    char blocksdir[640];
    snprintf(blocksdir, sizeof(blocksdir), "%s/blocks", netdir);
    sd_mkdir_p(blocksdir);

    char log_path[512], proj_path[512];
    snprintf(log_path, sizeof(log_path), "%s/events.log", dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo.db", dir);

    progress_store_close();
    bool store_ok = progress_store_open(dir);
    SD_CHECK("progress_store opens", store_ok);

    event_log_t *lg = store_ok ? event_log_open(log_path) : NULL;
    SD_CHECK("event log opens", lg != NULL);
    utxo_projection_t *proj = lg ? utxo_projection_open(proj_path, lg) : NULL;
    SD_CHECK("UTXO projection opens", proj != NULL);

    if (!store_ok || !lg || !proj) {
        if (proj) utxo_projection_close(proj);
        if (lg) event_log_close(lg);
        progress_store_close();
        SetDataDir(""); ClearDataDirCache();
        chain_params_select(CHAIN_MAIN);
        printf("=== reducer step-drain harness: %d failures (setup) ===\n", failures);
        return failures;
    }

    utxo_projection_set_event_log(lg);
    utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);

    struct main_state ms;
    main_state_init(&ms);

    struct uint256 genesis_hash = cp->consensus.hashGenesisBlock;
    struct block_index *genesis = chainstate_insert_block_index(
        (struct chainstate *)&ms, &genesis_hash);
    SD_CHECK("genesis block_index inserted", genesis != NULL);
    if (genesis) {
        genesis->nHeight = 0;
        genesis->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        genesis->nTx = 1;
        genesis->nChainTx = 1;
        genesis->nChainWork = GetBlockProof(genesis);
        active_chain_move_window_tip(&ms.chain_active, genesis);
        ms.pindex_best_header = genesis;
    }

    bool stages_ok =
        header_admit_stage_init(&ms) &&
        validate_headers_stage_init(&ms) &&
        body_fetch_stage_init(&ms) &&
        body_persist_stage_init(&ms) &&
        script_validate_stage_init(&ms) &&
        proof_validate_stage_init(&ms) &&
        utxo_apply_stage_init(&ms) &&
        tip_finalize_stage_init(&ms);
    SD_CHECK("all eight reducer stages init", stages_ok);
    SD_CHECK("seed genesis utxo_apply row",
             sd_seed_genesis_utxo_apply_row(progress_store_db()));
    SD_CHECK("tip_finalize anchor seed at genesis",
             tip_finalize_stage_seed_anchor(0, genesis_hash.data, false));

    sqlite3 *db = progress_store_db();

    /* ── Mine ONE successor-less block (the `generate 1` scenario) ──────── */
    struct block blk1;
    int64_t cb_val = 0;
    bool built = stages_ok &&
                 sd_build_regtest_block(&blk1, 1, &genesis_hash, cp, &cb_val) &&
                 mine_block_pow(&blk1, 1, cp, 0);
    SD_CHECK("block 1 built + Equihash-mined", built);

    if (built) {
        struct uint256 h1;
        block_get_hash(&blk1, &h1);

        /* (1) Push the header EXACTLY like reducer_ingest_block: height=-1. */
        struct header_admit_msg m;
        memset(&m, 0, sizeof(m));
        m.hash = h1;
        m.has_header = true;
        m.header = blk1.header;
        m.height = -1;
        SD_CHECK("header pushed to admit inbox", mailbox_header_admit_push(&m));

        /* (2) Body-independent prefix: header_admit then validate_headers. */
        (void)header_admit_stage_drain(100);
        struct block_index *bi1 = block_map_find(&ms.map_block_index, &h1);
        SD_CHECK("STEP header_admit: block_index for block 1 created", bi1 != NULL);
        SD_CHECK("STEP header_admit: best_header advanced to block 1 (L2.5)",
                 ms.pindex_best_header == bi1);

        (void)validate_headers_stage_drain(100);
        int64_t vh_ok = -1;
        bool vh_row = sd_log_int(db, "validate_headers_log", "ok", 1, &vh_ok);
        SD_CHECK("STEP validate_headers: row written ok=1 (header passes PoW)",
                 vh_row && vh_ok == 1);

        /* (3) Persist the body to disk + mark HAVE_DATA (reducer_persist step). */
        bool persisted = false;
        if (bi1) {
            struct disk_block_pos pos;
            disk_block_pos_init(&pos);
            persisted = write_block_to_disk(&blk1, &pos, netdir, cp->pchMessageStart) &&
                        block_index_set_have_data_verified(bi1, &pos, netdir);
        }
        SD_CHECK("STEP persist: body on disk + BLOCK_HAVE_DATA set",
                 persisted && bi1 && (bi1->nStatus & BLOCK_HAVE_DATA));

        /* (4) SINGLE-STEP the body-dependent stages, asserting each row. The
         * first stage to write ok=0 here is the bug. */
        (void)body_fetch_stage_step_once();
        int64_t bf_ok = -1; char bf_src[64] = {0};
        bool bf_row = sd_log_int(db, "body_fetch_log", "ok", 1, &bf_ok);
        (void)sd_log_text(db, "body_fetch_log", "source", 1, bf_src, sizeof(bf_src));
        SD_CHECK("STEP body_fetch: ok=1 source=disk (NOT skipped_invalid)",
                 bf_row && bf_ok == 1 && strcmp(bf_src, "disk") == 0);
        if (!(bf_row && bf_ok == 1))
            printf("  >> body_fetch row: found=%d ok=%lld source=%s\n",
                   (int)bf_row, (long long)bf_ok, bf_src);

        (void)body_persist_stage_step_once();
        int64_t bp_ok = -1; char bp_src[64] = {0};
        bool bp_row = sd_log_int(db, "body_persist_log", "ok", 1, &bp_ok);
        (void)sd_log_text(db, "body_persist_log", "source", 1, bp_src, sizeof(bp_src));
        SD_CHECK("STEP body_persist: ok=1 source=verified (NOT upstream_failed)",
                 bp_row && bp_ok == 1 && strcmp(bp_src, "verified") == 0);
        if (!(bp_row && bp_ok == 1))
            printf("  >> body_persist row: found=%d ok=%lld source=%s\n",
                   (int)bp_row, (long long)bp_ok, bp_src);

        (void)script_validate_stage_step_once();
        int64_t sv_ok = -1;
        bool sv_row = sd_log_int(db, "script_validate_log", "ok", 1, &sv_ok);
        SD_CHECK("STEP script_validate: ok=1 (coinbase-only, trivially valid)",
                 sv_row && sv_ok == 1);

        (void)proof_validate_stage_step_once();
        int64_t pv_ok = -1;
        bool pv_row = sd_log_int(db, "proof_validate_log", "ok", 1, &pv_ok);
        SD_CHECK("STEP proof_validate: ok=1 (no shielded proofs)",
                 pv_row && pv_ok == 1);

        (void)utxo_apply_stage_step_once();
        int64_t ua_ok = -1;
        bool ua_row = sd_log_int(db, "utxo_apply_log", "ok", 1, &ua_ok);
        SD_CHECK("STEP utxo_apply: ok=1 + succeeded_at(1) (coinbase added)",
                 ua_row && ua_ok == 1 && utxo_apply_stage_succeeded_at(1));

        /* Finalize block 1 the way reducer_ingest_block's L2 gate does, so the
         * single-stepped path lands a real tip at height 1. */
        (void)tip_finalize_stage_step_once();
        tip_finalize_stage_set_authoritative_tip(1, h1.data);
        SD_CHECK("single-stepped: tip advanced to height 1",
                 active_chain_height(&ms.chain_active) == 1);

        block_free(&blk1);
    }

    /* ── INTEGRATION: drive the REAL reducer_ingest_block on a SECOND
     * successor-less block (block 2 on top of the now-tip block 1). This is the
     * exact `generate` path — push(height=-1) + the two-drain sequence + the L2
     * finalize, all inside reducer_ingest_block — so if it diverges from the
     * single-stepped drive above, the bug is in the integration, not the
     * stages. ──────────────────────────────────────────────────────────── */
    if (stages_ok && active_chain_height(&ms.chain_active) == 1) {
        struct chain_activation_controller ctl;
        activation_controller_init(&ctl, &ms, NULL, cp, netdir);

        struct block_index *tip1 = active_chain_tip(&ms.chain_active);
        struct uint256 prev = tip1 && tip1->phashBlock ? *tip1->phashBlock
                                                        : genesis_hash;
        struct block blk2;
        int64_t cb2 = 0;
        bool built2 = sd_build_regtest_block(&blk2, 2, &prev, cp, &cb2) &&
                      mine_block_pow(&blk2, 2, cp, 0);
        SD_CHECK("block 2 built + mined (integration)", built2);

        if (built2) {
            int h_before = active_chain_height(&ms.chain_active);
            struct validation_state out;
            validation_state_init(&out);
            bool acc = reducer_ingest_block(&ctl, &blk2, REDUCER_SRC_MINED,
                                            true, &out);
            int h_after = active_chain_height(&ms.chain_active);
            SD_CHECK("INTEGRATION reducer_ingest_block(block 2) accepted", acc);
            SD_CHECK("INTEGRATION tip advanced 1 -> 2 via reducer_ingest_block",
                     h_after == 2);
            if (!acc || h_after != 2)
                printf("  >> integration: acc=%d h %d->%d reject=%s\n",
                       (int)acc, h_before, h_after,
                       out.reject_reason[0] ? out.reject_reason : "(none)");
            block_free(&blk2);
        }
        activation_controller_destroy(&ctl);
    }

    /* ── teardown ──────────────────────────────────────────────────────── */
    tip_finalize_stage_shutdown();
    utxo_apply_stage_shutdown();
    proof_validate_stage_shutdown();
    script_validate_stage_shutdown();
    body_persist_stage_shutdown();
    body_fetch_stage_shutdown();
    validate_headers_stage_shutdown();
    header_admit_stage_shutdown();
    utxo_projection_close(proj);
    event_log_close(lg);
    progress_store_close();
    test_cleanup_tmpdir(blocksdir);
    test_cleanup_tmpdir(netdir);
    test_cleanup_tmpdir(dir);
    SetDataDir(""); ClearDataDirCache();
    chain_params_select(CHAIN_MAIN);

    printf("=== reducer step-drain harness: %d failures ===\n", failures);
    return failures;
}

/* ── Regression guard: regtest on-demand GENESIS SELF-SEED ────────────────────
 * The sibling harness above MANUALLY seeds the genesis anchor (the line the
 * import/snapshot/reindex paths run), so it cannot catch the regression where a
 * FRESH genesis-only regtest node fails to self-seed. Before the
 * fMineBlocksOnDemand-gated genesis-seed in reducer_ingest_block
 * (app/services/src/reducer_ingest_service.c), the first `generate` on such a
 * node left utxo_apply unseeded (utx=-1) so the block never finalized
 * ("block-not-finalized-by-reducer", tip stuck at 0). This drives the REAL
 * reducer_ingest_block front door on an UNSEEDED genesis node (NO manual seed)
 * and asserts the ingest SELF-SEEDS genesis + advances the tip 0->1. If the fix
 * regresses, reducer_ingest_block(block 1) leaves the tip at 0 and this FAILS
 * loudly — the durable guard the 2026-06-06 fix lacked (it silently regressed).
 * Hermetic in-process mirror of the 2026-06-17 live copy-prove (generate 5 -> 5,
 * rejects=0). */
int test_reducer_ondemand_genesis_seed(void);
int test_reducer_ondemand_genesis_seed(void)
{
    int failures = 0;

    blocker_module_init();
    chain_params_select(CHAIN_REGTEST);
    const struct chain_params *cp = chain_params_get();
    SD_CHECK("regtest mines on demand (fMineBlocksOnDemand)",
             cp->fMineBlocksOnDemand);

    char dir[256];
    sd_mkdir_p("./test-tmp");
    test_fmt_tmpdir(dir, sizeof(dir), "reducer_ondemand_genesis_seed", "main");
    sd_mkdir_p(dir);
    SetDataDir(dir);
    ClearDataDirCache();
    char netdir[512];
    GetDataDir(true, netdir, sizeof(netdir));
    sd_mkdir_p(netdir);
    char blocksdir[640];
    snprintf(blocksdir, sizeof(blocksdir), "%s/blocks", netdir);
    sd_mkdir_p(blocksdir);

    char log_path[512], proj_path[512];
    snprintf(log_path, sizeof(log_path), "%s/events.log", dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo.db", dir);

    progress_store_close();
    bool store_ok = progress_store_open(dir);
    SD_CHECK("progress_store opens", store_ok);
    event_log_t *lg = store_ok ? event_log_open(log_path) : NULL;
    SD_CHECK("event log opens", lg != NULL);
    utxo_projection_t *proj = lg ? utxo_projection_open(proj_path, lg) : NULL;
    SD_CHECK("UTXO projection opens", proj != NULL);

    if (!store_ok || !lg || !proj) {
        if (proj) utxo_projection_close(proj);
        if (lg) event_log_close(lg);
        progress_store_close();
        SetDataDir(""); ClearDataDirCache();
        chain_params_select(CHAIN_MAIN);
        printf("=== reducer on-demand genesis-seed: %d failures (setup) ===\n",
               failures);
        return failures;
    }

    utxo_projection_set_event_log(lg);
    utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);

    struct main_state ms;
    main_state_init(&ms);

    struct uint256 genesis_hash = cp->consensus.hashGenesisBlock;
    struct block_index *genesis = chainstate_insert_block_index(
        (struct chainstate *)&ms, &genesis_hash);
    SD_CHECK("genesis block_index inserted", genesis != NULL);
    if (genesis) {
        genesis->nHeight = 0;
        genesis->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        genesis->nTx = 1;
        genesis->nChainTx = 1;
        genesis->nChainWork = GetBlockProof(genesis);
        active_chain_move_window_tip(&ms.chain_active, genesis);
        ms.pindex_best_header = genesis;
    }

    bool stages_ok =
        header_admit_stage_init(&ms) &&
        validate_headers_stage_init(&ms) &&
        body_fetch_stage_init(&ms) &&
        body_persist_stage_init(&ms) &&
        script_validate_stage_init(&ms) &&
        proof_validate_stage_init(&ms) &&
        utxo_apply_stage_init(&ms) &&
        tip_finalize_stage_init(&ms);
    SD_CHECK("all eight reducer stages init", stages_ok);

    /* THE PRECONDITION that makes this a real guard: NOTHING seeded the genesis
     * anchor (no sd_seed_genesis_utxo_apply_row, no tip_finalize_stage_seed_anchor
     * — unlike the sibling harness). The cursor MUST be unseeded at genesis;
     * the fix is what seeds it from inside reducer_ingest_block. */
    SD_CHECK("precondition: tip is genesis (height 0)",
             active_chain_height(&ms.chain_active) == 0);
    SD_CHECK("precondition: tip_finalize cursor UNSEEDED (0)",
             tip_finalize_stage_cursor() == 0);

    if (stages_ok) {
        struct block blk1;
        int64_t cb_val = 0;
        bool built = sd_build_regtest_block(&blk1, 1, &genesis_hash, cp, &cb_val) &&
                     mine_block_pow(&blk1, 1, cp, 0);
        SD_CHECK("block 1 built + Equihash-mined", built);

        if (built) {
            struct chain_activation_controller ctl;
            activation_controller_init(&ctl, &ms, NULL, cp, netdir);
            struct validation_state out;
            validation_state_init(&out);

            int h_before = active_chain_height(&ms.chain_active);
            bool acc = reducer_ingest_block(&ctl, &blk1, REDUCER_SRC_MINED,
                                            true, &out);
            int h_after = active_chain_height(&ms.chain_active);

            SD_CHECK("reducer_ingest_block(block 1) accepted on UNSEEDED genesis",
                     acc);
            SD_CHECK("tip SELF-SEEDED + advanced 0 -> 1 (the fix; was utx=-1)",
                     h_after == 1);
            SD_CHECK("utxo_apply succeeded at height 1 (no utx=-1 reject)",
                     utxo_apply_stage_succeeded_at(1));
            SD_CHECK("tip_finalize cursor advanced past the genesis seed",
                     tip_finalize_stage_cursor() >= 1);
            if (!acc || h_after != 1)
                printf("  >> ondemand-seed: acc=%d h %d->%d reject=%s\n",
                       (int)acc, h_before, h_after,
                       out.reject_reason[0] ? out.reject_reason : "(none)");

            activation_controller_destroy(&ctl);
            block_free(&blk1);
        }
    }

    tip_finalize_stage_shutdown();
    utxo_apply_stage_shutdown();
    proof_validate_stage_shutdown();
    script_validate_stage_shutdown();
    body_persist_stage_shutdown();
    body_fetch_stage_shutdown();
    validate_headers_stage_shutdown();
    header_admit_stage_shutdown();
    utxo_projection_close(proj);
    event_log_close(lg);
    progress_store_close();
    test_cleanup_tmpdir(blocksdir);
    test_cleanup_tmpdir(netdir);
    test_cleanup_tmpdir(dir);
    SetDataDir(""); ClearDataDirCache();
    chain_params_select(CHAIN_MAIN);

    printf("=== reducer on-demand genesis-seed: %d failures ===\n", failures);
    return failures;
}
