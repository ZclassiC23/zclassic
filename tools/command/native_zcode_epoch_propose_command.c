/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: scratch-only Proof-of-Participation epoch schedule proposals. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_epoch_schedule.h"

#include <stdlib.h>
#include <string.h>

static const char *zep_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zep_keys(const struct json_value *input,
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

static bool zep_root(const struct json_value *input, const char *key,
                     uint8_t root[32])
{
    const char *hex = zep_str(input, key);
    return hex && strlen(hex) == 64 && zcl_hex_decode_lower(hex, root, 32);
}

static bool zep_u64_positive(const struct json_value *input, const char *key,
                             uint64_t *out)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    if (!value || value->type != JSON_INT || json_get_int(value) <= 0)
        return false;
    *out = (uint64_t)json_get_int(value);
    return true;
}

static void zep_fail(struct zcl_command_reply *reply, const char *code,
                     const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.commons.schedule.propose");
}

static void zep_hex(struct json_value *data, const char *key,
                    const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static void zep_safety(struct json_value *data, bool persisted)
{
    (void)json_push_kv_bool(data, "simulated", true);
    (void)json_push_kv_bool(data, "persisted", persisted);
    (void)json_push_kv_bool(data, "schedule_proposal", true);
    (void)json_push_kv_bool(data, "mint", false);
    (void)json_push_kv_bool(data, "token_exists", false);
    (void)json_push_kv_bool(data, "funds_moved", false);
    (void)json_push_kv_bool(data, "custody_used", false);
    (void)json_push_kv_bool(data, "genesis_gate_satisfied", false);
    (void)json_push_kv_bool(data, "balance_used_for_truth", false);
}

static bool zep_workspace(const char *workspace,
                          struct zcl_command_reply *reply)
{
    if (zcl_native_zcode_workspace_is_explicit_scratch(workspace))
        return true;
    zep_fail(reply, "UNSAFE_PROPOSE_WORKSPACE",
             "workspace must explicitly name an isolated tmp, test-tmp, or scratch path");
    return false;
}

static const char *zep_class_name(uint16_t schedule_class)
{
    switch (schedule_class) {
    case VCS_ZCODE_EPOCH_SCHEDULE_CLASS_CREATION: return "creation";
    case VCS_ZCODE_EPOCH_SCHEDULE_CLASS_REPRODUCTION: return "reproduction";
    case VCS_ZCODE_EPOCH_SCHEDULE_CLASS_REPAIR: return "repair";
    case VCS_ZCODE_EPOCH_SCHEDULE_CLASS_PRESERVATION: return "preservation";
    }
    return "unknown";
}

static void zep_render_proposal(
    struct json_value *data,
    const struct vcs_zcode_epoch_schedule_proposal_v1 *proposal,
    const uint8_t proposal_root[32], bool persisted)
{
    zep_hex(data, "schedule_proposal_root", proposal_root);
    zep_hex(data, "previous_proposal_root",
            proposal->previous_proposal_root);
    (void)json_push_kv_int(data, "epoch", (int64_t)proposal->epoch);
    (void)json_push_kv_int(data, "cap_atoms",
                           (int64_t)VCS_ZC23_SCHEDULE_CAP_ATOMS);
    (void)json_push_kv_int(data, "total_epochs",
                           (int64_t)VCS_ZC23_SCHEDULE_TOTAL_EPOCHS);
    (void)json_push_kv_int(data, "budget_atoms",
                           (int64_t)proposal->budget_atoms);
    (void)json_push_kv_int(data, "already_emitted_atoms",
                           (int64_t)proposal->already_emitted_atoms);
    (void)json_push_kv_int(data, "proposed_mint_atoms",
                           (int64_t)proposal->proposed_mint_atoms);
    (void)json_push_kv_int(data, "unissued_atoms",
                           (int64_t)proposal->unissued_atoms);
    (void)json_push_kv_int(data, "evidence_count",
                           (int64_t)proposal->evidence_count);
    (void)json_push_kv_int(data, "eligible_count",
                           (int64_t)proposal->eligible_count);
    (void)json_push_kv_int(data, "preservation_skipped",
                           (int64_t)proposal->preservation_skipped);
    (void)json_push_kv_str(data, "preservation_skip_reason",
        VCS_ZCODE_EPOCH_SCHEDULE_PRESERVATION_SKIP_REASON);
    (void)json_push_kv_int(data, "class_weight_creation", 100);
    (void)json_push_kv_int(data, "class_weight_reproduction", 40);
    (void)json_push_kv_int(data, "class_weight_repair", 20);
    (void)json_push_kv_int(data, "class_weight_preservation", 5);
    struct json_value allocations;
    json_init(&allocations); json_set_array(&allocations);
    for (size_t i = 0; i < proposal->allocation_count; i++) {
        const struct vcs_zcode_epoch_schedule_allocation *allocation =
            &proposal->allocations[i];
        struct json_value row;
        json_init(&row); json_set_object(&row);
        zep_hex(&row, "contributor_binding_root",
                allocation->contributor_binding_root);
        (void)json_push_kv_str(&row, "class",
                               zep_class_name(allocation->schedule_class));
        (void)json_push_kv_int(&row, "award_atoms",
                               (int64_t)allocation->award_atoms);
        (void)json_push_back(&allocations, &row);
        json_free(&row);
    }
    (void)json_push_kv(data, "allocations", &allocations);
    json_free(&allocations);
    (void)json_push_kv_str(data, "mint_authority",
                           "schedule_proposal_simulation_only");
    zep_safety(data, persisted);
}

static bool zep_parse_propose(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    struct vcs_zcode_epoch_schedule_input *input,
    uint8_t previous_proposal_root[32])
{
    static const char *const keys[] = {
        "workspace", "epoch", "previous_proposal_root",
    };
    memset(input, 0, sizeof(*input));
    const char *workspace = request
        ? zep_str(request->input, "workspace") : NULL;
    if (!request || !reply || !workspace ||
        !zep_keys(request->input, keys, sizeof(keys) / sizeof(keys[0])) ||
        !zep_u64_positive(request->input, "epoch", &input->epoch) ||
        !zep_root(request->input, "previous_proposal_root",
                  previous_proposal_root)) {
        zep_fail(reply, "BAD_PROPOSE_INPUT",
                 "closed input requires workspace, positive epoch and previous_proposal_root");
        return false;
    }
    if (!zep_workspace(workspace, reply)) return false;
    input->workspace = workspace;
    input->previous_proposal_root = previous_proposal_root;
    return true;
}

static void zep_propose_handle(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    bool persist)
{
    struct vcs_zcode_epoch_schedule_input input;
    struct vcs_zcode_epoch_schedule_proposal_v1 proposal;
    uint8_t previous_proposal_root[32], proposal_root[32];
    if (!zep_parse_propose(request, reply, &input,
                           previous_proposal_root))
        return;
    enum vcs_zcode_epoch_schedule_error error =
        vcs_zcode_epoch_schedule_propose_cas(&input, &proposal);
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (error == VCS_ZCODE_EPOCH_SCHEDULE_OK)
        error = vcs_zcode_epoch_schedule_root(&proposal, proposal_root);
    if (error == VCS_ZCODE_EPOCH_SCHEDULE_OK)
        error = vcs_zcode_epoch_schedule_serialize(&proposal, &wire,
                                                   &wire_len);
    if (error != VCS_ZCODE_EPOCH_SCHEDULE_OK) {
        zep_fail(reply, "EPOCH_SCHEDULE_PROPOSE_REFUSED",
                 vcs_zcode_epoch_schedule_error_string(error));
        vcs_zcode_epoch_schedule_proposal_free(&proposal);
        free(wire);
        return;
    }
    if (persist &&
        (!vcs_object_store_init(input.workspace) ||
         !vcs_object_put_addressed(input.workspace, proposal_root,
                                   wire, wire_len))) {
        zep_fail(reply, "EPOCH_SCHEDULE_STORE_REFUSED",
                 "existing scratch CAS refused the canonical schedule proposal");
        vcs_zcode_epoch_schedule_proposal_free(&proposal);
        free(wire);
        return;
    }
    zep_render_proposal(&reply->data, &proposal, proposal_root, persist);
    vcs_zcode_epoch_schedule_proposal_free(&proposal);
    free(wire);
}

void zcl_native_handle_zcode_commons_schedule_propose_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zep_propose_handle(request, reply, false);
}

void zcl_native_handle_zcode_commons_schedule_propose_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zep_propose_handle(request, reply, true);
}
