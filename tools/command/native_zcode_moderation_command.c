/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: non-creating native views of Commons v2 policy authorities. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/zcode_commons_v2.h"

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
    (void)json_push_kv_bool(data, "default_public_view", true);
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

void zcl_native_handle_zcode_commons_economics_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !moderation_no_keys(request->input)) {
        if (reply) moderation_fail(reply, "BAD_COMMONS_ECONOMICS_INPUT",
            "zcode commons economics status accepts no input keys");
        return;
    }
    (void)json_push_kv_str(&reply->data, "policy_object",
                           "zc23_policy_candidate.v2");
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_bool(&reply->data, "token_exists", false);
    (void)json_push_kv_bool(&reply->data, "funds_moved", false);
    (void)json_push_kv_bool(&reply->data, "ordinary_activity_mints", false);
    (void)json_push_kv_int(&reply->data, "challenge_blocks",
                           VCS_ZCODE_COMMONS_CHALLENGE_BLOCKS);
    (void)json_push_kv_int(&reply->data, "challenge_seconds",
                           VCS_ZCODE_COMMONS_CHALLENGE_SECONDS);
    (void)json_push_kv_int(&reply->data, "module_publication_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_MODULE_PUBLICATION));
    (void)json_push_kv_int(&reply->data, "defect_repair_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_DEFECT_REPAIR));
    (void)json_push_kv_int(&reply->data, "security_finding_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_SECURITY_FINDING));
    (void)json_push_kv_int(&reply->data, "independent_test_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_INDEPENDENT_TEST));
    (void)json_push_kv_int(&reply->data, "reproduction_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_REPRODUCTION));
    (void)json_push_kv_int(&reply->data, "performance_frontier_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_PERFORMANCE_FRONTIER));
    (void)json_push_kv_int(&reply->data, "compatibility_proof_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_COMPATIBILITY_PROOF));
    (void)json_push_kv_int(&reply->data, "preservation_atoms",
        (int64_t)vcs_zcode_creation_award_atoms_v2(
            VCS_ZCODE_CREATION_V2_PRESERVATION));
    (void)json_push_kv_str(&reply->data, "queue_order",
                           "maturity_height,maturity_mtp,claim_root");
    (void)json_push_kv_str(&reply->data, "category_order",
                           "previous-epoch-root rotation, cyclic");
    (void)json_push_kv_str(&reply->data, "concentration_cap",
        "min(epoch_capacity,max(1 ZC23,floor(epoch_capacity/100)))");
    (void)json_push_kv_bool(&reply->data, "partial_claim_issuance", false);
    (void)json_push_kv_bool(&reply->data, "unused_capacity_carries", false);
}
