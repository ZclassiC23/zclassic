/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: scratch-only Living Commons creation and epoch simulation. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_shadow_simulation.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *zcs_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zcs_keys(const struct json_value *input,
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

static bool zcs_root(const struct json_value *input, const char *key,
                     uint8_t root[32])
{
    const char *hex = zcs_str(input, key);
    return hex && strlen(hex) == 64 && zcl_hex_decode_lower(hex, root, 32);
}

static bool zcs_u64(const struct json_value *input, const char *key,
                    uint64_t *out)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    if (!value || value->type != JSON_INT || json_get_int(value) < 0)
        return false;
    *out = (uint64_t)json_get_int(value);
    return true;
}

static bool zcs_i64_positive(const struct json_value *input, const char *key,
                             int64_t *out)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    if (!value || value->type != JSON_INT || json_get_int(value) <= 0)
        return false;
    *out = json_get_int(value);
    return true;
}

static void zcs_fail(struct zcl_command_reply *reply, const char *code,
                     const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.commons.shadow");
}

static void zcs_hex(struct json_value *data, const char *key,
                    const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(data, key, hex);
}

static void zcs_safety(struct json_value *data, bool persisted)
{
    (void)json_push_kv_bool(data, "simulated", true);
    (void)json_push_kv_bool(data, "persisted", persisted);
    (void)json_push_kv_bool(data, "token_exists", false);
    (void)json_push_kv_bool(data, "funds_moved", false);
    (void)json_push_kv_bool(data, "custody_used", false);
    (void)json_push_kv_bool(data, "genesis_gate_satisfied", false);
    (void)json_push_kv_bool(data, "balance_used_for_truth", false);
}

static bool zcs_workspace(const char *workspace,
                          struct zcl_command_reply *reply)
{
    if (zcl_native_zcode_workspace_is_explicit_scratch(workspace))
        return true;
    zcs_fail(reply, "UNSAFE_SHADOW_WORKSPACE",
             "workspace must explicitly name an isolated tmp, test-tmp, or scratch path");
    return false;
}

static void zcs_render_attribution(
    struct json_value *data,
    const struct vcs_zcode_shadow_attribution_plan *plan, bool persisted)
{
    zcs_hex(data, "creation_attribution_root", plan->attribution_root);
    zcs_hex(data, "fixture_branch_root", plan->fixture_branch_root);
    zcs_hex(data, "policy_candidate_root",
            plan->attribution.zc23_policy_root);
    zcs_hex(data, "score_receipt_root",
            plan->attribution.score_receipt_root);
    zcs_hex(data, "contributor_binding_root",
            plan->attribution.contributor_binding_root);
    zcs_hex(data, "package_root", plan->attribution.package_root);
    zcs_hex(data, "release_root", plan->attribution.release_root);
    zcs_hex(data, "license_evidence_root",
            plan->attribution.license_evidence_root);
    (void)json_push_kv_int(data, "epoch",
                           (int64_t)plan->attribution.epoch);
    (void)json_push_kv_int(data, "award_atoms",
                           (int64_t)plan->attribution.award_atoms);
    (void)json_push_kv_int(data, "challenge_opening_height",
        (int64_t)plan->attribution.challenge_opening_height);
    (void)json_push_kv_int(data, "challenge_maturity_height",
        (int64_t)plan->attribution.challenge_maturity_height);
    (void)json_push_kv_bool(data, "qualification_ready", true);
    (void)json_push_kv_bool(data, "exact_reproduction_match",
                            plan->qualification.exact_reproduction_match);
    (void)json_push_kv_bool(data, "remote_transport_used",
                            plan->qualification.remote_transport_used);
    (void)json_push_kv_bool(data, "physical_independence_proven",
                            plan->qualification.physical_independence_proven);
    (void)json_push_kv_str(data, "anchor_authority",
                           "deterministic_shadow_fixture_only");
    zcs_safety(data, persisted);
}

static bool zcs_parse_attribution(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    struct vcs_zcode_shadow_attribution_input *input,
    uint8_t roots[5][32])
{
    static const char *const keys[] = {
        "workspace", "score_receipt_root", "policy_candidate_root",
        "reproduction_request_root", "reproduction_proof_set_root",
        "contributor_binding_root", "epoch", "now_unix",
    };
    memset(input, 0, sizeof(*input));
    const char *workspace = request
        ? zcs_str(request->input, "workspace") : NULL;
    if (!request || !reply || !workspace ||
        !zcs_keys(request->input, keys, sizeof(keys) / sizeof(keys[0])) ||
        !zcs_root(request->input, "score_receipt_root", roots[0]) ||
        !zcs_root(request->input, "policy_candidate_root", roots[1]) ||
        !zcs_root(request->input, "reproduction_request_root", roots[2]) ||
        !zcs_root(request->input, "reproduction_proof_set_root", roots[3]) ||
        !zcs_root(request->input, "contributor_binding_root", roots[4]) ||
        !zcs_u64(request->input, "epoch", &input->epoch) ||
        !zcs_i64_positive(request->input, "now_unix", &input->now_unix)) {
        zcs_fail(reply, "BAD_SHADOW_ATTRIBUTION_INPUT",
                 "closed input requires workspace, five full roots, epoch and now_unix");
        return false;
    }
    if (!zcs_workspace(workspace, reply)) return false;
    input->workspace = workspace;
    input->score_receipt_root = roots[0];
    input->policy_candidate_root = roots[1];
    input->reproduction_request_root = roots[2];
    input->reproduction_proof_set_root = roots[3];
    input->contributor_binding_root = roots[4];
    return true;
}

static void zcs_attribution_handle(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    bool persist)
{
    struct vcs_zcode_shadow_attribution_input input;
    struct vcs_zcode_shadow_attribution_plan plan;
    uint8_t roots[5][32];
    if (!zcs_parse_attribution(request, reply, &input, roots)) return;
    enum vcs_zcode_shadow_simulation_error error =
        vcs_zcode_shadow_attribution_plan_cas(&input, &plan);
    uint8_t wire[VCS_ZCODE_CREATION_ATTRIBUTION_WIRE_BYTES];
    if (error != VCS_ZCODE_SHADOW_SIMULATION_OK ||
        vcs_zcode_creation_attribution_serialize(&plan.attribution, wire) !=
            VCS_ZCODE_CREATION_OK) {
        zcs_fail(reply, "SHADOW_ATTRIBUTION_REFUSED",
                 vcs_zcode_shadow_simulation_error_string(error));
        return;
    }
    if (persist &&
        (!vcs_object_store_init(input.workspace) ||
         !vcs_object_put_addressed(input.workspace, plan.attribution_root,
                                   wire, sizeof(wire)))) {
        zcs_fail(reply, "SHADOW_ATTRIBUTION_STORE_REFUSED",
                 "existing scratch CAS refused the canonical attribution");
        return;
    }
    zcs_render_attribution(&reply->data, &plan, persist);
}

void zcl_native_handle_zcode_commons_shadow_attribution_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcs_attribution_handle(request, reply, false);
}

void zcl_native_handle_zcode_commons_shadow_attribution_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcs_attribution_handle(request, reply, true);
}

static bool zcs_parse_epoch(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    struct vcs_zcode_shadow_epoch_input *input, uint8_t roots[4][32])
{
    static const char *const keys[] = {
        "workspace", "policy_candidate_root", "attribution_root",
        "fixture_branch_root", "previous_epoch_creation_root", "now_unix",
    };
    memset(input, 0, sizeof(*input));
    const char *workspace = request
        ? zcs_str(request->input, "workspace") : NULL;
    if (!request || !reply || !workspace ||
        !zcs_keys(request->input, keys, sizeof(keys) / sizeof(keys[0])) ||
        !zcs_root(request->input, "policy_candidate_root", roots[0]) ||
        !zcs_root(request->input, "attribution_root", roots[1]) ||
        !zcs_root(request->input, "fixture_branch_root", roots[2]) ||
        !zcs_root(request->input, "previous_epoch_creation_root", roots[3]) ||
        !zcs_i64_positive(request->input, "now_unix", &input->now_unix)) {
        zcs_fail(reply, "BAD_SHADOW_EPOCH_INPUT",
                 "closed input requires workspace, policy, attribution, branch, predecessor roots and now_unix");
        return false;
    }
    if (!zcs_workspace(workspace, reply)) return false;
    input->workspace = workspace;
    input->policy_candidate_root = roots[0];
    input->attribution_root = roots[1];
    input->fixture_branch_root = roots[2];
    input->previous_epoch_creation_root = roots[3];
    return true;
}

static void zcs_render_epoch(
    struct json_value *data, const struct vcs_zcode_shadow_epoch_plan *plan,
    bool persisted)
{
    zcs_hex(data, "epoch_creation_root", plan->epoch_root);
    zcs_hex(data, "creation_attribution_root", plan->attribution_root);
    zcs_hex(data, "policy_candidate_root", plan->epoch.zc23_policy_root);
    zcs_hex(data, "previous_epoch_creation_root",
            plan->epoch.previous_epoch_creation_root);
    zcs_hex(data, "fixture_branch_root",
            plan->epoch.committee_evidence_snapshot_root);
    (void)json_push_kv_int(data, "epoch", (int64_t)plan->epoch.epoch);
    (void)json_push_kv_int(data, "emission_cap_atoms",
                           (int64_t)plan->epoch.emission_cap_atoms);
    (void)json_push_kv_int(data, "actual_mint_atoms",
                           (int64_t)plan->epoch.actual_mint_atoms);
    (void)json_push_kv_int(data, "unissued_atoms",
                           (int64_t)plan->epoch.unissued_atoms);
    (void)json_push_kv_bool(data, "exact_attribution_sum", true);
    (void)json_push_kv_str(data, "mint_authority",
                           "simulation_accounting_only");
    zcs_safety(data, persisted);
}

static void zcs_epoch_handle(
    const struct zcl_command_request *request, struct zcl_command_reply *reply,
    bool persist)
{
    struct vcs_zcode_shadow_epoch_input input;
    struct vcs_zcode_shadow_epoch_plan plan;
    uint8_t roots[4][32], *wire = NULL;
    size_t wire_len = 0;
    if (!zcs_parse_epoch(request, reply, &input, roots)) return;
    enum vcs_zcode_shadow_simulation_error error =
        vcs_zcode_shadow_epoch_plan_cas(&input, &plan);
    if (error != VCS_ZCODE_SHADOW_SIMULATION_OK ||
        vcs_zcode_epoch_creation_serialize(&plan.epoch, &wire, &wire_len) !=
            VCS_ZCODE_EPOCH_CREATION_OK) {
        zcs_fail(reply, "SHADOW_EPOCH_REFUSED",
                 vcs_zcode_shadow_simulation_error_string(error));
        vcs_zcode_shadow_epoch_plan_free(&plan);
        free(wire);
        return;
    }
    if (persist &&
        (!vcs_object_store_init(input.workspace) ||
         !vcs_object_put_addressed(input.workspace, plan.epoch_root,
                                   wire, wire_len))) {
        zcs_fail(reply, "SHADOW_EPOCH_STORE_REFUSED",
                 "existing scratch CAS refused the canonical epoch");
        vcs_zcode_shadow_epoch_plan_free(&plan);
        free(wire);
        return;
    }
    zcs_render_epoch(&reply->data, &plan, persist);
    vcs_zcode_shadow_epoch_plan_free(&plan);
    free(wire);
}

void zcl_native_handle_zcode_commons_shadow_epoch_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcs_epoch_handle(request, reply, false);
}

void zcl_native_handle_zcode_commons_shadow_epoch_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcs_epoch_handle(request, reply, true);
}

void zcl_native_handle_zcode_commons_shadow_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcl_native_handle_zcode_commons_status(request, reply);
    if (reply && reply->status == ZCL_COMMAND_STATUS_PASSED)
        zcs_safety(&reply->data, false);
}

void zcl_native_handle_zcode_commons_shadow_verify(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcl_native_handle_zcode_commons_verify(request, reply);
    if (reply && reply->status == ZCL_COMMAND_STATUS_PASSED)
        zcs_safety(&reply->data, false);
}
