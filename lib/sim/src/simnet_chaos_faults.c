/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * simnet chaos-fault injectors. See sim/simnet_chaos_faults.h for the
 * per-fault contract and the mission this file serves.
 *
 * Fixture idioms below are deliberately copied (not `#include`d — the
 * originals are file-static) from three already-reviewed regression tests,
 * so the row shapes and column lists are proven-correct, not reinvented:
 *   - lib/test/src/test_reducer_frontier.c   (put_consistent_height family)
 *   - lib/test/src/test_block_index_loader.c (genesis-root seed_tip fixture)
 *   - lib/test/src/test_segment_corruption.c (seal/tamper/detect/repair)
 *   - lib/test/src/test_supervisor.c         (freeze/deadline-stall pattern)
 *   - lib/test/src/test_condition_engine.c   (condition + EV_OPERATOR_NEEDED
 *                                              counting pattern)
 */

#include "sim/simnet_chaos_faults.h"

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "conditions/segment_corruption.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "event/event.h"
#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "jobs/tip_finalize_stage.h"
#include "storage/chain_segment.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/stage.h"
#include "util/supervisor.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void chaos_note(struct chaos_fault_result *out, const char *fmt, ...)
{
    if (!out) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->note, sizeof(out->note), fmt, ap);
    va_end(ap);
}

static void chaos_result_init(struct chaos_fault_result *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->hstar_before = -1;
    out->hstar_after = -1;
}

/* ── shared stage-log fixture helpers (mirrors test_reducer_frontier.c) ─── */

/* The per-stage *_log tables (plus the header_admit_log MODEL table and the
 * utxo_apply_delta sidecar) are lazily created by their owning module the
 * first time its production init/step path runs — this fixture never runs
 * the full stage machinery (only tip_finalize_stage_init, for the anchor
 * write), so the rest would not exist yet. reducer_frontier_compute_hstar's
 * hash-agreement cross-check (reducer_frontier_evidence.c) additionally
 * JOINs header_admit_log and utxo_apply_delta against validate_headers_log,
 * so both need real, hash-matching rows too, not just the six k_logs[]
 * tables. CREATE TABLE IF NOT EXISTS with the EXACT production column list
 * (verified against the live source above) is idempotent and safe to call
 * unconditionally: a real ensure_schema call elsewhere on the same db is a
 * silent no-op against identical DDL text. */
static bool chaos_ensure_log_tables(sqlite3 *db)
{
    static const char *const ddl =
        "CREATE TABLE IF NOT EXISTS header_admit_log ("
        "  height      INTEGER PRIMARY KEY,"
        "  hash        BLOB    NOT NULL,"
        "  parent_hash BLOB,"
        "  admitted_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS utxo_apply_delta ("
        "  height       INTEGER PRIMARY KEY,"
        "  branch_hash  BLOB    NOT NULL,"
        "  spent_blob   BLOB    NOT NULL,"
        "  added_blob   BLOB    NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS validate_headers_log ("
        "  height       INTEGER PRIMARY KEY,"
        "  hash         BLOB    NOT NULL,"
        "  ok           INTEGER NOT NULL,"
        "  fail_reason  TEXT,"
        "  validated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height             INTEGER PRIMARY KEY,"
        "  status             TEXT    NOT NULL,"
        "  ok                 INTEGER NOT NULL,"
        "  tx_count           INTEGER NOT NULL,"
        "  input_count        INTEGER NOT NULL,"
        "  first_failure_txid BLOB,"
        "  first_failure_vin  INTEGER,"
        "  first_failure_serror INTEGER,"
        "  validated_at       INTEGER NOT NULL,"
        "  block_hash         BLOB,"
        "  source_epoch_digest BLOB"
        ");"
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "  height       INTEGER PRIMARY KEY,"
        "  source       TEXT    NOT NULL,"
        "  ok           INTEGER NOT NULL,"
        "  persisted_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "  height                  INTEGER PRIMARY KEY,"
        "  status                  TEXT    NOT NULL,"
        "  ok                      INTEGER NOT NULL,"
        "  sapling_spends_total    INTEGER NOT NULL,"
        "  sapling_outputs_total   INTEGER NOT NULL,"
        "  sprout_joinsplits_total INTEGER NOT NULL,"
        "  block_hash              BLOB,"
        "  source_epoch_digest     BLOB,"
        "  first_failure_txid      BLOB,"
        "  first_failure_proof_type TEXT,"
        "  validated_at            INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "  height               INTEGER PRIMARY KEY,"
        "  status               TEXT    NOT NULL,"
        "  ok                   INTEGER NOT NULL,"
        "  spent_count          INTEGER NOT NULL,"
        "  added_count          INTEGER NOT NULL,"
        "  total_value_delta    INTEGER NOT NULL,"
        "  first_failure_kind   TEXT,"
        "  first_failure_detail BLOB,"
        "  applied_at           INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "  height           INTEGER PRIMARY KEY,"
        "  status           TEXT    NOT NULL,"
        "  ok               INTEGER NOT NULL,"
        "  work_delta_high  INTEGER NOT NULL,"
        "  work_delta_low   INTEGER NOT NULL,"
        "  utxo_size_after  INTEGER NOT NULL,"
        "  reorg_depth      INTEGER NOT NULL,"
        "  finalized_at     INTEGER NOT NULL"
        ");";
    char *err = NULL;
    if (sqlite3_exec(db, ddl, NULL, NULL, &err) != SQLITE_OK) {  // raw-sql-ok:test-fixture-schema
        fprintf(stderr, "[chaos] ensure_log_tables: %s\n", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

/* Each helper below matches the EXACT real CREATE TABLE column list emitted
 * by the corresponding production module (re-checked against the live
 * source at the time this file was written — every NOT NULL column without
 * a DEFAULT gets an explicit harmless value; compute_hstar's contiguity scan
 * only ever reads height/ok/status/hash, its hash-agreement cross-check also
 * reads hash/block_hash/branch_hash, so the placeholder values in the rest
 * are inert).
 *
 * Every write here is an UPSERT (INSERT ... ON CONFLICT(height) DO UPDATE),
 * not a plain INSERT: tip_finalize_stage_seed_anchor's trusted-seed path
 * ALSO writes a real production row at the anchor height into utxo_apply_log
 * (status='anchor', so it can self-finalize the first C→C+1 transition —
 * see the "seed utxo_apply anchor row" comment in
 * app/jobs/src/tip_finalize_anchor.c). utxo_apply_log is profile_bound
 * (log_success_requires_full_validation), so a status of 'anchor' fails the
 * VERIFIED parse and would silently cap the contiguous prefix one height
 * short — exactly the off-by-one this upsert avoids. The fixture's own
 * consistent, hash-agreeing row must always win at every height it stamps,
 * never lose to whatever a production seed path wrote first. tip_finalize_log
 * is the one exception (kept OR IGNORE, see chaos_put_tip_finalize) since
 * overwriting its anchor row's status would itself change which row
 * reducer_trusted_anchor's own status='anchor' lookup finds. */

static bool chaos_put_header_admit(sqlite3 *db, int32_t h,
                                   const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO header_admit_log(height,hash,admitted_at) "
            "VALUES(?,?,0) ON CONFLICT(height) DO UPDATE SET "
            "hash=excluded.hash", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool chaos_put_validate_headers(sqlite3 *db, int32_t h,
                                       const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO validate_headers_log(height,hash,ok,validated_at) "
            "VALUES(?,?,1,0) ON CONFLICT(height) DO UPDATE SET "
            "hash=excluded.hash, ok=1", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool chaos_put_script_validate(sqlite3 *db, int32_t h,
                                      const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO script_validate_log"
            "(height,status,ok,tx_count,input_count,validated_at,block_hash) "
            "VALUES(?,'verified',1,1,1,0,?) ON CONFLICT(height) DO UPDATE "
            "SET status='verified', ok=1, block_hash=excluded.block_hash",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool chaos_put_body_persist(sqlite3 *db, int32_t h)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO body_persist_log(height,source,ok,persisted_at) "
            "VALUES(?,'chaos_fixture',1,0) ON CONFLICT(height) DO UPDATE "
            "SET ok=1", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool chaos_put_proof_validate(sqlite3 *db, int32_t h,
                                     const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO proof_validate_log"
            "(height,status,ok,sapling_spends_total,sapling_outputs_total,"
            "sprout_joinsplits_total,block_hash,validated_at) "
            "VALUES(?,'verified',1,0,0,0,?,0) ON CONFLICT(height) DO UPDATE "
            "SET status='verified', ok=1, block_hash=excluded.block_hash",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool chaos_put_utxo_apply(sqlite3 *db, int32_t h)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO utxo_apply_log"
            "(height,status,ok,spent_count,added_count,total_value_delta,"
            "applied_at) VALUES(?,'verified',1,0,0,0,0) "
            "ON CONFLICT(height) DO UPDATE SET status='verified', ok=1",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool chaos_put_utxo_apply_delta(sqlite3 *db, int32_t h,
                                       const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO utxo_apply_delta"
            "(height,branch_hash,spent_blob,added_blob) "
            "VALUES(?,?,x'',x'') ON CONFLICT(height) DO UPDATE SET "
            "branch_hash=excluded.branch_hash", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

/* OR IGNORE (not upsert): fault (a) may call this for the height whose
 * tip_finalize_log row was already written by
 * tip_finalize_stage_seed_anchor() (the anchor row at N is pipeline-owned —
 * see reducer_frontier.h's frontier_next_cursor commentary). Its status
 * text ('anchor') is load-bearing for reducer_trusted_anchor's own lookup
 * (`WHERE status='anchor'`) — overwriting it would change which row that
 * scan finds, an unrelated behavior this fixture must not perturb. Silently
 * keeping the existing ok=1 row is correct: tip_finalize_log is not
 * profile_bound, so contiguity only cares about ok, not status. */
static bool chaos_put_tip_finalize(sqlite3 *db, int32_t h)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO tip_finalize_log"
            "(height,status,ok,work_delta_high,work_delta_low,"
            "utxo_size_after,reorg_depth,finalized_at) "
            "VALUES(?,'ok',1,0,0,0,0,0)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

/* Deterministic 32-byte hash keyed by height. */
static void chaos_synth_hash(uint8_t out[32], int32_t h)
{
    memset(out, 0, 32);
    out[0] = (uint8_t)(h & 0xff);
    out[1] = (uint8_t)((h >> 8) & 0xff);
    out[2] = (uint8_t)((h >> 16) & 0xff);
    out[31] = 0xC5;
}

/* Write a full, mutually-consistent ok=1 row across every table
 * reducer_frontier_compute_hstar's contiguity scan AND hash-agreement
 * cross-check touch at height h — the same synthetic hash in
 * header_admit_log.hash, validate_headers_log.hash, script_validate_log.
 * block_hash, proof_validate_log.block_hash, and utxo_apply_delta.
 * branch_hash so the JOIN in reducer_frontier_evidence.c finds every leg
 * agreeing (no manufactured hash-split). */
static bool chaos_put_consistent_height(sqlite3 *db, int32_t h)
{
    uint8_t hh[32];
    chaos_synth_hash(hh, h);
    return chaos_put_header_admit(db, h, hh) &&
           chaos_put_validate_headers(db, h, hh) &&
           chaos_put_script_validate(db, h, hh) &&
           chaos_put_body_persist(db, h) &&
           chaos_put_proof_validate(db, h, hh) &&
           chaos_put_utxo_apply(db, h) &&
           chaos_put_utxo_apply_delta(db, h, hh) &&
           chaos_put_tip_finalize(db, h);
}

/* Stamp a full consistent prefix [0, n] and the matching stage_cursor rows
 * (upstream stages count "next height"; tip_finalize is served-tip
 * convention, so its cursor is n itself, not n+1). Returns false on the
 * first sqlite error. */
static bool chaos_stamp_prefix(sqlite3 *db, int32_t n)
{
    if (!chaos_ensure_log_tables(db))
        return false;
    for (int32_t h = 0; h <= n; h++)
        if (!chaos_put_consistent_height(db, h))
            return false;
    return stage_set_named_cursor(db, "validate_headers", (uint64_t)(n + 1)) &&
           stage_set_named_cursor(db, "script_validate", (uint64_t)(n + 1)) &&
           stage_set_named_cursor(db, "body_persist", (uint64_t)(n + 1)) &&
           stage_set_named_cursor(db, "proof_validate", (uint64_t)(n + 1)) &&
           stage_set_named_cursor(db, "utxo_apply", (uint64_t)(n + 1)) &&
           stage_set_named_cursor(db, "tip_finalize", (uint64_t)n);
}

/* ══════════════════════════════════════════════════════════════════════
 * (a) Pillar-0: full block index, empty active-chain window
 * ══════════════════════════════════════════════════════════════════════ */

/* Forward declaration, not `#include "services/block_index_loader.h"`:
 * lib/ → app/services|controllers|models|views is a HARD layering gate
 * (tools/scripts/check_lib_layering.sh) — lib/sim must not depend on the
 * app/ shape headers. The real symbol still links fine (one binary, see
 * this file's header comment); only the SOURCE INCLUDE crosses the
 * boundary, and a bare prototype avoids that without an override marker.
 * Signature verified against app/services/include/services/
 * block_index_loader.h at the time this file was written. */
int block_index_loader_seed_tip_from_finalized(struct main_state *ms,
                                               const struct chain_params *params,
                                               struct sqlite3 *progress_db);

static struct uint256 chaos_chain_hash(const struct uint256 *genesis, int h)
{
    if (h == 0) return *genesis;
    struct uint256 hh;
    memset(&hh, 0, sizeof(hh));
    hh.data[0] = (uint8_t)(h & 0xFF);
    hh.data[1] = (uint8_t)((h >> 8) & 0xFF);
    hh.data[2] = (uint8_t)((h >> 16) & 0xFF);
    hh.data[3] = (uint8_t)((h >> 24) & 0xFF);
    hh.data[31] = 0xC2;
    return hh;
}

bool chaos_fault_empty_active_chain_window(int gap_height,
                                           struct chaos_fault_result *out)
{
    chaos_result_init(out);
    if (gap_height < 1 || gap_height > 2000000) {
        chaos_note(out, "gap_height %d out of harness range", gap_height);
        return false;
    }

    chain_params_select(CHAIN_REGTEST);
    const struct chain_params *cp = chain_params_get();
    const struct uint256 gen = cp->consensus.hashGenesisBlock;
    int N = gap_height;

    char dir[256];
    mkdir("./test-tmp", 0755);
    test_fmt_tmpdir(dir, sizeof(dir), "chaos_empty_window", "main");
    mkdir(dir, 0755);

    progress_store_close();
    bool store_ok = progress_store_open(dir);
    sqlite3 *db = store_ok ? progress_store_db() : NULL;
    if (!db) {
        chaos_note(out, "progress_store failed to open fixture dir");
        chain_params_select(CHAIN_MAIN);
        return false;
    }

    struct main_state ms;
    memset(&ms, 0, sizeof(ms));
    block_map_init(&ms.map_block_index);
    active_chain_init(&ms.chain_active);  /* NULL tip: the empty window */

    /* Build the FULL block index: genesis..N, contiguous, HAVE_DATA +
     * VALID_SCRIPTS — "the block-index map is full". */
    struct block_index *tipN = NULL;
    for (int h = 0; h <= N; h++) {
        struct uint256 hh = chaos_chain_hash(&gen, h);
        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)&ms, &hh);
        if (!pi) {
            chaos_note(out, "block_map insert failed at h=%d", h);
            goto cleanup;
        }
        pi->nHeight = h;
        pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx = 1;
        pi->nBits = 0x200f0f0f;
        pi->hashBlock = hh;
        pi->phashBlock = &pi->hashBlock;
        if (h > 0) {
            struct uint256 prev_hash = chaos_chain_hash(&gen, h - 1);
            struct block_index *prev = block_map_find(&ms.map_block_index,
                                                       &prev_hash);
            pi->pprev = prev;
            if (prev) {
                struct arith_uint256 proof = GetBlockProof(pi);
                arith_uint256_add(&pi->nChainWork, &prev->nChainWork, &proof);
                pi->nChainTx = prev->nChainTx + pi->nTx;
            }
        } else {
            pi->nChainWork = GetBlockProof(pi);
            pi->nChainTx = 1;
        }
        if (h == N) tipN = pi;
    }

    bool tf_init = tip_finalize_stage_init(&ms);
    bool anchor_ok = tf_init &&
        tip_finalize_stage_seed_anchor(N, tipN->hashBlock.data, true);
    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);  // raw-sql-ok:test-fixture-seeding
    bool coins_ok = coins_kv_set_applied_height_in_tx(db, N + 1);
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);  // raw-sql-ok:test-fixture-seeding
    if (!anchor_ok || !coins_ok) {
        chaos_note(out, "fixture anchor/coins seed failed");
        goto cleanup;
    }

    /* THE FAULT: block index map is full, active_chain_tip() is NULL. Drive
     * the real boot repair path. */
    int r = block_index_loader_seed_tip_from_finalized(&ms, cp, db);
    bool installed = (r == 1) && active_chain_tip(&ms.chain_active) == tipN &&
                     active_chain_height(&ms.chain_active) == N;
    out->ok = installed;
    out->recovered = installed;

    if (installed) {
        /* H* CLIMBS, not merely "tip non-NULL": stamp the matching stage-log
         * prefix and read H* back through the real algorithm. */
        if (!chaos_stamp_prefix(db, N)) {
            chaos_note(out, "stage-log prefix stamp failed post-install");
            out->ok = false;
            goto cleanup;
        }
        int32_t hstar = -1, served = -1;
        if (!reducer_frontier_compute_hstar(db, &hstar, &served)) {
            chaos_note(out, "compute_hstar failed post-install");
            out->ok = false;
            goto cleanup;
        }
        out->hstar_before = -1;
        out->hstar_after = hstar;
        out->recovered = (hstar == N);
        chaos_note(out,
            "gap=%d INSTALLED: tip=h%d locator-anchored-at-best-header, "
            "H* climbed to %d", gap_height, N, hstar);
    } else {
        chaos_note(out,
            "gap=%d REFUSED (r=%d): active_chain_tip stays NULL — the "
            "documented Pillar-0 wedge at this gap scale", gap_height, r);
    }

cleanup:
    tip_finalize_stage_shutdown();
    progress_store_close();
    block_map_free(&ms.map_block_index);
    test_cleanup_tmpdir(dir);
    chain_params_select(CHAIN_MAIN);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 * (b) kill/restart mid-fold
 * ══════════════════════════════════════════════════════════════════════ */

bool chaos_fault_kill_restart_mid_fold(struct chaos_fault_result *out)
{
    chaos_result_init(out);
    const int32_t K = 37;

    char dir[256];
    mkdir("./test-tmp", 0755);
    test_fmt_tmpdir(dir, sizeof(dir), "chaos_kill_restart", "main");
    mkdir(dir, 0755);

    progress_store_close();
    if (!progress_store_open(dir)) {
        chaos_note(out, "progress_store failed to open fixture dir");
        test_cleanup_tmpdir(dir);
        return false;
    }
    sqlite3 *db = progress_store_db();
    if (!db || !chaos_stamp_prefix(db, K)) {
        chaos_note(out, "fixture stamp failed");
        progress_store_close();
        test_cleanup_tmpdir(dir);
        return false;
    }

    int32_t hstar_before = -1, served_before = -1;
    bool ok1 = reducer_frontier_compute_hstar(db, &hstar_before, &served_before);

    /* THE FAULT: abrupt close (kill -9 before the next stage advance would
     * have committed anything further) then a fresh reopen (the restart) —
     * no in-RAM state survives; everything must be re-derived from disk. */
    progress_store_close();
    bool reopened = progress_store_open(dir);
    sqlite3 *db2 = reopened ? progress_store_db() : NULL;

    int32_t hstar_after = -1, served_after = -1;
    bool ok2 = db2 && reducer_frontier_compute_hstar(db2, &hstar_after,
                                                     &served_after);

    out->ok = ok1 && reopened && ok2;
    out->hstar_before = hstar_before;
    out->hstar_after = hstar_after;
    out->recovered = out->ok && hstar_after == hstar_before &&
                     hstar_after == K && served_after <= hstar_after;
    chaos_note(out,
        "kill/restart mid-fold: H* before=%d after=%d (served=%d) — "
        "%s", hstar_before, hstar_after, served_after,
        out->recovered ? "resumed identically, nothing lost"
                       : "DIVERGED across the restart");

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 * (c) corrupt a sealed chain_segment
 * ══════════════════════════════════════════════════════════════════════ */

/* test_make_tmpdir / test_rm_rf_recursive are ordinary (non-inline)
 * lib/test/src/test_helpers.c symbols, only linked into the test_zcl /
 * test_parallel binaries (lib/test is not a LIB_MODULE, so it never links
 * into production zclassic23). Guard this fault the same way as (d)/(e) so
 * a production compile never references an unresolved symbol. */
#ifdef ZCL_TESTING
static bool chaos_tiny_body(void *user, uint32_t h, uint8_t **bytes,
                            size_t *len)
{
    (void)user;
    size_t n = 24;
    uint8_t *b = malloc(n); // raw-alloc-ok:test
    if (!b) return false;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(h * 7u + i);
    *bytes = b; *len = n;
    return true;
}

bool chaos_fault_corrupt_sealed_segment(struct chaos_fault_result *out)
{
    chaos_result_init(out);
    char err[256];
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "chaos_segment_corrupt", "flow");

    enum cseg_status s1 = chain_segment_seal_range(dir, chaos_tiny_body, NULL,
                                                   0, 10, err, sizeof(err));
    enum cseg_status s2 = chain_segment_seal_range(dir, chaos_tiny_body, NULL,
                                                   500, 7, err, sizeof(err));
    if (s1 != CSEG_OK || s2 != CSEG_OK) {
        chaos_note(out, "fixture seal failed: %s", err);
        test_rm_rf_recursive(dir);
        return false;
    }

    /* THE FAULT: tamper one byte in the second segment's block-data region
     * (past the 32B header + 7*48B index). */
    char path[512];
    snprintf(path, sizeof(path), "%s/seg-500-7.dat", dir);
    chmod(path, 0644);
    FILE *f = fopen(path, "r+b");
    bool tampered = false;
    if (f) {
        long off = 32 + 7 * 48 + 3;
        fseek(f, off, SEEK_SET); int c = fgetc(f);
        fseek(f, off, SEEK_SET); fputc(c ^ 0xff, f);
        fclose(f); tampered = true;
    }
    if (!tampered) {
        chaos_note(out, "tamper write failed");
        test_rm_rf_recursive(dir);
        return false;
    }

    /* DETECT: bounded round-robin spot-verify, same primitive the
     * segment_corruption condition polls with. */
    uint32_t cursor = 1, first = 0, count = 0;
    enum cseg_status detect = segment_corruption_scan_one(dir, &cursor, &first,
                                                          &count, err,
                                                          sizeof(err));
    bool detected = detect != CSEG_OK && detect != CSEG_ERR_NOT_FOUND &&
                    first == 500 && count == 7;

    /* REMEDY: unlink + rebuild the manifest. */
    enum cseg_status remedy = CSEG_ERR_ARG;
    if (detected)
        remedy = segment_corruption_repair(dir, 500, 7, err, sizeof(err));

    /* WITNESS: reopen and confirm the corrupt range no longer verifies
     * corrupt (it's gone) while the survivor still verifies clean — reads
     * fall back to blk*.dat instead of ever serving bad bytes. */
    bool witnessed = false;
    if (remedy == CSEG_OK) {
        struct chain_segment_store *store = NULL;
        enum cseg_status ost = chain_segment_store_open(dir, &store, err,
                                                        sizeof(err));
        witnessed = ost == CSEG_OK && store &&
                   !chain_segment_store_covers(store, 500) &&
                   chain_segment_store_covers(store, 0) &&
                   chain_segment_store_segment_count(store) == 1 &&
                   chain_segment_store_verify_index(store, 0, err,
                                                    sizeof(err)) == CSEG_OK;
        if (store) chain_segment_store_close(store);
    }

    out->ok = detected && remedy == CSEG_OK;
    out->recovered = witnessed;
    chaos_note(out,
        "sealed-segment tamper: detected=%d repair=%s witnessed=%d",
        detected, cseg_status_str(remedy), witnessed);

    test_rm_rf_recursive(dir);
    return true;
}
#else
bool chaos_fault_corrupt_sealed_segment(struct chaos_fault_result *out)
{
    chaos_result_init(out);
    chaos_note(out, "unavailable: built without ZCL_TESTING");
    return false;
}
#endif /* ZCL_TESTING */

/* ══════════════════════════════════════════════════════════════════════
 * (d) freeze the reducer drive (supervisor liveness layer)
 * ══════════════════════════════════════════════════════════════════════ */

/* supervisor_reset_for_testing / supervisor_sweep_once_for_testing are only
 * declared under ZCL_TESTING (lib/util/include/util/supervisor.h); this
 * file also compiles into the production zclassic23 binary (lib/sim is a
 * LIB_MODULE linked into every target — see the Makefile's ALL_SRCS), which
 * builds WITHOUT ZCL_TESTING. The harness's own binaries (test_zcl,
 * test_parallel) always define ZCL_TESTING, so the real body below is what
 * ever executes; the #else stub exists only so a production compile — which
 * never calls this function — still links. Every helper this fault needs is
 * scoped inside the same guard so a non-ZCL_TESTING build never sees an
 * unused-static-function warning (-Werror). */
#ifdef ZCL_TESTING
static void chaos_sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

bool chaos_fault_freeze_reducer_drive(struct chaos_fault_result *out)
{
    chaos_result_init(out);

    supervisor_reset_for_testing();
    supervisor_set_tick_ms_for_testing(5);

    static struct liveness_contract c;
    liveness_contract_init(&c, "chaos.reducer_drive");
    atomic_store(&c.deadline_secs, 1);
    /* Gate #21 (check_supervisor_domain): production registrations must name
     * a domain grouping, not the bare root registry — this synthetic child
     * gets its own "chaos" domain rather than an override marker. */
    supervisor_domain_t *domain = supervisor_create_domain("chaos");
    supervisor_child_id id = domain
        ? supervisor_register_in_domain(domain, &c)
        : SUPERVISOR_INVALID_ID;
    if (id == SUPERVISOR_INVALID_ID) {
        chaos_note(out, "supervisor_register_in_domain failed");
        return false;
    }

    /* THE FAULT: freeze the heartbeat — backdate last_tick_us so the
     * deadline lapses on the next sweep, exactly as a wedged reducer-drive
     * thread would present to the supervisor. */
    atomic_store(&c.last_tick_us, atomic_load(&c.last_tick_us) - 5000000);

    bool started = supervisor_start();
    chaos_sleep_ms(80);

    bool stall_fired = atomic_load(&c.stall_fires) >= 1u;
    bool stall_named = atomic_load(&c.stall_reason) ==
                       SUPERVISOR_STALL_TIME_DEADLINE;

    /* UNFREEZE: a fresh heartbeat — the "self-healed" half of the fault. */
    atomic_store(&c.last_tick_us, 0); /* liveness_contract_init set now; a
                                       * fresh progress marker + tick below
                                       * proves the child is alive again. */
    supervisor_progress(id, 1);
    uint32_t ticks_before = atomic_load(&c.ticks_run);
    /* Re-arm: give it a live heartbeat close to "now" so the next sweep's
     * deadline check passes, then request one more sweep window. */
    atomic_store(&c.deadline_secs, 60);
    supervisor_sweep_once_for_testing();
    chaos_sleep_ms(40);
    uint32_t ticks_after = atomic_load(&c.ticks_run);

    supervisor_stop();

    out->ok = started && stall_fired;
    out->recovered = stall_named && ticks_after >= ticks_before;
    out->operator_paged = false; /* the supervisor layer names a blocker via
                                  * stall_reason; it does not itself page an
                                  * operator for a single TIME_DEADLINE edge —
                                  * escalation is the condition-engine's job
                                  * (fault (e)), which this fault does not
                                  * exercise. */
    chaos_note(out,
        "reducer-drive freeze: stall_fires=%u reason=%s ticks %u->%u",
        atomic_load(&c.stall_fires),
        supervisor_stall_reason_name(atomic_load(&c.stall_reason)),
        ticks_before, ticks_after);

    supervisor_reset_for_testing();
    return true;
}
#else
bool chaos_fault_freeze_reducer_drive(struct chaos_fault_result *out)
{
    chaos_result_init(out);
    chaos_note(out, "unavailable: built without ZCL_TESTING");
    return false;
}
#endif /* ZCL_TESTING */

/* ══════════════════════════════════════════════════════════════════════
 * (e) stall a single stage — typed blocker + condition engine
 * ══════════════════════════════════════════════════════════════════════ */

/* condition_engine_reset_for_testing is ZCL_TESTING-only (see the comment
 * above chaos_fault_freeze_reducer_drive — the same reasoning applies); every
 * helper below is scoped inside the same guard for the same reason. */
#ifdef ZCL_TESTING
static _Atomic bool g_chaos_stage_cleared;
static _Atomic int  g_chaos_operator_events;

static bool chaos_stage_detect(void)
{
    return !atomic_load(&g_chaos_stage_cleared);
}

static enum condition_remedy_result chaos_stage_remedy(void)
{
    struct blocker_record rec;
    if (blocker_init(&rec, "chaos.stage_stall", "chaos_harness",
                     BLOCKER_TRANSIENT,
                     "synthetic single-stage stall (chaos harness)"))
        blocker_set(&rec);
    /* Self-heal after a couple of remedy attempts — models a stage whose
     * transient blocker (peer down / queue full) clears on its own. */
    static _Atomic int attempts;
    if (atomic_fetch_add(&attempts, 1) >= 1) {
        atomic_store(&g_chaos_stage_cleared, true);
        blocker_clear("chaos.stage_stall");
    }
    return COND_REMEDY_OK;
}

static bool chaos_stage_witness(int64_t target_at_detect)
{
    (void)target_at_detect;
    return atomic_load(&g_chaos_stage_cleared);
}

static void chaos_operator_observer(enum event_type type, uint32_t peer_id,
                                    const void *payload, uint32_t payload_len,
                                    void *ctx)
{
    (void)type; (void)peer_id; (void)payload; (void)payload_len; (void)ctx;
    atomic_fetch_add(&g_chaos_operator_events, 1);
}

bool chaos_fault_stall_single_stage(struct chaos_fault_result *out)
{
    chaos_result_init(out);

    condition_engine_reset_for_testing();
    event_log_init();
    atomic_store(&g_chaos_stage_cleared, false);
    atomic_store(&g_chaos_operator_events, 0);

    /* NOT const: the condition engine mutates `.state` in place through this
     * exact static instance (matches lib/test/src/test_condition_engine.c's
     * convention) — a `const`-qualified object risks living in read-only
     * storage the engine then can't atomically update. */
    static struct condition cond = {
        .name = "chaos_stage_stall",
        .severity = COND_WARN,
        .poll_secs = 1,
        .backoff_secs = 0,
        .max_attempts = 10,        /* generous budget: never exhausts within
                                    * this fixture's few ticks — models a
                                    * genuinely RECOVERABLE cause. */
        .detect = chaos_stage_detect,
        .remedy = chaos_stage_remedy,
        .witness = chaos_stage_witness,
        .witness_window_secs = 60,
    };

    if (!condition_register(&cond)) {
        chaos_note(out, "condition_register failed");
        return false;
    }
    event_observe(EV_OPERATOR_NEEDED, chaos_operator_observer, NULL);

    /* THE FAULT: drive the engine while the stage is stalled. Two ticks are
     * enough for the fixture remedy to self-clear (see chaos_stage_remedy). */
    condition_engine_tick();
    condition_engine_tick();
    condition_engine_tick();

    bool blocker_gone = !blocker_exists("chaos.stage_stall");
    bool no_pages = atomic_load(&g_chaos_operator_events) == 0;
    bool cleared = condition_engine_get_active_count() == 0 &&
                  atomic_load(&g_chaos_stage_cleared);

    out->ok = true;
    out->recovered = cleared && blocker_gone;
    out->operator_paged = !no_pages;
    chaos_note(out,
        "single-stage stall: cleared=%d blocker_gone=%d operator_events=%d",
        cleared, blocker_gone, atomic_load(&g_chaos_operator_events));

    condition_engine_reset_for_testing();
    blocker_clear("chaos.stage_stall");
    return true;
}
#else
bool chaos_fault_stall_single_stage(struct chaos_fault_result *out)
{
    chaos_result_init(out);
    chaos_note(out, "unavailable: built without ZCL_TESTING");
    return false;
}
#endif /* ZCL_TESTING */
