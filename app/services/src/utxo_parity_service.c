/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_parity_service — see services/utxo_parity_service.h for rationale.
 *
 * Layout:
 *   1. Config + global state
 *   2. Finalized-frontier marker (EV_CHAIN_TIP_COMMIT observer, monotonic)
 *   3. utxo_parity_check_height() — one honest same-height comparison
 *   4. utxo_parity_tick_once() — the supervised, dormant-until-stable body
 *   5. init / set_reference_source / reset / dump_state_json
 *
 * Two correctness invariants, both load-bearing:
 *
 *   SAME-HEIGHT: the local SHA3 is computed over the LIVE utxos table, which
 *   reflects the set as of the live applied-coins height. There is no
 *   historical "as-of height" local commitment, so a byte DRIFT is only
 *   declared when an EXACT reference is at that SAME applied height
 *   (enforced in utxo_audit_compare_source). The tick therefore compares at
 *   exactly the live applied height — never a relabeled stable_ceiling, which
 *   would strcmp the live set against a reference at a DIFFERENT height and
 *   false-page — and only once that applied height is itself reorg-safe
 *   (at/below frontier - finality_depth). Continuous at-tip exact parity
 *   needs a reorg-safe historical commitment; see the header follow-ups.
 *
 *   FINALIZED FRONTIER: EV_CHAIN_TIP_COMMIT fires on every active-tip move,
 *   including reorgs and tip-clear ("to=-1"). It is NOT a finality signal.
 *   The frontier marker here is the MONOTONIC maximum of the durable
 *   finalized height (tip_finalize_stage_last_height) and committed,
 *   non-negative "to=" values — it never regresses, so the "dormant until
 *   the frontier advances" invariant holds even across deep reorgs.
 */

#include "services/utxo_parity_service.h"

#include "services/utxo_audit_service.h"
#include "services/utxo_reference_source.h"
#include "jobs/tip_finalize_stage.h"
#include "config/runtime.h"
#include "event/event.h"
#include "models/database.h"
#include "json/json.h"
#include "util/log_macros.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define PARITY_DEFAULT_FINALITY_DEPTH  100  /* matches ORACLE_TIP_SAFETY_MARGIN */
#define PARITY_DEFAULT_MAX_PER_TICK     1

/* ── Global state ──────────────────────────────────────────────── */

static struct {
    pthread_mutex_t lock;       /* guards config + ref pointer + ndb */
    bool   initialized;
    bool   enabled;
    int    finality_depth;
    int    max_checks_per_tick;
    struct node_db *ndb;
    const struct utxo_reference_source *ref;

    /* Monotonic finalized-frontier marker (highest stable committed height). */
    _Atomic int32_t finalized_frontier;

    /* Stats (atomic — readable from the dump path without the lock). */
    _Atomic int64_t checks_total;
    _Atomic int64_t matches;
    _Atomic int64_t mismatches;
    _Atomic int64_t coarse_checks;
    _Atomic int64_t reference_errors;
    _Atomic int32_t last_checked_height;
    _Atomic int32_t last_mismatch_height;
} g_parity = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .finalized_frontier = 0,
    .last_checked_height = 0,
    .last_mismatch_height = -1,
};

/* ── Finalized-frontier marker ─────────────────────────────────── */

/* Raise the monotonic frontier to `h` (ignores regressions / negatives). */
static void parity_frontier_raise(int32_t h)
{
    if (h <= 0)
        return;
    int32_t cur = atomic_load(&g_parity.finalized_frontier);
    while (h > cur) {
        if (atomic_compare_exchange_weak(&g_parity.finalized_frontier,
                                         &cur, h))
            return;
        /* cur reloaded by CAS; loop re-checks h > cur. */
    }
}

/* EV_CHAIN_TIP_COMMIT observer: parse "to=%d" and raise the frontier. The
 * raw "to=" tracks the volatile ACTIVE tip (it can regress on reorg and is
 * "-1" on a tip-clear), so we (a) ignore non-positive values and (b) only
 * ever raise the marker — finality comes from monotonicity + finality_depth,
 * cross-checked against the durable finalized height in the tick. */
static void parity_on_tip_commit(enum event_type type, uint32_t peer_id,
                                 const void *payload, uint32_t payload_len,
                                 void *ctx)
{
    (void)type; (void)peer_id; (void)payload_len; (void)ctx;
    if (!payload)
        return;
    const char *s = strstr((const char *)payload, "to=");
    if (!s)
        return;
    int to = 0;
    if (sscanf(s, "to=%d", &to) != 1)
        return;
    if (to > 0)
        parity_frontier_raise((int32_t)to);
}

void utxo_parity_observe_finalization(void)
{
    /* event_observe APPENDS unconditionally (no (type, fn) dedup) and observer
     * slots are a small fixed pool, so register exactly once per process — a
     * static guard keeps repeated boot/restart cycles from leaking slots. */
    static atomic_bool observed = false;
    bool expected = false;
    if (atomic_compare_exchange_strong(&observed, &expected, true))
        (void)event_observe(EV_CHAIN_TIP_COMMIT, parity_on_tip_commit, NULL);
}

void utxo_parity_set_frontier_for_test(int32_t height)
{
    parity_frontier_raise(height);
}

/* ── Synchronous same-height check ─────────────────────────────── */

struct zcl_result utxo_parity_check_height(int32_t height,
                                           struct utxo_audit_result *out)
{
    if (!out)
        return ZCL_ERR(-1, "utxo_parity: null out");

    pthread_mutex_lock(&g_parity.lock);
    struct node_db *ndb = g_parity.ndb ? g_parity.ndb : app_runtime_node_db();
    const struct utxo_reference_source *ref = g_parity.ref;
    pthread_mutex_unlock(&g_parity.lock);

    if (!ref) {
        memset(out, 0, sizeof(*out));
        out->status = UTXO_AUDIT_ERROR;
        snprintf(out->error, sizeof(out->error), "no reference source");
        return ZCL_ERR(-2, "utxo_parity: no reference source");
    }

    struct zcl_result r = utxo_audit_compare_source(ndb, ref, height, out);

    /* Update counters from the single comparator outcome. */
    atomic_fetch_add(&g_parity.checks_total, 1);
    atomic_store(&g_parity.last_checked_height, height);
    if (!r.ok) {
        atomic_fetch_add(&g_parity.reference_errors, 1);
    } else if (!ref->exact) {
        atomic_fetch_add(&g_parity.coarse_checks, 1);
    } else if (out->status == UTXO_AUDIT_DRIFT) {
        atomic_fetch_add(&g_parity.mismatches, 1);
        atomic_store(&g_parity.last_mismatch_height, height);
    } else if (out->status == UTXO_AUDIT_MATCH) {
        atomic_fetch_add(&g_parity.matches, 1);
    }
    /* LOCAL_ONLY (exact height-skew) is neither a match nor a mismatch:
     * it is intentionally uncounted in either bucket so a transient skew
     * during catch-up does not look like a confirmed match. */
    return r;
}

/* ── Supervised tick body ──────────────────────────────────────── */

void utxo_parity_tick_once(void)
{
    pthread_mutex_lock(&g_parity.lock);
    bool enabled = g_parity.initialized && g_parity.enabled;
    const struct utxo_reference_source *ref = g_parity.ref;
    int finality_depth = g_parity.finality_depth > 0
                             ? g_parity.finality_depth
                             : PARITY_DEFAULT_FINALITY_DEPTH;
    int max_per_tick = g_parity.max_checks_per_tick > 0
                           ? g_parity.max_checks_per_tick
                           : PARITY_DEFAULT_MAX_PER_TICK;
    struct node_db *ndb = g_parity.ndb ? g_parity.ndb : app_runtime_node_db();
    pthread_mutex_unlock(&g_parity.lock);

    /* Activation authority is the `enabled` flag (set by utxo_parity_init from
     * cfg.enabled) AND a wired reference. Boot turns `enabled` on only when a
     * zclassicd reference resolves (auto-detected from RPC creds, opt-out via
     * ZCL_PARITY_ORACLE=0) — so without a co-located zclassicd, neither is set
     * and the tick is a quiet no-op. The reference vtable owns reachability: if
     * the daemon is momentarily down, commitment_at returns a reference error
     * (counted, never a DRIFT), never a stall. No env var is required to run. */
    if (!enabled || !ref)
        return;

    /* Cross-check the volatile EV_CHAIN_TIP_COMMIT marker against the durable
     * finalized height; take the max so neither a missed event nor a reorg
     * regresses the frontier. */
    int64_t durable = tip_finalize_stage_last_height();
    if (durable > 0)
        parity_frontier_raise((int32_t)durable);
    int32_t frontier = atomic_load(&g_parity.finalized_frontier);
    if (frontier <= 0)
        return; /* tip not advancing — genuinely dormant */

    /* Stability gate: only compare at a height that is provably below the
     * frontier by finality_depth (cannot reorg) AND that the live applied
     * set actually reflects. */
    int32_t stable_ceiling = frontier - (int32_t)finality_depth;
    if (stable_ceiling <= 0)
        return;

    int applied = app_runtime_node_db_utxo_max_height(ndb);
    if (applied <= 0)
        return;

    /* The local SHA3 is computed over the LIVE utxos table, so the only height
     * it honestly reflects is `applied`. Compare at exactly that height (never
     * a relabeled stable_ceiling — that would strcmp the live set against a
     * reference at a DIFFERENT height and false-page). Only do so once
     * `applied` is itself reorg-safe (at/below frontier - finality_depth);
     * while the live tip races ahead of finality there is no reorg-safe height
     * whose set the live table reflects, so we wait. Continuous at-tip exact
     * parity needs a reorg-safe historical commitment (see the header
     * follow-ups). */
    if ((int32_t)applied > stable_ceiling)
        return;
    int32_t target = (int32_t)applied;
    if (target <= atomic_load(&g_parity.last_checked_height))
        return; /* already current — nothing new to compare */

    /* Walk forward, bounded, so we never burst the full-set SHA3. */
    for (int i = 0; i < max_per_tick && target > 0; i++) {
        struct utxo_audit_result res;
        (void)utxo_parity_check_height(target, &res);
        /* check_height advances last_checked_height; one stable target per
         * tick is the common case (the frontier advances slowly). */
        break;
    }
}

/* ── init / wiring ─────────────────────────────────────────────── */

struct zcl_result utxo_parity_init(const struct utxo_parity_config *cfg,
                                   struct node_db *ndb)
{
    pthread_mutex_lock(&g_parity.lock);
    g_parity.enabled = cfg ? cfg->enabled : false;
    g_parity.finality_depth = (cfg && cfg->finality_depth > 0)
                                  ? cfg->finality_depth
                                  : PARITY_DEFAULT_FINALITY_DEPTH;
    g_parity.max_checks_per_tick = (cfg && cfg->max_checks_per_tick > 0)
                                       ? cfg->max_checks_per_tick
                                       : PARITY_DEFAULT_MAX_PER_TICK;
    g_parity.ndb = ndb;
    g_parity.initialized = true;
    pthread_mutex_unlock(&g_parity.lock);
    return ZCL_OK;
}

void utxo_parity_set_reference_source(const struct utxo_reference_source *src)
{
    pthread_mutex_lock(&g_parity.lock);
    g_parity.ref = src;
    pthread_mutex_unlock(&g_parity.lock);
}

void utxo_parity_reset_for_test(void)
{
    pthread_mutex_lock(&g_parity.lock);
    g_parity.initialized = false;
    g_parity.enabled = false;
    g_parity.finality_depth = 0;
    g_parity.max_checks_per_tick = 0;
    g_parity.ndb = NULL;
    g_parity.ref = NULL;
    pthread_mutex_unlock(&g_parity.lock);
    atomic_store(&g_parity.finalized_frontier, 0);
    atomic_store(&g_parity.checks_total, 0);
    atomic_store(&g_parity.matches, 0);
    atomic_store(&g_parity.mismatches, 0);
    atomic_store(&g_parity.coarse_checks, 0);
    atomic_store(&g_parity.reference_errors, 0);
    atomic_store(&g_parity.last_checked_height, 0);
    atomic_store(&g_parity.last_mismatch_height, -1);
}

/* ── State dump (see CLAUDE.md "Adding state introspection") ───── */

bool utxo_parity_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    pthread_mutex_lock(&g_parity.lock);
    bool enabled = g_parity.enabled;
    int finality_depth = g_parity.finality_depth;
    const struct utxo_reference_source *ref = g_parity.ref;
    const char *ref_name = ref && ref->name ? ref->name : "none";
    bool ref_exact = ref ? ref->exact : false;
    struct node_db *ndb = g_parity.ndb;
    pthread_mutex_unlock(&g_parity.lock);

    bool drift_flag = false;
    if (ndb && ndb->open) {
        int64_t drift = 0;
        if (node_db_state_get_int(ndb, "utxo_drift_detected", &drift))
            drift_flag = (drift != 0);
    }

    json_push_kv_bool(out, "enabled", enabled);
    /* `active` is the real run predicate the tick checks: enabled AND a wired
     * reference. With no co-located zclassicd, boot leaves both off and this is
     * false (the service is quietly dormant — no health impact, no log spam). */
    json_push_kv_bool(out, "active", enabled && ref != NULL);
    json_push_kv_bool(out, "env_force_on", getenv("ZCL_PARITY_ENABLE") != NULL);
    json_push_kv_str (out, "reference_source", ref_name);
    json_push_kv_bool(out, "reference_exact", ref_exact);
    json_push_kv_int (out, "finality_depth", finality_depth);
    json_push_kv_int (out, "finalized_frontier",
                      atomic_load(&g_parity.finalized_frontier));
    json_push_kv_int (out, "last_checked_height",
                      atomic_load(&g_parity.last_checked_height));
    json_push_kv_int (out, "checks_total",
                      atomic_load(&g_parity.checks_total));
    json_push_kv_int (out, "matches", atomic_load(&g_parity.matches));
    json_push_kv_int (out, "mismatches", atomic_load(&g_parity.mismatches));
    json_push_kv_int (out, "coarse_checks",
                      atomic_load(&g_parity.coarse_checks));
    json_push_kv_int (out, "reference_errors",
                      atomic_load(&g_parity.reference_errors));
    json_push_kv_int (out, "last_mismatch_height",
                      atomic_load(&g_parity.last_mismatch_height));
    json_push_kv_bool(out, "drift_flag", drift_flag);
    return true;
}
