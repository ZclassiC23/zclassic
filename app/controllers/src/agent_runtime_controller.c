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
