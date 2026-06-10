/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "jobs/stage_repair.h"
#include "services/header_probe.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>

static _Atomic int g_target_at_detect = -1;
static _Atomic int g_remedy_calls = 0;
static _Atomic int g_mode_at_detect = STAGE_REPAIR_POISON_NONE;

static int repair_target_height(void)
{
    struct main_state *ms = condition_engine_main_state();
    if (!ms)
        return -1; // raw-return-ok:engine-not-ready
    int tip = active_chain_height(&ms->chain_active);
    return tip >= 0 ? tip + 1 : -1;
}

static bool detect_stale_validate_headers_repair(void)
{
    sqlite3 *db = progress_store_db();
    int target = repair_target_height();
    if (!db || target < 0)
        return false;

    enum stage_repair_header_solution_poison mode =
        stage_repair_header_solution_poison_mode(db, target);
    if (mode == STAGE_REPAIR_POISON_NONE)
        return false;

    /* For a SOLUTIONLESS poison the symptom is "the frontier block's Equihash
     * solution is missing." Once the CORRECT solution is backfilled, the
     * non-destructive validate_headers recheck (header_from_repair_table +
     * recheck_failed_rows, 060a5cb4c) flips the ok=0 row forward — no
     * destructive rewind is needed. So this Condition must DEACTIVATE the moment
     * the correct solution is present, letting the recheck self-heal and the
     * attempt counter reset (condition.c resets on !detected once cleared). The
     * availability check is HASH-AWARE against the canonical block at `target`
     * so a stale wrong-block row does not masquerade as "present". A
     * DOWNSTREAM_STALE poison (validate ok=1 but a skipped-invalid body) has no
     * non-destructive heal and always fires. */
    if (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS) {
        struct main_state *ms = condition_engine_main_state();
        struct block_index *bi =
            ms ? active_chain_at(&ms->chain_active, target) : NULL;
        const struct uint256 *canon = bi ? bi->phashBlock : NULL;
        if (canon &&
            stage_repair_header_solution_available(db, target, canon))
            return false;
    }

    atomic_store(&g_target_at_detect, target);
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

    if (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS) {
        struct main_state *ms0 = condition_engine_main_state();
        struct block_index *bi0 =
            ms0 ? active_chain_at(&ms0->chain_active, target) : NULL;
        const struct uint256 *canon = bi0 ? bi0->phashBlock : NULL;

        /* Step 1 — backfill the CORRECT (canonical) solution from the oracle if
         * it is not already present. header_probe_pull_range re-validates the
         * fetched header and writes it hash-bound into header_solution_repair
         * (INSERT OR REPLACE by height — it OVERWRITES any stale wrong-block
         * row, which the hash-aware availability check above does not accept). */
        if (!stage_repair_header_solution_available(db, target, canon)) {
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

        /* Step 2 — if the correct solution is now present, DEFER to the
         * non-destructive validate_headers recheck. Do NOT poison_rewind: the
         * recheck flips the ok=0 row forward (recheck floor pinned at the lowest
         * repairable height, 060a5cb4c) while preserving all downstream
         * progress. A destructive rewind here would delete forward validate work
         * and re-starve the recheck (forcing a long forward re-drain that parks
         * the recheck) — the precise churn that produced the 5x-unwitnessed
         * poison_rewind → operator_needed loop. Return SKIP and let the witness
         * (durable tip advanced past the frontier) govern success; detect()
         * deactivates next tick and resets the attempt counter. */
        if (stage_repair_header_solution_available(db, target, canon)) {
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
     * stage_repair_rewind.c are unchanged (frontier-only == active_tip+1;
     * refuses if any ok=1 success_checked row sits at/above the frontier; never
     * deletes tip_finalize_log), so the Tier-2 public-tip floor is preserved. */
    struct stage_repair_header_solution_result rr;
    struct main_state *ms = condition_engine_main_state();
    int active_tip = ms ? active_chain_height(&ms->chain_active) : -2;
    if (!stage_repair_header_solution_poison_rewind(db, target,
                                                    active_tip, &rr))
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

    /* SOLE success predicate: the durable public tip advanced PAST the
     * frontier captured at detect time. Anything less is NOT cleared.
     *
     * The old witness also returned true the instant the poison rows were
     * deleted or a repair header became available — but the destructive
     * rewind itself deletes those rows / writes that header WITHOUT moving
     * the tip, so it could self-certify "cleared" on every ~5s tick while
     * the tip stayed frozen (the Law-7 lie). Those shortcuts are gone:
     * active_chain_height reads MAX(height) FROM tip_finalize_log WHERE
     * ok=1 (or the chain-authority shim), which the rewind never moves, so
     * a non-advancing remedy now leaves the witness false, accrues
     * attempts, trips max_attempts, and pages EV_OPERATOR_NEEDED. */
    int target = atomic_load(&g_target_at_detect);
    if (target < 0)
        return false;

    struct main_state *ms = condition_engine_main_state();
    if (!ms)
        return false;

    return active_chain_height(&ms->chain_active) >= target;
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
    atomic_store(&g_remedy_calls, 0);
    atomic_store(&g_mode_at_detect, STAGE_REPAIR_POISON_NONE);
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
