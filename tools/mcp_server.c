/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZClassic23 MCP Server — Model Context Protocol for AI agents.
 *
 * Install:  claude mcp add zcl23 -- build/bin/zclassic23 -mcp
 * Usage:    Claude calls tools like zcl_status, zcl_getblock, zcl_peers
 *
 * Architecture:
 *   Claude Code <--stdio--> build/bin/zclassic23 -mcp <--HTTP--> zclassic23 RPC
 *
 * This file owns the MCP wire protocol only.  It:
 *
 *   1. Configures the RPC client (tools/mcp/rpc_client.c)
 *   2. Resets the router and calls each domain controller's
 *      mcp_register_XXX() in tools/mcp/controllers/.
 *   3. Runs the stdio loop, translating JSON-RPC messages into
 *      router calls and rendering results back to MCP content blocks.
 *
 * All parameter validation, schema JSON and error enveloping happens
 * inside tools/mcp/router.{h,c}.  Adding a new tool means touching a
 * controller file — not this file. */

#include "mcp/router.h"
#include "mcp/middleware.h"
#include "metrics/prometheus_metrics.h"
#include "mcp/replay.h"
#include "mcp/baseline.h"
#include "mcp/controllers.h"
#include "controllers/rpc_client.h"
#include "mcp/mcp_notify.h"

#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>

#include "util/safe_alloc.h"

/* Process-wide middleware.  Populated from environment at boot. */
/* MCP middleware singleton lives in middleware.c so test_zcl (which
 * links the controller lib but not mcp_server.c) can still resolve the
 * accessor symbol used by zcl_config_reload. */

/* ── MCP protocol ────────────────────────────────────────────── */

/* The request/response loop and the background event notifier both write
 * complete JSON-RPC lines to stdout. Serialize them so a pushed
 * notification frame can never interleave its bytes with a response. */
static pthread_mutex_t g_stdout_lock = PTHREAD_MUTEX_INITIALIZER;

static void mcp_send(const char *json)
{
    pthread_mutex_lock(&g_stdout_lock);
    fprintf(stdout, "%s\n", json);
    fflush(stdout);
    pthread_mutex_unlock(&g_stdout_lock);
}

/* Sink for mcp_notify: push one notification frame on the same stdout
 * channel the agent reads responses from. */
static void mcp_notify_sink_stdout(const char *json_line, void *ctx)
{
    (void)ctx;
    mcp_send(json_line);
}

static void handle_initialize(const struct json_value *req)
{
    const struct json_value *id = json_get(req, "id");
    char resp[512];
    snprintf(resp, sizeof(resp),
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{"
        "\"protocolVersion\":\"2024-11-05\","
        /* Advertise logging: operator-class EV_* events are pushed as
         * notifications/message frames on this channel (see mcp_notify). */
        "\"capabilities\":{\"tools\":{},\"logging\":{}},"
        "\"serverInfo\":{\"name\":\"zcl23\",\"version\":\"1.0.0\"}"
        "}}",
        id ? (long long)json_get_int(id) : 0LL);
    mcp_send(resp);
}

static void handle_tools_list(const struct json_value *req)
{
    const struct json_value *id = json_get(req, "id");
    size_t cap = 65536;
    char *buf = zcl_malloc(cap, "mcp tools list buf");
    if (!buf) return;

    int pos = snprintf(buf, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"tools\":",
        id ? (long long)json_get_int(id) : 0LL);

    size_t written = mcp_router_tools_list_json(buf + pos, cap - (size_t)pos);
    pos += (int)written;
    pos += snprintf(buf + pos, cap - (size_t)pos, "}}");
    mcp_send(buf);
    free(buf);
}

static void handle_tools_call(const struct json_value *req)
{
    const struct json_value *id = json_get(req, "id");
    const struct json_value *params = json_get(req, "params");
    const struct json_value *name_v = params ? json_get(params, "name") : NULL;
    const struct json_value *args = params ? json_get(params, "arguments") : NULL;

    if (!name_v) {
        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"jsonrpc\":\"2.0\",\"id\":%lld,"
            "\"error\":{\"code\":-32602,\"message\":\"missing tool name\"}}",
            id ? (long long)json_get_int(id) : 0LL);
        mcp_send(resp);
        return;
    }

    /* Bearer token, if the caller embedded one in the request metadata. */
    const char *bearer = NULL;
    const struct json_value *meta = params ? json_get(params, "_meta") : NULL;
    if (meta) {
        const struct json_value *auth = json_get(meta, "authorization");
        if (auth && auth->type == JSON_STR) bearer = json_get_str(auth);
    }

    struct mcp_middleware *mw = mcp_middleware_get_global();
    char *result = mcp_middleware_dispatch(mw,
                                            json_get_str(name_v), args, bearer);
    if (!result) result = strdup("null");

    /* Embed result as MCP text content, escaping for JSON. */
    size_t rlen = strlen(result);
    /* Worst case is 6 bytes per input char (\u00XX for a control char). */
    size_t cap = rlen * 6 + 512;
    char *resp = zcl_malloc(cap, "mcp tools call response");
    char *escaped = zcl_malloc(rlen * 6 + 1, "mcp tools call escaped");
    if (!resp || !escaped) {
        free(resp); free(escaped); free(result);
        char err[160];
        snprintf(err, sizeof(err),
            "{\"jsonrpc\":\"2.0\",\"id\":%lld,"
            "\"error\":{\"code\":-32603,\"message\":\"out of memory\"}}",
            id ? (long long)json_get_int(id) : 0LL);
        mcp_send(err);
        return;
    }
    size_t ei = 0;
    for (size_t i = 0; i < rlen; i++) {
        unsigned char c = (unsigned char)result[i];
        if (c == '"')       { escaped[ei++] = '\\'; escaped[ei++] = '"'; }
        else if (c == '\\') { escaped[ei++] = '\\'; escaped[ei++] = '\\'; }
        else if (c == '\n') { escaped[ei++] = '\\'; escaped[ei++] = 'n'; }
        else if (c == '\r') { escaped[ei++] = '\\'; escaped[ei++] = 'r'; }
        else if (c == '\t') { escaped[ei++] = '\\'; escaped[ei++] = 't'; }
        else if (c < 0x20)  { /* other control chars: JSON requires \u escaping */
            ei += (size_t)snprintf(escaped + ei, 7, "\\u%04x", c);
        }
        else                { escaped[ei++] = (char)c; }
    }
    escaped[ei] = 0;

    snprintf(resp, cap,
        "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{"
        "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}",
        id ? (long long)json_get_int(id) : 0LL, escaped);

    mcp_send(resp);
    free(escaped);
    free(resp);
    free(result);
}

/* ── Main loop ──────────────────────────────────────────────── */

static void register_all_controllers(void)
{
    mcp_router_reset();
    mcp_register_ops();
    mcp_register_diagnostics();
    mcp_register_chain();
    mcp_register_net();
    mcp_register_wallet();
    mcp_register_app();
    mcp_register_meta();
    mcp_register_dev_hotswap();
}

/* Live event source — polls the node's eventlog RPC. Defined in
 * mcp_notify.c; this is the seam exec-plan 4.1 replaces with a direct
 * in-process subscription. */
extern char *mcp_notify_eventlog_fetch(void *ctx);

int mcp_server_main(const char *datadir, int rpc_port)
{
    mcp_rpc_client_init(datadir, rpc_port);
    register_all_controllers();
    mcp_middleware_init_global();
    metrics_prometheus_init();
    mcp_replay_init();
    mcp_baseline_init();

    bool notify_started = false;
    const char *notify_env = getenv("ZCL_MCP_NOTIFY");

    /* Start the event push out-channel: operator-class EV_* events the
     * node emits are converted to MCP notifications/message frames and
     * pushed on stdout, so a chain halt / SLO breach / operator-needed
     * condition reaches the agent without it polling zcl_events. One-shot
     * tooling can set ZCL_MCP_NOTIFY=0 to avoid a background eventlog poll. */
    if (!notify_env || strcmp(notify_env, "0") != 0) {
        notify_started = mcp_notify_start(mcp_notify_eventlog_fetch, NULL,
                                          mcp_notify_sink_stdout, NULL, 750);
    }

    char line[65536];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = 0;
        if (len == 0) continue;

        struct json_value req;
        if (!json_read(&req, line, len)) {
            mcp_send("{\"jsonrpc\":\"2.0\",\"id\":null,"
                     "\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
            continue;
        }

        const struct json_value *method = json_get(&req, "method");
        if (!method || method->type != JSON_STR) {
            json_free(&req);
            continue;
        }

        const char *m = json_get_str(method);

        if (strcmp(m, "initialize") == 0)
            handle_initialize(&req);
        else if (strcmp(m, "notifications/initialized") == 0)
            { /* no response */ }
        else if (strcmp(m, "tools/list") == 0)
            handle_tools_list(&req);
        else if (strcmp(m, "tools/call") == 0)
            handle_tools_call(&req);
        else {
            const struct json_value *id = json_get(&req, "id");
            char resp[256];
            snprintf(resp, sizeof(resp),
                "{\"jsonrpc\":\"2.0\",\"id\":%lld,"
                "\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}",
                id ? (long long)json_get_int(id) : 0LL);
            mcp_send(resp);
        }

        json_free(&req);
    }

    /* stdin closed (agent disconnected) — drain and join the notifier. */
    if (notify_started)
        mcp_notify_stop();
    return 0;
}

/* In-process MCP server (default OFF, behind -mcp-inprocess).
 *
 * Same wire protocol and same controller/middleware/notify stack as
 * mcp_server_main, but the RPC transport is the in-process backend:
 * every tool's mcp_node_rpc call dispatches straight to the node's live
 * rpc_table (rpc_table_execute) instead of opening a socket and POSTing
 * JSON-RPC to 127.0.0.1. This requires a fully-booted node in the SAME
 * process (rpc_http_active_table() must be live), so main.c only reaches
 * here AFTER app_init has started the RPC service.
 *
 * The event push channel is unchanged: mcp_notify_eventlog_fetch still
 * calls mcp_node_rpc("eventlog",...), which now resolves in-process too.
 *
 * Returns when stdin closes (agent disconnects); the caller then drives a
 * normal node shutdown. */
int mcp_server_main_inprocess(const char *datadir, int rpc_port)
{
    /* Flip the transport BEFORE any controller can issue a call. From here
     * on mcp_node_rpc routes to the live rpc_table in this process. */
    mcp_rpc_client_use_inprocess();
    return mcp_server_main(datadir, rpc_port);
}
