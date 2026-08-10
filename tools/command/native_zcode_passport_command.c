/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: bounded native verification for canonical module_passport.v1. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/zcode_commons_v2.h"

#include <string.h>

static void passport_fail(struct zcl_command_reply *reply, const char *code,
                          const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "verify", false,
                           false, detail, "zcode.passport.verify");
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
    passport_push_root(&reply->data, "stable_api_root",
                       passport.stable_api_root);
    passport_push_root(&reply->data, "recipe_root", passport.recipe_root);
    passport_push_root(&reply->data, "toolchain_root",
                       passport.toolchain_root);
    passport_push_root(&reply->data, "tests_root", passport.tests_root);
    passport_push_root(&reply->data, "license_root", passport.license_root);
    passport_push_root(&reply->data, "semantic_fingerprint_root",
                       passport.semantic_fingerprint_root);
    passport_push_root(&reply->data, "workspace_lineage_root",
                       passport.workspace_lineage_root);
    passport_push_root(&reply->data, "source_assignment_root",
                       passport.source_assignment_root);
    passport_push_root(&reply->data, "quality_profiles_root",
                       passport.quality_profiles_root);
    passport_push_root(&reply->data, "signer_pubkey", passport.signer_root);
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           "bind this passport root into a workspace manifest");
}
