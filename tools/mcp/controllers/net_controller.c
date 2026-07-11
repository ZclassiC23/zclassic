/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP net controller: peers, network info, peer discovery, ping, games. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

/* Tier-1 hot-swap: this controller's route table is generation-exportable
 * (see config/hotswap_eligible.def). The probe names a read-only route it
 * owns; the #define MUST precede hotswap.h. */
#define ZCL_HOTSWAP_PROBE_TOOLS "zcl_networkinfo"
#include "hotswap/hotswap.h"
#include "controllers/net_native_handlers.h"
#include "json/json.h"
#include "mcp/metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_PT(h_zcl_peers,        "getpeerinfo",    "mcp.net")
DEFINE_PT(h_zcl_networkinfo,  "getnetworkinfo", "mcp.net")
DEFINE_PT(h_zcl_bootstrapstatus, "bootstrapstatus", "mcp.net")
DEFINE_PT(h_zcl_onion_status, "healthcheck",    "mcp.net")
DEFINE_PT(h_zcl_gametypes,    "gametypes",      "mcp.net")
DEFINE_PT(h_zcl_peerlatency,  "getpeerlatency", "mcp.net")

/* zcl_peer_incidents — body function re-homed to
 * app/controllers/src/net_native_handlers.c (ZERO-MCP W0-A). This wrapper
 * maps a NULL return back onto the historical MCP_ERR_HANDLER_FAILED /
 * "RPC peerincidents returned null" envelope; the body function already
 * logged the failure (mcp.net tag), so this wrapper does not re-log. */
static int h_zcl_peer_incidents(const struct mcp_request *req,
                                struct mcp_response *res)
{
    (void)req;
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_peer_incidents_body(req->args, &e);
    if (!body) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message), "%s",
                 e.message);
    }
    res->body = body;
    return 0;
}

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

/* zcl_onion_health — body function re-homed to
 * app/controllers/src/net_native_handlers.c (ZERO-MCP W0-A).
 *
 * Returns:
 *   { onion_address, path, ok, latency_ms, response_bytes, error? }
 * When onion service is not started:
 *   { ok: false, error: "not_started" }
 *
 * Byte-compat note: the body function reports a bad `path` arg as
 * ZCL_NATIVE_BODY_INVALID (the generic "caller input failed validation"
 * status), but this tool's *historical* MCP envelope used
 * MCP_ERR_HANDLER_FAILED for that case — so this wrapper maps INVALID to
 * HANDLER_FAILED here specifically, overriding the usual INVALID mapping.
 * An allocation failure (ZCL_NATIVE_BODY_INTERNAL) maps to MCP_ERR_INTERNAL
 * as usual. The body function already logged the failure; this wrapper
 * does not re-log. */
static int h_zcl_onion_health(const struct mcp_request *req,
                               struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_onion_health_body(req->args, &e);
    if (!body) {
        res->error = (e.status == ZCL_NATIVE_BODY_INTERNAL)
                          ? MCP_ERR_INTERNAL
                          : MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message), "%s",
                 e.message);
    }
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
    { "zcl_peer_incidents", "net",
      "Bounded peer lifecycle incident view: reconnects, duplicates, "
      "disconnect reasons, services, advertised height, and bootstrap usefulness.",
      NULL, 0, h_zcl_peer_incidents, 0, NULL },
    { "zcl_bootstrapstatus", "net",
      "Versioned bootstrap-service contract: ordinary P2P bootstrap "
      "readiness plus zclassicd beta6 snapshot-bootstrap compatibility.",
      NULL, 0, h_zcl_bootstrapstatus, 0, NULL },
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
        mcp_router_register_required(&k_routes[i]);
}

/* ── Hot-swap: generation entrypoint (dev-only; no-op in node/release) ──
 * Under -DZCL_HOTSWAP_GEN this emits the v2 manifest + zcl_hotswap_gen_init
 * that re-points every route above at this TU's freshly-compiled handlers.
 * Read-only network-introspection routes, all-const file scope — swap-safe.
 * No trailing semicolon. */
ZCL_HOTSWAP_EXPORT_ROUTES(k_routes, PARAM_COUNT(k_routes))
