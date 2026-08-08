/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: native simulation-only ZC23 package-continuity adapters. */
#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_continuity_policy.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *zcc_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zcc_keys(const struct json_value *input,
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

static bool zcc_hex(const struct json_value *input, const char *key,
                    uint8_t *out, size_t out_len)
{
    const char *value = zcc_str(input, key);
    return value && strlen(value) == out_len * 2u &&
           zcl_hex_decode_lower(value, out, out_len);
}

static void zcc_fail(struct zcl_command_reply *reply, const char *code,
                     const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.continuity");
}

static void zcc_root(struct json_value *data, const char *key,
                     const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static void zcc_u64(struct json_value *data, const char *key, uint64_t value)
{
    char decimal[21];
    (void)snprintf(decimal, sizeof(decimal), "%" PRIu64, value);
    (void)json_push_kv_str(data, key, decimal);
}

static bool zcc_context(
    const struct json_value *input,
    struct vcs_zcode_patronage_validation_context *context,
    uint8_t network[32])
{
    const char *workspace = zcc_str(input, "workspace");
    const struct json_value *now = input ? json_get(input, "now_unix") : NULL;
    if (!workspace ||
        !zcl_native_zcode_workspace_is_explicit_scratch(workspace) ||
        !zcc_hex(input, "expected_network_genesis_root",
                                network, 32) || !now ||
        now->type != JSON_INT || json_get_int(now) <= 0)
        return false;
    memset(context, 0, sizeof(*context));
    context->workspace = workspace;
    context->expected_network_genesis_root = network;
    context->now_unix = json_get_int(now);
    return true;
}

static void zcc_render(
    struct json_value *data,
    const struct vcs_zcode_continuity_policy_v1 *policy,
    const uint8_t root[32])
{
    (void)json_push_kv_str(data, "object", "continuity_policy");
    zcc_root(data, "continuity_policy_root", root);
    zcc_root(data, "package_root", policy->package_root);
    zcc_root(data, "current_release_root", policy->current_release_root);
    zcc_root(data, "from_capsule_root", policy->from_capsule_root);
    zcc_root(data, "to_capsule_root", policy->to_capsule_root);
    zcc_root(data, "proof_policy_root", policy->proof_policy_root);
    zcc_root(data, "patron_contributor_binding_root",
             policy->patron_contributor_binding_root);
    (void)json_push_kv_int(data, "event_mask", policy->event_mask);
    (void)json_push_kv_int(data, "maximum_cycles", policy->maximum_cycles);
    zcc_u64(data, "per_cycle_cap_atoms", policy->per_cycle_cap_atoms);
    zcc_u64(data, "total_cap_atoms", policy->total_cap_atoms);
    (void)json_push_kv_int(data, "created_unix", policy->created_unix);
    (void)json_push_kv_int(data, "expires_unix", policy->expires_unix);
    zcc_u64(data, "sequence", policy->sequence);
    (void)json_push_kv_bool(data, "born_red_fix_allowed",
        (policy->event_mask & VCS_ZCODE_CONTINUITY_BORN_RED_FIX) != 0);
    (void)json_push_kv_bool(data, "security_fix_allowed",
        (policy->event_mask & VCS_ZCODE_CONTINUITY_SECURITY_FIX) != 0);
    (void)json_push_kv_bool(data, "independent_reproduction_allowed",
        (policy->event_mask &
         VCS_ZCODE_CONTINUITY_INDEPENDENT_REPRODUCTION) != 0);
    (void)json_push_kv_bool(data, "compatibility_allowed",
        (policy->event_mask & VCS_ZCODE_CONTINUITY_COMPATIBILITY) != 0);
    (void)json_push_kv_bool(data, "preservation_allowed",
        (policy->event_mask & VCS_ZCODE_CONTINUITY_PRESERVATION) != 0);
    (void)json_push_kv_bool(data, "funded", false);
    (void)json_push_kv_bool(data, "guaranteed_income", false);
    (void)json_push_kv_bool(data, "simulation_only", true);
    (void)json_push_kv_bool(data, "no_authority", true);
    (void)json_push_kv_bool(data, "moves_live_funds", false);
    (void)json_push_kv_bool(data, "creates_score", false);
    (void)json_push_kv_bool(data, "creates_protocol_emission", false);
    (void)json_push_kv_str(data, "capsule_authority",
        "policy_commitments_require_event_evidence");
    (void)json_push_kv_str(data, "validation_authority",
        "caller_pinned_simulation_context");
}

static bool zcc_parse_verify(
    const struct json_value *input,
    struct vcs_zcode_continuity_policy_v1 *policy,
    uint8_t wire[VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES], uint8_t root[32],
    struct vcs_zcode_patronage_validation_context *context,
    uint8_t network[32], const char **reason)
{
    if (!zcc_context(input, context, network) ||
        !zcc_hex(input, "policy_hex", wire,
                 VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES)) {
        *reason = "workspace, exact policy_hex, network root and now required";
        return false;
    }
    enum vcs_zcode_continuity_error error =
        vcs_zcode_continuity_policy_parse(
            wire, VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES, policy);
    if (error == VCS_ZCODE_CONTINUITY_OK)
        error = vcs_zcode_continuity_policy_verify_cas(policy, context);
    if (error == VCS_ZCODE_CONTINUITY_OK)
        error = vcs_zcode_continuity_policy_root(policy, root);
    if (error != VCS_ZCODE_CONTINUITY_OK) {
        *reason = vcs_zcode_continuity_error_string(error);
        return false;
    }
    return true;
}

static void zcc_plan_or_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    bool persist)
{
    static const char *const keys[] = {
        "workspace", "policy_hex", "expected_network_genesis_root",
        "now_unix",
    };
    if (!request || !reply) return;
    if (!zcc_keys(request->input, keys, sizeof(keys) / sizeof(keys[0]))) {
        zcc_fail(reply, "BAD_CONTINUITY_POLICY", "closed input rejected");
        return;
    }
    uint8_t wire[VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES], root[32], network[32];
    struct vcs_zcode_continuity_policy_v1 policy;
    struct vcs_zcode_patronage_validation_context context;
    const char *reason = NULL;
    if (!zcc_parse_verify(request->input, &policy, wire, root, &context,
                          network, &reason) ||
        (persist && (!vcs_object_store_init(context.workspace) ||
                     !vcs_object_put_addressed(context.workspace, root, wire,
                                               sizeof(wire))))) {
        zcc_fail(reply, "CONTINUITY_POLICY_REFUSED",
                 reason ? reason : "existing workspace CAS write refused");
        return;
    }
    zcc_render(&reply->data, &policy, root);
    (void)json_push_kv_bool(&reply->data, "persisted", persist);
}

void zcl_native_handle_zcode_continuity_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcc_plan_or_commit(request, reply, false);
}

void zcl_native_handle_zcode_continuity_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcc_plan_or_commit(request, reply, true);
}

void zcl_native_handle_zcode_continuity_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {
        "workspace", "root", "expected_network_genesis_root", "now_unix",
    };
    if (!request || !reply) return;
    uint8_t root[32], derived[32], network[32], *wire = NULL;
    size_t wire_len = 0;
    struct vcs_zcode_patronage_validation_context context;
    struct vcs_zcode_continuity_policy_v1 policy;
    bool ok = zcc_keys(request->input, keys,
                       sizeof(keys) / sizeof(keys[0])) &&
        zcc_hex(request->input, "root", root, 32) &&
        zcc_context(request->input, &context, network) &&
        vcs_object_load_raw_bounded(
            context.workspace, root, VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES,
            &wire, &wire_len) == 0 &&
        vcs_zcode_continuity_policy_parse(wire, wire_len, &policy) ==
            VCS_ZCODE_CONTINUITY_OK &&
        vcs_zcode_continuity_policy_verify_cas(&policy, &context) ==
            VCS_ZCODE_CONTINUITY_OK &&
        vcs_zcode_continuity_policy_root(&policy, derived) ==
            VCS_ZCODE_CONTINUITY_OK && memcmp(root, derived, 32) == 0;
    free(wire);
    if (!ok) {
        zcc_fail(reply, "CONTINUITY_POLICY_NOT_VERIFIED",
                 "exact root absent, input malformed, or CAS authority failed");
        return;
    }
    zcc_render(&reply->data, &policy, root);
    (void)json_push_kv_bool(&reply->data, "persisted", true);
}
