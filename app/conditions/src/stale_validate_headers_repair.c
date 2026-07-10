/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "jobs/reducer_frontier.h"
#include "jobs/block_header_emit.h"
#include "jobs/stage_repair.h"
#include "services/header_probe.h"
#include "services/sync_monitor.h"
#include "net/connman.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>

static _Atomic int g_target_at_detect = -1;
static _Atomic int g_hstar_at_detect = -1;
static _Atomic int g_remedy_calls = 0;
static _Atomic int g_mode_at_detect = STAGE_REPAIR_POISON_NONE;

/* Detective A2 — which source (oracle vs P2P) the in-flight repair attempt last
 * fired, so the completion tick can attribute the served repair. The oracle
 * pull is synchronous (attributed inline), the P2P getdata is async (its
 * solution appears on a later tick and is attributed there). Reset when the
 * episode target changes (detect) or on test reset. */
static _Atomic int g_repair_pending_source = HEADER_PROBE_SRC_NONE;

/* Typed blocker id: names the missing input when NEITHER the oracle NOR any
 * peer can serve the header-solution repair. TRANSIENT class — the Condition's
 * unbounded cooldown re-arm (cooldown_secs>0, cooldown_max_rearms=0) governs
 * retry, so this never latches into a human dead-end on a recoverable cause. */
#define STALE_HEADER_NO_SOURCE_BLOCKER_ID "header_repair_no_source"

#ifdef ZCL_TESTING
/* Test-only override of the connected-peer count seen by the P2P fallback, so
 * a hermetic fixture can exercise both the peers-available and the
 * missing-input (0 peers) branches without wiring the net stack. -1 = use the
 * real connman via sync_monitor_connman(). */
static _Atomic int g_test_peer_count_override = -1;
void stale_validate_headers_repair_test_set_peer_count(int n);
void stale_validate_headers_repair_test_set_peer_count(int n)
{
    atomic_store(&g_test_peer_count_override, n);
}
#endif

/* LANE D / #3b + Detective A2 — file-scope forward declaration; defined after
 * the remedy. Oracle-independent P2P re-fetch of the canonical block at
 * `height`: clears BLOCK_HAVE_DATA on the canonical block_index entry AND
 * actively enqueues a getdata via the existing download-manager machinery.
 * Returns the count of connected peers available to serve the re-fetch (0 =>
 * missing input), or -1 if the height is unindexed (nothing to re-fetch). */
static int cure_request_peer_refetch(int height);

static void raise_stale_header_no_source_blocker(int height)
{
    struct blocker_record r;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "header-solution repair h=%d: zclassicd oracle unreachable and "
             "no connected peer can serve a P2P getdata re-fetch", height);
    if (!blocker_init(&r, STALE_HEADER_NO_SOURCE_BLOCKER_ID, "header_probe",
                      BLOCKER_TRANSIENT, reason))
        return; // raw-return-ok:blocker-init-failed-already-logged
    (void)blocker_set(&r);
}

static void clear_stale_header_no_source_blocker(void)
{
    blocker_clear(STALE_HEADER_NO_SOURCE_BLOCKER_ID);
}

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

    /* New episode (target moved) → forget any stale source attribution so the
     * next served solution is credited to the source THIS episode fires. */
    if (atomic_exchange(&g_target_at_detect, target) != target)
        atomic_store(&g_repair_pending_source, HEADER_PROBE_SRC_NONE);
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

        /* Already repaired (e.g. an async P2P getdata re-fetch fired on an
         * earlier tick has now delivered + saved the canonical solution).
         * Attribute the served repair to whichever source we last fired, then
         * DEFER to the non-destructive validate_headers recheck (see the long
         * rationale at the bottom of this block — no poison_rewind here). */
        if (solution_present) {
            int pending = atomic_exchange(&g_repair_pending_source,
                                          HEADER_PROBE_SRC_NONE);
            if (pending != HEADER_PROBE_SRC_NONE)
                header_probe_note_repair_served(
                    (enum header_probe_repair_source)pending, target);
            clear_stale_header_no_source_blocker();
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "solution present h=%d source=%s — deferring to "
                     "non-destructive validate_headers recheck (no "
                     "poison_rewind)",
                     target, header_probe_repair_source_name(
                         (enum header_probe_repair_source)pending));
            return COND_REMEDY_SKIP;
        }

        /* Step 1 — ORACLE FIRST (cheap, local). header_probe_pull_range
         * re-validates the fetched header and writes it hash-bound into
         * header_solution_repair (INSERT OR REPLACE by height — it OVERWRITES
         * any stale wrong-block row, which the hash-aware availability check
         * above does not accept). */
        struct zcl_result r = header_probe_pull_range(target, 128, NULL);
        if (r.ok) {
            solution_present = canon
                ? stage_repair_header_solution_available(db, target, canon)
                : (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS &&
                   stage_repair_header_solution_available(db, target, NULL));
            if (solution_present) {
                /* Oracle served it synchronously — attribute inline. */
                atomic_store(&g_repair_pending_source, HEADER_PROBE_SRC_NONE);
                header_probe_note_repair_served(HEADER_PROBE_SRC_ORACLE, target);
                clear_stale_header_no_source_blocker();
                LOG_WARN("condition",
                         "[condition:stale_validate_headers_repair] "
                         "solution present h=%d source=oracle — deferring to "
                         "non-destructive validate_headers recheck", target);
                return COND_REMEDY_SKIP;
            }
            /* Oracle reachable but did not supply the canonical solution
             * (remote behind / missing the row) — fall through to P2P. */
        } else {
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "header probe (oracle) failed h=%d code=%d msg=%s — "
                     "falling back to P2P", target, r.code, r.message);
        }

        /* Step 2 — P2P FALLBACK (oracle-independent, the zclassicd oracle is
         * being retired). Actively re-request the canonical block for `target`
         * from connected peers via the EXISTING getdata machinery. The arriving
         * block is re-validated by check_block (PoW/Equihash) on ingest and its
         * solution is saved hash-bound into header_solution_repair
         * (reducer_cache_ingested_solution); validate_headers then INDEPENDENTLY
         * re-verifies the replacement before H* can advance — the detective
         * never swaps a page for an unverified one. The solution arrives async,
         * so this tick only fires the request and defers; the "solution
         * present" branch above attributes + completes on a later tick. */
        int peers = cure_request_peer_refetch(target);
        atomic_store(&g_repair_pending_source, HEADER_PROBE_SRC_P2P);
        header_probe_note_p2p_request(target, peers);
        if (peers <= 0) {
            /* Missing input: no oracle, no peer can serve the repair right now.
             * Name it with a typed blocker and SKIP. condition.c increments
             * attempts unconditionally, but cooldown_secs>0 +
             * cooldown_max_rearms==0 re-arm the remedy forever, so this is an
             * always-terminating remedy on a recoverable cause — never a latch
             * to EV_OPERATOR_NEEDED. */
            raise_stale_header_no_source_blocker(target);
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "no durable repair header h=%d via oracle AND no peers "
                     "(peers=%d) — named blocker %s, deferring (cooldown "
                     "re-arms, no operator page)", target, peers,
                     STALE_HEADER_NO_SOURCE_BLOCKER_ID);
        } else {
            clear_stale_header_no_source_blocker();
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "no durable repair header h=%d via oracle — requested P2P "
                     "getdata re-fetch (peers=%d), deferring (no operator page)",
                     target, peers);
        }
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

/* LANE D / #3b + Detective A2 — oracle-independent P2P re-fetch of the
 * canonical block at `height`. Two coordinated steps, both through EXISTING
 * machinery (Law: one way in — no second header/body fetch stack):
 *   1. Drop BLOCK_HAVE_DATA on the canonical block_index entry and re-emit the
 *      header event, so the cleared re-fetch state persists across restarts
 *      (same discipline as body_persist_stage.c:requeue_body_for_refetch).
 *   2. ACTIVELY enqueue a getdata for that exact block via
 *      sync_monitor_queue_active_frontier_body → dl_queue_priority → the
 *      download-manager getdata loop, rather than passively waiting for a
 *      background scan to notice the cleared bit.
 * Returns the count of connected peers available to serve the re-fetch (0 =>
 * missing input; the caller names a typed blocker), or -1 if the height is
 * unindexed (nothing to re-fetch — the witness still governs and the next tick
 * retries). */
static int cure_request_peer_refetch(int height)
{
    if (height < 0)
        return -1; // raw-return-ok:nothing-to-refetch
    struct main_state *ms = condition_engine_main_state();
    if (!ms)
        return -1; // raw-return-ok:no-main-state
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (!bi || !bi->phashBlock)
        return -1; // raw-return-ok:height-unindexed

    if (bi->nStatus & BLOCK_HAVE_DATA) {
        bi->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
        block_index_emit_header_event(bi, "stale_validate_headers_repair",
                                      NULL, NULL);
        LOG_WARN("condition",
                 "[condition:stale_validate_headers_repair] cleared HAVE_DATA "
                 "h=%d — P2P getdata will re-download canonical body", height);
    }

    /* Active getdata via the existing sync machinery. A non-ok result just
     * means the sync context is not wired yet (e.g. an isolated fixture, or
     * pre-context boot) — not an error; the next tick retries. */
    struct zcl_result qr =
        sync_monitor_queue_active_frontier_body(height, "header_repair_p2p");
    if (!qr.ok)
        LOG_WARN("condition",
                 "[condition:stale_validate_headers_repair] P2P body queue "
                 "h=%d not accepted (code=%d msg=%s) — sync context may be "
                 "unset; retrying next tick", height, qr.code, qr.message);

    /* Connected-peer count = whether P2P can serve the re-fetch at all. */
    int peers = 0;
    struct connman *cm = sync_monitor_connman();
    if (cm)
        peers = (int)connman_get_node_count(cm);
#ifdef ZCL_TESTING
    int ov = atomic_load(&g_test_peer_count_override);
    if (ov >= 0)
        peers = ov;
#endif
    return peers;
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
    atomic_store(&g_repair_pending_source, HEADER_PROBE_SRC_NONE);
#ifdef ZCL_TESTING
    atomic_store(&g_test_hstar_override, -1);
    atomic_store(&g_test_peer_count_override, -1);
#endif
    clear_stale_header_no_source_blocker();
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
