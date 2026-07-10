/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the metric-threshold alert rules (C3):
 * tools/mcp/metrics.c's mcp_metrics_evaluate_alert_rules() and the
 * mcp_notify.c allow-list entry ("condition.detected") that lets its
 * output flow through the existing MCP push channel.
 *
 * Hermetic: no live node, no real MCP client. Gauges are seeded
 * directly via the public setters in tools/mcp/metrics.h; the emitted
 * EV_CONDITION_DETECTED events are captured with a local sync
 * event_observe() the same way lib/test/src/test_recovery_policy.c
 * captures EV_RECOVERY_POLICY_*.
 */

#include "test/test_helpers.h"
#include "mcp/metrics.h"
#include "mcp/mcp_notify.h"
#include "event/event.h"
#include "util/blocker.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ── Event capture (sync observer on EV_CONDITION_DETECTED) ────── */

#define MA_CAP_MAX 32

static _Atomic int g_ma_count;
static char        g_ma_payloads[MA_CAP_MAX][256];
static _Atomic int g_ma_write_pos;

static void ma_observer(enum event_type type, uint32_t peer_id,
                         const void *payload, uint32_t payload_len,
                         void *ctx)
{
    (void)peer_id; (void)ctx;
    if (type != EV_CONDITION_DETECTED) return;
    atomic_fetch_add(&g_ma_count, 1);

    int pos = atomic_fetch_add(&g_ma_write_pos, 1) % MA_CAP_MAX;
    size_t n = payload_len < sizeof(g_ma_payloads[0]) - 1
                   ? payload_len : sizeof(g_ma_payloads[0]) - 1;
    memcpy(g_ma_payloads[pos], payload, n);
    g_ma_payloads[pos][n] = '\0';
}

static void ma_install_observer(void)
{
    event_clear_observers(EV_CONDITION_DETECTED);
    atomic_store(&g_ma_count, 0);
    atomic_store(&g_ma_write_pos, 0);
    memset(g_ma_payloads, 0, sizeof(g_ma_payloads));
    event_observe(EV_CONDITION_DETECTED, ma_observer, NULL);
}

/* True if any captured payload since the last reset contains `needle`. */
static bool ma_captured_contains(const char *needle)
{
    int written = atomic_load(&g_ma_write_pos);
    int n = written < MA_CAP_MAX ? written : MA_CAP_MAX;
    for (int i = 0; i < n; i++)
        if (strstr(g_ma_payloads[i], needle)) return true;
    return false;
}

static void ma_reset_all(void)
{
    mcp_metrics_alerts_reset();
    atomic_store(&g_ma_count, 0);
    atomic_store(&g_ma_write_pos, 0);
    memset(g_ma_payloads, 0, sizeof(g_ma_payloads));
}

/* ── Rule table shape ────────────────────────────────────────── */

static int test_rule_count_and_names(void)
{
    int failures = 0;
    TEST("metric_alerts: seeds the expected rule set") {
        ma_reset_all();
        ASSERT(mcp_metrics_alert_rule_count() == 5);
        /* Every seeded rule name is queryable (starts at 0 fires). */
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 0);
        ASSERT(mcp_metrics_alert_fire_count("mirror_lag_high") == 0);
        ASSERT(mcp_metrics_alert_fire_count("mirror_lag_critical") == 0);
        ASSERT(mcp_metrics_alert_fire_count("blocker_permanent_active") == 0);
        ASSERT(mcp_metrics_alert_fire_count("rss_high") == 0);
        /* An unknown rule name is simply absent, not an error. */
        ASSERT(mcp_metrics_alert_fire_count("no_such_rule") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_allow_listed_for_push(void)
{
    int failures = 0;
    TEST("metric_alerts: condition.detected is in the mcp_notify push allow-list") {
        ASSERT(mcp_notify_is_operator_event("condition.detected"));
        /* Sanity: an unrelated, non-operator event type is still excluded. */
        ASSERT(!mcp_notify_is_operator_event("chain.tip_updated"));
        PASS();
    } _test_next:;
    return failures;
}

/* ── Edge-trigger + cooldown semantics (tip_advance_age rule) ───── */

static int test_fires_exactly_once_on_crossing(void)
{
    int failures = 0;
    TEST("metric_alerts: tip_advance_age fires exactly once on the rising edge") {
        ma_reset_all();
        ma_install_observer();

        /* Below threshold (default 600s): no fire. */
        mcp_metrics_set_tip_advance_age(30);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(atomic_load(&g_ma_count) == 0);
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 0);

        /* Crosses above threshold: fires once. */
        mcp_metrics_set_tip_advance_age(900);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(atomic_load(&g_ma_count) == 1);
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 1);
        ASSERT(ma_captured_contains("name=metric_alert.tip_stalled"));
        ASSERT(ma_captured_contains("severity=critical"));

        /* Still crossed, cooldown (300s) has not elapsed: no repeat. */
        mcp_metrics_evaluate_alert_rules();
        mcp_metrics_evaluate_alert_rules();
        ASSERT(atomic_load(&g_ma_count) == 1);
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_not_raised_below_threshold(void)
{
    int failures = 0;
    TEST("metric_alerts: never fires while the gauge stays under threshold") {
        ma_reset_all();
        ma_install_observer();

        for (int i = 0; i < 5; i++) {
            mcp_metrics_set_tip_advance_age(i * 10);
            mcp_metrics_evaluate_alert_rules();
        }
        ASSERT(atomic_load(&g_ma_count) == 0);
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_refires_after_clearing_and_recrossing(void)
{
    int failures = 0;
    TEST("metric_alerts: clearing the latch lets a fresh crossing fire again") {
        ma_reset_all();
        ma_install_observer();

        mcp_metrics_set_tip_advance_age(900);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 1);

        /* Drops back under threshold: clears the edge latch. */
        mcp_metrics_set_tip_advance_age(5);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 1); /* no fire while clear */

        /* Crosses again: a new rising edge, fires immediately (cooldown
         * gates repeats of a CONTINUOUS breach, not a fresh episode). */
        mcp_metrics_set_tip_advance_age(900);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 2);
        PASS();
    } _test_next:;
    return failures;
}

/* Pre-bootstrap sentinel (-1) must never spuriously cross a GT rule. */
static int test_sentinel_value_does_not_fire(void)
{
    int failures = 0;
    TEST("metric_alerts: the -1 'not yet observed' sentinel never crosses a GT rule") {
        ma_reset_all();
        ma_install_observer();

        mcp_metrics_set_tip_advance_age(-1);
        mcp_metrics_set_mirror_lag(-1, 0, 0);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(atomic_load(&g_ma_count) == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Other seeded rules: each fires with the right name/severity ── */

static int test_mirror_lag_high_rule(void)
{
    int failures = 0;
    TEST("metric_alerts: mirror_lag_high fires above its threshold") {
        ma_reset_all();
        ma_install_observer();

        mcp_metrics_set_mirror_lag(5, 0, 0);   /* under default 50 */
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("mirror_lag_high") == 0);

        mcp_metrics_set_mirror_lag(120, 0, 0); /* over default 50 */
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("mirror_lag_high") == 1);
        ASSERT(ma_captured_contains("name=metric_alert.mirror_lag_high"));
        ASSERT(ma_captured_contains("severity=warning"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_mirror_lag_critical_rule(void)
{
    int failures = 0;
    TEST("metric_alerts: mirror_lag_critical_seconds fires the instant it is nonzero") {
        ma_reset_all();
        ma_install_observer();

        mcp_metrics_set_mirror_lag(0, 0, 0);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("mirror_lag_critical") == 0);

        mcp_metrics_set_mirror_lag(0, 30, 5);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("mirror_lag_critical") == 1);
        ASSERT(ma_captured_contains("name=metric_alert.mirror_lag_critical"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_blocker_permanent_active_rule(void)
{
    int failures = 0;
    TEST("metric_alerts: a permanent blocker fires blocker_permanent_active") {
        ma_reset_all();
        ma_install_observer();
        blocker_clear("test.metric_alert_permanent");

        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("blocker_permanent_active") == 0);

        struct blocker_record r;
        ASSERT(blocker_init(&r, "test.metric_alert_permanent", "test",
                            BLOCKER_PERMANENT, "hermetic test"));
        ASSERT(blocker_set(&r) >= 0);

        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("blocker_permanent_active") == 1);
        ASSERT(ma_captured_contains("name=metric_alert.blocker_permanent_active"));
        ASSERT(ma_captured_contains("severity=critical"));

        blocker_clear("test.metric_alert_permanent");
        mcp_metrics_evaluate_alert_rules();
        /* Cleared: the latch drops; no further assertion on fire_count
         * needed since the crossing rule already proved the fire path. */
        PASS();
    } _test_next:;
    return failures;
}

static int test_rss_high_rule(void)
{
    int failures = 0;
    TEST("metric_alerts: rss_high fires above its ceiling") {
        ma_reset_all();
        ma_install_observer();

        mcp_metrics_set_node_gauges(0, 0, 512.0, 0, 0); /* under default 6000 MB */
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("rss_high") == 0);

        mcp_metrics_set_node_gauges(0, 0, 7000.0, 0, 0); /* over default 6000 MB */
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("rss_high") == 1);
        ASSERT(ma_captured_contains("name=metric_alert.rss_high"));
        ASSERT(ma_captured_contains("severity=warning"));
        PASS();
    } _test_next:;
    return failures;
}

/* ── mcp_metrics_reset() also clears alert state ────────────────── */

static int test_metrics_reset_clears_alert_state(void)
{
    int failures = 0;
    TEST("metric_alerts: mcp_metrics_reset() folds in alert state reset") {
        ma_install_observer();
        mcp_metrics_set_tip_advance_age(900);
        mcp_metrics_evaluate_alert_rules();
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") >= 1);

        mcp_metrics_reset();
        ASSERT(mcp_metrics_alert_fire_count("tip_stalled") == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_metric_alerts(void);

int test_metric_alerts(void)
{
    int failures = 0;

    failures += test_rule_count_and_names();
    failures += test_allow_listed_for_push();

    failures += test_fires_exactly_once_on_crossing();
    failures += test_not_raised_below_threshold();
    failures += test_refires_after_clearing_and_recrossing();
    failures += test_sentinel_value_does_not_fire();

    failures += test_mirror_lag_high_rule();
    failures += test_mirror_lag_critical_rule();
    failures += test_blocker_permanent_active_rule();
    failures += test_rss_high_rule();

    failures += test_metrics_reset_clears_alert_state();

    event_clear_observers(EV_CONDITION_DETECTED);
    blocker_clear("test.metric_alert_permanent");
    mcp_metrics_reset();
    return failures;
}
