/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "framework/condition.h"
#include "services/legacy_mirror_sync_service.h"

#include <string.h>

#define LMSC_CHECK(name, expr) do { \
    printf("legacy_mirror_stuck_condition: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

void register_legacy_mirror_stuck(void);
void legacy_mirror_stuck_test_reset(void);
int legacy_mirror_stuck_test_remedy_calls(void);

static void reset_lmsc(void)
{
    condition_engine_reset_for_testing();
    legacy_mirror_sync_reset_for_test();
    legacy_mirror_stuck_test_reset();
}

static void install_stats_lmsc(bool enabled, bool running, bool reachable,
                               bool in_flight, int stuck_height)
{
    struct legacy_mirror_sync_stats s;
    memset(&s, 0, sizeof(s));
    s.enabled = enabled;
    s.running = running;
    s.in_flight = in_flight;
    s.reachable = reachable;
    s.local_height = stuck_height > 0 ? stuck_height - 2 : 10;
    s.legacy_height = stuck_height > 0 ? stuck_height + 5 : 12;
    s.stuck_height = stuck_height;
    s.stalls_total = stuck_height > 0 ? 4 : 0;
    snprintf(s.stuck_reason, sizeof(s.stuck_reason), "%s",
             stuck_height > 0 ? "activation-state" : "");
    legacy_mirror_sync_test_set_stats(&s, NULL);
}

int test_legacy_mirror_stuck_condition(void)
{
    printf("\n=== legacy_mirror_stuck condition tests ===\n");
    int failures = 0;

    {
        reset_lmsc();
        install_stats_lmsc(true, true, true, false, 123);
        legacy_mirror_sync_test_set_catchup_result(true, true, true);
        register_legacy_mirror_stuck();
        bool ok = true;

        condition_engine_tick();

        struct legacy_mirror_sync_stats s;
        legacy_mirror_sync_stats_snapshot(&s);
        ok = ok && legacy_mirror_stuck_test_remedy_calls() == 1;
        ok = ok && legacy_mirror_sync_test_catchup_calls() == 1;
        ok = ok && s.stuck_height == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        LMSC_CHECK("stuck mirror catchup clears condition", ok);
    }

    {
        reset_lmsc();
        install_stats_lmsc(true, true, true, true, 124);
        legacy_mirror_sync_test_set_catchup_result(true, true, true);
        register_legacy_mirror_stuck();
        bool ok = true;

        condition_engine_tick();

        ok = ok && legacy_mirror_stuck_test_remedy_calls() == 0;
        ok = ok && legacy_mirror_sync_test_catchup_calls() == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        LMSC_CHECK("in-flight catchup suppresses duplicate remedy", ok);
    }

    {
        reset_lmsc();
        install_stats_lmsc(false, true, true, false, 125);
        legacy_mirror_sync_test_set_catchup_result(true, true, true);
        register_legacy_mirror_stuck();
        bool ok = true;

        condition_engine_tick();

        ok = ok && legacy_mirror_stuck_test_remedy_calls() == 0;
        ok = ok && legacy_mirror_sync_test_catchup_calls() == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        LMSC_CHECK("disabled mirror does not detect", ok);
    }

    {
        reset_lmsc();
        install_stats_lmsc(true, true, true, false, 126);
        legacy_mirror_sync_test_set_catchup_result(true, false, false);
        register_legacy_mirror_stuck();
        bool ok = true;

        condition_engine_tick();

        ok = ok && legacy_mirror_stuck_test_remedy_calls() == 1;
        ok = ok && legacy_mirror_sync_test_catchup_calls() == 1;
        ok = ok && condition_engine_get_active_count() == 1;
        LMSC_CHECK("failed catchup leaves condition active", ok);
    }

    {
        reset_lmsc();
        install_stats_lmsc(true, true, false, false, 127);
        legacy_mirror_sync_test_set_catchup_result(true, true, true);
        register_legacy_mirror_stuck();
        bool ok = true;

        condition_engine_tick();

        ok = ok && legacy_mirror_stuck_test_remedy_calls() == 0;
        ok = ok && legacy_mirror_sync_test_catchup_calls() == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        LMSC_CHECK("unreachable mirror does not request catchup", ok);
    }

    reset_lmsc();
    return failures;
}
