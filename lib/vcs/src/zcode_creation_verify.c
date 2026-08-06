/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: independently re-derive every creation-attribution authority. */
#include "vcs/zcode_creation_attribution.h"

#include "vcs/package_manifest.h"
#include "vcs/package_release.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_contributor_binding.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_lane.h"
#include "vcs/zcode_score_receipt.h"

#include <stdlib.h>
#include <string.h>

struct creation_vertical {
    struct vcs_zcode_task_v1 task;
    struct vcs_zcode_candidate_v1 candidate;
    struct vcs_zcode_proof_policy_v1 policy;
    struct vcs_zcode_lane_receipt_v1 lane;
    struct vcs_zcode_score_receipt_v1 score;
    struct vcs_package_release release;
    uint8_t proof_roots[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS][32];
    struct vcs_zcode_work_receipt_v1
        works[VCS_ZCODE_PROOF_SET_MAX_RECEIPTS];
    size_t proof_count;
};

static bool creation_equal(const uint8_t a[32], const uint8_t b[32])
{
    return memcmp(a, b, 32) == 0;
}

static bool creation_nonzero(const uint8_t root[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++)
        any |= root[i];
    return any != 0;
}

static bool creation_load(const char *workspace, const uint8_t root[32],
                          size_t maximum, uint8_t **wire, size_t *wire_len)
{
    *wire = NULL;
    *wire_len = 0;
    return vcs_object_load_raw_bounded(workspace, root, maximum,
                                       wire, wire_len) == 0;
}

static enum vcs_zcode_creation_error creation_load_task_triplet(
    const struct vcs_zcode_creation_attribution_v1 *a, const char *workspace,
    struct creation_vertical *vertical)
{
    uint8_t *wire = NULL, root[32];
    size_t wire_len = 0;
    if (!creation_load(workspace, a->task_root, VCS_ZCODE_TASK_WIRE_BYTES,
                       &wire, &wire_len) ||
        vcs_zcode_task_parse(wire, wire_len, &vertical->task) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_task_root(&vertical->task, root) != VCS_ZCODE_DEV_OK ||
        !creation_equal(root, a->task_root)) {
        free(wire);
        return VCS_ZCODE_CREATION_TASK;
    }
    free(wire); wire = NULL;
    if (!creation_load(workspace, a->candidate_root,
                       VCS_ZCODE_CANDIDATE_WIRE_BYTES, &wire, &wire_len) ||
        vcs_zcode_candidate_parse(wire, wire_len, &vertical->candidate) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_root(&vertical->candidate, root) !=
            VCS_ZCODE_DEV_OK ||
        !creation_equal(root, a->candidate_root) ||
        !creation_equal(vertical->candidate.task_root, a->task_root) ||
        !creation_equal(vertical->candidate.base_source_root,
                        vertical->task.source_root) ||
        !creation_equal(vertical->candidate.candidate_source_root,
                        a->package_root) ||
        vcs_zcode_task_validate_at(&vertical->task,
                                   vertical->candidate.created_unix) !=
            VCS_ZCODE_DEV_OK) {
        free(wire);
        return VCS_ZCODE_CREATION_CANDIDATE;
    }
    free(wire); wire = NULL;
    if (!creation_load(workspace, a->proof_policy_root,
                       VCS_ZCODE_PROOF_POLICY_WIRE_BYTES, &wire, &wire_len) ||
        vcs_zcode_proof_policy_parse(wire, wire_len, &vertical->policy) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_policy_root(&vertical->policy, root) !=
            VCS_ZCODE_DEV_OK ||
        !creation_equal(root, a->proof_policy_root) ||
        !creation_equal(vertical->task.proof_policy_root,
                        a->proof_policy_root)) {
        free(wire);
        return VCS_ZCODE_CREATION_PROOF_POLICY;
    }
    free(wire);
    return VCS_ZCODE_CREATION_OK;
}

static enum vcs_zcode_creation_error creation_load_contributor(
    const struct vcs_zcode_creation_attribution_v1 *a,
    const struct vcs_zcode_creation_validation_context *context,
    const struct vcs_zcode_candidate_v1 *candidate)
{
    uint8_t *wire = NULL, root[32], operation = 0, zid_pubkey[32];
    size_t wire_len = 0;
    if (!creation_load(context->workspace, a->contributor_binding_root,
                       VCS_ZCODE_CONTRIBUTOR_BINDING_V2_WIRE_BYTES,
                       &wire, &wire_len))
        return VCS_ZCODE_CREATION_CONTRIBUTOR;
    enum vcs_zcode_binding_error binding_error = VCS_ZCODE_BINDING_ERR_VERSION;
    if (wire_len == VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES) {
        struct vcs_zcode_contributor_binding_v1 binding;
        binding_error = vcs_zcode_contributor_binding_parse(
            wire, wire_len, &binding);
        if (binding_error == VCS_ZCODE_BINDING_OK)
            binding_error = vcs_zcode_contributor_binding_root(&binding, root);
        if (binding_error == VCS_ZCODE_BINDING_OK)
            binding_error = vcs_zcode_contributor_binding_verify(
                &binding, a->network_genesis_root, binding.zid_pubkey,
                context->now_unix);
        operation = binding.operation;
        memcpy(zid_pubkey, binding.zid_pubkey, sizeof(zid_pubkey));
    } else if (wire_len == VCS_ZCODE_CONTRIBUTOR_BINDING_V2_WIRE_BYTES) {
        struct vcs_zcode_contributor_binding_v2 binding;
        binding_error = vcs_zcode_contributor_binding_parse_v2(
            wire, wire_len, &binding);
        if (binding_error == VCS_ZCODE_BINDING_OK)
            binding_error = vcs_zcode_contributor_binding_root_v2(&binding,
                                                                   root);
        if (binding_error == VCS_ZCODE_BINDING_OK)
            binding_error = vcs_zcode_contributor_binding_verify_v2(
                &binding, a->network_genesis_root, binding.zid_pubkey,
                context->now_unix);
        operation = binding.operation;
        memcpy(zid_pubkey, binding.zid_pubkey, sizeof(zid_pubkey));
    }
    free(wire);
    if (binding_error != VCS_ZCODE_BINDING_OK ||
        !creation_equal(root, a->contributor_binding_root) ||
        !creation_equal(zid_pubkey, candidate->author_pubkey) ||
        operation == VCS_ZCODE_BINDING_REVOKE)
        return VCS_ZCODE_CREATION_CONTRIBUTOR;
    if (operation != VCS_ZCODE_BINDING_ACTIVE &&
        (!context->binding_is_current ||
         !context->binding_is_current(context->callback_opaque,
                                      a->contributor_binding_root)))
        return VCS_ZCODE_CREATION_CONTRIBUTOR;
    return VCS_ZCODE_CREATION_OK;
}

static enum vcs_zcode_creation_error creation_verify_license_chunks(
    const char *workspace, const struct vcs_package_file *license)
{
    if (!license || license->size == 0 || license->chunk_count == 0)
        return VCS_ZCODE_CREATION_LICENSE;
    uint64_t observed = 0;
    for (uint32_t i = 0; i < license->chunk_count; i++) {
        const uint8_t *chunk_root = license->chunk_hashes + (size_t)i * 32u;
        uint8_t *chunk = NULL;
        size_t chunk_len = 0;
        if (!creation_load(workspace, chunk_root, VCS_PACKAGE_CHUNK_BYTES,
                           &chunk, &chunk_len) ||
            !vcs_package_verify_chunk(license, i, chunk, chunk_len)) {
            free(chunk);
            return VCS_ZCODE_CREATION_LICENSE;
        }
        if (UINT64_MAX - observed < chunk_len) {
            free(chunk);
            return VCS_ZCODE_CREATION_OVERFLOW;
        }
        observed += chunk_len;
        free(chunk);
    }
    return observed == license->size ? VCS_ZCODE_CREATION_OK
                                     : VCS_ZCODE_CREATION_LICENSE;
}

static enum vcs_zcode_creation_error creation_load_package(
    const struct vcs_zcode_creation_attribution_v1 *a, const char *workspace,
    struct creation_vertical *vertical)
{
    uint8_t *wire = NULL, root[32];
    size_t wire_len = 0;
    struct vcs_package_manifest manifest;
    vcs_package_manifest_init(&manifest);
    if (!creation_load(workspace, a->package_root,
                       VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES,
                       &wire, &wire_len) ||
        !vcs_package_manifest_parse(wire, wire_len, &manifest) ||
        !vcs_package_manifest_root(&manifest, root) ||
        !creation_equal(root, a->package_root)) {
        free(wire); vcs_package_manifest_free(&manifest);
        return VCS_ZCODE_CREATION_PACKAGE;
    }
    free(wire); wire = NULL;
    const struct vcs_package_file *license = NULL;
    for (size_t i = 0; i < manifest.count; i++)
        if (strcmp(manifest.files[i].path, "LICENSE") == 0)
            license = &manifest.files[i];
    if (!license || !vcs_package_file_hash(license, root) ||
        !creation_equal(root, a->license_evidence_root)) {
        vcs_package_manifest_free(&manifest);
        return VCS_ZCODE_CREATION_LICENSE;
    }
    enum vcs_zcode_creation_error error =
        creation_verify_license_chunks(workspace, license);
    vcs_package_manifest_free(&manifest);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;

    if (!creation_load(workspace, a->release_root,
                       VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES,
                       &wire, &wire_len) ||
        vcs_package_release_parse(wire, wire_len, &vertical->release) !=
            VCS_PACKAGE_RELEASE_OK ||
        vcs_package_release_verify(&vertical->release) !=
            VCS_PACKAGE_RELEASE_OK ||
        vcs_package_release_id(&vertical->release, root) !=
            VCS_PACKAGE_RELEASE_OK ||
        !creation_equal(root, a->release_root) ||
        !creation_equal(vertical->release.package_root, a->package_root) ||
        strcmp(vertical->release.chain_id, "zclassic-main") != 0) {
        free(wire);
        return VCS_ZCODE_CREATION_RELEASE;
    }
    free(wire);
    return VCS_ZCODE_CREATION_OK;
}

static enum vcs_zcode_creation_error creation_load_proof_set(
    const struct vcs_zcode_creation_attribution_v1 *a, const char *workspace,
    struct creation_vertical *vertical)
{
    uint8_t *wire = NULL, root[32];
    size_t wire_len = 0;
    if (!creation_load(workspace, a->proof_set_root,
                       VCS_ZCODE_PROOF_SET_WIRE_MAX, &wire, &wire_len) ||
        vcs_zcode_proof_set_parse(
            wire, wire_len, vertical->proof_roots,
            VCS_ZCODE_PROOF_SET_MAX_RECEIPTS, &vertical->proof_count) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_set_root(vertical->proof_roots,
                                 vertical->proof_count, root) !=
            VCS_ZCODE_DEV_OK ||
        !creation_equal(root, a->proof_set_root)) {
        free(wire);
        return VCS_ZCODE_CREATION_PROOF_SET;
    }
    free(wire);
    for (size_t i = 0; i < vertical->proof_count; i++) {
        wire = NULL; wire_len = 0;
        if (!creation_load(workspace, vertical->proof_roots[i],
                           VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES,
                           &wire, &wire_len) ||
            vcs_zcode_work_receipt_parse(
                wire, wire_len, &vertical->works[i]) != VCS_ZCODE_DEV_OK ||
            vcs_zcode_work_receipt_id(&vertical->works[i], root) !=
                VCS_ZCODE_DEV_OK ||
            !creation_equal(root, vertical->proof_roots[i]) ||
            vcs_zcode_work_receipt_verify(
                &vertical->works[i], vertical->works[i].signer_pubkey) !=
                VCS_ZCODE_DEV_OK) {
            free(wire);
            return VCS_ZCODE_CREATION_PROOF_SET;
        }
        free(wire);
    }
    return VCS_ZCODE_CREATION_OK;
}

static enum vcs_zcode_creation_error creation_load_lane_score(
    const struct vcs_zcode_creation_attribution_v1 *a, const char *workspace,
    struct creation_vertical *vertical)
{
    uint8_t *wire = NULL, root[32];
    size_t wire_len = 0;
    if (!creation_load(workspace, a->score_receipt_root,
                       VCS_ZCODE_SCORE_WIRE_BYTES, &wire, &wire_len) ||
        vcs_zcode_score_receipt_parse(wire, wire_len, &vertical->score) !=
            VCS_ZCODE_SCORE_OK ||
        vcs_zcode_score_receipt_id(&vertical->score, root) !=
            VCS_ZCODE_SCORE_OK ||
        !creation_equal(root, a->score_receipt_root) ||
        vcs_zcode_score_receipt_verify(&vertical->score) !=
            VCS_ZCODE_SCORE_OK) {
        free(wire);
        return VCS_ZCODE_CREATION_SCORE;
    }
    free(wire); wire = NULL;
    if (!creation_load(workspace, a->proven_lane_root,
                       VCS_ZCODE_LANE_WIRE_BYTES, &wire, &wire_len) ||
        vcs_zcode_lane_receipt_parse(wire, wire_len, &vertical->lane) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_id(&vertical->lane, root) !=
            VCS_ZCODE_DEV_OK ||
        !creation_equal(root, a->proven_lane_root) ||
        vertical->lane.lane != VCS_ZCODE_LANE_PROVEN ||
        vcs_zcode_lane_receipt_verify(&vertical->lane,
                                      vertical->score.lane_signer) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_validate_for_candidate(
            &vertical->lane, &vertical->task, &vertical->candidate,
            &vertical->policy) != VCS_ZCODE_DEV_OK) {
        free(wire);
        return VCS_ZCODE_CREATION_LANE;
    }
    free(wire);
    return VCS_ZCODE_CREATION_OK;
}

static uint8_t creation_required_score_mask(uint16_t category)
{
    switch (category) {
    case VCS_ZCODE_CREATION_PUBLIC_SOURCE:
        return UINT8_C(1) << VCS_ZCODE_SCORE_ACCEPTED_EXTRACTION;
    case VCS_ZCODE_CREATION_BORN_RED_FIX:
    case VCS_ZCODE_CREATION_SECURITY_FIX:
        return UINT8_C(1) << VCS_ZCODE_SCORE_BORN_RED_DEFECT_TEST;
    case VCS_ZCODE_CREATION_INDEPENDENT_REPRODUCTION:
        return UINT8_C(1) << VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION;
    case VCS_ZCODE_CREATION_COMPATIBILITY:
        return UINT8_C(1) << VCS_ZCODE_SCORE_COMPATIBILITY_MAINTENANCE;
    case VCS_ZCODE_CREATION_PRESERVATION:
        return (UINT8_C(1) << VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION) |
               (UINT8_C(1) << VCS_ZCODE_SCORE_COMPATIBILITY_MAINTENANCE);
    }
    return 0;
}

static enum vcs_zcode_creation_error creation_rederive_score(
    const struct vcs_zcode_creation_attribution_v1 *a,
    struct creation_vertical *vertical)
{
    struct vcs_zcode_score_plan_input input = {
        .task = &vertical->task,
        .candidate = &vertical->candidate,
        .proof_policy = &vertical->policy,
        .proven_lane = &vertical->lane,
        .proof_receipt_roots = vertical->proof_roots,
        .work_receipts = vertical->works,
        .work_receipt_count = vertical->proof_count,
        .package_root = a->package_root,
        .release_root = a->release_root,
        .recipe_root = vertical->release.recipe_root,
        .dependency_lock_root = vertical->task.dependency_lock_root,
        .api_capsule_root = vertical->task.toolchain_capsule_root,
    };
    struct vcs_zcode_score_receipt_v1 expected;
    uint8_t actual_body[VCS_ZCODE_SCORE_BODY_BYTES];
    uint8_t expected_body[VCS_ZCODE_SCORE_BODY_BYTES];
    uint8_t needed = creation_required_score_mask(a->category);
    if (needed == 0 ||
        (vertical->score.awarded_mask & needed) != needed ||
        !creation_equal(vertical->score.task_root, a->task_root) ||
        !creation_equal(vertical->score.candidate_root, a->candidate_root) ||
        !creation_equal(vertical->score.proof_policy_root,
                        a->proof_policy_root) ||
        !creation_equal(vertical->score.proof_set_root, a->proof_set_root) ||
        !creation_equal(vertical->score.proven_lane_root,
                        a->proven_lane_root) ||
        !creation_equal(vertical->score.package_root, a->package_root) ||
        !creation_equal(vertical->score.release_root, a->release_root) ||
        vcs_zcode_score_plan(&input, &expected) != VCS_ZCODE_SCORE_OK ||
        vcs_zcode_score_receipt_body(&vertical->score, actual_body) !=
            VCS_ZCODE_SCORE_OK ||
        vcs_zcode_score_receipt_body(&expected, expected_body) !=
            VCS_ZCODE_SCORE_OK ||
        memcmp(actual_body, expected_body, sizeof(actual_body)) != 0)
        return VCS_ZCODE_CREATION_SCORE;
    return VCS_ZCODE_CREATION_OK;
}

enum vcs_zcode_creation_error vcs_zcode_creation_attribution_verify_cas(
    const struct vcs_zcode_creation_attribution_v1 *a,
    const struct vcs_zcode_creation_validation_context *context)
{
    enum vcs_zcode_creation_error error =
        vcs_zcode_creation_attribution_validate(a);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;
    if (!context || !context->workspace ||
        !context->expected_network_genesis_root ||
        !context->expected_zc23_policy_root ||
        !context->anchor_is_active ||
        !context->contribution_is_duplicate || context->now_unix <= 0 ||
        !creation_nonzero(context->expected_network_genesis_root) ||
        !creation_nonzero(context->expected_zc23_policy_root))
        return VCS_ZCODE_CREATION_CONTEXT;
    if (!creation_equal(a->network_genesis_root,
                        context->expected_network_genesis_root))
        return VCS_ZCODE_CREATION_NETWORK;
    if (!creation_equal(a->zc23_policy_root,
                        context->expected_zc23_policy_root))
        return VCS_ZCODE_CREATION_POLICY;
    if (a->epoch != context->expected_epoch)
        return VCS_ZCODE_CREATION_EPOCH;
    if (a->award_atoms != context->expected_award_atoms ||
        context->expected_award_atoms == 0)
        return VCS_ZCODE_CREATION_AMOUNT;
    if (context->active_height < a->challenge_maturity_height ||
        context->active_mtp < a->challenge_maturity_mtp ||
        context->now_unix < a->created_unix)
        return VCS_ZCODE_CREATION_IMMATURE;
    if (!context->anchor_is_active(context->callback_opaque,
                                   a->challenge_opening_height,
                                   a->challenge_opening_hash))
        return VCS_ZCODE_CREATION_REORG;

    struct creation_vertical vertical;
    memset(&vertical, 0, sizeof(vertical));
    error = creation_load_task_triplet(a, context->workspace, &vertical);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;
    error = creation_load_contributor(a, context, &vertical.candidate);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;
    error = creation_load_package(a, context->workspace, &vertical);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;
    error = creation_load_proof_set(a, context->workspace, &vertical);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;
    error = creation_load_lane_score(a, context->workspace, &vertical);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;
    error = creation_rederive_score(a, &vertical);
    if (error != VCS_ZCODE_CREATION_OK)
        return error;
    uint8_t attribution_root[32];
    if (vcs_zcode_creation_attribution_root(a, attribution_root) !=
            VCS_ZCODE_CREATION_OK)
        return VCS_ZCODE_CREATION_ROOT;
    if (context->contribution_is_duplicate(
            context->callback_opaque, a->candidate_root, attribution_root))
        return VCS_ZCODE_CREATION_DUPLICATE;
    return VCS_ZCODE_CREATION_OK;
}
