/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer — see sync/sync_reduce.h.
 *
 * The whole snapshot fast-sync FSM as one total, side-effect-free function.
 * It models the EXISTING flow (lib/sync/src/sync_state.c g_snapsync_transitions
 * + the offer/receive/verify path in app/services/src/snapshot_*.c and
 * lib/net/src/msgprocessor_snapshot.c):
 *
 *     IDLE ──OFFER_RECEIVED/START──▶ NEGOTIATING
 *     NEGOTIATING ──OFFER_ACCEPTED──▶ RECEIVING
 *     RECEIVING ──RECEIVE_COMPLETE──▶ VERIFYING
 *     VERIFYING ──PROOF_VERIFIED(proof_ok)──▶ STAGED
 *     STAGED ──any progress event──▶ ACTIVATION_CONTAINED   (blocker raised)
 *     {NEGOTIATING,RECEIVING,VERIFYING} ──PROOF_FAILED/PEER_LOST/TIMEOUT──▶ FAILED
 *
 * Containment is UNREPRESENTABLE: the action enum has no ACTIVATE / PUBLISH
 * member (sync_kernel_catalog.def), mirroring
 * g_snapsync_transitions[VERIFYING][COMPLETE]=false. The furthest a decision
 * carries state is STAGED; the only thing it can do at the activation boundary
 * is RAISE_CONTAINMENT_BLOCKER and move to ACTIVATION_CONTAINED. Tip activation
 * stays the reference FSM's job (this kernel lands in SHADOW mode).
 *
 * Purity law: no clock, RNG, socket, DB, global, or allocation. Every input is
 * in the two by-value arguments; the memset-zeroed decision is byte-identical
 * for identical inputs (the determinism invariant the fuzz lane asserts under
 * memcmp). The exhaustive phase×event switches carry NO default — a new
 * phase/event forces a compile error until every arm is handled. */

#include "sync/sync_reduce.h"

#include <string.h>

/* Catalog count guards (pattern: config/src/command_catalog.c:278). If a
 * phase/event/action is added to the .def, bump the matching number here so
 * the exhaustive switches and name tables are forced back into sync. */
_Static_assert(SYNC_PHASE_COUNT == 7,
               "sync_phase catalog changed — update sync_reduce switches + name table");
_Static_assert(SYNC_EVENT_COUNT == 11,
               "sync_event catalog changed — update sync_reduce switches + name table");
_Static_assert(SYNC_ACTION_COUNT == 10,
               "sync_action catalog changed — update the adapter action executor");

/* Structural proof of the containment law: no action names ACTIVATE/PUBLISH.
 * This is only a reminder guard — the enum simply has no such member, so a
 * decision physically cannot ask for tip activation. */
_Static_assert(SYNC_BLOCKER_COUNT >= 1, "at least SYNC_BLOCKER_NONE must exist");

/* ── Decision builders (all memset-zeroed for byte-identical determinism) ── */

/* Base: zero the whole struct (padding included) and fill the actions[] tail
 * with SYNC_ACTION_NONE so a later memcmp compares only meaningful bytes. */
static struct sync_decision decision_base(enum sync_phase next)
{
    struct sync_decision d;
    memset(&d, 0, sizeof(d));
    d.next = next;
    for (int i = 0; i < SYNC_DECISION_MAX_ACTIONS; i++)
        d.actions[i] = SYNC_ACTION_NONE;
    d.action_count = 0;
    d.has_blocker = false;
    d.blocker = SYNC_BLOCKER_NONE;
    return d;
}

/* Stay in / move to `next` with no side effects. */
static struct sync_decision decision_noop(enum sync_phase next)
{
    return decision_base(next);
}

/* Move to `next` emitting a single action. */
static struct sync_decision decision_act1(enum sync_phase next,
                                          enum sync_action a0)
{
    struct sync_decision d = decision_base(next);
    d.actions[0] = a0;
    d.action_count = 1;
    return d;
}

/* Move to `next` emitting two ordered actions. */
static struct sync_decision decision_act2(enum sync_phase next,
                                          enum sync_action a0,
                                          enum sync_action a1)
{
    struct sync_decision d = decision_base(next);
    d.actions[0] = a0;
    d.actions[1] = a1;
    d.action_count = 2;
    return d;
}

/* Terminal failure with a typed blocker (proof failure / peer loss / timeout). */
static struct sync_decision decision_fail(enum sync_blocker blocker)
{
    struct sync_decision d = decision_base(SYNC_PHASE_FAILED);
    d.actions[0] = SYNC_ACTION_FAIL;
    d.action_count = 1;
    d.has_blocker = true;
    d.blocker = blocker;
    return d;
}

/* The activation door: verified state may NOT self-activate. Raise the typed
 * containment blocker and move to (or hold at) ACTIVATION_CONTAINED. The only
 * action available here is RAISE_CONTAINMENT_BLOCKER — there is deliberately
 * no ACTIVATE action to reach for. */
static struct sync_decision decision_contain(void)
{
    struct sync_decision d = decision_base(SYNC_PHASE_ACTIVATION_CONTAINED);
    d.actions[0] = SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER;
    d.action_count = 1;
    d.has_blocker = true;
    d.blocker = SYNC_BLOCKER_ACTIVATION_CONTAINED;
    return d;
}

/* ── Per-phase transition arms (each an exhaustive event switch, no default) ── */

static struct sync_decision reduce_idle(const struct sync_event *e)
{
    switch (e->kind) {
    case SYNC_EVENT_START:            return decision_noop(SYNC_PHASE_NEGOTIATING);
    case SYNC_EVENT_OFFER_RECEIVED:   return decision_act1(SYNC_PHASE_NEGOTIATING, SYNC_ACTION_STORE_OFFER);
    case SYNC_EVENT_OFFER_ACCEPTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_CHUNK_RECEIVED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_CHUNK_REJECTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_RECEIVE_COMPLETE: return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_PROOF_VERIFIED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_PROOF_FAILED:     return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_PEER_LOST:        return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_TIMEOUT:          return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_STOP_REQUESTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_COUNT:            break; /* not a real event */
    }
    return decision_noop(SYNC_PHASE_IDLE);
}

static struct sync_decision reduce_negotiating(const struct sync_event *e)
{
    switch (e->kind) {
    case SYNC_EVENT_START:            return decision_noop(SYNC_PHASE_NEGOTIATING);
    case SYNC_EVENT_OFFER_RECEIVED:   return decision_act1(SYNC_PHASE_NEGOTIATING, SYNC_ACTION_STORE_OFFER);
    case SYNC_EVENT_OFFER_ACCEPTED:   return decision_act2(SYNC_PHASE_RECEIVING, SYNC_ACTION_RESET_OFFSET, SYNC_ACTION_BEGIN_RECEIVE);
    case SYNC_EVENT_CHUNK_RECEIVED:   return decision_noop(SYNC_PHASE_NEGOTIATING);
    case SYNC_EVENT_CHUNK_REJECTED:   return decision_noop(SYNC_PHASE_NEGOTIATING);
    case SYNC_EVENT_RECEIVE_COMPLETE: return decision_noop(SYNC_PHASE_NEGOTIATING);
    case SYNC_EVENT_PROOF_VERIFIED:   return decision_noop(SYNC_PHASE_NEGOTIATING);
    case SYNC_EVENT_PROOF_FAILED:     return decision_fail(SYNC_BLOCKER_PROOF_FAILED);
    case SYNC_EVENT_PEER_LOST:        return decision_fail(SYNC_BLOCKER_PEER_LOST);
    case SYNC_EVENT_TIMEOUT:          return decision_fail(SYNC_BLOCKER_TIMEOUT);
    case SYNC_EVENT_STOP_REQUESTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_COUNT:            break;
    }
    return decision_noop(SYNC_PHASE_NEGOTIATING);
}

static struct sync_decision reduce_receiving(const struct sync_event *e)
{
    switch (e->kind) {
    case SYNC_EVENT_START:            return decision_noop(SYNC_PHASE_RECEIVING);
    case SYNC_EVENT_OFFER_RECEIVED:   return decision_noop(SYNC_PHASE_RECEIVING);
    case SYNC_EVENT_OFFER_ACCEPTED:   return decision_noop(SYNC_PHASE_RECEIVING);
    case SYNC_EVENT_CHUNK_RECEIVED:   return decision_act1(SYNC_PHASE_RECEIVING, SYNC_ACTION_APPLY_CHUNK);
    case SYNC_EVENT_CHUNK_REJECTED:   return decision_act1(SYNC_PHASE_RECEIVING, SYNC_ACTION_PENALIZE_PEER);
    case SYNC_EVENT_RECEIVE_COMPLETE: return decision_act1(SYNC_PHASE_VERIFYING, SYNC_ACTION_START_VERIFY);
    case SYNC_EVENT_PROOF_VERIFIED:   return decision_noop(SYNC_PHASE_RECEIVING);
    case SYNC_EVENT_PROOF_FAILED:     return decision_fail(SYNC_BLOCKER_PROOF_FAILED);
    case SYNC_EVENT_PEER_LOST:        return decision_fail(SYNC_BLOCKER_PEER_LOST);
    case SYNC_EVENT_TIMEOUT:          return decision_fail(SYNC_BLOCKER_TIMEOUT);
    case SYNC_EVENT_STOP_REQUESTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_COUNT:            break;
    }
    return decision_noop(SYNC_PHASE_RECEIVING);
}

static struct sync_decision reduce_verifying(const struct sync_event *e)
{
    switch (e->kind) {
    case SYNC_EVENT_START:            return decision_noop(SYNC_PHASE_VERIFYING);
    case SYNC_EVENT_OFFER_RECEIVED:   return decision_noop(SYNC_PHASE_VERIFYING);
    case SYNC_EVENT_OFFER_ACCEPTED:   return decision_noop(SYNC_PHASE_VERIFYING);
    case SYNC_EVENT_CHUNK_RECEIVED:   return decision_noop(SYNC_PHASE_VERIFYING);
    case SYNC_EVENT_CHUNK_REJECTED:   return decision_noop(SYNC_PHASE_VERIFYING);
    case SYNC_EVENT_RECEIVE_COMPLETE: return decision_noop(SYNC_PHASE_VERIFYING);
    /* The ONLY door to STAGED, and only when the proof actually passed. A
     * PROOF_VERIFIED carrying proof_ok==false is a failed proof, never a stage. */
    case SYNC_EVENT_PROOF_VERIFIED:
        return e->proof_ok
                   ? decision_act1(SYNC_PHASE_STAGED, SYNC_ACTION_STAGE_BUNDLE)
                   : decision_fail(SYNC_BLOCKER_PROOF_FAILED);
    case SYNC_EVENT_PROOF_FAILED:     return decision_fail(SYNC_BLOCKER_PROOF_FAILED);
    case SYNC_EVENT_PEER_LOST:        return decision_fail(SYNC_BLOCKER_PEER_LOST);
    case SYNC_EVENT_TIMEOUT:          return decision_fail(SYNC_BLOCKER_TIMEOUT);
    case SYNC_EVENT_STOP_REQUESTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_COUNT:            break;
    }
    return decision_noop(SYNC_PHASE_VERIFYING);
}

static struct sync_decision reduce_staged(const struct sync_event *e)
{
    switch (e->kind) {
    /* Any attempt to PROGRESS past STAGED hits the contained activation door:
     * next==ACTIVATION_CONTAINED, blocker==SYNC_BLOCKER_ACTIVATION_CONTAINED,
     * and the only action is RAISE_CONTAINMENT_BLOCKER. No activate exists. */
    case SYNC_EVENT_START:            return decision_contain();
    case SYNC_EVENT_OFFER_RECEIVED:   return decision_contain();
    case SYNC_EVENT_OFFER_ACCEPTED:   return decision_contain();
    case SYNC_EVENT_CHUNK_RECEIVED:   return decision_contain();
    case SYNC_EVENT_RECEIVE_COMPLETE: return decision_contain();
    case SYNC_EVENT_PROOF_VERIFIED:   return decision_contain();
    /* Non-progress events: the bundle is already staged & verified — irrelevant
     * faults hold at STAGED; an explicit stop abandons it. */
    case SYNC_EVENT_CHUNK_REJECTED:   return decision_noop(SYNC_PHASE_STAGED);
    case SYNC_EVENT_PROOF_FAILED:     return decision_noop(SYNC_PHASE_STAGED);
    case SYNC_EVENT_PEER_LOST:        return decision_noop(SYNC_PHASE_STAGED);
    case SYNC_EVENT_TIMEOUT:          return decision_noop(SYNC_PHASE_STAGED);
    case SYNC_EVENT_STOP_REQUESTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_COUNT:            break;
    }
    return decision_noop(SYNC_PHASE_STAGED);
}

static struct sync_decision reduce_activation_contained(const struct sync_event *e)
{
    switch (e->kind) {
    /* Containment holds. A renewed progress attempt idempotently re-raises the
     * containment blocker; a stop resets to IDLE; everything else is inert. */
    case SYNC_EVENT_START:            return decision_contain();
    case SYNC_EVENT_OFFER_RECEIVED:   return decision_contain();
    case SYNC_EVENT_OFFER_ACCEPTED:   return decision_contain();
    case SYNC_EVENT_CHUNK_RECEIVED:   return decision_contain();
    case SYNC_EVENT_RECEIVE_COMPLETE: return decision_contain();
    case SYNC_EVENT_PROOF_VERIFIED:   return decision_contain();
    case SYNC_EVENT_CHUNK_REJECTED:   return decision_noop(SYNC_PHASE_ACTIVATION_CONTAINED);
    case SYNC_EVENT_PROOF_FAILED:     return decision_noop(SYNC_PHASE_ACTIVATION_CONTAINED);
    case SYNC_EVENT_PEER_LOST:        return decision_noop(SYNC_PHASE_ACTIVATION_CONTAINED);
    case SYNC_EVENT_TIMEOUT:          return decision_noop(SYNC_PHASE_ACTIVATION_CONTAINED);
    case SYNC_EVENT_STOP_REQUESTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_COUNT:            break;
    }
    return decision_noop(SYNC_PHASE_ACTIVATION_CONTAINED);
}

static struct sync_decision reduce_failed(const struct sync_event *e)
{
    switch (e->kind) {
    /* Terminal. Only an explicit stop clears the failure back to IDLE so a
     * fresh session can begin; every other event is inert. */
    case SYNC_EVENT_START:            return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_OFFER_RECEIVED:   return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_OFFER_ACCEPTED:   return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_CHUNK_RECEIVED:   return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_CHUNK_REJECTED:   return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_RECEIVE_COMPLETE: return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_PROOF_VERIFIED:   return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_PROOF_FAILED:     return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_PEER_LOST:        return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_TIMEOUT:          return decision_noop(SYNC_PHASE_FAILED);
    case SYNC_EVENT_STOP_REQUESTED:   return decision_noop(SYNC_PHASE_IDLE);
    case SYNC_EVENT_COUNT:            break;
    }
    return decision_noop(SYNC_PHASE_FAILED);
}

/* ── The reducer ─────────────────────────────────────────────────────── */

struct sync_decision sync_reduce(struct sync_kernel_state state,
                                 struct sync_event event)
{
    /* Stale-session guard (load-bearing invariant): an event addressed to a
     * different, still-active session is ignored — zero actions, unchanged
     * phase. A zero state session_id means "no session yet" and does not gate. */
    if (state.session_id != 0 && event.session_id != state.session_id)
        return decision_noop(state.phase);

    /* Exhaustive phase switch — NO default. Each arm dispatches to a per-phase
     * event switch that is itself exhaustive. Adding a phase to the catalog
     * breaks the build here until its arm exists. */
    switch (state.phase) {
    case SYNC_PHASE_IDLE:                 return reduce_idle(&event);
    case SYNC_PHASE_NEGOTIATING:          return reduce_negotiating(&event);
    case SYNC_PHASE_RECEIVING:            return reduce_receiving(&event);
    case SYNC_PHASE_VERIFYING:            return reduce_verifying(&event);
    case SYNC_PHASE_STAGED:               return reduce_staged(&event);
    case SYNC_PHASE_ACTIVATION_CONTAINED: return reduce_activation_contained(&event);
    case SYNC_PHASE_FAILED:               return reduce_failed(&event);
    case SYNC_PHASE_COUNT:                break; /* not a real phase */
    }
    /* Unreachable for a valid enum; a safe no-op rather than UB. */
    return decision_noop(state.phase);
}

/* ── Name lookups (generated from the catalog) ──────────────────────── */

const char *sync_phase_name(enum sync_phase phase)
{
    switch (phase) {
#define SYNC_PHASE(id, name) case SYNC_PHASE_##id: return (name);
#include "sync/sync_kernel_catalog.def"
    case SYNC_PHASE_COUNT: break;
    }
    return "?";
}

const char *sync_event_name(enum sync_event_kind kind)
{
    switch (kind) {
#define SYNC_EVENT(id, name) case SYNC_EVENT_##id: return (name);
#include "sync/sync_kernel_catalog.def"
    case SYNC_EVENT_COUNT: break;
    }
    return "?";
}

const char *sync_action_name(enum sync_action action)
{
    switch (action) {
#define SYNC_ACTION(id, name) case SYNC_ACTION_##id: return (name);
#include "sync/sync_kernel_catalog.def"
    case SYNC_ACTION_COUNT: break;
    }
    return "?";
}
