/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "dev_activation.h"
#include "dev_failure_store.h"
#include "devloop.h"
#include "framework/app_definition.h"
#include "framework/app_platform.h"
#include "json/json.h"
#include "keys/key.h"
#include "sim/social_app_sim.h"
#include "util/safe_alloc.h"
#include "wallet/wallet.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static const uint8_t g_test_social_chain_id[ZCL_APP_EVENT_CHAIN_ID_SIZE] = {
    0x02, 0x06, 0x26, 0x01, 0x43, 0x83, 0x8b, 0x5f,
    0xf5, 0x2d, 0xc2, 0xeb, 0x7b, 0x4b, 0x80, 0x99,
    0xd4, 0xe4, 0xc9, 0x9d, 0xc3, 0xef, 0x19, 0x79,
    0x42, 0x89, 0xa2, 0xcd, 0x4c, 0x10, 0x07, 0x00,
};

static struct zcl_app_event_scope_v1 test_social_scope(void)
{
    struct zcl_app_event_scope_v1 scope;
    memset(&scope, 0, sizeof(scope));
    scope.struct_size = sizeof(scope);
    memcpy(scope.app_id, "social", sizeof("social"));
    memcpy(scope.topic, "social.events.v1", sizeof("social.events.v1"));
    memcpy(scope.chain_id, g_test_social_chain_id, sizeof(scope.chain_id));
    scope.max_event_bytes = 65536;
    return scope;
}

static struct zcl_app_event_intent_v1 test_social_intent(
    const uint8_t *payload, size_t payload_len)
{
    struct zcl_app_event_intent_v1 intent;
    memset(&intent, 0, sizeof(intent));
    intent.struct_size = sizeof(intent);
    intent.kind = 1;
    intent.sequence = 1;
    intent.created_at = UINT64_C(1700000000);
    intent.payload.data = payload;
    intent.payload.len = payload_len;
    return intent;
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

/* Read and verify the worktree-scoped SHA3-sealed cycle verdict. */
static size_t read_native_cycle(const char *repo_root, char *buf, size_t cap)
{
    size_t len = 0;
    return zcl_devloop_cycle_state_read(repo_root, buf, cap, &len, NULL,
                                        NULL, 0) ==
                   ZCL_DEVLOOP_STATE_FOUND
               ? len : 0;
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
         * it still selects the heavy proof, but watcher classification never
         * grants publication authority. */
        const char *val[] = { "lib/validation/src/sighash.c" };
        ASSERT(zcl_devloop_plan_files(val, 1, &plan));
        ASSERT(!plan.sealed_core);
        ASSERT(plan.consensus_risk);
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD);
        PASS();
    } _test_next:;
    return failures;
}

static int test_watcher_publication_containment(void)
{
    int failures = 0;
    TEST("dev platform: watchers verify by default and apply is contained") {
        ASSERT(zcl_devloop_default_watch_publish_mode() ==
               ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY);
        ASSERT(!zcl_devloop_publish_mode_applies(
            ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY));
        ASSERT(zcl_devloop_publish_mode_applies(ZCL_DEVLOOP_PUBLISH_APPLY));
        ASSERT(strcmp(zcl_devloop_publish_mode_name(
                          ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY),
                      "verify") == 0);
        ASSERT(strcmp(zcl_devloop_publish_mode_name(ZCL_DEVLOOP_PUBLISH_APPLY),
                      "auto") == 0);
        ASSERT(zcl_devloop_publish_mode_name(
                   (enum zcl_devloop_publish_mode)99) == NULL);

        char lock_path[ZCL_DEVLOOP_PATH_MAX];
        ASSERT(zcl_devloop_watch_lock_path("/tmp/zcl-wt-main", lock_path,
                                           sizeof(lock_path)));
        ASSERT(strcmp(lock_path,
                      "/tmp/zcl-wt-main/.cache/zcl-dev-watch.lock") == 0);
        char other_lock[ZCL_DEVLOOP_PATH_MAX];
        ASSERT(zcl_devloop_watch_lock_path("/tmp/zcl-wt-2", other_lock,
                                           sizeof(other_lock)));
        ASSERT(strcmp(lock_path, other_lock) != 0);
        ASSERT(!zcl_devloop_watch_lock_path(NULL, lock_path,
                                            sizeof(lock_path)));

        /* The containment decision is independent of classification. These
         * cover hot-swap, ordinary reload, consensus reload, and sealed-core
         * reload without granting any of them watcher publication authority. */
        const char *hot[] = { "tools/mcp/controllers/app_controller.c" };
        const char *reload[] = { "app/services/src/node_health_service.c" };
        const char *consensus[] = { "lib/validation/src/sighash.c" };
        const char *sealed[] = { "core/consensus/src/check_block.c" };
        struct zcl_devloop_plan plan;
        ASSERT(zcl_devloop_plan_files(hot, 1, &plan));
        ASSERT(plan.action == ZCL_DEVLOOP_HOTSWAP);
        ASSERT(!zcl_devloop_publish_mode_applies(
            zcl_devloop_default_watch_publish_mode()));
        ASSERT(zcl_devloop_plan_files(reload, 1, &plan));
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD && !plan.consensus_risk);
        ASSERT(!zcl_devloop_publish_mode_applies(
            zcl_devloop_default_watch_publish_mode()));
        ASSERT(zcl_devloop_plan_files(consensus, 1, &plan));
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD && plan.consensus_risk &&
               !plan.sealed_core);
        ASSERT(!zcl_devloop_publish_mode_applies(
            zcl_devloop_default_watch_publish_mode()));
        ASSERT(zcl_devloop_plan_files(sealed, 1, &plan));
        ASSERT(plan.action == ZCL_DEVLOOP_RELOAD && plan.consensus_risk &&
               plan.sealed_core);
        ASSERT(!zcl_devloop_publish_mode_applies(
            zcl_devloop_default_watch_publish_mode()));

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
    TEST("dev platform: every apply cycle is contained before Core authority") {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "core_refusal", "notoken");
        char *saved_home = getenv("HOME");
        saved_home = saved_home ? strdup(saved_home) : NULL;
        setenv("HOME", dir, 1);

        const char *core[] = { "core/consensus/src/check_block.c" };
        /* Publication containment precedes the Core-unseal boundary. */
        ASSERT(!zcl_devloop_unseal_token_present(dir));
        int rc = zcl_devloop_run_cycle(dir, core, 1);
        ASSERT(rc == 3);  /* blocked-by-precondition, before any publish */

        /* The refusal was persisted as the zcl.dev_cycle.v1 verdict. */
        char verdict[4096];
        size_t vn = read_native_cycle(dir, verdict, sizeof(verdict));
        ASSERT(vn > 0);
        struct json_value v = {0};
        ASSERT(json_read(&v, verdict, vn));
        ASSERT(strcmp(json_get_str(json_get(&v, "status")), "blocked") == 0);
        ASSERT(strcmp(json_get_str(json_get(&v, "phase")),
                      "publication_contained") == 0);
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
    TEST("dev platform: an unseal token is not publication authority") {
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
        ASSERT(rc == 3);

        char verdict[4096];
        size_t vn = read_native_cycle(dir, verdict, sizeof(verdict));
        ASSERT(vn > 0);
        struct json_value v = {0};
        ASSERT(json_read(&v, verdict, vn));
        ASSERT(strcmp(json_get_str(json_get(&v, "status")), "blocked") == 0);
        ASSERT(strcmp(json_get_str(json_get(&v, "phase")),
                      "publication_contained") == 0);
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

        static const struct zcl_app_topic_v1 duplicate_topics[] = {
            { sizeof(struct zcl_app_topic_v1), "social.events.v1", 1, 1024 },
            { sizeof(struct zcl_app_topic_v1), "social.events.v1", 1, 2048 },
        };
        manifest = valid_manifest();
        manifest.topics = duplicate_topics;
        manifest.topic_count = 2;
        ASSERT(!zcl_app_manifest_v1_validate(
            &manifest, ZCL_APP_CAP_WEB_ROUTES, "build", why, sizeof(why)));
        ASSERT(strstr(why, "duplicate app topic") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_app_definition_compiler(void)
{
    int failures = 0;
    TEST("dev platform: strict compiler accepts Blog and Social catalog") {
        struct zcl_app_definition_v1 blog, social;
        struct zcl_result result =
            zcl_app_definition_load_v1(".", "blog", &blog);
        ASSERT(result.ok);
        ASSERT(blog.struct_size == sizeof(blog));
        ASSERT(blog.definition_version == ZCL_APP_DEFINITION_V1);
        ASSERT(strcmp(blog.app_id, "blog") == 0);
        ASSERT(strcmp(blog.display_name, "ZClassic Blog") == 0);
        ASSERT(strcmp(blog.app_version, "0.1.0") == 0);
        ASSERT(blog.resource_count == 2);
        ASSERT(blog.topic_count == 1);
        ASSERT(strcmp(blog.topics[0].name, "blog.posts.v1") == 0);
        ASSERT(blog.topics[0].wire_version == 1);
        ASSERT(blog.topics[0].max_event_bytes == 20000);
        ASSERT(blog.mount_count == 1);
        ASSERT(strcmp(blog.mounts[0].path, "/blog") == 0);
        ASSERT(blog.onion_declared && blog.onion_enabled);
        ASSERT(blog.znam_declared && strcmp(blog.znam, "blog") == 0);
        ASSERT(blog.state_schema_declared && blog.state_schema_version == 1);
        ASSERT(blog.simulation_count == 0);
        ASSERT((blog.required_capabilities & ZCL_APP_CAP_SIGNED_EVENTS) != 0);

        result = zcl_app_definition_load_v1(".", "social", &social);
        ASSERT(result.ok);
        ASSERT(strcmp(social.app_id, "social") == 0);
        ASSERT(social.resource_count == 4);
        ASSERT(social.topic_count == 1);
        ASSERT(strcmp(social.topics[0].name, "social.events.v1") == 0);
        ASSERT(social.mount_count == 1);
        ASSERT(strcmp(social.mounts[0].path, "/") == 0);
        ASSERT(social.simulation_count == 4);

        struct zcl_app_definition_catalog_v1 catalog;
        ASSERT(zcl_app_definition_builtin_count_v1() == 2);
        ASSERT(strcmp(zcl_app_definition_builtin_id_v1(0), "blog") == 0);
        ASSERT(strcmp(zcl_app_definition_builtin_id_v1(1), "social") == 0);
        ASSERT(zcl_app_definition_builtin_id_v1(2) == NULL);
        ASSERT(zcl_app_definition_builtin_v1("blog"));
        ASSERT(!zcl_app_definition_builtin_v1("Blog"));
        ASSERT(!zcl_app_definition_builtin_v1("missing"));
        result = zcl_app_definition_builtin_catalog_compile_v1(".", &catalog);
        ASSERT(result.ok);
        ASSERT(catalog.struct_size == sizeof(catalog));
        ASSERT(catalog.catalog_version == ZCL_APP_DEFINITION_V1);
        ASSERT(catalog.app_count == 2);
        ASSERT(strcmp(catalog.apps[0].app_id, "blog") == 0);
        ASSERT(strcmp(catalog.apps[1].app_id, "social") == 0);

        static const char *const duplicate_ids[] = { "blog", "blog" };
        memset(&catalog, 0xa5, sizeof(catalog));
        result = zcl_app_definition_catalog_compile_v1(
            ".", duplicate_ids,
            sizeof(duplicate_ids) / sizeof(duplicate_ids[0]), &catalog);
        ASSERT(!result.ok);
        ASSERT(strstr(result.message, "duplicate catalog app id") != NULL);
        ASSERT(catalog.struct_size == 0 && catalog.app_count == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_strict_dev_app_producers(void)
{
    int failures = 0;
    TEST("dev platform: app describe and plan consume strict definitions only") {
        char body[8192];
        size_t n = zcl_devloop_app_describe_json(
            ".", "blog", body, sizeof(body));
        ASSERT(n > 0 && n < sizeof(body));
        struct json_value doc = {0};
        ASSERT(json_read(&doc, body, n));
        ASSERT(strcmp(json_get_str(json_get(&doc, "status")), "ok") == 0);
        ASSERT(strcmp(json_get_str(json_get(&doc, "app_id")), "blog") == 0);
        ASSERT(strcmp(json_get_str(json_get(&doc, "display_name")),
                      "ZClassic Blog") == 0);
        ASSERT(strcmp(json_get_str(json_get(&doc, "compiler")),
                      "strict-bounded-v1") == 0);
        ASSERT(strcmp(json_get_str(json_get(&doc, "authority")),
                      "definition-only") == 0);
        const struct json_value *topics = json_get(&doc, "topics");
        ASSERT(topics && topics->type == JSON_ARR && topics->num_children == 1);
        ASSERT(strcmp(json_get_str(json_get(&topics->children[0], "name")),
                      "blog.posts.v1") == 0);
        ASSERT(strstr(body, "signed_events") != NULL);
        ASSERT(strstr(body, "runtime_authority") != NULL);
        json_free(&doc);

        n = zcl_devloop_app_plan_json(
            ".", "blog", "comments", body, sizeof(body));
        ASSERT(n > 0 && n < sizeof(body));
        memset(&doc, 0, sizeof(doc));
        ASSERT(json_read(&doc, body, n));
        ASSERT(strcmp(json_get_str(json_get(&doc, "mode")),
                      "preview-only") == 0);
        const struct json_value *writes = json_get(&doc, "writes");
        ASSERT(writes && writes->type == JSON_BOOL && !writes->val.b);
        ASSERT(strstr(body, "app/models/src/comments.c") != NULL);
        ASSERT(strstr(body, "apps/blog/models/comments.c") == NULL);
        ASSERT(strstr(body, "writes and publishes nothing") != NULL);
        json_free(&doc);

        /* The old textual scraper/planner accepted both of these. The strict
         * compiler now requires a real, valid app.def before producing a
         * description or even a preview-only resource plan. */
        ASSERT(zcl_devloop_app_describe_json(
            ".", "Blog", body, sizeof(body)) == 0);
        ASSERT(zcl_devloop_app_plan_json(
            ".", "missing", "posts", body, sizeof(body)) == 0);
        ASSERT(zcl_devloop_app_plan_json(
            ".", "blog", "Bad-Resource", body, sizeof(body)) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_app_definition_hostile_fixtures(void)
{
    int failures = 0;
    TEST("dev platform: strict app compiler rejects hostile definitions") {
        static const struct {
            const char *name;
            const char *source;
            const char *error;
        } fixtures[] = {
            {
                "unknown directive",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(CHAIN_READ)\n"
                "ZCL_APP_FUTURE(\"x\")\n",
                "unknown directive",
            },
            {
                "duplicate singleton",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n",
                "duplicate ZCL_APP",
            },
            {
                "malformed mount",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(WEB_ROUTES)\n"
                "ZCL_APP_WEB_MOUNT(\"/bad/../route\")\n",
                "invalid web mount",
            },
            {
                "missing mount capability",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(CHAIN_READ)\n"
                "ZCL_APP_WEB_MOUNT(\"/fixture\")\n",
                "web mounts require WEB_ROUTES",
            },
            {
                "capability without mount",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(WEB_ROUTES)\n",
                "lacks a web mount",
            },
            {
                "duplicate mount",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(WEB_ROUTES)\n"
                "ZCL_APP_WEB_MOUNT(\"/fixture\")\n"
                "ZCL_APP_WEB_MOUNT(\"/fixture\")\n",
                "duplicate web mount",
            },
            {
                "duplicate topic",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(SIGNED_EVENTS)\n"
                "ZCL_APP_CAPABILITY(P2P_TOPICS)\n"
                "ZCL_APP_TOPIC(\"fixture.events.v1\", 1, 1024)\n"
                "ZCL_APP_TOPIC(\"fixture.events.v1\", 1, 2048)\n",
                "duplicate topic",
            },
            {
                "too many resources",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_RESOURCE(\"r00\")\nZCL_APP_RESOURCE(\"r01\")\n"
                "ZCL_APP_RESOURCE(\"r02\")\nZCL_APP_RESOURCE(\"r03\")\n"
                "ZCL_APP_RESOURCE(\"r04\")\nZCL_APP_RESOURCE(\"r05\")\n"
                "ZCL_APP_RESOURCE(\"r06\")\nZCL_APP_RESOURCE(\"r07\")\n"
                "ZCL_APP_RESOURCE(\"r08\")\nZCL_APP_RESOURCE(\"r09\")\n"
                "ZCL_APP_RESOURCE(\"r10\")\nZCL_APP_RESOURCE(\"r11\")\n"
                "ZCL_APP_RESOURCE(\"r12\")\nZCL_APP_RESOURCE(\"r13\")\n"
                "ZCL_APP_RESOURCE(\"r14\")\nZCL_APP_RESOURCE(\"r15\")\n"
                "ZCL_APP_RESOURCE(\"r16\")\n",
                "too many resources",
            },
            {
                "trailing junk",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\");\n",
                "trailing junk",
            },
            {
                "unknown capability",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(RAW_SOCKET)\n",
                "unknown capability",
            },
            {
                "duplicate capability",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(CHAIN_READ)\n"
                "ZCL_APP_CAPABILITY(CHAIN_READ)\n",
                "duplicate capability",
            },
            {
                "conflicting onion capability",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(WEB_ROUTES)\n"
                "ZCL_APP_CAPABILITY(ONION_BINDING)\n"
                "ZCL_APP_WEB_MOUNT(\"/fixture\")\n"
                "ZCL_APP_ONION(false)\n",
                "conflicts with disabled binding",
            },
            {
                "oversized topic event",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(SIGNED_EVENTS)\n"
                "ZCL_APP_CAPABILITY(P2P_TOPICS)\n"
                "ZCL_APP_TOPIC(\"fixture.events.v1\", 1, 65537)\n",
                "invalid topic declaration",
            },
            {
                "leading zero",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(SIGNED_EVENTS)\n"
                "ZCL_APP_CAPABILITY(P2P_TOPICS)\n"
                "ZCL_APP_TOPIC(\"fixture.events.v1\", 01, 1024)\n",
                "leading zero",
            },
            {
                "malformed declared id",
                "ZCL_APP(\"Fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(CHAIN_READ)\n",
                "invalid app id",
            },
            {
                "malformed version",
                "ZCL_APP(\"fixture\", \"Fixture\", \"01.0.0\")\n"
                "ZCL_APP_CAPABILITY(CHAIN_READ)\n",
                "invalid semantic version",
            },
            {
                "unterminated comment",
                "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
                "ZCL_APP_CAPABILITY(CHAIN_READ)\n/* never closed",
                "unterminated comment",
            },
        };

        for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
            struct zcl_app_definition_v1 definition;
            memset(&definition, 0xa5, sizeof(definition));
            struct zcl_result result = zcl_app_definition_parse_v1(
                "fixture", fixtures[i].source, strlen(fixtures[i].source),
                &definition);
            ASSERT(!result.ok);
            ASSERT(strstr(result.message, fixtures[i].error) != NULL);
            ASSERT(definition.struct_size == 0);
        }

        static const char valid[] =
            "ZCL_APP(\"fixture\", \"Fixture\", \"1.0.0\")\n"
            "ZCL_APP_CAPABILITY(CHAIN_READ)\n";
        struct zcl_app_definition_v1 definition;
        struct zcl_result result = zcl_app_definition_parse_v1(
            "../fixture", valid, sizeof(valid) - 1, &definition);
        ASSERT(!result.ok);
        ASSERT(strstr(result.message, "path id") != NULL);
        ASSERT(definition.struct_size == 0);

        char unterminated_id[ZCL_APP_ID_MAX + 1];
        memset(unterminated_id, 'a', sizeof(unterminated_id));
        result = zcl_app_definition_parse_v1(
            unterminated_id, valid, sizeof(valid) - 1, &definition);
        ASSERT(!result.ok);
        ASSERT(strstr(result.message, "path id") != NULL);
        ASSERT(definition.struct_size == 0);

        static const char embedded_nul[] = {
            'Z','C','L','_','A','P','P','(', '"','f','i','x','t','u','r','e','"',
            ',', '"','F','i','x','t','u','r','e','"', ',', '"','1','.','0','.',
            '0','"',')','\n','\0','X'
        };
        result = zcl_app_definition_parse_v1(
            "fixture", embedded_nul, sizeof(embedded_nul), &definition);
        ASSERT(!result.ok);
        ASSERT(strstr(result.message, "embedded NUL") != NULL);
        ASSERT(definition.struct_size == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_signed_app_events(void)
{
    int failures = 0;
    struct wallet *wallet = NULL;
    struct zcl_app_event_signing_binding_v1 *binding = NULL;
    bool wallet_ready = false;
    TEST("dev platform: Core wallet signs canonical scoped App events") {
        static const uint8_t payload[] = "hello zclassic23";
        static const uint8_t expected_event_id[32] = {
            0xd2, 0xbf, 0x68, 0xe3, 0x05, 0xf1, 0xe5, 0x8a,
            0x02, 0xb0, 0x09, 0x43, 0x23, 0xd8, 0xfd, 0x8a,
            0xca, 0x29, 0x76, 0xe8, 0xa6, 0xdd, 0xaf, 0xe8,
            0x7a, 0x34, 0x59, 0x9d, 0xe3, 0xe4, 0xe3, 0xf8,
        };
        static const uint8_t high_s_signature[] = {
            0x30, 0x45, 0x02, 0x20, 0x2c, 0x27, 0x65, 0xc3,
            0x93, 0x17, 0xd3, 0x3a, 0xb0, 0x6e, 0x69, 0x6c,
            0x0a, 0x93, 0x4a, 0xff, 0xb0, 0xe6, 0x6b, 0xa5,
            0x2e, 0x80, 0x0f, 0xd6, 0x85, 0x3f, 0x8f, 0x10,
            0x5d, 0xd0, 0xe1, 0xb4, 0x02, 0x21, 0x00, 0x84,
            0xea, 0x62, 0x50, 0x59, 0x89, 0x60, 0x7a, 0x6b,
            0xe6, 0x27, 0x7d, 0xe7, 0x7e, 0x71, 0xf6, 0x54,
            0xd7, 0x30, 0x67, 0xe4, 0xc6, 0xe8, 0x71, 0x8d,
            0x55, 0x50, 0x71, 0xf1, 0xa6, 0xe9, 0x00,
        };
        struct privkey key;
        privkey_init(&key);
        key.vch[31] = 1;
        key.fValid = true;
        key.fCompressed = true;
        struct pubkey pubkey;
        ASSERT(privkey_get_pubkey(&key, &pubkey));
        struct key_id key_id = pubkey_get_id(&pubkey);

        wallet = zcl_calloc(1, sizeof(*wallet), "test_app_event_wallet");
        ASSERT(wallet != NULL);
        wallet_init(wallet);
        wallet_ready = true;
        ASSERT(wallet_import_key(wallet, &key));
        size_t wallet_tx_count = wallet->num_wallet_tx;
        size_t wallet_spent_count = wallet->num_spent;

        struct zcl_app_event_scope_v1 scope = test_social_scope();
        struct zcl_app_event_binding_test_spec_v1 binding_spec;
        memset(&binding_spec, 0, sizeof(binding_spec));
        binding_spec.struct_size = sizeof(binding_spec);
        binding_spec.operation = ZCL_APP_WALLET_OP_SIGN_EVENT_V1;
        binding_spec.app_generation = 7;
        binding_spec.grant_revision = 3;
        memset(binding_spec.grant_id, 0x11, sizeof(binding_spec.grant_id));
        memset(binding_spec.manifest_digest, 0x22,
               sizeof(binding_spec.manifest_digest));
        binding_spec.scope = scope;
        memcpy(binding_spec.author_key_id, key_id.id.data,
               sizeof(binding_spec.author_key_id));
        binding_spec.grant_active = true;
        struct zcl_app_event_intent_v1 intent =
            test_social_intent(payload, sizeof(payload) - 1);
        struct zcl_app_signed_event_v1 event;
        char why[256];

        ASSERT(zcl_app_event_signing_binding_v1_test_create(
            &binding_spec, &binding, why, sizeof(why)));

        ASSERT(zcl_app_signed_event_v1_sign_wallet(
            &intent, binding, wallet, &event, why, sizeof(why)));
        ASSERT(zcl_app_signed_event_v1_verify(
            &event, &scope, why, sizeof(why)));
        ASSERT(event.signature_len > 0 &&
               event.signature_len <= ZCL_APP_EVENT_SIGNATURE_MAX);
        ASSERT(memcmp(event.event_id, expected_event_id,
                      sizeof(expected_event_id)) == 0);

        uint8_t canonical[256];
        size_t canonical_len = 0;
        ASSERT(zcl_app_signed_event_v1_canonical_unsigned(
            &event, canonical, sizeof(canonical), &canonical_len,
            why, sizeof(why)));
        ASSERT(canonical_len == 187);
        ASSERT(canonical[0] == 1 && canonical[1] == 0 &&
               canonical[2] == 0 && canonical[3] == 0);
        ASSERT(canonical[36] == 6 && canonical[37] == 0);
        size_t required = 0;
        ASSERT(!zcl_app_signed_event_v1_canonical_unsigned(
            &event, NULL, 0, &required, why, sizeof(why)));
        ASSERT(required == canonical_len);

        struct zcl_app_signed_event_v1 replay;
        ASSERT(zcl_app_signed_event_v1_sign_wallet(
            &intent, binding, wallet, &replay,
            why, sizeof(why)));
        ASSERT(replay.signature_len == event.signature_len);
        ASSERT(memcmp(replay.signature, event.signature,
                      event.signature_len) == 0);
        ASSERT(memcmp(replay.event_id, event.event_id,
                      sizeof(event.event_id)) == 0);

        static const uint8_t tampered_payload[] = "jello zclassic23";
        struct zcl_app_signed_event_v1 bad = event;
        bad.payload.data = tampered_payload;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.app_id[0] = 'S';
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.topic[0] = 'x';
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.chain_id[0] ^= 1;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.author_key_id[0] ^= 1;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.author_pubkey[8] ^= 1;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.kind++;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.created_at++;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.sequence = 2;
        bad.previous_event_id[0] = 1;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.signature[bad.signature_len - 1] ^= 1;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.signature_len = ZCL_APP_EVENT_SIGNATURE_MAX + 1u;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        memset(bad.signature, 0, sizeof(bad.signature));
        memcpy(bad.signature, high_s_signature, sizeof(high_s_signature));
        bad.signature_len = sizeof(high_s_signature);
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        ASSERT(event.signature_len < sizeof(event.signature));
        bad = event;
        bad.signature[event.signature_len] = 1;
        ASSERT(zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.event_id[0] ^= 1;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.previous_event_id[0] = 1;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.payload.len = ZCL_APP_EVENT_PAYLOAD_MAX + 1u;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));
        bad = event;
        bad.payload.data = NULL;
        ASSERT(!zcl_app_signed_event_v1_verify(
            &bad, &scope, why, sizeof(why)));

        struct zcl_app_signed_event_v1 zero_event = {0};
        struct zcl_app_signed_event_v1 denied = event;
        struct zcl_app_event_signing_binding_v1 *denied_binding = NULL;
        struct zcl_app_event_binding_test_spec_v1 denied_spec = binding_spec;
        denied_spec.grant_active = false;
        ASSERT(!zcl_app_event_signing_binding_v1_test_create(
            &denied_spec, &denied_binding, why, sizeof(why)));
        ASSERT(denied_binding == NULL);
        ASSERT(!zcl_app_signed_event_v1_sign_wallet(
            &intent, NULL, wallet, &denied, why, sizeof(why)));
        ASSERT(memcmp(&denied, &zero_event, sizeof(denied)) == 0);

        struct zcl_app_event_binding_test_spec_v1 wrong_spec = binding_spec;
        wrong_spec.author_key_id[0] ^= 1;
        struct zcl_app_event_signing_binding_v1 *wrong_binding = NULL;
        ASSERT(zcl_app_event_signing_binding_v1_test_create(
            &wrong_spec, &wrong_binding, why, sizeof(why)));
        denied = event;
        ASSERT(!zcl_app_signed_event_v1_sign_wallet(
            &intent, wrong_binding, wallet, &denied, why, sizeof(why)));
        ASSERT(memcmp(&denied, &zero_event, sizeof(denied)) == 0);
        zcl_app_event_signing_binding_v1_test_destroy(wrong_binding);

        struct zcl_app_event_scope_v1 small_scope = scope;
        small_scope.max_event_bytes = (uint32_t)(canonical_len + 2 +
            event.signature_len);
        struct zcl_app_event_binding_test_spec_v1 small_spec = binding_spec;
        small_spec.scope = small_scope;
        struct zcl_app_event_signing_binding_v1 *small_binding = NULL;
        ASSERT(zcl_app_event_signing_binding_v1_test_create(
            &small_spec, &small_binding, why, sizeof(why)));
        denied = event;
        ASSERT(zcl_app_signed_event_v1_sign_wallet(
            &intent, small_binding, wallet, &denied, why, sizeof(why)));
        ASSERT(zcl_app_signed_event_v1_verify(
            &denied, &small_scope, why, sizeof(why)));
        zcl_app_event_signing_binding_v1_test_destroy(small_binding);
        small_scope.max_event_bytes--;
        small_spec.scope = small_scope;
        small_binding = NULL;
        ASSERT(zcl_app_event_signing_binding_v1_test_create(
            &small_spec, &small_binding, why, sizeof(why)));
        denied = event;
        ASSERT(!zcl_app_signed_event_v1_sign_wallet(
            &intent, small_binding, wallet, &denied, why, sizeof(why)));
        ASSERT(memcmp(&denied, &zero_event, sizeof(denied)) == 0);
        zcl_app_event_signing_binding_v1_test_destroy(small_binding);

        struct zcl_app_event_scope_v1 malformed_scope = scope;
        memset(malformed_scope.app_id, 'x', sizeof(malformed_scope.app_id));
        struct zcl_app_event_binding_test_spec_v1 malformed_spec = binding_spec;
        malformed_spec.scope = malformed_scope;
        struct zcl_app_event_signing_binding_v1 *malformed_binding = NULL;
        ASSERT(!zcl_app_event_signing_binding_v1_test_create(
            &malformed_spec, &malformed_binding, why, sizeof(why)));
        ASSERT(malformed_binding == NULL);

        uint8_t failed_id[32];
        memset(failed_id, 0xff, sizeof(failed_id));
        bad = event;
        bad.version = 0;
        ASSERT(!zcl_app_signed_event_v1_id(
            &bad, failed_id, why, sizeof(why)));
        uint8_t zero_id[32] = {0};
        ASSERT(memcmp(failed_id, zero_id, sizeof(failed_id)) == 0);
        ASSERT(wallet->num_wallet_tx == wallet_tx_count);
        ASSERT(wallet->num_spent == wallet_spent_count);
        PASS();
    } _test_next:;
    if (wallet) {
        if (wallet_ready)
            wallet_free(wallet);
        free(wallet);
    }
    zcl_app_event_signing_binding_v1_test_destroy(binding);
    return failures;
}

static int test_social_sim(void)
{
    int failures = 0;
    TEST("dev platform: social censorship proof is deterministic") {
        const uint64_t seed = UINT64_C(0x534f4349414c0001);
        struct zcl_social_sim_report a, b;
        ASSERT(zcl_social_app_sim_run(seed, &a));
        ASSERT(zcl_social_app_sim_run(seed, &b));
        ASSERT(a.censorship_bypassed);
        ASSERT(a.partition_rejoin_converged);
        ASSERT(a.late_joiner_caught_up);
        ASSERT(a.invalid_signature_rejected);
        ASSERT(a.real_secp256k1_verified);
        ASSERT(a.tampered_payload_rejected);
        ASSERT(a.wrong_author_rejected);
        ASSERT(a.forged_event_id_distinct);
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
 * the result mapper. The switch selects retained machinery only; public
 * publication entrypoints remain contained for every value. */
static int test_native_activation_switch(void)
{
    int failures = 0;
    TEST("dev platform: retained native engine selector defaults OFF") {
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

static int test_watch_relevance(void)
{
    int failures = 0;
    TEST("dev platform: watcher ignores transient lint fixtures, keeps real edits") {
        /* The bug this guards: the persistent watcher fired a phantom reload
         * cycle every test-suite run because test_make_lint_gates.c writes
         * `_*fixture*` .c files under app/, lib/, domain/ then deletes them. */
        static const char *const fixtures[] = {
            "app/_lint_gate_fixture_tmp.c",
            "app/_node_db_exec_lint_fixture_probe_tmp.c",
            "app/_e10_offshape_fixture_probe_tmp.c",
            "app/controllers/src/_coins_lookup_guard_fixture_tmp.c",
            "app/jobs/src/_e5_stage_fixture_tmp_stage.c",
            "lib/storage/src/_e4_pure_fixture_projection.c",
            "domain/wallet/src/_domain_purity_fixture_tmp.c",
        };
        for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++)
            ASSERT(!zcl_devloop_path_is_relevant(fixtures[i]));

        /* Real edits — including the genuine fixture SOURCES under
         * lib/test/fixtures/ (no leading underscore) — must still fire. */
        static const char *const real[] = {
            "app/jobs/src/stage_repair_reducer_frontier_coin.c",
            "core/consensus/src/check_block.c",
            "tools/dev/devloop_watch.c",
            "lib/test/fixtures/raw_sqlite_step_fixture.c",
            "docs/HANDOFF.md",
            "Makefile",
            "app/controllers/include/controllers/agent_impact_rules.def",
        };
        for (size_t i = 0; i < sizeof(real) / sizeof(real[0]); i++)
            ASSERT(zcl_devloop_path_is_relevant(real[i]));

        /* Editor temp / build / vcs noise stays filtered. */
        ASSERT(!zcl_devloop_path_is_relevant("app/services/src/foo.c~"));
        ASSERT(!zcl_devloop_path_is_relevant("build/bin/zclassic23"));
        ASSERT(!zcl_devloop_path_is_relevant(".git/index"));
        ASSERT(!zcl_devloop_path_is_relevant(""));
        ASSERT(!zcl_devloop_path_is_relevant(NULL));
        PASS();
    } _test_next:;
    return failures;
}

static void fill_hex(char out[65], char digit)
{
    memset(out, digit, 64);
    out[64] = 0;
}

static bool failure_record_path(char out[PATH_MAX], const char *home,
                                const struct zcl_dev_failure_record *record,
                                const char *leaf)
{
    int n = snprintf(
        out, PATH_MAX,
        "%s/.local/state/zclassic23-dev/workspaces/%s/failures/%s/%s",
        home, record->workspace_id, record->failure_id, leaf);
    return n > 0 && n < PATH_MAX;
}

static bool run_failure_store_fixture(void)
{
    bool ok = false;
    char home[PATH_MAX], repo1[PATH_MAX], repo2[PATH_MAX], repo3[PATH_MAX];
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    if (getenv("HOME") && !saved_home)
        return false;
    test_make_tmpdir(home, sizeof(home), "dev_platform", "failure_store");
    if (snprintf(repo1, sizeof(repo1), "%s/repo1", home) <= 0 ||
        snprintf(repo2, sizeof(repo2), "%s/repo2", home) <= 0 ||
        snprintf(repo3, sizeof(repo3), "%s/repo3", home) <= 0 ||
        mkdir(repo1, 0700) != 0 || mkdir(repo2, 0700) != 0 ||
        mkdir(repo3, 0700) != 0 || setenv("HOME", home, 1) != 0)
        goto cleanup;

#define FS_REQUIRE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "failure-store fixture failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                              \
            goto cleanup;                                                    \
        }                                                                    \
    } while (0)

    char source[65], mutation1[65], mutation2[65], execution1[65],
         execution2[65], why[192] = {0};
    fill_hex(source, 'a');
    fill_hex(mutation1, 'b');
    fill_hex(mutation2, 'd');
    fill_hex(execution1, 'c');
    fill_hex(execution2, 'e');

    struct zcl_dev_failure_record record, readback;
    FS_REQUIRE(zcl_dev_failure_read_latest(repo1, &readback, why,
                                           sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_ABSENT);
    char state_dir[PATH_MAX];
    FS_REQUIRE(zcl_devloop_workspace_state_dir(repo1, state_dir,
                                               sizeof(state_dir)));
    FS_REQUIRE(access(state_dir, F_OK) != 0); /* reads create no state */

    char normalized[ZCL_DEV_FAILURE_ERROR_MAX], pinned[65];
    FS_REQUIRE(zcl_dev_failure_normalize_error(
        " \tfoo.c:1: error: bad   token \r\nignored",
        normalized));
    FS_REQUIRE(strcmp(normalized, "foo.c:1: error: bad token") == 0);
    FS_REQUIRE(zcl_dev_failure_compute_id(
        source, "verify.compile", "foo.c:1: error: bad", pinned));
    FS_REQUIRE(strcmp(
        pinned,
        "be4309e5f776d702bf96a5ed6d36b5be1dfd559176bb7b767860ffd00af14b37")
        == 0);

    const char *error = "foo.c:12:5: error: bad token";
    const char *capsule =
        "first_error=foo.c:12:5: error: bad token\n\"quoted\"\\tail";
    FS_REQUIRE(zcl_dev_failure_record_failure(
        repo1, source, mutation1, execution1, "verify.compile", error,
        capsule, "dev.ff", &record, why, sizeof(why)));
    FS_REQUIRE(record.repeat_count == 1);
    FS_REQUIRE(strcmp(record.first_source_mutation, mutation1) == 0);
    FS_REQUIRE(strcmp(record.first_execution_id, execution1) == 0);
    FS_REQUIRE(strcmp(record.retry_command, "dev.ff") == 0);
    FS_REQUIRE(zcl_dev_failure_read(repo1, record.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_FOUND);
    FS_REQUIRE(strcmp(readback.record_digest, record.record_digest) == 0);
    FS_REQUIRE(zcl_dev_failure_match_latest(
        repo1, source, mutation1, execution1, "verify.compile", &readback,
        why, sizeof(why)));
    FS_REQUIRE(!zcl_dev_failure_match_latest(
        repo1, source, mutation2, execution1, "verify.compile", &readback,
        why, sizeof(why)) && why[0] == 0);
    FS_REQUIRE(!zcl_dev_failure_note_coalesced(
        repo1, record.failure_id, source, mutation2, execution1,
        "verify.compile", &readback, why, sizeof(why)));
    FS_REQUIRE(zcl_dev_failure_note_coalesced(
        repo1, record.failure_id, source, mutation1, execution1,
        "verify.compile", &readback, why, sizeof(why)));
    FS_REQUIRE(readback.repeat_count == 2);

    struct zcl_dev_failure_record observed;
    FS_REQUIRE(zcl_dev_failure_record_failure(
        repo1, source, mutation2, execution2, "verify.compile", error,
        capsule, "dev.ff", &observed, why, sizeof(why)));
    FS_REQUIRE(strcmp(observed.failure_id, record.failure_id) == 0);
    FS_REQUIRE(observed.repeat_count == 3);
    FS_REQUIRE(strcmp(observed.first_source_mutation, mutation1) == 0);
    FS_REQUIRE(strcmp(observed.first_execution_id, execution1) == 0);
    FS_REQUIRE(zcl_dev_failure_match_latest(
        repo1, source, mutation2, execution2, "verify.compile", &readback,
        why, sizeof(why)));

    pid_t children[8];
    for (size_t i = 0; i < 8; i++) {
        children[i] = fork();
        FS_REQUIRE(children[i] >= 0);
        if (children[i] == 0) {
            struct zcl_dev_failure_record child_record;
            char child_why[128] = {0};
            bool child_ok = zcl_dev_failure_note_coalesced(
                repo1, record.failure_id, source, mutation2, execution2,
                "verify.compile", &child_record, child_why,
                sizeof(child_why));
            _exit(child_ok ? 0 : 90);
        }
    }
    for (size_t i = 0; i < 8; i++) {
        int status = 0;
        FS_REQUIRE(waitpid(children[i], &status, 0) == children[i]);
        FS_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    FS_REQUIRE(zcl_dev_failure_read(repo1, record.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_FOUND);
    FS_REQUIRE(readback.repeat_count == 11);
    FS_REQUIRE(zcl_dev_failure_read_latest(repo2, &readback, why,
                                           sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_ABSENT);

    /* Cycle verdicts share the exact worktree scope and distinguish absent,
     * found, and invalid sealed state. */
    static const char cycle[] =
        "{\"schema\":\"zcl.dev_cycle.v1\",\"producer\":\"test\","
        "\"status\":\"passed\",\"action\":\"check\","
        "\"reason\":\"fixture\",\"phase\":\"verify\","
        "\"runtime_published\":false,\"elapsed_ms\":1,\"files\":[]}";
    FS_REQUIRE(zcl_devloop_cycle_state_write(
        repo1, cycle, sizeof(cycle) - 1, why, sizeof(why)));
    char cycle_out[4096];
    size_t cycle_len = 0;
    int64_t cycle_epoch = 0;
    FS_REQUIRE(zcl_devloop_cycle_state_read(
        repo1, cycle_out, sizeof(cycle_out), &cycle_len, &cycle_epoch,
        why, sizeof(why)) == ZCL_DEVLOOP_STATE_FOUND);
    FS_REQUIRE(cycle_len == sizeof(cycle) - 1 &&
               memcmp(cycle_out, cycle, cycle_len) == 0 && cycle_epoch > 0);
    int64_t first_cycle_epoch = cycle_epoch;
    FS_REQUIRE(zcl_devloop_cycle_state_read(
        repo2, cycle_out, sizeof(cycle_out), &cycle_len, &cycle_epoch,
        why, sizeof(why)) == ZCL_DEVLOOP_STATE_ABSENT);

    char path[PATH_MAX];
    FS_REQUIRE(snprintf(path, sizeof(path), "%s/native-cycle.json",
                        state_dir) > 0);
    char sealed_cycle_record[32768];
    FILE *cycle_file = fopen(path, "r");
    FS_REQUIRE(cycle_file != NULL);
    size_t sealed_cycle_len =
        fread(sealed_cycle_record, 1, sizeof(sealed_cycle_record), cycle_file);
    FS_REQUIRE(!ferror(cycle_file) && fclose(cycle_file) == 0 &&
               sealed_cycle_len > 0 &&
               sealed_cycle_len < sizeof(sealed_cycle_record));
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    FS_REQUIRE(fd >= 0 && pwrite(fd, "X", 1, 0) == 1 && close(fd) == 0);
    FS_REQUIRE(zcl_devloop_cycle_state_read(
        repo1, cycle_out, sizeof(cycle_out), &cycle_len, &cycle_epoch,
        why, sizeof(why)) == ZCL_DEVLOOP_STATE_INVALID);
    FS_REQUIRE(!zcl_devloop_cycle_state_write(
        repo1, cycle, sizeof(cycle) - 1, why, sizeof(why)));

    /* Publication fails closed on corrupt current state. Physical restoration
     * of the exact prior sealed generation recovers without lowering epoch. */
    cycle_file = fopen(path, "w");
    FS_REQUIRE(cycle_file != NULL &&
               fwrite(sealed_cycle_record, 1, sealed_cycle_len, cycle_file) ==
                   sealed_cycle_len &&
               fflush(cycle_file) == 0 && fsync(fileno(cycle_file)) == 0 &&
               fclose(cycle_file) == 0);
    int64_t restored_epoch = 0;
    FS_REQUIRE(zcl_devloop_cycle_state_read(
        repo1, cycle_out, sizeof(cycle_out), &cycle_len, &restored_epoch,
        why, sizeof(why)) == ZCL_DEVLOOP_STATE_FOUND);
    FS_REQUIRE(restored_epoch == first_cycle_epoch);
    FS_REQUIRE(zcl_devloop_cycle_state_write(
        repo1, cycle, sizeof(cycle) - 1, why, sizeof(why)));
    int64_t second_epoch = 0;
    FS_REQUIRE(zcl_devloop_cycle_state_read(
        repo1, cycle_out, sizeof(cycle_out), &cycle_len, &second_epoch,
        why, sizeof(why)) == ZCL_DEVLOOP_STATE_FOUND);
    FS_REQUIRE(second_epoch == restored_epoch + 1);

    /* Inode timestamp changes are not wait authority. */
    struct timespec times[2] = {
        { .tv_sec = 1, .tv_nsec = 0 }, { .tv_sec = 1, .tv_nsec = 0 }
    };
    FS_REQUIRE(utimensat(AT_FDCWD, path, times, 0) == 0);
    int64_t timestamp_tamper_epoch = 0;
    FS_REQUIRE(zcl_devloop_cycle_state_read(
        repo1, cycle_out, sizeof(cycle_out), &cycle_len,
        &timestamp_tamper_epoch, why, sizeof(why)) ==
               ZCL_DEVLOOP_STATE_FOUND);
    FS_REQUIRE(timestamp_tamper_epoch == second_epoch);

    /* Concurrent writers serialize through the workspace capability lock. */
    pid_t cycle_children[8];
    for (size_t i = 0; i < 8; i++) {
        cycle_children[i] = fork();
        FS_REQUIRE(cycle_children[i] >= 0);
        if (cycle_children[i] == 0) {
            char child_why[128] = {0};
            bool child_ok = zcl_devloop_cycle_state_write(
                repo1, cycle, sizeof(cycle) - 1, child_why,
                sizeof(child_why));
            _exit(child_ok ? 0 : 91);
        }
    }
    for (size_t i = 0; i < 8; i++) {
        int status = 0;
        FS_REQUIRE(waitpid(cycle_children[i], &status, 0) ==
                   cycle_children[i]);
        FS_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    int64_t concurrent_epoch = 0;
    FS_REQUIRE(zcl_devloop_cycle_state_read(
        repo1, cycle_out, sizeof(cycle_out), &cycle_len, &concurrent_epoch,
        why, sizeof(why)) == ZCL_DEVLOOP_STATE_FOUND);
    FS_REQUIRE(concurrent_epoch == second_epoch + 8);

    /* A syntactically valid-looking counter edit still breaks its SHA3 seal. */
    FS_REQUIRE(failure_record_path(path, home, &record,
                                   "observations.json"));
    char observation_body[1024];
    FILE *observation_file = fopen(path, "r");
    FS_REQUIRE(observation_file != NULL);
    size_t observation_len = fread(observation_body, 1,
                                   sizeof(observation_body) - 1,
                                   observation_file);
    FS_REQUIRE(!ferror(observation_file) && fclose(observation_file) == 0 &&
               observation_len > 0);
    observation_body[observation_len] = 0;
    char *count_value = strstr(observation_body, "\"count\":11");
    FS_REQUIRE(count_value != NULL);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    off_t count_digit = (off_t)(count_value - observation_body) +
                        (off_t)strlen("\"count\":1");
    FS_REQUIRE(fd >= 0 && pwrite(fd, "2", 1, count_digit) == 1 &&
               close(fd) == 0);
    FS_REQUIRE(zcl_dev_failure_read(repo1, record.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_INVALID);

    /* A second workspace proves private-mode, hardlink, and symlink rejection
     * without relying on the now-intentionally-corrupt first record. */
    struct zcl_dev_failure_record record2;
    FS_REQUIRE(zcl_dev_failure_record_failure(
        repo2, source, mutation1, execution1, "verify.compile", error,
        capsule, "dev.ff", &record2, why, sizeof(why)));
    FS_REQUIRE(failure_record_path(path, home, &record2, "base.json"));
    FS_REQUIRE(chmod(path, 0644) == 0);
    FS_REQUIRE(zcl_dev_failure_read(repo2, record2.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_INVALID);
    FS_REQUIRE(chmod(path, 0600) == 0);
    char alias[PATH_MAX];
    FS_REQUIRE(snprintf(alias, sizeof(alias), "%s.hardlink", path) > 0);
    FS_REQUIRE(link(path, alias) == 0);
    FS_REQUIRE(zcl_dev_failure_read(repo2, record2.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_INVALID);
    FS_REQUIRE(unlink(alias) == 0);
    FS_REQUIRE(zcl_dev_failure_read(repo2, record2.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_FOUND);
    FS_REQUIRE(unlink(path) == 0 && symlink("/etc/passwd", path) == 0);
    FS_REQUIRE(zcl_dev_failure_read(repo2, record2.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_INVALID);

    /* Third workspace: an unsealed extra JSON field is rejected. */
    struct zcl_dev_failure_record record3;
    FS_REQUIRE(zcl_dev_failure_record_failure(
        repo3, source, mutation1, execution1, "verify.compile", error,
        capsule, "dev.ff", &record3, why, sizeof(why)));
    FS_REQUIRE(failure_record_path(path, home, &record3, "base.json"));
    char json_body[4096];
    FILE *f = fopen(path, "r");
    FS_REQUIRE(f != NULL);
    size_t json_len = fread(json_body, 1, sizeof(json_body) - 1, f);
    FS_REQUIRE(!ferror(f) && fclose(f) == 0 && json_len > 2);
    while (json_len > 0 &&
           (json_body[json_len - 1] == '\n' ||
            json_body[json_len - 1] == '\r'))
        json_len--;
    FS_REQUIRE(json_len > 0 && json_body[json_len - 1] == '}');
    json_len--;
    static const char extra[] = ",\"unsealed_extra\":true}\n";
    FS_REQUIRE(json_len + sizeof(extra) < sizeof(json_body));
    memcpy(json_body + json_len, extra, sizeof(extra) - 1);
    json_len += sizeof(extra) - 1;
    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    FS_REQUIRE(fd >= 0 &&
               write(fd, json_body, json_len) == (ssize_t)json_len &&
               fsync(fd) == 0 && close(fd) == 0);
    FS_REQUIRE(zcl_dev_failure_read(repo3, record3.failure_id, &readback,
                                    why, sizeof(why)) ==
               ZCL_DEV_FAILURE_LOOKUP_INVALID);

    ok = true;

cleanup:
    if (saved_home) {
        (void)setenv("HOME", saved_home, 1);
        free(saved_home);
    } else
        (void)unsetenv("HOME");
    test_rm_rf_recursive(home);
#undef FS_REQUIRE
    return ok;
}

static int test_failure_store(void)
{
    int failures = 0;
    TEST("dev platform: failure receipts are scoped, sealed, concurrent, and fail closed") {
        ASSERT(run_failure_store_fixture());
        PASS();
    } _test_next:;
    return failures;
}

/* A4: distill_first_error picks the first actionable line (compiler
 * ": error:" or test FAIL/Assertion/EXPECT) and falls back cleanly when no
 * pattern matches. Exercised via the thin zcl_devloop_distill_first_error
 * wrapper (the underlying function is static in devloop_cycle.c). */
static int test_distill_first_error(void)
{
    int failures = 0;
    TEST("dev platform: distill_first_error extracts the first actionable line") {
        char dst[256];

        /* A compiler ": error:" line is extracted, newline-stripped, even when
         * it is not the last line of output. */
        const char *compiler =
            "cc -c foo.c\n"
            "foo.c: In function 'bar':\n"
            "foo.c:12:5: error: 'x' undeclared (first use in this function)\n"
            "make: *** [foo.o] Error 1\n";
        ASSERT(zcl_devloop_distill_first_error(compiler, strlen(compiler),
                                               dst, sizeof(dst)));
        ASSERT(strcmp(dst,
            "foo.c:12:5: error: 'x' undeclared (first use in this function)")
            == 0);

        /* A test FAIL line is extracted. */
        const char *testfail =
            "running group vcs_devloop\n"
            "[dev-watch-selftest] FAIL: stage command order is wrong\n"
            "1 failure\n";
        ASSERT(zcl_devloop_distill_first_error(testfail, strlen(testfail),
                                               dst, sizeof(dst)));
        ASSERT(strcmp(dst,
            "[dev-watch-selftest] FAIL: stage command order is wrong") == 0);

        /* An Assertion line is extracted (the first actionable line wins over
         * a later error-looking line). */
        const char *assertion =
            "ok: sanity\n"
            "Assertion `n > 0 && n < sizeof(body)' failed.\n";
        ASSERT(zcl_devloop_distill_first_error(assertion, strlen(assertion),
                                               dst, sizeof(dst)));
        ASSERT(strcmp(dst, "Assertion `n > 0 && n < sizeof(body)' failed.")
            == 0);

        /* No matching pattern => false, dst emptied (caller falls back to the
         * tail). */
        const char *clean = "cc -c foo.c\nlink ok\nall good here\n";
        ASSERT(!zcl_devloop_distill_first_error(clean, strlen(clean),
                                                dst, sizeof(dst)));
        ASSERT(dst[0] == 0);

        /* Bounded copy: a long matching line is truncated to dstcap-1, never
         * overruns, always NUL-terminated. */
        char tiny[8];
        const char *longline = "src.c:1:1: error: this line is far too long\n";
        ASSERT(zcl_devloop_distill_first_error(longline, strlen(longline),
                                               tiny, sizeof(tiny)));
        ASSERT(strlen(tiny) == sizeof(tiny) - 1);

        /* Defensive: NULL / zero-cap inputs are rejected without a crash. */
        ASSERT(!zcl_devloop_distill_first_error(NULL, 0, dst, sizeof(dst)));
        ASSERT(!zcl_devloop_distill_first_error(compiler, strlen(compiler),
                                                dst, 0));

        struct zcl_devloop_process_result result;
        memset(&result, 0, sizeof(result));
        result.exit_code = 1;
        (void)snprintf(
            result.output, sizeof(result.output),
            "raw compiler output\n"
            "[agent-fast-ci] FIRST-ERROR[compile]: "
            "foo.c:12:5: error: bad token\n"
            "[agent-fast-ci] FAIL: rung compile failed (exit 2)\n");
        result.output_len = strlen(result.output);
        char classified[512];
        ASSERT(zcl_devloop_deterministic_compile_failure(
            &result, classified));
        ASSERT(strcmp(classified, "foo.c:12:5: error: bad token") == 0);

        (void)snprintf(
            result.output, sizeof(result.output),
            "source.c:9:1: error: literal says "
            "[agent-fast-ci] FIRST-ERROR[compile]: "
            "fake.c:1:1: error: fake\n");
        result.output_len = strlen(result.output);
        ASSERT(!zcl_devloop_deterministic_compile_failure(
            &result, classified)); /* marker is not at a line boundary */

        (void)snprintf(
            result.output, sizeof(result.output),
            "[agent-fast-ci] FIRST-ERROR[compile]: "
            "foo.c:12:5: error: Killed\n");
        result.output_len = strlen(result.output);
        ASSERT(!zcl_devloop_deterministic_compile_failure(
            &result, classified));
        result.timed_out = true;
        ASSERT(!zcl_devloop_deterministic_compile_failure(
            &result, classified));
        result.timed_out = false;
        result.term_signal = 9;
        ASSERT(!zcl_devloop_deterministic_compile_failure(
            &result, classified));
        PASS();
    } _test_next:;
    return failures;
}

int test_dev_platform(void)
{
    int failures = 0;
    failures += test_failure_store();
    failures += test_distill_first_error();
    failures += test_menu_and_search();
    failures += test_change_classification();
    failures += test_watcher_publication_containment();
    failures += test_watch_relevance();
    failures += test_core_classification();
    failures += test_core_refusal_envelope();
    failures += test_core_refusal_cycle();
    failures += test_core_refusal_token();
    failures += test_public_app_abi();
    failures += test_app_definition_compiler();
    failures += test_strict_dev_app_producers();
    failures += test_app_definition_hostile_fixtures();
    failures += test_signed_app_events();
    failures += test_social_sim();
    failures += test_native_activation_switch();
    failures += test_native_activation_request_builder();
    failures += test_native_activation_result_mapping();
    printf("=== dev_platform: %d failures ===\n", failures);
    return failures;
}
