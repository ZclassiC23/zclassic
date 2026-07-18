/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native argument parsing and RPC composition for chain read commands. See
 * controllers/native_handler_body.h for the failure contract. */

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
    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, txid);
    rpc_arg_builder_push_int(&p, json_get_int_or(args, "verbose", 1));
    char *params = rpc_arg_builder_to_json(&p);
    char *out = params ? node_rpc_call("getrawtransaction", params) : NULL;
    free(params);
    if (!out) {
        char ctx[192];
        snprintf(ctx, sizeof(ctx), "txid=%s", txid ? txid : "(null)");
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s failed: %s", "getrawtransaction", ctx);
        LOG_NULL("native.chain", "%s failed: %s", "getrawtransaction", ctx);
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
        struct rpc_arg_builder ph;
        rpc_arg_builder_init(&ph);
        rpc_arg_builder_push_int(&ph, id_str ? atoll(id_str) : 0);
        char *php = rpc_arg_builder_to_json(&ph);
        char *hash = php ? node_rpc_call("getblockhash", php) : NULL;
        free(php);
        if (!hash) {
            char ctx[192];
            snprintf(ctx, sizeof(ctx), "height=%s", id_str ? id_str : "(null)");
            err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
            snprintf(err->message, sizeof(err->message),
                     "RPC %s failed: %s", "getblockhash", ctx);
            LOG_NULL("native.chain", "%s failed: %s", "getblockhash", ctx);
        }
        size_t ci = 0;
        for (size_t i = 0; hash[i] && ci < 127; i++)
            if (hash[i] != '"' && hash[i] != '\n') clean[ci++] = hash[i];
        clean[ci] = 0;
        free(hash);
        hash_str = clean;
    }

    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, hash_str);
    rpc_arg_builder_push_int(&p, verbosity);
    char *params = rpc_arg_builder_to_json(&p);
    char *out = params ? node_rpc_call("getblock", params) : NULL;
    free(params);
    if (!out) {
        char ctx[192];
        snprintf(ctx, sizeof(ctx), "id=%s", id_str ? id_str : "(null)");
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s failed: %s", "getblock", ctx);
        LOG_NULL("native.chain", "%s failed: %s", "getblock", ctx);
    }
    return out;
}

char *zcl_native_utxo_audit_body(const struct json_value *args,
                                  struct zcl_native_body_err *err)
{
    const char *remote = json_get_str_or(args, "remote_sha3", NULL);
    const char *source = json_get_str_or(args, "source",      NULL);

    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    if (remote && remote[0]) {
        rpc_arg_builder_push_str(&p, remote);
        rpc_arg_builder_push_int(&p, json_get_int_or(args, "remote_height", 0));
        rpc_arg_builder_push_str(&p, source && source[0] ? source : "trusted-peer");
    }
    char *params = rpc_arg_builder_to_json(&p);
    char *out = node_rpc_call("getutxoaudit", params);
    free(params);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "getutxoaudit");
        LOG_NULL("native.chain", "RPC %s returned null", "getutxoaudit");
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

/* REAL (activatable) single-handler module ABI export. Compiled only under a
 * `make hotswap-module-so HANDLER=core.consensus.utxo.audit` build
 * (-DZCL_HOTSWAP_MODULE_GEN); expands to nothing in the node/release TU. The
 * module re-points ONLY the `core.consensus.utxo.audit` leaf to this TU's
 * freshly-compiled body via the same zcl_native_bridge_run() seam the leaf
 * provider uses. See hotswap_module.h and hotswap_activate() (lib/hotswap).
 * NOTE: this leaf is a READ-ONLY audit projection — it is not consensus
 * validation; consensus/state roots remain unswappable (the allowlist +
 * check-hotswap-swappable-shape hard line). */
#ifdef ZCL_HOTSWAP_MODULE_GEN
#include "hotswap/hotswap_module.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void module_tramp_utxo_audit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_utxo_audit_body, reply);
}

/* The module's own health hook — runs before the loader publishes it. Kept
 * node-independent (no RPC): a structural OK. */
static bool module_selftest_utxo_audit(char *err, size_t cap)
{
    (void)err;
    (void)cap;
    return true;
}

ZCL_HOTSWAP_MODULE("core.consensus.utxo.audit",
                   module_tramp_utxo_audit,
                   module_selftest_utxo_audit)
#endif /* ZCL_HOTSWAP_MODULE_GEN */
