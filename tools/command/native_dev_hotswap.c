/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native Tier-1 hot-swap command glue (Zero-MCP W1-B/C).
 *
 * The MCP-free successor of the MCP hot-swap surface:
 *   - tools/mcp/controllers/dev_hotswap_controller.c (h_zcl_agent_hotswap,
 *     hotswap_commit_mcp_routes), and
 *   - tools/mcp/dev_rpc_bridge.c (rpc_dev_hotswap, dev_bridge_register_impl),
 * re-homed here so it survives the later MCP deletion. Phase 0 keeps both
 * publication and resident candidate loading contained: dlopen executes ELF
 * constructors before manifest admission, so even a discard-only probe must
 * move to a disposable worker behind pre-load ELF/sidecar policy first.
 *
 * Architecture: `zclassic23-dev dev hotswap apply` runs as a SEPARATE
 * short-lived process, so its CLI handler cannot re-point a leaf in the RUNNING
 * dev node's registry. The retained JSON-RPC (`dev_hotswap_native`) gives old
 * clients a typed refusal without reaching dlopen. Build/test/source checks
 * remain available through the verify-only watcher.
 *
 * The ENTIRE executable surface (CLI handlers and resident RPC)
 * is `#ifdef ZCL_DEV_BUILD`. A release build links only the no-op
 * register_dev_native_hotswap_rpc() stub. */

#define _GNU_SOURCE
#include "command/native_dev_hotswap.h"
#include "command/native_command.h"

#include "hotswap/hotswap.h"
#include "json/json.h"
#include "mcp/rpc_client.h"
#include "rpc/protocol.h"
#include "rpc/server.h"

#ifdef ZCL_DEV_BUILD

/* Positional params: [so_path, probe_leaf, (probe_only)] — the same shape
 * rpc_dev_hotswap uses, extended with an optional probe_only bool. */
static bool rpc_dev_hotswap_native(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "dev_hotswap_native \"/absolute/generation.so\" \"probe_leaf\" "
            "( probe_only )");
        return true;
    }
    /* Resident lane guard: this RPC only ever runs in the dev node
     * ~/.zclassic-c23-dev (the same lane hotswap_load_leaves enforces). */
    if (!hotswap_datadir_is_dev(mcp_rpc_client_datadir())) {
        json_rpc_error_full(result, RPC_FORBIDDEN_BY_SAFE_MODE,
            "native hot-swap available only in the running ~/.zclassic-c23-dev node",
            "dev_hotswap_native");
        return false;
    }
    if (!params || params->type != JSON_ARR || json_size(params) < 2 ||
        json_size(params) > 3) {
        json_rpc_error_full(result, RPC_INVALID_PARAMS,
            "expected [so_path, probe_leaf, (probe_only)]", "dev_hotswap_native");
        return false;
    }
    const struct json_value *path_v = json_at(params, 0);
    const struct json_value *probe_v = json_at(params, 1);
    const struct json_value *only_v =
        json_size(params) > 2 ? json_at(params, 2) : NULL;
    if (!path_v || path_v->type != JSON_STR || !probe_v ||
        probe_v->type != JSON_STR) {
        json_rpc_error_full(result, RPC_INVALID_PARAMS,
            "so_path and probe_leaf must be strings", "dev_hotswap_native");
        return false;
    }
    const char *so_path = json_get_str(path_v);
    const char *probe_leaf = json_get_str(probe_v);
    if (!so_path || so_path[0] != '/') {
        json_rpc_error_full(result, RPC_INVALID_PARAMETER,
            "so_path must be absolute", "dev_hotswap_native");
        return false;
    }
    if (!probe_leaf || !probe_leaf[0]) {
        json_rpc_error_full(result, RPC_INVALID_PARAMETER,
            "probe_leaf is required", "dev_hotswap_native");
        return false;
    }
    bool probe_only = only_v && only_v->type == JSON_BOOL && json_get_bool(only_v);
    json_rpc_error_full(
        result, RPC_FORBIDDEN_BY_SAFE_MODE,
        probe_only
            ? "resident candidate probing is contained: pre-admission dlopen "
              "can execute constructors/destructors; use the build/test-only "
              "verify loop until a disposable worker and ELF policy land"
            : "runtime hot-swap publication is contained until immutable source "
              "epochs, complete proof receipts, resident CAS, and rollback are "
              "one durable transaction",
        "dev_hotswap_native");
    return false;
}

void zcl_native_handle_dev_hotswap_apply(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
        "RUNTIME_PUBLICATION_CONTAINED", "authority", false, false,
        "runtime hot-swap publication is contained until immutable source "
        "epochs, complete proof receipts, resident CAS, and rollback are one "
        "durable transaction",
        "use the verify-only build/test loop; resident probing is also contained");
}

void zcl_native_handle_dev_hotswap_probe(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
        "RESIDENT_PROBE_CONTAINED", "pre_admission", false, false,
        "resident candidate probing is contained because pre-admission dlopen "
        "can execute constructors/destructors before ELF and manifest policy",
        "use the build/test-only verify loop until disposable probe workers land");
}

bool register_dev_native_hotswap_rpc(struct rpc_table *table,
                                     const char *datadir, int rpc_port)
{
    static const struct rpc_command cmd = {
        "dev", "dev_hotswap_native", rpc_dev_hotswap_native, true,
    };
    /* Only the exact dev lane gets the resident hot-swap RPC; every other lane
     * is a successful no-op (mirrors register_dev_mcp_rpc_commands). */
    if (!table || rpc_port <= 0 || rpc_port > 65535 ||
        !hotswap_datadir_is_dev(datadir))
        return true;
    rpc_table_must_append(table, &cmd);
    return true;
}

#else /* !ZCL_DEV_BUILD — release: no resident hot-swap RPC surface */

bool register_dev_native_hotswap_rpc(struct rpc_table *table,
                                     const char *datadir, int rpc_port)
{
    (void)table;
    (void)datadir;
    (void)rpc_port;
    return true;
}

#endif /* ZCL_DEV_BUILD */
