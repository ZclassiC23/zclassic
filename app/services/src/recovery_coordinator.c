/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * recovery_coordinator — implementation. See services/recovery_coordinator.h.
 *
 * // one-result-type-ok:recovery-coordinator-orchestrator
 * //
 * // This is a supervisor-child ORCHESTRATOR over already-fallible healers, not
 * // a fallible executor of its own. Its public surfaces are the void
 * // register/setter, a single dump_state_json out-struct, and the rung
 * // selector which returns the typed rung + outcome (never a lost reason).
 * // Every rung dispatch logs + emits EV_RECOVERY_ACTION; the no-applicable-rung
 * // path names a typed blocker. No bare-bool lost-reason here.
 *
 * It calls the EXISTING healer entry points and never re-implements them:
 *   rung 1  stage_reconcile_clamp_tip_finalize_to_floor  (jobs/stage_repair.h)
 *   rung 2  stage_reducer_frontier_reconcile_light        (jobs/stage_repair.h)
 *   rung 3  segment_corruption_scan_one / _repair (conditions/segment_corruption.h)
 *   rung 4  blocker_set                                    (util/blocker.h) */

#include "services/recovery_coordinator.h"

#include "platform/time_compat.h"
#include "conditions/segment_corruption.h"
#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "services/sync_monitor.h"
#include "storage/chain_segment.h"
#include "storage/progress_store.h"
#include "supervisors/domains.h"
#include "validation/main_state.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/supervisor.h"
#include "event/event.h"
#include "json/json.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Module state ──────────────────────────────────────────────────── */

static struct main_state       *g_ms      = NULL;
static const char              *g_datadir = NULL;
static struct liveness_contract g_contract;
static _Atomic supervisor_child_id g_id = SUPERVISOR_INVALID_ID;

/* Recorded outcome of the most recent pass + lifetime counters (all atomic:
 * dumped off-thread while the supervisor tick drives). */
static _Atomic int      g_last_rung    = RECOVERY_RUNG_NONE;
static _Atomic int      g_last_outcome = RECOVERY_OUTCOME_NONE;
static _Atomic int64_t  g_last_run_unix = 0;
static _Atomic uint64_t g_runs          = 0;
static _Atomic uint64_t g_rung_fires[RECOVERY_RUNG_COUNT];

/* Pluggable rung attempts (rungs 1..3); NULL = default. */
static recovery_rung_fn g_rung_fn[RECOVERY_RUNG_COUNT];

/* Round-robin segment scan cursor for the self-driven tick. */
static uint32_t g_seg_scan_cursor = 0;

const char *recovery_rung_name(enum recovery_rung r)
{
    switch (r) {
    case RECOVERY_RUNG_NONE:                return "none";
    case RECOVERY_RUNG_CURSOR_WARM_RESTART: return "cursor_warm_restart";
    case RECOVERY_RUNG_REDERIVE_RANGE:      return "rederive_range";
    case RECOVERY_RUNG_SEGMENT_REFETCH:     return "segment_refetch";
    case RECOVERY_RUNG_BLOCKER:             return "blocker";
    case RECOVERY_RUNG_COUNT:               break;
    }
    return "unknown";
}

const char *recovery_outcome_name(enum recovery_outcome o)
{
    switch (o) {
    case RECOVERY_OUTCOME_NONE:        return "none";
    case RECOVERY_OUTCOME_RECOVERED:   return "recovered";
    case RECOVERY_OUTCOME_PROGRESSING: return "progressing";
    case RECOVERY_OUTCOME_NOOP:        return "noop";
    case RECOVERY_OUTCOME_BLOCKED:     return "blocked";
    }
    return "unknown";
}

static int64_t now_unix(void)
{
    return (int64_t)platform_time_wall_time_t();
}

/* ── Default rung attempts (call the real healers) ─────────────────── */

/* Rung 1 — journal replay / cursor warm restart. The clamp is idempotent and
 * no-op-safe: it reports clamped=false when the tip_finalize cursor already
 * sits at the durably-applied coins frontier, so we can use it as both detect
 * and remedy — a clamp that actually moved the cursor IS this rung firing. */
static bool rung1_cursor_warm_restart(struct recovery_ctx *ctx,
                                      enum recovery_outcome *outcome)
{
    if (!ctx->db || ctx->coins_best < 0)
        return false;
    struct stage_reconcile_result rr;
    memset(&rr, 0, sizeof(rr));
    if (!stage_reconcile_clamp_tip_finalize_to_floor(ctx->db, ctx->coins_best,
                                                     &rr))
        return false; /* store error — not this rung's class */
    if (!rr.clamped)
        return false; /* cursor already at floor — fall through */
    *outcome = RECOVERY_OUTCOME_PROGRESSING; /* durable; stages re-derive */
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=recovery-cursor-warm-restart floor=%d", rr.floor);
    LOG_INFO("recovery_coordinator",
             "[recovery_coordinator] rung1 cursor warm restart: clamped "
             "tip_finalize to coins frontier %d", rr.floor);
    return true;
}

/* Rung 2 — bounded range re-derive. reducer_frontier_reconcile_light rewinds
 * the suspect stage cursors to the lowest hole so the forward stages re-derive
 * the range from PoW-verified on-disk bodies; it is gated internally and only
 * acts when there is real repair evidence. */
static bool rung2_rederive_range(struct recovery_ctx *ctx,
                                 enum recovery_outcome *outcome)
{
    if (!ctx->db || !ctx->ms)
        return false;
    struct stage_reducer_frontier_reconcile_result rr;
    memset(&rr, 0, sizeof(rr));
    if (!stage_reducer_frontier_reconcile_light(ctx->db, ctx->ms, &rr))
        return false; /* raw-return-ok:reconcile-store-error-falls-through-to-next-rung */
    if (!rr.repaired &&
        !stage_reducer_frontier_result_has_gate_loudness_evidence(&rr))
        return false; /* nothing actionable in this range — fall through */
    *outcome = rr.repaired ? RECOVERY_OUTCOME_PROGRESSING
                           : RECOVERY_OUTCOME_NOOP;
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=recovery-rederive-range repaired=%d hstar=%d",
                (int)rr.repaired, rr.hstar);
    LOG_INFO("recovery_coordinator",
             "[recovery_coordinator] rung2 range re-derive: repaired=%d "
             "hstar=%d", (int)rr.repaired, rr.hstar);
    return true;
}

/* Rung 3 — segment refetch-by-hash. A bounded round-robin spot-verify finds a
 * corrupt sealed segment; the repair unlinks it + rebuilds the manifest so
 * reads fall back to blk*.dat and the sealer re-seals the range from disk. */
static bool rung3_segment_refetch(struct recovery_ctx *ctx,
                                  enum recovery_outcome *outcome)
{
    if (!ctx->segments_dir || !ctx->segments_dir[0])
        return false;
    uint32_t local_cursor = 0;
    uint32_t *cursor = ctx->segment_scan_cursor ? ctx->segment_scan_cursor
                                                : &local_cursor;
    uint32_t first = 0, count = 0;
    char err[256];
    enum cseg_status st = segment_corruption_scan_one(ctx->segments_dir, cursor,
                                                      &first, &count, err,
                                                      sizeof(err));
    if (st == CSEG_OK || st == CSEG_ERR_NOT_FOUND)
        return false; /* clean or empty — fall through */
    ctx->seg_first = first;
    ctx->seg_count = count;
    enum cseg_status rst = segment_corruption_repair(ctx->segments_dir, first,
                                                     count, err, sizeof(err));
    *outcome = rst == CSEG_OK ? RECOVERY_OUTCOME_RECOVERED
                              : RECOVERY_OUTCOME_NOOP;
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=recovery-segment-refetch first=%u count=%u repair=%d",
                first, count, (int)rst);
    LOG_WARN("recovery_coordinator",
             "[recovery_coordinator] rung3 segment refetch: corrupt seg-%u-%u "
             "detected (scan=%d), repair=%d", first, count, (int)st, (int)rst);
    return true;
}

/* Rung 4 — no applicable rung: name a typed blocker. A stall reached the
 * coordinator with no rung-1..3 class present; make it a named, escalatable
 * dependency rather than a silent cycle. */
static bool rung4_name_blocker(struct recovery_ctx *ctx,
                               enum recovery_outcome *outcome)
{
    (void)ctx;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "recovery_coordinator: inconsistency detected but no cheap rung "
             "applies (cursor clamp / range re-derive / segment refetch all "
             "declined) — deeper recovery or operator attention required");
    struct blocker_record b;
    if (blocker_init(&b, "recovery_coordinator.no_applicable_rung",
                     "recovery_coordinator", BLOCKER_DEPENDENCY, reason)) {
        b.retry_budget = -1;
        (void)blocker_set(&b);
    }
    *outcome = RECOVERY_OUTCOME_BLOCKED;
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=recovery-no-applicable-rung blocker=named");
    LOG_WARN("recovery_coordinator",
             "[recovery_coordinator] rung4: no cheap rung applies — named "
             "typed blocker recovery_coordinator.no_applicable_rung");
    return true;
}

static recovery_rung_fn rung_fn(enum recovery_rung r)
{
    recovery_rung_fn f = g_rung_fn[r];
    if (f)
        return f;
    switch (r) {
    case RECOVERY_RUNG_CURSOR_WARM_RESTART: return rung1_cursor_warm_restart;
    case RECOVERY_RUNG_REDERIVE_RANGE:      return rung2_rederive_range;
    case RECOVERY_RUNG_SEGMENT_REFETCH:     return rung3_segment_refetch;
    default:                                return NULL;
    }
}

/* ── Public selector ──────────────────────────────────────────────── */

enum recovery_rung recovery_coordinator_run(struct recovery_ctx *ctx,
                                            enum recovery_outcome *out_outcome)
{
    enum recovery_outcome outcome = RECOVERY_OUTCOME_NONE;
    enum recovery_rung fired = RECOVERY_RUNG_NONE;

    if (!ctx) {
        if (out_outcome)
            *out_outcome = RECOVERY_OUTCOME_NONE;
        return RECOVERY_RUNG_NONE; // raw-return-ok:null-ctx-nothing-to-do
    }

    /* Cheapest sufficient rung, in cost order. The first rung whose class is
     * present claims the pass. */
    for (enum recovery_rung r = RECOVERY_RUNG_CURSOR_WARM_RESTART;
         r <= RECOVERY_RUNG_SEGMENT_REFETCH; r++) {
        recovery_rung_fn f = rung_fn(r);
        if (f && f(ctx, &outcome)) {
            fired = r;
            break;
        }
    }
    if (fired == RECOVERY_RUNG_NONE) {
        (void)rung4_name_blocker(ctx, &outcome);
        fired = RECOVERY_RUNG_BLOCKER;
    }

    atomic_store(&g_last_rung, (int)fired);
    atomic_store(&g_last_outcome, (int)outcome);
    atomic_store(&g_last_run_unix, now_unix());
    atomic_fetch_add(&g_runs, 1u);
    atomic_fetch_add(&g_rung_fires[fired], 1u);

    if (out_outcome)
        *out_outcome = outcome;
    return fired;
}

/* ── Self-driven supervised tick ──────────────────────────────────── */

static void recovery_coordinator_drive(void)
{
    /* Only act on a genuinely detected inconsistency: an unresolved CRITICAL
     * condition. A healthy node never mutates anything here. */
    if (condition_engine_get_unresolved_critical_count() <= 0)
        return;

    struct main_state *ms = g_ms ? g_ms : sync_monitor_main_state();
    struct recovery_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.db = progress_store_db();
    ctx.ms = ms;
    ctx.coins_best = -1;
    ctx.segment_scan_cursor = &g_seg_scan_cursor;

    int32_t coins_best = -1;
    if (ctx.db && reducer_frontier_derive_coins_best_now(&coins_best, NULL, NULL))
        ctx.coins_best = (int)coins_best;

    char segdir[512];
    if (g_datadir && g_datadir[0]) {
        snprintf(segdir, sizeof(segdir), "%s/segments", g_datadir);
        ctx.segments_dir = segdir;
    }

    (void)recovery_coordinator_run(&ctx, NULL);
}

static void recovery_coordinator_tick(struct liveness_contract *c)
{
    (void)c;
    recovery_coordinator_drive();
    supervisor_progress(atomic_load(&g_id),
                        (int64_t)atomic_load(&g_runs));
}

void recovery_coordinator_set_datadir(const char *datadir)
{
    g_datadir = datadir; /* process-lifetime string from boot ctx */
}

void recovery_coordinator_register(struct main_state *ms)
{
    if (!ms) return;
    if (atomic_load(&g_id) != SUPERVISOR_INVALID_ID) return; /* idempotent */
    g_ms = ms;
    liveness_contract_init(&g_contract, "chain.recovery_coordinator");
    atomic_store(&g_contract.period_secs, (int64_t)30);
    atomic_store(&g_contract.deadline_secs, (int64_t)0);
    atomic_store(&g_contract.progress_max_quiet_us, (int64_t)0);
    g_contract.on_tick  = recovery_coordinator_tick;
    g_contract.on_stall = NULL;
    supervisor_domains_init();
    atomic_store(&g_id, supervisor_register_in_domain(g_chain_sup, &g_contract));
    if (atomic_load(&g_id) == SUPERVISOR_INVALID_ID)
        LOG_WARN("recovery_coordinator",
                 "[recovery_coordinator] WARN register failed");
}

bool recovery_coordinator_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    enum recovery_rung last = (enum recovery_rung)atomic_load(&g_last_rung);
    enum recovery_outcome lo = (enum recovery_outcome)atomic_load(&g_last_outcome);
    json_push_kv_bool(out, "registered",
                      atomic_load(&g_id) != SUPERVISOR_INVALID_ID);
    json_push_kv_str(out, "last_rung", recovery_rung_name(last));
    json_push_kv_int(out, "last_rung_index", (int64_t)last);
    json_push_kv_str(out, "last_outcome", recovery_outcome_name(lo));
    json_push_kv_int(out, "last_run_unix", atomic_load(&g_last_run_unix));
    json_push_kv_int(out, "runs", (int64_t)atomic_load(&g_runs));
    json_push_kv_int(out, "rung_count", (int64_t)RECOVERY_RUNG_COUNT);

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (int i = RECOVERY_RUNG_CURSOR_WARM_RESTART; i < RECOVERY_RUNG_COUNT; i++) {
        struct json_value o;
        json_init(&o);
        json_set_object(&o);
        json_push_kv_str(&o, "rung", recovery_rung_name((enum recovery_rung)i));
        json_push_kv_int(&o, "fires", (int64_t)atomic_load(&g_rung_fires[i]));
        json_push_kv_bool(&o, "pluggable_fn_set",
                          i < RECOVERY_RUNG_COUNT && g_rung_fn[i] != NULL);
        json_push_back(&arr, &o);
        json_free(&o);
    }
    json_push_kv(out, "rungs", &arr);
    json_free(&arr);
    return true;
}

#ifdef ZCL_TESTING
void recovery_coordinator_test_reset(void)
{
    atomic_store(&g_last_rung, RECOVERY_RUNG_NONE);
    atomic_store(&g_last_outcome, RECOVERY_OUTCOME_NONE);
    atomic_store(&g_last_run_unix, 0);
    atomic_store(&g_runs, 0u);
    g_seg_scan_cursor = 0;
    for (int i = 0; i < RECOVERY_RUNG_COUNT; i++) {
        atomic_store(&g_rung_fires[i], 0u);
        g_rung_fn[i] = NULL;
    }
}

void recovery_coordinator_test_set_rung_fn(enum recovery_rung r,
                                           recovery_rung_fn fn)
{
    if ((int)r < 0 || r >= RECOVERY_RUNG_COUNT)
        return;
    g_rung_fn[r] = fn;
}

enum recovery_rung recovery_coordinator_test_last_rung(void)
{
    return (enum recovery_rung)atomic_load(&g_last_rung);
}

enum recovery_outcome recovery_coordinator_test_last_outcome(void)
{
    return (enum recovery_outcome)atomic_load(&g_last_outcome);
}
#endif
