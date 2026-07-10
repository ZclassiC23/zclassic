/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the MCP metrics baseline ring (tools/mcp/baseline.c):
 * labeled snapshot capture, exact-leaf diffing against the live
 * metrics render, ring overwrite, and the MCP handler error bodies.
 */

#include "test/test_helpers.h"
#include "mcp/baseline.h"
#include "mcp/metrics.h"
#include "mcp/router.h"
#include "mcp/controllers.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static bool contains(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

static void register_meta_only(void)
{
    mcp_router_reset();
    mcp_register_meta();
}

/* ── Module-level: capture, list, lookups ────────────────────── */

static int test_set_assigns_auto_label(void)
{
    int failures = 0;
    TEST("baseline: set() auto-labels when none given") {
        mcp_baseline_init();

        char label[MCP_BASELINE_LABEL_MAX];
        uint64_t ts = 0;
        ASSERT(mcp_baseline_set(NULL, label, sizeof(label), &ts));
        ASSERT(label[0] == 'b');
        ASSERT(ts > 0);
        ASSERT(mcp_baseline_count() == 1);
        ASSERT(mcp_baseline_find(label) >= 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_set_honors_explicit_label(void)
{
    int failures = 0;
    TEST("baseline: set() with an explicit label is findable by that label") {
        mcp_baseline_init();

        char label[MCP_BASELINE_LABEL_MAX];
        ASSERT(mcp_baseline_set("hourly-check", label, sizeof(label), NULL));
        ASSERT(strcmp(label, "hourly-check") == 0);

        int idx = mcp_baseline_find("hourly-check");
        ASSERT(idx >= 0);
        ASSERT(mcp_baseline_find("does-not-exist") == -1);
        ASSERT(mcp_baseline_latest() == idx);
        PASS();
    } _test_next:;
    return failures;
}

static int test_list_json_shape(void)
{
    int failures = 0;
    TEST("baseline: list_json enumerates label/timestamp/age/bytes") {
        mcp_baseline_init();
        mcp_baseline_set("first", NULL, 0, NULL);
        mcp_baseline_set("second", NULL, 0, NULL);

        char *out = mcp_baseline_list_json();
        ASSERT(out != NULL);
        ASSERT(contains(out, "\"count\":2"));
        ASSERT(contains(out, "\"ring_size\":16"));
        ASSERT(contains(out, "\"label\":\"first\""));
        ASSERT(contains(out, "\"label\":\"second\""));
        ASSERT(contains(out, "\"timestamp_us\""));
        ASSERT(contains(out, "\"age_seconds\""));
        ASSERT(contains(out, "\"bytes\""));
        free(out);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Diff: exact-leaf change detection ───────────────────────── */

static int test_diff_shows_exactly_the_mutated_leaf(void)
{
    int failures = 0;
    TEST("baseline: diff reports exactly the one counter that changed") {
        mcp_baseline_init();
        mcp_metrics_reset();

        char label[MCP_BASELINE_LABEL_MAX];
        ASSERT(mcp_baseline_set("before", label, sizeof(label), NULL));
        int idx = mcp_baseline_find("before");
        ASSERT(idx >= 0);

        /* Mutate exactly one Prometheus line: zcl_peer_bans_total is a
         * single independent gauge with no histogram / no cumulative
         * neighbors, so this is the cleanest "exactly one leaf changed"
         * mutation available via metrics.c's public test surface. */
        mcp_metrics_record_peer_ban();

        char *diff = mcp_baseline_diff_json(idx);
        ASSERT(diff != NULL);
        ASSERT(contains(diff, "\"label\":\"before\""));
        ASSERT(contains(diff, "\"changed_count\":1"));
        ASSERT(contains(diff, "\"metric\":\"zcl_peer_bans_total\""));
        ASSERT(contains(diff, "\"from\":0"));
        ASSERT(contains(diff, "\"to\":1"));
        ASSERT(contains(diff, "\"delta\":1"));
        /* Nothing unrelated should show up as changed. */
        ASSERT(!contains(diff, "\"metric\":\"zcl_mcp_requests_total"));
        free(diff);
        PASS();
    } _test_next:;
    return failures;
}

static int test_diff_empty_when_nothing_changed(void)
{
    int failures = 0;
    TEST("baseline: diff against an untouched live state is empty") {
        mcp_baseline_init();
        mcp_metrics_reset();

        int idx = -1;
        {
            char label[MCP_BASELINE_LABEL_MAX];
            ASSERT(mcp_baseline_set("steady", label, sizeof(label), NULL));
            idx = mcp_baseline_find("steady");
        }
        ASSERT(idx >= 0);

        char *diff = mcp_baseline_diff_json(idx);
        ASSERT(diff != NULL);
        ASSERT(contains(diff, "\"changed_count\":0"));
        ASSERT(contains(diff, "\"changed\":[]"));
        free(diff);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Ring overwrite ───────────────────────────────────────────── */

static int test_ring_overwrite_evicts_oldest(void)
{
    int failures = 0;
    TEST("baseline: capturing past the ring size evicts the oldest labels") {
        mcp_baseline_init();

        /* Ring size is 16; auto-labels are "b1".."b20" since
         * mcp_baseline_init() resets the sequence counter to 0. */
        for (int i = 0; i < 20; i++)
            ASSERT(mcp_baseline_set(NULL, NULL, 0, NULL));

        ASSERT(mcp_baseline_count() == MCP_BASELINE_RING_SIZE);
        /* First 4 captures were pushed out of the 16-slot ring. */
        ASSERT(mcp_baseline_find("b1") == -1);
        ASSERT(mcp_baseline_find("b4") == -1);
        /* The most recent MCP_BASELINE_RING_SIZE labels are still live. */
        ASSERT(mcp_baseline_find("b5") >= 0);
        ASSERT(mcp_baseline_find("b20") >= 0);
        ASSERT(mcp_baseline_latest() == mcp_baseline_find("b20"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_ring_reused_label_resolves_to_newest(void)
{
    int failures = 0;
    TEST("baseline: a re-used label resolves to the newest capture") {
        mcp_baseline_init();
        mcp_metrics_reset();

        char label[MCP_BASELINE_LABEL_MAX];
        int idx_old, idx_new;

        ASSERT(mcp_baseline_set("dup", label, sizeof(label), NULL));
        idx_old = mcp_baseline_find("dup");
        mcp_metrics_record_peer_ban(); /* state advances between captures */
        ASSERT(mcp_baseline_set("dup", label, sizeof(label), NULL));
        idx_new = mcp_baseline_find("dup");

        ASSERT(idx_new == mcp_baseline_latest());
        /* Diffing the resolved (newest) "dup" baseline against the
         * still-unchanged live state must show no drift. */
        char *diff = mcp_baseline_diff_json(idx_new);
        ASSERT(diff != NULL);
        ASSERT(contains(diff, "\"changed_count\":0"));
        free(diff);
        (void)idx_old;
        PASS();
    } _test_next:;
    return failures;
}

/* ── Bounds / invalid input ──────────────────────────────────── */

static int test_diff_invalid_index_returns_null(void)
{
    int failures = 0;
    TEST("baseline: diff_json rejects out-of-range and empty slots") {
        mcp_baseline_init();
        ASSERT(mcp_baseline_diff_json(-1) == NULL);
        ASSERT(mcp_baseline_diff_json(MCP_BASELINE_RING_SIZE) == NULL);
        ASSERT(mcp_baseline_diff_json(0) == NULL); /* empty ring: slot 0 unused */
        PASS();
    } _test_next:;
    return failures;
}

/* ── Handler-level: MCP tool error bodies ────────────────────── */

static int test_handler_diff_unknown_label_error_body(void)
{
    int failures = 0;
    TEST("zcl_metrics_baseline_diff: unknown label sets an error body") {
        register_meta_only();
        mcp_baseline_init();

        struct json_value args = {0};
        const char *src = "{\"label\":\"totally-made-up-label\"}";
        ASSERT(json_read(&args, src, strlen(src)));

        char *body = mcp_router_dispatch("zcl_metrics_baseline_diff", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"error\""));
        ASSERT(contains(body, "HANDLER_FAILED"));
        ASSERT(contains(body, "totally-made-up-label"));
        free(body);
        json_free(&args);
        PASS();
    } _test_next:;
    return failures;
}

static int test_handler_diff_empty_ring_error_body(void)
{
    int failures = 0;
    TEST("zcl_metrics_baseline_diff: empty ring (no args) sets an error body") {
        register_meta_only();
        mcp_baseline_init();

        char *body = mcp_router_dispatch("zcl_metrics_baseline_diff", NULL);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"error\""));
        ASSERT(contains(body, "no baselines recorded yet"));
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

static int test_handler_set_then_list_round_trip(void)
{
    int failures = 0;
    TEST("zcl_metrics_baseline_set/_list round-trip through the router") {
        register_meta_only();
        mcp_baseline_init();

        char *set_body = mcp_router_dispatch("zcl_metrics_baseline_set", NULL);
        ASSERT(set_body != NULL);
        ASSERT(contains(set_body, "\"ok\":true"));
        ASSERT(contains(set_body, "\"label\":\"b1\""));
        free(set_body);

        char *list_body = mcp_router_dispatch("zcl_metrics_baseline_list", NULL);
        ASSERT(list_body != NULL);
        ASSERT(contains(list_body, "\"count\":1"));
        ASSERT(contains(list_body, "\"label\":\"b1\""));
        free(list_body);
        PASS();
    } _test_next:;
    return failures;
}

static int test_handler_diff_by_label_through_router(void)
{
    int failures = 0;
    TEST("zcl_metrics_baseline_diff resolves by label through the router") {
        register_meta_only();
        mcp_baseline_init();
        mcp_metrics_reset();

        struct json_value set_args = {0};
        const char *set_src = "{\"label\":\"rt\"}";
        ASSERT(json_read(&set_args, set_src, strlen(set_src)));
        char *set_body = mcp_router_dispatch("zcl_metrics_baseline_set", &set_args);
        ASSERT(set_body != NULL);
        free(set_body);
        json_free(&set_args);

        mcp_metrics_record_peer_ban();

        struct json_value diff_args = {0};
        const char *diff_src = "{\"label\":\"rt\"}";
        ASSERT(json_read(&diff_args, diff_src, strlen(diff_src)));
        char *diff_body = mcp_router_dispatch("zcl_metrics_baseline_diff", &diff_args);
        ASSERT(diff_body != NULL);
        ASSERT(contains(diff_body, "\"changed_count\":1"));
        ASSERT(contains(diff_body, "\"metric\":\"zcl_peer_bans_total\""));
        free(diff_body);
        json_free(&diff_args);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_mcp_baseline(void);

int test_mcp_baseline(void)
{
    int failures = 0;

    failures += test_set_assigns_auto_label();
    failures += test_set_honors_explicit_label();
    failures += test_list_json_shape();

    failures += test_diff_shows_exactly_the_mutated_leaf();
    failures += test_diff_empty_when_nothing_changed();

    failures += test_ring_overwrite_evicts_oldest();
    failures += test_ring_reused_label_resolves_to_newest();

    failures += test_diff_invalid_index_returns_null();

    failures += test_handler_diff_unknown_label_error_body();
    failures += test_handler_diff_empty_ring_error_body();
    failures += test_handler_set_then_list_round_trip();
    failures += test_handler_diff_by_label_through_router();

    mcp_baseline_init();
    mcp_metrics_reset();
    mcp_router_reset();
    return failures;
}
