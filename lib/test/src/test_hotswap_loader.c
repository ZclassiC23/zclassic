/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the Tier-1 hot-swap loader's non-dlopen logic: path
 * acceptance, the dev-datadir guard, the generation registry, and the
 * zcl_state dumper.
 *
 * The test binary is built WITHOUT ZCL_DEV_BUILD, so hotswap_load() here is
 * the release stub (refuses, no dlopen) — that release behavior is asserted
 * too. The pure predicates + registry + dumper compile in all builds and are
 * exercised directly. A real end-to-end dlopen is proven by the standalone
 * demo in tools/scripts/hotswap_demo.sh (a ZCL_DEV_BUILD harness). */

#include "test/test_helpers.h"
#include "hotswap/hotswap.h"
#include "mcp/router.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_hotswap_path_acceptance(void)
{
    int failures = 0;
    TEST("hotswap_path_is_acceptable enforces absolute/.so/exists/confined") {
        char why[192];
        /* Relative path. */
        ASSERT(!hotswap_path_is_acceptable("rel/gen.so", why, sizeof(why)));
        /* Not a .so. */
        ASSERT(!hotswap_path_is_acceptable("/tmp/gen.txt", why, sizeof(why)));
        /* Contains "..". */
        ASSERT(!hotswap_path_is_acceptable("/tmp/../etc/gen.so",
                                           why, sizeof(why)));
        /* Absolute .so that does not exist. */
        ASSERT(!hotswap_path_is_acceptable("/tmp/zcl_hs_absent_00.so",
                                           why, sizeof(why)));

        /* A real file under /tmp is accepted. */
        char okp[256];
        snprintf(okp, sizeof(okp), "/tmp/zcl_hs_ok_%d.so", (int)getpid());
        FILE *f = fopen(okp, "w");
        ASSERT(f != NULL);
        if (f) { fputs("x", f); fclose(f); }
        ASSERT(hotswap_path_is_acceptable(okp, why, sizeof(why)));
        unlink(okp);

        /* A real .so NOT under /tmp or a build/hotswap dir is rejected. */
        const char *home = getenv("HOME");
        char rej[512];
        snprintf(rej, sizeof(rej), "%s/zcl_hs_reject_%d.so",
                 home ? home : ".", (int)getpid());
        FILE *g = fopen(rej, "w");
        if (g) { fputs("x", g); fclose(g); }
        ASSERT(!hotswap_path_is_acceptable(rej, why, sizeof(why)));
        unlink(rej);
        PASS();
    } _test_next:;
    return failures;
}

static int test_hotswap_datadir_guard(void)
{
    int failures = 0;
    TEST("hotswap_datadir_is_dev refuses canonical/legacy, allows dev") {
        const char *home = getenv("HOME");
        char canonical[512], legacy[512], dev[512];
        snprintf(canonical, sizeof(canonical), "%s/.zclassic-c23",
                 home ? home : "");
        snprintf(legacy, sizeof(legacy), "%s/.zclassic", home ? home : "");
        snprintf(dev, sizeof(dev), "%s/.zclassic-c23-dev", home ? home : "");

        ASSERT(hotswap_datadir_is_dev(NULL) == true);
        ASSERT(hotswap_datadir_is_dev("") == true);
        ASSERT(hotswap_datadir_is_dev("/some/dev/datadir") == true);
        ASSERT(hotswap_datadir_is_dev(dev) == true);
        ASSERT(hotswap_datadir_is_dev(canonical) == false);
        ASSERT(hotswap_datadir_is_dev(legacy) == false);
        PASS();
    } _test_next:;
    return failures;
}

static int test_hotswap_load_stub_and_registry(void)
{
    int failures = 0;
    TEST("release build: hotswap_load refuses without loading; registry empty") {
        ASSERT(hotswap_generation_count() == 0);

        struct hotswap_load_report rep;
        bool ok = hotswap_load("/tmp/whatever.so", "/tmp/dev-datadir",
                               NULL, &rep);
        ASSERT(ok == false);
        ASSERT(rep.ok == false);
        ASSERT(rep.error[0] != '\0');
        ASSERT(strstr(rep.error, "unavailable") != NULL);
        /* No dlopen happened → no generation registered. */
        ASSERT(hotswap_generation_count() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_hotswap_dump_state(void)
{
    int failures = 0;
    TEST("hotswap_dump_state_json reports availability + empty registry") {
        struct json_value out;
        json_init(&out);
        ASSERT(hotswap_dump_state_json(&out, NULL) == true);

        const struct json_value *avail = json_get(&out, "available");
        ASSERT(avail != NULL);
        /* Test binary is not a ZCL_DEV_BUILD → available is false. */
        ASSERT(json_get_bool(avail) == false);

        const struct json_value *gc = json_get(&out, "generation_count");
        ASSERT(gc != NULL);
        ASSERT(json_get_int(gc) == 0);

        const struct json_value *gens = json_get(&out, "generations");
        ASSERT(gens != NULL);
        ASSERT(gens->type == JSON_ARR);
        ASSERT(json_size(gens) == 0);
        json_free(&out);
        PASS();
    } _test_next:;
    return failures;
}

/* ── mcp_router_replace: the publish seam the loader drives ──────────
 *
 * hotswap_load() stages each generation route via a caller-supplied
 * zcl_hotswap_replace_cb — in production that callback is mcp_router_replace.
 * The loader is a release stub in this (non-ZCL_DEV_BUILD) test binary, so we
 * exercise the replace seam it depends on directly: an atomic re-point that
 * dispatch observes, plus the structural refusals that keep a malformed
 * generation from publishing a bad slot. (Test groups run in forked processes,
 * so touching the global router here is isolated.) */

static int h_orig(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    res->body = strdup("{\"gen\":\"ORIGINAL\"}");
    return res->body ? 0 : -1;
}

static int h_swapped(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    res->body = strdup("{\"gen\":\"SWAPPED\"}");
    return res->body ? 0 : -1;
}

static const struct mcp_tool_route k_route_orig = {
    "t.hotswap_probe", "ops", "original", NULL, 0, h_orig, 0, NULL
};
static const struct mcp_tool_route k_route_swapped = {
    "t.hotswap_probe", "ops", "swapped", NULL, 0, h_swapped, 0, NULL
};
/* Same handler, DIFFERENT name — a structural mismatch replace must refuse. */
static const struct mcp_tool_route k_route_mismatch = {
    "t.other_name", "ops", "mismatch", NULL, 0, h_swapped, 0, NULL
};

static int test_hotswap_router_replace_semantics(void)
{
    int failures = 0;
    TEST("mcp_router_replace re-points a slot dispatch observes; refuses malformed") {
        mcp_router_reset();
        ASSERT(mcp_router_register(&k_route_orig) == true);

        /* Original handler is live. */
        char *b0 = mcp_router_dispatch("t.hotswap_probe", NULL);
        ASSERT(b0 != NULL);
        ASSERT(strstr(b0, "ORIGINAL") != NULL);
        free(b0);

        /* Replace with the swapped handler; dispatch now hits the new fn. */
        ASSERT(mcp_router_replace("t.hotswap_probe", &k_route_swapped) == true);
        char *b1 = mcp_router_dispatch("t.hotswap_probe", NULL);
        ASSERT(b1 != NULL);
        ASSERT(strstr(b1, "SWAPPED") != NULL);
        free(b1);

        /* A replace is not a new slot — count is unchanged. */
        ASSERT(mcp_router_count() == 1);

        /* Structural refusals: unknown slot, name!=slot mismatch, NULLs. */
        ASSERT(mcp_router_replace("t.absent", &k_route_swapped) == false);
        ASSERT(mcp_router_replace("t.hotswap_probe", &k_route_mismatch) == false);
        ASSERT(mcp_router_replace("t.hotswap_probe", NULL) == false);
        ASSERT(mcp_router_replace(NULL, &k_route_swapped) == false);

        /* A refused replace leaves the swapped handler in place. */
        char *b2 = mcp_router_dispatch("t.hotswap_probe", NULL);
        ASSERT(b2 != NULL);
        ASSERT(strstr(b2, "SWAPPED") != NULL);
        free(b2);

        mcp_router_reset();
        PASS();
    } _test_next:;
    return failures;
}

int test_hotswap_loader(void);

int test_hotswap_loader(void)
{
    int failures = 0;
    failures += test_hotswap_path_acceptance();
    failures += test_hotswap_datadir_guard();
    failures += test_hotswap_load_stub_and_registry();
    failures += test_hotswap_dump_state();
    failures += test_hotswap_router_replace_semantics();
    return failures;
}
