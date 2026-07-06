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
    char operator_lane_source[64];
    char runtime_profile[32];
    char datadir[1024];
    int rpc_port;
    int p2p_port;
    int https_port;
    int fs_port;
    bool operator_lane_declared;
    bool operator_lane_inferred;
};

static struct agent_runtime_context g_agent_runtime = {
    .operator_lane = "unknown",
    .operator_lane_source = "unset",
    .runtime_profile = "unknown",
    .datadir = "",
    .rpc_port = 0,
    .p2p_port = 0,
    .https_port = 0,
    .fs_port = 0,
    .operator_lane_declared = false,
    .operator_lane_inferred = false,
};

enum {
    AGENT_RUNTIME_METHOD_CAPACITY = 0
#define AGENT_CONTRACT(method, capability, schema, native, mcp, rest, purpose) + 1
#include "controllers/agent_contracts.def"
#undef AGENT_CONTRACT
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
    struct agent_runtime_method_probe methods[AGENT_RUNTIME_METHOD_CAPACITY];
};

static struct agent_runtime_availability_state g_agent_availability = {
    .probe_started = false,
    .source = "producer_runtime",
    .datadir = "",
    .rpc_port = 0,
    .probe_status = "self_declared_current_runtime",
    .target_build_commit = "",
};

void rpc_agent_set_boot_context(const char *operator_lane,
                                const char *runtime_profile,
                                const char *datadir,
                                int rpc_port, int p2p_port,
                                int https_port, int fs_port)
{
    const bool declared = operator_lane && operator_lane[0] &&
        strcmp(operator_lane, "unknown") != 0;
    const struct agent_operator_lane_topology *inferred = NULL;
    const char *lane = declared ? operator_lane : "unknown";
    const char *source = declared ? "declared_boot_context" : "unknown";

    if (!declared) {
        inferred = agent_operator_lane_topology_match_runtime(datadir,
                                                             rpc_port,
                                                             p2p_port);
        if (inferred) {
            lane = inferred->lane;
            source = "inferred_exact_topology";
        }
    }

    snprintf(g_agent_runtime.operator_lane,
             sizeof(g_agent_runtime.operator_lane), "%s",
             lane && lane[0] ? lane : "unknown");
    snprintf(g_agent_runtime.operator_lane_source,
             sizeof(g_agent_runtime.operator_lane_source), "%s", source);
    g_agent_runtime.operator_lane_declared = declared;
    g_agent_runtime.operator_lane_inferred = inferred != NULL;
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

static size_t agent_runtime_contract_count(void)
{
    size_t n = agent_contract_count();
    return n < AGENT_RUNTIME_METHOD_CAPACITY ? n
                                             : AGENT_RUNTIME_METHOD_CAPACITY;
}

static size_t agent_runtime_method_index(const char *method)
{
    size_t n = agent_runtime_contract_count();
    if (!method || !method[0])
        return n;
    for (size_t i = 0; i < n; i++) {
        const struct agent_contract *c = agent_contract_at(i);
        if (c && strcmp(c->method, method) == 0)
            return i;
    }
    return n;
}

size_t agent_runtime_probe_method_count(void)
{
    return agent_runtime_contract_count();
}

const char *agent_runtime_probe_method_name(size_t index)
{
    const struct agent_contract *c = agent_contract_at(index);
    return c ? c->method : "";
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
    if (idx >= agent_runtime_contract_count())
        return;
    const struct agent_contract *c = agent_contract_at(idx);
    if (!c)
        return;
    struct agent_runtime_method_probe *m =
        &g_agent_availability.methods[idx];
    snprintf(m->method, sizeof(m->method), "%s", c->method);
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
    for (size_t i = 0; i < agent_runtime_contract_count(); i++) {
        if (g_agent_availability.methods[i].recorded)
            return true;
    }
    return false;
}

static const char *agent_availability_method_support(size_t idx)
{
    if (idx >= agent_runtime_contract_count())
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

    for (size_t i = 0; i < agent_runtime_contract_count(); i++) {
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
    json_push_kv_str(&obj, "operator_lane_source",
                     g_agent_runtime.operator_lane_source[0]
                         ? g_agent_runtime.operator_lane_source : "unset");
    json_push_kv_bool(&obj, "operator_lane_declared",
                      g_agent_runtime.operator_lane_declared);
    json_push_kv_bool(&obj, "operator_lane_inferred",
                      g_agent_runtime.operator_lane_inferred);
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
    for (size_t i = 0; i < agent_runtime_contract_count(); i++) {
        const struct agent_contract *pm = agent_contract_at(i);
        if (!pm)
            continue;
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

void agent_push_operator_lane_fields_json(struct json_value *out)
{
    if (!out)
        return;

    const char *lane = g_agent_runtime.operator_lane[0]
        ? g_agent_runtime.operator_lane : "unknown";
    json_push_kv_str(out, "operator_lane_name", lane);
    json_push_kv_str(out, "operator_lane_source",
                     g_agent_runtime.operator_lane_source[0]
                         ? g_agent_runtime.operator_lane_source : "unset");
    json_push_kv_bool(out, "operator_lane_declared",
                      g_agent_runtime.operator_lane_declared);
    json_push_kv_bool(out, "operator_lane_inferred",
                      g_agent_runtime.operator_lane_inferred);
    agent_push_operator_lane_safety_fields_json(out, lane);
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
    json_push_kv_str(&lane_obj, "lane_source",
                     g_agent_runtime.operator_lane_source[0]
                         ? g_agent_runtime.operator_lane_source : "unset");
    json_push_kv_bool(&lane_obj, "lane_declared",
                      g_agent_runtime.operator_lane_declared);
    json_push_kv_bool(&lane_obj, "lane_inferred",
                      g_agent_runtime.operator_lane_inferred);
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
