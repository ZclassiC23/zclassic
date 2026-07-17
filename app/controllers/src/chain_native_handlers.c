/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for zcl_getblock / zcl_getrawtransaction
 * / zcl_utxo_audit. Each function is the argument-parsing
 * plus RPC-composition core of the legacy MCP handler in
 * tools/mcp/controllers/chain_controller.c, with the MCP-specific error
 * envelope stripped out — see controllers/native_handler_body.h for the
 * failure contract. Called by both the MCP wrapper handler (which maps a
 * NULL return onto the historical res->error / res->error_message) and the
 * native command bridge (tools/command/native_command.c). */

#include "controllers/chain_native_handlers.h"

#include "json/json.h"
#include "controllers/rpc_client.h"
#include "controllers/rpc_params.h"
#include "util/log_macros.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

char *zcl_native_getrawtransaction_body(const struct json_value *args,
                                         struct zcl_native_body_err *err)
{
    const char *txid = json_get_str(json_get(args, "txid"));
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, txid);
    mcp_params_push_int(&p, json_get_int_or(args, "verbose", 1));
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("getrawtransaction", params) : NULL;
    free(params);
    if (!out) {
        char ctx[192];
        snprintf(ctx, sizeof(ctx), "txid=%s", txid ? txid : "(null)");
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s failed: %s", "getrawtransaction", ctx);
        LOG_NULL("mcp.chain", "%s failed: %s", "getrawtransaction", ctx);
    }
    return out;
}

char *zcl_native_getblock_body(const struct json_value *args,
                                struct zcl_native_body_err *err)
{
    const char *id_str = json_get_str(json_get(args, "block_id"));
    int verbosity = (int)json_get_int_or(args, "verbosity", 1);

    bool is_num = id_str && id_str[0];
    for (const char *c = id_str; is_num && *c; c++)
        if (*c < '0' || *c > '9') is_num = false;

    char clean[128] = {0};
    const char *hash_str = id_str;
    if (is_num) {
        struct mcp_params ph;
        mcp_params_init(&ph);
        mcp_params_push_int(&ph, id_str ? atoll(id_str) : 0);
        char *php = mcp_params_to_json(&ph);
        char *hash = php ? mcp_node_rpc("getblockhash", php) : NULL;
        free(php);
        if (!hash) {
            char ctx[192];
            snprintf(ctx, sizeof(ctx), "height=%s", id_str ? id_str : "(null)");
            err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
            snprintf(err->message, sizeof(err->message),
                     "RPC %s failed: %s", "getblockhash", ctx);
            LOG_NULL("mcp.chain", "%s failed: %s", "getblockhash", ctx);
        }
        size_t ci = 0;
        for (size_t i = 0; hash[i] && ci < 127; i++)
            if (hash[i] != '"' && hash[i] != '\n') clean[ci++] = hash[i];
        clean[ci] = 0;
        free(hash);
        hash_str = clean;
    }

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, hash_str);
    mcp_params_push_int(&p, verbosity);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("getblock", params) : NULL;
    free(params);
    if (!out) {
        char ctx[192];
        snprintf(ctx, sizeof(ctx), "id=%s", id_str ? id_str : "(null)");
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s failed: %s", "getblock", ctx);
        LOG_NULL("mcp.chain", "%s failed: %s", "getblock", ctx);
    }
    return out;
}

char *zcl_native_utxo_audit_body(const struct json_value *args,
                                  struct zcl_native_body_err *err)
{
    const char *remote = json_get_str_or(args, "remote_sha3", NULL);
    const char *source = json_get_str_or(args, "source",      NULL);

    struct mcp_params p;
    mcp_params_init(&p);
    if (remote && remote[0]) {
        mcp_params_push_str(&p, remote);
        mcp_params_push_int(&p, json_get_int_or(args, "remote_height", 0));
        mcp_params_push_str(&p, source && source[0] ? source : "trusted-peer");
    }
    char *params = mcp_params_to_json(&p);
    char *out = mcp_node_rpc("getutxoaudit", params);
    free(params);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "getutxoaudit");
        LOG_NULL("mcp.chain", "RPC %s returned null", "getutxoaudit");
    }
    return out;
}

/* ── Tier-1 hot-swap: native.leaves generation entrypoint ──────
 * Dev-only (compiled only under -DZCL_HOTSWAP_GEN, a generation .so build;
 * expands to nothing in the node/release TU — see ZCL_HOTSWAP_EXPORT_LEAVES
 * in lib/hotswap/include/hotswap/hotswap.h). Stages every native command
 * leaf this controller owns; the resident bridge re-points them at THIS
 * TU's freshly-compiled bodies via zcl_native_bridge_run(). Probe is
 * core.consensus.utxo.audit: rpc_getutxoaudit (blockchain_controller_chain.c)
 * accepts 0..3 params (rpc_params_expect(&p, 0, 3)) and, with remote_sha3
 * absent, falls back to utxo_audit_local() — a genuine local-only audit, not
 * an error — so the empty-args self-test dispatch succeeds. core.chain.
 * block.get and core.chain.transaction.get are NOT probe candidates: both
 * require a caller-supplied id (block_id / txid); with no args they call
 * getblock/getrawtransaction with an empty hash and get back a top-level
 * {"error":...} envelope, which would make the self-test spuriously fail.
 * See config/hotswap_eligible.def. */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_PROBE_LEAF "core.consensus.utxo.audit"
#include "hotswap/hotswap.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void tramp_getblock(const struct zcl_command_request *request,
                           struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_getblock_body, reply);
}

static void tramp_getrawtransaction(const struct zcl_command_request *request,
                                    struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_getrawtransaction_body, reply);
}

static void tramp_utxo_audit(const struct zcl_command_request *request,
                             struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_utxo_audit_body, reply);
}

static const struct zcl_hotswap_leaf_replacement k_leaves[] = {
    { "core.chain.block.get",       tramp_getblock },
    { "core.chain.transaction.get", tramp_getrawtransaction },
    { "core.consensus.utxo.audit",  tramp_utxo_audit },
};

ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, sizeof(k_leaves) / sizeof(k_leaves[0]))
#endif /* ZCL_HOTSWAP_GEN */
