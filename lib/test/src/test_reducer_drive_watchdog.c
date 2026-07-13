/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the reducer_drive_watchdog condition (Lane 0.3) + the
 * "reducer_drive" dumpstate subsystem it owns (Lane 0.4).
 *
 * The condition compares two consecutive detect() ticks of the (test-
 * injected) utxo_apply cursor once the drive has been active longer than
 * a (test-forced) threshold. condition_tick_one() gates a NOT-YET-active
 * condition's detect() calls by real wall-clock poll_secs, so this test
 * installs the same fake clock_iface_t the sync_watchdog condition tests
 * use to advance wall time without sleeping. reducer_drive_age_us() itself
 * reads GetTimeMicros() (real time, not the fake clock — see
 * lib/core/src/utiltime.c), so a short real nanosleep is used to get a
 * nonzero age; the forced threshold of 0s means any nonzero age trips it.
 */

#include "test/test_helpers.h"

#include "conditions/reducer_drive_watchdog.h"
#include "framework/condition.h"
#include "json/json.h"
#include "platform/clock.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/reducer_drive_guard.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RDW_CHECK(name, expr) do { \
    printf("reducer_drive_watchdog: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

struct rdw_fake_clock {
    _Atomic int64_t wall_ms;
};

static int64_t rdw_fake_now_mono(void *self)
{
    (void)self;
    return 1;
}

static int64_t rdw_fake_now_wall(void *self)
{
    struct rdw_fake_clock *c = (struct rdw_fake_clock *)self;
    return atomic_load(&c->wall_ms);
}

static void rdw_fake_clock_install(struct rdw_fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
    static clock_iface_t iface;
    iface.now_monotonic_ns = rdw_fake_now_mono;
    iface.now_wall_ms = rdw_fake_now_wall;
    iface.self = c;
    clock_set_default(&iface);
}

static void rdw_fake_clock_set(struct rdw_fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
}

/* A few ms of REAL sleep so reducer_drive_age_us() (GetTimeMicros(), not the
 * fake clock) reports a nonzero age. */
static void rdw_real_nap(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 3 * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

static void rdw_reset(void)
{
    condition_engine_reset_for_testing();
    blocker_reset_for_testing();
    reducer_drive_watchdog_test_reset();
    /* reducer_drive_guard has no test-reset hook of its own (it is a bare
     * enter/exit counter) — make sure no earlier test group left a drive
     * "active" behind by forcing it fully closed. */
    while (reducer_drive_active())
        reducer_drive_exit();
    register_reducer_drive_watchdog();
}

static void rdw_cleanup(void)
{
    while (reducer_drive_active())
        reducer_drive_exit();
    condition_engine_reset_for_testing();
    blocker_reset_for_testing();
    reducer_drive_watchdog_test_reset();
    clock_reset_default();
}

int test_reducer_drive_watchdog(void);
int test_reducer_drive_watchdog(void)
{
    printf("\n=== reducer_drive_watchdog condition + dumpstate tests ===\n");
    int failures = 0;
    struct rdw_fake_clock fc;
    int64_t t0 = 2000000000;

    /* ---- (a) injected spin: detect fires + blocker named ---- */
    {
        rdw_reset();
        rdw_fake_clock_install(&fc, t0);
        reducer_drive_watchdog_test_set_threshold_secs(0);
        reducer_drive_watchdog_test_set_cursor_override(100);

        reducer_drive_enter_labeled("test_drive");
        rdw_real_nap();

        condition_engine_tick(); /* tick 1: baseline only, cursor=100 */
        bool ok = true;
        ok = ok && reducer_drive_watchdog_test_remedy_calls() == 0;
        ok = ok && condition_engine_get_active_count() == 0;
        RDW_CHECK("baseline tick does not trip", ok);

        rdw_fake_clock_set(&fc, t0 + 20); /* clear the poll_secs gate */
        condition_engine_tick(); /* tick 2: cursor still 100 -> trip + remedy */

        struct condition_runtime_snapshot snap;
        bool got = condition_engine_get_registered_snapshot(
            "reducer_drive_watchdog", &snap);
        bool ok2 = true;
        ok2 = ok2 && reducer_drive_watchdog_test_remedy_calls() == 1;
        ok2 = ok2 && condition_engine_get_active_count() == 1;
        ok2 = ok2 && got && snap.currently_active;
        ok2 = ok2 && blocker_exists("reducer_drive_stuck");
        RDW_CHECK("frozen cursor across two ticks trips + fires remedy once",
                 ok2);

        struct blocker_snapshot snaps[BLOCKER_CAP];
        int n = blocker_snapshot_all(snaps, BLOCKER_CAP);
        bool found_reason = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(snaps[i].id, "reducer_drive_stuck") == 0 &&
                strstr(snaps[i].reason, "test_drive") != NULL) {
                found_reason = true;
                break;
            }
        }
        RDW_CHECK("blocker detail names the driver label", found_reason);

        /* ---- (b) cursor movement clears it ---- */
        reducer_drive_watchdog_test_set_cursor_override(101);
        rdw_fake_clock_set(&fc, t0 + 40);
        condition_engine_tick(); /* cursor advanced past frozen -> witness clears */

        bool ok3 = true;
        ok3 = ok3 && !blocker_exists("reducer_drive_stuck");
        ok3 = ok3 && condition_engine_get_active_count() == 0;
        struct condition_runtime_snapshot snap3;
        bool got3 = condition_engine_get_registered_snapshot(
            "reducer_drive_watchdog", &snap3);
        ok3 = ok3 && got3 && snap3.cleared_count == 1;
        RDW_CHECK("cursor advance past the frozen height clears the blocker",
                 ok3);

        /* ---- (c) drive exit clears it (re-trip first) ---- */
        reducer_drive_watchdog_test_set_cursor_override(200);
        rdw_fake_clock_set(&fc, t0 + 60);
        condition_engine_tick(); /* baseline reset to 200, no trip */
        rdw_fake_clock_set(&fc, t0 + 80);
        condition_engine_tick(); /* cursor unchanged -> re-trip */

        bool ok4 = true;
        ok4 = ok4 && blocker_exists("reducer_drive_stuck");
        ok4 = ok4 && condition_engine_get_active_count() == 1;
        RDW_CHECK("re-trip after a fresh frozen cursor", ok4);

        reducer_drive_exit();
        rdw_fake_clock_set(&fc, t0 + 100);
        condition_engine_tick(); /* drive inactive -> witness clears */

        bool ok5 = true;
        ok5 = ok5 && !blocker_exists("reducer_drive_stuck");
        ok5 = ok5 && condition_engine_get_active_count() == 0;
        ok5 = ok5 && !reducer_drive_active();
        RDW_CHECK("drive exit clears the blocker", ok5);
    }

    /* ---- (d) dumpstate subsystem ---- */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "reducer_drive_dump", "d");
        bool ok = progress_store_open(dir);
        RDW_CHECK("dump: progress_store opens", ok);

        if (ok) {
            sqlite3 *db = progress_store_db();
            char *err = NULL;
            bool seeded =
                sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) ==
                    SQLITE_OK &&
                coins_kv_set_applied_height_in_tx(db, 4242) &&
                sqlite3_exec(db, "COMMIT", NULL, NULL, &err) == SQLITE_OK;
            if (err) sqlite3_free(err);
            RDW_CHECK("dump: seed coins_applied_height", seeded);

            struct json_value v;
            json_init(&v);
            bool dumped = reducer_drive_dump_state_json(&v, NULL);
            bool okd = true;
            okd = okd && dumped;
            okd = okd && json_get(&v, "active") != NULL;
            okd = okd && json_get_bool(json_get(&v, "active")) == false;
            okd = okd && json_get(&v, "label") != NULL;
            okd = okd && json_get(&v, "age_us") != NULL;
            okd = okd && json_get(&v, "watchdog_threshold_secs") != NULL;
            okd = okd && json_get_int(
                json_get(&v, "watchdog_threshold_secs")) == 0;
            okd = okd && json_get(&v, "last_watchdog_fire_unix") != NULL;
            okd = okd &&
                  json_get_int(json_get(&v, "last_watchdog_fire_unix")) > 0;
            okd = okd && json_get(&v, "utxo_apply_cursor") != NULL;
            okd = okd &&
                  json_get_int(json_get(&v, "utxo_apply_cursor")) == 200;
            okd = okd && json_get(&v, "coins_applied_read_ok") != NULL;
            okd = okd && json_get_bool(
                json_get(&v, "coins_applied_read_ok")) == true;
            okd = okd && json_get(&v, "coins_applied_height") != NULL;
            okd = okd &&
                  json_get_int(json_get(&v, "coins_applied_height")) == 4242;
            RDW_CHECK("dump emits all documented fields with live values",
                     okd);
            json_free(&v);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    rdw_cleanup();

    printf("=== test_reducer_drive_watchdog complete: %d failure(s) ===\n",
           failures);
    return failures;
}
