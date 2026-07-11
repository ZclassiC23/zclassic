/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native Tier-1 hot-swap command glue (Zero-MCP W1-B/C).
 *
 * The MCP-free successor of the MCP hot-swap surface:
 *   - tools/mcp/controllers/dev_hotswap_controller.c (h_zcl_agent_hotswap,
 *     hotswap_commit_mcp_routes), and
 *   - tools/mcp/dev_rpc_bridge.c (rpc_dev_hotswap, dev_bridge_register_impl),
 * re-homed here so it survives the later MCP deletion. It never includes
 * tools/mcp/router.h nor enters the MCP router — the loader publishes native
 * command leaves via zcl_command_registry_replace_batch, not MCP routes.
 *
 * Architecture: `zclassic23-dev dev hotswap apply` runs as a SEPARATE
 * short-lived process, so its CLI handler cannot re-point a leaf in the RUNNING
 * dev node's registry. It forwards over JSON-RPC (`dev_hotswap_native`) to the
 * resident node, which runs the actual hotswap_load_leaves() +
 * zcl_command_registry_replace_batch() in-process. `dev.hotswap.probe` forwards
 * the same request with probe_only=true: the loader stages + self-tests the
 * generation but the commit callback returns WITHOUT publishing.
 *
 * The ENTIRE executable surface (CLI handlers, resident RPC, commit callbacks)
 * is `#ifdef ZCL_DEV_BUILD`. A release build links only the no-op
 * register_dev_native_hotswap_rpc() stub. */

#define _GNU_SOURCE
#include "command/native_dev_hotswap.h"
#include "command/native_command.h"

#include "hotswap/hotswap.h"
#include "kernel/command_registry.h"
#include "json/json.h"
#include "mcp/rpc_client.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZCL_DEV_BUILD

/* ── loader commit callbacks ───────────────────────────────────────────
 * The loader stages every native leaf first; these callbacks are invoked once
 * with the whole validated batch. apply publishes it as one immutable override
 * snapshot; probe validates the same batch but leaves the resident snapshot
 * untouched. */
static bool hotswap_commit_native_leaves(
    void *ctx, uint32_t gen,
    const struct zcl_hotswap_leaf_replacement *reps, size_t n,
    char *why, size_t why_sz)
{
    (void)ctx;
    if (n == 0 || n > ZCL_HOTSWAP_GEN_MAX_REPLACED) {
        if (why && why_sz)
            (void)snprintf(why, why_sz, "invalid staged leaf count: %zu", n);
        return false;
    }
    struct zcl_command_handler_override batch[ZCL_HOTSWAP_GEN_MAX_REPLACED];
    for (size_t i = 0; i < n; i++) {
        batch[i].path = reps[i].path;
        batch[i].handler = reps[i].handler;
    }
    /* TODO(W1): isolated precommit probe — look up the probe leaf's spec from
     * the active registry and invoke the candidate handler against an empty
     * request, asserting a non-error/blocked/failed reply BEFORE publish. The
     * generation self_test (loader) plus zcl_command_registry_replace_batch's
     * READY / read-only / non-branch / non-duplicate validation are the v1
     * guards; a dry precommit probe needs a new registry lookup+invoke API and
     * is deferred rather than blocking the batch publish. */
    return zcl_command_registry_replace_batch(gen, batch, n, why, why_sz);
}

/* probe_only: stage + validate the batch shape without publishing. Returning
 * true lets the loader's generation self-test complete and reports success
 * while the resident override snapshot is never touched (no replace_batch). */
static bool hotswap_probe_native_leaves(
    void *ctx, uint32_t gen,
    const struct zcl_hotswap_leaf_replacement *reps, size_t n,
    char *why, size_t why_sz)
{
    (void)ctx;
    (void)gen;
    if (n == 0 || n > ZCL_HOTSWAP_GEN_MAX_REPLACED) {
        if (why && why_sz)
            (void)snprintf(why, why_sz, "invalid staged leaf count: %zu", n);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!reps[i].path || !reps[i].path[0] || !reps[i].handler) {
            if (why && why_sz)
                (void)snprintf(why, why_sz, "staged leaf %zu is malformed", i);
            return false;
        }
    }
    return true;
}

/* ── resident-node RPC: dev_hotswap_native ─────────────────────────────
 * Mirrors h_zcl_agent_hotswap's response object exactly (ok, gen, so_path,
 * error, rejection_stage, provider_id, build_identity, source_identity,
 * input_content_sha256, artifact_sha256, replaced[], replaced_overflow) so the
 * native surface and the retiring MCP surface stay observationally equal. */
static void hotswap_native_fill_result(struct json_value *result, bool ok,
                                       const char *so_path,
                                       const struct hotswap_load_report *rep)
{
    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", ok);
    (void)json_push_kv_int(result, "gen", (int64_t)rep->gen);
    (void)json_push_kv_str(result, "so_path", so_path ? so_path : "");
    if (rep->error[0])
        (void)json_push_kv_str(result, "error", rep->error);
    if (rep->rejection_stage[0])
        (void)json_push_kv_str(result, "rejection_stage", rep->rejection_stage);
    if (rep->provider_id[0])
        (void)json_push_kv_str(result, "provider_id", rep->provider_id);
    if (rep->build_identity[0])
        (void)json_push_kv_str(result, "build_identity", rep->build_identity);
    if (rep->source_identity[0])
        (void)json_push_kv_str(result, "source_identity", rep->source_identity);
    if (rep->input_digest[0])
        (void)json_push_kv_str(result, "input_content_sha256", rep->input_digest);
    if (rep->artifact_sha256[0])
        (void)json_push_kv_str(result, "artifact_sha256", rep->artifact_sha256);

    struct json_value replaced;
    json_init(&replaced);
    json_set_array(&replaced);
    for (size_t i = 0; i < rep->replaced_count; i++) {
        struct json_value item;
        json_init(&item);
        json_set_str(&item, rep->replaced[i]);
        (void)json_push_back(&replaced, &item);
        json_free(&item);
    }
    (void)json_push_kv(result, "replaced", &replaced);
    json_free(&replaced);
    (void)json_push_kv_bool(result, "replaced_overflow", rep->replaced_overflow);
}

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

    struct hotswap_load_report rep = {0};
    bool ok = hotswap_load_leaves(
        so_path, mcp_rpc_client_datadir(), probe_leaf,
        probe_only ? hotswap_probe_native_leaves : hotswap_commit_native_leaves,
        NULL, &rep);

    hotswap_native_fill_result(result, ok, so_path, &rep);
    (void)json_push_kv_bool(result, "probe_only", probe_only);
    return true;
}

/* ── CLI handlers (short-lived process → resident node over RPC) ────────
 * The dev CLI process does not run app_init(), so nothing has initialized the
 * JSON-RPC client (only bridge leaves do, lazily). The native hot-swap lane is
 * fixed to the dev node ~/.zclassic-c23-dev; when the client is not yet
 * initialized, point it at that node so apply/probe reach the resident RPC.
 * TODO(W1): thread the CLI's resolved -datadir/-rpcport (today held in
 * native_command.c's g_bridge_datadir/g_bridge_rpc_port statics) into the
 * request context so this dev-lane default is never consulted. */
static void hotswap_cli_ensure_rpc_client(void)
{
    if (mcp_rpc_client_datadir()[0])
        return;
    const char *home = getenv("HOME");
    char datadir[512];
    (void)snprintf(datadir, sizeof(datadir), "%s/.zclassic-c23-dev",
                   home ? home : "");
    int port = 18252; /* dev-lane convention (Makefile ZCL_AGENT_DEV_RPCPORT) */
    const char *env = getenv("ZCL_AGENT_DEV_RPCPORT");
    if (!env || !env[0])
        env = getenv("ZCL_DEV_RPCPORT");
    if (env && env[0]) {
        int v = atoi(env);
        if (v > 0 && v < 65536)
            port = v;
    }
    mcp_rpc_client_init(datadir, port);
}

/* Build the positional JSON-RPC array [so_path, probe_leaf, (true)]. Caller
 * frees. Returns NULL on allocation failure. */
static char *hotswap_cli_build_params(const char *so_path,
                                      const char *probe_leaf, bool probe_only)
{
    struct json_value arr, item;
    json_init(&arr);
    json_set_array(&arr);
    json_init(&item);
    json_set_str(&item, so_path);
    (void)json_push_back(&arr, &item);
    json_free(&item);
    json_init(&item);
    json_set_str(&item, probe_leaf);
    (void)json_push_back(&arr, &item);
    json_free(&item);
    if (probe_only) {
        json_init(&item);
        json_set_bool(&item, true);
        (void)json_push_back(&arr, &item);
        json_free(&item);
    }
    size_t need = json_write(&arr, NULL, 0);
    char *out = zcl_malloc(need + 1u, "dev_hotswap params");
    if (out)
        json_write(&arr, out, need + 1u);
    json_free(&arr);
    return out;
}

static void hotswap_cli_dispatch(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply,
                                 bool probe_only)
{
    if (!request || !request->input || !reply)
        return;
    const char *so_path = json_get_str(json_get(request->input, "so_path"));
    const char *probe_leaf = json_get_str(json_get(request->input, "probe_leaf"));
    if (!so_path || !so_path[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_SO_PATH",
                               "normalize", false, false,
                               "so_path is required", "so_path");
        return;
    }
    if (so_path[0] != '/') {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "SO_PATH_NOT_ABSOLUTE",
                               "normalize", false, false,
                               "so_path must be an absolute path", so_path);
        return;
    }
    if (!probe_leaf || !probe_leaf[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_PROBE_LEAF",
                               "normalize", false, false,
                               "probe_leaf is required", "probe_leaf");
        return;
    }

    hotswap_cli_ensure_rpc_client();

    char *params = hotswap_cli_build_params(so_path, probe_leaf, probe_only);
    if (!params) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "PARAMS_BUILD_FAILED",
                               "serialize", false, false,
                               "could not serialize hot-swap request", "");
        return;
    }
    char *body = mcp_node_rpc("dev_hotswap_native", params);
    free(params);
    if (!body) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "NODE_UNREACHABLE",
                               "execute", true, false,
                               "resident dev node returned no body", "");
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the dev node is running");
        return;
    }

    struct json_value doc;
    if (!json_read(&doc, body, strlen(body)) || doc.type != JSON_OBJ) {
        json_free(&doc);
        free(body);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_NODE_BODY",
                               "serialize", false, false,
                               "resident node returned a non-object body", "");
        return;
    }
    free(body);

    /* mcp_node_rpc returns two error shapes: a wrapped {"error":{...}} for
     * transport failures (no socket / connect / cookie), and a BARE
     * {"code","message"} for an RPC-layer error (the actor returned false via
     * json_rpc_error_full). A successful hot-swap report always carries "ok";
     * a bare error object never does. Discriminate on both. */
    const struct json_value *err = json_get(&doc, "error");
    const struct json_value *code = json_get(&doc, "code");
    const struct json_value *ok_field = json_get(&doc, "ok");
    if ((err && !json_is_null(err)) || (code && !ok_field)) {
        const char *msg = NULL;
        if (err && err->type == JSON_OBJ)
            msg = json_get_str(json_get(err, "message"));
        else if (err && err->type == JSON_STR)
            msg = json_get_str(err);
        else
            msg = json_get_str(json_get(&doc, "message"));
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "NODE_RPC_ERROR",
                               "execute", true, false,
                               msg && msg[0] ? msg : "resident node RPC error",
                               "dev_hotswap_native");
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the dev node is running");
        json_free(&doc);
        return;
    }

    /* Success envelope: surface the resident report verbatim, then classify. */
    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &doc);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;

    if (!json_get_bool(json_get(&doc, "ok"))) {
        const char *emsg = json_get_str(json_get(&doc, "error"));
        const char *stage = json_get_str(json_get(&doc, "rejection_stage"));
        /* A rejected hot-swap never publishes (replace_batch is all-or-none),
         * so the active override snapshot is unchanged — mutated=false. */
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "HOTSWAP_REJECTED",
                               probe_only ? "probe" : "commit", false, false,
                               emsg && emsg[0] ? emsg
                                   : "native hot-swap was not published",
                               stage ? stage : "");
    }
    json_free(&doc);
}

void zcl_native_handle_dev_hotswap_apply(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    hotswap_cli_dispatch(request, reply, false);
}

void zcl_native_handle_dev_hotswap_probe(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    hotswap_cli_dispatch(request, reply, true);
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
