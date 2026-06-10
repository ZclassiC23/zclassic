/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the supervisor primitive (lib/util/src/supervisor.c).
 *
 * Coverage:
 *   - liveness_contract_init: name truncation, default fields
 *   - registration: register/unregister, duplicate rejection, capacity
 *   - child-side: tick monotonicity, progress edge-rearm, stall report
 *   - supervisor loop: period drives on_tick, deadline fires on_stall,
 *     progress-frozen fires on_stall, edge-once not every tick
 *   - lifecycle: start/stop idempotency, second start returns true
 *   - introspection: snapshot_all + dump_state_json shape
 *
 * The supervisor's loop is time-driven; tests use
 * supervisor_set_tick_ms_for_testing(5) to keep each sub-test under
 * ~150 ms. supervisor_reset_for_testing() runs between sub-tests to
 * keep the registry clean. */

#include "test/test_helpers.h"
#include "util/supervisor.h"
#include "json/json.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SUP_CHECK(name, expr) do { \
    printf("supervisor: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Test contract state for callback tracking. */
struct cb_counts {
    _Atomic int tick_calls;
    _Atomic int stall_calls;
};

static void inc_tick(struct liveness_contract *self)
{
    struct cb_counts *cc = (struct cb_counts *)self->ctx;
    if (cc) atomic_fetch_add(&cc->tick_calls, 1);
}

static void inc_stall(struct liveness_contract *self)
{
    struct cb_counts *cc = (struct cb_counts *)self->ctx;
    if (cc) atomic_fetch_add(&cc->stall_calls, 1);
}

int test_supervisor(void)
{
    printf("\n=== supervisor tests ===\n");
    int failures = 0;

    /* ── liveness_contract_init basics ──────────────────────────── */
    {
        struct liveness_contract c;
        liveness_contract_init(&c, "alpha");
        SUP_CHECK("init sets name", strcmp(c.name, "alpha") == 0);
        SUP_CHECK("init sets parent -1", c.parent == -1);
        SUP_CHECK("init clears stall_reason",
            atomic_load(&c.stall_reason) == SUPERVISOR_STALL_NONE);
        SUP_CHECK("init clears ticks_run", atomic_load(&c.ticks_run) == 0u);

        /* Name truncation. */
        char long_name[SUPERVISOR_NAME_MAX + 32];
        memset(long_name, 'x', sizeof(long_name));
        long_name[sizeof(long_name) - 1] = '\0';
        struct liveness_contract c2;
        liveness_contract_init(&c2, long_name);
        SUP_CHECK("init truncates over-long name",
            strlen(c2.name) == SUPERVISOR_NAME_MAX - 1);
        SUP_CHECK("init NULL name is safe (empty)",
            (liveness_contract_init(&c2, NULL), c2.name[0] == '\0'));
    }

    /* ── stall reason naming ────────────────────────────────────── */
    SUP_CHECK("reason_name(NONE)",
        strcmp(supervisor_stall_reason_name(SUPERVISOR_STALL_NONE),
               "none") == 0);
    SUP_CHECK("reason_name(TIME_DEADLINE)",
        strcmp(supervisor_stall_reason_name(SUPERVISOR_STALL_TIME_DEADLINE),
               "time_deadline") == 0);
    SUP_CHECK("reason_name(NO_PROGRESS)",
        strcmp(supervisor_stall_reason_name(SUPERVISOR_STALL_NO_PROGRESS),
               "no_progress") == 0);
    SUP_CHECK("reason_name(invalid) -> (invalid)",
        strcmp(supervisor_stall_reason_name((enum supervisor_stall_reason)99),
               "(invalid)") == 0);

    /* ── register / unregister ──────────────────────────────────── */
    supervisor_reset_for_testing();
    {
        static struct liveness_contract a;
        static struct liveness_contract b;
        liveness_contract_init(&a, "reg.a");
        liveness_contract_init(&b, "reg.b");

        supervisor_child_id ida = supervisor_register(&a);
        supervisor_child_id idb = supervisor_register(&b);
        SUP_CHECK("register returns >=0 for a", ida >= 0);
        SUP_CHECK("register returns >=0 for b", idb >= 0);
        SUP_CHECK("ids are distinct", ida != idb);

        /* Duplicate name rejection. */
        static struct liveness_contract dup;
        liveness_contract_init(&dup, "reg.a");
        supervisor_child_id id_dup = supervisor_register(&dup);
        SUP_CHECK("duplicate name rejected",
            id_dup == SUPERVISOR_INVALID_ID);

        /* NULL pointer rejected. */
        supervisor_child_id id_null = supervisor_register(NULL);
        SUP_CHECK("NULL contract rejected",
            id_null == SUPERVISOR_INVALID_ID);

        supervisor_unregister(ida);
        supervisor_unregister(idb);
    }

    /* ── registry capacity ───────────────────────────────────────── */
    supervisor_reset_for_testing();
    {
        static struct liveness_contract pool[SUPERVISOR_CAP + 4];
        for (int i = 0; i < SUPERVISOR_CAP; i++) {
            char nm[SUPERVISOR_NAME_MAX];
            snprintf(nm, sizeof(nm), "cap.%d", i);
            liveness_contract_init(&pool[i], nm);
            supervisor_child_id id = supervisor_register(&pool[i]);
            if (id < 0) { failures++; break; }
        }
        /* One more should fail. */
        liveness_contract_init(&pool[SUPERVISOR_CAP], "cap.overflow");
        supervisor_child_id over =
            supervisor_register(&pool[SUPERVISOR_CAP]);
        SUP_CHECK("registry rejects past capacity",
            over == SUPERVISOR_INVALID_ID);
    }

    /* ── tick monotonicity + ticks_run counter ──────────────────── */
    supervisor_reset_for_testing();
    {
        static struct liveness_contract c;
        liveness_contract_init(&c, "tick.basic");
        supervisor_child_id id = supervisor_register(&c);
        SUP_CHECK("tick register ok", id >= 0);

        uint32_t before = atomic_load(&c.ticks_run);
        supervisor_tick(id);
        supervisor_tick(id);
        supervisor_tick(id);
        uint32_t after = atomic_load(&c.ticks_run);
        SUP_CHECK("tick increments ticks_run by 3", after - before == 3);

        /* Tick clears TIME_DEADLINE / CHILD_REPORTED stall flags. */
        atomic_store(&c.stall_reason, SUPERVISOR_STALL_TIME_DEADLINE);
        supervisor_tick(id);
        SUP_CHECK("tick clears TIME_DEADLINE stall",
            atomic_load(&c.stall_reason) == SUPERVISOR_STALL_NONE);

        atomic_store(&c.stall_reason, SUPERVISOR_STALL_CHILD_REPORTED);
        supervisor_tick(id);
        SUP_CHECK("tick clears CHILD_REPORTED stall",
            atomic_load(&c.stall_reason) == SUPERVISOR_STALL_NONE);
    }

    /* ── progress edge-rearm ─────────────────────────────────────── */
    supervisor_reset_for_testing();
    {
        static struct liveness_contract c;
        liveness_contract_init(&c, "progress.basic");
        supervisor_child_id id = supervisor_register(&c);

        supervisor_progress(id, 100);
        SUP_CHECK("progress sets marker to 100",
            atomic_load(&c.progress_marker) == 100);
        supervisor_progress(id, 200);
        SUP_CHECK("progress advances to 200",
            atomic_load(&c.progress_marker) == 200);

        /* Same value is a no-op for changed_at. */
        atomic_store(&c.stall_reason, SUPERVISOR_STALL_NO_PROGRESS);
        supervisor_progress(id, 200);  /* unchanged */
        SUP_CHECK("same-value progress does NOT clear NO_PROGRESS",
            atomic_load(&c.stall_reason) == SUPERVISOR_STALL_NO_PROGRESS);

        /* A change clears NO_PROGRESS. */
        supervisor_progress(id, 201);
        SUP_CHECK("changed progress clears NO_PROGRESS",
            atomic_load(&c.stall_reason) == SUPERVISOR_STALL_NONE);
    }

    /* ── child-reported stall (rising edge only) ────────────────── */
    supervisor_reset_for_testing();
    {
        static struct liveness_contract c;
        static struct cb_counts cc = {0};
        liveness_contract_init(&c, "stall.report");
        c.ctx = &cc;
        c.on_stall = inc_stall;
        supervisor_child_id id = supervisor_register(&c);

        supervisor_report_stall(id, SUPERVISOR_STALL_CHILD_REPORTED);
        SUP_CHECK("first report fires callback",
            atomic_load(&cc.stall_calls) == 1);

        /* Second report while stall_reason != NONE is a no-op. */
        supervisor_report_stall(id, SUPERVISOR_STALL_CHILD_REPORTED);
        SUP_CHECK("second report (already stalled) does not re-fire",
            atomic_load(&cc.stall_calls) == 1);

        /* Tick clears, then re-fire works. */
        supervisor_tick(id);
        supervisor_report_stall(id, SUPERVISOR_STALL_CHILD_REPORTED);
        SUP_CHECK("after tick+report, callback re-fires",
            atomic_load(&cc.stall_calls) == 2);
    }

    /* ── supervisor loop drives on_tick when period_secs>0 ──────── */
    supervisor_reset_for_testing();
    supervisor_set_tick_ms_for_testing(5);
    {
        static struct liveness_contract c;
        static struct cb_counts cc;
        memset(&cc, 0, sizeof(cc));
        liveness_contract_init(&c, "loop.tick");
        c.ctx = &cc;
        c.on_tick = inc_tick;
        atomic_store(&c.period_secs, 0);  /* 0 = use micro period below */
        supervisor_child_id id = supervisor_register(&c);
        (void)id;

        /* Configure a 0-sec period so every loop iteration triggers
         * on_tick (period_secs > 0 check uses *=1e6 microseconds, but
         * the loop also checks (now - last_tick) >= period * 1e6, so 0
         * is treated as "disabled". Use 1 second period and run the
         * loop for 1.2 s? Slow. Instead test via direct sweep:
         * supervisor_set_period to 1, then bypass time by manipulating
         * last_tick. Simpler: assert the supervisor thread starts and
         * dispatches at least once when period is reachable. */
        SUP_CHECK("supervisor_start succeeds", supervisor_start());

        /* Backdate last_tick by 2 seconds so a period_secs=1 will fire
         * on next sweep (~5 ms away). */
        atomic_store(&c.last_tick_us,
            atomic_load(&c.last_tick_us) - 2000000);
        atomic_store(&c.period_secs, 1);

        sleep_ms(80);  /* allow ≥10 sweeps */
        SUP_CHECK("on_tick fired at least once",
            atomic_load(&cc.tick_calls) >= 1);
        SUP_CHECK("ticks_run advanced",
            atomic_load(&c.ticks_run) >= 1u);

        supervisor_stop();
    }

    /* ── deadline triggers on_stall once (edge-triggered) ───────── */
    supervisor_reset_for_testing();
    supervisor_set_tick_ms_for_testing(5);
    {
        static struct liveness_contract c;
        static struct cb_counts cc;
        memset(&cc, 0, sizeof(cc));
        liveness_contract_init(&c, "loop.deadline");
        c.ctx = &cc;
        c.on_stall = inc_stall;
        atomic_store(&c.deadline_secs, 1);
        supervisor_child_id id = supervisor_register(&c);
        (void)id;

        /* Backdate last_tick by 5 s so deadline fires next sweep. */
        atomic_store(&c.last_tick_us,
            atomic_load(&c.last_tick_us) - 5000000);

        SUP_CHECK("supervisor_start succeeds", supervisor_start());
        sleep_ms(80);
        SUP_CHECK("deadline fires on_stall exactly once",
            atomic_load(&cc.stall_calls) == 1);
        SUP_CHECK("stall_reason = TIME_DEADLINE",
            atomic_load(&c.stall_reason) ==
                SUPERVISOR_STALL_TIME_DEADLINE);
        SUP_CHECK("stall_fires counter advanced",
            atomic_load(&c.stall_fires) == 1u);

        /* Further sweeps must NOT re-fire (edge-once). */
        sleep_ms(80);
        SUP_CHECK("deadline stall does not re-fire while still stalled",
            atomic_load(&cc.stall_calls) == 1);

        supervisor_stop();
    }

    /* ── progress-frozen triggers on_stall ──────────────────────── */
    supervisor_reset_for_testing();
    supervisor_set_tick_ms_for_testing(5);
    {
        static struct liveness_contract c;
        static struct cb_counts cc;
        memset(&cc, 0, sizeof(cc));
        liveness_contract_init(&c, "loop.progress_frozen");
        c.ctx = &cc;
        c.on_stall = inc_stall;
        /* No deadline; only progress check. */
        atomic_store(&c.progress_max_quiet_us, 500000);  /* 0.5 s */
        supervisor_child_id id = supervisor_register(&c);
        (void)id;

        /* Backdate progress_changed_at by 2 s. */
        atomic_store(&c.progress_changed_at_us,
            atomic_load(&c.progress_changed_at_us) - 2000000);

        SUP_CHECK("supervisor_start succeeds (progress test)",
            supervisor_start());
        sleep_ms(80);
        SUP_CHECK("frozen progress fires on_stall",
            atomic_load(&cc.stall_calls) == 1);
        SUP_CHECK("stall_reason = NO_PROGRESS",
            atomic_load(&c.stall_reason) ==
                SUPERVISOR_STALL_NO_PROGRESS);

        supervisor_stop();
    }

    /* ── lifecycle: start idempotency + stop without start ──────── */
    supervisor_reset_for_testing();
    supervisor_set_tick_ms_for_testing(5);
    {
        SUP_CHECK("supervisor_stop without start is safe",
            (supervisor_stop(), true));
        SUP_CHECK("first start returns true", supervisor_start());
        SUP_CHECK("second start returns true (idempotent)",
            supervisor_start());
        supervisor_stop();
        SUP_CHECK("stop+start cycle works",
            (supervisor_start() && (supervisor_stop(), true)));
    }

    /* ── snapshot_all + dump_state_json shape ───────────────────── */
    supervisor_reset_for_testing();
    {
        static struct liveness_contract one;
        static struct liveness_contract two;
        liveness_contract_init(&one, "snap.one");
        liveness_contract_init(&two, "snap.two");
        atomic_store(&one.period_secs,   30);
        atomic_store(&one.deadline_secs, 120);
        atomic_store(&two.period_secs,   60);
        supervisor_child_id ida = supervisor_register(&one);
        supervisor_child_id idb = supervisor_register(&two);
        (void)ida; (void)idb;

        supervisor_tick(ida);
        supervisor_progress(ida, 42);

        struct supervisor_snapshot snaps[SUPERVISOR_CAP];
        int n = supervisor_snapshot_all(snaps, SUPERVISOR_CAP);
        SUP_CHECK("snapshot returns 2 children", n == 2);
        bool found_one = false, found_two = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(snaps[i].name, "snap.one") == 0) {
                found_one = true;
                if (snaps[i].progress_marker != 42) failures++;
                if (snaps[i].period_secs != 30)    failures++;
                if (snaps[i].deadline_secs != 120) failures++;
            }
            if (strcmp(snaps[i].name, "snap.two") == 0)
                found_two = true;
        }
        SUP_CHECK("snapshot includes snap.one with fields", found_one);
        SUP_CHECK("snapshot includes snap.two", found_two);

        /* JSON dump shape. */
        struct json_value v;
        json_init(&v);
        SUP_CHECK("dump_state_json returns true",
            supervisor_dump_state_json(&v, NULL));
        const struct json_value *count = json_get(&v, "child_count");
        SUP_CHECK("dump has child_count=2",
            count && json_get_int(count) == 2);
        /* After the supervisor tree split, children registered without a
         * domain appear in root_orphans[], not the top-level children[]
         * key (which no longer exists). */
        const struct json_value *kids = json_get(&v, "root_orphans");
        SUP_CHECK("dump has root_orphans array of size 2",
            kids && json_size(kids) == 2);
        json_free(&v);
    }

    /* Restore default tick period for any later tests. */
    supervisor_set_tick_ms_for_testing(1000);
    supervisor_reset_for_testing();
    return failures;
}
