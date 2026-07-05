/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "jobs/reducer_frontier.h"
#include "jobs/block_header_emit.h"
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

/* LANE D / #3b — file-scope forward declaration; defined after the remedy.
 * Drops BLOCK_HAVE_DATA on the canonical block_index entry at `height` so the
 * normal P2P getheaders/getdata sync path re-downloads the canonical
 * header+body when the zclassicd oracle is unreachable (oracle-independent
 * always-terminating fallback). */
static void cure_request_peer_refetch(int height);

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
                cure_request_peer_refetch(target);
                LOG_WARN("condition",
                         "[condition:stale_validate_headers_repair] "
                         "header probe failed h=%d code=%d msg=%s — "
                         "requested P2P re-fetch, deferring",
                         target, r.code, r.message);
                return COND_REMEDY_SKIP;
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
         * tick. With the oracle DEAD this would never recover via RPC, so fall
         * back to a P2P re-fetch of the canonical bytes (LANE D / #3b) and SKIP
         * rather than accrue toward the operator page — an always-terminating
         * remedy given any honest peer. */
        cure_request_peer_refetch(target);
        LOG_WARN("condition",
                 "[condition:stale_validate_headers_repair] "
                 "no durable repair header h=%d via oracle — requested P2P "
                 "re-fetch, deferring (no operator page)", target);
        return COND_REMEDY_SKIP;
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

/* LANE D / #3b — drop BLOCK_HAVE_DATA on the canonical block_index entry at
 * `height` so the normal P2P getheaders/getdata sync path re-downloads the
 * canonical header+body (oracle-independent). Same discipline as
 * body_persist_stage.c:requeue_body_for_refetch: clear the bit, re-emit the
 * header event so the cleared state persists across restarts, and let the sync
 * scheduler re-request. No-op if the height/entry is unknown (nothing to
 * re-fetch — the witness still governs and the next tick retries). */
static void cure_request_peer_refetch(int height)
{
    if (height < 0)
        return; // raw-return-ok:nothing-to-refetch
    struct main_state *ms = condition_engine_main_state();
    if (!ms)
        return; // raw-return-ok:no-main-state
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (!bi || !bi->phashBlock)
        return; // raw-return-ok:height-unindexed
    if (!(bi->nStatus & BLOCK_HAVE_DATA))
        return; // raw-return-ok:already-cleared-refetch-pending
    bi->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
    block_index_emit_header_event(bi, "stale_validate_headers_repair",
                                  NULL, NULL);
    LOG_WARN("condition",
             "[condition:stale_validate_headers_repair] cleared HAVE_DATA "
             "h=%d — normal P2P sync will re-download canonical body", height);
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
    if (!db)
        return false;

    int hstar_at_detect = atomic_load(&g_hstar_at_detect);
    if (hstar_at_detect >= 0 && target <= hstar_at_detect) {
        return stage_repair_header_solution_poison_mode(db, target) ==
               STAGE_REPAIR_POISON_NONE;
    }

#ifdef ZCL_TESTING
    int ov = atomic_load(&g_test_hstar_override);
    if (ov >= 0)
        return ov >= target;
#endif

    progress_store_tx_lock();
    int32_t hstar_now = -1;
    int32_t served_floor = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar_now, &served_floor);
    progress_store_tx_unlock();
    return ok && hstar_now >= target;
}

static struct condition c_stale_validate_headers_repair = {
    .name = "stale_validate_headers_repair",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    /* Finite fast ladder: 5 un-witnessed remedies page a human once per
     * episode (the honest-witness escalation the W2 tests pin). */
    .max_attempts = 5,
    /* Continue-with-cooldown (sticky-node plan #7), routed for LANE D / #3b.
     * The repair frontier can be solutionless purely because an EXTERNAL
     * dependency is absent (the zclassicd oracle is unreachable / a peer is
     * forging the header page). In that case the remedy's oracle-independent
     * fallback (cure_request_peer_refetch + COND_REMEDY_SKIP) still accrues an
     * attempt — condition.c:321 increments attempts UNCONDITIONALLY regardless
     * of result, so SKIP does NOT avoid the ladder — and would otherwise trip
     * max_attempts and LATCH FOREVER at EV_OPERATOR_NEEDED (condition.c:259 +
     * :353). That is a human dead-end on a RECOVERABLE class. With
     * cooldown_secs > 0 the engine re-arms the remedy every 10 minutes after the
     * page, UNBOUNDED (cooldown_max_rearms = 0), so an oracle-absent /
     * forged-page stall keeps retrying the P2P re-fetch forever and can NEVER
     * permanently give up healing on a recoverable cause. The episode resets
     * (fresh ladder) the instant the fault identity (target_at_detect) moves or
     * detect() goes false; the single per-episode page still fires once at
     * max_attempts so a human is informed. */
    .cooldown_secs = 600,
    .cooldown_max_rearms = 0,
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
