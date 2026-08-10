/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: non-creating native views of Commons v2 policy authorities. */

#include "command/native_command.h"

#include "base/hex.h"
#include "hotswap/hotswap_service.h"
#include "json/json.h"
#include "services/zcode_c23_economics_service.h"
#include "services/zcode_moderation_view_service.h"
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

static bool render_family_policy(struct zcl_command_reply *reply)
{
    struct vcs_zcode_family_policy_v1 policy;
    uint8_t root[32];
    char root_hex[65];
    vcs_zcode_family_policy_v1_default(&policy);
    if (vcs_zcode_family_policy_v1_root(&policy, root) !=
            VCS_ZCODE_COMMONS_V2_OK) {
        moderation_fail(reply, "MODERATION_POLICY_ROOT",
                        "the immutable Family policy root could not be derived");
        return false;
    }
    zcl_hex_encode(root, sizeof(root), root_hex);
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_moderation_view_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_MODERATION_VIEW_SERVICE_ID, &lease);
    if (!service)
        service = zcode_moderation_view_service_builtin();
    struct zcode_moderation_policy_view_v1 view;
    if (!service->render_policy(&policy, root_hex, &view) || !view.valid) {
        zcl_hotswap_service_release(&lease);
        moderation_fail(reply, "MODERATION_VIEW_FAILED",
                        "the pure moderation view refused the Family policy");
        return false;
    }
    struct json_value *data = &reply->data;
    (void)json_push_kv_str(data, "profile", "family-c23.v1");
    (void)json_push_kv_str(data, "policy_root", view.policy_root);
    (void)json_push_kv_str(data, "view_service_id",
                           ZCODE_MODERATION_VIEW_SERVICE_ID);
    (void)json_push_kv_int(data, "view_service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_bool(data, "immutable", true);
    (void)json_push_kv_bool(data, "policy_selected_as_default", true);
    (void)json_push_kv_bool(data, "enforcement_complete", false);
    (void)json_push_kv_bool(data, "effective_default", false);
    (void)json_push_kv_bool(data, "default_public_view", false);
    (void)json_push_kv_bool(data, "simulation_only", true);
    (void)json_push_kv_bool(data, "owner_can_rewrite", false);
    (void)json_push_kv_int(data, "excluded_reason_mask",
                           view.excluded_reason_mask);
    (void)json_push_kv_int(data, "max_dependency_objects",
                           view.max_dependency_objects);
    (void)json_push_kv_int(data, "max_extracted_bytes",
                           (int64_t)view.max_extracted_bytes);
    (void)json_push_kv_str(data, "pass_audiences",
                           view.pass_audiences);
    (void)json_push_kv_str(data, "pass_behaviors", view.pass_behaviors);
    (void)json_push_kv_str(data, "incomplete_result",
                           view.incomplete_result);
    (void)json_push_kv_str(data, "new_content_state",
                           view.new_content_state);
    (void)json_push_kv_str(data, "contextual_eligibility",
                           view.contextual_eligibility);
    (void)json_push_kv_bool(data,
                            "separate_from_accuracy_quality_security",
                            view.separate_from_accuracy_quality_security);
    (void)json_push_kv_str(data, "policy_summary", view.policy_summary);
    zcl_hotswap_service_release(&lease);
    return true;
}

void zcl_native_handle_zcode_moderation_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !moderation_no_keys(request->input)) {
        if (reply) moderation_fail(reply, "BAD_MODERATION_STATUS_INPUT",
            "zcode moderation status accepts no input keys");
        return;
    }
    if (!render_family_policy(reply))
        return;
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

void zcl_native_handle_zcode_moderation_service_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !moderation_no_keys(request->input)) {
        if (reply) moderation_fail(reply, "BAD_MODERATION_SERVICE_INPUT",
            "zcode moderation service status accepts no input keys");
        return;
    }
    if (!render_family_policy(reply))
        return;
    (void)json_push_kv_bool(&reply->data, "projection_ready", false);
    (void)json_push_kv_int(&reply->data, "registered_service_count", 0);
    (void)json_push_kv_int(&reply->data, "eligible_service_count", 0);
    (void)json_push_kv_bool(&reply->data, "roster_finalized", false);
    (void)json_push_kv_bool(&reply->data, "classification_enabled", false);
    (void)json_push_kv_bool(&reply->data, "advertisement_enabled", false);
    (void)json_push_kv_bool(&reply->data, "chain_selection_enabled", false);
    (void)json_push_kv_bool(&reply->data,
                            "operator_group_diversity_declared", false);
    (void)json_push_kv_str(&reply->data, "bootstrap_label",
                           "unavailable:no_signed_service_roster");
    (void)json_push_kv_str(&reply->data, "blocker",
        "signed service registration and finalized roster projection are not implemented");
    (void)json_push_kv_str(&reply->data, "next_command",
                           "zcode moderation status");
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
    if (!render_family_policy(reply))
        return;
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
    if (!render_family_policy(reply))
        return;
}

static bool moderation_view_frozen_kat(const void *opaque, char *why,
                                       size_t why_sz)
{
    const struct zcode_moderation_view_service_v1 *service = opaque;
    struct vcs_zcode_family_policy_v1 policy;
    struct zcode_moderation_policy_view_v1 view;
    uint8_t root[32];
    char root_hex[65];
    vcs_zcode_family_policy_v1_default(&policy);
    if (!service || !service->render_policy ||
        vcs_zcode_family_policy_v1_root(&policy, root) !=
            VCS_ZCODE_COMMONS_V2_OK) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen moderation service shape/root vector failed");
        return false;
    }
    zcl_hex_encode(root, sizeof(root), root_hex);
    if (!service->render_policy(&policy, root_hex, &view) || !view.valid ||
        strcmp(view.policy_root, root_hex) != 0 ||
        view.excluded_reason_mask != policy.excluded_reason_mask ||
        view.max_dependency_objects != policy.max_dependency_objects ||
        view.max_extracted_bytes != policy.max_extracted_bytes ||
        strcmp(view.pass_audiences, "GENERAL|CONTEXTUAL_SCIENCE") != 0 ||
        strcmp(view.pass_behaviors, "BENIGN|DUAL_USE") != 0 ||
        strcmp(view.incomplete_result, "UNKNOWN") != 0 ||
        strcmp(view.new_content_state, "PENDING") != 0 ||
        !view.separate_from_accuracy_quality_security) {
        if (why && why_sz) (void)snprintf(
            why, why_sz, "frozen Family policy presentation vector failed");
        return false;
    }
    return true;
}

static const struct zcl_hotswap_service_contract k_moderation_view_contract = {
    .service_id = ZCODE_MODERATION_VIEW_SERVICE_ID,
    .source_tu = "app/services/src/zcode_moderation_view_service.c",
    .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
    .vtable_size = sizeof(struct zcode_moderation_view_service_v1),
    .abi_fingerprint = ZCODE_MODERATION_VIEW_ABI_FINGERPRINT,
    .schema_fingerprint = ZCODE_MODERATION_VIEW_SCHEMA_FINGERPRINT,
    .wire_fingerprint = ZCODE_MODERATION_VIEW_WIRE_FINGERPRINT,
    .kat_fingerprint = ZCODE_MODERATION_VIEW_KAT_FINGERPRINT,
    .frozen_kat = moderation_view_frozen_kat,
};

const struct zcl_hotswap_service_contract *
zcl_native_zcode_moderation_view_service_contract(void)
{
    return &k_moderation_view_contract;
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
        !service->render_schedule_proposal || !service->schedule_class_name ||
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
        !zcl_hex_decode(ZCODE_C23_ECONOMICS_POLICY_KAT_ROOT, expected, 32) ||
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
    struct vcs_zcode_epoch_schedule_proposal_v1 proposal;
    vcs_zcode_epoch_schedule_proposal_init(&proposal);
    proposal.schema_version = VCS_ZCODE_EPOCH_SCHEDULE_VERSION;
    proposal.epoch = 1;
    proposal.budget_atoms =
        VCS_ZC23_SCHEDULE_CAP_ATOMS / VCS_ZC23_SCHEDULE_TOTAL_EPOCHS;
    proposal.unissued_atoms = proposal.budget_atoms;
    struct zcode_c23_schedule_proposal_view_v1 proposal_view;
    char class_name[16];
    if (!service->render_schedule_proposal(&proposal, false, &proposal_view) ||
        proposal_view.epoch != 1 ||
        proposal_view.budget_atoms != UINT64_C(2019230769230) ||
        proposal_view.class_weights[0] != 100 ||
        proposal_view.class_weights[1] != 40 ||
        proposal_view.class_weights[2] != 20 ||
        proposal_view.class_weights[3] != 5 || !proposal_view.simulated ||
        proposal_view.persisted || proposal_view.mint ||
        strcmp(proposal_view.mint_authority,
               "simulation_only;no_issuance_authority") != 0 ||
        !service->schedule_class_name(
            VCS_ZCODE_EPOCH_SCHEDULE_CLASS_REPRODUCTION, class_name,
            sizeof(class_name)) || strcmp(class_name, "reproduction") != 0) {
        if (why && why_sz) (void)snprintf(why, why_sz,
            "frozen schedule-proposal view vector failed");
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

void zcl_native_handle_zcode_commons_backlog(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !moderation_no_keys(request->input)) {
        if (reply) moderation_fail(reply, "BAD_COMMONS_BACKLOG_INPUT",
            "zcode commons backlog accepts no input keys");
        return;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_c23_economics_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_C23_ECONOMICS_SERVICE_ID, &lease);
    if (!service) service = zcode_c23_economics_service_builtin();
    struct zcode_c23_economics_status_result_v1 status;
    if (!service->render_status(&status)) {
        zcl_hotswap_service_release(&lease);
        moderation_fail(reply, "BACKLOG_SERVICE_FAILED",
                        "the pure economics service refused backlog rendering");
        return;
    }
    (void)json_push_kv_str(&reply->data, "service_id",
                           ZCODE_C23_ECONOMICS_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_bool(&reply->data, "projection_ready", false);
    (void)json_push_kv_int(&reply->data, "claim_count", 0);
    (void)json_push_kv_bool(&reply->data, "issuance_enabled", false);
    (void)json_push_kv_bool(&reply->data, "funds_moved", false);
    (void)json_push_kv_str(&reply->data, "queue_order",
                           status.queue_order);
    (void)json_push_kv_str(&reply->data, "category_rotation",
                           status.category_order);
    (void)json_push_kv_bool(&reply->data, "partial_claim_issuance",
                            status.partial_claim_issuance);
    (void)json_push_kv_bool(&reply->data, "unused_capacity_carries",
                            status.unused_capacity_carries);
    (void)json_push_kv_bool(&reply->data, "unused_capacity_expires",
                            !status.unused_capacity_carries);
    (void)json_push_kv_str(&reply->data, "blocker",
        "claim/backlog projection is not implemented; issuance selection is unavailable");
    (void)json_push_kv_str(&reply->data, "next_command",
                           "zcode commons economics status");
    zcl_hotswap_service_release(&lease);
}
