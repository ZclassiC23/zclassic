/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer — invariants (sync/sync_reduce.h). Step-0 contract test:
 * the exhaustive state×event sweep proving the decision is always well-formed
 * and that activation is structurally unrepresentable. WF1 lane 1C replaces
 * this with the full invariant + fuzz suite. */

#include "test/test_helpers.h"
#include "sync/sync_reduce.h"
#include <string.h>

static int test_sync_reduce_exhaustive_wellformed(void)
{
    int failures = 0;
    TEST("sync_reduce: every (phase,event) yields a well-formed decision") {
        for (int p = 0; p < SYNC_PHASE_COUNT; p++) {
            for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
                struct sync_kernel_state s;
                memset(&s, 0, sizeof(s));
                s.session_id = 5;
                s.phase = (enum sync_phase)p;
                struct sync_event ev;
                memset(&ev, 0, sizeof(ev));
                ev.session_id = 5;
                ev.kind = (enum sync_event_kind)e;

                struct sync_decision d = sync_reduce(s, ev);
                ASSERT(d.next >= 0 && d.next < SYNC_PHASE_COUNT);
                ASSERT(d.action_count >= 0 &&
                       d.action_count <= SYNC_DECISION_MAX_ACTIONS);
                for (int i = 0; i < d.action_count; i++)
                    ASSERT(d.actions[i] >= 0 && d.actions[i] < SYNC_ACTION_COUNT);
                if (d.has_blocker)
                    ASSERT(d.blocker > SYNC_BLOCKER_NONE &&
                           d.blocker < SYNC_BLOCKER_COUNT);
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_reduce_no_activation_from_verifying(void)
{
    int failures = 0;
    TEST("sync_reduce: VERIFYING never self-activates a tip (structural)") {
        /* The action enum has no ACTIVATE/PUBLISH member — the strongest form
         * of this invariant is proven at compile time. Here we assert the
         * runtime corollary: from VERIFYING, no decision jumps straight past
         * STAGED into a live-tip phase (there is no such phase to reach). */
        for (int e = 0; e < SYNC_EVENT_COUNT; e++) {
            struct sync_kernel_state s;
            memset(&s, 0, sizeof(s));
            s.session_id = 3;
            s.phase = SYNC_PHASE_VERIFYING;
            struct sync_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.session_id = 3;
            ev.kind = (enum sync_event_kind)e;
            struct sync_decision d = sync_reduce(s, ev);
            /* Every emitted action is a real catalog action — and the catalog
             * has NO activate/publish member, so no decision can ever ask for
             * tip activation. This is the runtime witness of the compile-time
             * structural guarantee. */
            ASSERT(d.next >= 0 && d.next < SYNC_PHASE_COUNT);
            for (int i = 0; i < d.action_count; i++) {
                ASSERT(d.actions[i] >= SYNC_ACTION_NONE &&
                       d.actions[i] < SYNC_ACTION_COUNT);
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_reduce_invariants(void)
{
    int failures = 0;
    failures += test_sync_reduce_exhaustive_wellformed();
    failures += test_sync_reduce_no_activation_from_verifying();
    return failures;
}
