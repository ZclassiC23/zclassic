/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer — see sync/sync_reduce.h.
 *
 * STEP-0 STATUS: this is the contracts-commit skeleton. The name lookups and
 * the count guards are REAL; the transition logic is a deterministic no-op
 * (next == current phase, no actions) EXCEPT for the stale-session guard,
 * which is load-bearing for the fuzz/invariant lanes. Lane 1A (wf/sync-kernel)
 * fills the exhaustive phase×event switches; it must NEVER add a default case
 * and NEVER add an ACTIVATE/PUBLISH action (the enum has none by construction). */

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

static struct sync_decision decision_noop(enum sync_phase phase)
{
    /* memset first so the returned struct is byte-deterministic — padding
     * bytes are zeroed, not left unspecified. This is what makes
     * "same input ⇒ byte-identical decision" hold under memcmp (the invariant
     * the fuzz lane asserts), not just field-wise equality. */
    struct sync_decision d;
    memset(&d, 0, sizeof(d));
    d.next = phase;
    for (int i = 0; i < SYNC_DECISION_MAX_ACTIONS; i++)
        d.actions[i] = SYNC_ACTION_NONE;
    d.action_count = 0;
    d.has_blocker = false;
    d.blocker = SYNC_BLOCKER_NONE;
    return d;
}

struct sync_decision sync_reduce(struct sync_kernel_state state,
                                 struct sync_event event)
{
    /* Stale-session guard (load-bearing invariant): an event addressed to a
     * different, still-active session is ignored — zero actions, unchanged
     * phase. A zero state session_id means "no session yet" and does not gate. */
    if (state.session_id != 0 && event.session_id != state.session_id)
        return decision_noop(state.phase);

    /* Exhaustive phase switch (no default — lane 1A fills each arm with the
     * per-event transition table). Today every arm is the deterministic no-op,
     * which already satisfies determinism, "stale ⇒ no-op", and the
     * "VERIFYING can never emit activation" invariants. */
    switch (state.phase) {
    case SYNC_PHASE_IDLE:
    case SYNC_PHASE_NEGOTIATING:
    case SYNC_PHASE_RECEIVING:
    case SYNC_PHASE_VERIFYING:
    case SYNC_PHASE_STAGED:
    case SYNC_PHASE_ACTIVATION_CONTAINED:
    case SYNC_PHASE_FAILED:
        (void)event;
        return decision_noop(state.phase);
    case SYNC_PHASE_COUNT:
        break; /* not a real phase */
    }
    /* Unreachable for a valid enum; return a safe no-op rather than UB. */
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
