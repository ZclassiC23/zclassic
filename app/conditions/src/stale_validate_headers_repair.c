/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "services/header_probe.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <limits.h>

static _Atomic int g_target_at_detect = -1;
static _Atomic int g_hstar_at_detect = -1;
static _Atomic int g_remedy_calls = 0;
static _Atomic int g_mode_at_detect = STAGE_REPAIR_POISON_NONE;

#ifdef ZCL_TESTING
static _Atomic int g_test_hstar_override = -1;

void stale_validate_headers_repair_test_set_hstar_override(int height);
void stale_validate_headers_repair_test_set_hstar_override(int height)
{
    atomic_store(&g_test_hstar_override, height);
}
#endif

static bool validate_repairable_mode(
    enum stage_repair_header_solution_poison mode)
{
    return mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS ||
           mode == STAGE_REPAIR_POISON_VALIDATE_HASH_MISMATCH;
}

static int reducer_frontier_height(sqlite3 *db)
{
    if (!db)
        return -1; // raw-return-ok:progress-db-not-open

#ifdef ZCL_TESTING
    int ov = atomic_load(&g_test_hstar_override);
    if (ov >= 0)
        return ov;
#endif

    progress_store_tx_lock();
    int32_t hstar = -1;
    int32_t served_floor = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served_floor);
    progress_store_tx_unlock();
    if (!ok)
        return -1; // raw-return-ok:hstar-read-failed
    return (int)hstar;
}

static int repair_target_height(sqlite3 *db)
{
    int scan = -1;
    bool have_scan =
        stage_repair_header_solution_repairable_validate_frontier(db, &scan) &&
        scan >= reducer_frontier_floor();

    int hstar = reducer_frontier_height(db);
    if (hstar < 0)
        return have_scan ? scan : -1; // raw-return-ok:no-repairable-frontier

    if (hstar >= INT_MAX)
        return have_scan ? scan : -1; // raw-return-ok:frontier-overflow

    int hstar_target = hstar + 1;
    if (have_scan && scan <= hstar_target)
        return scan;

    enum stage_repair_header_solution_poison hstar_mode =
        stage_repair_header_solution_poison_mode(db, hstar_target);
    if (hstar_mode != STAGE_REPAIR_POISON_NONE)
        return hstar_target;

    if (have_scan)
        return scan;
    return hstar_target;
}

#ifdef ZCL_TESTING
int stale_validate_headers_repair_test_repair_target(sqlite3 *db);
int stale_validate_headers_repair_test_repair_target(sqlite3 *db)
{
    return repair_target_height(db);
}
#endif

static bool detect_stale_validate_headers_repair(void)
{
    sqlite3 *db = progress_store_db();
    if (!db)
        return false;
    int target = repair_target_height(db);
    if (target < 0)
        return false;

    enum stage_repair_header_solution_poison mode =
        stage_repair_header_solution_poison_mode(db, target);
    if (mode == STAGE_REPAIR_POISON_NONE)
        return false;

    /* A repairable validate poison stays detected until H* advances. If the
     * correct repair header is already present, the remedy below returns SKIP
     * and lets validate_headers' non-destructive recheck flip the row forward;
     * keeping detect=true makes a stuck recheck page instead of going quiet. */

    atomic_store(&g_target_at_detect, target);
    atomic_store(&g_hstar_at_detect, reducer_frontier_height(db));
    atomic_store(&g_mode_at_detect, (int)mode);
    return true;
}

static enum condition_remedy_result remedy_stale_validate_headers_repair(void)
{
    sqlite3 *db = progress_store_db();
    int target = atomic_load(&g_target_at_detect);
    if (!db || target < 0)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);

    enum stage_repair_header_solution_poison mode =
        stage_repair_header_solution_poison_mode(db, target);

    if (validate_repairable_mode(mode)) {
        struct main_state *ms0 = condition_engine_main_state();
        struct block_index *bi0 =
            ms0 ? active_chain_at(&ms0->chain_active, target) : NULL;
        const struct uint256 *canon = bi0 ? bi0->phashBlock : NULL;
        bool solution_present = canon
            ? stage_repair_header_solution_available(db, target, canon)
            : (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS &&
               stage_repair_header_solution_available(db, target, NULL));

        /* Step 1 — backfill the CORRECT (canonical) solution from the oracle if
         * it is not already present. header_probe_pull_range re-validates the
         * fetched header and writes it hash-bound into header_solution_repair
         * (INSERT OR REPLACE by height — it OVERWRITES any stale wrong-block
         * row, which the hash-aware availability check above does not accept). */
        if (!solution_present) {
            int added = 0;
            struct zcl_result r = header_probe_pull_range(target, 128, &added);
            if (!r.ok) {
                LOG_WARN("condition",
                         "[condition:stale_validate_headers_repair] "
                         "header probe failed h=%d code=%d msg=%s",
                         target, r.code, r.message);
                return COND_REMEDY_FAILED;
            }
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "header probe h=%d added=%d",
                     target, added);
        }
        solution_present = canon
            ? stage_repair_header_solution_available(db, target, canon)
            : (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS &&
               stage_repair_header_solution_available(db, target, NULL));

        /* Step 2 — if the correct solution is now present, DEFER to the
         * non-destructive validate_headers recheck. Do NOT poison_rewind: the
         * recheck flips the ok=0 row forward (recheck floor pinned at the lowest
         * repairable height) while preserving all downstream
         * progress. A destructive rewind here would delete forward validate work
         * and re-starve the recheck (forcing a long forward re-drain that parks
         * the recheck) — the precise churn that produced the 5x-unwitnessed
         * poison_rewind → operator_needed loop. Return SKIP and let the witness
         * (H* advanced past the frontier) govern success; detect()
         * deactivates next tick and resets the attempt counter. */
        if (solution_present) {
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "solution present h=%d — deferring to non-destructive "
                     "validate_headers recheck (no poison_rewind)",
                     target);
            return COND_REMEDY_SKIP;
        }

        /* Solution still unavailable after a backfill attempt. A poison_rewind
         * cannot help a solutionless row (the forward re-drain would re-yield
         * ok=0), so do not rewind — fail this attempt and retry backfill next
         * tick (e.g. the oracle was briefly unreachable). */
        LOG_WARN("condition",
                 "[condition:stale_validate_headers_repair] "
                 "no durable repair header available h=%d", target);
        return COND_REMEDY_FAILED;
    }

    /* DOWNSTREAM_STALE: validate_headers is ok=1 but a body was skipped-invalid
     * at the frontier. There is no non-destructive heal for this, so the
     * sanctioned frontier poison_rewind is the correct tool. Its guards in
     * stage_repair_rewind.c are unchanged (frontier-only == H*+1;
     * refuses if any ok=1 success_checked row sits at/above the frontier; never
     * deletes tip_finalize_log), so the Tier-2 public-tip floor is preserved. */
    struct stage_repair_header_solution_result rr;
    int hstar = reducer_frontier_height(db);
    if (!stage_repair_header_solution_poison_rewind(db, target, hstar, &rr))
        return COND_REMEDY_FAILED;

    LOG_WARN("condition",
             "[condition:stale_validate_headers_repair] h=%d mode=%d "
             "deleted=%d rewound=%d",
             target, rr.mode, rr.deleted_rows, rr.rewound_cursors);
    return rr.repaired ? COND_REMEDY_OK : COND_REMEDY_SKIP;
}

static bool witness_stale_validate_headers_repair(int64_t target_at_detect)
{
    /* The engine passes a wall-clock TIMESTAMP here (condition.c stores
     * `now` into target_at_detect), NOT a height — ignore it and read our
     * own captured frontier height. */
    (void)target_at_detect;

    /* SOLE success predicate: the durable reducer frontier advanced PAST the
     * frontier captured at detect time. Anything less is NOT cleared.
     *
     * The old witness also returned true the instant the poison rows were
     * deleted or a repair header became available — but the destructive
     * rewind itself deletes those rows / writes that header WITHOUT moving
     * the tip, so it could self-certify "cleared" on every ~5s tick while
     * the tip stayed frozen (the Law-7 lie). Those shortcuts are gone:
     * reducer_frontier_compute_hstar reads the provable reducer prefix, which
     * the rewind cannot fake by leaving a higher served-tip anchor behind, so
     * a non-advancing remedy now leaves the witness false, accrues
     * attempts, trips max_attempts, and pages EV_OPERATOR_NEEDED. */
    int target = atomic_load(&g_target_at_detect);
    if (target < 0)
        return false;

    sqlite3 *db = progress_store_db();
    int hstar_at_detect = atomic_load(&g_hstar_at_detect);
    if (db && hstar_at_detect >= 0 && target <= hstar_at_detect) {
        return stage_repair_header_solution_poison_mode(db, target) ==
               STAGE_REPAIR_POISON_NONE;
    }
    return reducer_frontier_height(db) >= target;
}

static struct condition c_stale_validate_headers_repair = {
    .name = "stale_validate_headers_repair",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_stale_validate_headers_repair,
    .remedy = remedy_stale_validate_headers_repair,
    .witness = witness_stale_validate_headers_repair,
    .witness_window_secs = 60,
};

void register_stale_validate_headers_repair(void)
{
    (void)condition_register(&c_stale_validate_headers_repair);
}

#ifdef ZCL_TESTING
void stale_validate_headers_repair_test_reset(void)
{
    struct condition_state *s = &c_stale_validate_headers_repair.state;
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_hstar_at_detect, -1);
    atomic_store(&g_remedy_calls, 0);
    atomic_store(&g_mode_at_detect, STAGE_REPAIR_POISON_NONE);
#ifdef ZCL_TESTING
    atomic_store(&g_test_hstar_override, -1);
#endif
    condition_reset_state(&c_stale_validate_headers_repair);
    /* Zero last_remedy_unix so condition_due_for_remedy treats the next tick
     * as due (last==0 bypasses the wall-clock backoff). There is no
     * injectable clock; the escalation test re-zeros this between ticks to
     * drive successive remedy attempts within the same wall-second. */
    atomic_store(&s->last_remedy_unix, (int64_t)0);
    atomic_store(&s->last_operator_needed_unix, (int64_t)0);
}

/* Test-only: clear last_remedy_unix between ticks so the next remedy is due
 * despite backoff_secs (no injectable clock — see test_reset). */
void stale_validate_headers_repair_test_clear_backoff(void);
void stale_validate_headers_repair_test_clear_backoff(void)
{
    struct condition_state *s = &c_stale_validate_headers_repair.state;
    atomic_store(&s->last_remedy_unix, (int64_t)0);
}

int stale_validate_headers_repair_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}
#endif
