/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sticky escalator — implementation. See services/sticky_escalator.h. */

// one-result-type-ok:escalator-no-fallible-surface
//
// This is a supervisor-child meta-condition, not a fallible service executor.
// Its public surfaces are void drivers/setters, a single coherent dump_state_json
// out-struct, and bool PREDICATES (armed / test accessors) — none strips a
// failure reason. Every rung dispatch logs + emits EV_RECOVERY_ACTION; the
// non-latching page emits EV_OPERATOR_NEEDED. No bare-bool lost-reason here.

#include "platform/time_compat.h"
#include "services/sticky_escalator.h"

#include "supervisors/domains.h"
#include "framework/condition.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "jobs/reducer_frontier.h"
#include "storage/boot_auto_reindex.h"
#include "util/supervisor.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "event/event.h"
#include "json/json.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* ── Module state ──────────────────────────────────────────────────── */

static struct main_state       *g_ms      = NULL;
static const char              *g_datadir = NULL;
static struct liveness_contract g_contract;
static _Atomic supervisor_child_id g_id = SUPERVISOR_INVALID_ID;

/* Ladder state. All atomic — note_stall runs off-thread (watchdog /
 * worker_on_stall) while drive() runs on the self-heal + supervisor ticks. */
static _Atomic bool    g_armed            = false;
static _Atomic int     g_rung             = STICKY_RUNG_RETRY;
static _Atomic int64_t g_tip_at_rung      = -1;   /* H* observed when rung entered */
static _Atomic int64_t g_rung_entered_unix = 0;
static _Atomic int64_t g_last_page_unix   = 0;
static _Atomic int64_t g_rearm_until_unix = 0;
static _Atomic uint64_t g_rung_dispatches[STICKY_RUNG_COUNT];
static _Atomic uint64_t g_fires_operator_needed = 0;
static _Atomic uint64_t g_episodes_cleared = 0;
static _Atomic uint64_t g_ladder_cycles    = 0;

/* Pluggable rung actions; NULL = default stub (advance). */
static sticky_rung_fn g_rung_fn[STICKY_RUNG_COUNT];

/* Per-rung witness windows (seconds): the slow re-derivation rungs get a long
 * window so a legitimately multi-minute reindex/refold is not prematurely
 * advanced. */
static const int g_rung_window_secs[STICKY_RUNG_COUNT] = {
    [STICKY_RUNG_RETRY]            = 30,
    [STICKY_RUNG_TARGETED_REDERIVE]= 60,
    [STICKY_RUNG_RESNAPSHOT]       = 180,
    [STICKY_RUNG_REINDEX]          = 1200,
    [STICKY_RUNG_SELF_MINT_REFOLD] = 1800,
    [STICKY_RUNG_WIDEN_PEERS]      = 120,
    [STICKY_RUNG_REBOOTSTRAP]      = 3600,
};

const char *sticky_rung_name(enum sticky_rung r)
{
    switch (r) {
    case STICKY_RUNG_RETRY:             return "retry";
    case STICKY_RUNG_TARGETED_REDERIVE: return "targeted_rederive";
    case STICKY_RUNG_RESNAPSHOT:        return "resnapshot";
    case STICKY_RUNG_REINDEX:           return "reindex";
    case STICKY_RUNG_SELF_MINT_REFOLD:  return "self_mint_refold";
    case STICKY_RUNG_WIDEN_PEERS:       return "widen_peers";
    case STICKY_RUNG_REBOOTSTRAP:       return "rebootstrap";
    case STICKY_RUNG_COUNT:             break;
    }
    return "unknown";
}

static int64_t now_unix(void)
{
    return (int64_t)platform_time_wall_time_t();
}

/* Provable-tip first (sovereign H*), active-chain tip as fallback. */
static int64_t observe_tip(void)
{
    int32_t hstar = reducer_frontier_provable_tip_cached();
    if (hstar > 0)
        return (int64_t)hstar;
    if (g_ms)
        return (int64_t)active_chain_height(&g_ms->chain_active);
    return -1; // raw-return-ok:no-tip-observable-sentinel-not-an-error
}

/* ── Default rung stubs ────────────────────────────────────────────────
 *
 * Rungs 0/1/3 have real in-process surfaces and run them. Rungs 2/4/5/6 are
 * stubs that emit a typed EV_RECOVERY_ACTION (another lane's worker consumes it)
 * and report NOT_IMPLEMENTED so the ladder advances. A lane can plug a real
 * action via sticky_escalator_register_rung() with no edit here. */

static enum sticky_rung_result rung_retry_default(void)
{
    /* Cheapest: fire any due blocker escape_actions + let the condition engine
     * re-attempt. Wires the previously-dead blocker_supervisor_sweep() edge. */
    int dispatched = blocker_supervisor_sweep();
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=sticky-retry blocker_escapes=%d unresolved=%d",
                dispatched, condition_engine_get_unresolved_count());
    return STICKY_RUNG_PROGRESSING; /* hold one window to let conditions run */
}

static enum sticky_rung_result rung_targeted_rederive_default(void)
{
    /* Ask the S8 re-derivation lane to re-pull + re-run the suspect stage.
     * (The general hash-mismatch condition / Track A consumes this action.) */
    (void)blocker_supervisor_sweep();
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=sticky-targeted-rederive unresolved=%d",
                condition_engine_get_unresolved_count());
    return STICKY_RUNG_FAILED; /* no in-process curative surface yet -> advance */
}

static enum sticky_rung_result rung_resnapshot_default(void)
{
    event_emitf(EV_RECOVERY_ACTION, 0, "action=sticky-resnapshot-request");
    return STICKY_RUNG_NOT_IMPLEMENTED;
}

static enum sticky_rung_result rung_reindex_default(void)
{
    /* Durable, bounded-per-episode crash-only reindex: re-derive the UTXO set
     * from blocks/ on the NEXT boot (full crypto pipeline on replay). The
     * primitive itself caps attempts per anchor episode and marks terminal —
     * but the LADDER does not give up: a terminal reindex simply advances to the
     * deeper rungs. */
    if (!g_datadir) {
        event_emitf(EV_RECOVERY_ACTION, 0,
                    "action=sticky-reindex-skip reason=no_datadir");
        return STICKY_RUNG_NOT_IMPLEMENTED;
    }
    int64_t tip = observe_tip();
    int rc = boot_auto_reindex_request(g_datadir,
                                       (int32_t)(tip > 0 ? tip : 0));
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=sticky-reindex-request anchor=%lld rc=%d",
                (long long)tip, rc);
    if (rc == BOOT_AUTO_REINDEX_TERMINAL)
        return STICKY_RUNG_FAILED;   /* budget spent here -> go deeper */
    /* Cap the RUNTIME request budget at the same bound boot enforces. Without
     * this, apply_drive re-dispatches this rung every supervisor tick and the
     * on-disk count climbs unbounded (observed live: 4002) — a durable-write
     * storm that never self-terminates because the node, mid-stall, never
     * reboots to let boot_crashonly mark the budget terminal. Persist the
     * exhausted state now and go deeper, exactly as the boot path would. */
    if (rc >= BOOT_AUTO_REINDEX_MAX) {
        (void)boot_auto_reindex_mark_terminal(g_datadir,
                                              (int32_t)(tip > 0 ? tip : 0));
        return STICKY_RUNG_FAILED;
    }
    return STICKY_RUNG_PROGRESSING;  /* armed for next boot; hold a window */
}

static enum sticky_rung_result rung_self_mint_refold_default(void)
{
    event_emitf(EV_RECOVERY_ACTION, 0, "action=sticky-self-mint-refold-request");
    return STICKY_RUNG_NOT_IMPLEMENTED;
}

static enum sticky_rung_result rung_widen_peers_default(void)
{
    event_emitf(EV_RECOVERY_ACTION, 0, "action=sticky-widen-peer-discovery");
    return STICKY_RUNG_NOT_IMPLEMENTED;
}

static enum sticky_rung_result rung_rebootstrap_default(void)
{
    event_emitf(EV_RECOVERY_ACTION, 0, "action=sticky-rebootstrap-from-genesis");
    return STICKY_RUNG_NOT_IMPLEMENTED;
}

static const sticky_rung_fn g_rung_default[STICKY_RUNG_COUNT] = {
    [STICKY_RUNG_RETRY]            = rung_retry_default,
    [STICKY_RUNG_TARGETED_REDERIVE]= rung_targeted_rederive_default,
    [STICKY_RUNG_RESNAPSHOT]       = rung_resnapshot_default,
    [STICKY_RUNG_REINDEX]          = rung_reindex_default,
    [STICKY_RUNG_SELF_MINT_REFOLD] = rung_self_mint_refold_default,
    [STICKY_RUNG_WIDEN_PEERS]      = rung_widen_peers_default,
    [STICKY_RUNG_REBOOTSTRAP]      = rung_rebootstrap_default,
};

static sticky_rung_fn rung_action(enum sticky_rung r)
{
    sticky_rung_fn f = g_rung_fn[r];
    return f ? f : g_rung_default[r];
}

/* ── Ladder transitions ────────────────────────────────────────────── */

static void enter_rung(enum sticky_rung r, int64_t now)
{
    atomic_store(&g_rung, (int)r);
    atomic_store(&g_tip_at_rung, observe_tip());
    atomic_store(&g_rung_entered_unix, now);
}

static void clear_episode(int64_t now)
{
    (void)now;
    atomic_store(&g_armed, false);
    atomic_store(&g_rung, STICKY_RUNG_RETRY);
    atomic_store(&g_tip_at_rung, -1);
    atomic_fetch_add(&g_episodes_cleared, 1u);
    /* Release any operator_needed latch this episode raised: the tip
     * progressed, so the symptom genuinely cleared. note_cycling_page() emits
     * a (terminal=0) page that — depending on the alerts policy — may still
     * latch the health surface; this is the matching clear so recovery is
     * fully automatic and the node returns to healthy with no human action. */
    event_emitf(EV_CONDITION_CLEARED, 0,
                "condition=sticky_ladder_cycling reason=tip_progressed");
    LOG_INFO("sticky_escalator",
             "[sticky_escalator] episode cleared: tip progressed, ladder reset");
}

/* Non-latching page: a periodic "ladder is cycling, attention welcome" notice.
 * Rate-limited; NEVER a give-up. Per plan R4: keep the page, drop the latch. */
static void note_cycling_page(int64_t now, enum sticky_rung deepest)
{
    int64_t last = atomic_load(&g_last_page_unix);
    if (last != 0 && now - last < STICKY_PAGE_MIN_INTERVAL_SECS)
        return;
    atomic_store(&g_last_page_unix, now);
    atomic_fetch_add(&g_fires_operator_needed, 1u);
    LOG_WARN("sticky_escalator",
             "[sticky_escalator] ladder cycled through the deepest rung (%s) "
             "without clearing; RE-ARMING and continuing (NOT giving up) — "
             "operator attention welcome but not required",
             sticky_rung_name(deepest));
    /* Non-latching: emitted as a recurring notice, NOT a terminal state. The
     * health surface may show "recovering, attention welcome"; the ladder keeps
     * driving on its own. */
    event_emitf(EV_OPERATOR_NEEDED, 0,
                "condition=sticky_ladder_cycling rung=%s cycles=%llu "
                "terminal=0 recoverable=1",
                sticky_rung_name(deepest),
                (unsigned long long)atomic_load(&g_ladder_cycles));
}

/* The core step, shared by drive() and the test seam. `now` is wall-unix;
 * `tip` is the observed provable tip (injected in tests). */
static void apply_drive(int64_t tip, int64_t now)
{
    if (!atomic_load(&g_armed)) {
        /* Auto-arm if the condition engine has an unresolved CRITICAL backlog,
         * even without an explicit note_stall (belt + suspenders). */
        if (condition_engine_get_unresolved_count() > 0) {
            atomic_store(&g_armed, true);
            enter_rung(STICKY_RUNG_RETRY, now);
        } else {
            return;
        }
    }

    /* Honour the post-deepest-rung cooldown before re-driving. */
    int64_t rearm = atomic_load(&g_rearm_until_unix);
    if (rearm != 0) {
        if (now < rearm)
            return;
        atomic_store(&g_rearm_until_unix, 0);
        enter_rung(STICKY_RUNG_RETRY, now);
    }

    /* Progress check: any climb past the entry tip clears the whole episode. */
    int64_t entry = atomic_load(&g_tip_at_rung);
    if (entry >= 0 && tip >= entry + STICKY_PROGRESS_MARGIN) {
        clear_episode(now);
        return;
    }

    enum sticky_rung cur = (enum sticky_rung)atomic_load(&g_rung);
    int64_t entered = atomic_load(&g_rung_entered_unix);
    int window = g_rung_window_secs[cur];

    /* Dispatch the current rung action (idempotent; each kicks durable work). */
    enum sticky_rung_result res = rung_action(cur)();
    atomic_fetch_add(&g_rung_dispatches[cur], 1u);

    if (res == STICKY_RUNG_RESOLVED) {
        clear_episode(now);
        return;
    }
    if (res == STICKY_RUNG_PROGRESSING) {
        /* Hold this rung until its witness window lapses without tip progress. */
        if (now - entered < window)
            return;
        /* Window lapsed with no progress — fall through to advance. */
    }

    /* Advance (FAILED / NOT_IMPLEMENTED, or PROGRESSING-window-lapsed). */
    if (cur + 1 < STICKY_RUNG_COUNT) {
        LOG_WARN("sticky_escalator",
                 "[sticky_escalator] rung '%s' made no progress (res=%d) — "
                 "advancing to '%s'",
                 sticky_rung_name(cur), (int)res,
                 sticky_rung_name((enum sticky_rung)(cur + 1)));
        enter_rung((enum sticky_rung)(cur + 1), now);
        return;
    }

    /* Deepest rung exhausted WITHOUT clearing. DO NOT give up: re-arm to rung 0
     * after a cooldown and keep cycling. Emit a non-latching cycling page. */
    atomic_fetch_add(&g_ladder_cycles, 1u);
    note_cycling_page(now, cur);
    atomic_store(&g_rearm_until_unix, now + STICKY_REARM_COOLDOWN_SECS);
}

/* ── Public API ────────────────────────────────────────────────────── */

void sticky_escalator_set_datadir(const char *datadir)
{
    g_datadir = datadir; /* process-lifetime string from boot ctx */
}

void sticky_escalator_register_rung(enum sticky_rung rung, sticky_rung_fn fn)
{
    if ((int)rung < 0 || rung >= STICKY_RUNG_COUNT)
        return;
    g_rung_fn[rung] = fn;
}

void sticky_escalator_note_stall(const char *cause)
{
    int64_t now = now_unix();
    bool was = atomic_exchange(&g_armed, true);
    if (!was) {
        enter_rung(STICKY_RUNG_RETRY, now);
        atomic_store(&g_rearm_until_unix, 0);
        LOG_WARN("sticky_escalator",
                 "[sticky_escalator] ARMED by stall cause=%s — entering "
                 "always-terminating remedy ladder at rung 'retry'",
                 cause ? cause : "(unknown)");
        event_emitf(EV_RECOVERY_ACTION, 0,
                    "action=sticky-ladder-armed cause=%s",
                    cause ? cause : "unknown");
    }
}

void sticky_escalator_drive(void)
{
    if (!g_ms) return;
    apply_drive(observe_tip(), now_unix());
}

static void sticky_escalator_tick(struct liveness_contract *c)
{
    (void)c;
    sticky_escalator_drive();
    supervisor_progress(atomic_load(&g_id),
                        (int64_t)atomic_load(&g_rung));
}

void sticky_escalator_register(struct main_state *ms)
{
    if (!ms) return;
    if (atomic_load(&g_id) != SUPERVISOR_INVALID_ID) return; /* idempotent */
    g_ms = ms;
    liveness_contract_init(&g_contract, "op.sticky_escalator");
    atomic_store(&g_contract.period_secs, (int64_t)30);
    atomic_store(&g_contract.deadline_secs, (int64_t)0);
    atomic_store(&g_contract.progress_max_quiet_us, (int64_t)0);
    g_contract.on_tick  = sticky_escalator_tick;
    g_contract.on_stall = NULL;
    supervisor_domains_init();
    atomic_store(&g_id, supervisor_register_in_domain(g_op_sup, &g_contract));
    if (atomic_load(&g_id) == SUPERVISOR_INVALID_ID)
        LOG_WARN("sticky_escalator", "[sticky_escalator] WARN register failed");
}

bool sticky_escalator_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    int64_t now = now_unix();
    enum sticky_rung cur = (enum sticky_rung)atomic_load(&g_rung);
    json_push_kv_bool(out, "registered",
                      atomic_load(&g_id) != SUPERVISOR_INVALID_ID);
    json_push_kv_bool(out, "armed", atomic_load(&g_armed));
    json_push_kv_str(out, "current_rung", sticky_rung_name(cur));
    json_push_kv_int(out, "current_rung_index", (int64_t)cur);
    json_push_kv_int(out, "rung_count", (int64_t)STICKY_RUNG_COUNT);
    json_push_kv_int(out, "tip_at_rung", atomic_load(&g_tip_at_rung));
    json_push_kv_int(out, "observed_tip", observe_tip());
    int64_t entered = atomic_load(&g_rung_entered_unix);
    json_push_kv_int(out, "rung_age_secs",
                     entered ? (now - entered) : 0);
    json_push_kv_int(out, "rung_window_secs", (int64_t)g_rung_window_secs[cur]);
    int64_t rearm = atomic_load(&g_rearm_until_unix);
    json_push_kv_int(out, "rearm_in_secs",
                     (rearm && rearm > now) ? (rearm - now) : 0);
    json_push_kv_int(out, "ladder_cycles",
                     (int64_t)atomic_load(&g_ladder_cycles));
    json_push_kv_int(out, "episodes_cleared",
                     (int64_t)atomic_load(&g_episodes_cleared));
    json_push_kv_int(out, "fires_operator_needed_nonlatching",
                     (int64_t)atomic_load(&g_fires_operator_needed));
    json_push_kv_int(out, "unresolved_conditions",
                     condition_engine_get_unresolved_count());

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (int i = 0; i < STICKY_RUNG_COUNT; i++) {
        struct json_value o;
        json_init(&o);
        json_set_object(&o);
        json_push_kv_str(&o, "rung", sticky_rung_name((enum sticky_rung)i));
        json_push_kv_int(&o, "dispatches",
                         (int64_t)atomic_load(&g_rung_dispatches[i]));
        json_push_kv_bool(&o, "pluggable_action_set", g_rung_fn[i] != NULL);
        json_push_back(&arr, &o);
        json_free(&o);
    }
    json_push_kv(out, "rungs", &arr);
    json_free(&arr);
    return true;
}

#ifdef ZCL_TESTING
void sticky_escalator_test_reset(void)
{
    atomic_store(&g_armed, false);
    atomic_store(&g_rung, STICKY_RUNG_RETRY);
    atomic_store(&g_tip_at_rung, -1);
    atomic_store(&g_rung_entered_unix, 0);
    atomic_store(&g_last_page_unix, 0);
    atomic_store(&g_rearm_until_unix, 0);
    atomic_store(&g_fires_operator_needed, 0u);
    atomic_store(&g_episodes_cleared, 0u);
    atomic_store(&g_ladder_cycles, 0u);
    for (int i = 0; i < STICKY_RUNG_COUNT; i++) {
        atomic_store(&g_rung_dispatches[i], 0u);
        g_rung_fn[i] = NULL;
    }
}

enum sticky_rung sticky_escalator_test_drive(int64_t injected_tip,
                                             int64_t now_unix_in)
{
    apply_drive(injected_tip, now_unix_in);
    return (enum sticky_rung)atomic_load(&g_rung);
}

enum sticky_rung sticky_escalator_test_current_rung(void)
{
    return (enum sticky_rung)atomic_load(&g_rung);
}

bool sticky_escalator_test_armed(void)
{
    return atomic_load(&g_armed);
}

uint64_t sticky_escalator_test_fires_operator_needed(void)
{
    return atomic_load(&g_fires_operator_needed);
}
#endif
