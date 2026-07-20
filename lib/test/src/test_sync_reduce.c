/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer (sync/sync_reduce.h) — the explicit transition matrix.
 * Every (phase, event) row (with a proof_ok split on the one proof-gated arm)
 * is pinned to its expected (next phase, ordered actions, blocker). If the
 * kernel's table drifts, exactly the drifted row fails and names itself. */

#include "test/test_helpers.h"
#include "sync/sync_reduce.h"
#include <string.h>

/* One expected row. `acts`/`n_acts` are the meaningful actions in order;
 * `proof_ok` is the event payload (only the VERIFYING+PROOF_VERIFIED arm reads
 * it, but carrying it everywhere keeps the table uniform). */
struct row {
    enum sync_phase       phase;
    enum sync_event_kind  event;
    bool                  proof_ok;
    enum sync_phase       next;
    enum sync_action      acts[SYNC_DECISION_MAX_ACTIONS];
    int                   n_acts;
    enum sync_blocker     blocker;
};

#define A0 {SYNC_ACTION_NONE, SYNC_ACTION_NONE, SYNC_ACTION_NONE, SYNC_ACTION_NONE}

static const struct row g_matrix[] = {
    /* ── IDLE ── */
    {SYNC_PHASE_IDLE, SYNC_EVENT_START,            false, SYNC_PHASE_NEGOTIATING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_OFFER_RECEIVED,   false, SYNC_PHASE_NEGOTIATING, {SYNC_ACTION_STORE_OFFER}, 1, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_OFFER_ACCEPTED,   false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_CHUNK_RECEIVED,   false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_CHUNK_REJECTED,   false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_RECEIVE_COMPLETE, false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_PROOF_VERIFIED,   true,  SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_PROOF_FAILED,     false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_PEER_LOST,        false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_TIMEOUT,          false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_IDLE, SYNC_EVENT_STOP_REQUESTED,   false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},

    /* ── NEGOTIATING ── */
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_START,            false, SYNC_PHASE_NEGOTIATING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_OFFER_RECEIVED,   false, SYNC_PHASE_NEGOTIATING, {SYNC_ACTION_STORE_OFFER}, 1, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_OFFER_ACCEPTED,   false, SYNC_PHASE_RECEIVING,   {SYNC_ACTION_RESET_OFFSET, SYNC_ACTION_BEGIN_RECEIVE}, 2, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_CHUNK_RECEIVED,   false, SYNC_PHASE_NEGOTIATING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_CHUNK_REJECTED,   false, SYNC_PHASE_NEGOTIATING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_RECEIVE_COMPLETE, false, SYNC_PHASE_NEGOTIATING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_PROOF_VERIFIED,   true,  SYNC_PHASE_NEGOTIATING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_PROOF_FAILED,     false, SYNC_PHASE_FAILED,      {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_PROOF_FAILED},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_PEER_LOST,        false, SYNC_PHASE_FAILED,      {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_PEER_LOST},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_TIMEOUT,          false, SYNC_PHASE_FAILED,      {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_TIMEOUT},
    {SYNC_PHASE_NEGOTIATING, SYNC_EVENT_STOP_REQUESTED,   false, SYNC_PHASE_IDLE,        A0, 0, SYNC_BLOCKER_NONE},

    /* ── RECEIVING ── */
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_START,            false, SYNC_PHASE_RECEIVING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_OFFER_RECEIVED,   false, SYNC_PHASE_RECEIVING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_OFFER_ACCEPTED,   false, SYNC_PHASE_RECEIVING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_CHUNK_RECEIVED,   false, SYNC_PHASE_RECEIVING, {SYNC_ACTION_APPLY_CHUNK}, 1, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_CHUNK_REJECTED,   false, SYNC_PHASE_RECEIVING, {SYNC_ACTION_PENALIZE_PEER}, 1, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_RECEIVE_COMPLETE, false, SYNC_PHASE_VERIFYING, {SYNC_ACTION_START_VERIFY}, 1, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_PROOF_VERIFIED,   true,  SYNC_PHASE_RECEIVING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_PROOF_FAILED,     false, SYNC_PHASE_FAILED,    {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_PROOF_FAILED},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_PEER_LOST,        false, SYNC_PHASE_FAILED,    {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_PEER_LOST},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_TIMEOUT,          false, SYNC_PHASE_FAILED,    {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_TIMEOUT},
    {SYNC_PHASE_RECEIVING, SYNC_EVENT_STOP_REQUESTED,   false, SYNC_PHASE_IDLE,      A0, 0, SYNC_BLOCKER_NONE},

    /* ── VERIFYING (the proof gate) ── */
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_START,            false, SYNC_PHASE_VERIFYING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_OFFER_RECEIVED,   false, SYNC_PHASE_VERIFYING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_OFFER_ACCEPTED,   false, SYNC_PHASE_VERIFYING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_CHUNK_RECEIVED,   false, SYNC_PHASE_VERIFYING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_CHUNK_REJECTED,   false, SYNC_PHASE_VERIFYING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_RECEIVE_COMPLETE, false, SYNC_PHASE_VERIFYING, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_PROOF_VERIFIED,   true,  SYNC_PHASE_STAGED,    {SYNC_ACTION_STAGE_BUNDLE}, 1, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_PROOF_VERIFIED,   false, SYNC_PHASE_FAILED,    {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_PROOF_FAILED},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_PROOF_FAILED,     false, SYNC_PHASE_FAILED,    {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_PROOF_FAILED},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_PEER_LOST,        false, SYNC_PHASE_FAILED,    {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_PEER_LOST},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_TIMEOUT,          false, SYNC_PHASE_FAILED,    {SYNC_ACTION_FAIL}, 1, SYNC_BLOCKER_TIMEOUT},
    {SYNC_PHASE_VERIFYING, SYNC_EVENT_STOP_REQUESTED,   false, SYNC_PHASE_IDLE,      A0, 0, SYNC_BLOCKER_NONE},

    /* ── STAGED (progress ⇒ contained) ── */
    {SYNC_PHASE_STAGED, SYNC_EVENT_START,            false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_STAGED, SYNC_EVENT_OFFER_RECEIVED,   false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_STAGED, SYNC_EVENT_OFFER_ACCEPTED,   false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_STAGED, SYNC_EVENT_CHUNK_RECEIVED,   false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_STAGED, SYNC_EVENT_RECEIVE_COMPLETE, false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_STAGED, SYNC_EVENT_PROOF_VERIFIED,   true,  SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_STAGED, SYNC_EVENT_CHUNK_REJECTED,   false, SYNC_PHASE_STAGED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_STAGED, SYNC_EVENT_PROOF_FAILED,     false, SYNC_PHASE_STAGED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_STAGED, SYNC_EVENT_PEER_LOST,        false, SYNC_PHASE_STAGED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_STAGED, SYNC_EVENT_TIMEOUT,          false, SYNC_PHASE_STAGED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_STAGED, SYNC_EVENT_STOP_REQUESTED,   false, SYNC_PHASE_IDLE,   A0, 0, SYNC_BLOCKER_NONE},

    /* ── ACTIVATION_CONTAINED (holds; progress re-raises; stop resets) ── */
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_START,            false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_OFFER_RECEIVED,   false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_OFFER_ACCEPTED,   false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_CHUNK_RECEIVED,   false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_RECEIVE_COMPLETE, false, SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_PROOF_VERIFIED,   true,  SYNC_PHASE_ACTIVATION_CONTAINED, {SYNC_ACTION_RAISE_CONTAINMENT_BLOCKER}, 1, SYNC_BLOCKER_ACTIVATION_CONTAINED},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_CHUNK_REJECTED,   false, SYNC_PHASE_ACTIVATION_CONTAINED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_PROOF_FAILED,     false, SYNC_PHASE_ACTIVATION_CONTAINED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_PEER_LOST,        false, SYNC_PHASE_ACTIVATION_CONTAINED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_TIMEOUT,          false, SYNC_PHASE_ACTIVATION_CONTAINED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_ACTIVATION_CONTAINED, SYNC_EVENT_STOP_REQUESTED,   false, SYNC_PHASE_IDLE,                 A0, 0, SYNC_BLOCKER_NONE},

    /* ── FAILED (terminal; stop resets) ── */
    {SYNC_PHASE_FAILED, SYNC_EVENT_START,            false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_OFFER_RECEIVED,   false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_OFFER_ACCEPTED,   false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_CHUNK_RECEIVED,   false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_CHUNK_REJECTED,   false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_RECEIVE_COMPLETE, false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_PROOF_VERIFIED,   true,  SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_PROOF_FAILED,     false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_PEER_LOST,        false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_TIMEOUT,          false, SYNC_PHASE_FAILED, A0, 0, SYNC_BLOCKER_NONE},
    {SYNC_PHASE_FAILED, SYNC_EVENT_STOP_REQUESTED,   false, SYNC_PHASE_IDLE,   A0, 0, SYNC_BLOCKER_NONE},
};

static struct sync_kernel_state state_of(uint64_t sid, enum sync_phase p)
{
    struct sync_kernel_state s;
    memset(&s, 0, sizeof(s));
    s.session_id = sid;
    s.phase = p;
    return s;
}

static struct sync_event event_of(uint64_t sid, enum sync_event_kind k, bool proof_ok)
{
    struct sync_event e;
    memset(&e, 0, sizeof(e));
    e.session_id = sid;
    e.kind = k;
    e.proof_ok = proof_ok;
    return e;
}

/* ── The matrix drives the primary test ── */

static int test_sync_reduce_matrix(void)
{
    int failures = 0;
    const int rows = (int)(sizeof(g_matrix) / sizeof(g_matrix[0]));

    for (int r = 0; r < rows; r++) {
        const struct row *row = &g_matrix[r];
        TEST("sync_reduce matrix") {
            struct sync_decision d =
                sync_reduce(state_of(42, row->phase),
                            event_of(42, row->event, row->proof_ok));

            if (d.next != row->next || d.action_count != row->n_acts ||
                d.blocker != row->blocker ||
                d.has_blocker != (row->blocker != SYNC_BLOCKER_NONE)) {
                printf("\n  ROW %d: %s + %s (proof_ok=%d): "
                       "got next=%s ac=%d blk=%d, want next=%s ac=%d blk=%d\n",
                       r, sync_phase_name(row->phase),
                       sync_event_name(row->event), (int)row->proof_ok,
                       sync_phase_name(d.next), d.action_count, (int)d.blocker,
                       sync_phase_name(row->next), row->n_acts, (int)row->blocker);
            }
            ASSERT(d.next == row->next);
            ASSERT(d.action_count == row->n_acts);
            ASSERT(d.blocker == row->blocker);
            ASSERT(d.has_blocker == (row->blocker != SYNC_BLOCKER_NONE));
            for (int i = 0; i < row->n_acts; i++)
                ASSERT(d.actions[i] == row->acts[i]);
            /* The action tail past action_count is always SYNC_ACTION_NONE. */
            for (int i = row->n_acts; i < SYNC_DECISION_MAX_ACTIONS; i++)
                ASSERT(d.actions[i] == SYNC_ACTION_NONE);
            PASS();
        } _test_next:;
    }
    return failures;
}

/* The matrix must cover every (phase,event) pair exactly once — plus the one
 * extra VERIFYING+PROOF_VERIFIED proof_ok split. This prevents a silently
 * missing row from hiding a broken arm. */
static int test_sync_reduce_matrix_is_total(void)
{
    int failures = 0;
    TEST("sync_reduce: transition matrix covers every (phase,event)") {
        const int rows = (int)(sizeof(g_matrix) / sizeof(g_matrix[0]));
        /* Expected count: 7 phases × 11 events + 1 proof_ok split. */
        ASSERT(rows == SYNC_PHASE_COUNT * SYNC_EVENT_COUNT + 1);
        int seen[SYNC_PHASE_COUNT][SYNC_EVENT_COUNT];
        memset(seen, 0, sizeof(seen));
        for (int r = 0; r < rows; r++)
            seen[g_matrix[r].phase][g_matrix[r].event]++;
        for (int p = 0; p < SYNC_PHASE_COUNT; p++)
            for (int e = 0; e < SYNC_EVENT_COUNT; e++)
                ASSERT(seen[p][e] >= 1);
        PASS();
    } _test_next:;
    return failures;
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
        ASSERT(strcmp(sync_phase_name((enum sync_phase)SYNC_PHASE_COUNT), "?") == 0);
        ASSERT(strcmp(sync_event_name((enum sync_event_kind)SYNC_EVENT_COUNT), "?") == 0);
        ASSERT(strcmp(sync_action_name((enum sync_action)SYNC_ACTION_COUNT), "?") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_sync_reduce_deterministic(void)
{
    int failures = 0;
    TEST("sync_reduce: same (state,event) yields a byte-identical decision") {
        struct sync_kernel_state s = state_of(7, SYNC_PHASE_RECEIVING);
        struct sync_event e = event_of(7, SYNC_EVENT_CHUNK_RECEIVED, false);
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
        struct sync_kernel_state s = state_of(9, SYNC_PHASE_VERIFYING);
        struct sync_event e = event_of(1234, SYNC_EVENT_PROOF_VERIFIED, true);
        struct sync_decision d = sync_reduce(s, e);
        ASSERT(d.next == s.phase);
        ASSERT(d.action_count == 0);
        ASSERT(!d.has_blocker);
        PASS();
    } _test_next:;
    return failures;
}

/* A zero session_id (a fresh IDLE with no session) must NOT gate — the first
 * offer/start still folds through. */
static int test_sync_reduce_zero_session_not_stale(void)
{
    int failures = 0;
    TEST("sync_reduce: zero state session does not gate the opening event") {
        struct sync_kernel_state s = state_of(0, SYNC_PHASE_IDLE);
        struct sync_event e = event_of(555, SYNC_EVENT_OFFER_RECEIVED, false);
        struct sync_decision d = sync_reduce(s, e);
        ASSERT(d.next == SYNC_PHASE_NEGOTIATING);
        ASSERT(d.action_count == 1);
        ASSERT(d.actions[0] == SYNC_ACTION_STORE_OFFER);
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_reduce(void)
{
    int failures = 0;
    failures += test_sync_reduce_names();
    failures += test_sync_reduce_matrix();
    failures += test_sync_reduce_matrix_is_total();
    failures += test_sync_reduce_deterministic();
    failures += test_sync_reduce_stale_session();
    failures += test_sync_reduce_zero_session_not_stale();
    return failures;
}
