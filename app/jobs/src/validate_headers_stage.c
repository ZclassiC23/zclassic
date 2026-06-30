/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_stage — implementation. See validate_headers_stage.h.
 *
 * Single-process singleton. Validates header_admit_log output via a
 * fixed pthread pool (VH_POOL_SIZE workers) and a per-step bounded
 * batch. The pool stays warm for the lifetime of the stage; each step
 * submits up to VH_BATCH_SIZE jobs, awaits them all, and writes the
 * batch + cursor bump atomically. */

#include "platform/time_compat.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/stage_helpers.h"
#include "jobs/stage_db_fault.h"
#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"  /* STAGE_REPAIR_SOLUTIONLESS_REASON */
#include "validate_headers_internal.h"
#include "validate_headers_pool.h"
#include "jobs/header_admit_stage.h"
#include "validate_headers_log_store.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "json/json.h"
#include "primitives/block.h"
#include "storage/progress_store.h"
#include "storage/txdb.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/check_block.h"
#include "validation/main_state.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE_NAME       "validate_headers"

/* The default header validator (PoW target + Equihash-from-nSolution)
 * lives in validate_headers_validator.c — see validate_headers_internal.h. */

/* ── Job + pool state ─────────────────────────────────────────────── */

struct vh_job {
    const struct block_index *bi;       /* in: validation input */
    struct block_index       *mark_bi;  /* in: real index to mark on pass */
    struct block_index        snapshot; /* in: optional repaired-header copy */
    unsigned char             solution[MAX_SOLUTION_SIZE];
    int                       height;   /* in: convenience for logging */
    bool                      ok;       /* out */
    char                      reason[VH_MAX_REASON];
};

/* ── Globals ──────────────────────────────────────────────────────── */

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t          *g_stage = NULL;
static struct vh_pool    g_pool;

static _Atomic uint64_t g_passed_total = 0;
static _Atomic uint64_t g_failed_total = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_failure_recheck_cursor = 0;

/* Infra-db fault ladder (R5). A momentary sqlite glitch (busy/locked/transient
 * IO) must NOT be a dead JOB_FATAL: a transient fault holds the cursor and
 * retries (JOB_IDLE), a persistent/permanent one routes to the bounded
 * auto-reindex. Reset on every advancing step. NEVER used for a validity
 * verdict — those advance the cursor with an ok=0/ok=1 row. */
static struct stage_db_fault g_vh_db_fault;

/* Injectable validator. Default = full PoW + Equihash from disk.
 * Tests set this via validate_headers_stage_set_validator(). Reset to
 * default on shutdown. */
static vh_validator_fn g_validator      = NULL;
static void           *g_validator_user = NULL;
/* Datadir cached at init for the workers (g_validator may need it). */
static char            g_datadir[2048] = {0};

/* Map a sqlite failure inside the step body to the result the step must return.
 * `rc` is the sqlite (extended) code captured AT the failing call. Returns
 * JOB_IDLE (transient, within budget — cursor held, supervisor re-ticks) or
 * JOB_FATAL (persistent/permanent — a bounded auto-reindex was requested). */
static job_result_t vh_db_fault(int rc, int height, const char *ctx)
{
    return stage_db_fault_result(&g_vh_db_fault, rc, g_datadir,
                                 (int32_t)height, ctx);
}

/* A validate_headers failure is REPAIRABLE — worth keeping in the recheck
 * window until it can be re-validated — ONLY when it failed for lack of a
 * backfillable Equihash solution (STAGE_REPAIR_SOLUTIONLESS_REASON, the exact
 * reason the live tip-wedge produced: the header is canonical but its solution
 * had not yet been backfilled when the forward drain first reached it).
 * recheck_failed_rows retries these until backfill_header_solutions supplies
 * the solution and the unchanged PoW+Equihash validator flips the row to ok=1.
 *
 * A TERMINAL failure (high-hash / invalid-solution / version-too-low / a test
 * stub) will never pass, so the recheck floor must ADVANCE past it — never pin
 * — or recheck would re-run the validator on a permanently-bad header every
 * tick. Restricting the pin to repairable rows is what keeps the floor's
 * forward-progress behavior identical to before for genuine rejections. */
static inline bool vh_failure_is_repairable(const char *reason)
{
    return reason &&
           (strcmp(reason, STAGE_REPAIR_SOLUTIONLESS_REASON) == 0 ||
            strcmp(reason, "header-source-hash-mismatch") == 0);
}

/* ── Worker pool ──────────────────────────────────────────────────── */

static void vh_run_job(void *jobp, void *user)
{
    (void)user;
    struct vh_job *job = jobp;
    /* Snapshot validator under no lock — it can only change via
     * shutdown which joins workers first. */
    job->reason[0] = 0;
    job->ok = g_validator(job->bi, g_datadir,
                          job->reason, sizeof(job->reason),
                          g_validator_user);
}

/* ── Schema + log writes ──────────────────────────────────────────────
 * validate_headers_log schema + insert live in validate_headers_log_store.c
 * (pure sqlite kernel helpers below the AR layer). This stage owns only the
 * vh_job → primitive binding at the call sites. */

static bool mark_valid_header(struct block_index *bi);

/* Resolve the block_index at height `h`, including the live frontier ONE
 * (or more) above the finalized active-chain window.
 *
 * active_chain_at is a finalized-window accessor: it returns NULL for any
 * height above c->height (chainstate.c). That is by design and MUST stay
 * that way — every other reader depends on its finalized-window contract,
 * so we never "fix" it to peek the map. Instead, when the window does not
 * cover `h`, reach the canonical header via the block_map (pprev/pskip)
 * up to the best-header frontier. This only changes HOW bi is found; the
 * resolved bi still flows through the SAME validator (PoW + Equihash) and
 * is never marked valid without that verdict. No window extend (the
 * window-extend machinery is the tip_finalize oscillator we must not
 * poke) and no array-bound dependence. */
static struct block_index *vh_resolve_bi(struct main_state *ms, int h)
{
    if (!ms)
        return NULL;
    struct block_index *bi = active_chain_at(&ms->chain_active, h);
    if (bi)
        return bi;
    if (ms->pindex_best_header && h <= ms->pindex_best_header->nHeight)
        return block_index_get_ancestor(ms->pindex_best_header, h);
    return NULL;
}

static void vh_job_bind_block(sqlite3 *db, struct vh_job *job,
                              struct block_index *bi, int height)
{
    job->bi = bi;
    job->mark_bi = bi;
    job->height = height;

    if (!db || !bi || !bi->phashBlock)
        return;

    struct block_header repaired;
    if (!stage_repair_header_solution_load(db, height, bi->phashBlock,
                                           &repaired))
        return;

    job->snapshot = *bi;
    job->snapshot.nVersion = repaired.nVersion;
    job->snapshot.hashMerkleRoot = repaired.hashMerkleRoot;
    job->snapshot.hashFinalSaplingRoot = repaired.hashFinalSaplingRoot;
    job->snapshot.nTime = repaired.nTime;
    job->snapshot.nBits = repaired.nBits;
    job->snapshot.nNonce = repaired.nNonce;
    memcpy(job->solution, repaired.nSolution, repaired.nSolutionSize);
    job->snapshot.nSolution = job->solution;
    job->snapshot.nSolutionSize = repaired.nSolutionSize;
    job->bi = &job->snapshot;
}

/* Undo the recheck's write transaction. Batched (an outer stage_batch txn is
 * open): ROLLBACK TO then RELEASE the savepoint — undo only this recheck's
 * writes while leaving the outer batch (and any earlier advanced blocks in it)
 * open and the savepoint name free. Standalone: plain ROLLBACK of our own txn.
 * Mirrors stage_step_rollback in lib/util/src/stage.c. Best-effort. */
static void vh_recheck_rollback(sqlite3 *db, bool batched)
{
    if (batched) {
        sqlite3_exec(db, "ROLLBACK TO SAVEPOINT vh_recheck", NULL, NULL, NULL);
        sqlite3_exec(db, "RELEASE SAVEPOINT vh_recheck", NULL, NULL, NULL);
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
}

static job_result_t recheck_failed_rows(struct main_state *ms,
                                          sqlite3 *db,
                                          uint64_t validated_cursor)
{
    if (!ms || !db || validated_cursor == 0)
        return JOB_IDLE;

    int64_t start = atomic_load(&g_failure_recheck_cursor);
    if (start < 0)
        start = 0;

    /* Anchor the scan at the live pipeline frontier so stale reindex-artifact
     * ok=0 rows (cold-imported solutionless headers far below the tip) can
     * never pin the in-process floor and starve the bounded (LIMIT
     * VH_BATCH_SIZE) batch before it reaches the real frontier — the live
     * tip-wedge. The frontier is the LOWER of two durable cursors: tip_finalize
     * (= finalized_tip+1) and body_fetch (which JOB_BLOCKs on a live
     * solutionless ok=0 row, so its cursor is the height that must not be
     * skipped even if a torn WAL restart leaves the finalized seed above it).
     * Taking the lower can only widen the scan toward an earlier live row,
     * never skip one; being persisted, it is independent of the never-persisted
     * floor's boot value (converges identically after kill-9). Verdict-
     * preserving: only narrows WHICH heights reach the unchanged validator. */
    {
        uint64_t fin_cursor = 0;
        uint64_t bf_cursor = 0;
        if (!stage_cursor_read_or_zero(db, "tip_finalize", STAGE_NAME,
                                       &fin_cursor) ||
            !stage_cursor_read_or_zero(db, "body_fetch", STAGE_NAME,
                                       &bf_cursor))
            return vh_db_fault(sqlite3_extended_errcode(db), (int)start,
                               "recheck frontier cursor read");
        int64_t fin_cur = (int64_t)fin_cursor;
        int64_t bf_cur  = (int64_t)bf_cursor;
        int64_t frontier = fin_cur;
        if (bf_cur > 0 && (frontier <= 0 || bf_cur < frontier))
            frontier = bf_cur;
        if (frontier > start)
            start = frontier;
    }

    if ((uint64_t)start >= validated_cursor)
        return JOB_IDLE;

    progress_store_tx_lock();
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT height FROM validate_headers_log"
        " WHERE ok=0 AND height>=? AND height<?"
        " ORDER BY height ASC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        progress_store_tx_unlock();
        LOG_ERR("validate_headers",
                "failed-row recheck query prepare failed");
        return vh_db_fault(rc, (int)start, "recheck query prepare");
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)validated_cursor);
    sqlite3_bind_int(stmt, 3, VH_BATCH_SIZE);

    struct vh_job jobs[VH_BATCH_SIZE];
    memset(jobs, 0, sizeof(jobs));
    int n = 0;
    int64_t last_seen = start - 1;
    int64_t first_unresolved = -1;
    while (n < VH_BATCH_SIZE &&
           (rc = sqlite3_step(stmt)) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        int64_t h64 = sqlite3_column_int64(stmt, 0);
        last_seen = h64;
        if (h64 < 0 || h64 > INT32_MAX) {
            sqlite3_finalize(stmt);
            progress_store_tx_unlock();
            LOG_ERR("validate_headers",
                    "failed-row recheck height out of range");
            return JOB_FATAL;
        }
        struct block_index *bi = vh_resolve_bi(ms, (int)h64);
        if (!bi || !bi->phashBlock) {
            /* Transient resolve miss (e.g. a concurrent reorg between admit and
             * recheck). SKIP this one height instead of aborting the entire
             * pass — one unresolvable row must never strand every later
             * frontier row — and remember the lowest such height so the floor
             * is never advanced past it (the next pass retries it). */
            atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
            if (first_unresolved < 0)
                first_unresolved = h64;
            continue;
        }
        vh_job_bind_block(db, &jobs[n], bi, (int)h64);
        n++;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        progress_store_tx_unlock();
        LOG_ERR("validate_headers", "failed-row recheck query failed");
        return vh_db_fault(rc, (int)last_seen, "recheck query step");
    }
    sqlite3_finalize(stmt);
    progress_store_tx_unlock();

    if (n == 0) {
        /* If every selected row was skipped (unresolvable), pin at the lowest
         * skipped height so the next pass retries it; only a genuinely empty
         * scan may jump the floor to validated_cursor. */
        int64_t idle_floor = (first_unresolved >= 0)
                             ? first_unresolved : (int64_t)validated_cursor;
        atomic_store(&g_failure_recheck_cursor, idle_floor);
        return JOB_IDLE;
    }

    vh_pool_run_batch(&g_pool, jobs, sizeof(jobs[0]), n);
    char *err = NULL;
    progress_store_tx_lock();
    /* This path runs both standalone (validate_headers_stage_step_once with no
     * outer txn) and inside the bounded drain (STAGE_DRAIN_IMPL → stage_run_once
     * → here), where stage_batch_begin has already opened a BEGIN IMMEDIATE on
     * this same connection. A bare BEGIN here would nest a transaction inside
     * that batch ("cannot start a transaction within a transaction") and the
     * recheck/repair would never run — the failed-row self-heal silently
     * disabled. Mirror stage_run_once's contract: open a SAVEPOINT when a batch
     * is active (its writes stay enrolled in the batch, committed once by
     * stage_batch_end), a fresh BEGIN IMMEDIATE only when standalone. */
    const bool vh_batched = stage_batch_active();
    const char *vh_open   = vh_batched ? "SAVEPOINT vh_recheck" : "BEGIN IMMEDIATE";
    const char *vh_commit = vh_batched ? "RELEASE SAVEPOINT vh_recheck" : "COMMIT";
    if (sqlite3_exec(db, vh_open, NULL, NULL, &err) != SQLITE_OK) {
        int frc = sqlite3_extended_errcode(db);
        LOG_WARN("validate_headers", "[validate_headers] failed-row recheck BEGIN failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return vh_db_fault(frc, (int)start, "recheck begin/savepoint");
    }
    for (int i = 0; i < n; i++) {
        if (jobs[i].ok &&
            !mark_valid_header(jobs[i].mark_bi)) {
            LOG_WARN("validate_headers", "[validate_headers] recheck mark failed height=%d", jobs[i].height);
            vh_recheck_rollback(db, vh_batched);
            progress_store_tx_unlock();
            return JOB_FATAL;
        }
        if (!validate_headers_log_insert(db, jobs[i].height,
                                         jobs[i].bi->phashBlock, jobs[i].ok,
                                         jobs[i].reason)) {
            int frc = sqlite3_extended_errcode(db);
            vh_recheck_rollback(db, vh_batched);
            progress_store_tx_unlock();
            return vh_db_fault(frc, jobs[i].height, "recheck log insert");
        }
        if (jobs[i].ok)
            atomic_fetch_add(&g_passed_total, 1);
        else
            atomic_fetch_add(&g_failed_total, 1);
    }
    if (sqlite3_exec(db, vh_commit, NULL, NULL, &err) != SQLITE_OK) {
        int frc = sqlite3_extended_errcode(db);
        LOG_WARN("validate_headers", "[validate_headers] failed-row recheck COMMIT failed: %s", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        vh_recheck_rollback(db, vh_batched);
        progress_store_tx_unlock();
        return vh_db_fault(frc, (int)last_seen, "recheck commit");
    }
    progress_store_tx_unlock();
    /* Advance the floor only past rows that RESOLVED to ok=1; pin it at the
     * lowest row still ok=0 so the next pass retries it once its solution is
     * backfilled. jobs[] is height-ascending, so the first !ok is the lowest
     * still-failing height. Advancing unconditionally to last_seen+1 (the old
     * behavior) re-stranded a repairable row the moment recheck touched it
     * before its solution landed — the same monotonic-strand defect as the
     * forward drain. Verdict-preserving: a row only leaves ok=0 via a genuine
     * PoW + Equihash pass in vh_pool_run_batch above. */
    int64_t lowest_still_failing = -1;
    for (int i = 0; i < n; i++)
        if (!jobs[i].ok && vh_failure_is_repairable(jobs[i].reason)) {
            lowest_still_failing = jobs[i].height; break;
        }
    int64_t new_floor = (lowest_still_failing >= 0)
                        ? lowest_still_failing : (last_seen + 1);
    /* Never advance the floor past a height skipped as unresolvable this pass —
     * it must be retried on the next pass, not stepped over. */
    if (first_unresolved >= 0 && new_floor > first_unresolved)
        new_floor = first_unresolved;
    atomic_store(&g_failure_recheck_cursor, new_floor);
    /* Only claim progress when the floor actually advanced. A pinned floor
     * (the lowest repairable row still has no backfilled solution) returns
     * JOB_IDLE so the stage backs off rather than busy-looping on a row it
     * cannot yet flip; the next poll retries it once the solution lands. */
    return (new_floor > start) ? JOB_ADVANCED : JOB_IDLE;
}

static bool mark_valid_header(struct block_index *bi)
{
    if (!bi || (bi->nStatus & BLOCK_FAILED_MASK))
        return false;
    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_HEADER)
        bi->nStatus = (bi->nStatus & ~BLOCK_VALID_MASK) |
                      BLOCK_VALID_HEADER;
    return true;
}

/* ── Step body ────────────────────────────────────────────────────── */

static job_result_t step_validate(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    reducer_extend_window_to_candidate(ms, true);
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    /* Floor: never get ahead of header_admit. The header_admit cursor
     * is "next height to admit" — so heights [0, ha_cursor-1] are
     * admitted. We can validate up to ha_cursor-1. */
    uint64_t ha_cursor = 0;
    if (!stage_cursor_read_or_zero(db, "header_admit", STAGE_NAME,
                                   &ha_cursor))
        return vh_db_fault(sqlite3_extended_errcode(db), next_h,
                           "header_admit cursor read");
    if ((uint64_t)next_h >= ha_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;  /* not BLOCKED — header_admit will catch up */
    }

    int available = (int)(ha_cursor - (uint64_t)next_h);
    int batch_n = (available < VH_BATCH_SIZE) ? available : VH_BATCH_SIZE;
    if (batch_n <= 0) return JOB_IDLE;

    /* Build the batch. Each job points to the block_index entry at
     * its target height. */
    struct vh_job jobs[VH_BATCH_SIZE];
    memset(jobs, 0, sizeof(jobs));
    for (int i = 0; i < batch_n; i++) {
        int h = next_h + i;
        struct block_index *bi = vh_resolve_bi(ms, h);
        if (!bi || !bi->phashBlock) {
            /* Neither the finalized window nor the best-header frontier
             * covers this height — shouldn't happen since header_admit
             * just admitted it, but a concurrent reorg between admission
             * and validation is conceivable. Block on this specific case
             * so the supervisor surfaces it. */
            atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
            return JOB_IDLE;
        }
        vh_job_bind_block(db, &jobs[i], bi, h);
    }

    /* Dispatch + await. Pool stays inside this step. */
    vh_pool_run_batch(&g_pool, jobs, sizeof(jobs[0]), batch_n);

    /* Write rows + bump cursor, all under the F-2 BEGIN IMMEDIATE txn. */
    for (int i = 0; i < batch_n; i++) {
        if (jobs[i].ok &&
            !mark_valid_header(jobs[i].mark_bi)) {
            LOG_WARN("validate_headers", "[validate_headers] authoritative mark failed height=%d", jobs[i].height);
            return JOB_FATAL;
        }
        if (!validate_headers_log_insert(db, jobs[i].height,
                                         jobs[i].bi->phashBlock, jobs[i].ok,
                                         jobs[i].reason))
            return vh_db_fault(sqlite3_extended_errcode(db), jobs[i].height,
                               "validate_headers_log insert");
        if (jobs[i].ok) atomic_fetch_add(&g_passed_total, 1);
        else            atomic_fetch_add(&g_failed_total, 1);
    }

    c->cursor_out = c->cursor_in + (uint64_t)batch_n;
    /* Failure-recheck floor: the lowest height recheck_failed_rows will
     * revisit. It must NEVER advance past a still-failing (ok=0) row. recheck
     * only scans [floor, validated_cursor); slaving the floor to the forward
     * cursor (the old `floor = max(floor, cursor_out)`) made recheck a no-op
     * for every row this drain logged ok=0 — they were stranded the instant
     * they were written. That is the live tip-wedge: validate_headers logged
     * height H ok=0 "no-header-solution-backfill-required" (its Equihash
     * solution not yet backfilled), the floor jumped past H, backfill_header_
     * solutions later supplied H's solution, but recheck never looked back —
     * so body_fetch stalled at H and the public tip froze one block below.
     * Pin the floor at the lowest failure in this batch; only advance to
     * cursor_out when the whole batch passed. This changes only WHICH rows are
     * re-validated, never the verdict — a row still flips to ok=1 solely
     * through the unchanged PoW + Equihash validator, so it can never admit an
     * invalid header (zero fork risk). Only REPAIRABLE (solutionless) failures
     * pin the floor; a terminal rejection advances it exactly as before, so
     * recheck never re-runs the validator on a permanently-bad header. */
    int64_t lowest_fail = -1;
    for (int i = 0; i < batch_n; i++)
        if (!jobs[i].ok && vh_failure_is_repairable(jobs[i].reason)) {
            lowest_fail = jobs[i].height; break;
        }
    int64_t recheck_floor = atomic_load(&g_failure_recheck_cursor);
    if (lowest_fail >= 0) {
        if (recheck_floor > lowest_fail)
            atomic_store(&g_failure_recheck_cursor, lowest_fail);
    } else if (recheck_floor < (int64_t)c->cursor_out) {
        atomic_store(&g_failure_recheck_cursor, (int64_t)c->cursor_out);
    }
    /* Clean advancing step — the infra-db fault retry budget resets. */
    stage_db_fault_clear(&g_vh_db_fault);
    return JOB_ADVANCED;
}

/* ── Public API ───────────────────────────────────────────────────── */

bool validate_headers_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("validate_headers", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("validate_headers",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);

    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("validate_headers",
                "init: already bound to a different main_state");
        return true;
    }

    if (!validate_headers_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    /* Cache the datadir for workers. GetDataDir is the canonical
     * accessor; it's stable for the process lifetime. */
    GetDataDir(true, g_datadir, sizeof(g_datadir));

    /* Default validator runs full PoW + Equihash from disk. */
    if (!g_validator) {
        g_validator      = validate_headers_default_validator;
        g_validator_user = NULL;
    }

    uint64_t persisted_cursor = 0;
    if (!stage_cursor_read_or_zero(db, STAGE_NAME, STAGE_NAME,
                                   &persisted_cursor)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    if (!vh_pool_start(&g_pool, vh_run_job, NULL)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_validate, NULL);
    if (!s) {
        vh_pool_stop(&g_pool);
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("validate_headers", "init: stage_create failed");
    }
    if (!stage_set_cursor(s, db, persisted_cursor)) {
        stage_destroy(s);
        vh_pool_stop(&g_pool);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    g_ms    = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("validate_headers",
             "[validate_headers] stage initialised (authoritative, pool=%d batch=%d)",
             VH_POOL_SIZE, VH_BATCH_SIZE);
    return true;
}

void validate_headers_stage_set_validator(vh_validator_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_validator      = fn ? fn : validate_headers_default_validator;
    g_validator_user = fn ? user : NULL;
    pthread_mutex_unlock(&g_lock);
}

job_result_t validate_headers_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    progress_store_tx_lock();

    job_result_t r = stage_run_once(g_stage, db);
    if (r != JOB_IDLE) {
        progress_store_tx_unlock();
        return r;
    }

    uint64_t validated_cursor = 0;
    if (!stage_cursor_read_or_zero(db, STAGE_NAME, STAGE_NAME,
                                   &validated_cursor)) {
        int frc = sqlite3_extended_errcode(db);
        progress_store_tx_unlock();
        return vh_db_fault(frc, 0, "step_once validated cursor read");
    }
    job_result_t recheck = recheck_failed_rows(g_ms, db, validated_cursor);
    if (recheck == JOB_ADVANCED)
        stage_db_fault_clear(&g_vh_db_fault);
    progress_store_tx_unlock();
    return recheck;
}

STAGE_DRAIN_IMPL(validate_headers)

void validate_headers_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    /* Pool must stop before stage_destroy so the workers' read of
     * g_validator stays valid. */
    vh_pool_stop(&g_pool);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_validator      = NULL;   /* re-default on next init */
    g_validator_user = NULL;
    atomic_store(&g_passed_total, (uint64_t)0);
    atomic_store(&g_failed_total, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_failure_recheck_cursor, (int64_t)0);
    stage_db_fault_clear(&g_vh_db_fault);
    pthread_mutex_unlock(&g_lock);
}

uint64_t validate_headers_stage_cursor(void)
{
    if (!g_stage)
        return 0;
    return stage_cursor(g_stage);
}

uint64_t validate_headers_stage_passed_total(void)
{
    return atomic_load(&g_passed_total);
}

uint64_t validate_headers_stage_failed_total(void)
{
    return atomic_load(&g_failed_total);
}

bool validate_headers_stage_has_pass_record(int32_t height,
                                            const struct uint256 *hash)
{
    sqlite3 *db = progress_store_db();
    if (!db || !hash || height < 0) return false;

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT ok FROM validate_headers_log WHERE height=? AND hash=?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        progress_store_tx_unlock();
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)height);
    sqlite3_bind_blob(st, 2, hash->data, 32, SQLITE_STATIC);

    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
        found = sqlite3_column_int(st, 0) == 1;
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return found;
}

/* validate_headers_stage_window_report() and the failure-summary loader
 * live in validate_headers_report.c — see validate_headers_internal.h. */

bool validate_headers_stage_dump_state_json(struct json_value *out,
                                             const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);
    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_str(out, "authority", "authoritative");
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "pool_size", (int64_t)VH_POOL_SIZE);
    json_push_kv_int (out, "batch_size", (int64_t)VH_BATCH_SIZE);
    json_push_kv_int (out, "passed_total",
                      (int64_t)atomic_load(&g_passed_total));
    json_push_kv_int (out, "failed_total",
                      (int64_t)atomic_load(&g_failed_total));
    json_push_kv_int (out, "last_step_unix",
                      atomic_load(&g_last_step_unix));
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "failure_recheck_cursor",
                      atomic_load(&g_failure_recheck_cursor));
    struct validate_headers_failure_summary failures;
    validate_headers_failure_summary_load(&failures);
    json_push_kv_int(out, "failure_log_count", failures.count);
    json_push_kv_int(out, "first_failed_height", failures.first_height);
    json_push_kv_str(out, "first_fail_reason", failures.first_reason);
    json_push_kv_int(out, "last_failed_height", failures.last_height);
    json_push_kv_str(out, "last_fail_reason", failures.last_reason);
    stage_dump_counters(out, g_stage);
    return true;
}
