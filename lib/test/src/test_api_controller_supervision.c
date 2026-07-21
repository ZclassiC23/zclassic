/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Focused regression tests for supervision-coverage item #13: the REST
 * /api cache-refresh thread and the block/tx/address lookup-worker thread
 * are registered with the util/supervisor.h liveness tree (op domain) and
 * name a typed blocker when their on_stall fires. Exercises the
 * registration + stall wiring hermetically via ZCL_TESTING seams, without
 * spawning the real detached worker threads (which sleep and drive
 * compute_* against whatever main_state is/isn't set). */

#include "test/test_helpers.h"

#include "controllers/api_controller.h"
#include "supervisors/domains.h"
#include "util/blocker.h"
#include "util/supervisor.h"
#include "../../../app/controllers/src/api_controller_internal.h"

#include <stdio.h>
#include <string.h>

#define ACS_CHECK(name, expr) do { \
    printf("api_controller_supervision: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static bool find_snapshot(const char *name, struct supervisor_snapshot *out)
{
    struct supervisor_snapshot snaps[SUPERVISOR_CAP];
    int n = supervisor_snapshot_all(snaps, SUPERVISOR_CAP);
    for (int i = 0; i < n; i++) {
        if (strcmp(snaps[i].name, name) == 0) {
            *out = snaps[i];
            return true;
        }
    }
    return false;
}

int api_controller_supervision_focused_tests(void)
{
    printf("\n=== api_controller supervision-coverage tests (#13) ===\n");
    int failures = 0;

    /* ── Cache-refresh thread ─────────────────────────────────────── */
    {
        bool ok = true;
        blocker_clear("api_cache_refresh_stalled");

        api_cache_test_register_supervisor();
        ok = ok && api_cache_test_supervisor_id() != SUPERVISOR_INVALID_ID;

        struct supervisor_snapshot snap;
        bool found = find_snapshot("op.api_cache_refresh", &snap);
        ok = ok && found;
        ok = ok && found && snap.deadline_secs == 120;
        ok = ok && found && snap.period_secs == 0;

        ok = ok && !blocker_exists("api_cache_refresh_stalled");
        api_cache_test_force_stall();
        ok = ok && blocker_exists("api_cache_refresh_stalled");
        ok = ok && blocker_class_for("api_cache_refresh_stalled") ==
                         BLOCKER_TRANSIENT;

        blocker_clear("api_cache_refresh_stalled");
        ACS_CHECK("api cache-refresh thread registered + names blocker on stall",
                  ok);
    }

    /* ── Lookup-worker thread ─────────────────────────────────────── */
    {
        bool ok = true;
        blocker_clear("api_lookup_stalled");

        api_lookup_test_register_supervisor();
        ok = ok && api_lookup_test_supervisor_id() != SUPERVISOR_INVALID_ID;

        struct supervisor_snapshot snap;
        bool found = find_snapshot("op.api_lookup", &snap);
        ok = ok && found;
        ok = ok && found && snap.deadline_secs == 60;
        ok = ok && found && snap.period_secs == 0;

        ok = ok && !blocker_exists("api_lookup_stalled");
        api_lookup_test_force_stall();
        ok = ok && blocker_exists("api_lookup_stalled");
        ok = ok && blocker_class_for("api_lookup_stalled") ==
                         BLOCKER_TRANSIENT;

        blocker_clear("api_lookup_stalled");
        ACS_CHECK("api lookup-worker thread registered + names blocker on stall",
                  ok);
    }

    /* ── Both children distinct, both in the op domain (dumpstate
     * supervisor visibility — acceptance bar for #13). ─────────────── */
    {
        bool ok = true;
        ok = ok && api_cache_test_supervisor_id() !=
                         api_lookup_test_supervisor_id();

        struct supervisor_snapshot cache_snap, lookup_snap;
        ok = ok && find_snapshot("op.api_cache_refresh", &cache_snap);
        ok = ok && find_snapshot("op.api_lookup", &lookup_snap);
        ACS_CHECK("cache + lookup are distinct, both-visible supervisor children",
                  ok);
    }

    return failures;
}
