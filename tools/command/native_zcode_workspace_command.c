/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: verify exact Passport-to-workspace entry bindings without writes. */

#include "command/native_command.h"

#include "base/hex.h"
#include "hotswap/hotswap_service.h"
#include "json/json.h"
#include "services/zcode_workspace_view_service.h"
#include "vcs/zcode_commons_v2.h"

#include <stdio.h>
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
    bool verify_request, bool allow_manifest_fields,
    uint8_t binding_root[32], const char *phase)
{
    const size_t min_fields = verify_request ? 4u : 3u;
    if (!request || !reply || !passport || !entry || !binding_root ||
        !request->input || request->input->type != JSON_OBJ ||
        (!allow_manifest_fields &&
         (request->input->num_children < min_fields ||
          request->input->num_children > min_fields + 1u))) {
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
    struct zcode_workspace_binding_input_v1 service_input = {
        .passport = *passport,
        .sequence = entry->sequence,
    };
    memcpy(service_input.module_release_root, entry->module_release_root, 32);
    memcpy(service_input.predecessor_release_root,
           entry->predecessor_release_root, 32);
    struct vcs_zcode_workspace_entry_v1 expected = *entry;
    enum vcs_zcode_commons_v2_error error =
        vcs_zcode_module_passport_v1_root(
            passport, expected.module_passport_root);
    memcpy(expected.semantic_fingerprint_root,
           passport->semantic_fingerprint_root, 32);
    memcpy(expected.source_assignment_root,
           passport->source_assignment_root, 32);
    uint8_t expected_root[32];
    if (error == VCS_ZCODE_COMMONS_V2_OK)
        error = vcs_zcode_workspace_entry_v1_root(&expected, expected_root);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        workspace_binding_fail(reply, "WORKSPACE_BINDING_INVALID", phase,
                               vcs_zcode_commons_v2_error_string(error));
        return false;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_workspace_view_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_WORKSPACE_VIEW_SERVICE_ID, &lease);
    if (!service) service = zcode_workspace_view_service_builtin();
    struct zcode_workspace_binding_result_v1 derived;
    bool derived_ok = service->derive_binding(&service_input, &derived) &&
        derived.valid && memcmp(&derived.entry, &expected, sizeof(expected)) == 0 &&
        memcmp(derived.binding_root, expected_root, 32) == 0;
    zcl_hotswap_service_release(&lease);
    if (!derived_ok) {
        workspace_binding_fail(
            reply, "WORKSPACE_VIEW_DERIVATION_MISMATCH", phase,
            "the pure workspace view disagreed with resident root confirmation");
        return false;
    }
    *entry = derived.entry;
    memcpy(binding_root, derived.binding_root, 32);
    return true;
}

static void workspace_push_root(struct json_value *data, const char *key,
                                const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static bool workspace_binding_render(
    struct zcl_command_reply *reply,
    const struct vcs_zcode_workspace_entry_v1 *entry,
    const uint8_t binding_root[32], bool verified)
{
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_workspace_view_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_WORKSPACE_VIEW_SERVICE_ID, &lease);
    if (!service) service = zcode_workspace_view_service_builtin();
    struct zcode_workspace_view_result_v1 view;
    bool rendered = service->render_binding(verified, &view) && view.valid &&
        view.kind[0] && view.capability[0] && view.next_action[0];
    zcl_hotswap_service_release(&lease);
    if (!rendered) {
        workspace_binding_fail(
            reply, "WORKSPACE_VIEW_RENDER_FAILED", "render",
            "the pure workspace view refused a resident-confirmed binding");
        return false;
    }
    (void)json_push_kv_bool(&reply->data, "verified_passport", true);
    (void)json_push_kv_bool(&reply->data,
                           verified ? "binding_verified" : "ready_to_bind",
                           true);
    (void)json_push_kv_str(&reply->data, "kind", view.kind);
    (void)json_push_kv_str(&reply->data, "capability", view.capability);
    (void)json_push_kv_str(&reply->data, "view_service_id",
                           ZCODE_WORKSPACE_VIEW_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "view_service_generation",
                           zcl_hotswap_service_generation());
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
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           view.next_action);
    return true;
}

void zcl_native_handle_zcode_workspace_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_module_passport_v1 passport;
    struct vcs_zcode_workspace_entry_v1 entry;
    uint8_t binding_root[32];
    if (!workspace_binding_parse(request, reply, &passport, &entry, false,
                                 false, binding_root, "plan"))
        return;
    (void)workspace_binding_render(reply, &entry, binding_root, false);
}

void zcl_native_handle_zcode_workspace_verify(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_module_passport_v1 passport;
    struct vcs_zcode_workspace_entry_v1 entry;
    uint8_t binding_root[32];
    if (!workspace_binding_parse(request, reply, &passport, &entry, true,
                                 false, binding_root, "verify"))
        return;
    uint8_t expected_root[32];
    if (!workspace_decode_root(request->input, "binding_root", expected_root) ||
        memcmp(expected_root, binding_root, 32) != 0) {
        workspace_binding_fail(
            reply, "WORKSPACE_BINDING_ROOT_MISMATCH", "verify",
            "binding_root does not match the Passport and workspace entry");
        return;
    }
    (void)workspace_binding_render(reply, &entry, binding_root, true);
}

static void workspace_manifest_fail(struct zcl_command_reply *reply,
                                    const char *code, const char *phase,
                                    const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, phase, false,
                           false, detail,
                           "zcode.workspace.manifest.plan");
}

static bool workspace_manifest_key_allowed(const char *key, bool commit)
{
    static const char *const keys[] = {
        "passport", "module_release_root", "sequence",
        "predecessor_release_root", "workspace_sequence",
        "predecessor_workspace_root", "signer_root",
    };
    if (!key) return false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strcmp(key, keys[i]) == 0) return true;
    return commit && strcmp(key, "signature") == 0;
}

static bool workspace_manifest_parse(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply,
    struct vcs_zcode_workspace_entry_v1 *entry,
    struct vcs_zcode_workspace_manifest_v1 *manifest,
    uint8_t binding_root[32], bool commit)
{
    if (!request || !reply || !entry || !manifest || !binding_root ||
        !request->input || request->input->type != JSON_OBJ) {
        if (reply) workspace_manifest_fail(
            reply, "BAD_WORKSPACE_MANIFEST_INPUT", "parse",
            "provide one verified Passport binding, workspace sequence and "
            "offline signer public key");
        return false;
    }
    const struct json_value *input = request->input;
    for (size_t i = 0; i < input->num_children; i++) {
        if (!workspace_manifest_key_allowed(input->keys[i], commit)) {
            workspace_manifest_fail(
                reply, "WORKSPACE_MANIFEST_UNKNOWN_FIELD", "parse",
                "manifest input contains an undeclared field");
            return false;
        }
    }
    if (!json_get(input, "passport") ||
        !json_get(input, "module_release_root") ||
        !json_get(input, "sequence") ||
        !json_get(input, "workspace_sequence") ||
        !json_get(input, "signer_root") ||
        (commit && !json_get(input, "signature"))) {
        workspace_manifest_fail(
            reply, "BAD_WORKSPACE_MANIFEST_INPUT", "parse",
            "passport, module_release_root, sequence, workspace_sequence and "
            "signer_root are required; commit also requires signature");
        return false;
    }
    struct vcs_zcode_module_passport_v1 passport;
    if (!workspace_binding_parse(request, reply, &passport, entry, false,
                                 true, binding_root, "manifest"))
        return false;

    const struct json_value *sequence = json_get(input, "workspace_sequence");
    if (!sequence || sequence->type != JSON_INT || sequence->val.i <= 0) {
        workspace_manifest_fail(
            reply, "WORKSPACE_MANIFEST_SEQUENCE_INVALID", "parse",
            "workspace_sequence must be a positive integer");
        return false;
    }
    memset(manifest, 0, sizeof(*manifest));
    manifest->schema_version = 1;
    manifest->flags = VCS_ZCODE_COMMONS_V2_REQUIRED_FLAGS;
    manifest->sequence = (uint64_t)sequence->val.i;
    manifest->entries = entry;
    manifest->entry_count = 1;
    if (!workspace_decode_root(input, "signer_root",
                               manifest->signer_root)) {
        workspace_manifest_fail(
            reply, "WORKSPACE_MANIFEST_SIGNER_INVALID", "parse",
            "signer_root must be one canonical lowercase Ed25519 public key");
        return false;
    }
    const struct json_value *predecessor =
        json_get(input, "predecessor_workspace_root");
    if (manifest->sequence > 1u) {
        if (!predecessor || !workspace_decode_root(
                input, "predecessor_workspace_root",
                manifest->predecessor_workspace_root)) {
            workspace_manifest_fail(
                reply, "WORKSPACE_MANIFEST_PREDECESSOR_REQUIRED", "parse",
                "workspace_sequence above one requires its exact predecessor root");
            return false;
        }
    } else if (predecessor) {
        workspace_manifest_fail(
            reply, "WORKSPACE_MANIFEST_PREDECESSOR_FORBIDDEN", "parse",
            "workspace_sequence one must not declare a predecessor root");
        return false;
    }
    if (commit) {
        const char *signature = json_get_str(json_get(input, "signature"));
        if (!signature || strlen(signature) != 128u ||
            !zcl_hex_decode_lower(signature, manifest->signature, 64)) {
            workspace_manifest_fail(
                reply, "WORKSPACE_MANIFEST_SIGNATURE_INVALID", "verify",
                "signature must be canonical lowercase 64-byte Ed25519 hex");
            return false;
        }
    }
    return true;
}

static bool workspace_manifest_render_service(
    struct zcl_command_reply *reply,
    enum zcode_workspace_manifest_view_mode_v1 mode,
    struct zcode_workspace_view_result_v1 *view)
{
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_workspace_view_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_WORKSPACE_VIEW_SERVICE_ID, &lease);
    if (!service) service = zcode_workspace_view_service_builtin();
    bool rendered = service->render_manifest &&
        service->render_manifest(mode, view) && view->valid &&
        view->kind[0] && view->capability[0] && view->next_action[0];
    zcl_hotswap_service_release(&lease);
    if (!rendered) {
        workspace_manifest_fail(
            reply, "WORKSPACE_MANIFEST_VIEW_FAILED", "render",
            "the pure workspace view refused the verified manifest binding");
        return false;
    }
    (void)json_push_kv_str(&reply->data, "binding_capability",
                           view->capability);
    (void)json_push_kv_str(&reply->data, "view_service_id",
                           ZCODE_WORKSPACE_VIEW_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "view_service_generation",
                           zcl_hotswap_service_generation());
    return true;
}

void zcl_native_handle_zcode_workspace_manifest_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_workspace_entry_v1 entry;
    struct vcs_zcode_workspace_manifest_v1 manifest;
    uint8_t binding_root[32];
    if (!workspace_manifest_parse(request, reply, &entry, &manifest,
                                  binding_root, false))
        return;
    uint8_t unsigned_root[32];
    uint8_t payload[
        VCS_ZCODE_WORKSPACE_MANIFEST_V1_SIGNING_PAYLOAD_BYTES];
    size_t payload_len = 0;
    enum vcs_zcode_commons_v2_error error =
        vcs_zcode_workspace_manifest_v1_unsigned_root(
            &manifest, unsigned_root);
    if (error == VCS_ZCODE_COMMONS_V2_OK)
        error = vcs_zcode_workspace_manifest_v1_signing_payload(
            &manifest, payload, sizeof(payload), &payload_len);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        workspace_manifest_fail(reply, "WORKSPACE_MANIFEST_PLAN_INVALID",
                                "plan",
                                vcs_zcode_commons_v2_error_string(error));
        return;
    }
    struct zcode_workspace_view_result_v1 view;
    if (!workspace_manifest_render_service(
            reply, ZCODE_WORKSPACE_MANIFEST_VIEW_PLAN, &view))
        return;
    char payload_hex[
        VCS_ZCODE_WORKSPACE_MANIFEST_V1_SIGNING_PAYLOAD_BYTES * 2u + 1u];
    zcl_hex_encode(payload, payload_len, payload_hex);
    (void)json_push_kv_str(&reply->data, "kind", view.kind);
    (void)json_push_kv_bool(&reply->data, "ready_for_signature", true);
    workspace_push_root(&reply->data, "unsigned_root", unsigned_root);
    workspace_push_root(&reply->data, "binding_root", binding_root);
    (void)json_push_kv_str(&reply->data, "signing_payload", payload_hex);
    (void)json_push_kv_int(&reply->data, "entry_count", 1);
    (void)json_push_kv_bool(&reply->data, "persisted", false);
    (void)json_push_kv_bool(&reply->data, "published", false);
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           view.next_action);
}

void zcl_native_handle_zcode_workspace_manifest_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    struct vcs_zcode_workspace_entry_v1 entry;
    struct vcs_zcode_workspace_manifest_v1 manifest;
    uint8_t binding_root[32];
    if (!workspace_manifest_parse(request, reply, &entry, &manifest,
                                  binding_root, true))
        return;
    enum vcs_zcode_commons_v2_error error =
        vcs_zcode_workspace_manifest_v1_verify(&manifest);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        workspace_manifest_fail(
            reply, "WORKSPACE_MANIFEST_SIGNATURE_INVALID", "verify",
            "the external signature does not verify the exact manifest plan");
        return;
    }
    uint8_t manifest_root[32];
    error = vcs_zcode_workspace_manifest_v1_root(&manifest, manifest_root);
    if (error != VCS_ZCODE_COMMONS_V2_OK) {
        workspace_manifest_fail(reply, "WORKSPACE_MANIFEST_ROOT_FAILED",
                                "root",
                                vcs_zcode_commons_v2_error_string(error));
        return;
    }
    struct zcode_workspace_view_result_v1 view;
    if (!workspace_manifest_render_service(
            reply, ZCODE_WORKSPACE_MANIFEST_VIEW_COMMIT, &view))
        return;
    (void)json_push_kv_str(&reply->data, "kind", view.kind);
    (void)json_push_kv_bool(&reply->data, "verified", true);
    workspace_push_root(&reply->data, "manifest_root", manifest_root);
    workspace_push_root(&reply->data, "binding_root", binding_root);
    (void)json_push_kv_int(&reply->data, "entry_count", 1);
    (void)json_push_kv_bool(&reply->data, "persisted", false);
    (void)json_push_kv_bool(&reply->data, "published", false);
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           view.next_action);
}

void zcl_native_handle_zcode_workspace_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !request->input ||
        request->input->type != JSON_OBJ || request->input->num_children != 0) {
        if (reply) workspace_binding_fail(
            reply, "BAD_WORKSPACE_STATUS_INPUT", "status",
            "zcode workspace status accepts no input keys");
        return;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_workspace_view_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_WORKSPACE_VIEW_SERVICE_ID, &lease);
    if (!service) service = zcode_workspace_view_service_builtin();
    struct zcode_workspace_view_result_v1 view;
    bool rendered = service->render_status(&view) && view.valid &&
        view.kind[0] && view.capability[0] && view.next_action[0];
    zcl_hotswap_service_release(&lease);
    if (!rendered) {
        workspace_binding_fail(reply, "WORKSPACE_STATUS_VIEW_FAILED", "status",
                               "the pure workspace status view refused output");
        return;
    }
    (void)json_push_kv_bool(&reply->data, "ready", true);
    (void)json_push_kv_str(&reply->data, "kind", view.kind);
    (void)json_push_kv_str(&reply->data, "capability", view.capability);
    (void)json_push_kv_str(&reply->data, "view_service_id",
                           ZCODE_WORKSPACE_VIEW_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "view_service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_bool(&reply->data, "signature_verification_static", true);
    (void)json_push_kv_bool(&reply->data, "root_confirmation_static", true);
    (void)json_push_kv_bool(&reply->data, "effects_swappable", false);
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           view.next_action);
}

static bool workspace_view_frozen_kat(const void *opaque, char *why,
                                      size_t why_sz)
{
    const struct zcode_workspace_view_service_v1 *service = opaque;
    struct zcode_workspace_binding_input_v1 input = {
        .sequence = 1,
        .passport = {
            .schema_version = 1,
            .flags = VCS_ZCODE_COMMONS_V2_REQUIRED_FLAGS,
        },
    };
    memset(input.module_release_root, 0x41, 32);
    uint8_t *passport_roots[] = {
        input.passport.stable_api_root,
        input.passport.recipe_root,
        input.passport.toolchain_root,
        input.passport.tests_root,
        input.passport.license_root,
        input.passport.semantic_fingerprint_root,
        input.passport.workspace_lineage_root,
        input.passport.source_assignment_root,
        input.passport.quality_profiles_root,
        input.passport.signer_root,
    };
    for (size_t i = 0; i < sizeof(passport_roots) / sizeof(passport_roots[0]);
         i++)
        memset(passport_roots[i], (int)(0x21u + i), 32);
    memset(input.passport.signature, 0x31, 64);
    struct zcode_workspace_binding_result_v1 actual;
    struct vcs_zcode_workspace_entry_v1 expected = {.sequence = 1};
    memcpy(expected.module_release_root, input.module_release_root, 32);
    memcpy(expected.semantic_fingerprint_root,
           input.passport.semantic_fingerprint_root, 32);
    memcpy(expected.source_assignment_root,
           input.passport.source_assignment_root, 32);
    uint8_t expected_root[32];
    if (!service || !service->derive_binding || !service->render_binding ||
        !service->render_status || !service->render_manifest ||
        vcs_zcode_module_passport_v1_root(
            &input.passport, expected.module_passport_root) !=
            VCS_ZCODE_COMMONS_V2_OK ||
        vcs_zcode_workspace_entry_v1_root(&expected, expected_root) !=
            VCS_ZCODE_COMMONS_V2_OK ||
        !service->derive_binding(&input, &actual) || !actual.valid ||
        memcmp(&actual.entry, &expected, sizeof(expected)) != 0 ||
        memcmp(actual.binding_root, expected_root, 32) != 0) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen workspace binding vector failed");
        return false;
    }
    struct zcode_workspace_view_result_v1 view;
    if (!service->render_binding(false, &view) || !view.valid ||
        strcmp(view.kind, "workspace_entry.v1") != 0 ||
        !service->render_status(&view) || !view.valid ||
        strcmp(view.next_action, "zcode workspace plan") != 0) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen workspace view/status vector failed");
        return false;
    }
    if (!service->render_manifest(
            ZCODE_WORKSPACE_MANIFEST_VIEW_PLAN, &view) || !view.valid ||
        strcmp(view.kind, "workspace_manifest.v1") != 0 ||
        strcmp(view.next_action,
               "offline-sign payload, then zcode workspace manifest commit") != 0 ||
        !service->render_manifest(
            ZCODE_WORKSPACE_MANIFEST_VIEW_COMMIT, &view) || !view.valid ||
        strcmp(view.next_action,
               "retain manifest root; human publication stays separate") != 0 ||
        service->render_manifest(
            (enum zcode_workspace_manifest_view_mode_v1)99, &view)) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen workspace manifest view vector failed");
        return false;
    }
    input.sequence = 2;
    if (service->derive_binding(&input, &actual)) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen missing-predecessor rejection vector failed");
        return false;
    }
    return true;
}

static const struct zcl_hotswap_service_contract k_workspace_view_contract = {
    .service_id = ZCODE_WORKSPACE_VIEW_SERVICE_ID,
    .source_tu = "app/services/src/zcode_workspace_view_service.c",
    .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
    .vtable_size = sizeof(struct zcode_workspace_view_service_v1),
    .abi_fingerprint = ZCODE_WORKSPACE_VIEW_ABI_FINGERPRINT,
    .schema_fingerprint = ZCODE_WORKSPACE_VIEW_SCHEMA_FINGERPRINT,
    .wire_fingerprint = ZCODE_WORKSPACE_VIEW_WIRE_FINGERPRINT,
    .kat_fingerprint = ZCODE_WORKSPACE_VIEW_KAT_FINGERPRINT,
    .frozen_kat = workspace_view_frozen_kat,
};

const struct zcl_hotswap_service_contract *
zcl_native_zcode_workspace_view_service_contract(void)
{
    return &k_workspace_view_contract;
}
