/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: non-creating native views of Commons v2 policy authorities. */

#include "command/native_command.h"

#include "base/hex.h"
#include "hotswap/hotswap_service.h"
#include "json/json.h"
#include "services/zcode_c23_economics_service.h"
#include "vcs/zcode_commons_v2.h"

#include <stdio.h>
#include <string.h>

static void moderation_fail(struct zcl_command_reply *reply,
                            const char *code, const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.moderation");
}

static bool moderation_no_keys(const struct json_value *input)
{
    return input && input->type == JSON_OBJ && input->num_children == 0;
}

static void moderation_hex(struct json_value *data, const char *key,
                           const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static void render_family_policy(struct json_value *data)
{
    struct vcs_zcode_family_policy_v1 policy;
    uint8_t root[32];
    vcs_zcode_family_policy_v1_default(&policy);
    (void)vcs_zcode_family_policy_v1_root(&policy, root);
    (void)json_push_kv_str(data, "profile", "family-c23.v1");
    moderation_hex(data, "policy_root", root);
    (void)json_push_kv_bool(data, "immutable", true);
    (void)json_push_kv_bool(data, "policy_selected_as_default", true);
    (void)json_push_kv_bool(data, "enforcement_complete", false);
    (void)json_push_kv_bool(data, "effective_default", false);
    (void)json_push_kv_bool(data, "default_public_view", false);
    (void)json_push_kv_bool(data, "simulation_only", true);
    (void)json_push_kv_bool(data, "owner_can_rewrite", false);
    (void)json_push_kv_int(data, "excluded_reason_mask",
                           policy.excluded_reason_mask);
    (void)json_push_kv_int(data, "max_dependency_objects",
                           policy.max_dependency_objects);
    (void)json_push_kv_int(data, "max_extracted_bytes",
                           (int64_t)policy.max_extracted_bytes);
    (void)json_push_kv_str(data, "pass_audiences",
                           "GENERAL|CONTEXTUAL_SCIENCE");
    (void)json_push_kv_str(data, "pass_behaviors", "BENIGN|DUAL_USE");
    (void)json_push_kv_str(data, "incomplete_result", "UNKNOWN");
    (void)json_push_kv_str(data, "new_content_state", "PENDING");
}

void zcl_native_handle_zcode_moderation_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !moderation_no_keys(request->input)) {
        if (reply) moderation_fail(reply, "BAD_MODERATION_STATUS_INPUT",
            "zcode moderation status accepts no input keys");
        return;
    }
    render_family_policy(&reply->data);
    (void)json_push_kv_str(&reply->data, "phase",
                           "protocol_foundation");
    (void)json_push_kv_bool(&reply->data, "admission_projection_ready",
                            false);
    (void)json_push_kv_bool(&reply->data, "cross_surface_gate_passed",
                            false);
    (void)json_push_kv_str(&reply->data, "official_surface_policy",
                           "legacy_v1_unchanged");
    (void)json_push_kv_str(&reply->data, "activation_blocker",
        "family admission projection and cross-surface enforcement are incomplete");
}

void zcl_native_handle_zcode_moderation_policy_list(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !moderation_no_keys(request->input)) {
        if (reply) moderation_fail(reply, "BAD_MODERATION_POLICY_LIST_INPUT",
            "zcode moderation policy list accepts no input keys");
        return;
    }
    (void)json_push_kv_int(&reply->data, "count", 1);
    render_family_policy(&reply->data);
    (void)json_push_kv_str(&reply->data, "future_policy_rule",
                           "a new profile and root; never rewrite v1");
}

void zcl_native_handle_zcode_moderation_policy_show(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const struct json_value *profile_value = request && request->input
        ? json_get(request->input, "profile") : NULL;
    const char *profile = profile_value ? json_get_str(profile_value) : NULL;
    if (!request || !reply || !request->input ||
        request->input->type != JSON_OBJ ||
        request->input->num_children != 1 || !profile ||
        strcmp(profile, "family-c23.v1") != 0) {
        if (reply) moderation_fail(reply, "UNKNOWN_MODERATION_POLICY",
            "profile must be exactly family-c23.v1");
        return;
    }
    render_family_policy(&reply->data);
    (void)json_push_kv_str(&reply->data, "contextual_eligibility",
        "neutral scientific, medical, historical, cybersecurity and dual-use education");
    (void)json_push_kv_bool(&reply->data,
                            "separate_from_accuracy_quality_security", true);
}

static bool economics_service_frozen_kat(const void *opaque, char *why,
                                         size_t why_sz)
{
    const struct zcode_c23_economics_service_v1 *service = opaque;
    struct vcs_zcode_family_policy_v1 family;
    uint8_t family_root[32], network[32], qualification[32], backlog[32];
    struct vcs_zcode_policy_candidate_v2 policy;
    struct zcode_c23_economics_status_result_v1 status;
    memset(network, 0x21, sizeof(network));
    memset(qualification, 0x22, sizeof(qualification));
    memset(backlog, 0x23, sizeof(backlog));
    vcs_zcode_family_policy_v1_default(&family);
    if (!service || !service->award_atoms || !service->policy_init ||
        !service->policy_validate || !service->policy_root ||
        !service->epoch_select || !service->render_status ||
        vcs_zcode_family_policy_v1_root(&family, family_root) !=
            VCS_ZCODE_COMMONS_V2_OK) {
        if (why && why_sz) (void)snprintf(why, why_sz,
            "frozen economics service shape/family-root vector failed");
        return false;
    }
    service->policy_init(&policy, network, family_root, qualification, backlog);
    uint8_t root[32], expected[32];
    if (service->policy_validate(&policy) != VCS_ZCODE_COMMONS_V2_OK ||
        service->policy_root(&policy, root) != VCS_ZCODE_COMMONS_V2_OK ||
        !zcl_hex_decode(ZCODE_C23_ECONOMICS_KAT_FINGERPRINT, expected, 32) ||
        memcmp(root, expected, 32) != 0 || !service->render_status(&status) ||
        status.award_atoms[VCS_ZCODE_CREATION_V2_MODULE_PUBLICATION] !=
            UINT64_C(100000000) ||
        status.award_atoms[VCS_ZCODE_CREATION_V2_PRESERVATION] !=
            UINT64_C(12500000) || status.partial_claim_issuance ||
        status.unused_capacity_carries) {
        if (why && why_sz) (void)snprintf(why, why_sz,
            "frozen policy-root/award/status vector failed");
        return false;
    }
    struct vcs_zcode_epoch_selection_v2 input = {
        .epoch = 7, .cutoff_height = 2000, .cutoff_mtp = 4000,
        .epoch_capacity_atoms = UINT64_C(300000000),
    };
    struct vcs_zcode_epoch_selection_result_v2 selected;
    if (service->epoch_select(&input, &policy, &selected) !=
            VCS_ZCODE_COMMONS_V2_OK || selected.selected_count != 0 ||
        selected.expired_capacity_atoms != UINT64_C(300000000) ||
        selected.recipient_cap_atoms != UINT64_C(100000000)) {
        if (why && why_sz) (void)snprintf(why, why_sz,
            "frozen empty-epoch selection vector failed");
        return false;
    }
    return true;
}

static const struct zcl_hotswap_service_contract k_economics_contract = {
    .service_id = ZCODE_C23_ECONOMICS_SERVICE_ID,
    .source_tu = "app/services/src/zcode_c23_economics_service.c",
    .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
    .vtable_size = sizeof(struct zcode_c23_economics_service_v1),
    .abi_fingerprint = ZCODE_C23_ECONOMICS_ABI_FINGERPRINT,
    .schema_fingerprint = ZCODE_C23_ECONOMICS_SCHEMA_FINGERPRINT,
    .wire_fingerprint = ZCODE_C23_ECONOMICS_WIRE_FINGERPRINT,
    .kat_fingerprint = ZCODE_C23_ECONOMICS_KAT_FINGERPRINT,
    .frozen_kat = economics_service_frozen_kat,
};

const struct zcl_hotswap_service_contract *
zcl_native_zcode_economics_service_contract(void)
{
    return &k_economics_contract;
}

void zcl_native_handle_zcode_commons_economics_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !moderation_no_keys(request->input)) {
        if (reply) moderation_fail(reply, "BAD_COMMONS_ECONOMICS_INPUT",
            "zcode commons economics status accepts no input keys");
        return;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_c23_economics_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_C23_ECONOMICS_SERVICE_ID, &lease);
    if (!service) service = zcode_c23_economics_service_builtin();
    struct zcode_c23_economics_status_result_v1 status;
    if (!service->render_status(&status)) {
        zcl_hotswap_service_release(&lease);
        moderation_fail(reply, "ECONOMICS_SERVICE_FAILED",
                        "the pure economics service refused status rendering");
        return;
    }
    (void)json_push_kv_str(&reply->data, "policy_object",
                           "zc23_policy_candidate.v2");
    (void)json_push_kv_str(&reply->data, "service_id",
                           ZCODE_C23_ECONOMICS_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_bool(&reply->data, "token_exists", false);
    (void)json_push_kv_bool(&reply->data, "funds_moved", false);
    (void)json_push_kv_bool(&reply->data, "ordinary_activity_mints", false);
    (void)json_push_kv_int(&reply->data, "challenge_blocks",
                           (int64_t)status.challenge_blocks);
    (void)json_push_kv_int(&reply->data, "challenge_seconds",
                           status.challenge_seconds);
    (void)json_push_kv_int(&reply->data, "module_publication_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_MODULE_PUBLICATION]);
    (void)json_push_kv_int(&reply->data, "defect_repair_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_DEFECT_REPAIR]);
    (void)json_push_kv_int(&reply->data, "security_finding_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_SECURITY_FINDING]);
    (void)json_push_kv_int(&reply->data, "independent_test_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_INDEPENDENT_TEST]);
    (void)json_push_kv_int(&reply->data, "reproduction_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_REPRODUCTION]);
    (void)json_push_kv_int(&reply->data, "performance_frontier_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_PERFORMANCE_FRONTIER]);
    (void)json_push_kv_int(&reply->data, "compatibility_proof_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_COMPATIBILITY_PROOF]);
    (void)json_push_kv_int(&reply->data, "preservation_atoms",
        (int64_t)status.award_atoms[VCS_ZCODE_CREATION_V2_PRESERVATION]);
    (void)json_push_kv_str(&reply->data, "queue_order",
                           status.queue_order);
    (void)json_push_kv_str(&reply->data, "category_order",
                           status.category_order);
    (void)json_push_kv_str(&reply->data, "concentration_cap",
        status.concentration_cap);
    (void)json_push_kv_bool(&reply->data, "partial_claim_issuance",
                            status.partial_claim_issuance);
    (void)json_push_kv_bool(&reply->data, "unused_capacity_carries",
                            status.unused_capacity_carries);
    zcl_hotswap_service_release(&lease);
}
