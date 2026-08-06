/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: native simulation-only ZC23 patronage offer and funding adapters. */
#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_patronage_funding.h"
#include "vcs/zcode_patronage_settlement.h"

#include <stdlib.h>
#include <string.h>

static const char *zpc_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zpc_keys(const struct json_value *input,
                     const char *const *allowed, size_t count)
{
    if (!input || input->type != JSON_OBJ) return false;
    for (size_t i = 0; i < input->num_children; i++) {
        bool known = false;
        for (size_t j = 0; j < count; j++)
            known = known || strcmp(input->keys[i], allowed[j]) == 0;
        if (!known) return false;
    }
    return true;
}

static bool zpc_hex(const struct json_value *input, const char *key,
                    uint8_t *out, size_t out_len)
{
    const char *value = zpc_str(input, key);
    return value && strlen(value) == out_len * 2u &&
           zcl_hex_decode_lower(value, out, out_len);
}

static bool zpc_now(const struct json_value *input, int64_t *out)
{
    const struct json_value *value = input ? json_get(input, "now_unix")
                                           : NULL;
    if (!value || value->type != JSON_INT || json_get_int(value) <= 0)
        return false;
    *out = json_get_int(value);
    return true;
}

static void zpc_fail(struct zcl_command_reply *reply, const char *code,
                     const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.patronage");
}

static void zpc_root(struct json_value *data, const char *key,
                     const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static const char *zpc_mode(uint8_t mode)
{
    switch (mode) {
    case VCS_ZCODE_PATRONAGE_EXACT_TASK_COMMISSION:
        return "exact_task_commission";
    case VCS_ZCODE_PATRONAGE_PACKAGE_CONTINUITY:
        return "package_continuity";
    case VCS_ZCODE_PATRONAGE_DIRECT_GIFT: return "direct_gift";
    default: return "invalid";
    }
}

static void zpc_render_intent(
    struct json_value *data,
    const struct vcs_zcode_patronage_intent_v1 *intent,
    const uint8_t root[32])
{
    (void)json_push_kv_str(data, "object", "patronage_intent");
    zpc_root(data, "patronage_intent_root", root);
    (void)json_push_kv_str(data, "mode", zpc_mode(intent->mode));
    (void)json_push_kv_int(data, "amount_atoms",
                           (int64_t)intent->amount_atoms);
    (void)json_push_kv_int(data, "created_unix", intent->created_unix);
    (void)json_push_kv_int(data, "expires_unix", intent->expires_unix);
    zpc_root(data, "target_root", intent->target_root);
    zpc_root(data, "patron_contributor_binding_root",
             intent->patron_contributor_binding_root);
    zpc_root(data, "intended_recipient_binding_root",
             intent->intended_recipient_binding_root);
    (void)json_push_kv_bool(data, "funded", false);
    (void)json_push_kv_bool(data, "simulation_only", true);
    (void)json_push_kv_bool(data, "no_authority", true);
    (void)json_push_kv_bool(data, "implies_ownership", false);
    (void)json_push_kv_bool(data, "creates_score", false);
    (void)json_push_kv_bool(data, "moves_live_funds", false);
}

static void zpc_render_funding(
    struct json_value *data,
    const struct vcs_zcode_patronage_funding_v1 *funding,
    const uint8_t root[32])
{
    (void)json_push_kv_str(data, "object", "patronage_funding");
    zpc_root(data, "patronage_funding_root", root);
    zpc_root(data, "patronage_intent_root",
             funding->patronage_intent_root);
    zpc_root(data, "simulation_plan_root", funding->simulation_plan_root);
    (void)json_push_kv_int(data, "amount_atoms",
                           (int64_t)funding->amount_atoms);
    (void)json_push_kv_int(data, "created_unix", funding->created_unix);
    (void)json_push_kv_str(data, "funding_status", "fully_simulated");
    (void)json_push_kv_bool(data, "funded", false);
    (void)json_push_kv_bool(data, "simulation_funded", true);
    (void)json_push_kv_bool(data, "has_transaction_bytes", false);
    (void)json_push_kv_bool(data, "moves_live_funds", false);
    (void)json_push_kv_bool(data, "creates_protocol_emission", false);
}

static bool zpc_context(
    const struct json_value *input,
    struct vcs_zcode_patronage_validation_context *context,
    uint8_t network[32])
{
    const char *workspace = zpc_str(input, "workspace");
    int64_t now = 0;
    if (!workspace || !zpc_hex(input, "expected_network_genesis_root",
                                network, 32) || !zpc_now(input, &now))
        return false;
    memset(context, 0, sizeof(*context));
    context->workspace = workspace;
    context->expected_network_genesis_root = network;
    context->now_unix = now;
    return true;
}

static bool zpc_intent_input(
    const struct json_value *input,
    struct vcs_zcode_patronage_intent_v1 *intent,
    uint8_t wire[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES], uint8_t root[32],
    struct vcs_zcode_patronage_validation_context *context,
    uint8_t network[32],
    const char **reason)
{
    if (!zpc_context(input, context, network) ||
        !zpc_hex(input, "intent_hex", wire,
                 VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES)) {
        *reason = "workspace, exact intent_hex, network root and now required";
        return false;
    }
    enum vcs_zcode_patronage_error error =
        vcs_zcode_patronage_intent_parse(
            wire, VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES, intent);
    if (error == VCS_ZCODE_PATRONAGE_OK)
        error = vcs_zcode_patronage_intent_verify_cas(intent, context);
    if (error == VCS_ZCODE_PATRONAGE_OK)
        error = vcs_zcode_patronage_intent_root(intent, root);
    if (error != VCS_ZCODE_PATRONAGE_OK) {
        *reason = vcs_zcode_patronage_error_string(error);
        return false;
    }
    return true;
}

static void zpc_offer(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    bool persist)
{
    static const char *const keys[] = {
        "workspace", "intent_hex", "expected_network_genesis_root",
        "now_unix",
    };
    if (!request || !reply) return;
    if (!zpc_keys(request->input, keys, sizeof(keys) / sizeof(keys[0]))) {
        zpc_fail(reply, "BAD_PATRONAGE_OFFER", "closed input rejected");
        return;
    }
    uint8_t wire[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES], root[32], network[32];
    struct vcs_zcode_patronage_intent_v1 intent;
    struct vcs_zcode_patronage_validation_context context;
    const char *reason = NULL;
    if (!zpc_intent_input(request->input, &intent, wire, root,
                          &context, network, &reason) ||
        (persist && (!vcs_object_store_init(context.workspace) ||
                     !vcs_object_put_addressed(context.workspace, root, wire,
                                               sizeof(wire))))) {
        zpc_fail(reply, "PATRONAGE_OFFER_REFUSED",
                 reason ? reason : "existing workspace CAS write refused");
        return;
    }
    zpc_render_intent(&reply->data, &intent, root);
    (void)json_push_kv_bool(&reply->data, "persisted", persist);
    (void)json_push_kv_str(&reply->data, "validation_authority",
                           "caller_pinned_simulation_context");
}

void zcl_native_handle_zcode_patronage_offer_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zpc_offer(request, reply, false);
}

void zcl_native_handle_zcode_patronage_offer_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zpc_offer(request, reply, true);
}

static bool zpc_funding_input(
    const struct json_value *input,
    struct vcs_zcode_patronage_funding_v1 *funding,
    uint8_t wire[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES], uint8_t root[32],
    struct vcs_zcode_patronage_validation_context *context,
    uint8_t network[32],
    const char **reason)
{
    if (!zpc_context(input, context, network) ||
        !zpc_hex(input, "funding_hex", wire,
                 VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES)) {
        *reason = "workspace, exact funding_hex, network root and now required";
        return false;
    }
    enum vcs_zcode_patronage_funding_error error =
        vcs_zcode_patronage_funding_parse(
            wire, VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES, funding);
    if (error == VCS_ZCODE_PATRONAGE_FUNDING_OK)
        error = vcs_zcode_patronage_funding_verify_cas(funding, context);
    if (error == VCS_ZCODE_PATRONAGE_FUNDING_OK)
        error = vcs_zcode_patronage_funding_root(funding, root);
    if (error != VCS_ZCODE_PATRONAGE_FUNDING_OK) {
        *reason = vcs_zcode_patronage_funding_error_string(error);
        return false;
    }
    return true;
}

static void zpc_fund(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    bool persist)
{
    static const char *const keys[] = {
        "workspace", "funding_hex", "expected_network_genesis_root",
        "now_unix",
    };
    if (!request || !reply) return;
    if (!zpc_keys(request->input, keys, sizeof(keys) / sizeof(keys[0]))) {
        zpc_fail(reply, "BAD_PATRONAGE_FUNDING", "closed input rejected");
        return;
    }
    uint8_t wire[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES], root[32], network[32];
    struct vcs_zcode_patronage_funding_v1 funding;
    struct vcs_zcode_patronage_validation_context context;
    const char *reason = NULL;
    if (!zpc_funding_input(request->input, &funding, wire, root,
                           &context, network, &reason) ||
        (persist && (!vcs_object_store_init(context.workspace) ||
                     !vcs_object_put_addressed(context.workspace, root, wire,
                                               sizeof(wire))))) {
        zpc_fail(reply, "PATRONAGE_FUNDING_REFUSED",
                 reason ? reason : "existing workspace CAS write refused");
        return;
    }
    zpc_render_funding(&reply->data, &funding, root);
    (void)json_push_kv_bool(&reply->data, "persisted", persist);
    (void)json_push_kv_str(&reply->data, "validation_authority",
                           "caller_pinned_simulation_context");
}

void zcl_native_handle_zcode_patronage_fund_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zpc_fund(request, reply, false);
}

void zcl_native_handle_zcode_patronage_fund_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zpc_fund(request, reply, true);
}

void zcl_native_handle_zcode_patronage_show(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {
        "workspace", "root", "expected_network_genesis_root", "now_unix",
    };
    if (!request || !reply) return;
    uint8_t root[32], network[32], derived[32], *wire = NULL;
    size_t wire_len = 0;
    struct vcs_zcode_patronage_validation_context context;
    if (!zpc_keys(request->input, keys, sizeof(keys) / sizeof(keys[0])) ||
        !zpc_hex(request->input, "root", root, 32) ||
        !zpc_context(request->input, &context, network) ||
        vcs_object_load_raw_bounded(context.workspace, root,
            VCS_ZCODE_PATRONAGE_SETTLEMENT_WIRE_BYTES,
            &wire, &wire_len) != 0) {
        free(wire);
        zpc_fail(reply, "PATRONAGE_NOT_FOUND",
                 "exact root absent or closed show input malformed");
        return;
    }
    bool ok = false;
    if (wire_len == VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES) {
        struct vcs_zcode_patronage_intent_v1 intent;
        ok = vcs_zcode_patronage_intent_parse(wire, wire_len, &intent) ==
                VCS_ZCODE_PATRONAGE_OK &&
             vcs_zcode_patronage_intent_verify_cas(&intent, &context) ==
                VCS_ZCODE_PATRONAGE_OK &&
             vcs_zcode_patronage_intent_root(&intent, derived) ==
                VCS_ZCODE_PATRONAGE_OK && memcmp(root, derived, 32) == 0;
        if (ok) zpc_render_intent(&reply->data, &intent, root);
    } else if (wire_len == VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES) {
        struct vcs_zcode_patronage_funding_v1 funding;
        ok = vcs_zcode_patronage_funding_parse(wire, wire_len, &funding) ==
                VCS_ZCODE_PATRONAGE_FUNDING_OK &&
             vcs_zcode_patronage_funding_verify_cas(&funding, &context) ==
                VCS_ZCODE_PATRONAGE_FUNDING_OK &&
             vcs_zcode_patronage_funding_root(&funding, derived) ==
                VCS_ZCODE_PATRONAGE_FUNDING_OK &&
             memcmp(root, derived, 32) == 0;
        if (ok) zpc_render_funding(&reply->data, &funding, root);
    }
    free(wire);
    if (!ok) {
        zpc_fail(reply, "PATRONAGE_CORRUPT",
                 "stored offer or funding did not reverify");
        return;
    }
    (void)json_push_kv_str(&reply->data, "validation_authority",
                           "caller_pinned_simulation_context");
}
