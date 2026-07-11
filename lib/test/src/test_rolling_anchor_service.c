/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression tests for rolling_anchor_service supervisor ownership.
 * The service used to run from lib/health; it now owns a chain-domain
 * liveness contract and must stay idempotent across start/stop. */

#include "test/test_helpers.h"

#include "chain/sha3_windows.h"
#include "event/event.h"
#include "services/rolling_anchor_service.h"
#include "util/supervisor.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Row-8 page capture: count EV_OPERATOR_NEEDED events carrying the
 * rolling_anchor sealed-read-failure condition. */
static _Atomic int g_ra_pages = 0;
static void ra_page_observer(enum event_type type, uint32_t peer_id,
                             const void *payload, uint32_t payload_len,
                             void *ctx)
{
    (void)type; (void)peer_id; (void)ctx;
    if (payload && payload_len > 0 &&
        strstr((const char *)payload, "rolling_anchor_sealed_read_failure"))
        atomic_fetch_add(&g_ra_pages, 1);
}

#define RA_CHECK(name, expr) do { \
    printf("rolling_anchor: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int find_rolling_anchor_snapshot(struct supervisor_snapshot *out,
                                        int *out_count)
{
    struct supervisor_snapshot snap[SUPERVISOR_CAP];
    int n = supervisor_snapshot_all(snap, SUPERVISOR_CAP);
    int matches = 0;
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(snap[i].name, "chain.rolling_anchor") == 0) {
            if (out) *out = snap[i];
            matches++;
            found = i;
        }
    }
    if (out_count) *out_count = matches;
    return found;
}

int test_rolling_anchor_service(void)
{
    printf("\n=== rolling_anchor_service tests ===\n");
    int failures = 0;

    supervisor_reset_for_testing();
    rolling_anchor_reset_for_test();

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "rolling_anchor", "supervisor");

    struct main_state ms;
    main_state_init(&ms);

    struct zcl_result r = rolling_anchor_start(&ms, dir);
    RA_CHECK("start returns ZCL_OK", r.ok);

    struct supervisor_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    int count = 0;
    int idx = find_rolling_anchor_snapshot(&snap, &count);
    RA_CHECK("registered exactly one supervisor child", idx >= 0 && count == 1);
    RA_CHECK("period is 60 seconds", snap.period_secs == 60);
    RA_CHECK("deadline stall gate is disabled", snap.deadline_secs == 0);

    int64_t expected_marker =
        (g_sha3_windows_count == 0)
            ? -1
            : (int64_t)g_sha3_windows_count * SHA3_WINDOW_SIZE - 1;
    RA_CHECK("progress marker starts at effective prefix end",
             snap.progress_marker == expected_marker);

    rolling_anchor_stop();
    idx = find_rolling_anchor_snapshot(&snap, &count);
    RA_CHECK("stop keeps one registered child", idx >= 0 && count == 1);
    RA_CHECK("stop disables the supervisor period", snap.period_secs == 0);

    r = rolling_anchor_start(&ms, dir);
    RA_CHECK("restart returns ZCL_OK", r.ok);
    idx = find_rolling_anchor_snapshot(&snap, &count);
    RA_CHECK("restart does not duplicate child", idx >= 0 && count == 1);
    RA_CHECK("restart restores 60 second period", snap.period_secs == 60);

    rolling_anchor_reset_for_test();
    idx = find_rolling_anchor_snapshot(&snap, &count);
    RA_CHECK("test reset unregisters child", idx < 0 && count == 0);

    /* rolling_anchor_window_hash_ending_at — success + one failure envelope
     * (E2 migration to struct zcl_result; no prior direct coverage). */
    {
        uint8_t out[32];
        if (g_sha3_windows_count > 0) {
            int32_t end_h = (int32_t)SHA3_WINDOW_SIZE - 1;
            memset(out, 0, sizeof(out));
            struct zcl_result r2 = rolling_anchor_window_hash_ending_at(end_h,
                                                                        out);
            RA_CHECK("window_hash_ending_at: compile-time window is ZCL_OK",
                     r2.ok);
            RA_CHECK("window_hash_ending_at: hash matches table entry",
                     memcmp(out, g_sha3_windows[0].hash, 32) == 0);
        }
        /* end_h=5 does not end any 1000-block window. */
        struct zcl_result bad =
            rolling_anchor_window_hash_ending_at(5, out);
        RA_CHECK("window_hash_ending_at: non-window-boundary end_h fails",
                 !bad.ok && bad.message[0] != '\0');
        struct zcl_result null_out =
            rolling_anchor_window_hash_ending_at(
                (int32_t)SHA3_WINDOW_SIZE - 1, NULL);
        RA_CHECK("window_hash_ending_at: NULL out is a named failure",
                 !null_out.ok && null_out.message[0] != '\0');
    }

    /* ── §4d row-8: sealed-domain read failure PAGES after N consecutive,
     *    exactly once, re-arms on success; above-prefix never pages ── */
    {
        event_log_init();
        event_clear_all_observers();
        atomic_store(&g_ra_pages, 0);
        event_observe(EV_OPERATOR_NEEDED, ra_page_observer, NULL);

        rolling_anchor_reset_for_test();
        /* effective prefix end = compile-time prefix end. */
        int prefix_end = rolling_anchor_effective_prefix_end();

        /* The first block after the prefix is unsealed window territory.
         * A sparse/snapshot datadir may not have that old body; that must
         * defer extension without becoming a sealed-read failure. */
        rolling_anchor_test_reset_read_failures();
        rolling_anchor_test_note_window_read_failure(
            prefix_end, prefix_end + 1, prefix_end + 1);
        RA_CHECK("missing next-window body is a skip, not read failure",
                 rolling_anchor_test_total_read_failures() == 0 &&
                 rolling_anchor_test_total_skipped_missing_body() == 1 &&
                 rolling_anchor_test_consecutive_read_failures() == 0);
        rolling_anchor_test_run_stall_escalation();
        RA_CHECK("missing next-window body does not page",
                 atomic_load(&g_ra_pages) == 0);

        /* Below prefix, 4 consecutive failures: NOT enough (threshold is 5). */
        rolling_anchor_test_reset_read_failures();
        for (int i = 0; i < 4; i++)
            rolling_anchor_test_inject_read_failure(prefix_end - 100);
        rolling_anchor_test_run_stall_escalation();
        RA_CHECK("4 consecutive below-prefix failures do NOT page",
                 atomic_load(&g_ra_pages) == 0);

        /* 5th failure crosses the threshold: pages exactly once. */
        rolling_anchor_test_inject_read_failure(prefix_end - 100);
        rolling_anchor_test_run_stall_escalation();
        RA_CHECK("5th below-prefix failure pages once",
                 atomic_load(&g_ra_pages) == 1);

        /* A subsequent stall while still latched does NOT re-page. */
        rolling_anchor_test_run_stall_escalation();
        RA_CHECK("latched: repeated stall does not re-page",
                 atomic_load(&g_ra_pages) == 1);

        /* A success resets consecutive + re-arms the latch. */
        rolling_anchor_test_reset_read_failures();
        atomic_store(&g_ra_pages, 0);

        /* ABOVE prefix: even 6 consecutive failures never page (window
         * territory — re-fetch is normal). */
        for (int i = 0; i < 6; i++)
            rolling_anchor_test_inject_read_failure(prefix_end + 5000);
        rolling_anchor_test_run_stall_escalation();
        RA_CHECK("above-prefix failures never page",
                 atomic_load(&g_ra_pages) == 0);

        event_clear_all_observers();
        rolling_anchor_reset_for_test();
    }

    main_state_free(&ms);
    test_cleanup_tmpdir(dir);
    supervisor_reset_for_testing();
    return failures;
}
