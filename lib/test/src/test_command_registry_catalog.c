/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Golden contract tests for the native command registry catalog
 * (docs/NATIVE_COMMAND_INTERFACE.md §20). Proves the composition-root catalog
 * is well-formed, shallow, budgeted, fail-closed for planned leaves, and that
 * every READY leaf has a live binding — without contacting a node.
 */

#include "test/test_helpers.h"

#include "config/command_catalog.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "json/json.h"
#include "mcp/rpc_client.h"

#include <string.h>

static const struct zcl_command_spec *find_spec(
    const struct zcl_command_registry *reg, const char *path)
{
    for (size_t i = 0; i < reg->count; i++)
        if (strcmp(reg->commands[i].path, path) == 0)
            return &reg->commands[i];
    return NULL;
}

static bool exec_leaf(const struct zcl_command_registry *reg,
                      const struct zcl_command_spec *spec,
                      char *out, size_t out_size,
                      enum zcl_command_exit *exit_code)
{
    struct zcl_command_context ctx = {
        .registry = reg,
        .granted_capabilities = ~(uint64_t)0,
        .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
    };
    struct json_value input;
    json_init(&input);
    json_set_object(&input);
    size_t n = zcl_command_registry_execute_json(reg, spec, &ctx, &input,
                                                 false, spec->path, "normal", 0,
                                                 0, NULL,
                                                 out, out_size, exit_code);
    json_free(&input);
    return n > 0;
}

static int test_catalog_wellformed(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("catalog validates and is non-trivial") {
        char why[128] = { 0 };
        ASSERT(reg != NULL);
        ASSERT(reg->count > 40);
        ASSERT(zcl_command_registry_validate(reg, why, sizeof(why)));
        PASS();
    } _test_next:;
    return failures;
}

/* Count registry entries (branches + leaves) rooted at or under `root`
 * (either path == root, or path starts with "root."). This is the native
 * command-registry analog of the old MCP-router per-domain tool counts
 * (see lib/test/src/test_mcp_controllers.c EXPECTED_TOTAL / EXPECTED_*):
 * as the zero-MCP migration (docs/work/MCP-REMOVAL-WORKLIST.md, W2) moves
 * agent-facing surface off the MCP router and onto this registry, this is
 * the "how big is the native surface, per domain" contract going forward.
 * Floors are set with headroom below the live count so routine additions
 * don't require bumping this file every commit (unlike the old
 * EXPECTED_TOTAL, which pinned an exact number). */
static size_t count_domain(const struct zcl_command_registry *reg,
                           const char *root)
{
    size_t n = 0;
    size_t len = strlen(root);
    for (size_t i = 0; i < reg->count; i++) {
        const char *p = reg->commands[i].path;
        if (strcmp(p, root) == 0) {
            n++;
            continue;
        }
        if (strncmp(p, root, len) == 0 && p[len] == '.')
            n++;
    }
    return n;
}

static int test_domain_leaf_counts(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("native registry per-domain counts meet the zero-MCP floor "
         "(replaces MCP router EXPECTED_TOTAL/EXPECTED_* — see "
         "docs/work/MCP-REMOVAL-WORKLIST.md W2)") {
        ASSERT(reg->count >= 120);
        ASSERT(count_domain(reg, "core") >= 60);
        ASSERT(count_domain(reg, "dev") >= 35);
        ASSERT(count_domain(reg, "ops") >= 15);
        ASSERT(count_domain(reg, "app") >= 3);
        ASSERT(count_domain(reg, "code") >= 5);
        ASSERT(count_domain(reg, "discover") >= 4);
        ASSERT(count_domain(reg, "status") >= 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_six_roots(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("root exposes exactly seven choices") {
        size_t roots = 0;
        for (size_t i = 0; i < reg->count; i++) {
            const char *p = reg->commands[i].parent;
            if (!p || !p[0])
                roots++;
        }
        ASSERT_EQ(roots, (size_t)7);
        ASSERT(find_spec(reg, "status") != NULL);
        ASSERT(find_spec(reg, "core") != NULL);
        ASSERT(find_spec(reg, "app") != NULL);
        ASSERT(find_spec(reg, "dev") != NULL);
        ASSERT(find_spec(reg, "ops") != NULL);
        ASSERT(find_spec(reg, "discover") != NULL);
        ASSERT(find_spec(reg, "code") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_root_menu_budget(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("root menu is within its byte budget") {
        size_t n = zcl_command_registry_menu_json(reg, "root", out,
                                                  sizeof(out));
        ASSERT(n > 0);
        ASSERT(n <= ZCL_COMMAND_ROOT_BUDGET);
        PASS();
    } _test_next:;
    return failures;
}

static int test_branch_menus_shallow(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("branch menus stay in budget and list only immediate children") {
        const char *branches[] = { "core", "core.chain", "core.wallet",
                                   "ops", "ops.debug", "discover" };
        for (size_t b = 0; b < sizeof(branches) / sizeof(branches[0]); b++) {
            size_t n = zcl_command_registry_menu_json(reg, branches[b], out,
                                                      sizeof(out));
            ASSERT(n > 0);
            ASSERT(n <= ZCL_COMMAND_BRANCH_BUDGET);
            struct json_value doc;
            ASSERT(json_read(&doc, out, n) && doc.type == JSON_OBJ);
            const struct json_value *children = json_get(&doc, "children");
            ASSERT(children && children->type == JSON_ARR);
            for (size_t i = 0; i < children->num_children; i++) {
                const char *cpath =
                    json_get_str(json_get(&children->children[i], "path"));
                const struct zcl_command_spec *cs = find_spec(reg, cpath);
                ASSERT(cs != NULL);
                ASSERT_STR_EQ(cs->parent, branches[b]);
            }
            json_free(&doc);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_search_bounded(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("search returns at most five ranked matches") {
        const char *queries[] = { "block", "wallet", "sync", "peer", "a" };
        for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
            size_t n = zcl_command_registry_search_json(reg, queries[q], out,
                                                        sizeof(out));
            ASSERT(n > 0);
            struct json_value doc;
            ASSERT(json_read(&doc, out, n) && doc.type == JSON_OBJ);
            const struct json_value *matches = json_get(&doc, "matches");
            ASSERT(matches && matches->type == JSON_ARR);
            ASSERT(matches->num_children <= ZCL_COMMAND_SEARCH_LIMIT);
            json_free(&doc);
        }
        PASS();
    } _test_next:;
    return failures;
}

static size_t search_total_matches(const struct zcl_command_registry *reg,
                                   const char *query, char *out, size_t out_sz)
{
    size_t n = zcl_command_registry_search_json(reg, query, out, out_sz);
    if (n == 0)
        return 0;
    struct json_value doc;
    if (!json_read(&doc, out, n) || doc.type != JSON_OBJ)
        return 0;
    const struct json_value *tm = json_get(&doc, "total_matches");
    size_t total = tm && tm->type == JSON_INT ? (size_t)json_get_int(tm) : 0;
    json_free(&doc);
    return total;
}

static int test_search_multiword(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("a space-separated query matches dotted command paths") {
        /* Regression: "dev loop" (space) previously matched nothing because
         * the literal string is never a substring of "dev.loop.status". Each
         * word must appear for a hit; a nonsense word blocks the match. */
        ASSERT(search_total_matches(reg, "dev loop", out, sizeof(out)) > 0);
        ASSERT(search_total_matches(reg, "loop dev", out, sizeof(out)) > 0);
        ASSERT(search_total_matches(reg, "dev zzznope", out, sizeof(out)) == 0);
        /* Single-word behavior is unchanged and still finds matches. */
        ASSERT(search_total_matches(reg, "loop", out, sizeof(out)) > 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_ready_leaves_bound(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every READY leaf has a non-NULL handler and bridge binding") {
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (s->mode == ZCL_COMMAND_MODE_BRANCH)
                continue;
            if (s->availability != ZCL_COMMAND_READY)
                continue;
            ASSERT(s->handler != NULL);
            if (s->handler == zcl_native_bridge_command)
                ASSERT(zcl_native_bridge_tool_for_path(s->path) != NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_bridge_bindings_reverse(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every sampled bridge binding names a READY native leaf") {
        const char *sample[] = {
            "status", "core.status", "core.chain.tip", "ops.health",
            "ops.metrics", "core.storage.query", "core.chain.block.get",
        };
        for (size_t i = 0; i < sizeof(sample) / sizeof(sample[0]); i++) {
            const char *tool = zcl_native_bridge_tool_for_path(sample[i]);
            ASSERT(tool != NULL && tool[0] != 0);
            const struct zcl_command_spec *s = find_spec(reg, sample[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
            ASSERT(s->handler == zcl_native_bridge_command);
        }
        PASS();
    } _test_next:;
    return failures;
}

/* Both mcp_node_rpc backends strip the JSON-RPC envelope on a node error and
 * return the bare error object. Locally-generated transport failures retain
 * the older {"error": {...}} wrapper. The native bridge must fail closed for
 * both shapes; otherwise -32601 (runtime/source skew) is projected as passing
 * command data. */
static const char *g_bridge_rpc_error_fixture;
static const char *g_bridge_rpc_method_fixture;

static char *bridge_rpc_error_mock(const char *method,
                                   const char *params_json)
{
    (void)params_json;
    if (!g_bridge_rpc_method_fixture ||
        strcmp(method, g_bridge_rpc_method_fixture) != 0 ||
        !g_bridge_rpc_error_fixture)
        return strdup("null");
    return strdup(g_bridge_rpc_error_fixture);
}

static int test_bridge_rpc_errors_fail_closed(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *s = find_spec(reg, "core.chain.tip");
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("native bridge rejects bare and wrapped JSON-RPC errors") {
        static const struct {
            const char *body;
            const char *message;
        } cases[] = {
            {
                "{\"code\":-32601,\"message\":\"Method not found\","
                "\"method\":\"getchaintip\"}",
                "Method not found",
            },
            {
                "{\"error\":{\"code\":-32603,"
                "\"message\":\"cannot connect to node\"}}",
                "cannot connect to node",
            },
        };
        ASSERT(s != NULL);
        ASSERT(s->handler == zcl_native_bridge_command);
        ASSERT(zcl_native_bridge_body_for_path(s->path) == NULL);
        ASSERT_STR_EQ(zcl_native_bridge_rpc_for_path(s->path), "getchaintip");

        g_bridge_rpc_method_fixture = "getchaintip";
        mcp_rpc_client_set_test_hook(bridge_rpc_error_mock);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            g_bridge_rpc_error_fixture = cases[i].body;
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_FAILED);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
            ASSERT(strstr(out, "\"status\":\"failed\"") != NULL);
            ASSERT(strstr(out, "\"code\":\"TOOL_ERROR\"") != NULL);
            ASSERT(strstr(out, cases[i].message) != NULL);
        }
        PASS();
    } _test_next:;
    g_bridge_rpc_error_fixture = NULL;
    g_bridge_rpc_method_fixture = NULL;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

/* Direct-RPC leaves intentionally preserve zclassicd-compatible result bodies,
 * which do not carry zclassic23 schema labels. Prove each binding instead
 * checks its stable minimum field/type shape, and prove the three legitimate
 * top-level arrays accept empty/valid lists while rejecting mixed elements. */
static int test_bridge_rpc_success_shapes_fail_closed(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    static const struct {
        const char *path;
        const char *valid;
        const char *invalid;
    } cases[] = {
        { "core.chain.tip",
          "{\"hash\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"height\":1}",
          "{\"hash\":1,\"height\":\"1\"}" },
        { "core.chain.mempool.status", "{\"size\":0,\"bytes\":0}",
          "{}" },
        { "core.chain.mempool.list",
          "[\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]",
          "[\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",7]" },
        { "core.sync.status", "{\"state\":\"syncing\",\"state_id\":1}",
          "{}" },
        { "core.sync.validation", "{\"state\":\"not_initialized\"}",
          "{\"state\":1}" },
        { "core.consensus.integrity",
          "{\"source\":\"persisted_consensus_tables\",\"master\":\"abc\"}",
          "{}" },
        { "core.consensus.utxo.commitment",
          "{\"sha3_hash\":\"abc\",\"height\":1,\"utxo_count\":2}",
          "{\"sha3_hash\":\"abc\",\"height\":1}" },
        { "core.consensus.mmb", "{\"mmr_root\":\"abc\",\"num_leaves\":1}",
          "{}" },
        { "core.network.status", "{\"connections\":0,\"networks\":[]}",
          "{\"connections\":\"0\",\"networks\":[]}" },
        { "core.network.peers.list", "[{\"id\":1,\"addr\":\"peer\"}]",
          "[{\"id\":1,\"addr\":\"peer\"},{}]" },
        { "core.network.peers.latency",
          "[{\"peer_id\":1,\"addr\":\"peer\"}]",
          "[{\"peer_id\":1,\"addr\":7}]" },
        { "core.network.onion.status",
          "{\"status\":\"ok\",\"healthy\":true,\"serving\":true}",
          "{}" },
        { "core.wallet.status", "{\"balance\":\"1.0\",\"txcount\":0}",
          "{}" },
        { "core.wallet.balance",
          "{\"transparent\":\"1.0\",\"total\":\"1.0\"}", "{}" },
        { "core.wallet.backup.status", "{\"running\":false,\"total_runs\":0}",
          "{}" },
        { "core.wallet.audit", "{\"chain_height\":1,\"summary\":{}}",
          "{\"chain_height\":1,\"summary\":[]}" },
        { "core.storage.stats", "{\"tip_height\":1,\"utxo_count\":2}",
          "{}" },
        { "core.mining.status", "{\"blocks\":1,\"chain\":\"main\"}",
          "{}" },
        { "core.mining.benchmark",
          "{\"primary_benchmark_source\":\"local\",\"primary_benchmarks\":[]}",
          "{}" },
        { "ops.health",
          "{\"status\":\"blocked\",\"healthy\":false,\"serving\":false}",
          "{\"status\":\"blocked\",\"healthy\":false}" },
        { "ops.lanes", "{\"status\":\"ok\",\"lanes\":[]}", "{}" },
        { "ops.recovery.status",
          "{\"ready_for_refold\":false,\"primary_blocker\":\"missing\"}",
          "{}" },
    };

    TEST("direct RPC bridge validates every legacy success shape") {
        mcp_rpc_client_set_test_hook(bridge_rpc_error_mock);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, cases[i].path);
            ASSERT(s != NULL);
            ASSERT(s->handler == zcl_native_bridge_command);
            ASSERT(zcl_native_bridge_body_for_path(s->path) == NULL);
            g_bridge_rpc_method_fixture =
                zcl_native_bridge_rpc_for_path(s->path);
            ASSERT(g_bridge_rpc_method_fixture != NULL);

            char out[ZCL_COMMAND_RESULT_BUDGET + 1];
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            g_bridge_rpc_error_fixture = cases[i].valid;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
            ASSERT(strstr(out, "\"ok\":true") != NULL);

            g_bridge_rpc_error_fixture = cases[i].invalid;
            code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_FAILED);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
            ASSERT(strstr(out, "\"code\":\"TOOL_ERROR\"") != NULL);
            ASSERT(strstr(out, "incompatible success body") != NULL);
        }

        /* Empty arrays are real no-data states, not schema failures. */
        const char *empty_paths[] = {
            "core.chain.mempool.list", "core.network.peers.list",
            "core.network.peers.latency",
        };
        for (size_t i = 0;
             i < sizeof(empty_paths) / sizeof(empty_paths[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, empty_paths[i]);
            ASSERT(s != NULL);
            g_bridge_rpc_method_fixture =
                zcl_native_bridge_rpc_for_path(s->path);
            g_bridge_rpc_error_fixture = "[]";
            char out[ZCL_COMMAND_RESULT_BUDGET + 1];
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
            ASSERT(strstr(out, "\"items\":[]") != NULL);
            ASSERT(strstr(out, "\"total_items\":0") != NULL);
        }
        PASS();
    } _test_next:;
    g_bridge_rpc_error_fixture = NULL;
    g_bridge_rpc_method_fixture = NULL;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

/* Stubs the one bounded cached status document core.status.brief projects,
 * so the envelope test below runs without a live node (mcp_node_rpc's
 * ZCL_TESTING hook wins over both RPC backends). */
static const char *g_status_brief_agent_fixture;

static char *status_brief_mock_rpc(const char *method,
                                   const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "agent") == 0 && g_status_brief_agent_fixture)
        return strdup(g_status_brief_agent_fixture);
    if (strcmp(method, "agent") == 0)
        return strdup(
            "{\"schema\":\"zcl.public_status.v1\","
            "\"partial_result\":false,"
            "\"served_height\":3117073,\"header_height\":3117074,"
            "\"served_height_known\":true,"
            "\"header_height_known\":true,"
            "\"gap\":1,\"peer_best_height\":3117074,"
            "\"peer_best_height_known\":true,"
            "\"target_height\":3117074,\"target_height_known\":true,"
            "\"chain_evidence_consistent\":true,"
            "\"sync_state\":\"at_tip\",\"serving\":true,"
            "\"healthy\":true,\"primary_blocker\":\"none\","
            "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
                "\"budget_ms\":250,\"partial_result\":false,"
                "\"budget_exceeded\":false},"
            "\"peers\":{\"total\":1},"
            "\"conditions\":{"
                "\"schema\":\"zcl.condition_engine_summary.v1\","
                "\"active_count\":2},"
            "\"resources\":{\"schema\":\"zcl.node_resources.v1\","
                "\"rss_mb\":512},"
            "\"reducer\":{\"tip_advance_age_seconds\":3},"
            "\"security_posture\":{"
                "\"schema\":\"zcl.security_posture.v1\","
                "\"anchor_backfill_gap\":false,"
                "\"nullifier_backfill_gap\":false}}");
    return strdup("null");
}

/* core.status.brief exists so an operator/AI never has to pipe the ~15KB
 * core.status body through grep/tr for the handful of fields that answer
 * "is the node serving and caught up" — see docs/NATIVE_COMMAND_INTERFACE.md
 * "CLI UX contract" and status_brief_native_handler.c. This proves the leaf
 * is READY-bridged, dispatches to a real zcl.result.v1 envelope, and that
 * `data` stays flat (no nested containers besides the universal `_page`
 * pagination sidecar every bridged leaf carries) with exactly the thirteen
 * documented sync/serving keys. */
static int test_status_brief_flat_lean_envelope(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("core.status.brief: flat lean zcl.result.v1 body, thirteen "
        "sync/serving fields") {
        const struct zcl_command_spec *root_status = find_spec(reg, "status");
        const struct zcl_command_spec *s =
            find_spec(reg, "core.status.brief");
        ASSERT(root_status != NULL);
        ASSERT_EQ(root_status->availability, ZCL_COMMAND_READY);
        ASSERT(root_status->handler == zcl_native_bridge_command);
        ASSERT_STR_EQ(root_status->output_schema,
                      "zcl.core_status_brief.v1");
        ASSERT((root_status->allowed_lanes & ZCL_COMMAND_LANE_LOCAL) != 0);
        ASSERT(zcl_native_bridge_tool_for_path("status") != NULL);
        ASSERT(zcl_native_bridge_body_for_path("status") != NULL);
        ASSERT(s != NULL);
        ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
        ASSERT(s->handler == zcl_native_bridge_command);
        ASSERT(zcl_native_bridge_tool_for_path("core.status.brief") != NULL);

        mcp_rpc_client_set_test_hook(status_brief_mock_rpc);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        enum zcl_command_exit root_code = ZCL_COMMAND_EXIT_INTERNAL;
        char root_out[ZCL_COMMAND_RESULT_BUDGET + 1];
        bool root_dispatched = exec_leaf(reg, root_status, root_out,
                                         sizeof(root_out), &root_code);
        bool dispatched = exec_leaf(reg, s, out, sizeof(out), &code);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(root_dispatched);
        ASSERT_EQ(root_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(dispatched);
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);

        struct json_value root;
        ASSERT(json_read(&root, out, strlen(out)) && root.type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.result.v1");
        ASSERT(json_get_bool(json_get(&root, "ok")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "passed");

        const struct json_value *data = json_get(&root, "data");
        ASSERT(data != NULL && data->type == JSON_OBJ);

        static const char *const expected_keys[] = {
            "hstar", "header_height", "gap", "peer_best", "sync_state",
            "serving", "healthy", "peer_count", "primary_blocker",
            "blocker_age_s", "active_conditions", "rss_mb",
            "tip_advance_age_seconds",
        };
        size_t expected_count =
            sizeof(expected_keys) / sizeof(expected_keys[0]);
        /* The thirteen documented fields plus the universal `_page` sidecar
         * every bridged leaf's envelope carries — nothing else. */
        ASSERT(data->num_children == expected_count + 1);
        for (size_t i = 0; i < expected_count; i++) {
            const struct json_value *v = json_get(data, expected_keys[i]);
            ASSERT(v != NULL);
            ASSERT(v->type != JSON_OBJ && v->type != JSON_ARR);
        }
        const struct json_value *page = json_get(data, "_page");
        ASSERT(page != NULL && page->type == JSON_OBJ);
        ASSERT(!json_get_bool(json_get(page, "truncated")));

        ASSERT_EQ(json_get_int(json_get(data, "hstar")),
                  (int64_t)3117073);
        ASSERT_EQ(json_get_int(json_get(data, "header_height")),
                  (int64_t)3117074);
        ASSERT_EQ(json_get_int(json_get(data, "gap")), (int64_t)1);
        ASSERT_EQ(json_get_int(json_get(data, "peer_best")),
                  (int64_t)3117074);
        ASSERT_STR_EQ(json_get_str(json_get(data, "sync_state")), "at_tip");
        ASSERT(json_get_bool(json_get(data, "serving")));
        ASSERT(json_get_bool(json_get(data, "healthy")));
        ASSERT_EQ(json_get_int(json_get(data, "peer_count")), (int64_t)1);
        ASSERT_STR_EQ(json_get_str(json_get(data, "primary_blocker")),
                      "none");
        /* No active blocker in the fixture -> age honestly null. */
        ASSERT(json_is_null(json_get(data, "blocker_age_s")));
        ASSERT_EQ(json_get_int(json_get(data, "active_conditions")),
                  (int64_t)2);
        ASSERT_EQ(json_get_int(json_get(data, "rss_mb")), (int64_t)512);
        ASSERT_EQ(json_get_int(json_get(data, "tip_advance_age_seconds")),
                  (int64_t)3);
        json_free(&root);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_status_brief_composite_fails_closed(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *s = find_spec(reg, "status");
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("root status rejects RPC errors, schema skew, and wrong field types") {
        static const char *const cases[] = {
            "{\"code\":-32601,\"message\":\"Method not found\"}",
            "{\"error\":{\"code\":-32603,"
                "\"message\":\"cannot connect to node\"}}",
            "{\"schema\":\"zcl.public_status.v2\"}",
            "{\"schema\":\"zcl.public_status.v1\","
                "\"served_height\":\"3117073\"}",
        };
        ASSERT(s != NULL);
        mcp_rpc_client_set_test_hook(status_brief_mock_rpc);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            g_status_brief_agent_fixture = cases[i];
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_FAILED);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
            ASSERT(strstr(out, "\"status\":\"failed\"") != NULL);
            ASSERT(strstr(out, "\"code\":\"TOOL_ERROR\"") != NULL);
            ASSERT(strstr(out, "invalid zcl.public_status.v1") != NULL);
        }
        PASS();
    } _test_next:;
    g_status_brief_agent_fixture = NULL;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static void status_brief_fixture_write(
    char *out, size_t out_size,
    int64_t served, bool served_known,
    int64_t header, bool header_known,
    int64_t peer_best, bool peer_best_known,
    int64_t target, bool target_known,
    int64_t gap, bool chain_consistent,
    int64_t tip_age, bool partial, bool include_resources,
    bool serving, bool healthy, bool budget_exceeded,
    bool anchor_gap, bool nullifier_gap)
{
    (void)snprintf(
        out, out_size,
        "{\"schema\":\"zcl.public_status.v1\","
        "\"partial_result\":%s%s,"
        "\"served_height\":%lld,\"served_height_known\":%s,"
        "\"header_height\":%lld,\"header_height_known\":%s,"
        "\"gap\":%lld,\"peer_best_height\":%lld,"
        "\"peer_best_height_known\":%s,"
        "\"target_height\":%lld,\"target_height_known\":%s,"
        "\"chain_evidence_consistent\":%s,"
        "\"sync_state\":\"idle\",\"serving\":%s,\"healthy\":%s,"
        "\"primary_blocker\":\"none\","
        "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
        "\"budget_ms\":250,\"partial_result\":%s,"
        "\"budget_exceeded\":%s},"
        "\"peers\":{\"total\":0},"
        "\"conditions\":{"
        "\"schema\":\"zcl.condition_engine_summary.v1\","
        "\"active_count\":0},%s"
        "\"reducer\":{\"tip_advance_age_seconds\":%lld},"
        "\"security_posture\":{"
        "\"schema\":\"zcl.security_posture.v1\","
        "\"anchor_backfill_gap\":%s,"
        "\"nullifier_backfill_gap\":%s}}",
        partial ? "true" : "false",
        partial ? ",\"partial_reason\":\"optional_detail_budget_guard:resources\""
                : "",
        (long long)served, served_known ? "true" : "false",
        (long long)header, header_known ? "true" : "false",
        (long long)gap, (long long)peer_best,
        peer_best_known ? "true" : "false", (long long)target,
        target_known ? "true" : "false",
        chain_consistent ? "true" : "false",
        serving ? "true" : "false", healthy ? "true" : "false",
        partial ? "true" : "false",
        budget_exceeded ? "true" : "false",
        include_resources
            ? "\"resources\":{\"schema\":\"zcl.node_resources.v1\","
              "\"rss_mb\":512},"
            : "",
        (long long)tip_age, anchor_gap ? "true" : "false",
        nullifier_gap ? "true" : "false");
}

static int test_status_brief_valid_unknown_and_partial_contracts(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *s = find_spec(reg, "status");
    char fixture[2048];
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("root status preserves valid boot/no-peer/partial unknowns") {
        ASSERT(s != NULL);
        mcp_rpc_client_set_test_hook(status_brief_mock_rpc);

        /* Fresh boot: no selected frontier, header, peer height, or tip-age
         * sample is a valid v1 response, not schema skew. */
        status_brief_fixture_write(
            fixture, sizeof(fixture), 0, false, -1, false, -1, false,
            0, false, 0, false, -1, false, true,
            false, false, false, false, false);
        g_status_brief_agent_fixture = fixture;
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        struct json_value root;
        ASSERT(json_read(&root, out, strlen(out)) && root.type == JSON_OBJ);
        const struct json_value *data = json_get(&root, "data");
        ASSERT(data && data->type == JSON_OBJ);
        ASSERT(json_is_null(json_get(data, "hstar")));
        ASSERT(json_is_null(json_get(data, "header_height")));
        ASSERT(json_is_null(json_get(data, "gap")));
        ASSERT(json_is_null(json_get(data, "peer_best")));
        ASSERT(json_is_null(json_get(data, "tip_advance_age_seconds")));
        ASSERT_EQ(json_get_int(json_get(data, "peer_count")), (int64_t)0);
        json_free(&root);

        /* The producer intentionally omits resources when its optional-detail
         * guard fires before the 250ms first-call budget. */
        status_brief_fixture_write(
            fixture, sizeof(fixture), 100, true, 101, true, -1, false,
            101, true, 1, true, 3, true, false,
            true, true, false, false, false);
        code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        ASSERT(json_read(&root, out, strlen(out)) && root.type == JSON_OBJ);
        data = json_get(&root, "data");
        ASSERT(data && json_is_null(json_get(data, "rss_mb")));
        ASSERT_EQ(json_get_int(json_get(data, "hstar")), (int64_t)100);
        json_free(&root);

        /* Causal shielded gaps outrank the general latch without changing the
         * honest unknown peer projection. */
        status_brief_fixture_write(
            fixture, sizeof(fixture), 100, true, 101, true, -1, false,
            101, true, 1, true, 3, false, true,
            true, false, false, true, true);
        code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        ASSERT(json_read(&root, out, strlen(out)) && root.type == JSON_OBJ);
        data = json_get(&root, "data");
        ASSERT_STR_EQ(json_get_str(json_get(data, "primary_blocker")),
                      "utxo_apply.anchor_backfill_gap");
        ASSERT(json_get_bool(json_get(data, "serving")));
        ASSERT(!json_get_bool(json_get(data, "healthy")));
        json_free(&root);
        PASS();
    } _test_next:;
    g_status_brief_agent_fixture = NULL;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_status_brief_rejects_contract_contradictions(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *s = find_spec(reg, "status");
    char fixture[2048];
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("root status rejects known/sentinel, gap, partial, and budget contradictions") {
        ASSERT(s != NULL);
        mcp_rpc_client_set_test_hook(status_brief_mock_rpc);
        static const int cases = 4;
        for (int i = 0; i < cases; i++) {
            if (i == 0) {
                /* known peer height may not carry the -1 sentinel. */
                status_brief_fixture_write(
                    fixture, sizeof(fixture), 100, true, 101, true, -1, true,
                    101, true, 1, true, 3, false, true,
                    true, true, false, false, false);
            } else if (i == 1) {
                /* A consistent chain's gap is exact header-H*. */
                status_brief_fixture_write(
                    fixture, sizeof(fixture), 100, true, 101, true, 101, true,
                    101, true, 9, true, 3, false, true,
                    true, true, false, false, false);
            } else if (i == 2) {
                /* Full results cannot silently omit the resources member. */
                status_brief_fixture_write(
                    fixture, sizeof(fixture), 100, true, 101, true, 101, true,
                    101, true, 1, true, 3, false, false,
                    true, true, false, false, false);
            } else {
                status_brief_fixture_write(
                    fixture, sizeof(fixture), 100, true, 101, true, 101, true,
                    101, true, 1, true, 3, false, true,
                    true, true, true, false, false);
            }
            g_status_brief_agent_fixture = fixture;
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_FAILED);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
            ASSERT(strstr(out, "\"code\":\"TOOL_ERROR\"") != NULL);
        }
        PASS();
    } _test_next:;
    g_status_brief_agent_fixture = NULL;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

/* A fully valid zcl.public_status.v1 document (mirrors
 * status_brief_mock_rpc's default fixture) so each case below can drop or
 * corrupt exactly one field and prove the resulting error names that field
 * instead of the old one-size-fits-all "invalid zcl.public_status.v1". */
static const char g_status_brief_valid_doc[] =
    "{\"schema\":\"zcl.public_status.v1\","
    "\"partial_result\":false,"
    "\"served_height\":3117073,\"header_height\":3117074,"
    "\"served_height_known\":true,"
    "\"header_height_known\":true,"
    "\"gap\":1,\"peer_best_height\":3117074,"
    "\"peer_best_height_known\":true,"
    "\"target_height\":3117074,\"target_height_known\":true,"
    "\"chain_evidence_consistent\":true,"
    "\"sync_state\":\"at_tip\",\"serving\":true,"
    "\"healthy\":true,\"primary_blocker\":\"none\","
    "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
        "\"budget_ms\":250,\"partial_result\":false,"
        "\"budget_exceeded\":false},"
    "\"peers\":{\"total\":1},"
    "\"conditions\":{"
        "\"schema\":\"zcl.condition_engine_summary.v1\","
        "\"active_count\":2},"
    "\"resources\":{\"schema\":\"zcl.node_resources.v1\","
        "\"rss_mb\":512},"
    "\"reducer\":{\"tip_advance_age_seconds\":3},"
    "\"security_posture\":{"
        "\"schema\":\"zcl.security_posture.v1\","
        "\"anchor_backfill_gap\":false,"
        "\"nullifier_backfill_gap\":false}}";

/* E1: the composite validation used to collapse ~30 predicates into one
 * opaque "invalid zcl.public_status.v1" message. Each case here removes (or
 * corrupts) exactly one representative field from an otherwise-valid
 * document and proves the error names that exact field, and correctly
 * classifies an entirely-absent key (an older node binary's `agent` RPC
 * predating a newer field) as schema/version skew rather than a generic
 * malformed-document error. */
static int test_status_brief_names_first_failing_field(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *s = find_spec(reg, "status");
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("root status names the first failing zcl.public_status.v1 field") {
        ASSERT(s != NULL);
        mcp_rpc_client_set_test_hook(status_brief_mock_rpc);

        /* Case 1: an entirely-absent top-level bool field (as an older node
         * binary's agent RPC would omit) -> named + classified as version
         * skew, not a generic malformed-document error. */
        {
            const char *removed =
                "{\"schema\":\"zcl.public_status.v1\","
                "\"partial_result\":false,"
                "\"served_height\":3117073,\"header_height\":3117074,"
                "\"served_height_known\":true,"
                "\"header_height_known\":true,"
                "\"gap\":1,\"peer_best_height\":3117074,"
                "\"peer_best_height_known\":true,"
                "\"target_height\":3117074,\"target_height_known\":true,"
                "\"sync_state\":\"at_tip\",\"serving\":true,"
                "\"healthy\":true,\"primary_blocker\":\"none\","
                "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
                    "\"budget_ms\":250,\"partial_result\":false,"
                    "\"budget_exceeded\":false},"
                "\"peers\":{\"total\":1},"
                "\"conditions\":{"
                    "\"schema\":\"zcl.condition_engine_summary.v1\","
                    "\"active_count\":2},"
                "\"resources\":{\"schema\":\"zcl.node_resources.v1\","
                    "\"rss_mb\":512},"
                "\"reducer\":{\"tip_advance_age_seconds\":3},"
                "\"security_posture\":{"
                    "\"schema\":\"zcl.security_posture.v1\","
                    "\"anchor_backfill_gap\":false,"
                    "\"nullifier_backfill_gap\":false}}";
            g_status_brief_agent_fixture = removed;
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_FAILED);
            ASSERT(strstr(out, "chain_evidence_consistent") != NULL);
            ASSERT(strstr(out, "predates the CLI contract") != NULL);
        }

        /* Case 2: an entirely-absent nested object (a whole newer subsystem
         * projection an older node never emitted) -> named + version skew. */
        {
            const char *removed =
                "{\"schema\":\"zcl.public_status.v1\","
                "\"partial_result\":false,"
                "\"served_height\":3117073,\"header_height\":3117074,"
                "\"served_height_known\":true,"
                "\"header_height_known\":true,"
                "\"gap\":1,\"peer_best_height\":3117074,"
                "\"peer_best_height_known\":true,"
                "\"target_height\":3117074,\"target_height_known\":true,"
                "\"chain_evidence_consistent\":true,"
                "\"sync_state\":\"at_tip\",\"serving\":true,"
                "\"healthy\":true,\"primary_blocker\":\"none\","
                "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
                    "\"budget_ms\":250,\"partial_result\":false,"
                    "\"budget_exceeded\":false},"
                "\"peers\":{\"total\":1},"
                "\"conditions\":{"
                    "\"schema\":\"zcl.condition_engine_summary.v1\","
                    "\"active_count\":2},"
                "\"resources\":{\"schema\":\"zcl.node_resources.v1\","
                    "\"rss_mb\":512},"
                "\"reducer\":{\"tip_advance_age_seconds\":3}}";
            g_status_brief_agent_fixture = removed;
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_FAILED);
            ASSERT(strstr(out, "security_posture") != NULL);
            ASSERT(strstr(out, "predates the CLI contract") != NULL);
        }

        /* Case 3: a field that is PRESENT but the wrong JSON type -> named,
         * but classified as malformed, not version skew (the key exists;
         * its value just violates the contract). */
        {
            const char *malformed =
                "{\"schema\":\"zcl.public_status.v1\","
                "\"partial_result\":false,"
                "\"served_height\":3117073,\"header_height\":3117074,"
                "\"served_height_known\":true,"
                "\"header_height_known\":true,"
                "\"gap\":1,\"peer_best_height\":3117074,"
                "\"peer_best_height_known\":true,"
                "\"target_height\":3117074,\"target_height_known\":true,"
                "\"chain_evidence_consistent\":true,"
                "\"sync_state\":\"at_tip\",\"serving\":true,"
                "\"healthy\":\"yes\",\"primary_blocker\":\"none\","
                "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
                    "\"budget_ms\":250,\"partial_result\":false,"
                    "\"budget_exceeded\":false},"
                "\"peers\":{\"total\":1},"
                "\"conditions\":{"
                    "\"schema\":\"zcl.condition_engine_summary.v1\","
                    "\"active_count\":2},"
                "\"resources\":{\"schema\":\"zcl.node_resources.v1\","
                    "\"rss_mb\":512},"
                "\"reducer\":{\"tip_advance_age_seconds\":3},"
                "\"security_posture\":{"
                    "\"schema\":\"zcl.security_posture.v1\","
                    "\"anchor_backfill_gap\":false,"
                    "\"nullifier_backfill_gap\":false}}";
            g_status_brief_agent_fixture = malformed;
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_FAILED);
            ASSERT(strstr(out, "\"healthy\"") != NULL ||
                   strstr(out, "field healthy") != NULL);
            ASSERT(strstr(out, "predates the CLI contract") == NULL);
            ASSERT(strstr(out, "missing/invalid field healthy") != NULL);
        }

        /* Baseline: the fully valid document names nothing and passes. */
        {
            g_status_brief_agent_fixture = g_status_brief_valid_doc;
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        }
        PASS();
    } _test_next:;
    g_status_brief_agent_fixture = NULL;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_planned_fail_closed(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("every planned leaf blocks with exit 3 and no handler") {
        int checked = 0;
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (s->mode == ZCL_COMMAND_MODE_BRANCH)
                continue;
            if (s->availability != ZCL_COMMAND_PLANNED)
                continue;
            ASSERT(s->handler == NULL);
            ASSERT(s->availability_reason && s->availability_reason[0]);
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_BLOCKED);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
            ASSERT(strstr(out, "COMMAND_PLANNED") != NULL);
            checked++;
        }
        ASSERT(checked > 5);
        PASS();
    } _test_next:;
    return failures;
}

static int test_envelope_vectors(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("a local discovery leaf returns a passing common envelope") {
        const struct zcl_command_spec *s = find_spec(reg, "discover.describe");
        ASSERT(s != NULL);
        struct zcl_command_context ctx = {
            .registry = reg, .granted_capabilities = ~(uint64_t)0,
            .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
        };
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        (void)json_push_kv_str(&input, "path", "core.status");
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        size_t n = zcl_command_registry_execute_json(
            reg, s, &ctx, &input, false, "discover.describe", "normal", 0, 0,
            NULL, out, sizeof(out), &code);
        json_free(&input);
        ASSERT(n > 0);
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"schema\":\"zcl.result.v1\"") != NULL);
        ASSERT(strstr(out, "\"ok\":true") != NULL);
        ASSERT(strstr(out, "core.status") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_typo_stays_branch(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("a typo under a canonical branch resolves to the branch, not a leaf") {
        /* `core chain bogus`: longest path is the core.chain BRANCH with the
         * unknown word left over. The adapter turns this into the structured
         * unknown-command error; the registry never invents a leaf for it, so
         * it can never fall through to an arbitrary RPC method. */
        const char *words[] = { "core", "chain", "bogus" };
        size_t consumed = 0;
        bool alias = false;
        char invoked[ZCL_COMMAND_MAX_PATH];
        const struct zcl_command_spec *s = zcl_command_registry_resolve_words(
            reg, words, 3, &consumed, &alias, invoked, sizeof(invoked));
        ASSERT(s != NULL);
        ASSERT_STR_EQ(s->path, "core.chain");
        ASSERT_EQ(s->mode, ZCL_COMMAND_MODE_BRANCH);
        ASSERT_EQ(consumed, (size_t)2);
        ASSERT(find_spec(reg, "core.chain.bogus") == NULL);
        ASSERT(zcl_command_registry_find(reg, "core.chain.bogus", NULL) ==
               NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dev_branch_leaves(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("dev branch carries the expected ready/planned leaf availability") {
        const struct zcl_command_spec *dev = find_spec(reg, "dev");
        ASSERT(dev != NULL);
        ASSERT_EQ(dev->mode, ZCL_COMMAND_MODE_BRANCH);

        const char *ready[] = {
            "dev.status", "dev.core.boundary", "dev.app.describe",
            "dev.app.plan", "dev.app.simulate", "dev.change.plan",
            "dev.app.list", "dev.test.plan",
        };
        for (size_t i = 0; i < sizeof(ready) / sizeof(ready[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, ready[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
            ASSERT(s->handler != NULL);
        }
        /* Unfinished dev operations are explicitly planned + handlerless, so
         * discovery can never advertise a dev command that cannot dispatch. */
        const char *planned[] = {
            "dev.core.proof", "dev.app.inspect", "dev.test.replay",
            "dev.generation.rollback", "dev.loop.events",
        };
        for (size_t i = 0; i < sizeof(planned) / sizeof(planned[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, planned[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_PLANNED);
            ASSERT(s->handler == NULL);
        }
        /* Dev executors are real handlers only in ZCL_DEV_BUILD.  This test
         * binary is a release-shaped catalog, so those leaves must remain
         * explicit COMPAT entries rather than falsely READY. */
        const char *compat[] = {
            "dev.change.apply", "dev.loop.ensure", "dev.loop.status",
            "dev.loop.wait", "dev.loop.stop", "dev.test.run",
            "dev.test.sim", "dev.generation.current",
            "dev.generation.history", "dev.diagnose.latest",
            "dev.diagnose.show",
            "dev.vcs.revert", "dev.vcs.seal.grant",
        };
        for (size_t i = 0; i < sizeof(compat) / sizeof(compat[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, compat[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_COMPAT);
            ASSERT(s->handler == NULL);
            ASSERT(s->compat_target != NULL && s->compat_target[0]);
        }

        const struct zcl_command_spec *failure_latest =
            find_spec(reg, "dev.diagnose.latest");
        const struct zcl_command_spec *failure_show =
            find_spec(reg, "dev.diagnose.show");
        ASSERT(failure_latest != NULL && failure_show != NULL);
        ASSERT_STR_EQ(failure_latest->output_schema,
                      "zcl.dev_failure_latest_result.v1");
        ASSERT_EQ(failure_latest->budget_bytes, (size_t)2048);
        ASSERT_STR_EQ(failure_show->output_schema,
                      "zcl.dev_failure_show.v1");
        ASSERT_EQ(failure_show->budget_bytes, (size_t)6144);
        ASSERT_STR_EQ(failure_show->positional_keys, "failure_id");

        PASS();
    } _test_next:;
    return failures;
}

/* Build a body larger than the ordinary-result budget: several long scalar
 * fields plus one nested container, so projection must drop or page. */
static void make_large_body(struct json_value *body)
{
    char big[420];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    json_init(body);
    json_set_object(body);
    for (int i = 0; i < 8; i++) {
        char key[16];
        (void)snprintf(key, sizeof(key), "s%d", i);
        (void)json_push_kv_str(body, key, big);
    }
    struct json_value nested;
    json_init(&nested);
    json_set_object(&nested);
    (void)json_push_kv_str(&nested, "a", big);
    (void)json_push_kv_str(&nested, "b", big);
    (void)json_push_kv(body, "nested", &nested);
    json_free(&nested);
}

static int test_response_budget_views(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *s = find_spec(reg, "core.status");
    char scratch[ZCL_COMMAND_LIST_BUDGET + 1];

    TEST("bridge projection: summary/normal/full page a too-large body") {
        ASSERT(s != NULL);

        /* summary: drop containers, fit the ordinary-result budget. */
        struct json_value body;
        make_large_body(&body);
        struct zcl_command_request req = { .spec = s, .view = "summary" };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, s->output_schema);
        zcl_native_bridge_project(&req, &body, &reply);
        size_t n = json_write(&reply.data, scratch, sizeof(scratch));
        ASSERT(n > 0 && n <= ZCL_COMMAND_RESULT_BUDGET);
        ASSERT(json_get(&reply.data, "nested") == NULL);
        const struct json_value *page = json_get(&reply.data, "_page");
        ASSERT(page != NULL && page->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(page, "view")), "summary");
        ASSERT(json_get(page, "truncated") != NULL);
        zcl_command_reply_free(&reply);
        json_free(&body);

        /* normal: truncate, expose an advancing cursor, and point at the
         * command contract without emitting a self-loop. */
        make_large_body(&body);
        req = (struct zcl_command_request){ .spec = s, .view = "normal" };
        zcl_command_reply_init(&reply, s->output_schema);
        zcl_native_bridge_project(&req, &body, &reply);
        n = json_write(&reply.data, scratch, sizeof(scratch));
        ASSERT(n > 0 && n <= ZCL_COMMAND_RESULT_BUDGET);
        page = json_get(&reply.data, "_page");
        ASSERT(page != NULL);
        const struct json_value *trunc = json_get(page, "truncated");
        ASSERT(trunc != NULL && trunc->type == JSON_BOOL && trunc->val.b);
        ASSERT(json_get(page, "next_cursor") != NULL);
        ASSERT(reply.next_count >= 1);
        ASSERT_STR_EQ(reply.next[0].command, "discover.describe");
        ASSERT(strstr(reply.next[0].input_json, "core.status") != NULL);
        zcl_command_reply_free(&reply);
        json_free(&body);

        /* full: honor --max-items and page via an advancing cursor. */
        make_large_body(&body);
        req = (struct zcl_command_request){
            .spec = s, .view = "full", .max_items = 3, .cursor = "0",
        };
        zcl_command_reply_init(&reply, s->output_schema);
        zcl_native_bridge_project(&req, &body, &reply);
        page = json_get(&reply.data, "_page");
        ASSERT(page != NULL);
        ASSERT_EQ(json_get_int(json_get(page, "included")), (int64_t)3);
        const struct json_value *nc = json_get(page, "next_cursor");
        ASSERT(nc != NULL);
        ASSERT_EQ(json_get_int(nc), (int64_t)3);
        zcl_command_reply_free(&reply);
        json_free(&body);
        PASS();
    } _test_next:;

    return failures;
}

/* dev.vcs.revert IS a golden catalog row now (config/commands/dev.def via
 * ZCL_COMMAND_DEV_COMMAND, asserted COMPAT above in test_dev_branch_leaves).
 * What test_dev_branch_leaves does NOT reach is the handler body itself: a
 * release/testing build (this test binary is built WITHOUT ZCL_DEV_BUILD,
 * see Makefile TEST_FAST_CFLAGS) must link the `#ifndef ZCL_DEV_BUILD` stub
 * body of zcl_native_handle_dev_vcs_revert — never the real
 * vcs_revert()+shell-fallback path — and that stub must fail closed
 * (BLOCKED, not a silent no-op) instead of mutating anything. */
static int test_dev_vcs_revert_release_stub(void)
{
    int failures = 0;
    TEST("dev.vcs.revert fails closed (BLOCKED) outside a dev build") {
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        (void)json_push_kv_str(&input, "to",
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        (void)json_push_kv_bool(&input, "relink_generation", true);

        const struct zcl_command_spec *spec =
            find_spec(zcl_command_catalog(), "dev.vcs.revert");
        char why[128] = {0};
        ASSERT(spec != NULL);
        ASSERT(zcl_command_registry_input_validate(spec, &input, why,
                                                   sizeof(why)));

        struct zcl_command_request request = {
            .spec = NULL,
            .context = NULL,
            .input = &input,
            .view = "normal",
            .budget_bytes = 0,
            .invoked_by_alias = false,
            .invoked_name = "dev.vcs.revert",
        };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.dev_vcs_revert.v1");
        zcl_native_handle_dev_vcs_revert(&request, &reply);

        ASSERT_EQ((int)reply.status, (int)ZCL_COMMAND_STATUS_BLOCKED);
        ASSERT_EQ((int)reply.exit_code, (int)ZCL_COMMAND_EXIT_BLOCKED);
        ASSERT_STR_EQ(reply.error.code, "DEV_BUILD_REQUIRED");
        zcl_command_reply_free(&reply);
        json_free(&input);
        PASS();
    } _test_next:;
    return failures;
}

/* dev.vcs.seal.grant IS a golden catalog row now (config/commands/dev.def
 * via ZCL_COMMAND_DEV_COMMAND, asserted COMPAT above in
 * test_dev_branch_leaves). Same shape as test_dev_vcs_revert_release_stub:
 * this release/testing build (no ZCL_DEV_BUILD) links the `#ifndef
 * ZCL_DEV_BUILD` stub body of zcl_native_handle_dev_vcs_seal_grant — never
 * the real vcs_seal_grant_unseal() path — so the mandatory-confirm gate
 * inside ZCL_DEV_BUILD is not reachable from this binary. What IS provable
 * here is that the stub fails closed (BLOCKED, never a silent mutation)
 * regardless of whether the caller supplied a well-formed, owner-confirmed
 * request or an unconfirmed one — granting a ZVCS unseal token is simply
 * unavailable outside a dev build. */
static int test_dev_vcs_seal_grant_release_stub(void)
{
    int failures = 0;
    TEST("dev.vcs.seal.grant fails closed (BLOCKED) outside a dev build, "
         "confirmed or not") {
        const bool confirms[] = { true, false };
        for (size_t i = 0; i < sizeof(confirms) / sizeof(confirms[0]); i++) {
            struct json_value input;
            json_init(&input);
            json_set_object(&input);
            (void)json_push_kv_str(&input, "reason", "post-baseline review");
            (void)json_push_kv_bool(&input, "confirm", confirms[i]);

            struct zcl_command_request request = {
                .spec = NULL,
                .context = NULL,
                .input = &input,
                .view = "normal",
                .budget_bytes = 0,
                .invoked_by_alias = false,
                .invoked_name = "dev.vcs.seal.grant",
            };
            struct zcl_command_reply reply;
            zcl_command_reply_init(&reply, "zcl.dev_vcs_seal_grant.v1");
            zcl_native_handle_dev_vcs_seal_grant(&request, &reply);

            ASSERT_EQ((int)reply.status, (int)ZCL_COMMAND_STATUS_BLOCKED);
            ASSERT_EQ((int)reply.exit_code, (int)ZCL_COMMAND_EXIT_BLOCKED);
            ASSERT_STR_EQ(reply.error.code, "DEV_BUILD_REQUIRED");
            zcl_command_reply_free(&reply);
            json_free(&input);
        }
        PASS();
    } _test_next:;
    return failures;
}

/* W0-A: the native bridge dispatches WITHOUT the MCP router/middleware.
 * Every bridged READY leaf must resolve to exactly ONE MCP-free dispatch:
 * a re-homed transport-neutral body function (app/controllers/
 * *_native_handlers.c) XOR a direct JSON-RPC method (pure pass-through).
 * The MCP tool name stays as dual-run equivalence metadata. */
static int test_bridge_mcp_free_bindings(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every bridged leaf has exactly one MCP-free dispatch binding") {
        int checked = 0;
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (s->mode == ZCL_COMMAND_MODE_BRANCH)
                continue;
            if (s->handler != zcl_native_bridge_command)
                continue;
            const char *tool = zcl_native_bridge_tool_for_path(s->path);
            zcl_native_body_fn body = zcl_native_bridge_body_for_path(s->path);
            const char *rpc = zcl_native_bridge_rpc_for_path(s->path);
            ASSERT(tool != NULL && tool[0] != 0);
            /* exactly one of the two dispatch kinds */
            ASSERT((body != NULL) != (rpc != NULL));
            checked++;
        }
        /* the full bridged read surface, not a sample */
        ASSERT(checked >= 40);
        PASS();
    } _test_next:;
    return failures;
}

/* W0: ops.selftest is the native, node-free successor of the MCP
 * `zcl_self_test mode=registry`. It sweeps the catalog for the static
 * well-formedness the registry guarantees. Because test_catalog_wellformed
 * already proves the whole catalog validates, ops.selftest MUST report
 * fail == 0 with a passing envelope, so the dev-lane deploy verify can gate
 * on it without a running node. */
static int test_ops_selftest_registry(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("ops.selftest sweeps the registry and reports fail:0") {
        const struct zcl_command_spec *s = find_spec(reg, "ops.selftest");
        ASSERT(s != NULL);
        ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
        ASSERT(s->handler != NULL);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"ok\":true") != NULL);
        ASSERT(strstr(out, "\"mode\":\"registry\"") != NULL);
        ASSERT(strstr(out, "\"fail\":0") != NULL);
        /* At least the READY read/discovery leaves pass. */
        ASSERT(strstr(out, "\"pass\":0") == NULL);
        PASS();
    } _test_next:;
    return failures;
}

/* W0: ops.state is the native successor of the MCP `zcl_state` primitive.
 * Its node-contacting path (dumpstate RPC) needs a live node, but its input
 * guard is node-free: a missing `subsystem` must fail INVALID before any RPC,
 * naming MISSING_SUBSYSTEM and offering an executable next command. */
static int test_ops_state_requires_subsystem(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("ops.state fails INVALID without a subsystem, before any node call") {
        const struct zcl_command_spec *s = find_spec(reg, "ops.state");
        ASSERT(s != NULL);
        ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
        ASSERT(s->handler != NULL);
        ASSERT_STR_EQ(s->input_keys, "subsystem,key");
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_INVALID);
        ASSERT(strstr(out, "\"ok\":false") != NULL);
        ASSERT(strstr(out, "MISSING_SUBSYSTEM") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_is_root_ownership(void)
{
    int failures = 0;
    TEST("is_root owns terse status plus core/app/dev/ops/discover/code") {
        ASSERT(zcl_native_command_is_root("core"));
        ASSERT(zcl_native_command_is_root("app"));
        ASSERT(zcl_native_command_is_root("ops"));
        ASSERT(zcl_native_command_is_root("discover"));
        ASSERT(zcl_native_command_is_root("code"));
        ASSERT(zcl_native_command_is_root("help"));
        ASSERT(zcl_native_command_is_root("search"));
        ASSERT(zcl_native_command_is_root("status"));
        ASSERT(zcl_native_command_is_root("dev"));
        ASSERT(!zcl_native_command_is_root("getblockcount"));
        PASS();
    } _test_next:;
    return failures;
}

static void contract_noop_handler(const struct zcl_command_request *request,
                                  struct zcl_command_reply *reply)
{
    (void)request;
    (void)reply;
}

/* OS-B1: validate must reject a READY leaf without a distinct, non-empty
 * `semantics` contract and a budget_bytes outside {0} ∪ [256, 65536]. Built as
 * a two-entry fixture registry (a branch parent + one leaf) mutated per case. */
static int test_semantics_contract_negative(void)
{
    int failures = 0;
    TEST("validate rejects missing/duplicate semantics and out-of-range budget") {
        struct zcl_command_spec branch = {
            .path = "x", .parent = "", .aliases = "", .summary = "branch x",
            .semantics = "", .tags = "", .input_schema = "",
            .output_schema = "", .input_keys = "", .positional_keys = "",
            .example = "", .availability_reason = "", .compat_target = "",
            .budget_bytes = 0, .layer = ZCL_COMMAND_LAYER_OPS,
            .effect = ZCL_COMMAND_EFFECT_READ, .risk = ZCL_COMMAND_RISK_READ,
            .scope = ZCL_COMMAND_SCOPE_LOCAL, .authority = ZCL_COMMAND_AUTH_PUBLIC,
            .availability = ZCL_COMMAND_READY, .mode = ZCL_COMMAND_MODE_BRANCH,
            .latency = ZCL_COMMAND_LATENCY_INSTANT, .cost = ZCL_COMMAND_COST_TINY,
            .confirmation = ZCL_COMMAND_CONFIRM_NONE,
            .allowed_lanes = ZCL_COMMAND_LANE_LOCAL,
            .transports = ZCL_COMMAND_TRANSPORT_NATIVE, .handler = NULL,
        };
        struct zcl_command_spec leaf_base = {
            .path = "x.y", .parent = "x", .aliases = "", .summary = "do a thing",
            .semantics = "the settled result of the thing, read locally",
            .tags = "t", .input_schema = "zcl.in.v1",
            .output_schema = "zcl.out.v1", .input_keys = "",
            .positional_keys = "", .example = "zclassic23 x y",
            .availability_reason = "", .compat_target = "", .budget_bytes = 0,
            .layer = ZCL_COMMAND_LAYER_OPS, .effect = ZCL_COMMAND_EFFECT_READ,
            .risk = ZCL_COMMAND_RISK_READ, .scope = ZCL_COMMAND_SCOPE_LOCAL,
            .authority = ZCL_COMMAND_AUTH_PUBLIC,
            .availability = ZCL_COMMAND_READY, .mode = ZCL_COMMAND_MODE_SYNC,
            .latency = ZCL_COMMAND_LATENCY_INSTANT, .cost = ZCL_COMMAND_COST_TINY,
            .confirmation = ZCL_COMMAND_CONFIRM_NONE,
            .allowed_lanes = ZCL_COMMAND_LANE_LOCAL,
            .transports = ZCL_COMMAND_TRANSPORT_NATIVE,
            .handler = contract_noop_handler,
        };
        char why[128] = { 0 };

        struct zcl_command_spec ok_specs[2] = { branch, leaf_base };
        struct zcl_command_registry ok_reg = { .commands = ok_specs, .count = 2 };
        ASSERT(zcl_command_registry_validate(&ok_reg, why, sizeof(why)));

        struct zcl_command_spec miss = leaf_base;
        miss.semantics = "";
        struct zcl_command_spec miss_specs[2] = { branch, miss };
        struct zcl_command_registry miss_reg = {
            .commands = miss_specs, .count = 2 };
        ASSERT(!zcl_command_registry_validate(&miss_reg, why, sizeof(why)));

        struct zcl_command_spec dup = leaf_base;
        dup.semantics = dup.summary;
        struct zcl_command_spec dup_specs[2] = { branch, dup };
        struct zcl_command_registry dup_reg = {
            .commands = dup_specs, .count = 2 };
        ASSERT(!zcl_command_registry_validate(&dup_reg, why, sizeof(why)));

        struct zcl_command_spec big = leaf_base;
        big.budget_bytes = 100000;
        struct zcl_command_spec big_specs[2] = { branch, big };
        struct zcl_command_registry big_reg = {
            .commands = big_specs, .count = 2 };
        ASSERT(!zcl_command_registry_validate(&big_reg, why, sizeof(why)));

        PASS();
    } _test_next:;
    return failures;
}

/* OS-B1: the real catalog now carries the contract on every leaf. */
static int test_leaf_semantics_and_budget(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every READY leaf has distinct non-empty semantics and a valid budget") {
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            ASSERT(s->budget_bytes == 0 ||
                   (s->budget_bytes >= 256 && s->budget_bytes <= 65536));
            if (s->mode == ZCL_COMMAND_MODE_BRANCH)
                continue;
            if (s->availability != ZCL_COMMAND_READY)
                continue;
            ASSERT(s->semantics != NULL && s->semantics[0] != 0);
            ASSERT(strcmp(s->semantics, s->summary) != 0);
        }
        PASS();
    } _test_next:;
    return failures;
}

/* OS-B1: describe surfaces the semantics contract and effective budget. */
static int test_describe_emits_semantics(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_SPEC_BUDGET + 1];
    TEST("describe emits the semantics contract and budget for a leaf") {
        size_t n = zcl_command_registry_describe_json(reg, "core.status", out,
                                                      sizeof(out));
        ASSERT(n > 0);
        ASSERT(strstr(out, "\"semantics\"") != NULL);
        ASSERT(strstr(out, "\"budget_bytes\"") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int g_bad_next_case;

static void contract_bad_next_handler(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
    if (g_bad_next_case == 0)
        (void)zcl_command_reply_add_next(reply, request->spec->path, "{}",
                                         "illegal self-loop");
    else if (g_bad_next_case == 1)
        (void)zcl_command_reply_add_next(reply, "discover.describe", "{}",
                                         "missing required path");
    else
        (void)zcl_command_reply_add_next(reply, "unknown.command", "{}",
                                         "unknown command");
}

static int test_next_actions_fail_closed(void)
{
    int failures = 0;
    const struct zcl_command_registry *catalog = zcl_command_catalog();
    TEST("next actions reject self-loops, missing required input, and unknown leaves") {
        const struct zcl_command_spec *base = find_spec(catalog, "core.status");
        ASSERT(base != NULL);
        struct zcl_command_spec executable = *base;
        executable.handler = contract_bad_next_handler;
        struct zcl_command_registry local = {
            .commands = &executable,
            .count = 1,
        };
        struct zcl_command_context context = {
            .registry = catalog,
            .operator_lane = "dev",
            .granted_capabilities = ~(uint64_t)0,
            .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
        };
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        for (g_bad_next_case = 0; g_bad_next_case < 3; g_bad_next_case++) {
            char out[ZCL_COMMAND_RESULT_BUDGET + 1];
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
            size_t n = zcl_command_registry_execute_json(
                &local, &executable, &context, &input, false,
                executable.path, "normal", 0, 0, NULL, out, sizeof(out),
                &code);
            ASSERT(n > 0);
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_INTERNAL);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
        }
        json_free(&input);

        char menu[ZCL_COMMAND_ROOT_BUDGET + 1];
        size_t n = zcl_command_registry_menu_json(catalog, "", menu,
                                                   sizeof(menu));
        ASSERT(n > 0);
        struct json_value root;
        ASSERT(json_read(&root, menu, n));
        const struct json_value *next = json_get(&root, "next");
        ASSERT(next && next->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(next, "command")),
                      "discover.describe");
        const struct json_value *next_input = json_get(next, "input");
        const struct zcl_command_spec *describe =
            find_spec(catalog, "discover.describe");
        char why[160] = {0};
        ASSERT(describe && next_input &&
               zcl_command_registry_input_validate(describe, next_input, why,
                                                   sizeof(why)));
        json_free(&root);
        PASS();
    } _test_next:;
    return failures;
}

/* ── OS-B2: the per-command latency envelope ─────────────────────────── */

static int test_latency_budget_mapping(void)
{
    int failures = 0;
    TEST("latency enum maps to the documented ms budget, total over the enum") {
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_INSTANT),
                  (int64_t)50);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_FAST),
                  (int64_t)250);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_FOREGROUND),
                  (int64_t)750);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_BACKGROUND),
                  (int64_t)900);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_PERSISTENT),
                  (int64_t)900);
        /* Out-of-range falls back to the PERSISTENT/900ms ceiling. */
        ASSERT_EQ(zcl_command_latency_budget_ms((enum zcl_command_latency)999),
                  (int64_t)900);
        PASS();
    } _test_next:;
    return failures;
}

static int test_envelope_carries_latency_contract(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("zcl.result.v1 carries budget_ms/elapsed_ms/budget_exceeded") {
        const struct zcl_command_spec *s = find_spec(reg, "discover.help");
        ASSERT(s != NULL);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT(strstr(out, "\"budget_ms\"") != NULL);
        ASSERT(strstr(out, "\"elapsed_ms\"") != NULL);
        ASSERT(strstr(out, "\"budget_exceeded\":false") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static bool b2_latency_in_scope(const struct zcl_command_spec *s)
{
    if (s->availability != ZCL_COMMAND_READY ||
        s->effect != ZCL_COMMAND_EFFECT_READ ||
        s->mode != ZCL_COMMAND_MODE_SYNC)
        return false;
    return strncmp(s->path, "discover.", 9) == 0 ||
           strncmp(s->path, "code.", 5) == 0;
}

static int test_ready_read_leaves_meet_latency_bucket(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every READY discover.*/code.* leaf's WARM dispatch meets its latency bucket") {
        /* budget_ms is a WARM-latency contract (docs/NATIVE_COMMAND_INTERFACE.md
         * §8 "warm latency class"). code.* leaves lazily build the in-binary
         * code index on their first call (a ~1s one-time O(codebase) scan);
         * that cold build is not the steady-state read this bucket budgets. So
         * warm each in-scope leaf once (result ignored), then assert the SECOND
         * dispatch's envelope meets the bucket. */
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (!b2_latency_in_scope(s))
                continue;
            char warm[ZCL_COMMAND_RESULT_BUDGET + 1];
            enum zcl_command_exit wcode = ZCL_COMMAND_EXIT_INTERNAL;
            (void)exec_leaf(reg, s, warm, sizeof(warm), &wcode);
        }
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (!b2_latency_in_scope(s))
                continue;
            char out[ZCL_COMMAND_RESULT_BUDGET + 1];
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            /* Dispatch with an empty object; leaves needing a required
             * positional fail input validation FAST (before any I/O) — still a
             * valid latency measurement, ok=false is expected and not asserted
             * here. */
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT(strstr(out, "\"budget_exceeded\":false") != NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_describe_emits_observed_p99(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("describe surfaces observed_p99_us/observed_samples after repeated dispatch") {
        const struct zcl_command_spec *s = find_spec(reg, "discover.help");
        ASSERT(s != NULL);
        char out[ZCL_COMMAND_RESULT_BUDGET + 1];
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        for (int i = 0; i < 10; i++)
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        char describe_out[ZCL_COMMAND_SPEC_BUDGET + 1];
        size_t n = zcl_command_registry_describe_json(reg, "discover.help",
                                                       describe_out,
                                                       sizeof(describe_out));
        ASSERT(n > 0);
        ASSERT(strstr(describe_out, "\"observed_p99_us\"") != NULL);
        /* Earlier tests in this binary already dispatched discover.help, so the
         * ring holds >= 10 samples (tests share the static g_latency_rings). */
        struct json_value root;
        ASSERT(json_read(&root, describe_out, n));
        const struct json_value *policy = json_get(&root, "policy");
        ASSERT(policy != NULL);
        int64_t samples = json_get_int(json_get(policy, "observed_samples"));
        ASSERT(samples >= (int64_t)10);
        json_free(&root);
        PASS();
    } _test_next:;
    return failures;
}

/* The operator rollup dashboards ported from the legacy MCP ops controller
 * (tools/mcp/controllers/ops_controller.c) that had no native twin. Each is a
 * READY read leaf under ops.debug.dash, dispatched by the MCP-free bridge via a
 * re-homed body function (never an RPC-shape binding), and named by a
 * tool-for-path entry so the selftest sweep and the "exactly one dispatch"
 * invariant both accept it. */
static int test_ops_dash_dashboards_ported(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("ops.debug.dash operator dashboards are bridged READY leaves") {
        const struct {
            const char *path;
            const char *tool;
        } leaves[] = {
            { "ops.debug.dash.kpi", "zcl_kpi" },
            { "ops.debug.dash.snapshot", "zcl_operator_snapshot" },
            { "ops.debug.dash.summary", "zcl_operator_summary" },
            { "ops.debug.dash.milestone", "zcl_milestone" },
            { "ops.debug.dash.mirror", "zcl_mirror_status" },
            { "ops.debug.dash.selfheal", "zcl_self_heal_stats" },
        };
        const struct zcl_command_spec *branch =
            find_spec(reg, "ops.debug.dash");
        ASSERT(branch != NULL);
        ASSERT_EQ(branch->mode, ZCL_COMMAND_MODE_BRANCH);
        ASSERT_STR_EQ(branch->parent, "ops.debug");
        for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, leaves[i].path);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
            ASSERT_EQ(s->effect, ZCL_COMMAND_EFFECT_READ);
            ASSERT_STR_EQ(s->parent, "ops.debug.dash");
            ASSERT(s->handler == zcl_native_bridge_command);
            const char *tool = zcl_native_bridge_tool_for_path(leaves[i].path);
            ASSERT(tool != NULL);
            ASSERT_STR_EQ(tool, leaves[i].tool);
            /* body-backed composition, never an RPC-shape binding */
            ASSERT(zcl_native_bridge_body_for_path(leaves[i].path) != NULL);
            ASSERT(zcl_native_bridge_rpc_for_path(leaves[i].path) == NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}

int test_command_registry_catalog(void)
{
    int failures = 0;
    failures += test_catalog_wellformed();
    failures += test_semantics_contract_negative();
    failures += test_leaf_semantics_and_budget();
    failures += test_describe_emits_semantics();
    failures += test_latency_budget_mapping();
    failures += test_envelope_carries_latency_contract();
    failures += test_ready_read_leaves_meet_latency_bucket();
    failures += test_describe_emits_observed_p99();
    failures += test_next_actions_fail_closed();
    failures += test_domain_leaf_counts();
    failures += test_six_roots();
    failures += test_root_menu_budget();
    failures += test_branch_menus_shallow();
    failures += test_search_bounded();
    failures += test_search_multiword();
    failures += test_ready_leaves_bound();
    failures += test_bridge_bindings_reverse();
    failures += test_bridge_rpc_errors_fail_closed();
    failures += test_bridge_rpc_success_shapes_fail_closed();
    failures += test_status_brief_flat_lean_envelope();
    failures += test_status_brief_composite_fails_closed();
    failures += test_status_brief_valid_unknown_and_partial_contracts();
    failures += test_status_brief_rejects_contract_contradictions();
    failures += test_status_brief_names_first_failing_field();
    failures += test_bridge_mcp_free_bindings();
    failures += test_planned_fail_closed();
    failures += test_envelope_vectors();
    failures += test_dev_branch_leaves();
    failures += test_response_budget_views();
    failures += test_typo_stays_branch();
    failures += test_ops_selftest_registry();
    failures += test_ops_dash_dashboards_ported();
    failures += test_ops_state_requires_subsystem();
    failures += test_dev_vcs_revert_release_stub();
    failures += test_dev_vcs_seal_grant_release_stub();
    failures += test_is_root_ownership();
    printf("=== command_registry_catalog: %d failures ===\n", failures);
    return failures;
}
