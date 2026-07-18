/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native Tier-1 hot-swap command glue. The activatable machinery lives here
 * and in lib/hotswap:
 *
 *   - `dev.hotswap.probe`  — VERIFY-ONLY, in the CLI's own throwaway process:
 *     dlopen + ABI-validate + self_test of a module .so, NEVER commits. This is
 *     the default dev-loop surface — it proves a swap WOULD work.
 *   - `dev.hotswap.apply`  — forwards to the RESIDENT node's `dev_hotswap_native`
 *     RPC (a separate CLI process cannot re-point the running node's registry).
 *   - `dev_hotswap_native` — the resident RPC that actually performs the swap
 *     IN the running node, gated by hotswap_activation_authorized(): default is
 *     verify-only; a live swap needs BOTH `-hotswap-activate` AND
 *     `ZCL_HOTSWAP_ACTIVATE=1` and the exact dev datadir (canonical refused).
 *
 * The superseded module .so is dlclose'd only after the command-registry
 * override snapshots drain (registry_quiesced_cb -> epoch/refcount quiesce).
 *
 * The ENTIRE executable surface is `#ifdef ZCL_DEV_BUILD`; a release build links
 * only the no-op register_dev_native_hotswap_rpc() stub. */

#define _GNU_SOURCE
#include "command/native_dev_hotswap.h"
#include "command/native_command.h"

#include "hotswap/hotswap.h"
#include "hotswap/hotswap_module.h"
#include "config/command_catalog.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "controllers/rpc_client.h"
#include "rpc/protocol.h"
#include "rpc/server.h"

#include <stdlib.h>
#include <string.h>

#ifdef ZCL_DEV_BUILD

/* The resident node's own datadir, captured at RPC registration (boot) time.
 * rpc_dev_hotswap_native runs INSIDE the node process, where the one-shot
 * RPC-client global (node_rpc_client_datadir) is never initialized — using it
 * here made the dev-datadir self-check fail closed in exactly the process the
 * RPC exists for. Registration already refuses any non-dev datadir, so this
 * stash is always the exact dev datadir (or empty before boot). */
static char g_resident_datadir[512];

/* Publish ONE {handler_name, fn} override into the live command registry. */
static bool registry_commit_cb(void *ctx, const char *handler_name,
                               zcl_hotswap_handler_fn fn, uint32_t *out_gen,
                               char *why, size_t why_sz)
{
    (void)ctx;
    struct zcl_command_handler_override ovr = {
        .path = handler_name, .handler = fn,
    };
    if (!zcl_command_registry_replace_batch(0, &ovr, 1, why, why_sz))
        return false;
    if (out_gen)
        *out_gen = zcl_command_registry_active_generation();
    return true;
}

/* Gate the dlclose of a superseded module .so on override-snapshot drain. */
static bool registry_quiesced_cb(void *ctx)
{
    (void)ctx;
    return zcl_command_registry_all_retired_quiesced();
}

/* Render a hotswap_activate_report into an already-init'd reply. */
static void report_to_reply(struct zcl_command_reply *reply,
                            const struct hotswap_activate_report *report)
{
    json_free(&reply->data);
    json_init(&reply->data);
    json_set_object(&reply->data);
    json_push_kv_str(&reply->data, "schema", "zcl.hotswap_activate.v1");
    json_push_kv_bool(&reply->data, "ok", report->ok);
    json_push_kv_bool(&reply->data, "verify_only", report->verify_only);
    json_push_kv_bool(&reply->data, "activated", report->activated);
    json_push_kv_bool(&reply->data, "rolled_back", report->rolled_back);
    json_push_kv_int(&reply->data, "generation", (int64_t)report->generation);
    json_push_kv_str(&reply->data, "handler", report->handler_name);
    json_push_kv_str(&reply->data, "artifact_sha256", report->artifact_sha256);
    json_push_kv_str(&reply->data, "stage", report->stage);
    if (report->error[0])
        json_push_kv_str(&reply->data, "error", report->error);

    if (report->ok) {
        reply->status = ZCL_COMMAND_STATUS_PASSED;
        reply->exit_code = ZCL_COMMAND_EXIT_OK;
    } else {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "HOTSWAP_REFUSED", report->stage[0] ? report->stage : "activate",
            false, false,
            report->error[0] ? report->error : "hot-swap refused",
            report->handler_name);
    }
}

/* Resident RPC: perform the swap IN this (running node) process.
 * Positional params: [so_path, (activate_bool)]. activate defaults false
 * (verify-only). A true activate is still gated by hotswap_activation_authorized
 * inside hotswap_activate — no flag/env or a canonical datadir => typed refusal. */
static bool rpc_dev_hotswap_native(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "dev_hotswap_native \"/absolute/module.so\" ( activate )");
        return true;
    }
    if (!hotswap_datadir_is_dev(g_resident_datadir)) {
        json_rpc_error_full(result, RPC_FORBIDDEN_BY_SAFE_MODE,
            "native hot-swap available only in the running ~/.zclassic-c23-dev node",
            "dev_hotswap_native");
        return false;
    }
    if (!params || params->type != JSON_ARR || json_size(params) < 1 ||
        json_size(params) > 2) {
        json_rpc_error_full(result, RPC_INVALID_PARAMS,
            "expected [so_path, (activate)]", "dev_hotswap_native");
        return false;
    }
    const struct json_value *path_v = json_at(params, 0);
    const struct json_value *act_v = json_size(params) > 1 ? json_at(params, 1) : NULL;
    if (!path_v || path_v->type != JSON_STR) {
        json_rpc_error_full(result, RPC_INVALID_PARAMS,
            "so_path must be a string", "dev_hotswap_native");
        return false;
    }
    const char *so_path = json_get_str(path_v);
    if (!so_path || so_path[0] != '/') {
        json_rpc_error_full(result, RPC_INVALID_PARAMETER,
            "so_path must be absolute", "dev_hotswap_native");
        return false;
    }
    bool activate = act_v && act_v->type == JSON_BOOL && json_get_bool(act_v);

    struct hotswap_activate_report report;
    hotswap_activate(so_path, g_resident_datadir, activate,
                     registry_commit_cb, registry_quiesced_cb, NULL, &report);

    /* Return the full report either way; the CLI renders ok/verify_only/error. */
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.hotswap_activate.v1");
    json_push_kv_bool(result, "ok", report.ok);
    json_push_kv_bool(result, "verify_only", report.verify_only);
    json_push_kv_bool(result, "activated", report.activated);
    json_push_kv_bool(result, "rolled_back", report.rolled_back);
    json_push_kv_int(result, "generation", (int64_t)report.generation);
    json_push_kv_str(result, "handler", report.handler_name);
    json_push_kv_str(result, "artifact_sha256", report.artifact_sha256);
    json_push_kv_str(result, "stage", report.stage);
    if (report.error[0])
        json_push_kv_str(result, "error", report.error);
    return report.ok;
}

/* CLI `dev hotswap apply`: forward to the resident node's dev_hotswap_native so
 * the RUNNING node performs the gated swap. A separate CLI process cannot
 * re-point the resident registry itself. */
void zcl_native_handle_dev_hotswap_apply(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    /* Non-bridge handler: initialize the one-shot RPC client from the
     * CLI-resolved -datadir/-rpcport before node_rpc_call(). */
    zcl_native_bridge_ensure_rpc();
    const char *so_path = json_get_str(json_get(request->input, "so_path"));
    if (!so_path || so_path[0] != '/') {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
            ZCL_COMMAND_EXIT_INVALID, "HOTSWAP_BAD_INPUT", "validate", false,
            false, "so_path (absolute) is required", "dev.hotswap.apply");
        return;
    }
    /* Build [so_path, true] safely via the JSON writer. */
    struct json_value arr, s, b;
    json_init(&arr);
    json_set_array(&arr);
    json_init(&s);
    json_set_str(&s, so_path);
    json_push_back(&arr, &s);
    json_free(&s);
    json_init(&b);
    json_set_bool(&b, true);
    json_push_back(&arr, &b);
    json_free(&b);
    char params[1024];
    size_t n = json_write(&arr, params, sizeof(params));
    json_free(&arr);
    if (n == 0 || n >= sizeof(params)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
            ZCL_COMMAND_EXIT_INVALID, "HOTSWAP_BAD_INPUT", "serialize", false,
            false, "so_path too long", "dev.hotswap.apply");
        return;
    }

    char *resp = node_rpc_call("dev_hotswap_native", params);
    if (!resp) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
            ZCL_COMMAND_EXIT_TRANSIENT, "HOTSWAP_NO_RESIDENT", "dispatch",
            true, false, "resident node did not respond", "dev.hotswap.apply");
        return;
    }
    struct json_value doc;
    json_init(&doc);
    bool parsed = json_read(&doc, resp, strlen(resp)) && doc.type == JSON_OBJ;
    if (parsed) {
        json_free(&reply->data);
        json_init(&reply->data);
        json_copy(&reply->data, &doc);
        bool ok = json_get_bool(json_get(&doc, "ok"));
        if (ok) {
            reply->status = ZCL_COMMAND_STATUS_PASSED;
            reply->exit_code = ZCL_COMMAND_EXIT_OK;
        } else {
            const char *err = json_get_str(json_get(&doc, "error"));
            const char *stage = json_get_str(json_get(&doc, "stage"));
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                ZCL_COMMAND_EXIT_BLOCKED, "HOTSWAP_REFUSED",
                stage && stage[0] ? stage : "activate", false, false,
                err && err[0] ? err : "resident refused the swap",
                "dev.hotswap.apply");
        }
    } else {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
            ZCL_COMMAND_EXIT_INTERNAL, "HOTSWAP_BAD_RESPONSE", "serialize",
            false, false, "resident returned a non-object response",
            "dev.hotswap.apply");
    }
    json_free(&doc);
    free(resp);
}

/* CLI `dev hotswap probe`: VERIFY-ONLY in this throwaway CLI process. dlopen +
 * ABI + self_test, never commits — the safest way to prove a swap would work. */
void zcl_native_handle_dev_hotswap_probe(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    /* Non-bridge handler: initialize the one-shot RPC client so
     * node_rpc_client_datadir() below returns the CLI-resolved dev datadir. */
    zcl_native_bridge_ensure_rpc();
    const char *so_path = json_get_str(json_get(request->input, "so_path"));
    if (!so_path || so_path[0] != '/') {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
            ZCL_COMMAND_EXIT_INVALID, "HOTSWAP_BAD_INPUT", "validate", false,
            false, "so_path (absolute) is required", "dev.hotswap.probe");
        return;
    }
    struct hotswap_activate_report report;
    hotswap_activate(so_path, node_rpc_client_datadir(), /*request_activate=*/false,
                     NULL, NULL, NULL, &report);
    report_to_reply(reply, &report);
}

bool register_dev_native_hotswap_rpc(struct rpc_table *table,
                                     const char *datadir, int rpc_port)
{
    static const struct rpc_command cmd = {
        "dev", "dev_hotswap_native", rpc_dev_hotswap_native, true,
    };
    /* Only the exact dev lane gets the resident hot-swap RPC; every other lane
     * is a successful no-op. */
    if (!table || rpc_port <= 0 || rpc_port > 65535 ||
        !hotswap_datadir_is_dev(datadir))
        return true;
    (void)snprintf(g_resident_datadir, sizeof(g_resident_datadir), "%s",
                   datadir);
    /* The override commit (zcl_command_registry_replace_batch) requires an
     * active registry in THIS process. The node never dispatches native
     * leaves, so bind the catalog here: replace_batch validates the override
     * against it (READY + EFFECT_READ + canonical) and the slot/quiesce
     * bookkeeping becomes inspectable via dumpstate hotswap. */
    zcl_command_registry_set_active(zcl_command_catalog());
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
