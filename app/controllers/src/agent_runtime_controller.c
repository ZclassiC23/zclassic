/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Runtime boot context exported through the versioned agent/operator APIs.
 * Keep this state narrow: it describes how this binary instance is meant to be
 * operated, not consensus, wallet, or peer state. */

#include "controllers/agent_controller.h"

#include "json/json.h"
#include "net/file_service.h"
#include "net/https_server.h"
#include "rpc/httpserver.h"
#include "util/clientversion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_AVAILABILITY_SUPPORT_SUPPORTED "supported"
#define AGENT_AVAILABILITY_SUPPORT_UNSUPPORTED "unsupported_method_not_found"
#define AGENT_AVAILABILITY_SUPPORT_PRESENT_ERROR "present_error"
#define AGENT_AVAILABILITY_SUPPORT_UNKNOWN "unknown"

struct agent_runtime_context {
    char operator_lane[32];
    char runtime_profile[32];
    char datadir[1024];
    int rpc_port;
    int p2p_port;
    int https_port;
    int fs_port;
};

static struct agent_runtime_context g_agent_runtime = {
    .operator_lane = "unknown",
    .runtime_profile = "unknown",
    .datadir = "",
    .rpc_port = 0,
    .p2p_port = 0,
    .https_port = 0,
    .fs_port = 0,
};

struct agent_runtime_probe_method {
    const char *method;
    const char *capability;
    const char *native_command;
    const char *mcp_tool;
    const char *schema;
};

static const struct agent_runtime_probe_method g_probe_methods[] = {
    { "agent", "runtime_status", "zclassic23 agent", "zcl_agent",
      "zcl.public_status.v1" },
    { "agentops", "operator_command_center", "zclassic23 agentops",
      "zcl_agent_ops", "zcl.agent_ops.v1" },
    { "agentinterface", "discover_interface",
      "zclassic23 agentinterface", "zcl_agent_interface",
      "zcl.agent_interface.v1" },
    { "agentcontracts", "contract_registry", "zclassic23 agentcontracts",
      "zcl_agent_contracts", "zcl.agent_contracts.v1" },
    { "agentbuild", "build_loop", "zclassic23 agentbuild",
      "zcl_agent_build", "zcl.agent_build.v1" },
    { "agentmap", "code_map", "zclassic23 agentmap", "zcl_agent_map",
      "zcl.agent_map.v1" },
    { "agentlanes", "lane_topology", "zclassic23 agentlanes",
      "zcl_agent_lanes", "zcl.agent_lanes.v1" },
    { "agentimpact", "changed_files_to_tests",
      "zclassic23 agentimpact <files...>", "zcl_agent_impact",
      "zcl.agent_impact.v1" },
    { "agentdeployguard", "deploy_guard",
      "zclassic23 agentdeployguard <action>", "zcl_agent_deploy_guard",
      "zcl.agent_deploy_guard.v1" },
    { "statecatalog", "state_catalog", "zclassic23 statecatalog",
      "zcl_state_catalog", "zcl.state_catalog.v1" },
    { "timeline", "semantic_timeline",
      "zclassic23 timeline <category> <count>", "zcl_timeline",
      "zcl.timeline.v1" },
    { "getmirrorstatus", "mirror_status", "zclassic23 getmirrorstatus",
      "zcl_mirror_status", "zcl.mirror_status.v1" },
    { "api", "api_index", "zclassic23 api", "zcl_openapi",
      "zcl.api_index.v1" },
};

struct agent_runtime_method_probe {
    char method[48];
    char support[48];
    int64_t rpc_error_code;
    char error_message[192];
    bool recorded;
};

struct agent_runtime_availability_state {
    bool probe_started;
    char source[64];
    char datadir[1024];
    int rpc_port;
    char probe_status[64];
    char target_build_commit[128];
    struct agent_runtime_method_probe methods[
        sizeof(g_probe_methods) / sizeof(g_probe_methods[0])];
};

static struct agent_runtime_availability_state g_agent_availability = {
    .probe_started = false,
    .source = "producer_runtime",
    .datadir = "",
    .rpc_port = 0,
    .probe_status = "self_declared_current_runtime",
    .target_build_commit = "",
};

static const size_t g_probe_method_count =
    sizeof(g_probe_methods) / sizeof(g_probe_methods[0]);

static bool agent_lane_is(const char *lane, const char *want)
{
    return lane && want && strcmp(lane, want) == 0;
}

static const char *agent_lane_restart_policy(const char *lane)
{
    if (agent_lane_is(lane, "canonical"))
        return "operator_gated";
    if (agent_lane_is(lane, "soak"))
        return "restart_rebaselines_soak";
    if (agent_lane_is(lane, "dev"))
        return "frequent_deploy_ok";
    if (agent_lane_is(lane, "test") || agent_lane_is(lane, "copy"))
        return "ephemeral";
    return "unspecified";
}

static const char *agent_lane_safety_contract(const char *lane)
{
    if (agent_lane_is(lane, "canonical"))
        return "protect_long_running_public_node";
    if (agent_lane_is(lane, "soak"))
        return "preserve_clean_soak_window";
    if (agent_lane_is(lane, "dev"))
        return "exercise_fresh_binary_without_touching_canonical";
    if (agent_lane_is(lane, "test"))
        return "isolated_test_node";
    if (agent_lane_is(lane, "copy"))
        return "copy_proof_only_never_live_datadir";
    return "lane_not_declared";
}

static bool agent_lane_automation_restart_ok(const char *lane)
{
    return agent_lane_is(lane, "dev") ||
           agent_lane_is(lane, "test") ||
           agent_lane_is(lane, "copy");
}

static bool agent_lane_automation_deploy_ok(const char *lane)
{
    return agent_lane_is(lane, "dev") ||
           agent_lane_is(lane, "test");
}

static bool agent_lane_requires_operator_confirmation(const char *lane)
{
    return agent_lane_is(lane, "canonical") ||
           agent_lane_is(lane, "soak") ||
           agent_lane_is(lane, "unknown");
}

static bool agent_lane_is_isolated_from_canonical(const char *lane)
{
    return agent_lane_is(lane, "soak") ||
           agent_lane_is(lane, "dev") ||
           agent_lane_is(lane, "test") ||
           agent_lane_is(lane, "copy");
}

static const char *agent_lane_preferred_deploy_target(const char *lane)
{
    if (agent_lane_is(lane, "canonical") || agent_lane_is(lane, "soak"))
        return "dev";
    if (agent_lane_is(lane, "dev"))
        return "dev";
    if (agent_lane_is(lane, "test"))
        return "test";
    if (agent_lane_is(lane, "copy"))
        return "copy_fixture";
    return "declare_lane_first";
}

static const char *agent_lane_guard_env(const char *lane)
{
    if (agent_lane_is(lane, "canonical"))
        return "ZCL_DEPLOY_ALLOW_CANONICAL";
    if (agent_lane_is(lane, "soak"))
        return "ZCL_DEPLOY_ALLOW_SOAK";
    if (agent_lane_is(lane, "unknown"))
        return "ZCL_OPERATOR_LANE";
    return "";
}

static const char *agent_lane_safe_default_action(const char *lane)
{
    if (agent_lane_is(lane, "canonical"))
        return "observe_only_or_use_dev_lane";
    if (agent_lane_is(lane, "soak"))
        return "preserve_soak_window";
    if (agent_lane_is(lane, "dev"))
        return "deploy_dev_lane";
    if (agent_lane_is(lane, "test"))
        return "run_test_fixture";
    if (agent_lane_is(lane, "copy"))
        return "prove_on_copy";
    return "refuse_automation_until_lane_declared";
}

void rpc_agent_set_boot_context(const char *operator_lane,
                                const char *runtime_profile,
                                const char *datadir,
                                int rpc_port, int p2p_port,
                                int https_port, int fs_port)
{
    snprintf(g_agent_runtime.operator_lane,
             sizeof(g_agent_runtime.operator_lane), "%s",
             operator_lane && operator_lane[0] ? operator_lane : "unknown");
    snprintf(g_agent_runtime.runtime_profile,
             sizeof(g_agent_runtime.runtime_profile), "%s",
             runtime_profile && runtime_profile[0] ? runtime_profile
                                                   : "unknown");
    snprintf(g_agent_runtime.datadir, sizeof(g_agent_runtime.datadir), "%s",
             datadir ? datadir : "");
    g_agent_runtime.rpc_port = rpc_port;
    g_agent_runtime.p2p_port = p2p_port;
    g_agent_runtime.https_port = https_port;
    g_agent_runtime.fs_port = fs_port;
}

static size_t agent_runtime_method_index(const char *method)
{
    if (!method || !method[0])
        return g_probe_method_count;
    for (size_t i = 0; i < g_probe_method_count; i++) {
        if (strcmp(g_probe_methods[i].method, method) == 0)
            return i;
    }
    return g_probe_method_count;
}

size_t agent_runtime_probe_method_count(void)
{
    return g_probe_method_count;
}

const char *agent_runtime_probe_method_name(size_t index)
{
    if (index >= g_probe_method_count)
        return "";
    return g_probe_methods[index].method;
}

void agent_runtime_availability_reset(void)
{
    memset(&g_agent_availability, 0, sizeof(g_agent_availability));
    snprintf(g_agent_availability.source,
             sizeof(g_agent_availability.source), "%s",
             "producer_runtime");
    snprintf(g_agent_availability.probe_status,
             sizeof(g_agent_availability.probe_status), "%s",
             "self_declared_current_runtime");
}

void agent_runtime_availability_begin_probe(const char *source,
                                            const char *datadir,
                                            int rpc_port,
                                            const char *status)
{
    agent_runtime_availability_reset();
    g_agent_availability.probe_started = true;
    snprintf(g_agent_availability.source,
             sizeof(g_agent_availability.source), "%s",
             source && source[0] ? source : "target_rpc_probe");
    snprintf(g_agent_availability.datadir,
             sizeof(g_agent_availability.datadir), "%s",
             datadir ? datadir : "");
    g_agent_availability.rpc_port = rpc_port;
    snprintf(g_agent_availability.probe_status,
             sizeof(g_agent_availability.probe_status), "%s",
             status && status[0] ? status : "started");
}

void agent_runtime_availability_set_probe_status(const char *status)
{
    snprintf(g_agent_availability.probe_status,
             sizeof(g_agent_availability.probe_status), "%s",
             status && status[0] ? status : "unknown");
}

void agent_runtime_availability_record_method(const char *method,
                                              const char *support,
                                              int64_t rpc_error_code,
                                              const char *error_message)
{
    size_t idx = agent_runtime_method_index(method);
    if (idx >= g_probe_method_count)
        return;
    struct agent_runtime_method_probe *m =
        &g_agent_availability.methods[idx];
    snprintf(m->method, sizeof(m->method), "%s", g_probe_methods[idx].method);
    snprintf(m->support, sizeof(m->support), "%s",
             support && support[0] ? support
                                   : AGENT_AVAILABILITY_SUPPORT_UNKNOWN);
    m->rpc_error_code = rpc_error_code;
    snprintf(m->error_message, sizeof(m->error_message), "%s",
             error_message ? error_message : "");
    m->recorded = true;
}

void agent_runtime_availability_set_target_build_commit(
    const char *build_commit)
{
    if (!build_commit || !build_commit[0])
        return;
    snprintf(g_agent_availability.target_build_commit,
             sizeof(g_agent_availability.target_build_commit), "%s",
             build_commit);
}

static bool agent_availability_probe_attempted(void)
{
    if (!g_agent_availability.probe_started)
        return false;
    return strcmp(g_agent_availability.probe_status, "no_cookie") != 0;
}

static bool agent_availability_probe_reachable(void)
{
    for (size_t i = 0; i < g_probe_method_count; i++) {
        if (g_agent_availability.methods[i].recorded)
            return true;
    }
    return false;
}

static const char *agent_availability_method_support(size_t idx)
{
    if (idx >= g_probe_method_count)
        return AGENT_AVAILABILITY_SUPPORT_UNKNOWN;
    if (!g_agent_availability.probe_started)
        return AGENT_AVAILABILITY_SUPPORT_SUPPORTED;
    if (!agent_availability_probe_reachable())
        return AGENT_AVAILABILITY_SUPPORT_UNKNOWN;
    const struct agent_runtime_method_probe *m =
        &g_agent_availability.methods[idx];
    if (!m->recorded)
        return AGENT_AVAILABILITY_SUPPORT_UNKNOWN;
    return m->support[0] ? m->support
                         : AGENT_AVAILABILITY_SUPPORT_UNKNOWN;
}

static bool agent_availability_method_present(const char *support)
{
    return strcmp(support, AGENT_AVAILABILITY_SUPPORT_SUPPORTED) == 0 ||
           strcmp(support, AGENT_AVAILABILITY_SUPPORT_PRESENT_ERROR) == 0;
}

static bool agent_availability_method_safe_to_call(const char *support)
{
    return strcmp(support, AGENT_AVAILABILITY_SUPPORT_SUPPORTED) == 0;
}

static const char *agent_availability_scope(void)
{
    return g_agent_availability.probe_started ? "target_rpc_probe"
                                              : "producer_runtime";
}

static const char *agent_availability_build_relation(void)
{
    if (!g_agent_availability.probe_started)
        return "producer_runtime";
    if (!g_agent_availability.target_build_commit[0])
        return "unknown";
    return strcmp(zcl_build_commit(),
                  g_agent_availability.target_build_commit) == 0
        ? "same" : "different";
}

static const char *agent_availability_next_action(int64_t unsupported_count,
                                                  int64_t error_count)
{
    if (!g_agent_availability.probe_started)
        return "producer runtime supports these methods; probe the target lane before assuming target availability";
    if (strcmp(g_agent_availability.probe_status, "no_cookie") == 0)
        return "target runtime was not probed because no cookie was available; pass -datadir/-rpcport or start the lane";
    if (strcmp(g_agent_availability.probe_status, "connect_failed") == 0)
        return "target runtime RPC was not reachable; start or inspect the lane before calling agent methods";
    if (unsupported_count > 0)
        return "do not call unsupported methods on this target lane; deploy/smoke dev first or use methods marked supported";
    if (error_count > 0)
        return "target methods exist but some probe calls returned errors; use each method contract before automation";
    if (strcmp(agent_availability_build_relation(), "different") == 0)
        return "target runtime build differs from producer; rely on target_runtime_support before calling methods";
    return "target runtime supports the probed first-call agent methods";
}

void agent_push_runtime_availability_json(struct json_value *out,
                                          const char *key)
{
    if (!out)
        return;

    struct json_value obj, methods;
    int64_t supported_count = 0;
    int64_t unsupported_count = 0;
    int64_t error_count = 0;
    int64_t unknown_count = 0;

    for (size_t i = 0; i < g_probe_method_count; i++) {
        const char *support = agent_availability_method_support(i);
        if (strcmp(support, AGENT_AVAILABILITY_SUPPORT_SUPPORTED) == 0)
            supported_count++;
        else if (strcmp(support, AGENT_AVAILABILITY_SUPPORT_UNSUPPORTED) == 0)
            unsupported_count++;
        else if (strcmp(support,
                        AGENT_AVAILABILITY_SUPPORT_PRESENT_ERROR) == 0)
            error_count++;
        else
            unknown_count++;
    }

    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema",
                     "zcl.agent_runtime_availability.v1");
    json_push_kv_int(&obj, "schema_version", 1);
    json_push_kv_str(&obj, "producer_build_commit", zcl_build_commit());
    json_push_kv_str(&obj, "operator_lane_name",
                     g_agent_runtime.operator_lane[0]
                         ? g_agent_runtime.operator_lane : "unknown");
    json_push_kv_str(&obj, "producer_datadir", g_agent_runtime.datadir);
    json_push_kv_int(&obj, "producer_rpcport", g_agent_runtime.rpc_port);
    json_push_kv_str(&obj, "availability_scope",
                     agent_availability_scope());
    json_push_kv_str(&obj, "probe_source", g_agent_availability.source);
    json_push_kv_str(&obj, "probe_status",
                     g_agent_availability.probe_status);
    json_push_kv_bool(&obj, "target_rpc_attempted",
                      agent_availability_probe_attempted());
    json_push_kv_bool(&obj, "target_rpc_reachable",
                      agent_availability_probe_reachable());
    json_push_kv_str(&obj, "target_datadir",
                     g_agent_availability.datadir);
    json_push_kv_int(&obj, "target_rpcport",
                     g_agent_availability.rpc_port);
    json_push_kv_str(&obj, "target_build_commit",
                     g_agent_availability.target_build_commit);
    json_push_kv_str(&obj, "producer_target_build_relation",
                     agent_availability_build_relation());
    json_push_kv_int(&obj, "supported_count", supported_count);
    json_push_kv_int(&obj, "unsupported_count", unsupported_count);
    json_push_kv_int(&obj, "error_count", error_count);
    json_push_kv_int(&obj, "unknown_count", unknown_count);
    json_push_kv_str(&obj, "safe_next_action",
                     agent_availability_next_action(unsupported_count,
                                                    error_count));

    json_init(&methods);
    json_set_array(&methods);
    for (size_t i = 0; i < g_probe_method_count; i++) {
        const struct agent_runtime_probe_method *pm = &g_probe_methods[i];
        const struct agent_runtime_method_probe *probe =
            &g_agent_availability.methods[i];
        const char *support = agent_availability_method_support(i);
        struct json_value method;
        json_init(&method);
        json_set_object(&method);
        json_push_kv_str(&method, "method", pm->method);
        json_push_kv_str(&method, "capability", pm->capability);
        json_push_kv_str(&method, "native_command", pm->native_command);
        json_push_kv_str(&method, "mcp_tool", pm->mcp_tool);
        json_push_kv_str(&method, "schema", pm->schema);
        json_push_kv_bool(&method, "producer_advertises", true);
        json_push_kv_str(&method, "target_runtime_support", support);
        json_push_kv_bool(&method, "target_runtime_supports",
                          agent_availability_method_present(support));
        json_push_kv_bool(&method, "safe_to_call_target",
                          agent_availability_method_safe_to_call(support));
        json_push_kv_int(&method, "rpc_error_code",
                         probe->recorded ? probe->rpc_error_code : 0);
        json_push_kv_str(&method, "error_message",
                         probe->recorded ? probe->error_message : "");
        json_push_back(&methods, &method);
        json_free(&method);
    }
    json_push_kv(&obj, "methods", &methods);
    json_free(&methods);

    json_push_kv(out, key && key[0] ? key : "runtime_availability", &obj);
    json_free(&obj);
}

void agent_fill_operator_lane_contract_json(struct json_value *lane_obj,
                                            const char *operator_lane,
                                            const char *runtime_profile,
                                            const char *datadir,
                                            int rpc_port, int p2p_port,
                                            int https_port, int fs_port)
{
    if (!lane_obj)
        return;
    const char *lane =
        operator_lane && operator_lane[0] ? operator_lane : "unknown";
    const bool canonical = agent_lane_is(lane, "canonical");
    const bool soak = agent_lane_is(lane, "soak");
    const bool dev = agent_lane_is(lane, "dev");
    const bool ephemeral = agent_lane_is(lane, "test") ||
                           agent_lane_is(lane, "copy");
    const bool automation_restart_ok =
        agent_lane_automation_restart_ok(lane);
    const bool automation_deploy_ok =
        agent_lane_automation_deploy_ok(lane);
    const bool requires_operator_confirmation =
        agent_lane_requires_operator_confirmation(lane);
    struct json_value safety;

    json_set_object(lane_obj);
    json_push_kv_str(lane_obj, "schema", "zcl.operator_lane.v1");
    json_push_kv_int(lane_obj, "schema_version", 1);
    json_push_kv_str(lane_obj, "lane", lane);
    json_push_kv_str(lane_obj, "runtime_profile",
                     runtime_profile && runtime_profile[0]
                         ? runtime_profile : "unknown");
    json_push_kv_str(lane_obj, "datadir", datadir ? datadir : "");
    json_push_kv_int(lane_obj, "rpcport", rpc_port);
    json_push_kv_int(lane_obj, "p2p_port", p2p_port);
    json_push_kv_int(lane_obj, "https_port", https_port);
    json_push_kv_int(lane_obj, "fs_port", fs_port);
    json_push_kv_bool(lane_obj, "canonical", canonical);
    json_push_kv_bool(lane_obj, "soak_evidence", soak);
    json_push_kv_bool(lane_obj, "development", dev);
    json_push_kv_bool(lane_obj, "ephemeral", ephemeral);
    json_push_kv_str(lane_obj, "restart_policy",
                     agent_lane_restart_policy(lane));
    json_push_kv_str(lane_obj, "safety_contract",
                     agent_lane_safety_contract(lane));
    json_push_kv_bool(lane_obj, "automation_restart_ok",
                      automation_restart_ok);
    json_push_kv_bool(lane_obj, "automation_deploy_ok",
                      automation_deploy_ok);
    json_push_kv_bool(lane_obj, "requires_operator_confirmation",
                      requires_operator_confirmation);

    json_init(&safety);
    json_set_object(&safety);
    json_push_kv_str(&safety, "schema",
                     "zcl.operator_deployment_safety.v1");
    json_push_kv_int(&safety, "schema_version", 1);
    json_push_kv_bool(&safety, "automation_restart_ok",
                      automation_restart_ok);
    json_push_kv_bool(&safety, "automation_deploy_ok",
                      automation_deploy_ok);
    json_push_kv_bool(&safety, "requires_operator_confirmation",
                      requires_operator_confirmation);
    json_push_kv_bool(&safety, "protects_public_endpoint", canonical);
    json_push_kv_bool(&safety, "counts_for_soak_hours", soak);
    json_push_kv_bool(&safety, "isolated_from_canonical_datadir",
                      agent_lane_is_isolated_from_canonical(lane));
    json_push_kv_str(&safety, "preferred_deploy_target",
                     agent_lane_preferred_deploy_target(lane));
    json_push_kv_str(&safety, "guard_env",
                     agent_lane_guard_env(lane));
    json_push_kv_str(&safety, "safe_default_action",
                     agent_lane_safe_default_action(lane));
    json_push_kv(lane_obj, "deployment_safety", &safety);
    json_free(&safety);
}

void agent_push_operator_lane_fields_json(struct json_value *out)
{
    if (!out)
        return;

    const char *lane = g_agent_runtime.operator_lane[0]
        ? g_agent_runtime.operator_lane : "unknown";
    json_push_kv_str(out, "operator_lane_name", lane);
    json_push_kv_bool(out, "automation_restart_ok",
                      agent_lane_automation_restart_ok(lane));
    json_push_kv_bool(out, "automation_deploy_ok",
                      agent_lane_automation_deploy_ok(lane));
    json_push_kv_bool(out, "requires_operator_confirmation",
                      agent_lane_requires_operator_confirmation(lane));
    json_push_kv_str(out, "preferred_deploy_target",
                     agent_lane_preferred_deploy_target(lane));
    json_push_kv_str(out, "safe_default_action",
                     agent_lane_safe_default_action(lane));
}

void agent_push_operator_lane_json(struct json_value *out,
                                   const char *key)
{
    if (!out)
        return;
    const char *out_key = (key && key[0]) ? key : "operator_lane";
    struct json_value lane_obj;
    json_init(&lane_obj);
    agent_fill_operator_lane_contract_json(&lane_obj,
                                           g_agent_runtime.operator_lane,
                                           g_agent_runtime.runtime_profile,
                                           g_agent_runtime.datadir,
                                           g_agent_runtime.rpc_port,
                                           g_agent_runtime.p2p_port,
                                           g_agent_runtime.https_port,
                                           g_agent_runtime.fs_port);
    json_push_kv(out, out_key, &lane_obj);
    json_free(&lane_obj);
}

static const char *agent_env_or_empty(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] ? value : "";
}

void agent_push_runtime_build_json(struct json_value *out,
                                   const char *key)
{
    if (!out)
        return;

    const char *running = zcl_build_commit();
    const char *expected =
        agent_env_or_empty("ZCL_AGENT_EXPECT_BUILD_COMMIT");
    const char *source =
        agent_env_or_empty("ZCL_AGENT_EXPECT_BUILD_SOURCE");
    bool expected_present = expected[0] != '\0';
    bool matches = expected_present && strcmp(running, expected) == 0;
    const char *freshness = expected_present
        ? (matches ? "current" : "stale") : "unknown";
    struct json_value obj;

    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", "zcl.runtime_build.v1");
    json_push_kv_int(&obj, "schema_version", 1);
    json_push_kv_str(&obj, "running_build_commit", running);
    json_push_kv_str(&obj, "expected_build_commit", expected);
    json_push_kv_str(&obj, "expected_source",
                     source[0] ? source : "unset");
    json_push_kv_bool(&obj, "expected_present", expected_present);
    json_push_kv_bool(&obj, "matches_expected", matches);
    json_push_kv_bool(&obj, "stale", expected_present && !matches);
    json_push_kv_bool(&obj, "dirty_build",
                      strstr(running, "-dirty") != NULL);
    json_push_kv_str(&obj, "freshness", freshness);
    json_push_kv_str(&obj, "semantics",
                     "expected_build_commit is deploy-installed runtime intent; stale=true means this process is not the expected deployed binary");
    json_push_kv(out, key && key[0] ? key : "runtime_build", &obj);
    json_free(&obj);
}

void agent_push_runtime_services_json(struct json_value *out,
                                      const char *key)
{
    if (!out)
        return;
    const char *out_key = (key && key[0]) ? key : "runtime_services";
    const bool rpc_running = rpc_http_is_running();
    const bool https_running = https_server_is_running();
    const int https_bound_port = https_server_port();
    const bool fs_running = fs_server_is_running();
    const int fs_bound_port = fs_running ? (int)fs_server_get_port() : 0;
    struct json_value svc;

    json_init(&svc);
    json_set_object(&svc);
    json_push_kv_str(&svc, "schema", "zcl.agent_runtime_services.v1");
    json_push_kv_int(&svc, "schema_version", 1);
    json_push_kv_str(&svc, "configured_ports_source", "boot_context");
    json_push_kv_str(&svc, "observed_services_source",
                     "in_process_listener_state");
    json_push_kv_str(&svc, "semantics",
                     "configured ports are argv/config intent; running and bound_port fields are observed in-process listener state");
    json_push_kv_int(&svc, "rpc_configured_port", g_agent_runtime.rpc_port);
    json_push_kv_bool(&svc, "rpc_running", rpc_running);
    json_push_kv_int(&svc, "rpc_bound_port",
                     rpc_running ? g_agent_runtime.rpc_port : 0);
    json_push_kv_int(&svc, "p2p_configured_port", g_agent_runtime.p2p_port);
    json_push_kv_bool(&svc, "p2p_observed_here", false);
    json_push_kv_str(&svc, "p2p_observed_source",
                     "zcl_agent, zcl_peers, or lane_health");
    json_push_kv_int(&svc, "https_configured_port",
                     g_agent_runtime.https_port);
    json_push_kv_bool(&svc, "https_running", https_running);
    json_push_kv_int(&svc, "https_bound_port",
                     https_running ? https_bound_port : 0);
    json_push_kv_bool(&svc, "https_deferred",
                      https_deferred_pending());
    json_push_kv_int(&svc, "fs_configured_port", g_agent_runtime.fs_port);
    json_push_kv_bool(&svc, "fs_running", fs_running);
    json_push_kv_int(&svc, "fs_bound_port", fs_bound_port);
    json_push_kv(out, out_key, &svc);
    json_free(&svc);
}
