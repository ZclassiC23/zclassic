/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the MCP event push out-channel (tools/mcp/mcp_notify.c):
 * an EV_* event in an eventlog snapshot becomes an MCP
 * notifications/message frame, keyed by seq so none are duplicated or
 * fabricated. Runs without a live node — the eventlog snapshot is a
 * fixed JSON literal and the wire sink is a capture buffer. */

#include "test/test_helpers.h"
#include "mcp/mcp_notify.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Capture sink ───────────────────────────────────────────────
 * Records every notification frame the module would push on stdout. */

#define CAP_MAX 32
struct capture {
    char  frames[CAP_MAX][2048];
    int   count;
};

static void capture_sink(const char *json_line, void *ctx)
{
    struct capture *c = ctx;
    if (c->count < CAP_MAX) {
        snprintf(c->frames[c->count], sizeof(c->frames[0]), "%s", json_line);
        c->count++;
    }
}

static bool has(const char *buf, const char *needle)
{
    return strstr(buf, needle) != NULL;
}

/* ── Filter: which event types are operator-class ───────────────── */

static int test_notify_operator_filter(void)
{
    int failures = 0;
    TEST("operator-class filter accepts the loud events, rejects chatter") {
        ASSERT(mcp_notify_is_operator_event("condition.operator_needed"));
        ASSERT(mcp_notify_is_operator_event("oracle.chain_halted"));
        ASSERT(mcp_notify_is_operator_event("oracle.fork_suspected"));
        ASSERT(mcp_notify_is_operator_event("mirror.lag_slo_breach"));
        ASSERT(mcp_notify_is_operator_event("boot.validation_failed"));
        /* Routine high-volume events must NOT push. */
        ASSERT(!mcp_notify_is_operator_event("net.tcp_connected"));
        ASSERT(!mcp_notify_is_operator_event("val.block_connected"));
        ASSERT(!mcp_notify_is_operator_event(NULL));
        ASSERT(!mcp_notify_is_operator_event(""));
        PASS();
    } _test_next:;
    return failures;
}

/* ── Frame builder: well-formed JSON-RPC notification ───────────── */

static int test_notify_frame_shape(void)
{
    int failures = 0;
    TEST("built frame is a valid notifications/message with event payload") {
        char frame[1024];
        size_t n = mcp_notify_build_frame(frame, sizeof(frame),
                                          "oracle.chain_halted", 4242,
                                          "reason=fork distinct_heights=3",
                                          1700000000000000LL, 7);
        ASSERT(n > 0);
        /* JSON-RPC notification: method present, no id. */
        ASSERT(has(frame, "\"method\":\"notifications/message\""));
        ASSERT(!has(frame, "\"id\":"));
        /* MCP logging shape + the structured event. */
        ASSERT(has(frame, "\"level\":\"error\""));   /* halt is an error */
        ASSERT(has(frame, "\"type\":\"oracle.chain_halted\""));
        ASSERT(has(frame, "\"seq\":4242"));
        ASSERT(has(frame, "\"peer\":7"));
        ASSERT(has(frame, "distinct_heights=3"));
        /* It must parse as JSON. */
        struct json_value v;
        ASSERT(json_read(&v, frame, strlen(frame)));
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_notify_frame_warning_level(void)
{
    int failures = 0;
    TEST("a peer-floor breach is a warning, not an error") {
        char frame[1024];
        size_t n = mcp_notify_build_frame(frame, sizeof(frame),
                                          "peer.floor_breach", 9,
                                          "healthy=1 min=3 since=120s", 0, 0);
        ASSERT(n > 0);
        ASSERT(has(frame, "\"level\":\"warning\""));
        PASS();
    } _test_next:;
    return failures;
}

static int test_notify_frame_escapes_data(void)
{
    int failures = 0;
    TEST("frame escapes quotes/backslashes in event data") {
        char frame[1024];
        size_t n = mcp_notify_build_frame(frame, sizeof(frame),
                                          "oracle.disagree", 1,
                                          "their=\"a\\b\"", 0, 0);
        ASSERT(n > 0);
        ASSERT(has(frame, "\\\"a\\\\b\\\""));   /* properly escaped */
        struct json_value v;
        ASSERT(json_read(&v, frame, strlen(frame)));
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Snapshot path: prime, push, dedup ──────────────────────────── */

/* Two operator events (seq 100, 102) bracketing a routine event (101). */
static const char *const k_snap1 =
    "{\"sync_state\":\"active\",\"events\":["
    "{\"seq\":100,\"ts\":1,\"type\":\"oracle.chain_halted\","
        "\"peer\":0,\"data\":\"reason=fork\"},"
    "{\"seq\":101,\"ts\":2,\"type\":\"val.block_connected\","
        "\"peer\":0,\"data\":\"height=42\"},"
    "{\"seq\":102,\"ts\":3,\"type\":\"mirror.lag_slo_breach\","
        "\"peer\":0,\"data\":\"lag=600 severity=critical\"}]}";

static int test_notify_primes_then_pushes(void)
{
    int failures = 0;
    TEST("first snapshot primes the watermark (no replay of history)") {
        struct capture cap = {0};
        mcp_notify_reset(-1);   /* unprimed: anchor to the live tip */
        int emitted = mcp_notify_consider_snapshot(k_snap1, capture_sink, &cap);
        /* On connect the agent must NOT be flooded with the backlog. */
        ASSERT(emitted == 0);
        ASSERT(cap.count == 0);
        ASSERT(mcp_notify_total_emitted() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_notify_pushes_new_operator_events(void)
{
    int failures = 0;
    TEST("after priming, new operator events push (routine ones do not)") {
        struct capture cap = {0};
        /* Prime so only seq > 99 is considered new. */
        mcp_notify_reset(99);
        int emitted = mcp_notify_consider_snapshot(k_snap1, capture_sink, &cap);
        /* seq 100 (halt) + 102 (slo) push; seq 101 (block) is filtered. */
        ASSERT(emitted == 2);
        ASSERT(cap.count == 2);
        ASSERT(has(cap.frames[0], "oracle.chain_halted"));
        ASSERT(has(cap.frames[1], "mirror.lag_slo_breach"));
        ASSERT(!has(cap.frames[0], "val.block_connected"));
        ASSERT(mcp_notify_total_emitted() == 2);
        PASS();
    } _test_next:;
    return failures;
}

static int test_notify_dedups_on_reseen_snapshot(void)
{
    int failures = 0;
    TEST("re-seeing the same snapshot pushes nothing (seq watermark)") {
        struct capture cap = {0};
        mcp_notify_reset(99);
        mcp_notify_consider_snapshot(k_snap1, capture_sink, &cap);
        ASSERT(cap.count == 2);

        /* Same snapshot again — every seq is <= the watermark now. */
        struct capture cap2 = {0};
        int again = mcp_notify_consider_snapshot(k_snap1, capture_sink, &cap2);
        ASSERT(again == 0);
        ASSERT(cap2.count == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_notify_pushes_only_the_delta(void)
{
    int failures = 0;
    TEST("a later snapshot pushes only the newly-arrived operator event") {
        struct capture cap = {0};
        mcp_notify_reset(99);
        mcp_notify_consider_snapshot(k_snap1, capture_sink, &cap);
        ASSERT(cap.count == 2);   /* seq 100, 102 */

        /* Next poll: ring advanced; seq 103 is a new operator event,
         * 100/102 are still in the window but already pushed. */
        const char *snap2 =
            "{\"events\":["
            "{\"seq\":102,\"ts\":3,\"type\":\"mirror.lag_slo_breach\","
                "\"peer\":0,\"data\":\"lag=600\"},"
            "{\"seq\":103,\"ts\":4,\"type\":\"condition.operator_needed\","
                "\"peer\":0,\"data\":\"condition=stall attempts=3\"}]}";
        struct capture cap2 = {0};
        int delta = mcp_notify_consider_snapshot(snap2, capture_sink, &cap2);
        ASSERT(delta == 1);
        ASSERT(cap2.count == 1);
        ASSERT(has(cap2.frames[0], "condition.operator_needed"));
        ASSERT(has(cap2.frames[0], "\"seq\":103"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_notify_ignores_garbage_snapshot(void)
{
    int failures = 0;
    TEST("malformed / empty snapshots are safely ignored") {
        struct capture cap = {0};
        mcp_notify_reset(0);
        ASSERT(mcp_notify_consider_snapshot(NULL, capture_sink, &cap) == 0);
        ASSERT(mcp_notify_consider_snapshot("not json", capture_sink, &cap) == 0);
        ASSERT(mcp_notify_consider_snapshot("{\"events\":42}",
                                            capture_sink, &cap) == 0);
        ASSERT(mcp_notify_consider_snapshot("{}", capture_sink, &cap) == 0);
        ASSERT(cap.count == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_mcp_notify(void)
{
    int failures = 0;

    failures += test_notify_operator_filter();
    failures += test_notify_frame_shape();
    failures += test_notify_frame_warning_level();
    failures += test_notify_frame_escapes_data();
    failures += test_notify_primes_then_pushes();
    failures += test_notify_pushes_new_operator_events();
    failures += test_notify_dedups_on_reseen_snapshot();
    failures += test_notify_pushes_only_the_delta();
    failures += test_notify_ignores_garbage_snapshot();

    return failures;
}
