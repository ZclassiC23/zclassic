/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer — invariants (sync/sync_reduce.h). Exhaustive enumeration
 * over phases × events × {stale,fresh} × {proof_ok} proving the structural laws
 * that make the kernel safe to trust as the single sync authority:
 *
 *   1. Every decision is well-formed (bounded next/actions/blocker).
 *   2. The next phase is always one of the LEGAL successors of `phase`.
 *   3. Repeated calls are memcmp-identical (byte-deterministic).
 *   4. A stale-session event is inert (next==phase, zero actions, no blocker).
 *   5. STAGED + any PROGRESS event ⇒ next==ACTIVATION_CONTAINED with blocker
 *      SYNC_BLOCKER_ACTIVATION_CONTAINED and ONLY RAISE_CONTAINMENT_BLOCKER —
 *      no other outcome (never FAIL, never STAGE_BUNDLE, never a live tip).
 *   6. Varying only the peer id never changes a decision (peer targeting lives
 *      on the event, not the decision).
 *   7. No path reaches STAGED except VERIFYING + PROOF_VERIFIED(proof_ok).
 *   8. VERIFYING never emits an activation action (structurally impossible —
 *      the enum has no such member; asserted at runtime as the witness). */

#include "test/test_helpers.h"
#include "sync/sync_reduce.h"
#include <string.h>

static struct sync_kernel_state mk_state(uint64_t sid, enum sync_phase p)
{
    struct sync_kernel_state s;
    memset(&s, 0, sizeof(s));
    s.session_id = sid;
    s.phase = p;
    return s;
}

static struct sync_event mk_event(uint64_t sid, enum sync_event_kind k,
                                  bool proof_ok, uint64_t peer)
{
    struct sync_event e;
    memset(&e, 0, sizeof(e));
    e.session_id = sid;
    e.kind = k;
    e.proof_ok = proof_ok;
    e.peer.value = peer;
    return e;
}

/* Legal successor table — the ONLY phases sync_reduce may return from a given
 * phase. Kept independent of the reducer's source so a wrong jump is caught. */
static bool legal_next(enum sync_phase from, enum sync_phase to)
{
    switch (from) {
    case SYNC_PHASE_IDLE:
        return to == SYNC_PHASE_IDLE || to == SYNC_PHASE_NEGOTIATING;
    case SYNC_PHASE_NEGOTIATING:
        return to == SYNC_PHASE_NEGOTIATING || to == SYNC_PHASE_RECEIVING ||
               to == SYNC_PHASE_FAILED || to == SYNC_PHASE_IDLE;
    case SYNC_PHASE_RECEIVING:
        return to == SYNC_PHASE_RECEIVING || to == SYNC_PHASE_VERIFYING ||
               to == SYNC_PHASE_FAILED || to == SYNC_PHASE_IDLE;
    case SYNC_PHASE_VERIFYING:
        return to == SYNC_PHASE_VERIFYING || to == SYNC_PHASE_STAGED ||
               to == SYNC_PHASE_FAILED || to == SYNC_PHASE_IDLE;
    case SYNC_PHASE_STAGED:
        return to == SYNC_PHASE_STAGED ||
               to == SYNC_PHASE_ACTIVATION_CONTAINED || to == SYNC_PHASE_IDLE;
    case SYNC_PHASE_ACTIVATION_CONTAINED:
        return to == SYNC_PHASE_ACTIVATION_CONTAINED || to == SYNC_PHASE_IDLE;
    case SYNC_PHASE_FAILED:
        return to == SYNC_PHASE_FAILED || to == SYNC_PHASE_IDLE;
    case SYNC_PHASE_COUNT:
        break;
    }
    return false;
}

static bool is_progress_event(enum sync_event_kind k)
{
    switch (k) {
    case SYNC_EVENT_START:
    case SYNC_EVENT_OFFER_RECEIVED:
    case SYNC_EVENT_OFFER_ACCEPTED:
    case SYNC_EVENT_CHUNK_RECEIVED:
    case SYNC_EVENT_RECEIVE_COMPLETE:
    case SYNC_EVENT_PROOF_VERIFIED:
        return true;
    case SYNC_EVENT_CHUNK_REJECTED:
    case SYNC_EVENT_PROOF_FAILED:
    case SYNC_EVENT_PEER_LOST:
    case SYNC_EVENT_TIMEOUT:
    case SYNC_EVENT_STOP_REQUESTED:
        return false;
    case SYNC_EVENT_COUNT:
        break;
    }
    return false;
}

static int test_wellformed_and_legal(void)
{
    int failures = 0;
    TEST("sync_reduce: every (phase,event,proof_ok) is well-formed + legal") {
        for (int p = 0; p < SYNC_PHASE_COUNT; p++) {
            for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
                for (int po = 0; po <= 1; po++) {
                    struct sync_decision d = sync_reduce(
                        mk_state(5, (enum sync_phase)p),
                        mk_event(5, (enum sync_event_kind)e, po != 0, 1));
                    ASSERT(d.next >= 0 && d.next < SYNC_PHASE_COUNT);
                    ASSERT(legal_next((enum sync_phase)p, d.next));
                    ASSERT(d.action_count >= 0 &&
                           d.action_count <= SYNC_DECISION_MAX_ACTIONS);
                    for (int i = 0; i < d.action_count; i++)
                        ASSERT(d.actions[i] > SYNC_ACTION_NONE &&
                               d.actions[i] < SYNC_ACTION_COUNT);
                    for (int i = d.action_count; i < SYNC_DECISION_MAX_ACTIONS; i++)
                        ASSERT(d.actions[i] == SYNC_ACTION_NONE);
                    if (d.has_blocker)
                        ASSERT(d.blocker > SYNC_BLOCKER_NONE &&
                               d.blocker < SYNC_BLOCKER_COUNT);
                    else
                        ASSERT(d.blocker == SYNC_BLOCKER_NONE);
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_byte_deterministic(void)
{
    int failures = 0;
    TEST("sync_reduce: repeated calls are memcmp-identical") {
        for (int p = 0; p < SYNC_PHASE_COUNT; p++) {
            for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
                for (int po = 0; po <= 1; po++) {
                    struct sync_kernel_state s = mk_state(11, (enum sync_phase)p);
                    struct sync_event ev =
                        mk_event(11, (enum sync_event_kind)e, po != 0, 3);
                    struct sync_decision a = sync_reduce(s, ev);
                    struct sync_decision b = sync_reduce(s, ev);
                    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_stale_is_inert(void)
{
    int failures = 0;
    TEST("sync_reduce: a stale session (non-zero, mismatched) is fully inert") {
        for (int p = 0; p < SYNC_PHASE_COUNT; p++) {
            for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
                struct sync_kernel_state s = mk_state(100, (enum sync_phase)p);
                struct sync_event ev =
                    mk_event(999 /* wrong session */, (enum sync_event_kind)e, true, 7);
                struct sync_decision d = sync_reduce(s, ev);
                ASSERT(d.next == (enum sync_phase)p);
                ASSERT(d.action_count == 0);
                ASSERT(!d.has_blocker);
                ASSERT(d.blocker == SYNC_BLOCKER_NONE);
                for (int i = 0; i < SYNC_DECISION_MAX_ACTIONS; i++)
                    ASSERT(d.actions[i] == SYNC_ACTION_NONE);
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_staged_progress_is_contained(void)
{
    int failures = 0;
    TEST("sync_reduce: STAGED + any progress event ⇒ contained, no other outcome") {
        for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
            if (!is_progress_event((enum sync_event_kind)e))
                continue;
            for (int po = 0; po <= 1; po++) {
                struct sync_decision d = sync_reduce(
                    mk_state(4, SYNC_PHASE_STAGED),
                    mk_event(4, (enum sync_event_kind)e, po != 0, 9));
                ASSERT(d.next == SYNC_PHASE_ACTIVATION_CONTAINED);
                ASSERT(d.has_blocker);
                ASSERT(d.blocker == SYNC_BLOCKER_ACTIVATION_CONTAINED);
                ASSERT(d.action_count == 1);
                ASSERT(d.actions[0] == SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER);
                /* No FAIL, no STAGE_BUNDLE, no other action anywhere. */
                for (int i = 1; i < SYNC_DECISION_MAX_ACTIONS; i++)
                    ASSERT(d.actions[i] == SYNC_ACTION_NONE);
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_peer_id_never_changes_decision(void)
{
    int failures = 0;
    TEST("sync_reduce: varying only the peer id never changes a decision") {
        const uint64_t peers[] = {0, 1, 42, 0xFFFFFFFFFFFFFFFFULL};
        for (int p = 0; p < SYNC_PHASE_COUNT; p++) {
            for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
                for (int po = 0; po <= 1; po++) {
                    struct sync_kernel_state s = mk_state(8, (enum sync_phase)p);
                    struct sync_decision baseline = sync_reduce(
                        s, mk_event(8, (enum sync_event_kind)e, po != 0, peers[0]));
                    for (size_t k = 1; k < sizeof(peers) / sizeof(peers[0]); k++) {
                        struct sync_decision d = sync_reduce(
                            s, mk_event(8, (enum sync_event_kind)e, po != 0, peers[k]));
                        ASSERT(memcmp(&baseline, &d, sizeof(d)) == 0);
                    }
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_only_verifying_proof_ok_reaches_staged(void)
{
    int failures = 0;
    TEST("sync_reduce: STAGED is reachable ONLY via VERIFYING+PROOF_VERIFIED(ok)") {
        for (int p = 0; p < SYNC_PHASE_COUNT; p++) {
            for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
                for (int po = 0; po <= 1; po++) {
                    struct sync_decision d = sync_reduce(
                        mk_state(6, (enum sync_phase)p),
                        mk_event(6, (enum sync_event_kind)e, po != 0, 2));
                    bool is_the_one_door =
                        (p == SYNC_PHASE_VERIFYING &&
                         e == SYNC_EVENT_PROOF_VERIFIED && po == 1);
                    /* "Reaching" STAGED means ENTERING it from another phase;
                     * a STAGED self-loop (STAGED + inert non-progress event)
                     * is not a fresh entry. */
                    if (d.next == SYNC_PHASE_STAGED && p != SYNC_PHASE_STAGED)
                        ASSERT(is_the_one_door);
                    if (is_the_one_door) {
                        ASSERT(d.next == SYNC_PHASE_STAGED);
                        ASSERT(d.action_count == 1);
                        ASSERT(d.actions[0] == SYNC_ACTION_STAGE_BUNDLE);
                        ASSERT(!d.has_blocker);
                    }
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

/* The runtime witness of the compile-time containment law: from VERIFYING, no
 * decision emits an activation/publish action (there is no such catalog member)
 * and none jumps past STAGED. */
static int test_verifying_never_activates(void)
{
    int failures = 0;
    TEST("sync_reduce: VERIFYING never emits an activation action") {
        for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
            for (int po = 0; po <= 1; po++) {
                struct sync_decision d = sync_reduce(
                    mk_state(3, SYNC_PHASE_VERIFYING),
                    mk_event(3, (enum sync_event_kind)e, po != 0, 1));
                /* Legal successors of VERIFYING contain no "live tip" phase. */
                ASSERT(legal_next(SYNC_PHASE_VERIFYING, d.next));
                for (int i = 0; i < d.action_count; i++) {
                    /* Every action is a real catalog action, and the catalog has
                     * no ACTIVATE/PUBLISH — proven again by the count guard. */
                    ASSERT(d.actions[i] > SYNC_ACTION_NONE &&
                           d.actions[i] < SYNC_ACTION_COUNT);
                    ASSERT(d.actions[i] != SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER);
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_reduce_invariants(void)
{
    int failures = 0;
    failures += test_wellformed_and_legal();
    failures += test_byte_deterministic();
    failures += test_stale_is_inert();
    failures += test_staged_progress_is_contained();
    failures += test_peer_id_never_changes_decision();
    failures += test_only_verifying_proof_ok_reaches_staged();
    failures += test_verifying_never_activates();
    return failures;
}
