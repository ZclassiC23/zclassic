/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/agent_controller.h"

#include "json/json.h"

struct agent_contract_schema_surface {
    int rank;
    const char *schema;
    const char *producer;
    const char *purpose;
};

static const struct agent_contract_schema_surface g_agent_schema_surfaces[] = {
    { 1, "zcl.first_call_contract.v1",
      "nested in zcl.public_status.v1, zcl.healthcheck.v1, zcl.agent_liveness.v1, and zcl.agent_diagnose.v1",
      "first-call boundedness, elapsed time, partial-result, and budget semantics" },
    { 2, "zcl.agent_readiness.v1",
      "nested in zcl.public_status.v1 readiness",
      "separates chain-serving readiness from index projection freshness" },
    { 3, "zcl.height_contract.v1",
      "nested in zcl.public_status.v1 height_contract",
      "names served H*, active lookahead, header, peer, and target heights" },
    { 4, "zcl.operator_latch.v1",
      "nested in zcl.public_status.v1 operator_latch",
      "names EV_OPERATOR_NEEDED latch detail, age, and whether operator action is still required" },
    { 5, "zcl.condition_engine_summary.v1",
      "nested in zcl.public_status.v1 conditions",
      "cheap active/unresolved condition counts with drill-down routes" },
    { 6, "zcl.runtime_build.v1",
      "nested in zcl.public_status.v1 runtime_build",
      "running-vs-deploy-expected build freshness for stale-runtime detection" },
    { 7, "zcl.agent_runtime_availability.v1",
      "nested in zcl.agent_interface.v1 and zcl.agent_ops.v1",
      "producer-vs-target first-call method availability and method-not-found guardrail" },
    { 8, "zcl.background_quality_runtime.v1",
      "nested in zcl.agent_build.v1 background_quality_status",
      "native status-file reader for background tests/fuzz/coverage verdicts" },
    { 9, "zcl.agent_runtime_services.v1",
      "nested in zcl.agent_lanes.v1",
      "configured boot ports plus observed in-process listener state" },
    { 10, "zcl.agent_capability.v1",
      "nested in zcl.agent_interface.v1 capabilities[]",
      "one machine-readable agent operation and its transports" },
    { 11, "zcl.agent_machine_contract.v1",
      "nested in zcl.agent_interface.v1 machine_contract",
      "JSON/schema/version compatibility requirements for agents" },
    { 12, "zcl.agent_runtime_identity.v1",
      "nested in zcl.agent_interface.v1 runtime_identity",
      "running binary identity for the interface contract producer" },
    { 13, "zcl.operator_summary.v1",
      "zcl_operator_summary",
      "long MCP operator summary with raw drill-down" },
    { 14, "zcl.operator_lane.v1",
      "zclassic23 agent / GET /api/v1/agent",
      "declared or exact-topology-inferred canonical/soak/dev lane and restart policy" },
    { 15, "zcl.operator_deployment_safety.v1",
      "nested in zcl.operator_lane.v1",
      "machine-readable deploy/restart safety contract" },
    { 16, "zcl.node_resources.v1",
      "nested in zcl.public_status.v1 resources",
      "cheap process RSS, uptime, and memory-pressure telemetry" },
    { 17, "zcl.restart_watchdog.v1",
      "nested in zcl.public_status.v1 restart_watchdog",
      "chain tip watchdog restart budget and last autonomous recycle reason" },
    { 18, "zcl.security_posture.v1",
      "nested in zcl.public_status.v1 security_posture",
      "bounded bootstrap trust and shielded-nullifier history completeness posture" },
    { 19, "zcl.service_catalog.v1",
      "zclassic23 servicecatalog / GET /api/v1/service-catalog / zcl_service_catalog",
      "UX-oriented sovereign service catalog with transport, CRUD, verification, and trust boundaries" },
};

static const size_t g_agent_schema_surface_count =
    sizeof(g_agent_schema_surfaces) / sizeof(g_agent_schema_surfaces[0]);

size_t agent_contract_schema_surface_count(void)
{
    return g_agent_schema_surface_count;
}

static void agent_push_schema_surface_entry_json(
    struct json_value *arr, const struct agent_contract_schema_surface *entry)
{
    if (!arr || !entry)
        return;

    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", entry->schema);
    json_push_kv_str(&obj, "producer", entry->producer);
    json_push_kv_str(&obj, "purpose", entry->purpose);
    json_push_back(arr, &obj);
    json_free(&obj);
}

size_t agent_push_contract_schema_surface_json(struct json_value *arr)
{
    if (!arr)
        return 0;

    int max_rank = 0;
    for (size_t i = 0; i < g_agent_schema_surface_count; i++) {
        if (g_agent_schema_surfaces[i].rank > max_rank)
            max_rank = g_agent_schema_surfaces[i].rank;
    }

    size_t emitted = 0;
    for (int rank = 1; rank <= max_rank; rank++) {
        for (size_t i = 0; i < g_agent_schema_surface_count; i++) {
            const struct agent_contract_schema_surface *entry =
                &g_agent_schema_surfaces[i];
            if (entry->rank == rank) {
                agent_push_schema_surface_entry_json(arr, entry);
                emitted++;
            }
        }
    }
    return emitted;
}
