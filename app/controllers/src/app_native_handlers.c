/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for the read-only ZCL application MCP
 * tools. Moved out of tools/mcp/controllers/app_controller.c so both the MCP
 * wrapper and the MCP-free native command bridge can call the same
 * composition; see controllers/native_handler_body.h for the contract these
 * functions satisfy. The MCP wrapper (app_controller.c) maps a NULL return +
 * err->status back onto the historical res->error / res->error_message so the
 * MCP surface stays byte-identical (dual-run through W2).
 *
 * Scope: the READ surface only (names resolve/list, tokens list, message
 * inbox, market list/status, swap chains/list). The destructive app-layer
 * commands stay PLANNED native leaves (fail-closed) in
 * config/commands/app_features.def until the app-write plan/commit handshake
 * lands — the wallet-send precedent (config/commands/core.def). */

#include "controllers/app_native_handlers.h"

#include "json/json.h"
#include "mcp/rpc_client.h"
#include "mcp/rpc_params.h"
#include "util/log_macros.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* One shared tail for the parameter-free 1:1 RPC proxies: call the backing
 * method with no params and surface a NULL RPC result as the legacy
 * "RPC <method> returned null" body failure. */
static char *app_native_rpc_noargs(const char *method,
                                   struct zcl_native_body_err *err)
{
    char *out = mcp_node_rpc(method, NULL);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", method);
        LOG_NULL("mcp.app", "RPC %s returned null", method);
    }
    return out;
}

/* ── ZSLP tokens ────────────────────────────────────────────── */

char *zcl_native_zslp_listtokens_body(const struct json_value *args,
                                      struct zcl_native_body_err *err)
{
    (void)args;
    return app_native_rpc_noargs("zslp_listtokens", err);
}

/* ── Names (ZNAM) ───────────────────────────────────────────── */

char *zcl_native_name_resolve_body(const struct json_value *args,
                                   struct zcl_native_body_err *err)
{
    const char *n = json_get_str(json_get(args, "name"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, n);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("name_resolve", params) : NULL;
    free(params);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s failed: name=%s", "name_resolve", n ? n : "(null)");
        LOG_NULL("mcp.app", "RPC %s failed: name=%s", "name_resolve",
                 n ? n : "(null)");
    }
    return out;
}

char *zcl_native_name_list_body(const struct json_value *args,
                                struct zcl_native_body_err *err)
{
    (void)args;
    return app_native_rpc_noargs("name_list", err);
}

/* ── Messaging (ZMSG) ───────────────────────────────────────── */

/* Native inbox is the full inbox, newest first. The MCP tool's optional
 * `unread_only` boolean filter is intentionally not carried onto the native
 * leaf: the registry input validator only admits a fixed set of boolean keys
 * (verbose/confirm/relink_generation), so an `unread_only` bool would be
 * rejected before dispatch. The full inbox is the honest bounded read. */
char *zcl_native_msg_inbox_body(const struct json_value *args,
                                struct zcl_native_body_err *err)
{
    (void)args;
    return app_native_rpc_noargs("msg_inbox", err);
}

/* ── File market ────────────────────────────────────────────── */

char *zcl_native_zmarket_list_body(const struct json_value *args,
                                   struct zcl_native_body_err *err)
{
    (void)args;
    return app_native_rpc_noargs("zmarket_list", err);
}

char *zcl_native_zmarket_status_body(const struct json_value *args,
                                     struct zcl_native_body_err *err)
{
    (void)args;
    return app_native_rpc_noargs("zmarket_status", err);
}

/* ── Atomic swaps (ZSWP) ────────────────────────────────────── */

char *zcl_native_swap_chains_body(const struct json_value *args,
                                  struct zcl_native_body_err *err)
{
    (void)args;
    return app_native_rpc_noargs("swap_chains", err);
}

char *zcl_native_swap_list_body(const struct json_value *args,
                                struct zcl_native_body_err *err)
{
    const struct json_value *st = json_get(args, "state");
    char *out;
    if (st) {
        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str(&p, json_get_str(st));
        char *params = mcp_params_to_json(&p);
        out = params ? mcp_node_rpc("swap_list", params) : NULL;
        free(params);
    } else {
        out = mcp_node_rpc("swap_list", NULL);
    }
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "swap_list");
        LOG_NULL("mcp.app", "RPC %s returned null", "swap_list");
    }
    return out;
}

/* ── Tier-1 hot-swap: native.leaves generation entrypoint ──────
 * Dev-only (compiled only under -DZCL_HOTSWAP_GEN, a generation .so build;
 * expands to nothing in the node/release TU — see ZCL_HOTSWAP_EXPORT_LEAVES
 * in lib/hotswap/include/hotswap/hotswap.h). Stages every native command leaf
 * this controller owns; the resident bridge re-points them at THIS TU's
 * freshly-compiled bodies via zcl_native_bridge_run(). Probe is
 * app.names.list: zcl_native_name_list_body ignores `args` ((void)args) and
 * unconditionally calls name_list with no params, forwarding the node's body
 * verbatim (no top-level "error" key is synthesized on success), so the
 * empty-args self-test dispatch the generation loader runs succeeds. The other
 * seven leaves are equally args-free (or take one optional/required string) and
 * would work as probes too; app.names.list is kept as the pilot probe to match
 * the legacy app_controller.c HOTSWAP_PROBE("zcl_name_list").
 * See config/hotswap_eligible.def. */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_PROBE_LEAF "app.names.list"
#include "hotswap/hotswap.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void tramp_name_list(const struct zcl_command_request *request,
                            struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_name_list_body, reply);
}

static void tramp_name_resolve(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_name_resolve_body, reply);
}

static void tramp_tokens(const struct zcl_command_request *request,
                         struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_zslp_listtokens_body, reply);
}

static void tramp_msg_inbox(const struct zcl_command_request *request,
                            struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_msg_inbox_body, reply);
}

static void tramp_market_list(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_zmarket_list_body, reply);
}

static void tramp_market_status(const struct zcl_command_request *request,
                                struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_zmarket_status_body, reply);
}

static void tramp_swap_chains(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_swap_chains_body, reply);
}

static void tramp_swap_list(const struct zcl_command_request *request,
                            struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_swap_list_body, reply);
}

static const struct zcl_hotswap_leaf_replacement k_leaves[] = {
    { "app.names.list",      tramp_name_list },
    { "app.names.resolve",   tramp_name_resolve },
    { "app.tokens.list",     tramp_tokens },
    { "app.messaging.inbox", tramp_msg_inbox },
    { "app.market.list",     tramp_market_list },
    { "app.market.status",   tramp_market_status },
    { "app.swap.chains",     tramp_swap_chains },
    { "app.swap.list",       tramp_swap_list },
};

ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, sizeof(k_leaves) / sizeof(k_leaves[0]))
#endif /* ZCL_HOTSWAP_GEN */
