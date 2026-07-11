/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "dev_activation.h"
#include "devloop.h"
#include "framework/app_platform.h"
#include "json/json.h"
#include "sim/social_app_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int route_handler(const struct zcl_app_request_v1 *request,
                         struct zcl_app_mut_bytes *response,
                         struct zcl_app_error *error)
{
    (void)request;
    (void)response;
    (void)error;
    return 0;
}

static int app_self_test(const struct zcl_app_host_v1 *host,
                         struct zcl_app_error *error)
{
    (void)host;
    (void)error;
    return 0;
}

static int app_quiesce(const struct zcl_app_host_v1 *host,
                       uint32_t timeout_ms,
                       struct zcl_app_error *error)
{
    (void)host;
    (void)timeout_ms;
    (void)error;
    return 0;
}

static struct zcl_app_manifest_v1 valid_manifest(void)
{
    static const struct zcl_app_route_v1 routes[] = {
        {
            .struct_size = sizeof(struct zcl_app_route_v1),
            .method = "GET",
            .path = "/posts",
            .flags = ZCL_APP_ROUTE_READ_ONLY,
            .handler = route_handler,
        },
    };
    return (struct zcl_app_manifest_v1) {
        .struct_size = sizeof(struct zcl_app_manifest_v1),
        .manifest_version = ZCL_APP_MANIFEST_V1,
        .required_host_abi = ZCL_APP_HOST_ABI_V1,
        .state_schema_version = 0,
        .app_id = "social",
        .display_name = "ZClassic Social",
        .app_version = "0.1.0",
        .build_identity = "build",
        .content_sha256 =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .required_capabilities = ZCL_APP_CAP_WEB_ROUTES,
        .routes = routes,
        .route_count = 1,
        .self_test = app_self_test,
        .quiesce = app_quiesce,
    };
}

static int test_menu_and_search(void)
{
    int failures = 0;
    TEST("dev platform: shallow menu and semantic search are compact JSON") {
        char body[32768];
        /* Wave 2.2: menu/help/search are now registry-driven, so the schema is
         * the canonical zcl.command_menu.v1 and the shape comes from the single
         * command catalog rather than a hardcoded dev tree. */
        size_t n = zcl_devloop_menu_json("dev", body, sizeof(body));
        ASSERT(n > 0 && n < sizeof(body));
        struct json_value root = {0};
        ASSERT(json_read(&root, body, n));
        ASSERT(strcmp(json_get_str(json_get(&root, "schema")),
                      "zcl.command_menu.v1") == 0);
        /* Shallow: dev's immediate children appear, deep social nodes do not. */
        ASSERT(strstr(body, "dev.app") != NULL);
        ASSERT(strstr(body, "dev.app.describe") == NULL);
        ASSERT(strstr(body, "dev.app.social.resources") == NULL);
        json_free(&root);

        /* "censorship" is a registry tag on the deterministic App simulator. */
        n = zcl_devloop_menu_search_json("censorship", body, sizeof(body));
        ASSERT(n > 0);
        ASSERT(strstr(body, "dev.app.simulate") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_change_classification(void)
{
    int failures = 0;
    TEST("dev platform: classification maps every provider probe and keeps Core reload-only") {
        struct zcl_devloop_plan plan;
        static const struct {
            const char *path;
            const char *probe;
        } hot[] = {
            { "tools/mcp/controllers/app_controller.c", "zcl_name_list" },
            { "tools/mcp/controllers/meta_controller.c", "zcl_tools_list" },
            { "tools/mcp/controllers/chain_controller.c", "zcl_getblockchaininfo" },
            { "tools/mcp/controllers/net_controller.c", "zcl_networkinfo" },
            { "tools/mcp/controllers/wallet_controller.c", "zcl_balance" },
        };
        for (size_t i = 0; i < sizeof(hot) / sizeof(hot[0]); i++) {
            const char *files[] = { hot[i].path };
            ASSERT(zcl_devloop_plan_files(files, 1, &plan));
            ASSERT(plan.action == ZCL_DEVLOOP_HOTSWAP);
            ASSERT(strcmp(plan.proof_group, "hotswap_simnet") == 0);
            ASSERT(strcmp(plan.probe_tool, hot[i].probe) == 0);
        }

        const char *multi_hot[] = {
            hot[0].path, hot[1].path,
        };
        ASSERT(zcl_devloop_plan_files(multi_hot, 2, &plan));
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD);
        ASSERT(strcmp(plan.reason,
                      "multi_provider_generation_not_yet_admitted") == 0);

        const char *core[] = { "core/params/src/params.c" };
        ASSERT(zcl_devloop_plan_files(core, 1, &plan));
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD);
        ASSERT(plan.consensus_risk);

        const char *docs[] = { "docs/BUILD.md" };
        ASSERT(zcl_devloop_plan_files(docs, 1, &plan));
        ASSERT(plan.action == ZCL_DEVLOOP_CHECK);
        ASSERT(plan.docs_only);
        PASS();
    } _test_next:;
    return failures;
}

/* Read the persisted native-cycle verdict from <home>/.local/state/... into
 * buf; returns byte count or 0. */
static size_t read_native_cycle(const char *home, char *buf, size_t cap)
{
    char path[1024];
    int n = snprintf(path, sizeof(path),
                     "%s/.local/state/zclassic23-dev/native-cycle.json", home);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t rn = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[rn] = 0;
    return rn;
}

static int test_core_classification(void)
{
    int failures = 0;
    TEST("dev platform: sealed core is classified sealed + heaviest-proof") {
        struct zcl_devloop_plan plan;

        /* A file under core/ is sealed AND consensus_risk (heaviest proof),
         * routed reload — never hotswap. */
        const char *core[] = { "core/consensus/src/check_block.c" };
        ASSERT(zcl_devloop_plan_files(core, 1, &plan));
        ASSERT(plan.sealed_core);
        ASSERT(plan.consensus_risk);
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD);

        /* core/math is sealed too — broader than the legacy consensus_risk
         * prefix list, which never named it. */
        const char *math[] = { "core/math/src/uint256.c" };
        ASSERT(zcl_devloop_plan_files(math, 1, &plan));
        ASSERT(plan.sealed_core);
        ASSERT(plan.consensus_risk);

        /* A non-core consensus-risk file (lib/validation/...) is NOT sealed:
         * today's behavior is unchanged — heavy proof, still auto-publishes. */
        const char *val[] = { "lib/validation/src/sighash.c" };
        ASSERT(zcl_devloop_plan_files(val, 1, &plan));
        ASSERT(!plan.sealed_core);
        ASSERT(plan.consensus_risk);
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD);
        PASS();
    } _test_next:;
    return failures;
}

static int test_core_refusal_envelope(void)
{
    int failures = 0;
    TEST("dev platform: refusal envelope names paths + elevated procedure") {
        /* Mixed set: only the sealed member appears in "paths". */
        const char *mixed[] = {
            "core/consensus/src/check_block.c", "docs/notes.md"
        };
        char body[4096];
        size_t n = zcl_devloop_refusal_json(mixed, 2, body, sizeof(body));
        ASSERT(n > 0 && n < sizeof(body));

        struct json_value root = {0};
        ASSERT(json_read(&root, body, n));
        ASSERT(strcmp(json_get_str(json_get(&root, "schema")),
                      "zcl.dev_cycle.v1") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "status")),
                      "refused") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "reason")),
                      "sealed_consensus_core") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "manifest")),
                      "core/MANIFEST.sha3") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "law")),
                      "docs/CONSENSUS_PARITY_DOCTRINE.md") == 0);
        /* Sealed != frozen: the unseal command + elevated procedure must be
         * present so an agent is never dead-ended. */
        ASSERT(strstr(json_get_str(json_get(&root, "unseal")),
                      "make core-unseal") != NULL);
        ASSERT(strstr(json_get_str(json_get(&root, "elevated_procedure")),
                      "copy-prove") != NULL);
        json_free(&root);

        /* "paths" carries the sealed file, not the doc. */
        ASSERT(strstr(body, "core/consensus/src/check_block.c") != NULL);
        ASSERT(strstr(body, "docs/notes.md") == NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_core_refusal_cycle(void)
{
    int failures = 0;
    TEST("dev platform: a core cycle refuses (exit 3), no token = no publish") {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "core_refusal", "notoken");
        char *saved_home = getenv("HOME");
        saved_home = saved_home ? strdup(saved_home) : NULL;
        setenv("HOME", dir, 1);

        const char *core[] = { "core/consensus/src/check_block.c" };
        /* repo_root = dir, which has no .core-unseal-token -> refusal. */
        ASSERT(!zcl_devloop_unseal_token_present(dir));
        int rc = zcl_devloop_run_cycle(dir, core, 1);
        ASSERT(rc == 3);  /* blocked-by-precondition, before any publish */

        /* The refusal was persisted as the zcl.dev_cycle.v1 verdict. */
        char verdict[4096];
        size_t vn = read_native_cycle(dir, verdict, sizeof(verdict));
        ASSERT(vn > 0);
        struct json_value v = {0};
        ASSERT(json_read(&v, verdict, vn));
        ASSERT(strcmp(json_get_str(json_get(&v, "status")), "refused") == 0);
        json_free(&v);

        if (saved_home) {
            setenv("HOME", saved_home, 1);
            free(saved_home);
        } else {
            unsetenv("HOME");
        }
        test_rm_rf_recursive(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_core_refusal_token(void)
{
    int failures = 0;
    TEST("dev platform: a valid unseal token lets a core cycle proceed") {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "core_refusal", "token");
        char *saved_home = getenv("HOME");
        saved_home = saved_home ? strdup(saved_home) : NULL;
        setenv("HOME", dir, 1);

        /* Mint the one-shot token the owner ritual writes. */
        char tok[1024];
        snprintf(tok, sizeof(tok), "%s/.core-unseal-token", dir);
        FILE *tf = fopen(tok, "w");
        ASSERT(tf != NULL);
        if (tf) { fputs("unsealed test\n", tf); fclose(tf); }
        ASSERT(zcl_devloop_unseal_token_present(dir));

        const char *core[] = { "core/consensus/src/check_block.c" };
        int rc = zcl_devloop_run_cycle(dir, core, 1);
        /* Token present -> NOT refused. It proceeds to the heavy-proof reload
         * path; in this (non-dev) test binary that path is compiled out and
         * returns the dev_build_required rejection (exit 1). The load-bearing
         * assertion is simply "did NOT refuse" (rc != 3). */
        ASSERT(rc != 3);

        if (saved_home) {
            setenv("HOME", saved_home, 1);
            free(saved_home);
        } else {
            unsetenv("HOME");
        }
        test_rm_rf_recursive(dir);
        PASS();
    } _test_next:;

    return failures;
}

static int test_public_app_abi(void)
{
    int failures = 0;
    TEST("dev platform: Core validates public app ABI fail-closed") {
        char why[256];
        struct zcl_app_manifest_v1 manifest = valid_manifest();
        ASSERT(zcl_app_manifest_v1_validate(
            &manifest, ZCL_APP_CAP_WEB_ROUTES, "build", why, sizeof(why)));

        manifest.required_capabilities |= ZCL_APP_CAP_WALLET_REQUESTS;
        ASSERT(!zcl_app_manifest_v1_validate(
            &manifest, ZCL_APP_CAP_WEB_ROUTES, "build", why, sizeof(why)));
        ASSERT(strstr(why, "capability") != NULL);

        manifest = valid_manifest();
        manifest.state_schema_version = 1;
        manifest.required_capabilities |= ZCL_APP_CAP_RESIDENT_STATE;
        ASSERT(!zcl_app_manifest_v1_validate(
            &manifest,
            ZCL_APP_CAP_WEB_ROUTES | ZCL_APP_CAP_RESIDENT_STATE,
            "build", why, sizeof(why)));
        ASSERT(strstr(why, "migration") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_social_sim(void)
{
    int failures = 0;
    TEST("dev platform: social censorship and replay proof is deterministic") {
        const uint64_t seed = UINT64_C(0x534f4349414c0001);
        struct zcl_social_sim_report a, b;
        ASSERT(zcl_social_app_sim_run(seed, &a));
        ASSERT(zcl_social_app_sim_run(seed, &b));
        ASSERT(a.censorship_bypassed);
        ASSERT(a.partition_rejoin_converged);
        ASSERT(a.late_joiner_caught_up);
        ASSERT(a.invalid_signature_rejected);
        ASSERT(a.transcript == b.transcript);
        ASSERT(a.deliveries == b.deliveries);
        PASS();
    } _test_next:;
    return failures;
}

/* Wave 3.2 native activation engine wiring (devloop_cycle.c /
 * native_dev_command.c). devloop_cycle.c's own transactional_reload branch
 * and native_dev_command.c's dev.vcs.revert relink seam are both
 * ZCL_DEV_BUILD-only (they exec `make`/`systemctl`), so this build
 * (-DZCL_TESTING, no ZCL_DEV_BUILD -- see test_core_refusal_token() above)
 * cannot reach them directly. What IS reachable and load-bearing here is the
 * pure glue both call sites share (declared in devloop.h, defined in
 * devloop_cycle.c, compiled under `ZCL_DEV_BUILD || ZCL_TESTING`): the
 * ZCL_DEV_NATIVE_ACTIVATION switch itself, the dev-lane request builder, and
 * the result mapper. Proving the switch defaults to false IS the
 * "disabled-by-default path still uses the shell argv" case for a binary
 * that never links the engine's process-exec half. */
static int test_native_activation_switch(void)
{
    int failures = 0;
    TEST("dev platform: native activation defaults OFF (shell path stays live)") {
        char *saved = getenv("ZCL_DEV_NATIVE_ACTIVATION");
        saved = saved ? strdup(saved) : NULL;

        unsetenv("ZCL_DEV_NATIVE_ACTIVATION");
        ASSERT(!dev_activation_native_enabled());

        setenv("ZCL_DEV_NATIVE_ACTIVATION", "", 1);
        ASSERT(!dev_activation_native_enabled());

        setenv("ZCL_DEV_NATIVE_ACTIVATION", "0", 1);
        ASSERT(!dev_activation_native_enabled());

        setenv("ZCL_DEV_NATIVE_ACTIVATION", "nah", 1);
        ASSERT(!dev_activation_native_enabled());

        setenv("ZCL_DEV_NATIVE_ACTIVATION", "1", 1);
        ASSERT(dev_activation_native_enabled());

        setenv("ZCL_DEV_NATIVE_ACTIVATION", "true", 1);
        ASSERT(dev_activation_native_enabled());

        setenv("ZCL_DEV_NATIVE_ACTIVATION", "yes", 1);
        ASSERT(dev_activation_native_enabled());

        if (saved) {
            setenv("ZCL_DEV_NATIVE_ACTIVATION", saved, 1);
            free(saved);
        } else {
            unsetenv("ZCL_DEV_NATIVE_ACTIVATION");
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_native_activation_request_builder(void)
{
    int failures = 0;
    TEST("dev platform: dev_activation_request_from_cycle builds dev-lane defaults") {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "native_activation", "request");
        char *saved_home = getenv("HOME");
        saved_home = saved_home ? strdup(saved_home) : NULL;
        char *saved_gen_root = getenv("ZCL_DEV_GENERATION_ROOT");
        saved_gen_root = saved_gen_root ? strdup(saved_gen_root) : NULL;
        unsetenv("ZCL_DEV_GENERATION_ROOT");
        setenv("HOME", dir, 1);

        struct dev_activation_cycle_request creq;
        ASSERT(dev_activation_request_from_cycle("/repo", "abc1234", &creq));
        ASSERT(strcmp(creq.req.repo_root, "/repo") == 0);
        ASSERT(strcmp(creq.req.artifact_path,
                      "/repo/build/bin/zclassic23-dev") == 0);
        ASSERT(strcmp(creq.req.build_commit, "abc1234") == 0);
        ASSERT(strcmp(creq.req.build_type, "fast") == 0);
        ASSERT(strcmp(creq.req.unit, "zcl23-dev.service") == 0);
        ASSERT(creq.req.rpcport == 18252);
        ASSERT(creq.req.mode == DEV_ACTIVATION_MODE_ACTIVATE);
        char want_datadir[1024], want_gen_root[1024];
        snprintf(want_datadir, sizeof(want_datadir), "%s/.zclassic-c23-dev", dir);
        snprintf(want_gen_root, sizeof(want_gen_root),
                "%s/.local/lib/zclassic23-dev", dir);
        ASSERT(strcmp(creq.req.datadir, want_datadir) == 0);
        ASSERT(strcmp(creq.req.gen_root, want_gen_root) == 0);

        /* ZCL_DEV_GENERATION_ROOT overrides the default, same as
         * deploy-dev-lane.sh and native_dev_command.c:dev_generation_root(). */
        setenv("ZCL_DEV_GENERATION_ROOT", "/custom/gen-root", 1);
        ASSERT(dev_activation_request_from_cycle("/repo", "abc1234", &creq));
        ASSERT(strcmp(creq.req.gen_root, "/custom/gen-root") == 0);
        unsetenv("ZCL_DEV_GENERATION_ROOT");

        /* build_commit may be empty (the vcs.vcs.revert seam's use) but not
         * NULL; repo_root/out must not be NULL either. */
        ASSERT(dev_activation_request_from_cycle("/repo", "", &creq));
        ASSERT(creq.req.build_commit[0] == '\0');
        ASSERT(!dev_activation_request_from_cycle(NULL, "abc1234", &creq));
        ASSERT(!dev_activation_request_from_cycle("/repo", NULL, &creq));
        ASSERT(!dev_activation_request_from_cycle("/repo", "abc1234", NULL));

        unsetenv("HOME");
        ASSERT(!dev_activation_request_from_cycle("/repo", "abc1234", &creq));

        if (saved_home) {
            setenv("HOME", saved_home, 1);
            free(saved_home);
        } else {
            unsetenv("HOME");
        }
        if (saved_gen_root) {
            setenv("ZCL_DEV_GENERATION_ROOT", saved_gen_root, 1);
            free(saved_gen_root);
        } else {
            unsetenv("ZCL_DEV_GENERATION_ROOT");
        }
        test_rm_rf_recursive(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_native_activation_result_mapping(void)
{
    int failures = 0;
    TEST("dev platform: dev_activation_map_result maps status/capsule/generation") {
        struct dev_activation_result r = {0};
        r.status = DEV_ACTIVATION_OK;
        snprintf(r.candidate_sha256, sizeof(r.candidate_sha256),
                "%064x", 0);
        struct dev_activation_cycle_outcome out;
        dev_activation_map_result(&r, &out);
        ASSERT(out.ok);
        ASSERT(out.capsule[0] == '\0');
        ASSERT(strcmp(out.generation_hex, r.candidate_sha256) == 0);

        memset(&r, 0, sizeof(r));
        r.status = DEV_ACTIVATION_E_PREFLIGHT;
        snprintf(r.failure_capsule, sizeof(r.failure_capsule),
                "candidate preflight failed");
        dev_activation_map_result(&r, &out);
        ASSERT(!out.ok);
        ASSERT(strcmp(out.capsule, "candidate preflight failed") == 0);

        /* When failure_capsule is empty, verify_detail is the fallback. */
        memset(&r, 0, sizeof(r));
        r.status = DEV_ACTIVATION_E_ACTIVATE;
        snprintf(r.verify_detail, sizeof(r.verify_detail),
                "activation probe did not become ready");
        dev_activation_map_result(&r, &out);
        ASSERT(!out.ok);
        ASSERT(strcmp(out.capsule, "activation probe did not become ready") == 0);

        /* NULL result -> zeroed, never-ok outcome, never a crash. */
        dev_activation_map_result(NULL, &out);
        ASSERT(!out.ok);
        ASSERT(out.capsule[0] == '\0');
        ASSERT(out.generation_hex[0] == '\0');
        PASS();
    } _test_next:;
    return failures;
}

int test_dev_platform(void)
{
    int failures = 0;
    failures += test_menu_and_search();
    failures += test_change_classification();
    failures += test_core_classification();
    failures += test_core_refusal_envelope();
    failures += test_core_refusal_cycle();
    failures += test_core_refusal_token();
    failures += test_public_app_abi();
    failures += test_social_sim();
    failures += test_native_activation_switch();
    failures += test_native_activation_request_builder();
    failures += test_native_activation_result_mapping();
    printf("=== dev_platform: %d failures ===\n", failures);
    return failures;
}
