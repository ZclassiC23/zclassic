/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the `unhealthy` zcl_state rollup
 * (app/controllers/src/diagnostics_health_rollup.c): walks every OTHER
 * dumper in the g_dumpers[] registry, looks for the reserved `_health`
 * { ok, reason } key (see CLAUDE.md "Adding state introspection" +
 * the file header of diagnostics_health_rollup.c), and aggregates only the
 * unhealthy ones.
 *
 * Coverage, using two of the real subsystems that already seed `_health`
 * (lib/util/src/blocker.c and app/jobs/src/tip_finalize_stage_observe.c),
 * not a synthetic stand-in:
 *   (a) with the seeded dumpers in a healthy state, all_ok == true and the
 *       unhealthy array is empty.
 *   (b) seeding one dumper (the typed blocker registry) into an unhealthy
 *       state makes it appear in the unhealthy array with its subsystem
 *       name + reason, and flips all_ok to false.
 *   (c) subsystems that stayed healthy (legacy_mirror, tip_finalize,
 *       chain_advance_coordinator) are NOT included in the unhealthy array.
 *
 * tip_finalize's `_health` reports ok=false ("stage not initialised") until
 * tip_finalize_stage_init() runs, so this file pays the same minimal setup
 * cost (progress_store_open + main_state_init + tip_finalize_stage_init)
 * that lib/test/src/test_tip_finalize_stage.c already pays, purely to put
 * that one real dumper into its healthy state for test (a) — no new health
 * logic anywhere, just exercising what each dumper already computes. */

#include "test/test_helpers.h"
#include "controllers/diagnostics_internal.h"
#include "jobs/tip_finalize_stage.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "validation/main_state.h"
#include "json/json.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define HR_CHECK(name, expr) do { \
    printf("health_rollup: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static const struct json_value *hr_find_by_subsystem(
    const struct json_value *arr, const char *name)
{
    if (!arr || arr->type != JSON_ARR || !name)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *child = json_at(arr, i);
        const struct json_value *sub = json_get(child, "subsystem");
        if (sub && sub->type == JSON_STR && strcmp(sub->val.s, name) == 0)
            return child;
    }
    return NULL;
}

int test_health_rollup(void)
{
    printf("\n=== health_rollup tests ===\n");
    int failures = 0;

    blocker_reset_for_testing();

    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "health_rollup", "1");
    mkdir("./test-tmp", 0755);
    mkdir(dir, 0755);
    bool store_ok = progress_store_open(dir);

    struct main_state ms;
    memset(&ms, 0, sizeof(ms));
    main_state_init(&ms);
    bool tf_ok = tip_finalize_stage_init(&ms);

    /* ── (a) baseline: seeded dumpers healthy ──────────────────────── */
    {
        struct json_value v = {0};
        json_set_object(&v);
        bool ok = unhealthy_dump_state_json(&v, NULL);
        HR_CHECK("baseline: dump returns true", ok);

        const struct json_value *all_ok = json_get(&v, "all_ok");
        HR_CHECK("baseline: all_ok present and true",
                 all_ok && json_get_bool(all_ok));

        const struct json_value *unhealthy = json_get(&v, "unhealthy");
        HR_CHECK("baseline: unhealthy array present and empty",
                 unhealthy && unhealthy->type == JSON_ARR &&
                 json_size(unhealthy) == 0);

        const struct json_value *unhealthy_count =
            json_get(&v, "unhealthy_count");
        HR_CHECK("baseline: unhealthy_count == 0",
                 unhealthy_count && json_get_int(unhealthy_count) == 0);

        /* Sanity: the seeded subsystems this test controls really did
         * report `_health` this cycle (guards against a silent regression
         * where a dumper stops emitting `_health` and the rollup goes
         * quiet instead of failing loud). */
        const struct json_value *checked = json_get(&v, "checked");
        const struct json_value *reporting = json_get(&v, "reporting");
        HR_CHECK("baseline: checked == dumper_count - 1 (self excluded)",
                 checked &&
                 json_get_int(checked) ==
                     (int64_t)diagnostics_dumper_count() - 1);
        HR_CHECK("baseline: reporting >= 4 seeded subsystems "
                 "(blocker, legacy_mirror, chain_advance_coordinator, "
                 "tip_finalize)",
                 reporting && json_get_int(reporting) >= 4);

        json_free(&v);
    }

    /* ── (b) + (c): seed exactly one dumper unhealthy ──────────────── */
    {
        struct blocker_record r;
        blocker_init(&r, "hr_test_blocker", "health_rollup_test",
                    BLOCKER_TRANSIENT, "hr_test_blocker_reason");
        blocker_set(&r);

        struct json_value v = {0};
        json_set_object(&v);
        bool ok = unhealthy_dump_state_json(&v, NULL);
        HR_CHECK("seeded: dump returns true", ok);

        const struct json_value *all_ok = json_get(&v, "all_ok");
        HR_CHECK("seeded: all_ok flips to false",
                 all_ok && !json_get_bool(all_ok));

        const struct json_value *unhealthy = json_get(&v, "unhealthy");
        const struct json_value *blocker_entry =
            hr_find_by_subsystem(unhealthy, "blocker");
        HR_CHECK("seeded: blocker subsystem appears in unhealthy array",
                 blocker_entry != NULL);
        HR_CHECK("seeded: blocker entry's reason names the test blocker",
                 blocker_entry &&
                 json_get(blocker_entry, "reason") &&
                 strstr(json_get_str(json_get(blocker_entry, "reason")),
                        "hr_test_blocker_reason") != NULL);

        const struct json_value *unhealthy_count =
            json_get(&v, "unhealthy_count");
        HR_CHECK("seeded: unhealthy_count == 1",
                 unhealthy_count && json_get_int(unhealthy_count) == 1);
        HR_CHECK("seeded: unhealthy array length matches unhealthy_count",
                 unhealthy && unhealthy_count &&
                 (int64_t)json_size(unhealthy) ==
                     json_get_int(unhealthy_count));

        /* (c) subsystems that stayed healthy are NOT reported. */
        HR_CHECK("seeded: legacy_mirror (healthy) absent from array",
                 hr_find_by_subsystem(unhealthy, "legacy_mirror") == NULL);
        HR_CHECK("seeded: tip_finalize (healthy) absent from array",
                 hr_find_by_subsystem(unhealthy, "tip_finalize") == NULL);
        HR_CHECK("seeded: chain_advance_coordinator (healthy) absent "
                 "from array",
                 hr_find_by_subsystem(unhealthy,
                                      "chain_advance_coordinator") == NULL);
        /* The rollup never reports on itself (would recurse). */
        HR_CHECK("seeded: unhealthy never reports on itself",
                 hr_find_by_subsystem(unhealthy, "unhealthy") == NULL);

        json_free(&v);
        blocker_clear("hr_test_blocker");
    }

    tip_finalize_stage_shutdown();
    main_state_free(&ms);
    if (store_ok) progress_store_close();
    test_cleanup_tmpdir(dir);
    blocker_reset_for_testing();
    (void)tf_ok;

    return failures;
}
