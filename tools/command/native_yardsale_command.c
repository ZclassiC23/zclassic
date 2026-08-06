/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Native handlers for the `yardsale` root — the wallet-backed
 * seller arm/disarm/status and the wallet-backed buy.
 *
 * THE INVARIANT OF THIS FILE: there is no wallet or ceremony logic here.
 * The wallet, the seller profile, and the pending-buy table all live in
 * the running node's process memory, so every leaf forwards its input
 * object verbatim to the RPC method registered by
 * app/controllers/src/yardsale_wallet_controller.c, which adapts the
 * wallet RPC context onto app/services/src/yardsale_wallet_service*.c —
 * the one place the rules live. The plan/commit gate
 * (ZCL_COMMAND_CONFIRM_PLAN_COMMIT) is enforced node-side by the service:
 * without "confirm":true the answer is an exact expiring plan; with it,
 * the plan commits. */

#include "command/native_command.h"

#include "controllers/native_handler_body.h"
#include "controllers/rpc_client.h"
#include "controllers/rpc_params.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "util/log_macros.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YS_TAG "native.yardsale"

/* Fail the reply with a logged, evidence-carrying error body. Every
 * failure path in this file goes through here, so no leaf can return
 * without saying why. */
static void ys_fail(struct zcl_command_reply *reply,
                    enum zcl_command_status status,
                    enum zcl_command_exit exit_code, const char *code,
                    const char *phase, bool retryable, const char *message,
                    const char *evidence)
{
    LOG_ERROR(YS_TAG, "%s: %s (%s)", code ? code : "ERROR",
              message ? message : "", evidence ? evidence : "");
    zcl_command_reply_fail(reply, status, exit_code, code, phase, retryable,
                           false, message, evidence);
}

/* Forward the leaf's input object to the node's yardsale wallet RPC
 * method and project the node's body into the reply. The service body
 * contract is {ok:bool, ...}: ok:false carries code+message and means
 * nothing was armed, begun, or persisted. */
static void ys_forward(const struct zcl_command_request *request,
                       struct zcl_command_reply *reply,
                       const char *rpc_method, bool input_required)
{
    if (!request || !reply || !rpc_method)
        return;
    const char *leaf = request->spec ? request->spec->path : rpc_method;

    struct json_value empty;
    json_init(&empty);
    json_set_object(&empty);
    const struct json_value *input = request->input;
    if (!input || input->type != JSON_OBJ) {
        if (input_required) {
            ys_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                    ZCL_COMMAND_EXIT_INVALID, "INVALID_INPUT", "normalize",
                    false, "one input object is required", leaf);
            json_free(&empty);
            return;
        }
        input = &empty;
    }

    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_value(&p, input);
    char *params = rpc_arg_builder_to_json(&p);
    json_free(&empty);
    if (!params) {
        ys_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED", "normalize",
                false, "could not encode the RPC parameters", leaf);
        return;
    }

    zcl_native_bridge_ensure_rpc();
    char *result = node_rpc_call(rpc_method, params);
    free(params);
    if (!result) {
        ys_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE", "dispatch",
                true, "the node returned no body for the yardsale RPC",
                rpc_method);
        (void)zcl_command_reply_add_next(reply, "status", "{}",
                                         "confirm the node is running");
        return;
    }

    struct json_value body;
    json_init(&body);
    if (!json_read(&body, result, strlen(result)) || body.type != JSON_OBJ) {
        json_free(&body);
        free(result);
        ys_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                ZCL_COMMAND_EXIT_INTERNAL, "BAD_RPC_BODY", "serialize",
                false, "the yardsale RPC returned an unreadable body",
                rpc_method);
        return;
    }
    free(result);

    if (!json_get_bool_or(&body, "ok", false)) {
        const char *code = json_get_str(json_get(&body, "code"));
        const char *message = json_get_str(json_get(&body, "message"));
        ys_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
                (code && code[0]) ? code : "YARDSALE_REFUSED", "execute",
                false,
                (message && message[0])
                    ? message
                    : "the yardsale wallet service refused the request",
                rpc_method);
        json_free(&body);
        return;
    }

    json_copy(&reply->data, &body);
    reply->error.mutated = json_get_bool_or(&body, "committed", false);
    json_free(&body);
}

void zcl_native_handle_yardsale_seller_arm(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    ys_forward(request, reply, "yardsale_seller_arm", true);
}

void zcl_native_handle_yardsale_seller_disarm(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    ys_forward(request, reply, "yardsale_seller_disarm", false);
}

void zcl_native_handle_yardsale_seller_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    ys_forward(request, reply, "yardsale_seller_status", false);
}

void zcl_native_handle_yardsale_buy(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    ys_forward(request, reply, "yardsale_buy", true);
}
