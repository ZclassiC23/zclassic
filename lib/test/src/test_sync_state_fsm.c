/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Property tests for the sync_state and snapshot_sync_state FSMs in
 * lib/sync/. These tests assert the architectural invariant that
 * makes lib/sync/ load-bearing: every state transition is validated
 * against a static table; illegal transitions are rejected and do
 * not mutate state.
 *
 * Without these tests, a future commit that "simplifies"
 * sync_set_state by removing the table lookup would still pass the
 * rest of the suite — the existing tests exercise legal transitions
 * only.
 */

#include "test/test_helpers.h"
#include "sync/sync_state.h"
#include "event/event.h"

#include <string.h>

/* Reset the FSM to SYNC_IDLE before each test. SYNC_FAILED → SYNC_IDLE
 * and SYNC_AT_TIP → SYNC_IDLE are both legal edges, so this works from
 * any reachable steady state. If the FSM is already IDLE the setter
 * is a no-op (returns true). */
static void reset_sync_state(void)
{
    enum sync_state cur = sync_get_state();
    if (cur == SYNC_IDLE) return;
    if (cur == SYNC_AT_TIP) {
        (void)sync_set_state(SYNC_IDLE, "test_reset");
        return;
    }
    if (cur == SYNC_FAILED) {
        (void)sync_set_state(SYNC_IDLE, "test_reset");
        return;
    }
    /* Most non-tip states have a direct edge to SYNC_IDLE per the
     * transition table; try it and fall through if it ever changes. */
    (void)sync_set_state(SYNC_IDLE, "test_reset");
}

static int test_sync_fsm_rejects_idle_to_at_tip(void)
{
    int failures = 0;
    TEST("sync_state FSM: rejects SYNC_IDLE → SYNC_AT_TIP (no direct edge)") {
        reset_sync_state();
        ASSERT(sync_get_state() == SYNC_IDLE);

        bool ok = sync_set_state(SYNC_AT_TIP, "test illegal edge");
        ASSERT(!ok);

        /* State must not have changed on a rejected transition. */
        ASSERT(sync_get_state() == SYNC_IDLE);
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_fsm_accepts_legal_chain(void)
{
    int failures = 0;
    TEST("sync_state FSM: accepts IDLE → FINDING_PEERS → HEADERS → BLOCKS → CONNECTING → AT_TIP") {
        reset_sync_state();
        ASSERT(sync_set_state(SYNC_FINDING_PEERS,    "t1"));
        ASSERT(sync_set_state(SYNC_HEADERS_DOWNLOAD, "t2"));
        ASSERT(sync_set_state(SYNC_BLOCKS_DOWNLOAD,  "t3"));
        ASSERT(sync_set_state(SYNC_CONNECTING_BLOCKS,"t4"));
        ASSERT(sync_set_state(SYNC_AT_TIP,           "t5"));
        ASSERT(sync_get_state() == SYNC_AT_TIP);
        (void)sync_set_state(SYNC_IDLE, "test_cleanup");
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_fsm_no_self_loop(void)
{
    int failures = 0;
    TEST("sync_state FSM: same-state request is a no-op success") {
        reset_sync_state();
        ASSERT(sync_get_state() == SYNC_IDLE);

        bool ok = sync_set_state(SYNC_IDLE, "test self-loop");
        ASSERT(ok);
        ASSERT(sync_get_state() == SYNC_IDLE);
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_fsm_rejects_at_tip_to_blocks(void)
{
    int failures = 0;
    TEST("sync_state FSM: rejects SYNC_AT_TIP → SYNC_BLOCKS_DOWNLOAD (no edge)") {
        reset_sync_state();
        ASSERT(sync_set_state(SYNC_FINDING_PEERS,    "setup"));
        ASSERT(sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup"));
        ASSERT(sync_set_state(SYNC_AT_TIP,           "setup"));

        bool ok = sync_set_state(SYNC_BLOCKS_DOWNLOAD, "illegal");
        ASSERT(!ok);
        ASSERT(sync_get_state() == SYNC_AT_TIP);
        (void)sync_set_state(SYNC_IDLE, "test_cleanup");
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_state_name_covers_all_states(void)
{
    int failures = 0;
    TEST("sync_state_name() returns non-\"unknown\" for every defined state") {
        for (int s = 0; s < SYNC_NUM_STATES; s++) {
            const char *name = sync_state_name((enum sync_state)s);
            if (strcmp(name, "unknown") == 0) {
                printf("FAIL (state %d returned 'unknown')\n", s);
                failures++; goto _test_next;
            }
        }
        /* Out-of-range must return "unknown". */
        ASSERT(strcmp(sync_state_name((enum sync_state)SYNC_NUM_STATES), "unknown") == 0);
        ASSERT(strcmp(sync_state_name((enum sync_state)-1), "unknown") == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── snapshot_sync_state FSM ──────────────────────────── */

static void reset_snapsync_state(void)
{
    enum snapshot_sync_state cur = snapsync_get_state();
    if (cur == SNAPSYNC_IDLE) return;
    /* All non-idle snapsync states have a direct edge back to IDLE
     * (RECEIVING via "stall reset", COMPLETE/FAILED/NEGOTIATING
     * explicitly). */
    (void)snapsync_set_state(SNAPSYNC_IDLE, "test_reset");
}

static int test_snapsync_fsm_rejects_idle_to_complete(void)
{
    int failures = 0;
    TEST("snapsync FSM: rejects IDLE → COMPLETE (must transit VERIFYING)") {
        reset_snapsync_state();
        ASSERT(snapsync_get_state() == SNAPSYNC_IDLE);

        bool ok = snapsync_set_state(SNAPSYNC_COMPLETE, "illegal skip");
        ASSERT(!ok);
        ASSERT(snapsync_get_state() == SNAPSYNC_IDLE);
        PASS();
    } _test_next:;
    return failures;
}

static int test_snapsync_fsm_accepts_legal_chain(void)
{
    int failures = 0;
    TEST("snapsync FSM: IDLE → NEGOTIATING → RECEIVING → VERIFYING → COMPLETE → IDLE") {
        reset_snapsync_state();
        ASSERT(snapsync_set_state(SNAPSYNC_NEGOTIATING, "t1"));
        ASSERT(snapsync_set_state(SNAPSYNC_RECEIVING,   "t2"));
        ASSERT(snapsync_set_state(SNAPSYNC_VERIFYING,   "t3"));
        ASSERT(snapsync_set_state(SNAPSYNC_COMPLETE,    "t4"));
        ASSERT(snapsync_set_state(SNAPSYNC_IDLE,        "t5"));
        ASSERT(snapsync_get_state() == SNAPSYNC_IDLE);
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_state_fsm(void)
{
    int failures = 0;
    failures += test_sync_fsm_rejects_idle_to_at_tip();
    failures += test_sync_fsm_accepts_legal_chain();
    failures += test_sync_fsm_no_self_loop();
    failures += test_sync_fsm_rejects_at_tip_to_blocks();
    failures += test_sync_state_name_covers_all_states();
    failures += test_snapsync_fsm_rejects_idle_to_complete();
    failures += test_snapsync_fsm_accepts_legal_chain();
    return failures;
}
