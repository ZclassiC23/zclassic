/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the typed blocker primitive (lib/util/src/blocker.c).
 *
 * Coverage:
 *   - init: id/owner length validation, class assignment, default budget
 *   - set: new + refresh, rate limit window, fire_count semantics
 *   - clear: removes + idempotent
 *   - snapshot_all: correct count, age field, deadline_remaining sign
 *   - count_by_class / count_active
 *   - JSON dump: keys present, blockers array, class counts
 *   - escape registry: register, lookup, duplicate, capacity
 *   - escape dispatch: edge-triggered, no re-fire, missing action handled
 *   - rate-limit: env override + testing override
 *   - capacity: cap exhaustion returns -1
 *   - test clock override + advance */

#include "test/test_helpers.h"
#include "util/blocker.h"
#include "json/json.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define BCK_CHECK(name, expr) do { \
    printf("blocker: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Escape capture for dispatch tests. */
static _Atomic int g_esc_a_count;
static _Atomic int g_esc_b_count;
static char        g_esc_last_id[BLOCKER_ID_MAX];

static void esc_a(const struct blocker_snapshot *s)
{
    atomic_fetch_add(&g_esc_a_count, 1);
    snprintf(g_esc_last_id, sizeof(g_esc_last_id), "%s", s->id);
}

static void esc_b(const struct blocker_snapshot *s)
{
    (void)s;
    atomic_fetch_add(&g_esc_b_count, 1);
}

int test_blocker(void)
{
    printf("\n=== blocker tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── blocker_init basic ─────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        struct blocker_record r;
        bool ok = blocker_init(&r, "alpha", "test", BLOCKER_TRANSIENT, "why");
        BCK_CHECK("init returns true on valid", ok);
        BCK_CHECK("init sets id", strcmp(r.id, "alpha") == 0);
        BCK_CHECK("init sets owner", strcmp(r.owner_subsystem, "test") == 0);
        BCK_CHECK("init sets class", r.class == BLOCKER_TRANSIENT);
        BCK_CHECK("init copies reason", strcmp(r.reason, "why") == 0);
        BCK_CHECK("init transient budget=0", r.retry_budget == 0);

        struct blocker_record r2;
        ok = blocker_init(&r2, "perm", "test", BLOCKER_PERMANENT, NULL);
        BCK_CHECK("init NULL reason OK", ok);
        BCK_CHECK("init permanent budget=-1", r2.retry_budget == -1);

        /* Null arg → false */
        bool bad = blocker_init(NULL, "x", "y", BLOCKER_TRANSIENT, "z");
        BCK_CHECK("init NULL out → false", !bad);
        bad = blocker_init(&r, NULL, "y", BLOCKER_TRANSIENT, "z");
        BCK_CHECK("init NULL id → false", !bad);
        bad = blocker_init(&r, "x", NULL, BLOCKER_TRANSIENT, "z");
        BCK_CHECK("init NULL owner → false", !bad);

        /* Length overflow */
        char long_id[BLOCKER_ID_MAX + 8];
        memset(long_id, 'x', sizeof(long_id));
        long_id[sizeof(long_id) - 1] = '\0';
        bad = blocker_init(&r, long_id, "y", BLOCKER_TRANSIENT, "z");
        BCK_CHECK("init long id → false", !bad);
    }

    /* ── blocker_set + read ─────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(1000000);

        struct blocker_record r;
        blocker_init(&r, "alpha", "lms", BLOCKER_TRANSIENT, "stuck");
        int rc = blocker_set(&r);
        BCK_CHECK("set new → 0", rc == 0);
        BCK_CHECK("exists after set", blocker_exists("alpha"));
        BCK_CHECK("class_for matches", blocker_class_for("alpha") == BLOCKER_TRANSIENT);
        BCK_CHECK("count_active 1", blocker_count_active() == 1);
        BCK_CHECK("count transient 1",
                  blocker_count_by_class(BLOCKER_TRANSIENT) == 1);
        BCK_CHECK("count permanent 0",
                  blocker_count_by_class(BLOCKER_PERMANENT) == 0);

        BCK_CHECK("fire_count starts at 1",
                  blocker_fire_count_for_testing("alpha") == 1u);

        /* Set again immediately → rate-limited, fire_count++ only */
        rc = blocker_set(&r);
        BCK_CHECK("set rate-limited → 1", rc == 1);
        BCK_CHECK("fire_count 2 after re-set",
                  blocker_fire_count_for_testing("alpha") == 2u);
        BCK_CHECK("still 1 active",
                  blocker_count_active() == 1);

        /* Null record → -1 */
        rc = blocker_set(NULL);
        BCK_CHECK("set NULL → -1", rc == -1);

        /* Empty id → -1 */
        struct blocker_record empty = {0};
        rc = blocker_set(&empty);
        BCK_CHECK("set empty id → -1", rc == -1);
    }

    /* ── rate limit window ─────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(1000000);
        blocker_set_rate_limit_ms_for_testing(100); /* 100 ms */

        struct blocker_record r;
        blocker_init(&r, "beta", "lms", BLOCKER_TRANSIENT, "x");
        blocker_set(&r);
        BCK_CHECK("first set fc=1",
                  blocker_fire_count_for_testing("beta") == 1u);

        /* Advance 50 ms → still rate-limited */
        blocker_advance_clock_for_testing(50000);
        blocker_set(&r);
        BCK_CHECK("50ms later rate-limited",
                  blocker_fire_count_for_testing("beta") == 2u);

        /* Advance another 60 ms (total 110) → passes window */
        blocker_advance_clock_for_testing(60000);
        blocker_set(&r);
        BCK_CHECK("110ms later passes window",
                  blocker_fire_count_for_testing("beta") == 3u);
    }

    /* ── clear ─────────────────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(1000000);

        struct blocker_record r;
        blocker_init(&r, "gamma", "lms", BLOCKER_TRANSIENT, "x");
        blocker_set(&r);
        BCK_CHECK("exists before clear", blocker_exists("gamma"));
        blocker_clear("gamma");
        BCK_CHECK("absent after clear", !blocker_exists("gamma"));
        BCK_CHECK("clear missing id no-op", (blocker_clear("nope"), true));
        BCK_CHECK("count_active 0 post-clear", blocker_count_active() == 0);
    }

    /* ── snapshot ─────────────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(1000000);
        blocker_set_rate_limit_ms_for_testing(0);

        struct blocker_record r;
        blocker_init(&r, "snap-a", "owner", BLOCKER_RESOURCE, "disk full");
        r.escape_deadline_secs = 60;
        snprintf(r.escape_action, sizeof(r.escape_action), "fake_action");
        blocker_set(&r);

        struct blocker_snapshot snaps[8];
        uint64_t generation = 0;
        int dispatched = -1;
        int rate_limit_ms = -1;
        int n = blocker_snapshot_all_with_meta(
            snaps, 8, &generation, &dispatched, &rate_limit_ms);
        BCK_CHECK("snapshot returns 1", n == 1);
        BCK_CHECK("snapshot generation present", generation > 0);
        BCK_CHECK("snapshot dispatched metadata", dispatched >= 0);
        BCK_CHECK("snapshot rate metadata", rate_limit_ms == 0);
        BCK_CHECK("snap id matches", strcmp(snaps[0].id, "snap-a") == 0);
        BCK_CHECK("snap owner matches",
                  strcmp(snaps[0].owner_subsystem, "owner") == 0);
        BCK_CHECK("snap class resource", snaps[0].class == BLOCKER_RESOURCE);
        BCK_CHECK("snap age >= 0", snaps[0].age_us >= 0);
        BCK_CHECK("snap deadline set", snaps[0].escape_deadline_us > 0);
        BCK_CHECK("snap deadline_remaining positive",
                  snaps[0].deadline_remaining_us > 0);
        BCK_CHECK("snap escape_action copied",
                  strcmp(snaps[0].escape_action, "fake_action") == 0);
        uint64_t before_retry = generation;
        blocker_record_retry("snap-a");
        n = blocker_snapshot_all_with_meta(
            snaps, 8, &generation, &dispatched, &rate_limit_ms);
        BCK_CHECK("observable retry advances generation",
                  n == 1 && generation > before_retry &&
                  snaps[0].retry_count == 1);

        /* Advance past deadline */
        blocker_advance_clock_for_testing(61 * 1000000);
        n = blocker_snapshot_all(snaps, 8);
        BCK_CHECK("snap deadline_remaining negative past deadline",
                  snaps[0].deadline_remaining_us < 0);
    }

    {
        struct blocker_snapshot snapshots[4] = {0};
        snprintf(snapshots[0].id, sizeof(snapshots[0].id),
                 "script_validate.prevout_unresolved");
        snapshots[0].class = BLOCKER_PERMANENT;
        snapshots[0].age_us = 900000000;
        snprintf(snapshots[1].id, sizeof(snapshots[1].id),
                 "utxo_apply.nullifier_backfill_gap");
        snapshots[1].class = BLOCKER_PERMANENT;
        snapshots[1].age_us = 2000000;
        snprintf(snapshots[2].id, sizeof(snapshots[2].id),
                 "utxo_apply.anchor_backfill_gap");
        snapshots[2].class = BLOCKER_PERMANENT;
        snapshots[2].age_us = 1000000;
        snprintf(snapshots[3].id, sizeof(snapshots[3].id),
                 "peer_floor.no_eligible_peers");
        snapshots[3].class = BLOCKER_TRANSIENT;
        snapshots[3].age_us = 1000000000;

        const struct blocker_snapshot *dominant =
            blocker_select_dominant(snapshots, 4);
        BCK_CHECK("causal selector prefers anchor history gap",
                  dominant == &snapshots[2]);
        BCK_CHECK("anchor outranks nullifier history gap",
                  blocker_causal_priority(BLOCKER_PERMANENT,
                      snapshots[2].id) >
                  blocker_causal_priority(BLOCKER_PERMANENT,
                      snapshots[1].id));
        BCK_CHECK("nullifier history outranks downstream permanent",
                  blocker_causal_priority(BLOCKER_PERMANENT,
                      snapshots[1].id) >
                  blocker_causal_priority(BLOCKER_PERMANENT,
                      snapshots[0].id));

        snapshots[3].class = BLOCKER_RESOURCE;
        snprintf(snapshots[3].id, sizeof(snapshots[3].id),
                 "storage.disk_full");
        dominant = blocker_select_dominant(snapshots, 4);
        BCK_CHECK("resource exhaustion remains dominant",
                  dominant == &snapshots[3]);
        BCK_CHECK("causal selector rejects empty input",
                  blocker_select_dominant(NULL, 4) == NULL &&
                  blocker_select_dominant(snapshots, 0) == NULL);
    }

    /* ── JSON dump ─────────────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(2000000);
        blocker_set_rate_limit_ms_for_testing(0);

        struct blocker_record r;
        blocker_init(&r, "j1", "lms", BLOCKER_TRANSIENT, "transient");
        blocker_set(&r);
        blocker_init(&r, "j2", "validation", BLOCKER_PERMANENT, "perm");
        blocker_set(&r);

        struct json_value v;
        json_init(&v);
        bool ok = blocker_dump_state_json(&v, NULL);
        BCK_CHECK("dump returns true", ok);
        BCK_CHECK("active_count=2",
                  json_get_int(json_get(&v, "active_count")) == 2);
        BCK_CHECK("permanent_count=1",
                  json_get_int(json_get(&v, "permanent_count")) == 1);
        BCK_CHECK("transient_count=1",
                  json_get_int(json_get(&v, "transient_count")) == 1);
        BCK_CHECK("rate_limit_ms exposed",
                  json_get(&v, "rate_limit_ms") != NULL);
        BCK_CHECK("generation exposed",
                  json_get_int(json_get(&v, "generation")) > 0);
        const struct json_value *arr = json_get(&v, "blockers");
        BCK_CHECK("blockers array present", arr != NULL);
        BCK_CHECK("blockers array len 2", arr && json_size(arr) == 2);
        json_free(&v);
    }

    /* ── escape registry + dispatch ────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(3000000);
        blocker_set_rate_limit_ms_for_testing(0);
        atomic_store(&g_esc_a_count, 0);
        atomic_store(&g_esc_b_count, 0);
        g_esc_last_id[0] = '\0';

        bool ok = blocker_register_escape("esc_a", esc_a);
        BCK_CHECK("register esc_a ok", ok);
        ok = blocker_register_escape("esc_b", esc_b);
        BCK_CHECK("register esc_b ok", ok);

        ok = blocker_register_escape("esc_a", esc_a);
        BCK_CHECK("register duplicate → false", !ok);
        ok = blocker_register_escape(NULL, esc_a);
        BCK_CHECK("register NULL name → false", !ok);
        ok = blocker_register_escape("esc_c", NULL);
        BCK_CHECK("register NULL fn → false", !ok);

        BCK_CHECK("lookup esc_a", blocker_lookup_escape("esc_a") == esc_a);
        BCK_CHECK("lookup unknown NULL",
                  blocker_lookup_escape("nope") == NULL);

        /* Place a blocker with a 1s deadline, sweep before/after. */
        struct blocker_record r;
        blocker_init(&r, "esc-test", "lms", BLOCKER_TRANSIENT, "stuck");
        r.escape_deadline_secs = 1;
        snprintf(r.escape_action, sizeof(r.escape_action), "esc_a");
        blocker_set(&r);

        int fired = blocker_supervisor_sweep();
        BCK_CHECK("sweep before deadline → 0", fired == 0);

        /* Advance 1.5 s */
        blocker_advance_clock_for_testing(1500000);
        fired = blocker_supervisor_sweep();
        BCK_CHECK("sweep after deadline → 1", fired == 1);
        BCK_CHECK("esc_a fired once", atomic_load(&g_esc_a_count) == 1);
        BCK_CHECK("esc_a saw correct id",
                  strcmp(g_esc_last_id, "esc-test") == 0);

        /* Second sweep — edge-triggered, no re-fire */
        fired = blocker_supervisor_sweep();
        BCK_CHECK("second sweep no re-fire", fired == 0);
        BCK_CHECK("esc_a still 1", atomic_load(&g_esc_a_count) == 1);
    }

    /* ── escape: missing action ───────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(4000000);
        blocker_set_rate_limit_ms_for_testing(0);

        struct blocker_record r;
        blocker_init(&r, "missing-esc", "lms", BLOCKER_TRANSIENT, "stuck");
        r.escape_deadline_secs = 1;
        snprintf(r.escape_action, sizeof(r.escape_action), "unregistered_action");
        blocker_set(&r);

        blocker_advance_clock_for_testing(2000000);
        int fired = blocker_supervisor_sweep();
        BCK_CHECK("missing escape: sweep returns 0", fired == 0);
        /* Not re-fired on next sweep */
        fired = blocker_supervisor_sweep();
        BCK_CHECK("missing escape: second sweep returns 0", fired == 0);
    }

    /* ── retry counter ────────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(5000000);
        blocker_set_rate_limit_ms_for_testing(0);

        struct blocker_record r;
        blocker_init(&r, "retry-id", "lms", BLOCKER_TRANSIENT, "x");
        r.retry_budget = 5;
        blocker_set(&r);

        blocker_record_retry("retry-id");
        blocker_record_retry("retry-id");

        struct blocker_snapshot s;
        int n = blocker_snapshot_all(&s, 1);
        BCK_CHECK("retry_count=2", n == 1 && s.retry_count == 2);
        BCK_CHECK("retry_budget=5", s.retry_budget == 5);

        /* No-op for missing id */
        blocker_record_retry("nope");
        BCK_CHECK("missing retry id no-op", true);
    }

    /* ── capacity ─────────────────────────────────────────────── */
    {
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(6000000);
        blocker_set_rate_limit_ms_for_testing(0);

        char id[32];
        int filled = 0;
        for (int i = 0; i < BLOCKER_CAP; i++) {
            struct blocker_record r;
            snprintf(id, sizeof(id), "cap-%d", i);
            blocker_init(&r, id, "lms", BLOCKER_TRANSIENT, "x");
            if (blocker_set(&r) == 0) filled++;
        }
        BCK_CHECK("filled to cap", filled == BLOCKER_CAP);
        BCK_CHECK("count_active==cap",
                  blocker_count_active() == BLOCKER_CAP);

        /* One more should fail. */
        struct blocker_record r;
        blocker_init(&r, "overflow", "lms", BLOCKER_TRANSIENT, "x");
        int rc = blocker_set(&r);
        BCK_CHECK("overflow → -1", rc == -1);
    }

    /* ── class names ──────────────────────────────────────────── */
    {
        BCK_CHECK("class_name PERMANENT",
                  strcmp(blocker_class_name(BLOCKER_PERMANENT), "permanent") == 0);
        BCK_CHECK("class_name TRANSIENT",
                  strcmp(blocker_class_name(BLOCKER_TRANSIENT), "transient") == 0);
        BCK_CHECK("class_name DEPENDENCY",
                  strcmp(blocker_class_name(BLOCKER_DEPENDENCY), "dependency") == 0);
        BCK_CHECK("class_name RESOURCE",
                  strcmp(blocker_class_name(BLOCKER_RESOURCE), "resource") == 0);
        BCK_CHECK("class_name invalid",
                  strcmp(blocker_class_name((enum blocker_class)99),
                         "(invalid)") == 0);
    }

    /* ── module lifecycle idempotency ─────────────────────────── */
    {
        bool ok = blocker_module_init();
        BCK_CHECK("module_init re-entrant", ok);
        blocker_module_shutdown();
        ok = blocker_module_init();
        BCK_CHECK("module_init after shutdown", ok);
    }

    blocker_reset_for_testing();

    if (failures == 0) {
        printf("=== blocker tests: ALL PASS ===\n\n");
    } else {
        printf("=== blocker tests: %d FAILURE(S) ===\n\n", failures);
    }
    return failures;
}
