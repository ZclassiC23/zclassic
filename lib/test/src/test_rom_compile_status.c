/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Focused test for the ops.rom / `dumpstate rom_compile` data source
 * (app/jobs/src/rom_compile_status.c) and its pure ASCII renderer
 * (tools/command/rom_compile_render.c). Two halves:
 *   1. The live dump function returns the documented zcl.rom_compile.v1
 *      shape (schema, 8 stages in the ha/vh/bf/bp/sv/pv/ua/tf order, the
 *      five-layer ladder) and honestly reports idle/active per the
 *      refold_progress test-only cache setter — no live progress store
 *      required (the dump function degrades gracefully with the store
 *      closed, same as reducer_frontier_dump_state_json).
 *   2. The renderer, driven with a SYNTHETIC fixture (not the live dumper —
 *      deterministic and independent of process-global stage cursors),
 *      renders the expected percent and ETA text.
 *
 * One TEST()/_test_next: pair per function (the shared ASSERT() macro
 * hardcodes `goto _test_next`, so two TEST blocks in one function would
 * collide on the label) — each case below is its own static function. */

#include "test/test_helpers.h"

#include "jobs/rom_compile_status.h"
#include "jobs/refold_progress.h"
#include "command/rom_compile_render.h"
#include "json/json.h"

#include <string.h>

static int test_rom_compile_dump_idle_shape(void)
{
    int failures = 0;

    TEST("rom_compile dump: idle shape is well-formed and honest") {
        refold_progress_test_set_cached(false);
        rom_compile_status_test_reset_rate_sample();

        struct json_value out;
        json_init(&out);
        bool ok = rom_compile_status_dump_state_json(&out, NULL);
        ASSERT(ok);
        ASSERT(out.type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(&out, "schema")),
                     "zcl.rom_compile.v1");

        const struct json_value *fold = json_get(&out, "fold");
        ASSERT(fold && fold->type == JSON_OBJ);
        ASSERT(!json_get_bool(json_get(fold, "active")));
        ASSERT_STR_EQ(json_get_str(json_get(fold, "mode")), "idle");
        ASSERT_EQ(json_get_int(json_get(fold, "target")), (int64_t)3056758);
        double percent = json_get_real(json_get(fold, "percent"));
        ASSERT(percent >= 0.0 && percent <= 100.0);
        ASSERT(json_get_int(json_get(fold, "remaining_blocks")) >= 0);
        /* First sample in this reset session — no prior point, so rate is
         * unknown/zero and ETA is honestly "unknown". */
        ASSERT(json_get_real(json_get(fold, "rate_blk_s")) == 0.0);
        ASSERT_EQ(json_get_int(json_get(fold, "eta_seconds")), (int64_t)-1);
        ASSERT_STR_EQ(json_get_str(json_get(fold, "eta_human")), "unknown");

        const struct json_value *stages = json_get(&out, "stages");
        ASSERT(stages && stages->type == JSON_ARR);
        ASSERT_EQ(stages->num_children, (size_t)8);
        static const char *const expect_abbrev[8] = {
            "ha", "vh", "bf", "bp", "sv", "pv", "ua", "tf" };
        for (size_t i = 0; i < 8; i++) {
            const struct json_value *s = json_at(stages, i);
            ASSERT_STR_EQ(json_get_str(json_get(s, "abbrev")),
                         expect_abbrev[i]);
            ASSERT(json_get_int(json_get(s, "step_us_ewma")) >= 0);
        }
        ASSERT(json_get_str(json_get(&out, "bottleneck_stage")) != NULL);

        const struct json_value *layers = json_get(&out, "layers");
        ASSERT(layers && layers->type == JSON_OBJ);
        static const char *const expect_layers[5] = {
            "rom_checkpoint", "sealed_history", "sealed_base_receipt",
            "delta", "tip_ring" };
        for (size_t i = 0; i < 5; i++)
            ASSERT(json_get(layers, expect_layers[i]) != NULL);
        /* The ROM checkpoint is compiled into the binary — always present,
         * regardless of live fold state. */
        ASSERT(json_get_bool(
            json_get(json_get(layers, "rom_checkpoint"), "present")));

        json_free(&out);
        PASS();
    } _test_next:;

    return failures;
}

static int test_rom_compile_dump_active_mode(void)
{
    int failures = 0;

    TEST("rom_compile dump: refold_progress_test_set_cached(true) flips "
         "fold.active") {
        refold_progress_test_set_cached(true);
        struct json_value out;
        json_init(&out);
        bool ok = rom_compile_status_dump_state_json(&out, NULL);
        ASSERT(ok);
        const struct json_value *fold = json_get(&out, "fold");
        ASSERT(json_get_bool(json_get(fold, "active")));
        ASSERT_STR_EQ(json_get_str(json_get(fold, "mode")), "from_genesis");
        json_free(&out);
        refold_progress_test_set_cached(false);
        PASS();
    } _test_next:;

    return failures;
}

/* Build a synthetic zcl.rom_compile.v1 `state` object (the shape
 * rom_compile_status_dump_state_json produces) with hand-picked
 * percent/ETA/stage values — independent of any live process-global
 * counter, so the renderer assertions are fully deterministic. */
static void build_fixture(struct json_value *out)
{
    json_init(out);
    json_set_object(out);
    json_push_kv_str(out, "schema", "zcl.rom_compile.v1");

    struct json_value fold;
    json_init(&fold);
    json_set_object(&fold);
    json_push_kv_bool(&fold, "active", true);
    json_push_kv_str(&fold, "mode", "from_genesis");
    json_push_kv_int(&fold, "height", 2200000);
    json_push_kv_int(&fold, "target", 3056758);
    json_push_kv_real(&fold, "percent", 71.99);
    json_push_kv_int(&fold, "remaining_blocks", 856758);
    json_push_kv_real(&fold, "rate_blk_s", 32.5);
    json_push_kv_int(&fold, "eta_seconds", 26362);
    json_push_kv_str(&fold, "eta_human", "7h19m22s");
    json_push_kv(out, "fold", &fold);
    json_free(&fold);

    struct json_value stages;
    json_init(&stages);
    json_set_array(&stages);
    static const struct { const char *ab; const char *name; int64_t us; }
        rows[8] = {
            { "ha", "header_admit", 5 },     { "vh", "validate_headers", 12 },
            { "bf", "body_fetch", 40 },      { "bp", "body_persist", 90 },
            { "sv", "script_validate", 210 },{ "pv", "proof_validate", 812 },
            { "ua", "utxo_apply", 150 },     { "tf", "tip_finalize", 8 },
        };
    for (size_t i = 0; i < 8; i++) {
        struct json_value item;
        json_init(&item);
        json_set_object(&item);
        json_push_kv_str(&item, "abbrev", rows[i].ab);
        json_push_kv_str(&item, "stage", rows[i].name);
        json_push_kv_int(&item, "cursor", 2200000);
        json_push_kv_int(&item, "step_us_ewma", rows[i].us);
        json_push_kv_int(&item, "steps_per_sec",
                         rows[i].us > 0 ? 1000000 / rows[i].us : 0);
        json_push_back(&stages, &item);
        json_free(&item);
    }
    json_push_kv(out, "stages", &stages);
    json_free(&stages);
    json_push_kv_str(out, "bottleneck_stage", "pv");
    json_push_kv_int(out, "commit_us_ewma", 1400);

    struct json_value layers;
    json_init(&layers);
    json_set_object(&layers);
    {
        struct json_value l;
        json_init(&l); json_set_object(&l);
        json_push_kv_bool(&l, "present", true);
        json_push_kv_int(&l, "height", 3056758);
        json_push_kv_str(&l, "sha3_prefix", "5817f0ec");
        json_push_kv(&layers, "rom_checkpoint", &l); json_free(&l);
    }
    {
        struct json_value l;
        json_init(&l); json_set_object(&l);
        json_push_kv_bool(&l, "present", true);
        json_push_kv_int(&l, "segment_count", 12);
        json_push_kv_int(&l, "present_count", 12);
        json_push_kv_int(&l, "verified_count", 11);
        json_push_kv_int(&l, "min_height", 0);
        json_push_kv_int(&l, "max_height", 2199000);
        json_push_kv(&layers, "sealed_history", &l); json_free(&l);
    }
    {
        struct json_value l;
        json_init(&l); json_set_object(&l);
        json_push_kv_bool(&l, "present", false);
        json_push_kv_int(&l, "ratified_height", -1);
        json_push_kv(&layers, "sealed_base_receipt", &l); json_free(&l);
    }
    {
        struct json_value l;
        json_init(&l); json_set_object(&l);
        json_push_kv_bool(&l, "present", true);
        json_push_kv_int(&l, "coins_best_height", 2200000);
        json_push_kv(&layers, "delta", &l); json_free(&l);
    }
    {
        struct json_value l;
        json_init(&l); json_set_object(&l);
        json_push_kv_bool(&l, "present", true);
        json_push_kv_int(&l, "hstar", 2200000);
        json_push_kv_int(&l, "external_tip_height", 2200000);
        json_push_kv(&layers, "tip_ring", &l); json_free(&l);
    }
    {
        struct json_value l;
        json_init(&l); json_set_object(&l);
        json_push_kv_bool(&l, "present", true);
        json_push_kv_int(&l, "last_height", 2199999);
        json_push_kv_int(&l, "generations", 3);
        json_push_kv(&layers, "bundle_export", &l); json_free(&l);
    }
    json_push_kv(out, "layers", &layers);
    json_free(&layers);
}

static int test_rom_compile_render_active_fixture(void)
{
    int failures = 0;

    TEST("rom_compile render: synthetic fixture shows the expected "
         "percent/ETA/bottleneck/ladder") {
        struct json_value fixture;
        build_fixture(&fixture);

        char text[4096];
        rom_compile_render_ascii(&fixture, text, sizeof(text));

        ASSERT(strstr(text, "71.99%") != NULL);
        ASSERT(strstr(text, "rate=32.5 blk/s") != NULL);
        ASSERT(strstr(text, "eta=7h19m22s") != NULL);
        /* The bottleneck stage (pv, 812us — the max of the 8 fixture rows)
         * is marked. */
        const char *pv_line = strstr(text, " pv ");
        ASSERT(pv_line != NULL);
        ASSERT(strstr(pv_line, "*") != NULL);
        /* Ladder: ROM checkpoint / sealed history / delta / tip ring
         * present ("[#]"); sealed base+receipt absent ("[ ]"). */
        ASSERT(strstr(text, "[#] ROM checkpoint") != NULL);
        ASSERT(strstr(text, "[#] Sealed history") != NULL);
        ASSERT(strstr(text, "[ ] Sealed base+receipt") != NULL);
        ASSERT(strstr(text, "[#] Delta frontier") != NULL);
        ASSERT(strstr(text, "[#] Tip ring") != NULL);

        json_free(&fixture);
        PASS();
    } _test_next:;

    return failures;
}

static int test_rom_compile_render_idle_fixture(void)
{
    int failures = 0;

    TEST("rom_compile render: idle fold (no active fold) states so, "
         "not an error") {
        struct json_value fixture;
        build_fixture(&fixture);
        struct json_value *fold =
            (struct json_value *)json_get(&fixture, "fold");
        json_set_bool((struct json_value *)json_get(fold, "active"), false);
        json_set_str((struct json_value *)json_get(fold, "mode"), "idle");
        json_set_real((struct json_value *)json_get(fold, "percent"), 100.0);

        char text[4096];
        rom_compile_render_ascii(&fixture, text, sizeof(text));
        ASSERT(strstr(text, "100.00%") != NULL);
        ASSERT(strstr(text, "no fold active") != NULL);

        json_free(&fixture);
        PASS();
    } _test_next:;

    return failures;
}

static int test_rom_compile_render_pipeline_rows(void)
{
    int failures = 0;

    TEST("rom_compile render: the ROM pipeline promotes fold/seal/manifest/"
         "bundle to first-class rows") {
        struct json_value fixture;
        build_fixture(&fixture);

        char text[8192];
        rom_compile_render_ascii(&fixture, text, sizeof(text));

        ASSERT(strstr(text,
                     "ROM pipeline (fold -> seal -> manifest -> bundle):")
               != NULL);
        /* fold present (height 2200000), seal present (12 segments),
         * manifest present (verified 11), bundle export present (last 2199999,
         * 3 generations). */
        ASSERT(strstr(text, "[#] fold") != NULL);
        ASSERT(strstr(text, "height=2200000/3056758") != NULL);
        ASSERT(strstr(text, "[#] seal") != NULL);
        ASSERT(strstr(text, "12/12 segments sealed") != NULL);
        ASSERT(strstr(text, "[#] manifest") != NULL);
        ASSERT(strstr(text, "verified=11/12") != NULL);
        ASSERT(strstr(text, "[#] bundle export") != NULL);
        ASSERT(strstr(text, "last_height=2199999 generations=3") != NULL);

        json_free(&fixture);
        PASS();
    } _test_next:;

    return failures;
}

static int test_rom_compile_render_bundle_absent(void)
{
    int failures = 0;

    TEST("rom_compile render: an absent bundle_export renders 'absent' "
         "gracefully, never a crash") {
        struct json_value fixture;
        build_fixture(&fixture);
        /* Flip the bundle_export layer to not-present (no export yet). */
        struct json_value *layers =
            (struct json_value *)json_get(&fixture, "layers");
        struct json_value *bx =
            (struct json_value *)json_get(layers, "bundle_export");
        json_set_bool((struct json_value *)json_get(bx, "present"), false);

        char text[8192];
        rom_compile_render_ascii(&fixture, text, sizeof(text));
        ASSERT(strstr(text, "[ ] bundle export") != NULL);
        ASSERT(strstr(text, "absent") != NULL);

        json_free(&fixture);
        PASS();
    } _test_next:;

    return failures;
}

static int test_rom_compile_render_null_state(void)
{
    int failures = 0;

    TEST("rom_compile render: NULL state renders a diagnostic, never "
         "crashes") {
        char text[256];
        rom_compile_render_ascii(NULL, text, sizeof(text));
        ASSERT(text[0] != '\0');
        PASS();
    } _test_next:;

    return failures;
}

int test_rom_compile_status(void)
{
    int failures = 0;
    failures += test_rom_compile_dump_idle_shape();
    failures += test_rom_compile_dump_active_mode();
    failures += test_rom_compile_render_active_fixture();
    failures += test_rom_compile_render_idle_fixture();
    failures += test_rom_compile_render_pipeline_rows();
    failures += test_rom_compile_render_bundle_absent();
    failures += test_rom_compile_render_null_state();
    return failures;
}
