/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer — adapter shadow harness (sync/sync_reduce.h). Step-0
 * placeholder: proves the kernel is linkable and callable from the adapter's
 * translation unit. WF1 lane 1D lands the real SHADOW-mode assertion that the
 * kernel's decision agrees with snapsync_build_offer_acceptance() on the live
 * offer-accept path (msgprocessor_snapshot.c) — with the reference service
 * still authoritative. */

#include "test/test_helpers.h"
#include "sync/sync_reduce.h"
#include <string.h>

static int test_sync_reduce_adapter_linkable(void)
{
    int failures = 0;
    TEST("sync_reduce adapter: kernel is callable in shadow (no behavior change)") {
        struct sync_kernel_state s;
        memset(&s, 0, sizeof(s));
        s.session_id = 100;
        s.phase = SYNC_PHASE_IDLE;
        struct sync_event e;
        memset(&e, 0, sizeof(e));
        e.session_id = 100;
        e.kind = SYNC_EVENT_START;
        struct sync_decision d = sync_reduce(s, e);
        /* Shadow mode contract: whatever the kernel decides, it may never emit
         * an activation (there is no such action) — asserted structurally at
         * compile time; here we assert the decision is well-formed. */
        ASSERT(d.next >= 0 && d.next < SYNC_PHASE_COUNT);
        ASSERT(d.action_count >= 0 &&
               d.action_count <= SYNC_DECISION_MAX_ACTIONS);
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_reduce_adapter(void)
{
    return test_sync_reduce_adapter_linkable();
}
