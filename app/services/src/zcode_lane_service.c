/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: CAS-authoritative ZCODE lane promotion over the ZBuild ledger. */

#include "services/zcode_lane_service.h"

#include "base/hex.h"
#include "models/build_fabric.h"
#include "models/zcode_lane.h"
#include "services/build_fabric_service.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_lane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool lane_root_nonzero(const uint8_t root[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++) any |= root[i];
    return any != 0;
}

static bool lane_load_raw(const char *workspace, const char *hex,
                          uint8_t **wire, size_t *wire_len, uint8_t root[32])
{
    return zcl_hex_decode_lower(hex, root, 32) &&
           vcs_object_load_raw(workspace, root, wire, wire_len) == 0;
}

static struct zcl_result lane_load_context(
    const char *workspace, const struct db_build_action *action,
    struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_candidate_v1 *candidate,
    struct vcs_zcode_proof_policy_v1 *policy)
{
    uint8_t *wire = NULL, root[32], checked[32]; size_t wire_len = 0;
    if (!lane_load_raw(workspace, action->task_root_sha3,
                       &wire, &wire_len, root) ||
        vcs_zcode_task_parse(wire, wire_len, task) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_task_root(task, checked) != VCS_ZCODE_DEV_OK ||
        memcmp(root, checked, 32) != 0) {
        free(wire); return ZCL_ERR(-1, "lane-task-cas-invalid");
    }
    free(wire); wire = NULL;
    if (!lane_load_raw(workspace, action->candidate_root_sha3,
                       &wire, &wire_len, root) ||
        vcs_zcode_candidate_parse(wire, wire_len, candidate) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_root(candidate, checked) != VCS_ZCODE_DEV_OK ||
        memcmp(root, checked, 32) != 0) {
        free(wire); return ZCL_ERR(-1, "lane-candidate-cas-invalid");
    }
    free(wire); wire = NULL;
    if (!lane_load_raw(workspace, action->proof_policy_root_sha3,
                       &wire, &wire_len, root) ||
        vcs_zcode_proof_policy_parse(wire, wire_len, policy) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_policy_root(policy, checked) != VCS_ZCODE_DEV_OK ||
        memcmp(root, checked, 32) != 0) {
        free(wire); return ZCL_ERR(-1, "lane-policy-cas-invalid");
    }
    free(wire);
    return ZCL_OK;
}

static bool lane_load_receipt(
    const char *workspace, const char *receipt_hex,
    struct vcs_zcode_lane_receipt_v1 *receipt)
{
    uint8_t root[32], checked[32], *wire = NULL; size_t wire_len = 0;
    bool ok = lane_load_raw(workspace, receipt_hex, &wire, &wire_len, root) &&
        vcs_zcode_lane_receipt_parse(wire, wire_len, receipt) ==
            VCS_ZCODE_DEV_OK &&
        vcs_zcode_lane_receipt_id(receipt, checked) == VCS_ZCODE_DEV_OK &&
        memcmp(root, checked, 32) == 0 &&
        vcs_zcode_lane_receipt_verify(receipt, receipt->signer_pubkey) ==
            VCS_ZCODE_DEV_OK;
    free(wire);
    return ok;
}

static void lane_status_from_row(const struct db_zcode_lane_receipt *row,
                                 struct zcode_lane_status *out)
{
    memset(out, 0, sizeof(*out));
    out->lane = row->lane;
    (void)snprintf(out->lane_name, sizeof(out->lane_name), "%s",
                   vcs_zcode_lane_name((uint8_t)row->lane));
    (void)snprintf(out->source_root_sha3, sizeof(out->source_root_sha3),
                   "%s", row->source_root_sha3);
    (void)snprintf(out->task_root_sha3, sizeof(out->task_root_sha3),
                   "%s", row->task_root_sha3);
    (void)snprintf(out->candidate_root_sha3, sizeof(out->candidate_root_sha3),
                   "%s", row->candidate_root_sha3);
    (void)snprintf(out->proof_policy_root_sha3,
                   sizeof(out->proof_policy_root_sha3), "%s",
                   row->proof_policy_root_sha3);
    (void)snprintf(out->proof_set_root_sha3,
                   sizeof(out->proof_set_root_sha3), "%s",
                   row->proof_set_root_sha3);
    (void)snprintf(out->receipt_root_sha3, sizeof(out->receipt_root_sha3),
                   "%s", row->receipt_id);
    (void)snprintf(out->prior_receipt_root_sha3,
                   sizeof(out->prior_receipt_root_sha3), "%s",
                   row->prior_receipt_root_sha3);
    (void)snprintf(out->signer_pubkey, sizeof(out->signer_pubkey), "%s",
                   row->signer_pubkey);
    out->created_at = row->created_at;
}

static bool lane_row_matches_receipt(
    const struct db_zcode_lane_receipt *row,
    const struct vcs_zcode_lane_receipt_v1 *receipt)
{
    char hex[65];
    zcl_hex_encode(receipt->source_root, 32, hex);
    if (strcmp(hex, row->source_root_sha3) != 0) return false;
    zcl_hex_encode(receipt->task_root, 32, hex);
    if (strcmp(hex, row->task_root_sha3) != 0) return false;
    zcl_hex_encode(receipt->candidate_root, 32, hex);
    if (strcmp(hex, row->candidate_root_sha3) != 0) return false;
    zcl_hex_encode(receipt->proof_policy_root, 32, hex);
    if (strcmp(hex, row->proof_policy_root_sha3) != 0) return false;
    zcl_hex_encode(receipt->proof_set_root, 32, hex);
    if ((lane_root_nonzero(receipt->proof_set_root) &&
         strcmp(hex, row->proof_set_root_sha3) != 0) ||
        (!lane_root_nonzero(receipt->proof_set_root) &&
         row->proof_set_root_sha3[0]))
        return false;
    zcl_hex_encode(receipt->prior_receipt_root, 32, hex);
    if ((lane_root_nonzero(receipt->prior_receipt_root) &&
         strcmp(hex, row->prior_receipt_root_sha3) != 0) ||
        (!lane_root_nonzero(receipt->prior_receipt_root) &&
         row->prior_receipt_root_sha3[0]))
        return false;
    zcl_hex_encode(receipt->signer_pubkey, 32, hex);
    if (strcmp(hex, row->signer_pubkey) != 0) return false;
    return receipt->lane == row->lane &&
           receipt->created_unix == row->created_at;
}

struct zcl_result zcode_lane_find(
    struct node_db *ndb, const char *workspace,
    const char *source_root_sha3, struct zcode_lane_status *out)
{
    struct db_zcode_lane_receipt row;
    struct vcs_zcode_lane_receipt_v1 receipt;
    if (!ndb || !ndb->open || !workspace || !source_root_sha3 || !out ||
        !db_zcode_lane_latest(ndb, source_root_sha3, &row))
        return ZCL_ERR(-1, "zcode-lane-not-found");
    if (!lane_load_receipt(workspace, row.receipt_id, &receipt) ||
        !lane_row_matches_receipt(&row, &receipt))
        return ZCL_ERR(-1, "zcode-lane-projection-or-cas-corrupt");
    lane_status_from_row(&row, out);
    return ZCL_OK;
}

static struct zcl_result lane_prior_validate(
    const char *workspace, const struct db_zcode_lane_receipt *prior,
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct vcs_zcode_proof_policy_v1 *policy,
    const uint8_t signer_pubkey[32])
{
    struct vcs_zcode_lane_receipt_v1 receipt;
    if (!lane_load_receipt(workspace, prior->receipt_id, &receipt) ||
        !lane_row_matches_receipt(prior, &receipt) ||
        vcs_zcode_lane_receipt_validate_for_candidate(
            &receipt, task, candidate, policy) != VCS_ZCODE_DEV_OK ||
        memcmp(receipt.signer_pubkey, signer_pubkey, 32) != 0)
        return ZCL_ERR(-1, "prior-lane-receipt-invalid-or-wrong-signer");
    return ZCL_OK;
}

// long-function-ok:promotion-transaction — proof evaluation, prior-chain
// verification, CAS write, and model projection form one fail-closed ritual.
struct zcl_result zcode_lane_advance(
    struct node_db *ndb, const char *workspace, const char *action_id,
    int target_lane, int64_t now, const uint8_t signer_secret[32],
    const uint8_t signer_pubkey[32], struct zcode_lane_status *out)
{
    if (!ndb || !ndb->open || !workspace || !action_id || now <= 0 ||
        !signer_secret || !signer_pubkey || !out ||
        target_lane < VCS_ZCODE_LANE_FRONTIER ||
        target_lane > VCS_ZCODE_LANE_PROVEN)
        return ZCL_ERR(-1, "lane-advance-input-invalid");
    struct db_build_action action;
    if (!db_build_action_find(ndb, action_id, &action) ||
        !action.task_root_sha3[0])
        return ZCL_ERR(-1, "lane-action-not-found");
    struct vcs_zcode_task_v1 task;
    struct vcs_zcode_candidate_v1 candidate;
    struct vcs_zcode_proof_policy_v1 policy;
    ZCL_CHECK(lane_load_context(
        workspace, &action, &task, &candidate, &policy));
    if (vcs_zcode_candidate_validate_for_task(&task, &candidate, now) !=
            VCS_ZCODE_DEV_OK)
        return ZCL_ERR(-1, "lane-candidate-stale-or-expired");
    char source_hex[65];
    zcl_hex_encode(candidate.candidate_source_root, 32, source_hex);
    struct db_zcode_lane_receipt prior;
    bool have_prior = db_zcode_lane_latest(ndb, source_hex, &prior);
    if (have_prior && prior.lane >= target_lane) {
        if (prior.lane > target_lane)
            return ZCL_ERR(-1, "lane-downgrade-refused");
        if (strcmp(prior.task_root_sha3, action.task_root_sha3) != 0 ||
            strcmp(prior.candidate_root_sha3,
                   action.candidate_root_sha3) != 0 ||
            strcmp(prior.proof_policy_root_sha3,
                   action.proof_policy_root_sha3) != 0)
            return ZCL_ERR(-1, "lane-idempotency-context-mismatch");
        ZCL_CHECK(lane_prior_validate(
            workspace, &prior, &task, &candidate, &policy, signer_pubkey));
        lane_status_from_row(&prior, out);
        return ZCL_OK;
    }
    if ((!have_prior && target_lane != VCS_ZCODE_LANE_FRONTIER) ||
        (have_prior && target_lane != prior.lane + 1))
        return ZCL_ERR(-1, "lane-transition-must-be-sequential");
    if (have_prior)
        ZCL_CHECK(lane_prior_validate(
            workspace, &prior, &task, &candidate, &policy, signer_pubkey));
    if (have_prior && now < prior.created_at)
        return ZCL_ERR(-1, "lane-promotion-time-precedes-prior-receipt");
    struct build_fabric_proof_evaluation evaluation = {0};
    if (target_lane != VCS_ZCODE_LANE_FRONTIER) {
        ZCL_CHECK(build_fabric_proof_evaluate(
            ndb, workspace, action_id, now, &evaluation));
        bool candidate_ready = evaluation.compile_satisfied &&
            (!(policy.required_proofs & VCS_ZCODE_PROOF_TEST) ||
             evaluation.test_satisfied);
        if (target_lane == VCS_ZCODE_LANE_CANDIDATE && !candidate_ready)
            return ZCL_ERR(-1, "candidate-fast-proof-policy-unsatisfied");
        if (target_lane == VCS_ZCODE_LANE_PROVEN &&
            !evaluation.policy_satisfied)
            return ZCL_ERR(-1, "proven-proof-policy-unsatisfied");
    }
    struct vcs_zcode_lane_receipt_v1 receipt = {
        .schema_version = VCS_ZCODE_DEV_VERSION,
        .lane = (uint8_t)target_lane,
        .created_unix = now,
    };
    memcpy(receipt.source_root, candidate.candidate_source_root, 32);
    (void)zcl_hex_decode_lower(action.task_root_sha3, receipt.task_root, 32);
    (void)zcl_hex_decode_lower(
        action.candidate_root_sha3, receipt.candidate_root, 32);
    (void)zcl_hex_decode_lower(
        action.proof_policy_root_sha3, receipt.proof_policy_root, 32);
    if (target_lane != VCS_ZCODE_LANE_FRONTIER) {
        if (!zcl_hex_decode_lower(evaluation.proof_set_root_sha3,
                                  receipt.proof_set_root, 32) ||
            !zcl_hex_decode_lower(prior.receipt_id,
                                  receipt.prior_receipt_root, 32))
            return ZCL_ERR(-1, "lane-proof-or-prior-root-invalid");
    }
    if (vcs_zcode_lane_receipt_seal(
            &receipt, signer_secret, signer_pubkey) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_validate_for_candidate(
            &receipt, &task, &candidate, &policy) != VCS_ZCODE_DEV_OK)
        return ZCL_ERR(-1, "lane-receipt-seal-refused");
    uint8_t wire[VCS_ZCODE_LANE_WIRE_BYTES], root[32]; char root_hex[65];
    if (vcs_zcode_lane_receipt_serialize(&receipt, wire) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_id(&receipt, root) != VCS_ZCODE_DEV_OK)
        return ZCL_ERR(-1, "lane-receipt-encoding-failed");
    zcl_hex_encode(root, 32, root_hex);
    if (!vcs_object_put_addressed(workspace, root, wire, sizeof(wire)))
        return ZCL_ERR(-1, "lane-receipt-cas-store-failed");
    struct vcs_zcode_lane_receipt_v1 persisted_receipt;
    uint8_t persisted_wire[VCS_ZCODE_LANE_WIRE_BYTES];
    if (!lane_load_receipt(workspace, root_hex, &persisted_receipt) ||
        vcs_zcode_lane_receipt_serialize(
            &persisted_receipt, persisted_wire) != VCS_ZCODE_DEV_OK ||
        memcmp(persisted_wire, wire, sizeof(wire)) != 0)
        return ZCL_ERR(-1, "lane-receipt-cas-readback-mismatch");
    struct db_zcode_lane_receipt row = { .lane = target_lane,
                                        .created_at = now };
    (void)snprintf(row.receipt_id, sizeof(row.receipt_id), "%s", root_hex);
    (void)snprintf(row.source_root_sha3, sizeof(row.source_root_sha3),
                   "%s", source_hex);
    (void)snprintf(row.task_root_sha3, sizeof(row.task_root_sha3), "%s",
                   action.task_root_sha3);
    (void)snprintf(row.candidate_root_sha3,
                   sizeof(row.candidate_root_sha3), "%s",
                   action.candidate_root_sha3);
    (void)snprintf(row.proof_policy_root_sha3,
                   sizeof(row.proof_policy_root_sha3), "%s",
                   action.proof_policy_root_sha3);
    if (target_lane != VCS_ZCODE_LANE_FRONTIER) {
        (void)snprintf(row.proof_set_root_sha3,
                       sizeof(row.proof_set_root_sha3), "%s",
                       evaluation.proof_set_root_sha3);
        (void)snprintf(row.prior_receipt_root_sha3,
                       sizeof(row.prior_receipt_root_sha3), "%s",
                       prior.receipt_id);
    }
    zcl_hex_encode(signer_pubkey, 32, row.signer_pubkey);
    if (!db_zcode_lane_receipt_save(ndb, &row))
        return ZCL_ERR(-1, "lane-receipt-projection-save-failed");
    struct db_zcode_lane_receipt stored;
    if (!db_zcode_lane_receipt_find(ndb, root_hex, &stored) ||
        !lane_row_matches_receipt(&stored, &receipt))
        return ZCL_ERR(-1, "lane-receipt-projection-verify-failed");
    lane_status_from_row(&stored, out);
    return ZCL_OK;
}
