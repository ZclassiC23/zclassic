/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Typed, path-free buyer payment commands for the P2P file market. */

#include "controllers/native_handler_body.h"
#include "controllers/rpc_client.h"
#include "controllers/rpc_params.h"
#include "base/hex.h"
#include "command/native_command.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "util/log_macros.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPN_TAG "native.market.purchase"

struct mpn_code_map {
    const char *code;
    enum zcl_command_status status;
    enum zcl_command_exit exit_code;
};

static const struct mpn_code_map k_codes[] = {
    { "MONEY_STATE_NOT_CURRENT", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_TRANSIENT },
    { "IDEMPOTENCY_CONFLICT", ZCL_COMMAND_STATUS_FAILED,
      ZCL_COMMAND_EXIT_INVALID },
    { "CUSTODY_ALLOCATION_EXCEEDED", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_DENIED },
    { "COMMIT_UNCERTAIN", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_BLOCKED },
    { "MONEY_SNAPSHOT_CHANGED", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_TRANSIENT },
    { "OFFER_CONTRACT_CHANGED", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_BLOCKED },
    { "COMMIT_BUSY", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_TRANSIENT },
    { "COMMIT_STATE_UNCERTAIN", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_BLOCKED },
    { "DESTINATION_INVALID", ZCL_COMMAND_STATUS_FAILED,
      ZCL_COMMAND_EXIT_INVALID },
    { "DOWNLOAD_BINDING_CONFLICT", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_BLOCKED },
    { "DESTINATION_CONFLICT", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_BLOCKED },
    { "MANIFEST_VERIFICATION_FAILED", ZCL_COMMAND_STATUS_FAILED,
      ZCL_COMMAND_EXIT_FAILED },
    { "STAGING_VERIFICATION_FAILED", ZCL_COMMAND_STATUS_FAILED,
      ZCL_COMMAND_EXIT_FAILED },
    { "DELIVERY_NOT_READY", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_TRANSIENT },
    { "ONION_DELIVERY_UNAVAILABLE", ZCL_COMMAND_STATUS_BLOCKED,
      ZCL_COMMAND_EXIT_DENIED },
};

static void mpn_fail(struct zcl_command_reply *reply,
                     enum zcl_command_status status,
                     enum zcl_command_exit exit_code, const char *code,
                     const char *phase, const char *message,
                     const char *evidence, bool mutated)
{
    LOG_ERROR(MPN_TAG, "%s: %s", code, message);
    zcl_command_reply_fail(reply, status, exit_code, code, phase, mutated,
                           false, message, evidence ? evidence : "");
}

static void mpn_merge(struct json_value *dst, const struct json_value *src)
{
    if (!dst || !src || src->type != JSON_OBJ) return;
    for (size_t i = 0; i < src->num_children; i++) {
        const char *key = src->keys ? src->keys[i] : NULL;
        if (!key || !key[0] || strcmp(key, "ok") == 0 ||
            strcmp(key, "code") == 0 || strcmp(key, "message") == 0)
            continue;
        (void)json_push_kv(dst, key, &src->children[i]);
    }
}

static bool mpn_call(struct zcl_command_reply *reply, const char *method,
                     const struct json_value *input, struct json_value *body)
{
    struct rpc_arg_builder args;
    rpc_arg_builder_init(&args);
    rpc_arg_builder_push_value(&args, input);
    char *params = rpc_arg_builder_to_json(&args);
    if (!params) {
        mpn_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "ARG_BUILD_FAILED", "normalize",
                 "could not encode the purchase request", method, false);
        return false;
    }
    zcl_native_bridge_ensure_rpc();
    char *raw = node_rpc_call(method, params);
    free(params);
    if (!raw) {
        mpn_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                 ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE", "dispatch",
                 "the node did not answer the purchase request", method,
                 false);
        return false;
    }
    bool parsed = json_read(body, raw, strlen(raw));
    free(raw);
    if (!parsed || body->type != JSON_OBJ) {
        json_free(body);
        mpn_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
                 "BAD_RPC_BODY", "serialize",
                 "the node returned an invalid purchase document", method,
                 false);
        return false;
    }
    if (json_get_bool_or(body, "ok", false)) return true;

    const char *code = json_get_str(json_get(body, "code"));
    const char *message = json_get_str(json_get(body, "message"));
    enum zcl_command_status status = ZCL_COMMAND_STATUS_FAILED;
    enum zcl_command_exit exit_code = ZCL_COMMAND_EXIT_FAILED;
    for (size_t i = 0; i < sizeof(k_codes) / sizeof(k_codes[0]); i++) {
        if (code && strcmp(code, k_codes[i].code) == 0) {
            status = k_codes[i].status;
            exit_code = k_codes[i].exit_code;
            break;
        }
    }
    char code_copy[64], message_copy[320];
    (void)snprintf(code_copy, sizeof(code_copy), "%s",
                   code && code[0] ? code : "PURCHASE_REFUSED");
    (void)snprintf(message_copy, sizeof(message_copy), "%s",
                   message && message[0] ? message
                                         : "the purchase operation was refused");
    json_free(body);
    mpn_fail(reply, status, exit_code, code_copy, "execute", message_copy,
             method, false);
    return false;
}

void zcl_native_handle_market_purchase_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !request->input || !reply) return;
    const char *scope = json_get_str(json_get(request->input, "wallet_scope"));
    const char *offer = json_get_str(json_get(request->input, "offer_id"));
    const char *source = json_get_str(json_get(request->input,
                                                "source_address"));
    const char *key = json_get_str(json_get(request->input,
                                             "idempotency_key"));
    if (!scope || !offer || !source || !key ||
        !json_get(request->input, "chunk_start") ||
        !json_get(request->input, "chunks_paid")) {
        mpn_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "MISSING_INPUT", "normalize",
                 "wallet_scope, offer_id, source_address, chunk_start, "
                 "chunks_paid, and idempotency_key are required",
                 "app.market.purchase.plan", false);
        return;
    }
    struct json_value body;
    json_init(&body);
    if (!mpn_call(reply, "zmarket_purchase_plan", request->input, &body))
        return;
    mpn_merge(&reply->data, &body);
    const char *plan = json_get_str(json_get(&body, "plan_id"));
    bool replay = json_get_bool_or(&body, "idempotent_replay", false);
    char commit[192];
    (void)snprintf(commit, sizeof(commit),
                   "{\"wallet_scope\":\"%s\",\"plan_id\":\"%s\","
                   "\"confirm\":true}", scope, plan ? plan : "");
    (void)json_push_kv_str(&reply->data, "stage", "plan");
    (void)json_push_kv_bool(&reply->data, "committed", false);
    (void)json_push_kv_bool(&reply->data, "spends_funds", false);
    (void)json_push_kv_str(
        &reply->data, "confirm_hint",
        "pass commit_input to app market purchase commit to pay");
    (void)json_push_kv_str(&reply->data, "commit_input", commit);
    reply->error.mutated = !replay;
    json_free(&body);
}

void zcl_native_handle_market_purchase_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !request->input || !reply) return;
    const char *scope = json_get_str(json_get(request->input, "wallet_scope"));
    const char *plan = json_get_str(json_get(request->input, "plan_id"));
    if (!scope || !plan || !json_get_bool_or(request->input, "confirm", false)) {
        mpn_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "CONFIRM_REQUIRED", "normalize",
                 "wallet_scope, plan_id, and confirm:true are required",
                 "app.market.purchase.commit", false);
        return;
    }
    struct json_value input = {0};
    json_set_object(&input);
    json_push_kv_str(&input, "wallet_scope", scope);
    json_push_kv_str(&input, "plan_id", plan);
    struct json_value body;
    json_init(&body);
    if (!mpn_call(reply, "zmarket_purchase_commit", &input, &body)) {
        json_free(&input);
        return;
    }
    json_free(&input);
    mpn_merge(&reply->data, &body);
    bool replay = json_get_bool_or(&body, "idempotent_replay", false);
    bool queued = json_get_bool_or(&body,
                                    "payment_notification_queued", false);
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_bool(&reply->data, "spends_funds", true);
    reply->error.mutated = !replay || queued;
    json_free(&body);
}

void zcl_native_handle_market_purchase_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !request->input || !reply) return;
    const char *plan = json_get_str(json_get(request->input, "plan_id"));
    if (!plan || !plan[0]) {
        mpn_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "MISSING_PLAN_ID", "normalize", "plan_id is required",
                 "app.market.purchase.status", false);
        return;
    }
    struct json_value input = {0};
    json_set_object(&input);
    json_push_kv_str(&input, "plan_id", plan);
    struct json_value body;
    json_init(&body);
    if (!mpn_call(reply, "zmarket_purchase_status", &input, &body)) {
        json_free(&input);
        return;
    }
    json_free(&input);
    mpn_merge(&reply->data, &body);
    (void)json_push_kv_str(&reply->data, "stage", "status");
    reply->error.mutated = false;
    json_free(&body);
}

void zcl_native_handle_market_purchase_retrieve(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *plan = request && request->input
        ? json_get_str(json_get(request->input, "plan_id")) : NULL;
    const char *destination = request && request->input
        ? json_get_str(json_get(request->input, "destination_path")) : NULL;
    uint8_t parsed[32];
    if (!plan || !destination || !destination[0] ||
        !zcl_hex_decode(plan, parsed, 32)) {
        mpn_fail(reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
                 "MISSING_INPUT", "normalize",
                 "plan_id and private destination_path are required",
                 "app.market.purchase.retrieve", false);
        return;
    }
    struct json_value body;
    json_init(&body);
    if (!mpn_call(reply, "zmarket_purchase_retrieve", request->input,
                  &body))
        return;
    mpn_merge(&reply->data, &body);
    (void)json_push_kv_str(&reply->data, "stage", "retrieved");
    reply->error.mutated = true;
    json_free(&body);
}
