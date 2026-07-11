/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for the zcl_peer_incidents and
 * zcl_onion_health MCP tools (ZERO-MCP W0-A). Moved out of
 * tools/mcp/controllers/net_controller.c so both the MCP wrapper and
 * the future native command bridge can call the same composition;
 * see controllers/native_handler_body.h for the contract these
 * functions satisfy. The MCP wrapper (net_controller.c) maps a NULL
 * return + err->status back onto the historical res->error /
 * res->error_message so the MCP surface stays byte-identical. */

#include "controllers/net_native_handlers.h"

#include "controllers/network_controller.h"
#include "json/json.h"
#include "mcp/rpc_client.h"
#include "net/onion_service.h"
#include "platform/time_compat.h"
#include "rpc/protocol.h"
#include "util/log_macros.h"
#include "util/path_check.h"
#include "util/safe_alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── zcl_peer_incidents ──────────────────────────────────────────── */

static bool rpc_body_is_method_not_found(const char *body,
                                         char *message,
                                         size_t message_len)
{
    if (message && message_len)
        message[0] = '\0';
    if (!body || !body[0])
        return false;

    struct json_value root;
    json_init(&root);
    if (!json_read(&root, body, strlen(body)) || root.type != JSON_OBJ) {
        json_free(&root);
        return false;
    }

    const struct json_value *rerr = json_get(&root, "error");
    const struct json_value *obj =
        rerr && rerr->type == JSON_OBJ ? rerr : &root;
    int64_t code = json_get_int(json_get(obj, "code"));
    const char *msg = json_get_str(json_get(obj, "message"));
    if (message && message_len)
        snprintf(message, message_len, "%s", msg);
    json_free(&root);
    return code == RPC_METHOD_NOT_FOUND;
}

static char *peer_incidents_dumpstate_fallback_body(const char *reason)
{
    char *raw = mcp_node_rpc("dumpstate",
                             "[\"peer_lifecycle\",\"incidents\"]");
    if (!raw)
        return NULL;

    struct json_value dumpstate;
    json_init(&dumpstate);
    char *out = NULL;
    if (json_read(&dumpstate, raw, strlen(raw)) &&
        dumpstate.type == JSON_OBJ) {
        struct json_value normalized;
        json_init(&normalized);
        if (peer_incidents_from_dumpstate_result_json(&dumpstate, &normalized,
                                                      reason)) {
            size_t need = json_write(&normalized, NULL, 0) + 1;
            out = zcl_malloc(need, "peer_incidents_fallback_json");
            if (out)
                json_write(&normalized, out, need);
        }
        json_free(&normalized);
    }
    json_free(&dumpstate);
    free(raw);
    return out;
}

char *zcl_native_peer_incidents_body(const struct json_value *args,
                                     struct zcl_native_body_err *err)
{
    (void)args;
    char *out = mcp_node_rpc("peerincidents", NULL);
    char message[192];
    if (rpc_body_is_method_not_found(out, message, sizeof(message))) {
        char *fallback = peer_incidents_dumpstate_fallback_body(message);
        if (fallback) {
            free(out);
            return fallback;
        }
    }
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC peerincidents returned null");
        LOG_NULL("mcp.net", "RPC peerincidents returned null");
    }
    return out;
}

/* ── zcl_onion_health ────────────────────────────────────────────── */

/* Mirrors the legacy mcp_res_set_oom (tools/mcp/controllers.h) tail but
 * returns NULL (this TU's functions are char*-returning body fns, not
 * int-returning MCP handlers). cap==0 means no single byte count applies,
 * matching the legacy convention (never hit by either call site below,
 * both pass a nonzero cap). */
static char *net_native_oom(struct zcl_native_body_err *err, size_t cap,
                            const char *what)
{
    err->status = ZCL_NATIVE_BODY_INTERNAL;
    snprintf(err->message, sizeof(err->message), "malloc failed for %s", what);
    if (cap > 0)
        LOG_NULL("mcp.net", "malloc failed for %s (%zu bytes)", what, cap);
    LOG_NULL("mcp.net", "malloc failed for %s", what);
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
 *
 * err->status = ZCL_NATIVE_BODY_INVALID on a bad `path` arg — the
 * wrapper maps this back to the legacy MCP_ERR_HANDLER_FAILED for
 * this tool (byte-compat trumps the generic INVALID mapping). */
char *zcl_native_onion_health_body(const struct json_value *args,
                                   struct zcl_native_body_err *err)
{
    const char *probe_path = json_get_str_or(args, "path", "/directory.json");
    if (!probe_path || !*probe_path) probe_path = "/directory.json";
    if (!path_check_url_arg(probe_path, 256)) {
        err->status = ZCL_NATIVE_BODY_INVALID;
        snprintf(err->message, sizeof(err->message),
                 "path: must start with '/', "
                 "be 1..256 chars, contain no control chars or '..' segments");
        LOG_WARN("mcp.net", "onion_health: %s", err->message);
        return NULL;
    }

    const char *addr = onion_service_get_address();

    char *body = zcl_malloc(512, "onion_health_body");
    if (!body)
        return net_native_oom(err, 512, "onion health response");

    if (!addr) {
        snprintf(body, 512,
            "{\"ok\":false,\"error\":\"not_started\","
             "\"onion_address\":null,\"path\":\"%s\","
             "\"latency_ms\":0,\"response_bytes\":0}",
            probe_path);
        return body;
    }

    struct timespec t0, t1;
    platform_time_monotonic_timespec(&t0);

    /* Heap, NOT a function-static buffer: the middleware runs handlers on a
     * detached worker thread and abandons it on timeout, so two invocations
     * could race a shared static. 64 KB is also borderline for the stack —
     * allocate per call and free before return. */
    const size_t resp_cap = 65536;
    uint8_t *resp = zcl_malloc(resp_cap, "onion_health_resp");
    if (!resp) {
        free(body);
        return net_native_oom(err, resp_cap, "onion health probe buffer");
    }
    size_t n = onion_service_handle_request("GET", probe_path, NULL, 0,
                                              resp, resp_cap);

    platform_time_monotonic_timespec(&t1);
    free(resp);  /* only the byte count `n` is needed past this point */
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

    return body;
}
