/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Bounded diagnosis contract for AI/operator first calls. This composes
 * already-bounded C-owned views and marks skipped lower-priority sections
 * explicitly instead of blocking on a heavy drill-down. */

#include "controllers/agent_controller.h"
#include "controllers/agent_first_call.h"
#include "controllers/diagnostics_internal.h"
#include "controllers/event_healthcheck_controller.h"
#include "controllers/event_timeline_controller.h"
#include "controllers/strong_params.h"

#include "event_agent_summary.h"

#include "json/json.h"
#include "net/peer_lifecycle.h"
#include "rpc/server.h"
#include "util/clientversion.h"

#include <stdint.h>
#include <string.h>

static void diagnose_push_str(struct json_value *arr, const char *s)
{
    struct json_value v = {0};
    json_set_str(&v, s ? s : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static void diagnose_push_finding(struct json_value *arr,
                                  const char *name,
                                  const char *severity,
                                  const char *detail,
                                  const char *next_action)
{
    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_str(&obj, "name", name ? name : "");
    json_push_kv_str(&obj, "severity", severity ? severity : "info");
    json_push_kv_str(&obj, "detail", detail ? detail : "");
    json_push_kv_str(&obj, "next_action", next_action ? next_action : "");
    json_push_back(arr, &obj);
    json_free(&obj);
}

static void diagnose_push_skipped(struct json_value *out, const char *key,
                                  const char *reason)
{
    struct json_value skipped = {0};
    json_set_object(&skipped);
    json_push_kv_str(&skipped, "status", "skipped");
    json_push_kv_bool(&skipped, "partial_result", true);
    json_push_kv_str(&skipped, "partial_reason", reason ? reason : "");
    json_push_kv(out, key, &skipped);
    json_free(&skipped);
}

static void diagnose_timeline_params(struct json_value *params)
{
    struct json_value category = {0};
    struct json_value count = {0};

    json_set_array(params);
    json_set_str(&category, "all");
    json_push_back(params, &category);
    json_free(&category);
    json_set_int(&count, 12);
    json_push_back(params, &count);
    json_free(&count);
}

static bool diagnose_agent_chain_ready(const struct json_value *agent)
{
    const struct json_value *readiness = agent ? json_get(agent, "readiness")
                                               : NULL;
    if (readiness)
        return json_get_bool(json_get(readiness, "chain_serving_ready"));
    return agent && json_get_bool(json_get(agent, "chain_serving_ready"));
}

static bool diagnose_agent_normal_lookahead(const struct json_value *agent)
{
    const struct json_value *height_contract =
        agent ? json_get(agent, "height_contract") : NULL;
    return height_contract &&
           json_get_bool(json_get(height_contract, "normal_lookahead"));
}

static bool diagnose_peer_incident_material(const struct json_value *incident)
{
    if (!incident || incident->type != JSON_OBJ)
        return false;
    if (json_get_int(json_get(incident, "duplicate_host_entries")) > 1)
        return true;
    if (json_get_int(json_get(incident, "reconnects")) > 0)
        return true;
    if (json_get_int(json_get(incident, "timeout")) > 0)
        return true;
    if (json_get_int(json_get(incident, "rejected")) > 0)
        return true;
    if (json_get_int(json_get(incident, "pre_handshake_disconnects")) > 1)
        return true;
    return false;
}

static int64_t diagnose_peer_material_incidents(const struct json_value *peers)
{
    const struct json_value *top =
        peers ? json_get(peers, "top_incidents") : NULL;
    if (!top || top->type != JSON_ARR)
        return 0;
    int64_t material = 0;
    for (size_t i = 0; i < json_size(top); i++) {
        if (diagnose_peer_incident_material(json_at(top, i)))
            material++;
    }
    return material;
}

static bool diagnose_peer_group_material(const struct json_value *group)
{
    if (!group || group->type != JSON_OBJ)
        return false;
    if (json_get_int(json_get(group, "entries")) > 1)
        return true;
    if (json_get_int(json_get(group, "reconnects")) > 0)
        return true;
    if (json_get_int(json_get(group, "timeout")) > 0)
        return true;
    if (json_get_int(json_get(group, "rejected")) > 0)
        return true;
    if (json_get_int(json_get(group, "pre_handshake_disconnects")) > 1)
        return true;
    return false;
}

static int64_t diagnose_peer_material_groups(const struct json_value *peers)
{
    const struct json_value *groups =
        peers ? json_get(peers, "duplicate_host_groups") : NULL;
    if (!groups || groups->type != JSON_ARR)
        return 0;
    int64_t material = 0;
    for (size_t i = 0; i < json_size(groups); i++) {
        if (diagnose_peer_group_material(json_at(groups, i)))
            material++;
    }
    return material;
}

static const char *diagnose_peer_detail(int64_t peer_count,
                                        int64_t peer_incidents,
                                        int64_t material_signals)
{
    if (peer_count <= 0)
        return "no connected peers";
    if (material_signals > 0)
        return "peer lifecycle has material reconnect, duplicate, timeout, or reject incidents";
    if (peer_incidents > 0)
        return "minor peer lifecycle incidents only; no duplicate or reconnect storm";
    return "peer lifecycle has no scored incidents";
}

static const char *diagnose_peer_next_action(int64_t peer_count,
                                             int64_t peer_incidents,
                                             int64_t material_signals)
{
    if (peer_count <= 0)
        return "inspect peer lifecycle and bootstrap status";
    if (material_signals > 0)
        return "dumpstate peer_lifecycle incidents";
    if (peer_incidents > 0)
        return "monitor peer lifecycle incidents";
    return "monitor getnetworkinfo peer_lifecycle";
}

static const char *diagnose_next_action(bool operator_needed,
                                        bool agent_healthy,
                                        bool chain_attention,
                                        int64_t peer_count,
                                        bool peer_attention,
                                        bool mirror_unhealthy)
{
    if (operator_needed)
        return "inspect_condition_engine_and_operator_latch";
    if (!agent_healthy || chain_attention)
        return "inspect_agent_healthcheck_and_chain_timeline";
    if (peer_count <= 0)
        return "inspect_peer_lifecycle_incidents_and_bootstrapstatus";
    if (peer_attention)
        return "inspect_peer_lifecycle_incidents";
    if (mirror_unhealthy)
        return "inspect_mirror_status";
    return "monitor_agent_and_liveness";
}

bool rpc_agent_diagnose(const struct json_value *params, bool help,
                        struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agentdiagnose\n"
        "\nReturn a bounded no-jq diagnosis packet for AI/operators. It "
        "composes cheap status, healthcheck, peer incident, mirror, and "
        "timeline views without requiring a client-side pipeline.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_diagnose.v1\", "
        "\"verdict\":\"healthy|attention_needed\", "
        "\"findings\":[...] }\n");

    int64_t started_us = agent_first_call_start_us();
    const struct agent_contract *contract =
        agent_contract_lookup("agentdiagnose");
    struct json_value empty_params = {0};
    struct json_value agent = {0};
    struct json_value health = {0};
    struct json_value peers = {0};
    struct json_value mirror = {0};
    struct json_value timeline_params = {0};
    struct json_value timeline = {0};
    struct json_value findings = {0};
    struct json_value commands = {0};
    bool partial = false;
    bool mirror_present = false;
    bool timeline_present = false;

    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_diagnose.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_bool(result, "no_jq_required", true);
    json_push_kv_str(result, "native_command",
                     contract ? contract->native_command
                              : "zclassic23 agentdiagnose");
    json_push_kv_str(result, "mcp_tool",
                     contract ? contract->mcp_tool
                              : "zcl_agent_diagnose");
    json_set_array(&empty_params);

    bool agent_ok = rpc_agent_summary(&empty_params, false, &agent);
    bool health_ok = true;
    if (agent_first_call_budget_exceeded(
            started_us, ZCL_AGENT_FIRST_CALL_BUDGET_DIAGNOSE_MS)) {
        partial = true;
        diagnose_push_skipped(result, "healthcheck",
                              "first_call_budget_exhausted_before_healthcheck");
        health_ok = false;
    } else {
        health_ok = rpc_healthcheck(&empty_params, false, &health);
    }

    peer_lifecycle_incidents_json(&peers);

    if (agent_first_call_budget_exceeded(
            started_us, ZCL_AGENT_FIRST_CALL_BUDGET_DIAGNOSE_MS)) {
        partial = true;
    } else {
        mirror_present =
            diag_rpc_getmirrorstatus(&empty_params, false, &mirror);
    }

    if (agent_first_call_budget_exceeded(
            started_us, ZCL_AGENT_FIRST_CALL_BUDGET_DIAGNOSE_MS)) {
        partial = true;
    } else {
        diagnose_timeline_params(&timeline_params);
        timeline_present = rpc_timeline(&timeline_params, false, &timeline);
    }

    bool agent_healthy = agent_ok && json_get_bool(json_get(&agent, "healthy"));
    bool serving = agent_ok && json_get_bool(json_get(&agent, "serving"));
    bool operator_needed =
        agent_ok && json_get_bool(json_get(&agent, "operator_needed"));
    bool chain_serving_ready = agent_ok && diagnose_agent_chain_ready(&agent);
    bool normal_lookahead = agent_ok && diagnose_agent_normal_lookahead(&agent);
    int64_t gap = agent_ok ? json_get_int(json_get(&agent, "gap")) : -1;
    const struct json_value *agent_peers =
        agent_ok ? json_get(&agent, "peers") : NULL;
    int64_t peer_count =
        agent_peers ? json_get_int(json_get(agent_peers, "total")) : -1;
    int64_t peer_incidents =
        json_get_int(json_get(&peers, "incident_count"));
    int64_t duplicate_groups =
        json_get_int(json_get(&peers, "duplicate_host_group_count"));
    int64_t material_peer_incidents =
        diagnose_peer_material_incidents(&peers);
    int64_t material_peer_groups =
        diagnose_peer_material_groups(&peers);
    int64_t informational_peer_incidents =
        peer_incidents > material_peer_incidents
            ? peer_incidents - material_peer_incidents : 0;
    bool peer_attention =
        peer_count <= 0 || material_peer_incidents > 0 ||
        material_peer_groups > 0 || duplicate_groups > 0;
    const char *peer_severity =
        peer_attention ? "attention" :
        (peer_incidents > 0 ? "info" : "ok");
    int64_t material_peer_signals =
        material_peer_incidents + material_peer_groups;
    const struct json_value *mirror_contract =
        mirror_present ? json_get(&mirror, "mirror_contract") : NULL;
    const char *mirror_status =
        mirror_contract ? json_get_str(json_get(mirror_contract, "status"))
                        : "";
    bool mirror_unhealthy =
        mirror_present && mirror_status && mirror_status[0] &&
        strcmp(mirror_status, "healthy") != 0;

    json_push_kv_bool(result, "healthy", agent_healthy);
    json_push_kv_bool(result, "serving", serving);
    json_push_kv_bool(result, "operator_needed", operator_needed);
    json_push_kv_bool(result, "chain_serving_ready", chain_serving_ready);
    json_push_kv_bool(result, "normal_lookahead", normal_lookahead);
    json_push_kv_int(result, "gap", gap);
    json_push_kv_int(result, "peer_count", peer_count);
    json_push_kv_int(result, "peer_incident_count", peer_incidents);
    json_push_kv_int(result, "duplicate_host_group_count", duplicate_groups);
    json_push_kv_str(result, "peer_incident_severity", peer_severity);
    json_push_kv_bool(result, "peer_stability_blocker", peer_attention);
    json_push_kv_int(result, "peer_material_incident_count",
                     material_peer_incidents);
    json_push_kv_int(result, "peer_material_group_count",
                     material_peer_groups);
    json_push_kv_int(result, "peer_informational_incident_count",
                     informational_peer_incidents);
    json_push_kv_str(result, "peer_incident_summary",
                     diagnose_peer_detail(peer_count, peer_incidents,
                                          material_peer_signals));
    json_push_kv_bool(result, "partial_result", partial);

    bool chain_attention =
        !chain_serving_ready || (!normal_lookahead && gap > 0);
    const char *next_action =
        diagnose_next_action(operator_needed, agent_healthy, chain_attention,
                             peer_count, peer_attention, mirror_unhealthy);
    bool attention = operator_needed || !agent_healthy || chain_attention ||
                     peer_attention || mirror_unhealthy;
    json_push_kv_str(result, "verdict",
                     attention ? "attention_needed" : "healthy");
    json_push_kv_str(result, "safe_next_action", next_action);

    json_set_array(&findings);
    diagnose_push_finding(&findings, "chain_serving",
                          agent_healthy && serving && !chain_attention
                              ? "ok" : "attention",
                          agent_ok ? json_get_str(json_get(&agent, "summary"))
                                   : "agent status unavailable",
                          chain_attention
                              ? "inspect sync timeline and reducer frontier"
                              : "continue monitoring H* and peer target");
    diagnose_push_finding(&findings, "peer_lifecycle",
                          peer_severity,
                          diagnose_peer_detail(peer_count, peer_incidents,
                                               material_peer_signals),
                          diagnose_peer_next_action(peer_count,
                                                    peer_incidents,
                                                    material_peer_signals));
    diagnose_push_finding(&findings, "mirror",
                          mirror_unhealthy ? "attention" : "ok",
                          mirror_present ? mirror_status
                                         : "mirror status skipped or unavailable",
                          mirror_unhealthy ? "inspect getmirrorstatus"
                                           : "mirror is advisory only");
    json_push_kv(result, "findings", &findings);
    json_free(&findings);

    json_set_array(&commands);
    diagnose_push_str(&commands, "zclassic23 agent");
    diagnose_push_str(&commands, "zclassic23 agentliveness");
    diagnose_push_str(&commands,
                      "zclassic23 dumpstate peer_lifecycle incidents");
    diagnose_push_str(&commands,
                      "zclassic23 timeline '{\"category\":\"sync\",\"count\":50,\"since_secs\":3600}'");
    diagnose_push_str(&commands, "zclassic23 healthcheck full");
    json_push_kv(result, "recommended_commands", &commands);
    json_free(&commands);

    if (agent_ok)
        json_push_kv(result, "agent", &agent);
    else
        diagnose_push_skipped(result, "agent", "agent_status_unavailable");
    if (health_ok)
        json_push_kv(result, "healthcheck", &health);
    if (mirror_present)
        json_push_kv(result, "mirror", &mirror);
    else if (!json_get(result, "mirror"))
        diagnose_push_skipped(result, "mirror",
                              "first_call_budget_exhausted_before_mirror");
    json_push_kv(result, "peer_incidents", &peers);
    if (timeline_present)
        json_push_kv(result, "timeline", &timeline);
    else
        diagnose_push_skipped(result, "timeline",
                              "first_call_budget_exhausted_before_timeline");

    agent_push_first_call_simple_json(
        result, "first_call", "agentdiagnose",
        "bounded_status_health_peer_mirror_timeline",
        ZCL_AGENT_FIRST_CALL_BUDGET_DIAGNOSE_MS, started_us, true,
        partial ? "lower_priority_sections_skipped_by_budget"
                : "bounded_diagnosis_not_full_forensics",
        "zclassic23 healthcheck full");

    json_free(&timeline);
    json_free(&timeline_params);
    json_free(&mirror);
    json_free(&peers);
    json_free(&health);
    json_free(&agent);
    json_free(&empty_params);
    return true;
}
