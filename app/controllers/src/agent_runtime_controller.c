/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Runtime boot context exported through the versioned agent/operator APIs.
 * Keep this state narrow: it describes how this binary instance is meant to be
 * operated, not consensus, wallet, or peer state. */

#include "controllers/agent_controller.h"

#include "json/json.h"

#include <stdio.h>
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

void agent_push_operator_lane_json(struct json_value *out,
                                   const char *key)
{
    if (!out)
        return;
    const char *lane = g_agent_runtime.operator_lane;
    const char *out_key = (key && key[0]) ? key : "operator_lane";
    struct json_value lane_obj;

    json_init(&lane_obj);
    json_set_object(&lane_obj);
    json_push_kv_str(&lane_obj, "schema", "zcl.operator_lane.v1");
    json_push_kv_int(&lane_obj, "schema_version", 1);
    json_push_kv_str(&lane_obj, "lane", lane);
    json_push_kv_str(&lane_obj, "runtime_profile",
                     g_agent_runtime.runtime_profile);
    json_push_kv_str(&lane_obj, "datadir", g_agent_runtime.datadir);
    json_push_kv_int(&lane_obj, "rpcport", g_agent_runtime.rpc_port);
    json_push_kv_int(&lane_obj, "p2p_port", g_agent_runtime.p2p_port);
    json_push_kv_int(&lane_obj, "https_port", g_agent_runtime.https_port);
    json_push_kv_int(&lane_obj, "fs_port", g_agent_runtime.fs_port);
    json_push_kv_bool(&lane_obj, "canonical",
                      agent_lane_is(lane, "canonical"));
    json_push_kv_bool(&lane_obj, "soak_evidence",
                      agent_lane_is(lane, "soak"));
    json_push_kv_bool(&lane_obj, "development",
                      agent_lane_is(lane, "dev"));
    json_push_kv_bool(&lane_obj, "ephemeral",
                      agent_lane_is(lane, "test") ||
                      agent_lane_is(lane, "copy"));
    json_push_kv_str(&lane_obj, "restart_policy",
                     agent_lane_restart_policy(lane));
    json_push_kv_str(&lane_obj, "safety_contract",
                     agent_lane_safety_contract(lane));
    json_push_kv(out, out_key, &lane_obj);
    json_free(&lane_obj);
}
