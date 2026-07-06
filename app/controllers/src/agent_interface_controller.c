/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Versioned AI/operator interface contracts. These are pure JSON discovery and
 * safety-decision helpers; they do not mutate node, wallet, chain, or
 * consensus state. */

#include "controllers/agent_controller.h"
#include "controllers/strong_params.h"

#include "json/json.h"
#include "util/clientversion.h"

#include <stdint.h>
#include <string.h>

static void agent_interface_push_str(struct json_value *arr, const char *s)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, s ? s : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static void agent_interface_push_transport(struct json_value *arr,
                                           int64_t rank,
                                           const char *name,
                                           const char *interface,
                                           const char *first_call,
                                           const char *format,
                                           const char *use_for)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_int(&obj, "rank", rank);
    json_push_kv_str(&obj, "name", name);
    json_push_kv_str(&obj, "interface", interface);
    json_push_kv_str(&obj, "first_call", first_call);
    json_push_kv_str(&obj, "format", format);
    json_push_kv_str(&obj, "use_for", use_for);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static void agent_interface_push_capability(struct json_value *arr,
                                            const char *name,
                                            const char *schema,
                                            const char *native,
                                            const char *mcp,
                                            const char *rest,
                                            const char *purpose)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "name", name);
    json_push_kv_str(&obj, "schema", schema);
    json_push_kv_str(&obj, "native", native);
    json_push_kv_str(&obj, "mcp", mcp);
    json_push_kv_str(&obj, "rest", rest);
    json_push_kv_str(&obj, "purpose", purpose);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static const char *agent_interface_param0_str(const struct json_value *params,
                                              const char *fallback)
{
    if (!params)
        return fallback;

    const struct json_value *v = NULL;
    if (params->type == JSON_OBJ) {
        v = json_get(params, "action");
    } else if (params->type == JSON_ARR && json_size(params) > 0) {
        v = json_at(params, 0);
    }
    if (!v || v->type != JSON_STR)
        return fallback;
    const char *s = json_get_str(v);
    return (s && s[0]) ? s : fallback;
}

static bool agent_deploy_action_known(const char *action)
{
    return action &&
           (strcmp(action, "canonical-deploy") == 0 ||
            strcmp(action, "canonical-restart") == 0 ||
            strcmp(action, "deploy") == 0 ||
            strcmp(action, "restart") == 0);
}

static bool agent_deploy_action_is_restart(const char *action)
{
    return action &&
           (strcmp(action, "canonical-restart") == 0 ||
            strcmp(action, "restart") == 0);
}

bool rpc_agent_interface(const struct json_value *params, bool help,
                         struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agentinterface\n"
        "\nReturn the preferred AI development interface: ranked transports,\n"
        "JSON rules, native C ownership, and shell/Python anti-patterns.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_interface.v1\", "
        "\"preferred_transport\":\"mcp\", ... }\n");

    struct json_value transports, capabilities, machine, runtime, loop,
                      native_owned, avoid, versioning;
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_interface.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_str(result, "preferred_transport", "mcp");
    json_push_kv_str(result, "preferred_payload", "json");
    json_push_kv_str(result, "canonical_implementation",
                     "app/controllers/src/agent_interface_controller.c");
    json_push_kv_str(result, "capabilities_schema",
                     "zcl.agent_capability.v1");
    json_push_kv_str(result, "summary",
                     "Use MCP for typed interactive agent work, native zclassic23 JSON for scripts and humans, REST only for public read-only mirrors.");

    json_init(&transports);
    json_set_array(&transports);
    agent_interface_push_transport(&transports, 1, "mcp", "zcl_agent_*",
        "zcl_agent_interface", "typed JSON",
        "interactive AI operations, typed parameters, tool discovery, and drill-downs");
    agent_interface_push_transport(&transports, 2, "native_cli",
        "zclassic23 agent*", "zclassic23 agentinterface", "stdout JSON",
        "local terminal use, scripts, and zero-wrapper diagnostics");
    agent_interface_push_transport(&transports, 3, "rest",
        "GET /api/v1/agent", "GET /api/v1/agent", "HTTP JSON",
        "public read-only status and explorer-facing discovery");
    json_push_kv(result, "transports", &transports);
    json_free(&transports);

    json_init(&capabilities);
    json_set_array(&capabilities);
    agent_interface_push_capability(&capabilities, "discover_interface",
        "zcl.agent_interface.v1", "zclassic23 agentinterface",
        "zcl_agent_interface", "",
        "ranked transports, payload rules, and C ownership boundaries");
    agent_interface_push_capability(&capabilities, "runtime_status",
        "zcl.public_status.v1", "zclassic23 agent", "zcl_agent",
        "GET /api/v1/agent",
        "compact live status, lane safety, blocker, and next action");
    agent_interface_push_capability(&capabilities, "mirror_status",
        "zcl.mirror_status.v1", "zclassic23 getmirrorstatus",
        "zcl_mirror_status", "",
        "mirror reachability, lag, hash agreement, and active blocker contract");
    agent_interface_push_capability(&capabilities, "lane_topology",
        "zcl.agent_lanes.v1", "zclassic23 agentlanes",
        "zcl_agent_lanes", "",
        "canonical, soak, and dev lane topology with restart/deploy rules");
    agent_interface_push_capability(&capabilities, "semantic_state",
        "subsystem-specific zcl_state JSON", "zclassic23 dumpstate <subsystem>",
        "zcl_state", "",
        "generic subsystem state without adding bespoke tools");
    agent_interface_push_capability(&capabilities, "bounded_logs",
        "zcl.node_log.v1", "zclassic23 getnodelog <pattern>",
        "zcl_node_log", "",
        "server-side log search without shipping full node.log history");
    agent_interface_push_capability(&capabilities, "select_sql",
        "zcl.sql_result.v1", "zclassic23 dbquery <SELECT>",
        "zcl_sql", "",
        "bounded SELECT-only node.db inspection");
    agent_interface_push_capability(&capabilities, "changed_files_to_tests",
        "zcl.agent_impact.v1", "zclassic23 agentimpact <files...>",
        "zcl_agent_impact", "",
        "map edits to risk flags, docs, and focused validation");
    agent_interface_push_capability(&capabilities, "build_loop",
        "zcl.agent_build.v1", "zclassic23 agentbuild",
        "zcl_agent_build", "",
        "cache-aware compile/test/reproducibility contract");
    agent_interface_push_capability(&capabilities, "operator_command_center",
        "zcl.agent_ops.v1", "zclassic23 agentops",
        "zcl_agent_ops", "",
        "compact no-jq agent command center and next-work list");
    agent_interface_push_capability(&capabilities, "deploy_guard",
        "zcl.agent_deploy_guard.v1",
        "zclassic23 agentdeployguard <action>",
        "zcl_agent_deploy_guard", "",
        "machine allow/refuse decision before restart or deploy");
    json_push_kv(result, "capabilities", &capabilities);
    json_free(&capabilities);

    json_init(&machine);
    json_set_object(&machine);
    json_push_kv_str(&machine, "schema", "zcl.agent_machine_contract.v1");
    json_push_kv_str(&machine, "payload", "json_object");
    json_push_kv_bool(&machine, "schema_required", true);
    json_push_kv_bool(&machine, "api_version_required", true);
    json_push_kv_bool(&machine, "status_required", true);
    json_push_kv_bool(&machine, "append_only_v1", true);
    json_push_kv_bool(&machine, "transport_equivalent_payloads", true);
    json_push_kv_bool(&machine, "no_python_required", true);
    json_push_kv_bool(&machine, "no_tools_z_required", true);
    json_push_kv_str(&machine, "preferred_error_style",
                     "structured MCP error envelope or JSON object with schema/status/error");
    json_push_kv_str(&machine, "compatibility_rule",
                     "Agents may rely on existing field meanings; new optional fields are additive until v2.");
    json_push_kv(result, "machine_contract", &machine);
    json_free(&machine);

    json_init(&runtime);
    json_set_object(&runtime);
    json_push_kv_str(&runtime, "schema", "zcl.agent_runtime_identity.v1");
    json_push_kv_str(&runtime, "build_commit", zcl_build_commit());
    json_push_kv_str(&runtime, "binary", "zclassic23");
    json_push_kv_str(&runtime, "generated_by",
                     "app/controllers/src/agent_interface_controller.c");
    json_push_kv_str(&runtime, "identity_rule",
                     "Treat this as the runtime binary that produced the interface contract.");
    json_push_kv(result, "runtime_identity", &runtime);
    json_free(&runtime);

    json_init(&loop);
    json_set_object(&loop);
    json_push_kv_str(&loop, "discover", "zclassic23 agentinterface");
    json_push_kv_str(&loop, "status", "zcl_agent");
    json_push_kv_str(&loop, "mirror_status", "zcl_mirror_status");
    json_push_kv_str(&loop, "lane_topology", "zcl_agent_lanes");
    json_push_kv_str(&loop, "code_map", "zcl_agent_map");
    json_push_kv_str(&loop, "changed_files_to_tests", "zcl_agent_impact");
    json_push_kv_str(&loop, "build_contract", "zcl_agent_build");
    json_push_kv_str(&loop, "ops_command_center", "zcl_agent_ops");
    json_push_kv_str(&loop, "contract_registry", "zcl_agent_contracts");
    json_push_kv_str(&loop, "deploy_guard", "zcl_agent_deploy_guard");
    json_push_kv_str(&loop, "subsystem_state", "zcl_state");
    json_push_kv_str(&loop, "logs", "zcl_node_log");
    json_push_kv_str(&loop, "database", "zcl_sql");
    json_push_kv_str(&loop, "quality_lanes", "make quality-linger-status");
    json_push_kv(result, "development_loop", &loop);
    json_free(&loop);

    json_init(&native_owned);
    json_set_array(&native_owned);
    agent_interface_push_str(&native_owned, "schema/version emission");
    agent_interface_push_str(&native_owned, "status and blocker interpretation");
    agent_interface_push_str(&native_owned, "changed-file impact mapping");
    agent_interface_push_str(&native_owned, "deploy/restart safety decisions");
    agent_interface_push_str(&native_owned, "diagnostic drill-down routing");
    agent_interface_push_str(&native_owned,
                             "background quality lane contracts");
    json_push_kv(result, "must_live_in_c", &native_owned);
    json_free(&native_owned);

    json_init(&avoid);
    json_set_array(&avoid);
    agent_interface_push_str(&avoid,
                             "do not add new operator logic to tools/z");
    agent_interface_push_str(&avoid,
                             "do not require Python to parse agent API JSON");
    agent_interface_push_str(&avoid,
                             "do not scrape logs when zcl_node_log can answer");
    agent_interface_push_str(&avoid,
                             "do not scrape node.db when zcl_sql or a typed tool exists");
    agent_interface_push_str(&avoid,
                             "do not infer deploy safety from comments or unit names");
    json_push_kv(result, "avoid", &avoid);
    json_free(&avoid);

    json_init(&versioning);
    json_set_object(&versioning);
    json_push_kv_bool(&versioning, "schema_required", true);
    json_push_kv_bool(&versioning, "api_version_required", true);
    json_push_kv_str(&versioning, "compatibility_rule",
                     "Add fields without changing meaning; use v2 for breaking shape changes.");
    json_push_kv_str(&versioning, "test_floor",
                     "syncdiag_rpc, mcp_controllers, api, make_lint_gates");
    json_push_kv(result, "versioning", &versioning);
    json_free(&versioning);
    return true;
}

bool rpc_agent_deploy_guard(const struct json_value *params, bool help,
                            struct json_value *result)
{
    RPC_HELP(help, result,
        "agentdeployguard ( action )\n"
        "\nReturn a C-native allow/refuse decision for deploy/restart\n"
        "automation based on zcl.operator_deployment_safety.v1.\n"
        "\nActions: canonical-deploy, canonical-restart, deploy, restart.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_deploy_guard.v1\", "
        "\"allowed\":false, \"exit_code\":1, ... }\n");

    const char *action = agent_interface_param0_str(params,
                                                    "canonical-deploy");
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_deploy_guard.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "action", action);
    agent_push_operator_lane_json(result, "operator_lane");
    agent_push_operator_lane_fields_json(result);

    const struct json_value *lane = json_get(result, "operator_lane");
    const struct json_value *safety =
        lane ? json_get(lane, "deployment_safety") : NULL;
    const bool deploy_ok = safety &&
        json_get_bool(json_get(safety, "automation_deploy_ok"));
    const bool restart_ok = safety &&
        json_get_bool(json_get(safety, "automation_restart_ok"));
    const bool requires = safety &&
        json_get_bool(json_get(safety, "requires_operator_confirmation"));
    const char *lane_name = lane && json_get(lane, "lane")
        ? json_get_str(json_get(lane, "lane")) : "unknown";
    const bool known = agent_deploy_action_known(action);
    const bool wants_restart = agent_deploy_action_is_restart(action);
    const bool allowed = known && !requires &&
        (wants_restart ? restart_ok : deploy_ok);

    json_push_kv_bool(result, "allowed", allowed);
    json_push_kv_int(result, "exit_code", allowed ? 0 : 1);
    json_push_kv_str(result, "decision", allowed ? "allow" : "refuse");
    json_push_kv_str(result, "required_contract",
                     "zcl.operator_deployment_safety.v1");
    json_push_kv_str(result, "source", "native_c_agent_interface_controller");
    if (!known) {
        json_push_kv_str(result, "reason", "unknown_action");
    } else if (requires) {
        json_push_kv_str(result, "reason",
                         "operator_confirmation_required");
    } else if (wants_restart && !restart_ok) {
        json_push_kv_str(result, "reason", "automation_restart_not_allowed");
    } else if (!wants_restart && !deploy_ok) {
        json_push_kv_str(result, "reason", "automation_deploy_not_allowed");
    } else {
        json_push_kv_str(result, "reason", "deployment_safety_allows_action");
    }
    json_push_kv_str(result, "lane", lane_name);
    json_push_kv_str(result, "guard_env",
                     safety && json_get(safety, "guard_env")
                         ? json_get_str(json_get(safety, "guard_env")) : "");
    json_push_kv_str(result, "safe_default_action",
                     safety && json_get(safety, "safe_default_action")
                         ? json_get_str(json_get(safety,
                                                "safe_default_action")) : "");
    return true;
}
