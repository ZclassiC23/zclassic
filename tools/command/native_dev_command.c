/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Release-safe handlers for the six READY read-only `dev` leaves
 * (docs/NATIVE_COMMAND_INTERFACE.md §7). These bind the registry catalog's dev
 * subtree to the checkout-local read-only producers: App manifest describe /
 * plan / simulate, source-change classification, the Core/App boundary law, and
 * the latest native cycle verdict.
 *
 * These handlers perform NO process spawn, NO checkout mutation, and NO node
 * contact, so they compile into the release binary. Every *mutating* dev leaf
 * (change.apply, loop.*, generation.rollback, app.publish/scaffold) is COMPAT or
 * PLANNED with a NULL handler and is served — in a dev build only — by the
 * dev-only devloop dispatcher (tools/dev/devloop_cli.c). */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "devloop.h"
#include "kernel/command_registry.h"
#include "json/json.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy a produced JSON document (buffer producer output) into reply->data.
 * On any failure, fail the reply with an INTERNAL contract error. */
static void dev_reply_from_json(struct zcl_command_reply *reply,
                                const char *body, size_t n, const char *what)
{
    struct json_value doc;
    if (n == 0 || !json_read(&doc, body, n) || doc.type != JSON_OBJ) {
        json_free(&doc);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "DEV_RENDER_FAILED",
                               "serialize", false, false,
                               "read-only dev producer returned no document",
                               what ? what : "");
        return;
    }
    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &doc);
    json_free(&doc);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

static const char *dev_source_root(const struct zcl_command_request *request)
{
    if (request && request->context && request->context->source_root &&
        request->context->source_root[0])
        return request->context->source_root;
    const char *env = getenv("ZCL_DEV_SOURCE_ROOT");
    return env && env[0] ? env : ".";
}

/* ── dev.status ────────────────────────────────────────────────────────── */
void zcl_native_handle_dev_status(const struct zcl_command_request *request,
                                  struct zcl_command_reply *reply)
{
    (void)request;
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (home && home[0] &&
        snprintf(path, sizeof(path),
                 "%s/.local/state/zclassic23-dev/native-cycle.json", home) > 0) {
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[16384];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            struct json_value doc;
            if (n > 0 && json_read(&doc, buf, n) && doc.type == JSON_OBJ) {
                json_free(&reply->data);
                json_init(&reply->data);
                json_copy(&reply->data, &doc);
                json_free(&doc);
                return;
            }
            json_free(&doc);
        }
    }
    /* No durable verdict yet — a bounded, honest "unavailable" is passing. */
    (void)json_push_kv_str(&reply->data, "schema", "zcl.dev_cycle.v1");
    (void)json_push_kv_str(&reply->data, "status", "unavailable");
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           "keep editing; the native watcher records verdicts");
}

/* ── dev.core.boundary ─────────────────────────────────────────────────── */
void zcl_native_handle_dev_core_boundary(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    static const char *const core[] = {
        "consensus", "validation", "chain_mutation", "wallet_keys",
        "raw_storage", "sockets", "boot"
    };
    static const char *const apps[] = {
        "resources", "signed_events", "services", "projections", "web",
        "onion", "znam", "p2p_topics"
    };
    (void)json_push_kv_str(&reply->data, "schema", "zcl.core_app_boundary.v1");
    (void)json_push_kv_str(&reply->data, "rule",
                           "core_owns_truth_apps_consume_capabilities");
    struct json_value core_arr, app_arr;
    json_init(&core_arr);
    json_init(&app_arr);
    json_set_array(&core_arr);
    json_set_array(&app_arr);
    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++) {
        struct json_value it;
        json_init(&it);
        json_set_str(&it, core[i]);
        (void)json_push_back(&core_arr, &it);
        json_free(&it);
    }
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        struct json_value it;
        json_init(&it);
        json_set_str(&it, apps[i]);
        (void)json_push_back(&app_arr, &it);
        json_free(&it);
    }
    (void)json_push_kv(&reply->data, "core", &core_arr);
    (void)json_push_kv(&reply->data, "apps", &app_arr);
    (void)json_push_kv_str(&reply->data, "core_change", "guarded_reload");
    (void)json_push_kv_str(&reply->data, "app_change",
                           "simulate_then_atomic_publish");
    json_free(&core_arr);
    json_free(&app_arr);
}

/* ── dev.app.describe ──────────────────────────────────────────────────── */
void zcl_native_handle_dev_app_describe(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *app_id = json_get_str(json_get(request->input, "app_id"));
    if (!app_id || !app_id[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_APP_ID",
                               "normalize", false, false,
                               "app_id is required", "");
        return;
    }
    char body[8192];
    size_t n = zcl_devloop_app_describe_json(dev_source_root(request), app_id,
                                             body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_APP",
                               "resolve", false, false,
                               "unknown App or checkout root", app_id);
        return;
    }
    dev_reply_from_json(reply, body, n, app_id);
}

/* ── dev.app.plan ──────────────────────────────────────────────────────── */
void zcl_native_handle_dev_app_plan(const struct zcl_command_request *request,
                                    struct zcl_command_reply *reply)
{
    const char *app_id = json_get_str(json_get(request->input, "app_id"));
    const char *resource = json_get_str(json_get(request->input, "resource"));
    if (!app_id || !app_id[0] || !resource || !resource[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_ARGS",
                               "normalize", false, false,
                               "app_id and resource are required", "");
        return;
    }
    char body[4096];
    size_t n = zcl_devloop_app_plan_json(dev_source_root(request), app_id,
                                         resource, body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_ARGS",
                               "resolve", false, false,
                               "invalid App, resource, or checkout root",
                               app_id);
        return;
    }
    dev_reply_from_json(reply, body, n, app_id);
}

/* ── dev.app.simulate ──────────────────────────────────────────────────── */
void zcl_native_handle_dev_app_simulate(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *app_id = json_get_str(json_get(request->input, "app_id"));
    if (!app_id || !app_id[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_APP_ID",
                               "normalize", false, false,
                               "app_id is required", "");
        return;
    }
    uint64_t seed = UINT64_C(0x534f4349414c0001);
    const struct json_value *seed_v = json_get(request->input, "seed");
    if (seed_v && !json_is_null(seed_v)) {
        if (seed_v->type == JSON_STR) {
            char *end = NULL;
            seed = strtoull(json_get_str(seed_v), &end, 0);
            if (!end || *end)
                seed = 0;
        } else {
            seed = (uint64_t)json_get_int(seed_v);
        }
    }
    char body[4096];
    size_t n = zcl_devloop_app_simulate_json(app_id, seed, body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_SIM",
                               "resolve", false, false,
                               "unknown App or invalid seed", app_id);
        return;
    }
    dev_reply_from_json(reply, body, n, app_id);
}

/* ── dev.change.plan ───────────────────────────────────────────────────── */
void zcl_native_handle_dev_change_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const struct json_value *files = json_get(request->input, "files");
    const char *file_ptrs[ZCL_DEVLOOP_MAX_FILES];
    size_t count = 0;
    if (files && files->type == JSON_ARR) {
        for (size_t i = 0; i < files->num_children &&
                           count < ZCL_DEVLOOP_MAX_FILES; i++) {
            const char *f = json_get_str(&files->children[i]);
            if (f && f[0])
                file_ptrs[count++] = f;
        }
    }
    char body[16384];
    size_t n = zcl_devloop_plan_json(file_ptrs, count, body, sizeof(body));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_FILE_SET",
                               "normalize", false, false,
                               "invalid or oversized file set", "");
        return;
    }
    dev_reply_from_json(reply, body, n, "change.plan");
}
