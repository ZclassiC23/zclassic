/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression tests for rolling_anchor_service supervisor ownership.
 * The service used to run from lib/health; it now owns a chain-domain
 * liveness contract and must stay idempotent across start/stop. */

#include "test/test_helpers.h"

#include "chain/sha3_windows.h"
#include "services/rolling_anchor_service.h"
#include "util/supervisor.h"
#include "validation/main_state.h"

#include <stdio.h>
#include <string.h>

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

    main_state_free(&ms);
    test_cleanup_tmpdir(dir);
    supervisor_reset_for_testing();
    return failures;
}
