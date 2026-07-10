/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Authenticated, dev-only JSON-RPC bridge into the resident MCP router.
 * Unlike `mcpcall` (a short-lived helper process), these handlers execute in
 * the running node, so a successfully loaded hot-swap generation remains
 * visible to later requests in that same process.
 *
 * The HTTP JSON-RPC server remains the transport and authentication boundary:
 * this file opens no listener and bypasses no cookie/middleware checks. The
 * generic dev_mcp_call refuses destructive MCP routes; the sole destructive
 * operation exposed here is the narrow dev_hotswap method.
 */

#include "dev_rpc_bridge.h"

#include "controllers.h"
#include "router.h"
#include "rpc_client.h"

#include "json/json.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "util/log_macros.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_MCP_DATADIR_MAX 1024

static size_t path_len_without_trailing_slash(const char *path)
{
    size_t len = path ? strlen(path) : 0;
    while (len > 1 && path[len - 1] == '/')
        len--;
    return len;
}

bool dev_mcp_rpc_bridge_datadir_allowed(const char *datadir)
{
    const char *home = getenv("HOME");
    char expected[DEV_MCP_DATADIR_MAX];
    size_t actual_len;
    size_t expected_len;
    int n;

    if (!home || !home[0] || !datadir || !datadir[0])
        return false;
    n = snprintf(expected, sizeof(expected), "%s/.zclassic-c23-dev", home);
    if (n < 0 || (size_t)n >= sizeof(expected))
        return false;
    actual_len = path_len_without_trailing_slash(datadir);
    expected_len = path_len_without_trailing_slash(expected);
    return actual_len == expected_len &&
        memcmp(datadir, expected, expected_len) == 0;
}

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)

static pthread_mutex_t g_dev_bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool g_dev_bridge_initialized;
static char g_dev_bridge_datadir[DEV_MCP_DATADIR_MAX];
static int g_dev_bridge_rpc_port;

static bool dev_bridge_runtime_lane_allowed(void)
{
    bool allowed;

    pthread_mutex_lock(&g_dev_bridge_lock);
    allowed = atomic_load_explicit(&g_dev_bridge_initialized,
                                   memory_order_acquire) &&
        dev_mcp_rpc_bridge_datadir_allowed(g_dev_bridge_datadir) &&
        strcmp(mcp_rpc_client_datadir(), g_dev_bridge_datadir) == 0;
    pthread_mutex_unlock(&g_dev_bridge_lock);
    return allowed;
}

static bool dev_bridge_rpc_error(struct json_value *result, int code,
                                 const char *method, const char *message)
{
    json_rpc_error_full(result, code, message, method);
    LOG_WARN("mcp.dev_rpc_bridge", "%s rejected: %s",
             method ? method : "dev bridge", message ? message : "error");
    return false;
}

static bool dev_bridge_parse_dispatch_body(const char *method, char *body,
                                           struct json_value *result)
{
    struct json_value parsed;
    const struct json_value *error;
    const struct json_value *message;

    if (!body)
        return dev_bridge_rpc_error(result, RPC_INTERNAL_ERROR, method,
                                    "resident MCP dispatch returned no body");
    json_init(&parsed);
    if (!json_read(&parsed, body, strlen(body))) {
        free(body);
        json_free(&parsed);
        return dev_bridge_rpc_error(result, RPC_INTERNAL_ERROR, method,
                                    "resident MCP dispatch returned invalid JSON");
    }
    free(body);

    error = parsed.type == JSON_OBJ ? json_get(&parsed, "error") : NULL;
    if (error && error->type == JSON_OBJ) {
        message = json_get(error, "message");
        char detail[256];
        snprintf(detail, sizeof(detail), "resident MCP error: %s",
                 message ? json_get_str(message) : "dispatch failed");
        json_free(&parsed);
        return dev_bridge_rpc_error(result, RPC_INTERNAL_ERROR, method, detail);
    }

    *result = parsed;
    return true;
}

static bool rpc_dev_hotswap(const struct json_value *params, bool help,
                            struct json_value *result)
{
    const struct json_value *path_value;
    const struct json_value *probe_value;
    const char *so_path;
    const char *probe = NULL;
    struct json_value args;
    char *body;

    if (help) {
        json_set_str(result,
            "dev_hotswap \"/absolute/generation.so\" ( \"probe_tool\" )");
        return true;
    }
    if (!dev_bridge_runtime_lane_allowed())
        return dev_bridge_rpc_error(result, RPC_FORBIDDEN_BY_SAFE_MODE,
                                    "dev_hotswap",
                                    "available only in the running ~/.zclassic-c23-dev node");
    if (!params || params->type != JSON_ARR || json_size(params) < 1 ||
        json_size(params) > 2) {
        return dev_bridge_rpc_error(result, RPC_INVALID_PARAMS, "dev_hotswap",
                                    "expected absolute so_path and optional probe tool");
    }
    path_value = json_at(params, 0);
    probe_value = json_size(params) > 1 ? json_at(params, 1) : NULL;
    if (!path_value || path_value->type != JSON_STR ||
        (probe_value && probe_value->type != JSON_NULL &&
         probe_value->type != JSON_STR)) {
        return dev_bridge_rpc_error(result, RPC_INVALID_PARAMS, "dev_hotswap",
                                    "so_path and probe must be strings");
    }
    so_path = json_get_str(path_value);
    if (!so_path || so_path[0] != '/')
        return dev_bridge_rpc_error(result, RPC_INVALID_PARAMETER,
                                    "dev_hotswap", "so_path must be absolute");
    if (probe_value && probe_value->type == JSON_STR)
        probe = json_get_str(probe_value);

    json_init(&args);
    json_set_object(&args);
    json_push_kv_str(&args, "so_path", so_path);
    if (probe && probe[0])
        json_push_kv_str(&args, "probe_tool", probe);
    body = mcp_router_dispatch("zcl_agent_hotswap", &args);
    json_free(&args);
    return dev_bridge_parse_dispatch_body("dev_hotswap", body, result);
}

static bool rpc_dev_mcp_call(const struct json_value *params, bool help,
                             struct json_value *result)
{
    const struct json_value *tool_value;
    const struct json_value *args = NULL;
    const struct mcp_tool_route *route;
    const char *tool;
    char *body;

    if (help) {
        json_set_str(result, "dev_mcp_call \"tool_name\" ( args_object )");
        return true;
    }
    if (!dev_bridge_runtime_lane_allowed())
        return dev_bridge_rpc_error(result, RPC_FORBIDDEN_BY_SAFE_MODE,
                                    "dev_mcp_call",
                                    "available only in the running ~/.zclassic-c23-dev node");
    if (!params || params->type != JSON_ARR || json_size(params) < 1 ||
        json_size(params) > 2) {
        return dev_bridge_rpc_error(result, RPC_INVALID_PARAMS, "dev_mcp_call",
                                    "expected tool name and optional args object");
    }
    tool_value = json_at(params, 0);
    if (!tool_value || tool_value->type != JSON_STR)
        return dev_bridge_rpc_error(result, RPC_INVALID_PARAMS, "dev_mcp_call",
                                    "tool name must be a string");
    if (json_size(params) > 1) {
        args = json_at(params, 1);
        if (args && args->type == JSON_NULL)
            args = NULL;
        else if (args && args->type != JSON_OBJ)
            return dev_bridge_rpc_error(result, RPC_INVALID_PARAMS,
                                        "dev_mcp_call",
                                        "tool args must be an object");
    }
    tool = json_get_str(tool_value);
    route = mcp_router_find(tool);
    if (!route)
        return dev_bridge_rpc_error(result, RPC_INVALID_PARAMETER,
                                    "dev_mcp_call", "unknown resident MCP tool");
    if ((route->flags & MCP_TOOL_FLAG_DESTRUCTIVE) != 0)
        return dev_bridge_rpc_error(result, RPC_FORBIDDEN_BY_SAFE_MODE,
                                    "dev_mcp_call",
                                    "destructive MCP tools require a dedicated RPC bridge");

    body = mcp_router_dispatch(tool, args);
    return dev_bridge_parse_dispatch_body("dev_mcp_call", body, result);
}

static bool dev_bridge_initialize_router(const char *datadir, int rpc_port)
{
    bool ok = true;

    pthread_mutex_lock(&g_dev_bridge_lock);
    if (atomic_load_explicit(&g_dev_bridge_initialized,
                             memory_order_acquire)) {
        ok = strcmp(g_dev_bridge_datadir, datadir) == 0 &&
            g_dev_bridge_rpc_port == rpc_port;
        pthread_mutex_unlock(&g_dev_bridge_lock);
        return ok;
    }

    snprintf(g_dev_bridge_datadir, sizeof(g_dev_bridge_datadir), "%s", datadir);
    g_dev_bridge_rpc_port = rpc_port;
    mcp_rpc_client_init(datadir, rpc_port);
    mcp_rpc_client_use_inprocess();
    mcp_router_reset();
    mcp_register_ops();
    mcp_register_diagnostics();
    mcp_register_chain();
    mcp_register_net();
    mcp_register_wallet();
    mcp_register_app();
    mcp_register_meta();
    mcp_register_dev_hotswap();
    atomic_store_explicit(&g_dev_bridge_initialized, true,
                          memory_order_release);
    pthread_mutex_unlock(&g_dev_bridge_lock);
    return true;
}

static bool dev_bridge_register_impl(struct rpc_table *table,
                                     const char *datadir, int rpc_port)
{
    static const struct rpc_command commands[] = {
        { "dev", "dev_hotswap", rpc_dev_hotswap, true },
        { "dev", "dev_mcp_call", rpc_dev_mcp_call, true },
    };

    if (!table || rpc_port <= 0 || rpc_port > 65535 ||
        !dev_mcp_rpc_bridge_datadir_allowed(datadir)) {
        LOG_WARN("mcp.dev_rpc_bridge",
                 "refusing dev RPC registration outside exact dev lane datadir=%s port=%d",
                 datadir ? datadir : "(null)", rpc_port);
        return false;
    }
    if (!dev_bridge_initialize_router(datadir, rpc_port)) {
        LOG_WARN("mcp.dev_rpc_bridge",
                 "resident MCP router already initialized for another runtime");
        return false;
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        rpc_table_must_append(table, &commands[i]);
    return true;
}

#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

bool register_dev_mcp_rpc_commands(struct rpc_table *table,
                                   const char *datadir, int rpc_port)
{
#ifdef ZCL_DEV_BUILD
    /* Non-dev lanes intentionally have no bridge and are a successful no-op;
     * false is reserved for an exact dev lane whose required registration
     * could not be completed. */
    if (!dev_mcp_rpc_bridge_datadir_allowed(datadir))
        return true;
    return dev_bridge_register_impl(table, datadir, rpc_port);
#else
    (void)table;
    (void)datadir;
    (void)rpc_port;
    return true;
#endif
}

#ifdef ZCL_TESTING
bool dev_mcp_rpc_bridge_test_register(struct rpc_table *table,
                                      const char *datadir, int rpc_port)
{
    return dev_bridge_register_impl(table, datadir, rpc_port);
}
#endif
