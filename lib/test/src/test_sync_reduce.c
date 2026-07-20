/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer (sync/sync_reduce.h). Step-0 contract test: name lookups,
 * determinism, the stale-session guard, and the structural containment law.
 * WF1 lane 1A fills the real transition table and extends this group. */

#include "test/test_helpers.h"
#include "sync/sync_reduce.h"
#include <string.h>

static struct sync_kernel_state fresh_state(uint64_t sid, enum sync_phase p)
{
    struct sync_kernel_state s;
    memset(&s, 0, sizeof(s));
    s.session_id = sid;
    s.phase = p;
    return s;
}

static struct sync_event event_of(uint64_t sid, enum sync_event_kind k)
{
    struct sync_event e;
    memset(&e, 0, sizeof(e));
    e.session_id = sid;
    e.kind = k;
    return e;
}

static int test_sync_reduce_names(void)
{
    int failures = 0;
    TEST("sync_reduce: every catalog name is non-sentinel and round-trips") {
        for (int p = 0; p < SYNC_PHASE_COUNT; p++)
            ASSERT(strcmp(sync_phase_name((enum sync_phase)p), "?") != 0);
        for (int e = 0; e < SYNC_EVENT_COUNT; e++)
            ASSERT(strcmp(sync_event_name((enum sync_event_kind)e), "?") != 0);
        for (int a = 0; a < SYNC_ACTION_COUNT; a++)
            ASSERT(strcmp(sync_action_name((enum sync_action)a), "?") != 0);
        /* Out-of-range → "?" sentinel. */
        ASSERT(strcmp(sync_phase_name((enum sync_phase)SYNC_PHASE_COUNT), "?") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_reduce_deterministic(void)
{
    int failures = 0;
    TEST("sync_reduce: same (state,event) yields a byte-identical decision") {
        struct sync_kernel_state s = fresh_state(7, SYNC_PHASE_RECEIVING);
        struct sync_event e = event_of(7, SYNC_EVENT_CHUNK_RECEIVED);
        struct sync_decision d1 = sync_reduce(s, e);
        struct sync_decision d2 = sync_reduce(s, e);
        ASSERT(memcmp(&d1, &d2, sizeof(d1)) == 0);
        ASSERT(d1.action_count >= 0 &&
               d1.action_count <= SYNC_DECISION_MAX_ACTIONS);
        ASSERT(d1.next >= 0 && d1.next < SYNC_PHASE_COUNT);
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_reduce_stale_session(void)
{
    int failures = 0;
    TEST("sync_reduce: a stale-session event is a no-op") {
        struct sync_kernel_state s = fresh_state(9, SYNC_PHASE_VERIFYING);
        struct sync_event e = event_of(1234, SYNC_EVENT_PROOF_VERIFIED);
        struct sync_decision d = sync_reduce(s, e);
        ASSERT(d.next == s.phase);
        ASSERT(d.action_count == 0);
        ASSERT(!d.has_blocker);
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_reduce(void)
{
    int failures = 0;
    failures += test_sync_reduce_names();
    failures += test_sync_reduce_deterministic();
    failures += test_sync_reduce_stale_session();
    return failures;
}
