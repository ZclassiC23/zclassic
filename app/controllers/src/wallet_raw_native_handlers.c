/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Typed raw-transaction composition lane. The node already owns the canonical
 * create/sign/admit RPC implementations; these handlers expose them without
 * copying transaction-building logic into the command layer. */

#include "controllers/wallet_native_handlers.h"

#include "controllers/rpc_params.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *raw_native_string(const struct json_value *body)
{
    if (!body || body->type != JSON_STR) return NULL;
    const char *value = json_get_str(body);
    return value && value[0] ? value : NULL;
}

static void raw_native_plan_token(char out[17], const char *raw_hex,
                                  bool allow_high_fees)
{
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = raw_hex; p && *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= allow_high_fees ? 1u : 0u;
    h *= 1099511628211ULL;
    (void)snprintf(out, 17, "%016llx", (unsigned long long)h);
}

static char *raw_native_create_params(const struct json_value *inputs,
                                      const struct json_value *outputs,
                                      const struct json_value *op_return_hex)
{
    struct rpc_arg_builder args;
    rpc_arg_builder_init(&args);
    rpc_arg_builder_push_value(&args, inputs);
    rpc_arg_builder_push_value(&args, outputs);
    if (op_return_hex)
        rpc_arg_builder_push_value(&args, op_return_hex);
    return rpc_arg_builder_to_json(&args);
}

void zcl_native_handle_wallet_raw_create(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const struct json_value *inputs = json_get(request->input, "inputs");
    const struct json_value *outputs = json_get(request->input, "outputs");
    const struct json_value *op_return_hex =
        json_get(request->input, "op_return_hex");
    if (!inputs || inputs->type != JSON_ARR ||
        !outputs || outputs->type != JSON_OBJ) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "INVALID_RAW_TEMPLATE",
                 "inputs must be an array and outputs must be an object",
                 "core.wallet.transaction.raw.create");
        return;
    }
    if (op_return_hex && op_return_hex->type != JSON_STR) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "INVALID_OP_RETURN_HEX",
                 "op_return_hex must be a hexadecimal string",
                 "op_return_hex");
        return;
    }
    char *params = raw_native_create_params(inputs, outputs, op_return_hex);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode raw transaction template", "create");
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "createrawtransaction", params, &body);
    free(params);
    if (!ok) return;
    const char *raw_hex = raw_native_string(&body);
    if (!raw_hex) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_RAW_TRANSACTION",
                 "createrawtransaction did not return transaction hex",
                 "createrawtransaction");
        return;
    }
    (void)json_push_kv_str(&reply->data, "raw_hex", raw_hex);
    (void)json_push_kv_bool(&reply->data, "created", true);
    (void)json_push_kv_bool(&reply->data, "op_return_included",
                            op_return_hex != NULL);
    json_free(&body);
}

void zcl_native_handle_wallet_raw_sign(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *raw_hex = json_get_str(json_get(request->input, "raw_hex"));
    const struct json_value *prevtxs = json_get(request->input, "prevtxs");
    if (!raw_hex || !raw_hex[0]) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_RAW_TRANSACTION",
                 "raw_hex is required", "core.wallet.transaction.raw.sign");
        return;
    }
    if (prevtxs && prevtxs->type != JSON_ARR) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "INVALID_PREVTXS",
                 "prevtxs must be an array", "prevtxs");
        return;
    }
    struct rpc_arg_builder args;
    rpc_arg_builder_init(&args);
    rpc_arg_builder_push_str(&args, raw_hex);
    if (prevtxs) rpc_arg_builder_push_value(&args, prevtxs);
    char *params = rpc_arg_builder_to_json(&args);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode raw signing request", "sign");
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "signrawtransaction", params, &body);
    free(params);
    if (!ok) return;
    if (body.type != JSON_OBJ) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "BAD_SIGN_RESULT",
                 "signrawtransaction did not return an object",
                 "signrawtransaction");
        return;
    }
    const struct json_value *signed_hex = json_get(&body, "hex");
    const struct json_value *complete = json_get(&body, "complete");
    if (!signed_hex || signed_hex->type != JSON_STR ||
        !complete || complete->type != JSON_BOOL) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "BAD_SIGN_RESULT",
                 "signrawtransaction omitted hex or completion state",
                 "signrawtransaction");
        return;
    }
    (void)json_push_kv_str(&reply->data, "raw_hex", json_get_str(signed_hex));
    (void)json_push_kv_bool(&reply->data, "complete", json_get_bool(complete));
    const struct json_value *errors = json_get(&body, "errors");
    if (errors) (void)json_push_kv(&reply->data, "errors", errors);
    json_free(&body);
}

void zcl_native_handle_wallet_raw_broadcast(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *raw_hex = json_get_str(json_get(request->input, "raw_hex"));
    if (!raw_hex || !raw_hex[0]) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_RAW_TRANSACTION",
                 "raw_hex is required",
                 "core.wallet.transaction.raw.broadcast");
        return;
    }
    bool allow_high_fees =
        json_get_bool_or(request->input, "allow_high_fees", false);
    bool confirm = json_get_bool_or(request->input, "confirm", false);
    char token[17];
    raw_native_plan_token(token, raw_hex, allow_high_fees);
    if (!confirm) {
        (void)json_push_kv_str(&reply->data, "stage", "plan");
        (void)json_push_kv_bool(&reply->data, "committed", false);
        (void)json_push_kv_str(&reply->data, "plan_token", token);
        (void)json_push_kv_str(
            &reply->data, "confirm_hint",
            "re-run with the identical raw_hex and confirm:true");
        (void)json_push_kv_int(&reply->data, "raw_hex_chars",
                               (int64_t)strlen(raw_hex));
        (void)json_push_kv_bool(&reply->data, "allow_high_fees",
                                allow_high_fees);
        return;
    }

    struct rpc_arg_builder args;
    rpc_arg_builder_init(&args);
    rpc_arg_builder_push_str(&args, raw_hex);
    struct json_value high_fees;
    json_init(&high_fees);
    json_set_bool(&high_fees, allow_high_fees);
    rpc_arg_builder_push_value(&args, &high_fees);
    json_free(&high_fees);
    char *params = rpc_arg_builder_to_json(&args);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode raw transaction broadcast", "broadcast");
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "sendrawtransaction", params, &body);
    free(params);
    if (!ok) return;
    const char *txid = raw_native_string(&body);
    if (!txid) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_TXID",
                 "sendrawtransaction did not return a transaction id",
                 "sendrawtransaction");
        return;
    }
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_str(&reply->data, "txid", txid);
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    reply->error.mutated = true;
    json_free(&body);
}
