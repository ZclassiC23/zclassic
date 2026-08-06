/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Owner-only typed adapter for private paid-file content registration. */

#include "controllers/rpc_client.h"
#include "controllers/rpc_params.h"
#include "command/native_command.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCN_TAG "native.market.content"

static void market_content_native_fail(
    struct zcl_command_reply *reply, enum zcl_command_status status,
    enum zcl_command_exit exit_code, const char *code, const char *phase,
    const char *message)
{
    LOG_ERROR(MCN_TAG, "%s: %s", code, message);
    zcl_command_reply_fail(reply, status, exit_code, code, phase, false, false,
                           message, "zmarket_content_register");
}

void zcl_native_handle_market_content_register(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !request->spec || !request->input || !reply)
        return;
    const char *offer_id =
        json_get_str(json_get(request->input, "offer_id"));
    const char *content_path =
        json_get_str(json_get(request->input, "content_path"));
    if (!offer_id || !offer_id[0] || !content_path || !content_path[0]) {
        market_content_native_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "MISSING_INPUT", "normalize",
            "offer_id and content_path are required");
        return;
    }

    struct rpc_arg_builder params;
    rpc_arg_builder_init(&params);
    rpc_arg_builder_push_str(&params, offer_id);
    rpc_arg_builder_push_str(&params, content_path);
    char *params_json = rpc_arg_builder_to_json(&params);
    if (!params_json) {
        market_content_native_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
            "ARG_BUILD_FAILED", "normalize",
            "could not encode the registration request");
        return;
    }

    zcl_native_bridge_ensure_rpc();
    char *raw = node_rpc_call("zmarket_content_register", params_json);
    free(params_json);
    if (!raw) {
        market_content_native_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_TRANSIENT,
            "NODE_UNAVAILABLE", "dispatch",
            "the node did not answer the content registration request");
        return;
    }

    struct json_value body;
    bool parsed = json_read(&body, raw, strlen(raw));
    free(raw);
    if (!parsed) {
        json_free(&body);
        market_content_native_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
            "BAD_RPC_BODY", "serialize",
            "content registration returned an unparseable body");
        return;
    }
    if (body.type == JSON_STR) {
        const char *reason = json_get_str(&body);
        char message[256];
        snprintf(message, sizeof(message), "%s",
                 reason && reason[0] ? reason
                                     : "content registration was refused");
        json_free(&body);
        market_content_native_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_FAILED,
            "MARKET_CONTENT_REFUSED", "execute", message);
        return;
    }

    const char *status = json_get_str(json_get(&body, "status"));
    const char *saved_offer = json_get_str(json_get(&body, "offer_id"));
    const char *root_hash = json_get_str(json_get(&body, "root_hash"));
    const struct json_value *size = json_get(&body, "size_bytes");
    const struct json_value *chunks = json_get(&body, "num_chunks");
    const struct json_value *registered = json_get(&body, "registered_at");
    if (body.type != JSON_OBJ || !status ||
        strcmp(status, "registered") != 0 || !saved_offer || !root_hash ||
        !size || size->type != JSON_INT || !chunks || chunks->type != JSON_INT ||
        !registered || registered->type != JSON_INT) {
        json_free(&body);
        market_content_native_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "MARKET_CONTENT_NOT_REGISTERED", "execute",
            "the node did not return a complete registration receipt");
        return;
    }

    /* Whitelist fields so a future RPC addition cannot accidentally echo the
     * private path through the native/agent result. */
    (void)json_push_kv_str(&reply->data, "schema", "zcl.market_content.v1");
    (void)json_push_kv_str(&reply->data, "status", status);
    (void)json_push_kv_str(&reply->data, "offer_id", saved_offer);
    (void)json_push_kv_str(&reply->data, "root_hash", root_hash);
    (void)json_push_kv_int(&reply->data, "size_bytes", json_get_int(size));
    (void)json_push_kv_int(&reply->data, "num_chunks", json_get_int(chunks));
    (void)json_push_kv_int(&reply->data, "registered_at",
                           json_get_int(registered));
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    json_free(&body);
    reply->error.mutated = true;
}
