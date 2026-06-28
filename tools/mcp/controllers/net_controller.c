/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP net controller: peers, network info, peer discovery, ping, games. */

#include "platform/time_compat.h"
#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "json/json.h"
#include "mcp/metrics.h"
#include "net/onion_service.h"
#include "util/log_macros.h"
#include "util/path_check.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

DEFINE_PT(h_zcl_peers,        "getpeerinfo",    "mcp.net")
DEFINE_PT(h_zcl_networkinfo,  "getnetworkinfo", "mcp.net")
DEFINE_PT(h_zcl_onion_status, "healthcheck",    "mcp.net")
DEFINE_PT(h_zcl_gametypes,    "gametypes",      "mcp.net")
DEFINE_PT(h_zcl_peerlatency,  "getpeerlatency", "mcp.net")

static int h_zcl_addnode(const struct mcp_request *req, struct mcp_response *res)
{
    const char *addr = json_get_str(json_get(req->args, "addr"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, addr);
    mcp_params_push_str(&p, json_get_str_or(req->args, "action", "onetry"));
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("addnode", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "addnode", "mcp.net",
                                   "addr=%s", addr ? addr : "(null)");
}

static int h_zcl_pingpeer(const struct mcp_request *req, struct mcp_response *res)
{
    int64_t peer_id = json_get_int(json_get(req->args, "peer_id"));
    char params[64];
    snprintf(params, sizeof(params), "[%lld]", (long long)peer_id);
    char *out = mcp_node_rpc("pingpeer", params);
    return mcp_return_rpc_body_ctx(res, out, "pingpeer", "mcp.net",
                                   "peer_id=%lld", (long long)peer_id);
}

/* zcl_peer_report — peer scoring summary derived from in-process
 * metrics counters that subscribe to EV_PEER_MISBEHAVE / EV_PEER_BANNED.
 * Returns the live ban threshold/hours/decay config plus offence
 * counts since boot, bucketed by canonical offence kind. */
static int h_zcl_peer_report(const struct mcp_request *req,
                              struct mcp_response *res)
{
    (void)req;
    char body[2048];
    size_t n = mcp_metrics_peer_report_json(body, sizeof(body));
    char *out = (n == 0) ? NULL : strdup(body);
    return mcp_return_rpc_body(res, out, "peer_report", "mcp.net");
}

/* zcl_onion_health — probe the in-process onion service by calling
 * `onion_service_handle_request()` directly and measuring the
 * response size + wall-clock latency.  Synchronous: one call per
 * invocation.  Bypasses Tor and the SOCKS layer entirely (dynhost
 * has no SOCKS per the project memory), so this is a cheap
 * liveness check, not a full end-to-end test — the latter would
 * require an actual Tor circuit roundtrip and can't run from a
 * trusted-peer tool.
 *
 * Returns:
 *   { onion_address, path, ok, latency_ms, response_bytes, error? }
 * When onion service is not started:
 *   { ok: false, error: "not_started" }
 */
static int h_zcl_onion_health(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const char *probe_path = json_get_str_or(req->args, "path", "/directory.json");
    if (!probe_path || !*probe_path) probe_path = "/directory.json";
    if (!path_check_url_arg(probe_path, 256)) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "path: must start with '/', "
                 "be 1..256 chars, contain no control chars or '..' segments");
        LOG_WARN("mcp.net", "onion_health: %s", res->error_message);
        return 0;
    }

    const char *addr = onion_service_get_address();

    char *body = zcl_malloc(512, "onion_health_body");
    if (!body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for onion health response");
        LOG_ERR("mcp.net", "malloc failed for onion_health body (512 bytes)");
        return 0;
    }

    if (!addr) {
        snprintf(body, 512,
            "{\"ok\":false,\"error\":\"not_started\","
             "\"onion_address\":null,\"path\":\"%s\","
             "\"latency_ms\":0,\"response_bytes\":0}",
            probe_path);
        res->body = body;
        return 0;
    }

    struct timespec t0, t1;
    platform_time_monotonic_timespec(&t0);

    static uint8_t resp[65536];
    size_t n = onion_service_handle_request("GET", probe_path, NULL, 0,
                                              resp, sizeof(resp));

    platform_time_monotonic_timespec(&t1);
    int64_t latency_us =
        (t1.tv_sec - t0.tv_sec) * 1000000LL +
        (t1.tv_nsec - t0.tv_nsec) / 1000LL;
    int64_t latency_ms = latency_us / 1000;

    bool ok = (n > 0);

    snprintf(body, 512,
        "{\"ok\":%s,\"onion_address\":\"%s\",\"path\":\"%s\","
         "\"latency_ms\":%lld,\"response_bytes\":%zu%s}",
        ok ? "true" : "false",
        addr, probe_path,
        (long long)latency_ms, n,
        ok ? "" : ",\"error\":\"empty_response\"");

    res->body = body;
    return 0;
}

static const struct mcp_param_spec p_addnode[] = {
    { "addr",   MCP_PARAM_STR, true,  "IP:port",
      0, 0, 1, 128, NULL, NULL },
    { "action", MCP_PARAM_STR, false, "add | remove | onetry",
      0, 0, 0, 0, "add,remove,onetry", "\"onetry\"" },
};
static const struct mcp_param_spec p_pingpeer[] = {
    { "peer_id", MCP_PARAM_INT, true, "Peer ID from zcl_peers",
      0, 1000000, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_onion_health[] = {
    { "path", MCP_PARAM_STR, false,
      "URL path to probe (default /directory.json)",
      0, 0, 0, 256, NULL, "\"/directory.json\"" },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_peers", "net",
      "Connected peers with addresses, latency, services, heights.",
      NULL, 0, h_zcl_peers, 0, NULL },
    { "zcl_networkinfo", "net",
      "Network info: reachability, handshakes, and lifecycle failures by source.",
      NULL, 0, h_zcl_networkinfo, 0, NULL },
    { "zcl_addnode", "net",
      "Add/remove peer. Actions: add, remove, onetry.",
      p_addnode, PARAM_COUNT(p_addnode), h_zcl_addnode,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_onion_status", "net",
      "Tor onion service: .onion address, bootstrap state.",
      NULL, 0, h_zcl_onion_status, 0, NULL },
    { "zcl_gametypes", "net",
      "P2P game types: Ping (latency measurement), TicTacToe.",
      NULL, 0, h_zcl_gametypes, 0, NULL },
    { "zcl_pingpeer", "net",
      "Measure round-trip latency to a connected peer.",
      p_pingpeer, PARAM_COUNT(p_pingpeer), h_zcl_pingpeer,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE /* fires a P2P message */ },
    { "zcl_peerlatency", "net",
      "Latency for all peers: ping_ms, min_ping_ms, avg_latency_ms.",
      NULL, 0, h_zcl_peerlatency, 0, NULL },
    { "zcl_peer_report", "net",
      "Peer scoring report: live ban threshold/hours/decay config plus "
      "per-kind offence counts and total bans observed since boot.",
      NULL, 0, h_zcl_peer_report, 0, NULL },
    { "zcl_onion_health", "net",
      "Probe the in-process onion service via direct function call "
      "(no Tor circuit, no SOCKS). Returns {ok, onion_address, path, "
      "latency_ms, response_bytes}. Liveness check, not an e2e reach "
      "test.",
      p_onion_health, PARAM_COUNT(p_onion_health),
      h_zcl_onion_health, 0, NULL },
};

void mcp_register_net(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
}
