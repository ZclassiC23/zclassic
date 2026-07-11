/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for zcl_listunspent /
 * zcl_listtransactions / zcl_gettransaction / zcl_listaddresses
 * (ZERO-MCP W0-A). Each function is the argument-parsing plus
 * RPC-composition core of the legacy MCP handler in
 * tools/mcp/controllers/wallet_controller.c, with the MCP-specific error
 * envelope stripped out — see controllers/native_handler_body.h for the
 * failure contract. Called by both the MCP wrapper handler (which maps a
 * NULL return onto the historical res->error / res->error_message) and the
 * native command bridge (tools/command/native_command.c). */

#include "controllers/wallet_native_handlers.h"

#include "json/json.h"
#include "mcp/rpc_client.h"
#include "mcp/rpc_params.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *zcl_native_listunspent_body(const struct json_value *args,
                                   struct zcl_native_body_err *err)
{
    char params[128];
    snprintf(params, sizeof(params), "[%lld,%lld]",
             (long long)json_get_int_or(args, "minconf", 1),
             (long long)json_get_int_or(args, "maxconf", 9999999));
    char *out = mcp_node_rpc("listunspent", params);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "listunspent");
        LOG_NULL("mcp.wallet", "RPC %s returned null", "listunspent");
    }
    return out;
}

char *zcl_native_listtransactions_body(const struct json_value *args,
                                        struct zcl_native_body_err *err)
{
    char params[128];
    snprintf(params, sizeof(params), "[\"\",%lld,%lld]",
             (long long)json_get_int_or(args, "count", 10),
             (long long)json_get_int_or(args, "skip",   0));
    char *out = mcp_node_rpc("listtransactions", params);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "listtransactions");
        LOG_NULL("mcp.wallet", "RPC %s returned null", "listtransactions");
    }
    return out;
}

char *zcl_native_gettransaction_body(const struct json_value *args,
                                      struct zcl_native_body_err *err)
{
    const char *v = json_get_str(json_get(args, "txid"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, v);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("gettransaction", params) : NULL;
    free(params);
    if (!out) {
        char ctx[192];
        snprintf(ctx, sizeof(ctx), "txid=%s", v ? v : "(null)");
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s failed: %s", "gettransaction", ctx);
        LOG_NULL("mcp.wallet", "%s failed: %s", "gettransaction", ctx);
    }
    return out;
}

char *zcl_native_listaddresses_body(const struct json_value *args,
                                     struct zcl_native_body_err *err)
{
    (void)args;
    /* The node RPC `listwalletkeys` returns {transparent_keys:[{address,...}],
     * sapling_keys:[...]}.  Call it without private keys and project just
     * the addresses so the caller gets a clean list. */
    char *raw = mcp_node_rpc("listwalletkeys", "[false]");
    if (!raw) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "listwalletkeys");
        LOG_NULL("mcp.wallet", "RPC %s returned null", "listwalletkeys");
    }

    struct json_value root;
    if (!json_read(&root, raw, strlen(raw)))
        return raw;
    free(raw);

    size_t cap = 65536;
    char *out = zcl_malloc(cap, "listaddresses_body");
    if (!out) {
        json_free(&root);
        err->status = ZCL_NATIVE_BODY_INTERNAL;
        snprintf(err->message, sizeof(err->message),
                 "malloc failed for %s", "listaddresses response");
        if (cap > 0)
            LOG_NULL("mcp.wallet", "malloc failed for %s (%zu bytes)",
                     "listaddresses response", cap);
        LOG_NULL("mcp.wallet", "malloc failed for %s",
                 "listaddresses response");
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "{\"t_addresses\":[");

    const struct json_value *tk = json_get(&root, "transparent_keys");
    bool first = true;
    if (tk && tk->type == JSON_ARR) {
        for (size_t i = 0; i < tk->num_children; i++) {
            const struct json_value *k = &tk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    pos += (size_t)snprintf(out + pos, cap - pos, "],\"z_addresses\":[");

    const struct json_value *sk = json_get(&root, "sapling_keys");
    first = true;
    if (sk && sk->type == JSON_ARR) {
        for (size_t i = 0; i < sk->num_children; i++) {
            const struct json_value *k = &sk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    if (pos + 2 < cap) { out[pos++] = ']'; out[pos++] = '}'; out[pos] = 0; }

    json_free(&root);
    return out;
}

/* ── Tier-1 hot-swap: native.leaves generation entrypoint (W1-B/C) ──────
 * Dev-only (compiled only under -DZCL_HOTSWAP_GEN, a generation .so build;
 * expands to nothing in the node/release TU — see ZCL_HOTSWAP_EXPORT_LEAVES
 * in lib/hotswap/include/hotswap/hotswap.h). Stages every native command
 * leaf this controller owns; the resident bridge re-points them at THIS
 * TU's freshly-compiled bodies via zcl_native_bridge_run(). Probe is
 * core.wallet.address.list: zcl_native_listaddresses_body ignores `args`
 * ((void)args) and unconditionally calls listwalletkeys[false], returning
 * {"t_addresses":[...],"z_addresses":[...]} with no top-level "error" key
 * on success, so the empty-args self-test dispatch succeeds.
 * core.wallet.utxo.list / core.wallet.transaction.list also default their
 * params (minconf/maxconf, count/skip) and would work as a probe too;
 * core.wallet.transaction.get requires a caller-supplied txid and is NOT
 * a probe candidate. See config/hotswap_eligible.def. */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_PROBE_LEAF "core.wallet.address.list"
#include "hotswap/hotswap.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void tramp_listaddresses(const struct zcl_command_request *request,
                                struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_listaddresses_body, reply);
}

static void tramp_listunspent(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_listunspent_body, reply);
}

static void tramp_listtransactions(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_listtransactions_body, reply);
}

static void tramp_gettransaction(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_gettransaction_body, reply);
}

static const struct zcl_hotswap_leaf_replacement k_leaves[] = {
    { "core.wallet.address.list",      tramp_listaddresses },
    { "core.wallet.utxo.list",         tramp_listunspent },
    { "core.wallet.transaction.list",  tramp_listtransactions },
    { "core.wallet.transaction.get",   tramp_gettransaction },
};

ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, sizeof(k_leaves) / sizeof(k_leaves[0]))
#endif /* ZCL_HOTSWAP_GEN */
