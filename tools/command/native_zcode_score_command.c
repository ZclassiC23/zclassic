/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: plan, commit and show canonical evidence-derived ZC23 Score. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_score_receipt.h"

#include <stdlib.h>
#include <string.h>

static const char *zsc_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zsc_keys(const struct json_value *input,
                     const char *const *allowed, size_t allowed_count)
{
    if (!input || input->type != JSON_OBJ) return false;
    for (size_t i = 0; i < input->num_children; i++) {
        bool known = false;
        for (size_t j = 0; j < allowed_count; j++)
            known = known || strcmp(input->keys[i], allowed[j]) == 0;
        if (!known) return false;
    }
    return true;
}

static bool zsc_hex_fixed(const struct json_value *input, const char *key,
                          uint8_t *out, size_t len)
{
    const char *hex = zsc_str(input, key);
    return hex && strlen(hex) == len * 2u &&
           zcl_hex_decode_lower(hex, out, len);
}

static void zsc_fail(struct zcl_command_reply *reply, const char *code,
                     const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.package.dev.score");
}

static void zsc_push_hex(struct json_value *data, const char *key,
                         const uint8_t *bytes, size_t len)
{
    char hex[VCS_ZCODE_SCORE_WIRE_BYTES * 2u + 1u];
    if (len > VCS_ZCODE_SCORE_WIRE_BYTES) return;
    zcl_hex_encode(bytes, len, hex);
    (void)json_push_kv_str(data, key, hex);
}

static void zsc_render(struct json_value *data,
                       const struct vcs_zcode_score_receipt_v1 *score)
{
    (void)json_push_kv_int(data, "score", score->score);
    (void)json_push_kv_int(data, "awarded_mask", score->awarded_mask);
    (void)json_push_kv_str(data, "credit_class", "zc23_score");
    (void)json_push_kv_bool(data, "independent_reproduction_withheld",
        (score->awarded_mask &
         (1u << VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION)) == 0);
    struct json_value units;
    json_init(&units); json_set_array(&units);
    for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
        struct json_value unit;
        json_init(&unit); json_set_object(&unit);
        bool awarded = (score->awarded_mask & (1u << i)) != 0;
        (void)json_push_kv_str(&unit, "unit",
            vcs_zcode_score_unit_name((enum vcs_zcode_score_unit)i));
        (void)json_push_kv_bool(&unit, "awarded", awarded);
        zsc_push_hex(&unit, "evidence_root", score->evidence_roots[i], 32);
        if (i == VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION && !awarded)
            (void)json_push_kv_str(&unit, "reason",
                                   "no_approved_offhost_signer");
        (void)json_push_back(&units, &unit); json_free(&unit);
    }
    (void)json_push_kv(data, "units", &units); json_free(&units);
}

void zcl_native_handle_zcode_package_dev_score_plan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {
        "workspace", "task_hex", "candidate_hex", "proof_policy_hex",
        "proof_set_hex", "proven_lane_hex", "package_root",
        "release_root", "recipe_root", "dependency_lock_root",
        "api_capsule_root",
    };
    if (!request || !reply) return;
    const struct json_value *input = request->input;
    const char *workspace = zsc_str(input, "workspace");
    uint8_t task_wire[VCS_ZCODE_TASK_WIRE_BYTES];
    uint8_t candidate_wire[VCS_ZCODE_CANDIDATE_WIRE_BYTES];
    uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
    uint8_t lane_wire[VCS_ZCODE_LANE_WIRE_BYTES];
    uint8_t proof_wire[VCS_ZCODE_PROOF_SET_WIRE_MAX];
    const char *proof_hex = zsc_str(input, "proof_set_hex");
    size_t proof_len = proof_hex ? strlen(proof_hex) / 2u : 0;
    if (!zsc_keys(input, keys, sizeof(keys) / sizeof(keys[0])) || !workspace ||
        !zsc_hex_fixed(input, "task_hex", task_wire, sizeof(task_wire)) ||
        !zsc_hex_fixed(input, "candidate_hex", candidate_wire,
                       sizeof(candidate_wire)) ||
        !zsc_hex_fixed(input, "proof_policy_hex", policy_wire,
                       sizeof(policy_wire)) ||
        !zsc_hex_fixed(input, "proven_lane_hex", lane_wire,
                       sizeof(lane_wire)) || !proof_hex ||
        (strlen(proof_hex) & 1u) != 0 || proof_len > sizeof(proof_wire) ||
        !zcl_hex_decode_lower(proof_hex, proof_wire, proof_len)) {
        zsc_fail(reply, "BAD_SCORE_PLAN", "closed score-plan input refused");
        return;
    }
    struct vcs_zcode_task_v1 task;
    struct vcs_zcode_candidate_v1 candidate;
    struct vcs_zcode_proof_policy_v1 policy;
    struct vcs_zcode_lane_receipt_v1 lane;
    uint8_t roots[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32]; size_t count = 0;
    if (vcs_zcode_task_parse(task_wire, sizeof(task_wire), &task) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_parse(candidate_wire, sizeof(candidate_wire),
                                  &candidate) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_policy_parse(policy_wire, sizeof(policy_wire),
                                     &policy) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_parse(lane_wire, sizeof(lane_wire), &lane) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_set_parse(proof_wire, proof_len, roots,
            VCS_ZCODE_PROOF_SET_MAX_RECEIPTS, &count) != VCS_ZCODE_DEV_OK ||
        count == 0) {
        zsc_fail(reply, "BAD_SCORE_OBJECT", "a canonical input wire refused");
        return;
    }
    struct vcs_zcode_work_receipt_v1 receipts[
        VCS_ZCODE_PROOF_SET_MAX_RECEIPTS];
    for (size_t i = 0; i < count; i++) {
        uint8_t *wire = NULL; size_t len = 0;
        if (vcs_object_load_raw_bounded(workspace, roots[i],
                VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES, &wire, &len) != 0 ||
            vcs_zcode_work_receipt_parse(wire, len, &receipts[i]) !=
                VCS_ZCODE_DEV_OK) {
            free(wire);
            zsc_fail(reply, "MISSING_WORK_RECEIPT",
                     "proof-set receipt absent or corrupt in workspace CAS");
            return;
        }
        free(wire);
    }
    uint8_t package[32], release[32], recipe[32], lock[32], capsule[32];
    if (!zsc_hex_fixed(input, "package_root", package, 32) ||
        !zsc_hex_fixed(input, "release_root", release, 32) ||
        !zsc_hex_fixed(input, "recipe_root", recipe, 32) ||
        !zsc_hex_fixed(input, "dependency_lock_root", lock, 32) ||
        !zsc_hex_fixed(input, "api_capsule_root", capsule, 32)) {
        zsc_fail(reply, "BAD_SCORE_ROOT", "all five package roots are required");
        return;
    }
    struct vcs_zcode_score_plan_input planned = {
        .task = &task, .candidate = &candidate, .proof_policy = &policy,
        .proven_lane = &lane, .proof_receipt_roots = roots,
        .work_receipts = receipts, .work_receipt_count = count,
        .package_root = package, .release_root = release,
        .recipe_root = recipe, .dependency_lock_root = lock,
        .api_capsule_root = capsule,
    };
    struct vcs_zcode_score_receipt_v1 score;
    enum vcs_zcode_score_error err = vcs_zcode_score_plan(&planned, &score);
    uint8_t body[VCS_ZCODE_SCORE_BODY_BYTES], digest[32];
    if (err != VCS_ZCODE_SCORE_OK ||
        vcs_zcode_score_receipt_body(&score, body) != VCS_ZCODE_SCORE_OK ||
        vcs_zcode_score_receipt_id(&score, digest) != VCS_ZCODE_SCORE_OK) {
        zsc_fail(reply, "SCORE_PLAN_REFUSED",
                 vcs_zcode_score_error_string(err));
        return;
    }
    zsc_render(&reply->data, &score);
    zsc_push_hex(&reply->data, "receipt_body_hex", body, sizeof(body));
    zsc_push_hex(&reply->data, "signing_digest", digest, sizeof(digest));
    (void)json_push_kv_bool(&reply->data, "persisted", false);
}

void zcl_native_handle_zcode_package_dev_score_commit(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace", "receipt_hex"};
    if (!request || !reply) return;
    const char *workspace = zsc_str(request->input, "workspace");
    uint8_t wire[VCS_ZCODE_SCORE_WIRE_BYTES];
    if (!zsc_keys(request->input, keys, 2) || !workspace ||
        !zsc_hex_fixed(request->input, "receipt_hex", wire, sizeof(wire))) {
        zsc_fail(reply, "BAD_SCORE_COMMIT", "workspace and exact receipt_hex required");
        return;
    }
    struct vcs_zcode_score_receipt_v1 score; uint8_t root[32];
    if (vcs_zcode_score_receipt_parse(wire, sizeof(wire), &score) !=
            VCS_ZCODE_SCORE_OK ||
        vcs_zcode_score_receipt_verify(&score) != VCS_ZCODE_SCORE_OK ||
        vcs_zcode_score_receipt_id(&score, root) != VCS_ZCODE_SCORE_OK ||
        !vcs_object_store_init(workspace) ||
        !vcs_object_put_addressed(workspace, root, wire, sizeof(wire))) {
        zsc_fail(reply, "SCORE_COMMIT_REFUSED", "receipt signature or workspace CAS write refused");
        return;
    }
    zsc_render(&reply->data, &score);
    zsc_push_hex(&reply->data, "score_receipt_root", root, sizeof(root));
    (void)json_push_kv_bool(&reply->data, "persisted", true);
}

void zcl_native_handle_zcode_package_dev_score_show(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    static const char *const keys[] = {"workspace", "root"};
    if (!request || !reply) return;
    const char *workspace = zsc_str(request->input, "workspace");
    uint8_t root[32], *wire = NULL; size_t len = 0;
    if (!zsc_keys(request->input, keys, 2) || !workspace ||
        !zsc_hex_fixed(request->input, "root", root, sizeof(root)) ||
        vcs_object_load_raw_bounded(workspace, root,
            VCS_ZCODE_SCORE_WIRE_BYTES, &wire, &len) != 0) {
        free(wire); zsc_fail(reply, "SCORE_NOT_FOUND", "score receipt is absent");
        return;
    }
    struct vcs_zcode_score_receipt_v1 score; uint8_t derived[32];
    bool ok = vcs_zcode_score_receipt_parse(wire, len, &score) ==
            VCS_ZCODE_SCORE_OK &&
        vcs_zcode_score_receipt_verify(&score) == VCS_ZCODE_SCORE_OK &&
        vcs_zcode_score_receipt_id(&score, derived) == VCS_ZCODE_SCORE_OK &&
        memcmp(root, derived, 32) == 0;
    free(wire);
    if (!ok) { zsc_fail(reply, "SCORE_CORRUPT", "stored receipt did not reverify"); return; }
    zsc_render(&reply->data, &score);
    zsc_push_hex(&reply->data, "score_receipt_root", root, sizeof(root));
}
