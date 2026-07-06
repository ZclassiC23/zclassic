/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Versioned AI/operator API contract registry. */

#include "controllers/agent_controller.h"
#include "controllers/strong_params.h"

#include "json/json.h"

#include <stdio.h>

static void contracts_push_str(struct json_value *arr, const char *s)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, s ? s : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static void contracts_push_schema(struct json_value *arr, const char *schema,
                                  const char *producer, const char *purpose)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", schema);
    json_push_kv_str(&obj, "producer", producer);
    json_push_kv_str(&obj, "purpose", purpose);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static void contracts_push_registry_schema(struct json_value *arr,
                                           const char *method,
                                           const char *capability,
                                           const char *schema,
                                           const char *native,
                                           const char *mcp,
                                           const char *rest,
                                           const char *purpose)
{
    (void)method;
    (void)capability;
    char producer[320];
    if (rest && rest[0]) {
        snprintf(producer, sizeof(producer), "%s / %s / %s",
                 native ? native : "", mcp ? mcp : "", rest);
    } else {
        snprintf(producer, sizeof(producer), "%s / %s",
                 native ? native : "", mcp ? mcp : "");
    }
    contracts_push_schema(arr, schema, producer, purpose);
}

static void contracts_push_agent_registry_schemas(struct json_value *arr)
{
    for (size_t i = 0; i < agent_contract_count(); i++) {
        const struct agent_contract *c = agent_contract_at(i);
        if (!c)
            continue;
        contracts_push_registry_schema(arr, c->method, c->capability,
                                       c->schema, c->native_command,
                                       c->mcp_tool, c->rest_route,
                                       c->purpose);
    }
}

bool rpc_agent_contracts(const struct json_value *params, bool help,
                         struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agentcontracts\n"
        "\nReturn the versioned AI/operator API contracts and their native,\n"
        "MCP, and REST transport names.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_contracts.v1\", "
        "\"schemas\":[...] }\n");

    struct json_value contract_list, schemas, transports, rules;
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_contracts.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "canonical_interface",
                     "native zclassic23 agent* RPCs and MCP zcl_agent_* tools");

    json_init(&contract_list);
    json_set_array(&contract_list);
    agent_push_contracts_json(&contract_list);
    json_push_kv(result, "contracts", &contract_list);
    json_free(&contract_list);

    json_init(&schemas);
    json_set_array(&schemas);
    contracts_push_agent_registry_schemas(&schemas);
    contracts_push_schema(&schemas, "zcl.first_call_contract.v1",
                          "nested in zcl.public_status.v1, zcl.healthcheck.v1, zcl.agent_liveness.v1, and zcl.agent_diagnose.v1",
                          "first-call boundedness, elapsed time, partial-result, and budget semantics");
    contracts_push_schema(&schemas, "zcl.agent_readiness.v1",
                          "nested in zcl.public_status.v1 readiness",
                          "separates chain-serving readiness from index projection freshness");
    contracts_push_schema(&schemas, "zcl.height_contract.v1",
                          "nested in zcl.public_status.v1 height_contract",
                          "names served H*, active lookahead, header, peer, and target heights");
    contracts_push_schema(&schemas, "zcl.operator_latch.v1",
                          "nested in zcl.public_status.v1 operator_latch",
                          "names EV_OPERATOR_NEEDED latch detail, age, and whether operator action is still required");
    contracts_push_schema(&schemas, "zcl.condition_engine_summary.v1",
                          "nested in zcl.public_status.v1 conditions",
                          "cheap active/unresolved condition counts with drill-down routes");
    contracts_push_schema(&schemas, "zcl.runtime_build.v1",
                          "nested in zcl.public_status.v1 runtime_build",
                          "running-vs-deploy-expected build freshness for stale-runtime detection");
    contracts_push_schema(&schemas, "zcl.agent_runtime_availability.v1",
                          "nested in zcl.agent_interface.v1 and zcl.agent_ops.v1",
                          "producer-vs-target first-call method availability and method-not-found guardrail");
    contracts_push_schema(&schemas, "zcl.background_quality_runtime.v1",
                          "nested in zcl.agent_build.v1 background_quality_status",
                          "native status-file reader for background tests/fuzz/coverage verdicts");
    contracts_push_schema(&schemas, "zcl.agent_runtime_services.v1",
                          "nested in zcl.agent_lanes.v1",
                          "configured boot ports plus observed in-process listener state");
    contracts_push_schema(&schemas, "zcl.agent_capability.v1",
                          "nested in zcl.agent_interface.v1 capabilities[]",
                          "one machine-readable agent operation and its transports");
    contracts_push_schema(&schemas, "zcl.agent_machine_contract.v1",
                          "nested in zcl.agent_interface.v1 machine_contract",
                          "JSON/schema/version compatibility requirements for agents");
    contracts_push_schema(&schemas, "zcl.agent_runtime_identity.v1",
                          "nested in zcl.agent_interface.v1 runtime_identity",
                          "running binary identity for the interface contract producer");
    contracts_push_schema(&schemas, "zcl.operator_summary.v1",
                          "zcl_operator_summary",
                          "long MCP operator summary with raw drill-down");
    contracts_push_schema(&schemas, "zcl.operator_lane.v1",
                          "zclassic23 agent / GET /api/v1/agent",
                          "declared or exact-topology-inferred canonical/soak/dev lane and restart policy");
    contracts_push_schema(&schemas, "zcl.operator_deployment_safety.v1",
                          "nested in zcl.operator_lane.v1",
                          "machine-readable deploy/restart safety contract");
    contracts_push_schema(&schemas, "zcl.node_resources.v1",
                          "nested in zcl.public_status.v1 resources",
                          "cheap process RSS, uptime, and memory-pressure telemetry");
    contracts_push_schema(&schemas, "zcl.restart_watchdog.v1",
                          "nested in zcl.public_status.v1 restart_watchdog",
                          "chain tip watchdog restart budget and last autonomous recycle reason");
    json_push_kv(result, "schemas", &schemas);
    json_free(&schemas);

    json_init(&transports);
    json_set_array(&transports);
    agent_push_contract_transport_summary_json(&transports);
    json_push_kv(result, "transports", &transports);
    json_free(&transports);

    json_init(&rules);
    json_set_array(&rules);
    contracts_push_str(&rules, "Do not put new logic in shell wrappers.");
    contracts_push_str(&rules,
                       "Build JSON once in native services/controllers, then expose through MCP/REST.");
    contracts_push_str(&rules,
                       "Every new contract needs schema, docs, and focused tests.");
    contracts_push_str(&rules,
                       "No Python is required to consume the preferred agent API.");
    contracts_push_str(&rules,
                       "Automation must read deployment_safety before restarting a lane.");
    contracts_push_str(&rules,
                       "Consensus-risk paths require parity review and strict relevant tests.");
    json_push_kv(result, "design_rules", &rules);
    json_free(&rules);
    return true;
}
