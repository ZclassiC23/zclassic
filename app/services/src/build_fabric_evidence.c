/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Durable remote ZBuild evidence observation and trust evaluation. */

#include "services/build_fabric_service.h"

#include "base/hex.h"
#include "base/serialize_le.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"
#include "vcs/build_action.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_work_swarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BF_TEST_EVIDENCE_BYTES 84u
#define BF_FUZZ_EVIDENCE_BYTES 96u

struct zcl_result build_fabric_test_evidence_parse(
    const uint8_t *bytes, size_t len, uint8_t *status, int *exit_status)
{
    if (!bytes || len != BF_TEST_EVIDENCE_BYTES ||
        memcmp(bytes, "ZCTEST\r\n", 8) != 0 || bytes[8] != 1 ||
        bytes[9] != 0 || (bytes[10] != 1 && bytes[10] != 2) ||
        (bytes[11] & ~UINT8_C(7)) != 0 || !status || !exit_status)
        return ZCL_ERR(-1, "test-evidence-wire-invalid");
    uint32_t code = zcl_read_u32_le(bytes + 12);
    uint32_t signal = zcl_read_u32_le(bytes + 16);
    if (bytes[10] == 1 &&
        (code != 0 || signal != 0 || (bytes[11] & UINT8_C(1)) != 0))
        return ZCL_ERR(-1, "passing-test-evidence-reports-failure");
    *status = bytes[10] == 1 ? VCS_ZCODE_WORK_PASS : VCS_ZCODE_WORK_FAIL;
    *exit_status = *status == VCS_ZCODE_WORK_PASS ? 0
        : code > 0 && code <= 255 ? (int)code : 255;
    return ZCL_OK;
}

struct zcl_result build_fabric_fuzz_evidence_parse(
    const uint8_t *bytes, size_t len, uint32_t expected_seeds,
    uint8_t *status, int *exit_status)
{
    if (!bytes || len != BF_FUZZ_EVIDENCE_BYTES ||
        memcmp(bytes, "ZCFUZZ\r\n", 8) != 0 || bytes[8] != 1 ||
        bytes[9] != 0 || (bytes[10] != 1 && bytes[10] != 2) ||
        (bytes[11] & ~UINT8_C(7)) != 0 || !status || !exit_status)
        return ZCL_ERR(-1, "fuzz-evidence-wire-invalid");
    uint32_t seeds = zcl_read_u32_le(bytes + 12);
    uint32_t completed = zcl_read_u32_le(bytes + 16);
    uint32_t failing_seed = zcl_read_u32_le(bytes + 20);
    uint32_t code = zcl_read_u32_le(bytes + 24);
    uint32_t signal = zcl_read_u32_le(bytes + 28);
    if (seeds == 0 || seeds != expected_seeds || completed == 0 ||
        completed > seeds)
        return ZCL_ERR(-1, "fuzz-evidence-seed-range-invalid");
    if (bytes[10] == 1) {
        if (completed != seeds || failing_seed != UINT32_MAX || code != 0 ||
            signal != 0 || (bytes[11] & UINT8_C(1)) != 0)
            return ZCL_ERR(-1, "passing-fuzz-evidence-is-inconsistent");
        *status = VCS_ZCODE_WORK_PASS;
        *exit_status = 0;
    } else {
        if (failing_seed >= seeds || completed != failing_seed + 1u)
            return ZCL_ERR(-1, "failing-fuzz-evidence-seed-invalid");
        *status = VCS_ZCODE_WORK_FAIL;
        *exit_status = code > 0 && code <= 255 ? (int)code : 255;
    }
    return ZCL_OK;
}

static void bf_worker_id_from_pubkey(const uint8_t pubkey[32],
                                     char out[65])
{
    static const char domain[] = "zcl.build_worker.v1";
    struct sha3_256_ctx sha;
    uint8_t digest[32];
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, pubkey, 32);
    sha3_256_finalize(&sha, digest);
    zcl_hex_encode(digest, sizeof(digest), out);
}

struct zcl_result build_fabric_receipt_observe_remote(
    struct node_db *ndb, const char *workspace,
    const struct vcs_zcode_work_request_v1 *request,
    const struct vcs_zcode_work_result_v1 *result, int64_t now,
    char receipt_id[BUILD_FABRIC_ID_HEX + 1])
{
    if (receipt_id) receipt_id[0] = '\0';
    if (!ndb || !ndb->open || !workspace || !workspace[0] || !request ||
        !result || !receipt_id || now < 0)
        return ZCL_ERR(-1, "remote receipt observation requires complete inputs");
    const uint8_t *signer = result->receipt.signer_pubkey;
    if (!vcs_zcode_work_request_verify(request) ||
        !vcs_zcode_work_result_verify(request, result, signer))
        return ZCL_ERR(-1, "remote result signature or request binding is invalid");
    char action_id[65];
    zcl_hex_encode(result->action_root, 32, action_id);
    struct db_build_action action;
    if (!db_build_action_find(ndb, action_id, &action) ||
        strcmp(action.task_root_sha3, "") == 0 ||
        strcmp(action.candidate_root_sha3, "") == 0)
        return ZCL_ERR(-1, "remote result has no matching local ZBuild action");
    uint8_t expected_kind = vcs_build_action_v1_work_kind(action.kind);
    if (expected_kind == 0 || request->work_kind != expected_kind ||
        result->receipt.work_kind != expected_kind)
        return ZCL_ERR(-1, "remote receipt work kind conflicts with the fixed action");
    uint8_t expected[32];
    if (!zcl_hex_decode_lower(action.task_root_sha3, expected, 32) ||
        memcmp(expected, result->task_root, 32) != 0 ||
        !zcl_hex_decode_lower(action.candidate_root_sha3, expected, 32) ||
        memcmp(expected, result->candidate_root, 32) != 0 ||
        !zcl_hex_decode_lower(action.input_root_sha3, expected, 32) ||
        memcmp(expected, result->receipt.input_root, 32) != 0 ||
        !zcl_hex_decode_lower(action.proof_policy_root_sha3, expected, 32) ||
        memcmp(expected, result->receipt.proof_policy_root, 32) != 0)
        return ZCL_ERR(-1, "remote receipt conflicts with the local action");
    uint8_t work_wire[VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES];
    uint8_t work_root[32];
    if (vcs_zcode_work_receipt_serialize(&result->receipt, work_wire) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_work_receipt_id(&result->receipt, work_root) !=
            VCS_ZCODE_DEV_OK ||
        !vcs_object_store_init(workspace) ||
        !vcs_object_put_addressed(workspace, work_root, work_wire,
                                  sizeof(work_wire)))
        return ZCL_ERR(-1, "remote canonical receipt could not enter CAS");
    zcl_hex_encode(work_root, 32, receipt_id);
    struct db_build_receipt prior;
    if (db_build_receipt_find(ndb, receipt_id, &prior)) {
        if (strcmp(prior.action_id, action_id) == 0 &&
            strcmp(prior.work_receipt_sha3, receipt_id) == 0)
            return ZCL_OK;
        return ZCL_ERR(-1, "remote receipt id collides with different evidence");
    }
    struct db_build_worker worker;
    char worker_id[65];
    bf_worker_id_from_pubkey(signer, worker_id);
    if (db_build_worker_find(ndb, worker_id, &worker)) {
        char signer_hex[65];
        zcl_hex_encode(signer, 32, signer_hex);
        if (strcmp(worker.signer_pubkey, signer_hex) != 0)
            return ZCL_ERR(-1, "remote signer collides with a worker identity");
        worker.last_seen_at = now;
    } else {
        memset(&worker, 0, sizeof(worker));
        (void)snprintf(worker.worker_id, sizeof(worker.worker_id), "%s",
                       worker_id);
        zcl_hex_encode(signer, 32, worker.signer_pubkey);
        (void)snprintf(worker.capabilities, sizeof(worker.capabilities),
                       "p2p-observed,%s", action.kind);
        worker.last_seen_at = now;
    }
    struct db_build_receipt observed = {0};
    (void)snprintf(observed.receipt_id, sizeof(observed.receipt_id), "%s",
                   receipt_id);
    (void)snprintf(observed.action_id, sizeof(observed.action_id), "%s",
                   action_id);
    (void)snprintf(observed.job_id, sizeof(observed.job_id), "%s",
                   action.job_id);
    (void)snprintf(observed.worker_id, sizeof(observed.worker_id), "%s",
                   worker_id);
    zcl_hex_encode(result->receipt.lease_id, 32, observed.lease_id);
    (void)snprintf(observed.action_sha3, sizeof(observed.action_sha3), "%s",
                   action_id);
    zcl_hex_encode(result->output_root, 32, observed.output_sha3);
    (void)snprintf(observed.work_receipt_sha3,
                   sizeof(observed.work_receipt_sha3), "%s", receipt_id);
    zcl_hex_encode(result->receipt.signature, 64, observed.signature);
    (void)snprintf(observed.confinement, sizeof(observed.confinement),
                   "p2p-untrusted;facts-in-work-receipt");
    (void)snprintf(observed.trust_state, sizeof(observed.trust_state),
                   "REMOTE_OBSERVED");
    observed.exit_status = result->receipt.exit_status;
    observed.created_at = result->receipt.finished_unix;
    if (!node_db_begin(ndb))
        return ZCL_ERR(-1, "cannot begin remote receipt observation");
    bool ok = db_build_worker_save(ndb, &worker) &&
              db_build_receipt_save(ndb, &observed) && node_db_commit(ndb);
    if (!ok) {
        if (!node_db_rollback(ndb))
            LOG_ERROR("build_fabric", "remote receipt save and rollback failed");
        return ZCL_ERR(-1, "remote receipt observation could not persist");
    }
    return ZCL_OK;
}

struct bf_verified_receipt {
    struct db_build_receipt row;
    struct vcs_zcode_work_receipt_v1 receipt;
    uint8_t root[32];
    uint8_t work_kind;
    bool approved;
    bool local;
    bool review_approved;
};

static int bf_root_compare(const void *left, const void *right)
{
    return memcmp(left, right, 32);
}

static bool bf_load_dev_object(const char *workspace, const char *root_hex,
                               uint8_t **wire, size_t *wire_len,
                               uint8_t root[32])
{
    return zcl_hex_decode_lower(root_hex, root, 32) &&
           vcs_object_load_raw(workspace, root, wire, wire_len) == 0;
}

static bool bf_receipt_trusted(const struct bf_verified_receipt *receipt)
{
    return receipt && (receipt->local || receipt->approved);
}

static bool bf_trusted_evidence_has_root(
    const struct bf_verified_receipt *valid, size_t valid_count,
    const uint8_t root[32])
{
    for (size_t i = 0; i < valid_count; i++)
        if (valid[i].work_kind != VCS_ZCODE_WORK_REVIEW &&
            bf_receipt_trusted(&valid[i]) &&
            memcmp(valid[i].root, root, 32) == 0)
            return true;
    return false;
}

static bool bf_review_approves_evidence(
    const char *workspace, const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct bf_verified_receipt *valid, size_t valid_count,
    size_t review_index, int64_t now)
{
    const struct bf_verified_receipt *entry = &valid[review_index];
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (vcs_object_load_raw(workspace, entry->receipt.output_root,
                            &wire, &wire_len) != 0)
        return false;
    struct vcs_zcode_review_v1 review;
    bool ok = vcs_zcode_review_parse(wire, wire_len, &review) ==
                  VCS_ZCODE_DEV_OK &&
        vcs_zcode_review_validate_for_candidate(
            task, candidate, &review, now) == VCS_ZCODE_DEV_OK &&
        review.verdict == VCS_ZCODE_REVIEW_APPROVE &&
        memcmp(review.reviewer_pubkey, entry->receipt.signer_pubkey, 32) == 0;
    free(wire);
    if (!ok) return false;
    uint8_t proof_roots[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32];
    size_t proof_count = 0;
    wire = NULL; wire_len = 0;
    if (vcs_object_load_raw(workspace, review.proof_set_root,
                            &wire, &wire_len) != 0 ||
        vcs_zcode_proof_set_parse(
            wire, wire_len, proof_roots,
            VCS_ZCODE_PROOF_SET_MAX_RECEIPTS, &proof_count) !=
                VCS_ZCODE_DEV_OK) {
        free(wire);
        return false;
    }
    free(wire);
    for (size_t i = 0; i < proof_count; i++)
        if (!bf_trusted_evidence_has_root(
                valid, valid_count, proof_roots[i]))
            return false;
    return true;
}

static bool bf_has_local_match(
    const struct bf_verified_receipt *valid, size_t valid_count,
    size_t remote_index);

static size_t bf_count_kind(
    const struct bf_verified_receipt *valid, size_t valid_count,
    uint8_t work_kind, const uint8_t *required_output, bool independent)
{
    uint8_t signers[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32];
    size_t count = 0;
    for (size_t i = 0; i < valid_count; i++) {
        bool trusted = bf_receipt_trusted(&valid[i]) ||
                       bf_has_local_match(valid, valid_count, i);
        if (valid[i].work_kind != work_kind ||
            !trusted ||
            (work_kind == VCS_ZCODE_WORK_REVIEW &&
             !valid[i].review_approved) ||
            (required_output && memcmp(valid[i].receipt.output_root,
                                       required_output, 32) != 0))
            continue;
        bool duplicate = false;
        if (independent)
            for (size_t j = 0; j < count; j++)
                if (memcmp(signers[j], valid[i].receipt.signer_pubkey,
                           32) == 0)
                    duplicate = true;
        if (!duplicate) {
            memcpy(signers[count], valid[i].receipt.signer_pubkey, 32);
            count++;
        }
    }
    return count;
}

static size_t bf_count_approved_kind(
    const struct bf_verified_receipt *valid, size_t valid_count,
    uint8_t work_kind, const uint8_t *required_output, bool independent)
{
    uint8_t signers[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32];
    size_t count = 0;
    for (size_t i = 0; i < valid_count; i++) {
        if (valid[i].work_kind != work_kind || !valid[i].approved ||
            (required_output && memcmp(valid[i].receipt.output_root,
                                       required_output, 32) != 0))
            continue;
        bool duplicate = false;
        if (independent)
            for (size_t j = 0; j < count; j++)
                if (memcmp(signers[j], valid[i].receipt.signer_pubkey,
                           32) == 0)
                    duplicate = true;
        if (!duplicate) {
            memcpy(signers[count], valid[i].receipt.signer_pubkey, 32);
            count++;
        }
    }
    return count;
}

static bool bf_has_local_match(
    const struct bf_verified_receipt *valid, size_t valid_count,
    size_t remote_index)
{
    for (size_t i = 0; i < valid_count; i++)
        if (valid[i].local &&
            valid[i].work_kind == valid[remote_index].work_kind &&
            memcmp(valid[i].receipt.output_root,
                   valid[remote_index].receipt.output_root, 32) == 0)
            return true;
    return false;
}

// long-function-ok:proof-policy-transaction — selection, proof-set creation,
// and trust promotion must use one verified snapshot of the receipt ledger.
struct zcl_result build_fabric_proof_evaluate(
    struct node_db *ndb, const char *workspace, const char *action_id,
    int64_t now, struct build_fabric_proof_evaluation *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!ndb || !ndb->open || !workspace || !workspace[0] || !action_id ||
        !out || now < 0)
        return ZCL_ERR(-1, "proof evaluation requires complete inputs");
    struct db_build_action action;
    if (!db_build_action_find(ndb, action_id, &action) ||
        !action.task_root_sha3[0] || !action.candidate_root_sha3[0] ||
        !action.proof_policy_root_sha3[0])
        return ZCL_ERR(-1, "canonical ZCODE action not found");
    uint8_t *wire = NULL; size_t wire_len = 0; uint8_t root[32];
    struct vcs_zcode_task_v1 task;
    if (!bf_load_dev_object(workspace, action.task_root_sha3, &wire,
                            &wire_len, root) ||
        vcs_zcode_task_parse(wire, wire_len, &task) != VCS_ZCODE_DEV_OK) {
        free(wire); return ZCL_ERR(-1, "task CAS object is absent or corrupt");
    }
    free(wire); wire = NULL;
    struct vcs_zcode_candidate_v1 candidate;
    if (!bf_load_dev_object(workspace, action.candidate_root_sha3, &wire,
                            &wire_len, root) ||
        vcs_zcode_candidate_parse(wire, wire_len, &candidate) !=
            VCS_ZCODE_DEV_OK) {
        free(wire);
        return ZCL_ERR(-1, "candidate CAS object is absent or corrupt");
    }
    free(wire); wire = NULL;
    struct vcs_zcode_proof_policy_v1 policy;
    if (!bf_load_dev_object(workspace, action.proof_policy_root_sha3, &wire,
                            &wire_len, root) ||
        vcs_zcode_proof_policy_parse(wire, wire_len, &policy) !=
            VCS_ZCODE_DEV_OK) {
        free(wire);
        return ZCL_ERR(-1, "proof policy CAS object is absent or corrupt");
    }
    free(wire);
    struct db_build_receipt rows[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS];
    int row_count = db_build_candidate_receipts(
        ndb, action.task_root_sha3, action.candidate_root_sha3,
        action.proof_policy_root_sha3, rows,
        VCS_ZCODE_PROOF_SET_MAX_RECEIPTS);
    struct bf_verified_receipt valid[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS];
    size_t valid_count = 0;
    int64_t validation_now = now < task.expires_unix
        ? now : task.expires_unix - 1;
    for (int i = 0; i < row_count; i++) {
        if (!rows[i].work_receipt_sha3[0])
            continue;
        struct db_build_action receipt_action;
        if (!db_build_action_find(ndb, rows[i].action_id, &receipt_action) ||
            strcmp(receipt_action.task_root_sha3,
                   action.task_root_sha3) != 0 ||
            strcmp(receipt_action.candidate_root_sha3,
                   action.candidate_root_sha3) != 0 ||
            strcmp(receipt_action.proof_policy_root_sha3,
                   action.proof_policy_root_sha3) != 0)
            continue;
        uint8_t expected_kind =
            vcs_build_action_v1_work_kind(receipt_action.kind);
        if (expected_kind == 0) continue;
        uint8_t receipt_root[32]; uint8_t *receipt_wire = NULL;
        size_t receipt_len = 0;
        if (!bf_load_dev_object(workspace, rows[i].work_receipt_sha3,
                                &receipt_wire, &receipt_len, receipt_root))
            continue;
        struct vcs_zcode_work_receipt_v1 receipt;
        bool verified = vcs_zcode_work_receipt_parse(
                receipt_wire, receipt_len, &receipt) == VCS_ZCODE_DEV_OK &&
            vcs_zcode_work_receipt_verify(&receipt,
                                           receipt.signer_pubkey) ==
                VCS_ZCODE_DEV_OK &&
            vcs_zcode_work_receipt_validate_for_candidate(
                &task, &candidate, &receipt, validation_now) ==
                VCS_ZCODE_DEV_OK;
        free(receipt_wire);
        uint8_t action_root[32], input_root[32];
        verified = verified && zcl_hex_decode_lower(
            receipt_action.action_id, action_root, 32) &&
            zcl_hex_decode_lower(receipt_action.input_root_sha3,
                                 input_root, 32) &&
            memcmp(receipt.action_root, action_root, 32) == 0 &&
            memcmp(receipt.input_root, input_root, 32) == 0 &&
            receipt.work_kind == expected_kind &&
            receipt.status == VCS_ZCODE_WORK_PASS &&
            receipt.finished_unix <= now &&
            (policy.maximum_proof_age_seconds == 0 ||
             now - receipt.finished_unix <=
                 (int64_t)policy.maximum_proof_age_seconds);
        if (!verified || valid_count >= VCS_ZCODE_PROOF_SET_MAX_RECEIPTS)
            continue;
        struct db_build_worker worker;
        bool approved = db_build_worker_find(
                ndb, rows[i].worker_id, &worker) && worker.approved &&
            !worker.revoked &&
            (worker.expires_at == 0 || now < worker.expires_at);
        valid[valid_count].row = rows[i];
        valid[valid_count].receipt = receipt;
        memcpy(valid[valid_count].root, receipt_root, 32);
        valid[valid_count].work_kind = expected_kind;
        valid[valid_count].approved = approved;
        valid[valid_count].local =
            strcmp(rows[i].trust_state, "LOCAL_ACCEPTED") == 0;
        valid_count++;
    }
    for (size_t i = 0; i < valid_count; i++)
        if (valid[i].work_kind == VCS_ZCODE_WORK_REVIEW &&
            bf_receipt_trusted(&valid[i]))
            valid[i].review_approved = bf_review_approves_evidence(
                workspace, &task, &candidate, valid, valid_count, i,
                validation_now);
    out->valid_receipts = valid_count;
    size_t selected = SIZE_MAX, best = 0, best_ties = 0;
    for (size_t i = 0; i < valid_count; i++) {
        if (valid[i].local &&
            valid[i].work_kind == VCS_ZCODE_WORK_BUILD) {
            selected = i;
            break;
        }
    }
    for (size_t i = 0; selected == SIZE_MAX && i < valid_count; i++) {
        if (!valid[i].approved ||
            valid[i].work_kind != VCS_ZCODE_WORK_BUILD)
            continue;
        uint8_t signers[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32];
        size_t count = 0;
        for (size_t j = 0; j < valid_count; j++) {
            if (!valid[j].approved ||
                valid[j].work_kind != VCS_ZCODE_WORK_BUILD ||
                memcmp(valid[i].receipt.output_root,
                                             valid[j].receipt.output_root,
                                             32) != 0)
                continue;
            bool duplicate = false;
            for (size_t k = 0; k < count; k++)
                if (memcmp(signers[k], valid[j].receipt.signer_pubkey, 32) == 0)
                    duplicate = true;
            if (!duplicate)
                memcpy(signers[count++], valid[j].receipt.signer_pubkey, 32);
        }
        if (count > best) { best = count; selected = i; best_ties = 1; }
        else if (count == best && best > 0 && selected != SIZE_MAX &&
                 memcmp(valid[selected].receipt.output_root,
                        valid[i].receipt.output_root, 32) != 0)
            best_ties++;
    }
    if (best_ties > 1) selected = SIZE_MAX;
    bool have_selected_output = selected != SIZE_MAX;
    uint8_t selected_output[32] = {0};
    if (have_selected_output)
        memcpy(selected_output, valid[selected].receipt.output_root, 32);
    uint8_t proof_roots[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32];
    uint8_t distinct[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32];
    size_t distinct_count = 0, proof_count = 0;
    bool have_local = false, have_remote = false;
    for (size_t i = 0; i < valid_count; i++) {
        if (have_selected_output &&
            valid[i].work_kind == VCS_ZCODE_WORK_BUILD &&
            memcmp(valid[i].receipt.output_root, selected_output, 32) == 0) {
            if (valid[i].local) have_local = true;
            else have_remote = true;
        }
        if (valid[i].approved) {
            bool duplicate = false;
            for (size_t j = 0; j < distinct_count; j++)
                if (memcmp(distinct[j], valid[i].receipt.signer_pubkey, 32) == 0)
                    duplicate = true;
            if (!duplicate)
                memcpy(distinct[distinct_count++],
                       valid[i].receipt.signer_pubkey, 32);
        }
    }
    out->approved_distinct_signers = distinct_count;
    out->local_reproduced = have_local && have_remote;
    bool independent =
        (policy.flags & VCS_ZCODE_POLICY_INDEPENDENT_SIGNERS) != 0;
    out->compile_receipts = have_selected_output
        ? bf_count_kind(valid, valid_count, VCS_ZCODE_WORK_BUILD,
                        selected_output, independent)
        : 0;
    out->test_receipts = bf_count_kind(
        valid, valid_count, VCS_ZCODE_WORK_TEST, NULL, independent);
    out->fuzz_receipts = bf_count_kind(
        valid, valid_count, VCS_ZCODE_WORK_FUZZ, NULL, independent);
    out->review_receipts = bf_count_kind(
        valid, valid_count, VCS_ZCODE_WORK_REVIEW, NULL, independent);
    out->matching_receipts = out->compile_receipts;
    size_t compile_needed = policy.minimum_compile_receipts;
    if (policy.minimum_matching_receipts > compile_needed)
        compile_needed = policy.minimum_matching_receipts;
    size_t approved_compile_receipts = have_selected_output
        ? bf_count_approved_kind(
            valid, valid_count, VCS_ZCODE_WORK_BUILD, selected_output,
            independent)
        : 0;
    out->quorum_satisfied = approved_compile_receipts >= compile_needed;
    out->compile_satisfied = out->compile_receipts >= compile_needed &&
        (out->local_reproduced || out->quorum_satisfied);
    out->test_satisfied = out->test_receipts >= policy.minimum_test_receipts;
    out->fuzz_satisfied = out->fuzz_receipts >= policy.minimum_fuzz_receipts;
    out->review_satisfied =
        out->review_receipts >= policy.minimum_reviews;
    out->release_identity_satisfied =
        (policy.flags & VCS_ZCODE_POLICY_RELEASE_BYTE_IDENTITY) == 0;
    bool local_required = (policy.required_proofs &
                           VCS_ZCODE_PROOF_LOCAL_REPRODUCTION) != 0;
    out->policy_satisfied =
        (!(policy.required_proofs & VCS_ZCODE_PROOF_COMPILE) ||
         out->compile_satisfied) &&
        (!(policy.required_proofs & VCS_ZCODE_PROOF_TEST) ||
         out->test_satisfied) &&
        (!(policy.required_proofs & VCS_ZCODE_PROOF_FUZZ) ||
         out->fuzz_satisfied) &&
        (!(policy.required_proofs & VCS_ZCODE_PROOF_REVIEW) ||
         out->review_satisfied) &&
        (!local_required || out->local_reproduced) &&
        out->release_identity_satisfied;
    if (have_selected_output)
        zcl_hex_encode(selected_output, 32, out->output_root_sha3);
    for (size_t i = 0; i < valid_count; i++) {
        bool contributes = false;
        if (have_selected_output &&
            valid[i].work_kind == VCS_ZCODE_WORK_BUILD &&
            memcmp(valid[i].receipt.output_root,
                   selected_output, 32) == 0)
            contributes = bf_receipt_trusted(&valid[i]) ||
                          bf_has_local_match(valid, valid_count, i);
        else if (valid[i].work_kind == VCS_ZCODE_WORK_TEST ||
                 valid[i].work_kind == VCS_ZCODE_WORK_FUZZ)
            contributes = bf_receipt_trusted(&valid[i]);
        else if (valid[i].work_kind == VCS_ZCODE_WORK_REVIEW)
            contributes = bf_receipt_trusted(&valid[i]) &&
                          valid[i].review_approved;
        if (contributes) memcpy(proof_roots[proof_count++], valid[i].root, 32);
    }
    if (proof_count == 0) return ZCL_OK;
    qsort(proof_roots, proof_count, 32, bf_root_compare);
    uint8_t proof_wire[VCS_ZCODE_PROOF_SET_WIRE_MAX], proof_root[32];
    size_t proof_len = 0;
    if (vcs_zcode_proof_set_serialize(
            (const uint8_t (*)[32])proof_roots, proof_count, proof_wire,
            sizeof(proof_wire), &proof_len) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_set_root(
            (const uint8_t (*)[32])proof_roots, proof_count, proof_root) !=
            VCS_ZCODE_DEV_OK ||
        !vcs_object_put_addressed(workspace, proof_root, proof_wire,
                                  proof_len))
        return ZCL_ERR(-1, "canonical proof set could not enter CAS");
    zcl_hex_encode(proof_root, 32, out->proof_set_root_sha3);
    if (!node_db_begin(ndb))
        return ZCL_ERR(-1, "cannot begin receipt trust promotion");
    bool saved = true;
    for (size_t i = 0; i < valid_count && saved; i++) {
        if (strcmp(valid[i].row.trust_state, "REMOTE_OBSERVED") != 0)
            continue;
        bool local_match = bf_has_local_match(valid, valid_count, i);
        bool class_quorum = false;
        if (valid[i].work_kind == VCS_ZCODE_WORK_BUILD)
            class_quorum = have_selected_output && out->quorum_satisfied &&
                memcmp(valid[i].receipt.output_root,
                       selected_output, 32) == 0;
        else if (valid[i].work_kind == VCS_ZCODE_WORK_TEST)
            class_quorum = out->test_satisfied;
        else if (valid[i].work_kind == VCS_ZCODE_WORK_FUZZ)
            class_quorum = out->fuzz_satisfied;
        else if (valid[i].work_kind == VCS_ZCODE_WORK_REVIEW)
            class_quorum = out->review_satisfied &&
                           valid[i].review_approved;
        if (local_match)
            (void)snprintf(valid[i].row.trust_state,
                           sizeof(valid[i].row.trust_state),
                           "LOCAL_REPRODUCED");
        else if (class_quorum && valid[i].approved)
            (void)snprintf(valid[i].row.trust_state,
                           sizeof(valid[i].row.trust_state),
                           "QUORUM_MATCHED");
        else
            continue;
        saved = db_build_receipt_save(ndb, &valid[i].row);
    }
    saved = saved && node_db_commit(ndb);
    if (!saved) {
        if (!node_db_rollback(ndb))
            LOG_ERROR("build_fabric", "trust promotion and rollback failed");
        return ZCL_ERR(-1, "receipt trust promotion could not persist");
    }
    return ZCL_OK;
}
