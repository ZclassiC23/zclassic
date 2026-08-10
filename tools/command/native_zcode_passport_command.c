/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: bounded native verification for canonical module_passport.v1. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/zcode_commons_v2.h"

#include <string.h>

static void passport_fail_action(struct zcl_command_reply *reply,
                                 const char *code, const char *phase,
                                 const char *detail, const char *next_action)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, phase, false,
                           false, detail, next_action);
}

static void passport_fail(struct zcl_command_reply *reply, const char *code,
                          const char *detail)
{
    passport_fail_action(reply, code, "verify", detail,
                         "zcode.passport.verify");
}

static void passport_push_root(struct json_value *data, const char *key,
                               const uint8_t root[32]);

static bool passport_parse_roots(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply,
    struct vcs_zcode_module_passport_v1 *passport,
    size_t expected_children, const char *phase)
{
    if (!request || !reply || !passport || !request->input ||
        request->input->type != JSON_OBJ ||
        request->input->num_children != expected_children) {
        if (reply) passport_fail_action(
            reply, "BAD_MODULE_PASSPORT_INPUT", phase,
            "provide exactly the nine evidence roots and signer_pubkey as "
            "canonical lowercase 32-byte hexadecimal values",
            "zcode.passport.plan");
        return false;
    }
    memset(passport, 0, sizeof(*passport));
    passport->schema_version = 1;
    passport->flags = VCS_ZCODE_COMMONS_V2_REQUIRED_FLAGS;
    static const char *keys[] = {
        "stable_api_root", "recipe_root", "toolchain_root", "tests_root",
        "license_root", "semantic_fingerprint_root",
        "workspace_lineage_root", "source_assignment_root",
        "quality_profiles_root", "signer_pubkey",
    };
    uint8_t *roots[] = {
        passport->stable_api_root, passport->recipe_root,
        passport->toolchain_root, passport->tests_root,
        passport->license_root, passport->semantic_fingerprint_root,
        passport->workspace_lineage_root, passport->source_assignment_root,
        passport->quality_profiles_root, passport->signer_root,
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *hex = json_get_str(json_get(request->input, keys[i]));
        if (!hex || strlen(hex) != 64u ||
            !zcl_hex_decode_lower(hex, roots[i], 32)) {
            passport_fail_action(
                reply, "BAD_MODULE_PASSPORT_ROOT", phase,
                "every evidence root and signer_pubkey must be canonical "
                "lowercase 32-byte hexadecimal",
                "zcode.passport.plan");
            memset(passport, 0, sizeof(*passport));
            return false;
        }
    }
    return true;
}

static void passport_render_evidence(
    struct json_value *data,
    const struct vcs_zcode_module_passport_v1 *passport)
{
    passport_push_root(data, "stable_api_root", passport->stable_api_root);
    passport_push_root(data, "recipe_root", passport->recipe_root);
    passport_push_root(data, "toolchain_root", passport->toolchain_root);
    passport_push_root(data, "tests_root", passport->tests_root);
    passport_push_root(data, "license_root", passport->license_root);
    passport_push_root(data, "semantic_fingerprint_root",
                       passport->semantic_fingerprint_root);
    passport_push_root(data, "workspace_lineage_root",
                       passport->workspace_lineage_root);
    passport_push_root(data, "source_assignment_root",
                       passport->source_assignment_root);
    passport_push_root(data, "quality_profiles_root",
                       passport->quality_profiles_root);
    passport_push_root(data, "signer_pubkey", passport->signer_root);
}

void zcl_native_handle_zcode_passport_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_module_passport_v1 passport;
    if (!passport_parse_roots(request, reply, &passport, 10u, "plan")) return;
    uint8_t payload[VCS_ZCODE_MODULE_PASSPORT_V1_SIGNING_PAYLOAD_BYTES];
    size_t payload_len = 0;
    enum vcs_zcode_commons_v2_error error =
        vcs_zcode_module_passport_v1_signing_payload(
            &passport, payload, sizeof(payload), &payload_len);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        passport_fail_action(reply, "MODULE_PASSPORT_PLAN_FAILED", "plan",
                             vcs_zcode_commons_v2_error_string(error),
                             "zcode.passport.plan");
        return;
    }
    char payload_hex[
        VCS_ZCODE_MODULE_PASSPORT_V1_SIGNING_PAYLOAD_BYTES * 2u + 1u];
    zcl_hex_encode(payload, payload_len, payload_hex);
    (void)json_push_kv_bool(&reply->data, "ready_to_sign", true);
    (void)json_push_kv_str(&reply->data, "kind", "module_passport.v1");
    (void)json_push_kv_str(&reply->data, "signing_algorithm", "Ed25519");
    (void)json_push_kv_str(&reply->data, "signing_domain",
                           VCS_ZCODE_MODULE_PASSPORT_V1_SIGNING_DOMAIN);
    (void)json_push_kv_str(&reply->data, "signing_payload", payload_hex);
    (void)json_push_kv_int(&reply->data, "signing_payload_bytes",
                           (int64_t)payload_len);
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_bool(&reply->data, "private_key_accepted", false);
    (void)json_push_kv_bool(&reply->data, "wallet_accessed", false);
    passport_render_evidence(&reply->data, &passport);
    (void)json_push_kv_str(
        &reply->data, "agent_next_action",
        "sign signing_payload with the matching offline Ed25519 key, then "
        "run zcode passport commit with the same roots and signature");
}

void zcl_native_handle_zcode_passport_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_module_passport_v1 passport;
    if (!passport_parse_roots(request, reply, &passport, 11u, "commit"))
        return;
    const char *signature = json_get_str(json_get(request->input, "signature"));
    if (!signature || strlen(signature) != 128u ||
        !zcl_hex_decode_lower(signature, passport.signature, 64)) {
        passport_fail_action(
            reply, "BAD_MODULE_PASSPORT_SIGNATURE", "commit",
            "signature must be one canonical lowercase 64-byte Ed25519 "
            "signature produced over the planned payload",
            "zcode.passport.plan");
        return;
    }
    enum vcs_zcode_commons_v2_error error =
        vcs_zcode_module_passport_v1_verify(&passport);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        passport_fail_action(reply, "MODULE_PASSPORT_SIGNATURE_INVALID",
                             "commit",
                             vcs_zcode_commons_v2_error_string(error),
                             "zcode.passport.plan");
        return;
    }
    uint8_t wire[VCS_ZCODE_MODULE_PASSPORT_V1_WIRE_BYTES], root[32];
    size_t wire_len = 0;
    error = vcs_zcode_module_passport_v1_encode(
        &passport, wire, sizeof(wire), &wire_len);
    if (error == VCS_ZCODE_COMMONS_V2_OK)
        error = vcs_zcode_module_passport_v1_root(&passport, root);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        passport_fail_action(reply, "MODULE_PASSPORT_COMMIT_FAILED", "commit",
                             vcs_zcode_commons_v2_error_string(error),
                             "zcode.passport.plan");
        return;
    }
    char wire_hex[VCS_ZCODE_MODULE_PASSPORT_V1_WIRE_BYTES * 2u + 1u];
    zcl_hex_encode(wire, wire_len, wire_hex);
    (void)json_push_kv_bool(&reply->data, "verified", true);
    (void)json_push_kv_str(&reply->data, "kind", "module_passport.v1");
    (void)json_push_kv_str(&reply->data, "passport", wire_hex);
    passport_push_root(&reply->data, "passport_root", root);
    (void)json_push_kv_bool(&reply->data, "persisted", false);
    (void)json_push_kv_bool(&reply->data, "published", false);
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    passport_render_evidence(&reply->data, &passport);
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           "zcode passport verify --passport=<passport>");
}

static void passport_push_root(struct json_value *data, const char *key,
                               const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

void zcl_native_handle_zcode_passport_verify(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const struct json_value *value = request && request->input
        ? json_get(request->input, "passport") : NULL;
    const char *hex = json_get_str(value);
    if (!request || !reply || !request->input ||
        request->input->type != JSON_OBJ ||
        request->input->num_children != 1 || !hex ||
        strlen(hex) != VCS_ZCODE_MODULE_PASSPORT_V1_WIRE_BYTES * 2u) {
        if (reply) passport_fail(
            reply, "BAD_MODULE_PASSPORT_INPUT",
            "passport must be exactly one canonical 396-byte lowercase hex wire");
        return;
    }

    uint8_t wire[VCS_ZCODE_MODULE_PASSPORT_V1_WIRE_BYTES];
    if (!zcl_hex_decode_lower(hex, wire, sizeof(wire))) {
        passport_fail(reply, "BAD_MODULE_PASSPORT_INPUT",
                      "passport must use canonical lowercase hexadecimal");
        return;
    }
    struct vcs_zcode_module_passport_v1 passport;
    enum vcs_zcode_commons_v2_error error =
        vcs_zcode_module_passport_v1_decode(&passport, wire, sizeof(wire));
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        passport_fail(reply, "MODULE_PASSPORT_INVALID",
                      vcs_zcode_commons_v2_error_string(error));
        return;
    }
    uint8_t root[32];
    error = vcs_zcode_module_passport_v1_root(&passport, root);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        passport_fail(reply, "MODULE_PASSPORT_ROOT_FAILED",
                      vcs_zcode_commons_v2_error_string(error));
        return;
    }

    (void)json_push_kv_bool(&reply->data, "verified", true);
    (void)json_push_kv_str(&reply->data, "kind", "module_passport.v1");
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    passport_push_root(&reply->data, "passport_root", root);
    passport_render_evidence(&reply->data, &passport);
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           "bind this passport root into a workspace manifest");
}
