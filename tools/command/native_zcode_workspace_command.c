/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: verify exact Passport-to-workspace entry bindings without writes. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/zcode_commons_v2.h"

#include <string.h>

static void workspace_binding_fail(struct zcl_command_reply *reply,
                                   const char *code, const char *phase,
                                   const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, phase, false,
                           false, detail, "zcode.workspace.plan");
}

static bool workspace_decode_root(const struct json_value *input,
                                  const char *key, uint8_t out[32])
{
    const char *hex = json_get_str(json_get(input, key));
    return hex && strlen(hex) == 64u && zcl_hex_decode_lower(hex, out, 32);
}

static bool workspace_binding_parse(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply,
    struct vcs_zcode_module_passport_v1 *passport,
    struct vcs_zcode_workspace_entry_v1 *entry,
    bool require_binding, uint8_t binding_root[32], const char *phase)
{
    const size_t min_fields = require_binding ? 4u : 3u;
    if (!request || !reply || !passport || !entry || !binding_root ||
        !request->input || request->input->type != JSON_OBJ ||
        request->input->num_children < min_fields ||
        request->input->num_children > min_fields + 1u) {
        if (reply) workspace_binding_fail(
            reply, "BAD_WORKSPACE_BINDING_INPUT", phase,
            "provide passport, module_release_root and positive sequence; "
            "sequence above one also requires predecessor_release_root");
        return false;
    }
    const char *passport_hex = json_get_str(json_get(request->input,
                                                       "passport"));
    const struct json_value *sequence_value = json_get(request->input,
                                                        "sequence");
    if (!passport_hex ||
        strlen(passport_hex) != VCS_ZCODE_MODULE_PASSPORT_V1_WIRE_BYTES * 2u ||
        !sequence_value || sequence_value->type != JSON_INT ||
        sequence_value->val.i <= 0) {
        workspace_binding_fail(
            reply, "BAD_WORKSPACE_BINDING_INPUT", phase,
            "passport must be canonical 396-byte lowercase hex and sequence "
            "must be a positive integer");
        return false;
    }
    uint8_t passport_wire[VCS_ZCODE_MODULE_PASSPORT_V1_WIRE_BYTES];
    if (!zcl_hex_decode_lower(passport_hex, passport_wire,
                              sizeof(passport_wire)) ||
        vcs_zcode_module_passport_v1_decode(
            passport, passport_wire, sizeof(passport_wire)) !=
            VCS_ZCODE_COMMONS_V2_OK) {
        workspace_binding_fail(reply, "WORKSPACE_PASSPORT_INVALID", phase,
                               "Passport decoding or Ed25519 verification failed");
        return false;
    }

    memset(entry, 0, sizeof(*entry));
    entry->sequence = (uint64_t)sequence_value->val.i;
    if (!workspace_decode_root(request->input, "module_release_root",
                               entry->module_release_root)) {
        workspace_binding_fail(
            reply, "WORKSPACE_RELEASE_ROOT_INVALID", phase,
            "module_release_root must be canonical lowercase 32-byte hex");
        return false;
    }
    const struct json_value *predecessor = json_get(
        request->input, "predecessor_release_root");
    if (entry->sequence > 1u) {
        if (!predecessor ||
            !workspace_decode_root(request->input, "predecessor_release_root",
                                   entry->predecessor_release_root)) {
            workspace_binding_fail(
                reply, "WORKSPACE_PREDECESSOR_REQUIRED", phase,
                "sequence above one requires one exact predecessor release root");
            return false;
        }
    } else if (predecessor) {
        workspace_binding_fail(
            reply, "WORKSPACE_PREDECESSOR_FORBIDDEN", phase,
            "sequence one must not declare a predecessor release root");
        return false;
    }
    enum vcs_zcode_commons_v2_error error =
        vcs_zcode_module_passport_v1_root(
            passport, entry->module_passport_root);
    memcpy(entry->semantic_fingerprint_root,
           passport->semantic_fingerprint_root, 32);
    memcpy(entry->source_assignment_root,
           passport->source_assignment_root, 32);
    if (error == VCS_ZCODE_COMMONS_V2_OK)
        error = vcs_zcode_workspace_entry_v1_root(entry, binding_root);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        workspace_binding_fail(reply, "WORKSPACE_BINDING_INVALID", phase,
                               vcs_zcode_commons_v2_error_string(error));
        return false;
    }
    if (require_binding) {
        uint8_t expected[32];
        if (!workspace_decode_root(request->input, "binding_root", expected) ||
            memcmp(expected, binding_root, 32) != 0) {
            workspace_binding_fail(
                reply, "WORKSPACE_BINDING_ROOT_MISMATCH", phase,
                "binding_root does not match the Passport and workspace entry");
            return false;
        }
    }
    return true;
}

static void workspace_push_root(struct json_value *data, const char *key,
                                const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static void workspace_binding_render(
    struct zcl_command_reply *reply,
    const struct vcs_zcode_workspace_entry_v1 *entry,
    const uint8_t binding_root[32], bool verified)
{
    (void)json_push_kv_bool(&reply->data, "verified_passport", true);
    (void)json_push_kv_bool(&reply->data,
                           verified ? "binding_verified" : "ready_to_bind",
                           true);
    (void)json_push_kv_str(&reply->data, "kind", "workspace_entry.v1");
    workspace_push_root(&reply->data, "binding_root", binding_root);
    workspace_push_root(&reply->data, "module_release_root",
                        entry->module_release_root);
    workspace_push_root(&reply->data, "module_passport_root",
                        entry->module_passport_root);
    workspace_push_root(&reply->data, "passport_root",
                        entry->module_passport_root);
    workspace_push_root(&reply->data, "semantic_fingerprint_root",
                        entry->semantic_fingerprint_root);
    workspace_push_root(&reply->data, "source_assignment_root",
                        entry->source_assignment_root);
    workspace_push_root(&reply->data, "predecessor_release_root",
                        entry->predecessor_release_root);
    (void)json_push_kv_int(&reply->data, "sequence", (int64_t)entry->sequence);
    (void)json_push_kv_bool(&reply->data, "persisted", false);
    (void)json_push_kv_bool(&reply->data, "published", false);
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_str(
        &reply->data, "agent_next_action",
        verified ? "include this exact entry in a signed workspace manifest"
                 : "zcode workspace verify --input='<plan input plus binding_root>'");
}

void zcl_native_handle_zcode_workspace_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_module_passport_v1 passport;
    struct vcs_zcode_workspace_entry_v1 entry;
    uint8_t binding_root[32];
    if (!workspace_binding_parse(request, reply, &passport, &entry, false,
                                 binding_root, "plan"))
        return;
    workspace_binding_render(reply, &entry, binding_root, false);
}

void zcl_native_handle_zcode_workspace_verify(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_module_passport_v1 passport;
    struct vcs_zcode_workspace_entry_v1 entry;
    uint8_t binding_root[32];
    if (!workspace_binding_parse(request, reply, &passport, &entry, true,
                                 binding_root, "verify"))
        return;
    workspace_binding_render(reply, &entry, binding_root, true);
}
