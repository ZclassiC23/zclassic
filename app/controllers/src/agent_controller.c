/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native AI/operator contracts for coding agents. These RPCs are pure JSON
 * discovery/introspection helpers; they do not mutate node, wallet, chain, or
 * consensus state. */

#include "controllers/agent_controller.h"
#include "controllers/agent_background_quality.h"
#include "controllers/agent_impact_rules.h"
#include "controllers/strong_params.h"

#include "json/json.h"
#include "rpc/server.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void agent_push_str(struct json_value *arr, const char *s)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, s ? s : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static void agent_push_build_command(struct json_value *arr, const char *name,
                                     const char *command,
                                     const char *purpose)
{
    struct json_value cmd;
    json_init(&cmd);
    json_set_object(&cmd);
    json_push_kv_str(&cmd, "name", name);
    json_push_kv_str(&cmd, "command", command);
    json_push_kv_str(&cmd, "purpose", purpose);
    json_push_back(arr, &cmd);
    json_free(&cmd);
}

static void agent_push_build_knob(struct json_value *arr, const char *name,
                                  const char *value,
                                  const char *purpose)
{
    struct json_value knob;
    json_init(&knob);
    json_set_object(&knob);
    json_push_kv_str(&knob, "name", name);
    json_push_kv_str(&knob, "value", value);
    json_push_kv_str(&knob, "purpose", purpose);
    json_push_back(arr, &knob);
    json_free(&knob);
}

static void agent_push_subsystem(struct json_value *arr, const char *name,
                                 const char *purpose, const char *files, const char *docs,
                                 const char *tests, const char *mcp_tool)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "name", name);
    json_push_kv_str(&obj, "purpose", purpose);
    json_push_kv_str(&obj, "files", files);
    json_push_kv_str(&obj, "docs", docs);
    json_push_kv_str(&obj, "tests", tests);
    json_push_kv_str(&obj, "mcp_tool", mcp_tool);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static bool agent_str_starts(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool agent_path_is_native_agent_api(const char *path)
{
    static const char *const paths[] = {
        "app/controllers/src/agent_controller.c", "app/controllers/src/agent_background_quality.c", "app/controllers/src/agent_anchor_status_controller.c", "app/controllers/src/agent_diagnose_controller.c", "app/controllers/src/agent_impact_rules.c", "app/controllers/src/agent_interface_controller.c",
        "app/controllers/src/agent_contracts_controller.c", "app/controllers/src/agent_lane_runtime.c", "app/controllers/src/agent_lanes_controller.c", "app/controllers/src/agent_liveness_controller.c", "app/controllers/src/agent_operator_contracts.c", "app/controllers/src/agent_ops_controller.c", "app/controllers/src/agent_resources.c", "app/controllers/src/agent_restart_watchdog.c",
        "app/controllers/src/agent_runtime_controller.c", "app/controllers/src/agent_first_call.c", "app/controllers/src/event_agent_peers.c", "app/controllers/src/event_agent_peers.h", "app/controllers/src/event_agent_summary.c",
        "app/controllers/src/event_agent_summary.h", "app/controllers/src/event_timeline_controller.c", "app/controllers/src/event_timeline_filter_controller.c", "app/controllers/include/controllers/event_timeline_controller.h", "app/controllers/include/controllers/event_timeline_filter_controller.h", "app/controllers/include/controllers/agent_controller.h", "app/controllers/include/controllers/agent_background_quality.h", "app/controllers/include/controllers/agent_contracts.def", "app/controllers/include/controllers/agent_impact_rules.def",
        "app/controllers/include/controllers/agent_first_call.h", "app/controllers/include/controllers/agent_impact_rules.h", "app/controllers/include/controllers/agent_operator_contracts.h", "app/controllers/include/controllers/agent_resources.h", "app/controllers/include/controllers/agent_restart_watchdog.h",
        "app/controllers/src/diagnostics_registry.c", "app/controllers/src/diagnostics_controller.c", "app/controllers/include/controllers/diagnostics_controller.h", "app/controllers/include/controllers/diagnostics_internal.h",
    };

    if (!path || !path[0])
        return false;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (strcmp(path, paths[i]) == 0)
            return true;
    }
    return false;
}

static const char *agent_classify_path(const char *path,
                                       struct agent_impact_acc *acc,
                                       const char **risk_out,
                                       const char **docs_out)
{
    const char *subsystem = "general";
    const char *risk = "normal";
    const char *docs = "docs/CODEBASE_MAP.md";
    bool shared_match = agent_impact_apply_shared_rules(path, acc);

    if (shared_match && path && !agent_str_starts(path, "docs/"))
        acc->code_changed = true;

    if (!path || !path[0]) {
        risk = "invalid";
        docs = "docs/AGENT_API.md";
    } else if (agent_str_starts(path, "docs/")) {
        subsystem = "documentation";
        risk = "docs_only";
        docs = path;
    } else if (strcmp(path, "Makefile") == 0 ||
               agent_str_starts(path, "tools/agent_fast_ci") ||
               agent_str_starts(path, "tools/githooks/")) {
        subsystem = "fast_ci_and_gates";
        docs = "docs/work/fast-path.md";
        acc->code_changed = true;
    } else if (strcmp(path, "tools/z") == 0) {
        subsystem = "deprecated_tools_z_shim";
        risk = "operator_api";
        docs = "docs/AGENT_API.md";
        acc->agent_api_changed = true;
        acc->code_changed = true;
    } else if (agent_str_starts(path, "tools/mcp/")) {
        subsystem = "mcp_agent_api";
        risk = "operator_api";
        docs = "docs/AGENT_API.md";
        acc->mcp_changed = true;
        acc->agent_api_changed = true;
        acc->code_changed = true;
    } else if (agent_path_is_native_agent_api(path)) {
        subsystem = "native_agent_api";
        risk = "operator_api";
        docs = "docs/AGENT_API.md";
        acc->agent_api_changed = true;
        acc->code_changed = true;
    } else if (strcmp(path, "app/controllers/src/event_controller.c") == 0) {
        subsystem = "native_status_api";
        risk = "operator_api";
        docs = "docs/AGENT_API.md";
        acc->agent_api_changed = true;
        acc->code_changed = true;
    } else if (strcmp(path, "app/controllers/src/health_controller.c") == 0 ||
               strcmp(path,
                      "app/controllers/include/controllers/health_controller.h") == 0) {
        subsystem = "native_status_api";
        risk = "operator_api";
        docs = "docs/AGENT_API.md";
        acc->agent_api_changed = true;
        acc->code_changed = true;
    } else if (strcmp(path, "src/main.c") == 0) {
        subsystem = "native_cli_api";
        risk = "operator_api";
        docs = "docs/AGENT_API.md";
        acc->agent_api_changed = true;
        acc->code_changed = true;
    } else if (agent_str_starts(path, "app/controllers/src/api_controller")) {
        subsystem = "rest_agent_api";
        risk = "operator_api";
        docs = "docs/CODEBASE_MAP.md";
        acc->agent_api_changed = true;
        acc->code_changed = true;
    } else if (strcmp(path, "app/services/src/node_health_service.c") == 0 ||
               agent_str_starts(path,
                                "app/services/include/services/node_health_service")) {
        subsystem = "node_health";
        risk = "operator_health";
        docs = "docs/RUNBOOK.md";
        acc->code_changed = true;
    } else if (agent_str_starts(path, "domain/consensus/") ||
               agent_str_starts(path, "lib/consensus/") ||
               agent_str_starts(path, "lib/chain/") ||
               agent_str_starts(path, "lib/validation/") ||
               agent_str_starts(path, "lib/script/") ||
               agent_str_starts(path, "lib/primitives/")) {
        subsystem = "consensus_or_validation";
        risk = "consensus_parity";
        docs = "docs/CONSENSUS_PARITY_DOCTRINE.md";
        acc->consensus_risk = true;
        acc->code_changed = true;
    } else if (agent_str_starts(path, "app/jobs/")) {
        subsystem = "reducer_stage";
        risk = "reducer_liveness";
        docs = "docs/HOW_THE_NODE_WORKS.md";
        if (!shared_match) {
            agent_impact_add_group(acc, "stage_repair");
            agent_impact_add_group(acc, "reducer_frontier");
        }
        acc->code_changed = true;
    } else if (agent_str_starts(path, "app/services/")) {
        subsystem = "service_layer";
        risk = "operator_liveness";
        docs = "docs/CODEBASE_MAP.md";
        if (!shared_match)
            agent_impact_add_group(acc, "make_lint_gates");
        acc->code_changed = true;
    } else if (agent_str_starts(path, "lib/test/")) {
        subsystem = "test_harness";
        docs = "docs/work/fast-path.md";
        if (!shared_match)
            agent_impact_add_group(acc, "make_lint_gates");
        acc->code_changed = true;
    } else if (agent_str_starts(path, "app/") ||
               agent_str_starts(path, "lib/") ||
               agent_str_starts(path, "config/") ||
               agent_str_starts(path, "src/")) {
        subsystem = "code";
        docs = "docs/CODEBASE_MAP.md";
        if (!shared_match)
            agent_impact_add_group(acc, "make_lint_gates");
        acc->code_changed = true;
    }

    if (risk_out) *risk_out = risk;
    if (docs_out) *docs_out = docs;
    return subsystem;
}

bool rpc_agent_map(const struct json_value *params, bool help,
                   struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agentmap\n"
        "\nReturn the AI-coder map: primary native/MCP commands, where the\n"
        "operator API lives, which docs explain it, and which tests cover it.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_map.v1\", \"commands\":[...], "
        "\"subsystems\":[...] }\n");

    struct json_value commands, subsystems, telemetry, shim;
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_map.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "summary",
                     "Use the zclassic23 binary or MCP directly; tools/z is a deprecated shim.");

    json_init(&commands);
    json_set_array(&commands);
    agent_push_contract_command_surface_json(&commands,
                                             "agentmap.commands.core");
    agent_push_contract_command_surface_json(
        &commands, "agentmap.commands.drilldown");
    json_push_kv(result, "commands", &commands);
    json_free(&commands);

    json_init(&telemetry);
    json_set_array(&telemetry);
    agent_push_contract_command_surface_json(&telemetry,
                                             "agentmap.telemetry");
    json_push_kv(result, "telemetry_drilldowns", &telemetry);
    json_free(&telemetry);

    json_init(&subsystems);
    json_set_array(&subsystems);
    agent_push_subsystem(&subsystems, "native_agent_api",
                         "first-call binary JSON contracts",
                         "app/controllers/src/agent_controller.c, app/controllers/src/agent_contracts_controller.c, app/controllers/src/agent_anchor_status_controller.c, app/controllers/src/agent_background_quality.c, app/controllers/src/agent_lane_runtime.c, app/controllers/src/agent_lanes_controller.c, app/controllers/src/agent_liveness_controller.c, app/controllers/src/agent_runtime_controller.c, app/controllers/include/controllers/agent_contracts.def, src/main.c",
                         "docs/AGENT_API.md",
                         "syncdiag_rpc, mcp_controllers, make_lint_gates",
                         "zcl_agent, zcl_agent_interface, zcl_agent_lanes, zcl_agent_liveness, zcl_agent_map");
    agent_push_subsystem(&subsystems, "mcp_agent_api",
                         "typed AI tool routes over the same native RPC truth",
                         "tools/mcp/controllers/ops_controller.c",
                         "docs/CODEBASE_MAP.md",
                         "mcp_controllers",
                         "zcl_tools_list, zcl_openapi");
    agent_push_subsystem(&subsystems, "rest_agent_api",
                         "public HTTP status/discovery mirror",
                         "app/controllers/src/api_controller_status.c",
                         "docs/CODEBASE_MAP.md",
                         "api",
                         "zcl_openapi");
    agent_push_subsystem(&subsystems, "health_and_blockers",
                         "serving/healthy/blocker semantics",
                         "app/services/src/node_health_service.c",
                         "docs/RUNBOOK.md",
                         "node_health_service, syncdiag_rpc",
                         "zcl_health, zcl_conditions");
    agent_push_subsystem(&subsystems, "fast_ci",
                         "cache-aware edit loop and focused test routing",
                         "app/controllers/include/controllers/agent_impact_rules.def, tools/agent_fast_ci.sh, Makefile",
                         "docs/work/fast-path.md",
                         "make_lint_gates",
                         "zcl_agent_impact, zcl_agent_build");
    agent_push_subsystem(&subsystems, "background_quality_lanes",
                         "long fuzz and coverage proof work outside the push path",
                         "app/controllers/src/agent_background_quality.c, tools/scripts/background_quality_lane.sh, deploy/zclassic23-fuzz.service, deploy/zclassic23-coverage.service",
                         "docs/work/fast-path.md",
                         "make_lint_gates",
                         "zcl_agent_build");
    json_push_kv(result, "subsystems", &subsystems);
    json_free(&subsystems);

    json_init(&shim);
    json_set_object(&shim);
    json_push_kv_str(&shim, "name", "tools/z");
    json_push_kv_bool(&shim, "primary", false);
    json_push_kv_str(&shim, "status", "deprecated_compatibility_shim");
    json_push_kv_str(&shim, "replacement",
                     "zclassic23 agent, zclassic23 agentmap, MCP zcl_agent_*");
    json_push_kv(result, "deprecated_shim", &shim);
    json_free(&shim);
    return true;
}

bool rpc_agent_impact(const struct json_value *params, bool help,
                      struct json_value *result)
{
    RPC_HELP(help, result,
        "agentimpact ( file ... )\n"
        "\nMap changed file paths to subsystem, risk, docs, and relevant tests.\n"
        "Pass explicit paths; the node never shells out to git.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_impact.v1\", "
        "\"relevant_test_groups\":[\"syncdiag_rpc\"], ... }\n");

    struct agent_impact_acc acc = {0};
    acc.docs_only = true;
    struct json_value files;
    json_init(&files);
    json_set_array(&files);

    size_t file_count = 0;
    if (params && params->type == JSON_ARR) {
        for (size_t i = 0; i < json_size(params); i++) {
            const struct json_value *v = json_at(params, i);
            const char *path = v && v->type == JSON_STR
                ? json_get_str(v) : "";
            const char *risk = NULL;
            const char *docs = NULL;
            const char *subsystem = agent_classify_path(path, &acc,
                                                        &risk, &docs);
            if (risk && strcmp(risk, "docs_only") != 0)
                acc.docs_only = false;

            struct json_value entry;
            json_init(&entry);
            json_set_object(&entry);
            json_push_kv_str(&entry, "path", path);
            json_push_kv_str(&entry, "subsystem", subsystem);
            json_push_kv_str(&entry, "risk", risk);
            json_push_kv_str(&entry, "docs", docs);
            json_push_back(&files, &entry);
            json_free(&entry);
            file_count++;
        }
    }

    if (file_count == 0)
        acc.docs_only = false;

    char group_csv[512] = {0};
    struct json_value groups, commands, gates;
    json_init(&groups);
    json_set_array(&groups);
    for (size_t i = 0; i < acc.groups_len; i++) {
        if (i > 0)
            strncat(group_csv, ",", sizeof(group_csv) - strlen(group_csv) - 1);
        strncat(group_csv, acc.groups[i],
                sizeof(group_csv) - strlen(group_csv) - 1);
        agent_push_str(&groups, acc.groups[i]);
    }

    json_init(&commands);
    json_set_array(&commands);
    agent_push_str(&commands, "make lint");
    agent_push_str(&commands, "make build-only");
    for (size_t i = 0; i < acc.groups_len; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "make t ONLY=%s", acc.groups[i]);
        agent_push_str(&commands, cmd);
    }
    if (acc.groups_len > 0) {
        char cmd[640];
        snprintf(cmd, sizeof(cmd), "ZCL_FAST_TESTS=%s make fast-ci",
                 group_csv);
        agent_push_str(&commands, cmd);
    } else {
        agent_push_str(&commands, "make fast-ci");
    }

    json_init(&gates);
    json_set_array(&gates);
    agent_push_str(&gates, "make lint");
    agent_push_str(&gates, "make build-only");
    agent_push_str(&gates, "relevant strict make t ONLY=<group>");
    agent_push_str(&gates, "tracked pre-push hook runs make pre-push-ci");
    agent_push_str(&gates,
                   "long fuzz/coverage evidence runs in background quality lanes");

    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_impact.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_int(result, "files_count", (int64_t)file_count);
    json_push_kv_bool(result, "code_changed", acc.code_changed);
    json_push_kv_bool(result, "docs_only", acc.docs_only);
    json_push_kv_bool(result, "consensus_risk", acc.consensus_risk);
    json_push_kv_bool(result, "agent_api_changed", acc.agent_api_changed);
    json_push_kv_bool(result, "mcp_changed", acc.mcp_changed);
    json_push_kv_str(result, "mapping_source",
                     "app/controllers/include/controllers/agent_impact_rules.def");
    json_push_kv_int(result, "shared_rule_count",
                     (int64_t)agent_impact_rule_count());
    json_push_kv_int(result, "shared_rule_hits",
                     (int64_t)acc.shared_rule_hits);
    json_push_kv_int(result, "relevant_test_groups_count",
                     (int64_t)acc.groups_len);
    json_push_kv_str(result, "source",
                     file_count > 0 ? "explicit_file_args"
                                    : "no_files_supplied");
    json_push_kv_str(result, "empty_input_hint",
                     "pass changed paths, e.g. zclassic23 agentimpact $(git diff --name-only origin/main...HEAD)");
    json_push_kv(result, "files", &files);
    json_push_kv(result, "relevant_test_groups", &groups);
    json_push_kv(result, "recommended_commands", &commands);
    json_push_kv(result, "required_pre_push_gates", &gates);

    json_free(&files);
    json_free(&groups);
    json_free(&commands);
    json_free(&gates);
    return true;
}

bool rpc_agent_build(const struct json_value *params, bool help,
                     struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agentbuild\n"
        "\nReturn the fast-build contract for AI C development: incremental\n"
        "object rebuilds, cache knobs, focused tests, and the byte-identity\n"
        "reproducibility gate.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_build.v1\", "
        "\"commands\":[...], \"reproducible_release\":{...} }\n");

    struct json_value loop, incremental, dev, cache, knobs, commands, repro,
                      background, quality_status, lanes, gates;
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_build.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "language", "c23");
    json_push_kv_str(result, "summary",
                     "Use make fast-compile for the cheapest no-link dev compile, make fast-rebuild when a runnable non-LTO dev node is needed, compiler caches when available, and make ci-reproducible for byte identity.");

    json_init(&loop);
    json_set_object(&loop);
    json_push_kv_str(&loop, "schema", "zcl.agent_build_loop.v1");
    json_push_kv_int(&loop, "schema_version", 1);
    json_push_kv_str(&loop, "default_edit_gate", "make fast-ci");
    json_push_kv_str(&loop, "fast_no_link_compile", "make fast-compile");
    json_push_kv_str(&loop, "strict_no_link_compile", "make build-only");
    json_push_kv_str(&loop, "runnable_dev_binary", "make fast-rebuild");
    json_push_kv_str(&loop, "focused_fast_test", "make t-fast ONLY=<group>");
    json_push_kv_str(&loop, "pre_push_gate", "make pre-push-ci");
    json_push_kv_str(&loop, "fast_ci_compile_default",
                     "ZCL_FAST_COMPILE=dev -> make fast-compile");
    json_push_kv_str(&loop, "pre_push_compile_default",
                     "ZCL_FAST_COMPILE=strict -> make build-only");
    json_push_kv_str(&loop, "rule",
                     "Compile cheaply first; link a dev binary only when you need to run it; push through the strict pre-push gate.");
    json_push_kv(result, "recommended_loop", &loop);
    json_free(&loop);

    json_init(&incremental);
    json_set_object(&incremental);
    json_push_kv_bool(&incremental, "enabled", true);
    json_push_kv_str(&incremental, "node_object_dir", "build/obj");
    json_push_kv_str(&incremental, "dev_node_object_dir", "build/dev-obj");
    json_push_kv_str(&incremental, "fast_test_object_dir", "build/test-obj");
    json_push_kv_bool(&incremental, "header_depfiles", true);
    json_push_kv_str(&incremental, "depfile_rule",
                     "-MMD -MP with included .d files for build-only, dev-bin, and test_parallel_fast");
    json_push_kv_str(&incremental, "compile_check", "make build-only");
    json_push_kv_str(&incremental, "fast_compile_check",
                     "make fast-compile");
    json_push_kv_str(&incremental, "strict_compile_check",
                     "make build-only");
    json_push_kv_str(&incremental, "dev_binary_command", "make fast-rebuild");
    json_push_kv_str(&incremental, "behavior",
                     "Unchanged translation units keep their .o files; changed headers recompile dependent objects through .d files.");
    json_push_kv_str(&incremental, "whole_program_link_caveat",
                     "The release binary still uses whole-program LTO; make dev-bin is the fast linked executable gate.");
    json_push_kv(result, "incremental_compile", &incremental);
    json_free(&incremental);

    json_init(&dev);
    json_set_object(&dev);
    json_push_kv_bool(&dev, "enabled", true);
    json_push_kv_str(&dev, "command", "make fast-rebuild");
    json_push_kv_str(&dev, "alias", "make dev-bin; make zclassic23-dev");
    json_push_kv_str(&dev, "binary", "build/bin/zclassic23-dev");
    json_push_kv_str(&dev, "object_dir", "build/dev-obj");
    json_push_kv_bool(&dev, "lto", false);
    json_push_kv_bool(&dev, "stripped", false);
    json_push_kv_bool(&dev, "release_or_deploy_artifact", false);
    json_push_kv_str(&dev, "default_opt", "ZCL_DEV_OPT=-Og");
    json_push_kv_str(&dev, "hot_opt", "ZCL_DEV_HOT_OPT=-O2");
    json_push_kv_str(&dev, "hot_path_buckets",
                     "lib/chain, lib/consensus, lib/crypto, lib/primitives, lib/sapling, lib/script, lib/validation");
    json_push_kv_str(&dev, "linker_knob", "ZCL_DEV_LINKER");
    json_push_kv_str(&dev, "purpose",
                     "Run changed native agent, diagnostics, parser, and API code without paying the release LTO link.");
    json_push_kv(result, "dev_node_binary", &dev);
    json_free(&dev);

    json_init(&cache);
    json_set_object(&cache);
    json_push_kv_str(&cache, "auto_select_order", "sccache cc, ccache cc, cc");
    json_push_kv_str(&cache, "makefile_auto_wrapper",
                     "ccache cc when ccache exists; set ZCL_USE_CCACHE=0 to disable");
    json_push_kv_str(&cache, "script", "tools/agent_fast_ci.sh");
    json_push_kv_str(&cache, "default_loop", "make fast-ci");
    json_push_kv_str(&cache, "success_fingerprint_dir",
                     ".cache/zcl-agent-fast-ci");
    json_push_kv_bool(&cache, "cache_hit_refreshes_live_probe", true);
    json_push_kv_bool(&cache, "safe_for_edit_loop", true);

    json_init(&knobs);
    json_set_array(&knobs);
    agent_push_build_knob(&knobs, "ZCL_FAST_CC", "sccache cc",
                          "override compiler/cache wrapper");
    agent_push_build_knob(&knobs, "ZCL_FAST_JOBS", "auto, capped at 16",
                          "parallel jobs for fast-ci");
    agent_push_build_knob(&knobs, "ZCL_FAST_COMPILE", "dev",
                          "dev uses make fast-compile; strict uses make build-only");
    agent_push_build_knob(&knobs, "ZCL_FAST_TESTS",
                          "group[,group]",
                          "force focused test groups");
    agent_push_build_knob(&knobs, "ZCL_FAST_STRICT_TESTS", "0",
                          "set to 1 to use strict make t");
    agent_push_build_knob(&knobs, "ZCL_FAST_LIVE", "auto",
                          "set to 0 to skip live linger-service probe");
    agent_push_build_knob(&knobs, "ZCL_FAST_CACHE", "1",
                          "set to 0 to force lint/build/focused tests");
    agent_push_build_knob(&knobs, "ZCL_FAST_CACHE_RESET", "0",
                          "set to 1 to clear the fast-ci green-input cache");
    agent_push_build_knob(&knobs, "ZCL_FAST_CACHE_DIR",
                          ".cache/zcl-agent-fast-ci",
                          "override green-input cache directory");
    agent_push_build_knob(&knobs, "ZCL_USE_CCACHE", "1",
                          "Makefile auto-wraps CC with ccache when available; set 0 to disable");
    agent_push_build_knob(&knobs, "ZCL_DEV_OPT", "-Og",
                          "default optimization for make dev-bin");
    agent_push_build_knob(&knobs, "ZCL_DEV_HOT_OPT", "-O2",
                          "optimization for dev-bin hot-path buckets");
    agent_push_build_knob(&knobs, "ZCL_DEV_LINKER", "auto",
                          "optional fast linker for make dev-bin");
    json_push_kv(&cache, "knobs", &knobs);
    json_free(&knobs);
    json_push_kv(result, "cache", &cache);
    json_free(&cache);

    json_init(&commands);
    json_set_array(&commands);
    agent_push_build_command(&commands, "fast_compile", "make fast-compile",
                             "fastest non-LTO no-link dev compile check");
    agent_push_build_command(&commands, "compile_check", "make build-only",
                             "strict incremental no-link compile of all release-flag node objects");
    agent_push_build_command(&commands, "fast_rebuild", "make fast-rebuild",
                             "incremental non-LTO node executable; preferred local rebuild target");
    agent_push_build_command(&commands, "dev_node_binary", "make dev-bin",
                             "incremental non-LTO node executable for local agent/API iteration");
    agent_push_build_command(&commands, "focused_fast_test",
                             "make t-fast ONLY=<group>",
                             "cached non-LTO per-file test harness");
    agent_push_build_command(&commands, "agent_fast_ci", "make fast-ci",
                             "lint-fast, build-only, focused tests, live probe");
    agent_push_build_command(&commands, "strict_focused_test",
                             "make t ONLY=<group>",
                             "strict rebuilt harness for relevant tests");
    agent_push_build_command(&commands, "full_lint", "make lint",
                             "full defensive-coding and parity lint gates");
    agent_push_build_command(&commands, "byte_identity",
                             "make ci-reproducible",
                             "build twice in isolated dirs and cmp binaries");
    agent_push_build_command(&commands, "background_quality_status",
                             "make quality-linger-status",
                             "show latest background tests/fuzz/coverage JSON verdicts");
    json_push_kv(result, "commands", &commands);
    json_free(&commands);

    json_init(&repro);
    json_set_object(&repro);
    json_push_kv_str(&repro, "command", "make ci-reproducible");
    json_push_kv_str(&repro, "script",
                     "tools/scripts/check_reproducible_build.sh");
    json_push_kv_str(&repro, "flag_source",
                     "tools/scripts/repro_build_vars.sh");
    json_push_kv_str(&repro, "source_date_epoch",
                     "HEAD commit time unless caller overrides");
    json_push_kv_str(&repro, "portable_isa", "x86-64-v3");
    json_push_kv_str(&repro, "linker_build_id", "disabled");
    json_push_kv_bool(&repro, "isolated_build_dirs", true);
    json_push_kv_bool(&repro, "byte_for_byte_cmp", true);
    json_push_kv(result, "reproducible_release", &repro);
    json_free(&repro);

    json_init(&background);
    json_set_object(&background);
    json_push_kv_str(&background, "install", "make install-quality-linger");
    json_push_kv_str(&background, "status", "make quality-linger-status");
    json_push_kv_str(&background, "script",
                     "tools/scripts/background_quality_lane.sh");
    json_push_kv_str(&background, "state_dir",
                     "~/.local/state/zclassic23-quality");
    json_push_kv_bool(&background, "pre_push_blocks_on_long_lanes", false);

    json_init(&lanes);
    json_set_array(&lanes);
    agent_push_build_command(&lanes, "fuzz",
                             "zclassic23-fuzz.timer",
                             "hourly libFuzzer background lane with JSON verdict");
    agent_push_build_command(&lanes, "coverage",
                             "zclassic23-coverage.timer",
                             "weekly coverage lane with JSON verdict");
    agent_push_build_command(&lanes, "tests",
                             "zclassic23-test-suite.timer",
                             "hourly full test suite lane with JSON verdict");
    json_push_kv(&background, "lanes", &lanes);
    json_free(&lanes);
    json_push_kv(result, "background_quality_lanes", &background);
    json_free(&background);

    json_init(&quality_status);
    agent_build_background_quality_status(&quality_status);
    json_push_kv(result, "background_quality_status", &quality_status);
    json_free(&quality_status);

    json_init(&gates);
    json_set_array(&gates);
    agent_push_str(&gates, "make lint");
    agent_push_str(&gates, "make build-only");
    agent_push_str(&gates, "relevant strict make t ONLY=<group>");
    agent_push_str(&gates,
                   "tracked pre-push hook runs make pre-push-ci");
    agent_push_str(&gates,
                   "pre-push-ci skips live service probe; inspect live health separately");
    agent_push_str(&gates,
                   "full-suite/fuzz/coverage lanes run via make install-quality-linger");
    json_push_kv(result, "pre_push_gates", &gates);
    json_free(&gates);
    return true;
}
