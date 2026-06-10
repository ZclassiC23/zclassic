/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit test for block_index_backfill_canonical_chain() — the one-time
 * historical-chain seed that makes block_index_projection a COMPLETE
 * durable store.
 *
 * What it proves
 * --------------
 *   1. A small legacy-style canonical chain (N blocks, real nSolution
 *      persisted ONLY in the legacy LevelDB, in-memory entries carrying
 *      nSolution=NULL exactly as the real loaders produce) backfills:
 *        - the event_log gains N EV_BLOCK_HEADER events;
 *        - block_index_projection folds to N entries with matching
 *          hash / height / nStatus AND non-empty nSolution (recovered
 *          from the LevelDB disk_block_index);
 *        - the durable flag is set;
 *        - a SECOND backfill call is a no-op (idempotent, already_done).
 *   2. load_block_index_from_projection() on a fresh chainstate
 *      reproduces the same N-entry map + the canonical tip.
 *   3. nSolution that is already resident in RAM (synthetic / freshly
 *      connected tip) is emitted without a disk read.
 *   4. NULL projection / no active tip are safe no-ops.
 *
 * The nSolution-via-LevelDB path is the load-bearing correctness concern:
 * the in-memory block_index never retains a solution, so the backfill must
 * read it back from the legacy disk index — this test asserts the folded,
 * round-tripped header carries the correct non-empty solution bytes.
 *
 * Scratch files live under ./test-tmp/bib_<pid>_<tag>/ per the project's
 * no-/tmp convention.
 */

#include "test/test_helpers.h"

#include "services/block_index_backfill.h"
#include "services/block_index_loader.h"
#include "storage/block_index_db.h"
#include "storage/block_index_projection.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/event_log_singleton.h"
#include "storage/progress_store.h"
#include "storage/txdb.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BIB_CHECK(name, expr) do { \
    printf("block_index_backfill: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Distinctive per-height solution bytes so a round-tripped solution can
 * be byte-compared. Length 1344 = real Equihash 200,9 solution size. */
#define BIB_SOLUTION_LEN 1344u
static void bib_fill_solution(unsigned char sol[BIB_SOLUTION_LEN], int height)
{
    for (unsigned i = 0; i < BIB_SOLUTION_LEN; i++)
        sol[i] = (unsigned char)((height * 31 + (int)i * 7 + 0x11) & 0xFF);
}

/* Build a disk_block_index for `height` chained to `prev_hash` (NULL for
 * genesis) with a deterministic real solution, returning its computed
 * hash via `*out_hash`. */
static void bib_make_dbi(struct disk_block_index *dbi, int height,
                         const struct uint256 *prev_hash,
                         struct uint256 *out_hash)
{
    disk_block_index_init(dbi);
    if (prev_hash) dbi->hashPrev = *prev_hash;
    dbi->nHeight  = height;
    dbi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
    dbi->nTx      = 1;
    dbi->nFile    = height / 1000;
    dbi->nDataPos = (unsigned)(height * 2048 + 100);
    dbi->nUndoPos = (unsigned)(height * 64 + 7);
    dbi->nVersion = 4;
    dbi->nTime    = 1700000000u + (uint32_t)height * 150u;
    dbi->nBits    = 0x1f07ffffu;
    /* Distinctive nNonce + merkle so each header hashes uniquely. */
    memset(dbi->nNonce.data, 0, 32);
    dbi->nNonce.data[0] = (uint8_t)(height & 0xFF);
    dbi->nNonce.data[1] = (uint8_t)((height >> 8) & 0xFF);
    memset(dbi->hashMerkleRoot.data, 0, 32);
    dbi->hashMerkleRoot.data[0] = (uint8_t)((height + 1) & 0xFF);
    dbi->hashMerkleRoot.data[31] = 0x5A;
    dbi->nSolutionSize = BIB_SOLUTION_LEN;
    bib_fill_solution(dbi->nSolution, height);
    disk_block_index_get_hash(dbi, out_hash);
}

/* ── Test 1: full backfill, LevelDB nSolution recovery, idempotency,
 *            and projection -> map rebuild round-trip ─────────────────── */

static int run_backfill_full(int *failures)
{
    int start_failures = *failures;
    const int N = 24;

    char dir[256];
    snprintf(dir, sizeof(dir), "./test-tmp/bib_%d_full", (int)getpid());
    mkdir("./test-tmp", 0755);
    mkdir(dir, 0755);

    char el_path[320];   snprintf(el_path, sizeof(el_path), "%s/event_log.dat", dir);
    char bip_path[320];  snprintf(bip_path, sizeof(bip_path), "%s/bip.db", dir);
    char btdb_path[320]; snprintf(btdb_path, sizeof(btdb_path), "%s/blocktree", dir);

    /* --- (1) Build the legacy LevelDB index (carries the real solutions)
     * BEFORE the event_log singleton is wired, so its own projection emit on
     * write does NOT pollute the log we measure. --- */
    struct block_tree_db btdb;
    bool btdb_ok = block_tree_db_open(&btdb, btdb_path, 1 << 20, false, true);
    BIB_CHECK("full: blocktree opens", btdb_ok);
    if (!btdb_ok) goto done;

    struct uint256 hashes[64];
    bool write_ok = true;
    for (int h = 0; h < N; h++) {
        struct disk_block_index dbi;
        bib_make_dbi(&dbi, h, h > 0 ? &hashes[h - 1] : NULL, &hashes[h]);
        write_ok = write_ok &&
                   block_tree_db_write_block_index(&btdb, &dbi);
    }
    BIB_CHECK("full: wrote N disk block indexes", write_ok);

    /* --- (2) Build the in-memory canonical chain. Mirror the real
     * loaders: nSolution is NOT retained in RAM (NULL/0), so the backfill
     * MUST recover it from the LevelDB index. --- */
    struct main_state ms;
    main_state_init(&ms);
    for (int h = 0; h < N; h++) {
        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)&ms, &hashes[h]);
        if (!pi) { write_ok = false; break; }
        pi->nHeight  = h;
        pi->nBits    = 0x1f07ffffu;
        pi->nTime    = 1700000000u + (uint32_t)h * 150u;
        pi->nVersion = 4;
        pi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx      = 1;
        pi->nFile    = h / 1000;
        pi->nDataPos = (unsigned)(h * 2048 + 100);
        pi->nUndoPos = (unsigned)(h * 64 + 7);
        pi->nNonce.data[0] = (uint8_t)(h & 0xFF);
        pi->nNonce.data[1] = (uint8_t)((h >> 8) & 0xFF);
        memset(pi->hashMerkleRoot.data, 0, 32);
        pi->hashMerkleRoot.data[0] = (uint8_t)((h + 1) & 0xFF);
        pi->hashMerkleRoot.data[31] = 0x5A;
        pi->nSolution     = NULL;   /* loaders never retain it */
        pi->nSolutionSize = 0;
    }
    /* phashBlock can be invalidated by block_map rehashing during inserts
     * — refresh exactly as the loaders' post-load pass does. */
    {
        size_t it = 0; struct block_index *pi; const struct uint256 *hp;
        while (block_map_next(&ms.map_block_index, &it, &hp, &pi))
            if (pi) pi->phashBlock = hp;
    }
    /* Link pprev + set the active tip = the most-work canonical lineage. */
    for (int h = 1; h < N; h++) {
        struct block_index *cur  = block_map_find(&ms.map_block_index, &hashes[h]);
        struct block_index *prev = block_map_find(&ms.map_block_index, &hashes[h - 1]);
        if (cur && prev) cur->pprev = prev;
    }
    struct block_index *tip = block_map_find(&ms.map_block_index, &hashes[N - 1]);
    BIB_CHECK("full: tip found", tip != NULL);
    BIB_CHECK("full: set active tip",
              tip && active_chain_move_window_tip(&ms.chain_active, tip));

    /* --- (3) Wire the event_log + projection. --- */
    event_log_t *log = event_log_open(el_path);
    BIB_CHECK("full: event log opens", log != NULL);
    if (!log) { main_state_free(&ms); block_tree_db_close(&btdb); goto done; }
    event_log_set_singleton(log);

    block_index_projection_t *bip = block_index_projection_open(bip_path, log);
    BIB_CHECK("full: projection opens", bip != NULL);
    if (!bip) {
        event_log_set_singleton(NULL); event_log_close(log);
        main_state_free(&ms); block_tree_db_close(&btdb); goto done;
    }

    uint64_t log_size_before = event_log_size(log);

    /* --- (4) Run the backfill. --- */
    struct block_index_backfill_result res;
    struct zcl_result rr = block_index_backfill_canonical_chain(&ms, bip, &btdb, &res);
    BIB_CHECK("full: backfill returns ok", rr.ok);
    BIB_CHECK("full: backfill ran (not already-done)", res.ran && !res.already_done);
    BIB_CHECK("full: walked == N", res.walked == (uint64_t)N);
    BIB_CHECK("full: emitted == N", res.emitted == (uint64_t)N);
    BIB_CHECK("full: every solution came from the LevelDB disk read",
              res.solution_disk_reads == (uint64_t)N);

    /* The log grew (N EV_BLOCK_HEADER events appended). */
    BIB_CHECK("full: event log grew", event_log_size(log) > log_size_before);

    /* Projection folded to exactly N entries. */
    BIB_CHECK("full: projection count == N",
              block_index_projection_count(bip) == (uint64_t)N);

    /* Each entry round-trips with matching hash/height/nStatus AND a
     * non-empty solution equal to the bytes we persisted. */
    bool entries_ok = true, solutions_ok = true;
    for (int h = 0; h < N && entries_ok; h++) {
        struct disk_block_index got;
        disk_block_index_init(&got);
        if (!block_index_projection_get(bip, hashes[h].data, &got)) {
            entries_ok = false; break;
        }
        if (got.nHeight != h) entries_ok = false;
        if (got.nStatus != (unsigned)(BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA))
            entries_ok = false;
        /* hash identity: re-derive from the folded disk index. */
        struct uint256 rehash;
        disk_block_index_get_hash(&got, &rehash);
        if (!uint256_eq(&rehash, &hashes[h])) entries_ok = false;
        /* nSolution: non-empty and byte-exact. */
        if (got.nSolutionSize != BIB_SOLUTION_LEN) { solutions_ok = false; }
        else {
            unsigned char want[BIB_SOLUTION_LEN];
            bib_fill_solution(want, h);
            if (memcmp(got.nSolution, want, BIB_SOLUTION_LEN) != 0)
                solutions_ok = false;
        }
    }
    BIB_CHECK("full: folded entries match hash/height/nStatus", entries_ok);
    BIB_CHECK("full: folded headers carry correct non-empty nSolution",
              solutions_ok);

    /* Durable flag set. */
    BIB_CHECK("full: backfill flag is set",
              block_index_projection_backfill_done(bip));

    /* --- (5) Second call is a no-op (idempotent). --- */
    uint64_t log_size_after_first = event_log_size(log);
    struct block_index_backfill_result res2;
    struct zcl_result rr2 = block_index_backfill_canonical_chain(&ms, bip, &btdb, &res2);
    BIB_CHECK("full: second backfill returns ok", rr2.ok);
    BIB_CHECK("full: second backfill is already_done no-op",
              res2.already_done && !res2.ran && res2.emitted == 0);
    BIB_CHECK("full: second backfill appended nothing",
              event_log_size(log) == log_size_after_first);
    BIB_CHECK("full: projection still N entries after second call",
              block_index_projection_count(bip) == (uint64_t)N);

    /* --- (6) Rebuild a FRESH chainstate purely from the projection.
     * Seed the tip_finalize cursor so the rebuild can publish the tip. --- */
    progress_store_close();
    bool pk_ok = progress_store_open(dir);
    BIB_CHECK("full: progress store opens", pk_ok);
    sqlite3 *pk = pk_ok ? progress_store_db() : NULL;
    if (pk) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(pk,
            "INSERT OR REPLACE INTO stage_cursor (name, cursor, updated_at) "
            "VALUES ('tip_finalize', ?, 0)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (int64_t)N);   /* cursor = N, tip = N-1 */
            (void)sqlite3_step(st);
            sqlite3_finalize(st);
        }
        sqlite3_exec(pk,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
            "  height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL,"
            "  work_delta_high INTEGER NOT NULL, work_delta_low INTEGER NOT NULL,"
            "  utxo_size_after INTEGER NOT NULL, reorg_depth INTEGER NOT NULL,"
            "  finalized_at INTEGER NOT NULL, tip_hash BLOB)",
            NULL, NULL, NULL);
        st = NULL;
        if (sqlite3_prepare_v2(pk,
            "INSERT OR REPLACE INTO tip_finalize_log "
            "(height,status,ok,work_delta_high,work_delta_low,utxo_size_after,"
            " reorg_depth,finalized_at,tip_hash) "
            "VALUES (?, 'finalized', 1, 0, 0, 0, 0, 0, ?)", -1, &st, NULL)
            == SQLITE_OK) {
            sqlite3_bind_int(st, 1, N - 1);
            sqlite3_bind_blob(st, 2, hashes[N - 1].data, 32, SQLITE_STATIC);
            (void)sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }

    struct main_state ms2;
    main_state_init(&ms2);
    bool rebuilt = load_block_index_from_projection(&ms2, chain_params_get(),
                                                    bip, pk);
    BIB_CHECK("full: rebuild from projection returns true", rebuilt);
    BIB_CHECK("full: rebuilt map size == N",
              ms2.map_block_index.size == (size_t)N);
    bool rb_heights_ok = true;
    for (int h = 0; h < N && rb_heights_ok; h++) {
        struct block_index *bi = block_map_find(&ms2.map_block_index, &hashes[h]);
        if (!bi || bi->nHeight != h) rb_heights_ok = false;
    }
    BIB_CHECK("full: rebuilt all heights present", rb_heights_ok);
    struct block_index *rb_tip = active_chain_tip(&ms2.chain_active);
    BIB_CHECK("full: rebuilt active tip set", rb_tip != NULL);
    BIB_CHECK("full: rebuilt tip height == N-1",
              rb_tip && rb_tip->nHeight == N - 1);
    BIB_CHECK("full: rebuilt tip hash == canonical tip",
              rb_tip && rb_tip->phashBlock &&
              uint256_eq(rb_tip->phashBlock, &hashes[N - 1]));

    main_state_free(&ms2);
    progress_store_close();
    block_index_projection_close(bip);
    event_log_set_singleton(NULL);
    event_log_close(log);
    main_state_free(&ms);
    block_tree_db_close(&btdb);
    test_cleanup_tmpdir(dir);
done:
    return *failures - start_failures;
}

/* ── Test 2: in-RAM solution is emitted without a disk read ──────────── */

static int run_backfill_inram_solution(int *failures)
{
    int start_failures = *failures;
    const int N = 8;

    char dir[256];
    snprintf(dir, sizeof(dir), "./test-tmp/bib_%d_inram", (int)getpid());
    mkdir("./test-tmp", 0755);
    mkdir(dir, 0755);

    char el_path[320];  snprintf(el_path, sizeof(el_path), "%s/event_log.dat", dir);
    char bip_path[320]; snprintf(bip_path, sizeof(bip_path), "%s/bip.db", dir);

    struct uint256 hashes[16];
    /* Hashes derived the same way (so the projection get-by-hash works),
     * but here we ALSO keep the solution resident in RAM and pass NO btdb;
     * the backfill must emit from RAM without any disk read. */
    for (int h = 0; h < N; h++) {
        struct disk_block_index tmp;
        bib_make_dbi(&tmp, h, h > 0 ? &hashes[h - 1] : NULL, &hashes[h]);
    }

    struct main_state ms;
    main_state_init(&ms);
    unsigned char *ram_sols[16] = {0};
    for (int h = 0; h < N; h++) {
        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)&ms, &hashes[h]);
        if (!pi) continue;
        pi->nHeight  = h;
        pi->nBits    = 0x1f07ffffu;
        pi->nTime    = 1700000000u + (uint32_t)h * 150u;
        pi->nVersion = 4;
        pi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx      = 1;
        pi->nNonce.data[0] = (uint8_t)(h & 0xFF);
        pi->nNonce.data[1] = (uint8_t)((h >> 8) & 0xFF);
        pi->hashMerkleRoot.data[0] = (uint8_t)((h + 1) & 0xFF);
        pi->hashMerkleRoot.data[31] = 0x5A;
        ram_sols[h] = (unsigned char *)malloc(BIB_SOLUTION_LEN);
        bib_fill_solution(ram_sols[h], h);
        pi->nSolution     = ram_sols[h];     /* resident in RAM */
        pi->nSolutionSize = BIB_SOLUTION_LEN;
    }
    {
        size_t it = 0; struct block_index *pi; const struct uint256 *hp;
        while (block_map_next(&ms.map_block_index, &it, &hp, &pi))
            if (pi) pi->phashBlock = hp;
    }
    for (int h = 1; h < N; h++) {
        struct block_index *cur  = block_map_find(&ms.map_block_index, &hashes[h]);
        struct block_index *prev = block_map_find(&ms.map_block_index, &hashes[h - 1]);
        if (cur && prev) cur->pprev = prev;
    }
    struct block_index *tip = block_map_find(&ms.map_block_index, &hashes[N - 1]);
    (void)active_chain_move_window_tip(&ms.chain_active, tip);

    event_log_t *log = event_log_open(el_path);
    if (!log) { *failures += 1; printf("inram: event_log_open FAIL\n"); goto cleanup; }
    event_log_set_singleton(log);
    block_index_projection_t *bip = block_index_projection_open(bip_path, log);
    if (!bip) {
        event_log_set_singleton(NULL); event_log_close(log);
        *failures += 1; printf("inram: projection_open FAIL\n"); goto cleanup;
    }

    /* No btdb (NULL) — solutions must come from RAM, no disk reads. */
    struct block_index_backfill_result res;
    struct zcl_result rr = block_index_backfill_canonical_chain(&ms, bip, NULL, &res);
    BIB_CHECK("inram: backfill returns ok", rr.ok);
    BIB_CHECK("inram: emitted == N", res.emitted == (uint64_t)N);
    BIB_CHECK("inram: zero disk reads (solution was in RAM)",
              res.solution_disk_reads == 0);
    BIB_CHECK("inram: projection count == N",
              block_index_projection_count(bip) == (uint64_t)N);

    bool sols_ok = true;
    for (int h = 0; h < N; h++) {
        struct disk_block_index got;
        disk_block_index_init(&got);
        if (!block_index_projection_get(bip, hashes[h].data, &got) ||
            got.nSolutionSize != BIB_SOLUTION_LEN) { sols_ok = false; break; }
        unsigned char want[BIB_SOLUTION_LEN];
        bib_fill_solution(want, h);
        if (memcmp(got.nSolution, want, BIB_SOLUTION_LEN) != 0) sols_ok = false;
    }
    BIB_CHECK("inram: folded headers carry correct in-RAM nSolution", sols_ok);

    block_index_projection_close(bip);
    event_log_set_singleton(NULL);
    event_log_close(log);
cleanup:
    main_state_free(&ms);
    for (int h = 0; h < N; h++) free(ram_sols[h]);
    test_cleanup_tmpdir(dir);
    return *failures - start_failures;
}

/* ── Test 3: NULL projection + no-active-tip are safe no-ops ─────────── */

static int run_backfill_noops(int *failures)
{
    int start_failures = *failures;

    struct main_state ms;
    main_state_init(&ms);

    /* NULL projection. */
    struct block_index_backfill_result res;
    struct zcl_result rr = block_index_backfill_canonical_chain(&ms, NULL, NULL, &res);
    BIB_CHECK("noop: NULL projection returns ok", rr.ok);
    BIB_CHECK("noop: NULL projection did not run",
              !res.ran && res.emitted == 0);

    /* Real (empty) projection but NO active tip -> nothing to walk, flag
     * left UNSET so a later boot (with a tip) does the real backfill. */
    char dir[256];
    snprintf(dir, sizeof(dir), "./test-tmp/bib_%d_noop", (int)getpid());
    mkdir("./test-tmp", 0755);
    mkdir(dir, 0755);
    char el_path[320];  snprintf(el_path, sizeof(el_path), "%s/event_log.dat", dir);
    char bip_path[320]; snprintf(bip_path, sizeof(bip_path), "%s/bip.db", dir);

    event_log_t *log = event_log_open(el_path);
    if (log) {
        event_log_set_singleton(log);
        block_index_projection_t *bip =
            block_index_projection_open(bip_path, log);
        if (bip) {
            struct block_index_backfill_result r2;
            struct zcl_result rr2 = block_index_backfill_canonical_chain(&ms, bip, NULL, &r2);
            BIB_CHECK("noop: no-tip returns ok", rr2.ok);
            BIB_CHECK("noop: no-tip did not run", !r2.ran);
            BIB_CHECK("noop: no-tip left flag UNSET (retry later)",
                      !block_index_projection_backfill_done(bip));
            block_index_projection_close(bip);
        }
        event_log_set_singleton(NULL);
        event_log_close(log);
    }

    main_state_free(&ms);
    test_cleanup_tmpdir(dir);
    return *failures - start_failures;
}

int test_block_index_backfill(void)
{
    printf("\n=== block_index_backfill tests ===\n");
    int failures = 0;

    run_backfill_full(&failures);
    run_backfill_inram_solution(&failures);
    run_backfill_noops(&failures);

    printf("block_index_backfill: %d failures\n", failures);
    return failures;
}
