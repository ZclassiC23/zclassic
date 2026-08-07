/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: typed CLI adapters for durable vault intent RPCs. */

#include "command/native_command.h"
#include "controllers/rpc_params.h"
#include "controllers/vault_intent_controller.h"
#include "controllers/wallet_native_handlers.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "rpc/rpc_timeout.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VNI_TAG "native.vault.intent"

static void vni_fail(struct zcl_command_reply *reply, const char *code,
                     const char *message, const char *evidence)
{
    LOG_ERROR(VNI_TAG, "%s: %s", code, message);
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
        ZCL_COMMAND_EXIT_FAILED, code, "intent", false, false, message,
        evidence ? evidence : "");
}

static bool vni_rpc(const struct zcl_command_request *request,
                    struct zcl_command_reply *reply, const char *method)
{
    struct rpc_arg_builder args;
    rpc_arg_builder_init(&args);
    rpc_arg_builder_push_value(&args, request->input);
    char *params = rpc_arg_builder_to_json(&args);
    if (!params) {
        vni_fail(reply, "ARG_BUILD_FAILED", "could not encode intent input",
                 method);
        return false;
    }
    struct json_value body;
    bool proof_build = strcmp(method, "vault_intent_plan") == 0 ||
                       strcmp(method, "vault_intent_fanout_plan") == 0;
    bool called = proof_build
        ? wnh_call_rpc_deadline(reply, method, params,
                                RPC_PROOF_BUILD_TIMEOUT_MS, &body)
        : wnh_call_rpc(reply, method, params, &body);
    free(params);
    if (!called) return false;
    bool ok = json_get_bool(json_get(&body, "ok"));
    if (!ok) {
        const char *code = json_get_str(json_get(&body, "code"));
        const char *message = json_get_str(json_get(&body, "message"));
        vni_fail(reply, code ? code : "INTENT_FAILED",
                 message ? message : "intent operation failed", method);
        json_free(&body);
        return false;
    }
    json_copy(&reply->data, &body);
    json_free(&body);
    return true;
}

void zcl_native_handle_vault_intent_issue(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *asset = json_get_str(json_get(request->input, "asset"));
    const char *amount = json_get_str(json_get(request->input, "amount"));
    int64_t amount_zat = 0;
    if (!asset) asset = "ZCL";
    if (strcmp(asset, "ZCL") != 0 ||
        !vault_intent_parse_zcl_amount(amount, &amount_zat)) {
        vni_fail(reply, "INVALID_REQUEST",
                 "issue currently requires asset=ZCL and decimal-string amount",
                 "vault.intent.issue");
        return;
    }
    struct json_value body;
    if (!wnh_call_rpc(reply, "getnewaddress", NULL, &body)) return;
    const char *address = body.type == JSON_STR ? json_get_str(&body) : NULL;
    if (!address || !address[0]) {
        json_free(&body);
        vni_fail(reply, "NO_ADDRESS", "wallet did not return a fresh address",
                 "getnewaddress");
        return;
    }
    char uri[256];
    snprintf(uri, sizeof(uri), "zclassic:%s?amount=%s", address, amount);
    json_push_kv_str(&reply->data, "uri", uri);
    json_push_kv_str(&reply->data, "address", address);
    json_push_kv_str(&reply->data, "asset", "ZCL");
    json_push_kv_str(&reply->data, "amount", amount);
    json_push_kv_str(&reply->data, "sender_authentication", "anonymous");
    json_push_kv_str(&reply->data, "request_format", "zclassic-uri");
    reply->error.mutated = true;
    json_free(&body);
}

void zcl_native_handle_vault_intent_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!zcl_native_input_was_stdin()) {
        vni_fail(reply, "STDIN_REQUIRED",
                 "intent effects are accepted only through --input=-",
                 "vault.intent.plan");
        return;
    }
    (void)vni_rpc(request, reply, "vault_intent_plan");
}

void zcl_native_handle_vault_intent_fanout_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (vni_rpc(request, reply, "vault_intent_fanout_plan"))
        reply->error.mutated = !json_get_bool_or(&reply->data,
                                                  "idempotent_plan", false);
}

void zcl_native_handle_vault_intent_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (vni_rpc(request, reply, "vault_intent_commit"))
        reply->error.mutated = !json_get_bool_or(&reply->data,
                                                 "idempotent_replay", false);
}

void zcl_native_handle_vault_intent_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)vni_rpc(request, reply, "vault_intent_status");
}

void zcl_native_handle_vault_intent_list(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)vni_rpc(request, reply, "vault_intent_list");
}
