/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: scratch-only Proof-of-Participation epoch schedule proposals. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "hotswap/hotswap_service.h"
#include "services/zcode_c23_economics_service.h"
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

static bool zep_workspace(const char *workspace,
                          struct zcl_command_reply *reply)
{
    if (zcl_native_zcode_workspace_is_explicit_scratch(workspace))
        return true;
    zep_fail(reply, "UNSAFE_PROPOSE_WORKSPACE",
             "workspace must explicitly name an isolated tmp, test-tmp, or scratch path");
    return false;
}

static bool zep_render_proposal(
    struct json_value *data,
    const struct vcs_zcode_epoch_schedule_proposal_v1 *proposal,
    const uint8_t proposal_root[32],
    const struct zcode_c23_economics_service_v1 *service,
    uint64_t service_generation, bool persisted)
{
    struct zcode_c23_schedule_proposal_view_v1 view;
    if (!service || !service->render_schedule_proposal ||
        !service->schedule_class_name ||
        !service->render_schedule_proposal(proposal, persisted, &view))
        return false;
    (void)json_push_kv_str(data, "service_id",
                           ZCODE_C23_ECONOMICS_SERVICE_ID);
    (void)json_push_kv_int(data, "service_generation",
                           (int64_t)service_generation);
    (void)json_push_kv_bool(data, "pure_calculation", true);
    zep_hex(data, "schedule_proposal_root", proposal_root);
    zep_hex(data, "previous_proposal_root",
            proposal->previous_proposal_root);
    (void)json_push_kv_int(data, "epoch", (int64_t)view.epoch);
    (void)json_push_kv_int(data, "cap_atoms", (int64_t)view.cap_atoms);
    (void)json_push_kv_int(data, "total_epochs",
                           (int64_t)view.total_epochs);
    (void)json_push_kv_int(data, "budget_atoms", (int64_t)view.budget_atoms);
    (void)json_push_kv_int(data, "already_emitted_atoms",
                           (int64_t)view.already_emitted_atoms);
    (void)json_push_kv_int(data, "proposed_mint_atoms",
                           (int64_t)view.proposed_mint_atoms);
    (void)json_push_kv_int(data, "unissued_atoms",
                           (int64_t)view.unissued_atoms);
    (void)json_push_kv_int(data, "evidence_count",
                           (int64_t)view.evidence_count);
    (void)json_push_kv_int(data, "eligible_count",
                           (int64_t)view.eligible_count);
    (void)json_push_kv_int(data, "preservation_skipped",
                           (int64_t)view.preservation_skipped);
    (void)json_push_kv_str(data, "preservation_skip_reason",
                           view.preservation_skip_reason);
    (void)json_push_kv_int(data, "class_weight_creation",
                           (int64_t)view.class_weights[0]);
    (void)json_push_kv_int(data, "class_weight_reproduction",
                           (int64_t)view.class_weights[1]);
    (void)json_push_kv_int(data, "class_weight_repair",
                           (int64_t)view.class_weights[2]);
    (void)json_push_kv_int(data, "class_weight_preservation",
                           (int64_t)view.class_weights[3]);
    struct json_value allocations;
    json_init(&allocations); json_set_array(&allocations);
    for (size_t i = 0; i < proposal->allocation_count; i++) {
        const struct vcs_zcode_epoch_schedule_allocation *allocation =
            &proposal->allocations[i];
        struct json_value row;
        char class_name[16];
        json_init(&row); json_set_object(&row);
        zep_hex(&row, "contributor_binding_root",
                allocation->contributor_binding_root);
        if (!service->schedule_class_name(allocation->schedule_class,
                                          class_name, sizeof(class_name))) {
            json_free(&row);
            json_free(&allocations);
            return false;
        }
        (void)json_push_kv_str(&row, "class", class_name);
        (void)json_push_kv_int(&row, "award_atoms",
                               (int64_t)allocation->award_atoms);
        (void)json_push_back(&allocations, &row);
        json_free(&row);
    }
    (void)json_push_kv(data, "allocations", &allocations);
    json_free(&allocations);
    (void)json_push_kv_str(data, "mint_authority", view.mint_authority);
    (void)json_push_kv_bool(data, "simulated", view.simulated);
    (void)json_push_kv_bool(data, "persisted", view.persisted);
    (void)json_push_kv_bool(data, "schedule_proposal",
                            view.schedule_proposal);
    (void)json_push_kv_bool(data, "mint", view.mint);
    (void)json_push_kv_bool(data, "token_exists", view.token_exists);
    (void)json_push_kv_bool(data, "funds_moved", view.funds_moved);
    (void)json_push_kv_bool(data, "custody_used", view.custody_used);
    (void)json_push_kv_bool(data, "genesis_gate_satisfied",
                            view.genesis_gate_satisfied);
    (void)json_push_kv_bool(data, "balance_used_for_truth",
                            view.balance_used_for_truth);
    return true;
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
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_c23_economics_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_C23_ECONOMICS_SERVICE_ID, &lease);
    if (!service) service = zcode_c23_economics_service_builtin();
    uint64_t service_generation = zcl_hotswap_service_generation();
    if (!zep_render_proposal(&reply->data, &proposal, proposal_root, service,
                             service_generation, persist)) {
        zcl_hotswap_service_release(&lease);
        zep_fail(reply, "EPOCH_SCHEDULE_VIEW_REFUSED",
                 "the pure economics service refused the validated proposal view");
        vcs_zcode_epoch_schedule_proposal_free(&proposal);
        free(wire);
        return;
    }
    zcl_hotswap_service_release(&lease);
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
