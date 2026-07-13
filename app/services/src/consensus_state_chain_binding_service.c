/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Bind contained state manifests to stable selected-chain evidence. */

// one-result-type-ok:opaque-read-only-evidence-accessors — fallible capture
// and decision surfaces return zcl_result; bool exports are total predicates.

#include "services/consensus_state_chain_binding_service.h"

#include "chain/chain.h"
#include "config/consensus_state_snapshot_install.h"
#include "core/arith_uint256.h"
#include "crypto/sha3.h"
#include "framework/condition.h"
#include "jobs/validate_headers_stage.h"
#include "services/chain_frontier_snapshot_service.h"
#include "services/chain_state_service.h"
#include "storage/progress_store.h"
#include "util/safe_alloc.h"
#include "validation/main_state.h"

#include <stdlib.h>
#include <string.h>

struct consensus_state_chain_evidence {
    uint8_t artifact_receipt_digest[32];
    uint8_t evidence_digest[32];
    enum consensus_state_target_lane target_lane;
};

static const char *target_lane_name(enum consensus_state_target_lane lane)
{
    switch (lane) {
    case CONSENSUS_STATE_TARGET_LANE_COPY_PROOF: return "copy-proof";
    case CONSENSUS_STATE_TARGET_LANE_DEV: return "dev";
    case CONSENSUS_STATE_TARGET_LANE_SOAK: return "soak";
    case CONSENSUS_STATE_TARGET_LANE_CANONICAL: return "canonical";
    case CONSENSUS_STATE_TARGET_LANE_UNKNOWN: return NULL;
    }
    return NULL;
}

static bool bytes_nonzero(const uint8_t bytes[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++)
        any |= bytes[i];
    return any != 0;
}

static void digest_u32(struct sha3_256_ctx *ctx, uint32_t value)
{
    uint8_t encoded[4];
    for (size_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t)(value >> (8u * i));
    sha3_256_write(ctx, encoded, sizeof(encoded));
}

static void digest_i32(struct sha3_256_ctx *ctx, int32_t value)
{
    digest_u32(ctx, (uint32_t)value);
}

static bool manifest_is_complete_and_self_bound(
    const struct consensus_state_bundle_manifest *manifest)
{
    if (!manifest || manifest->height < 0 ||
        manifest->sapling_frontier_height < 0 ||
        manifest->sapling_frontier_height > manifest->height ||
        !manifest->history_complete || manifest->activation_boundary != 0 ||
        manifest->sprout_source_cursor != 0 ||
        manifest->sapling_source_cursor != 0 ||
        manifest->nullifier_source_cursor != 0 ||
        manifest->source_fold_cursor != (int64_t)manifest->height + 1 ||
        !bytes_nonzero(manifest->block_hash) ||
        !bytes_nonzero(manifest->sapling_frontier_root) ||
        !bytes_nonzero(manifest->proof_manifest_digest) ||
        !bytes_nonzero(manifest->source_digest) ||
        !bytes_nonzero(manifest->artifact_digest))
        return false;
    uint8_t computed[32];
    consensus_state_bundle_artifact_digest(manifest, computed);
    return memcmp(computed, manifest->artifact_digest, 32) == 0;
}

struct zcl_result consensus_state_chain_binding_decide(
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_chain_binding_observation *observation)
{
    if (!manifest || !observation)
        return ZCL_ERR(-1, "chain binding: null manifest/observation");
    if (!manifest_is_complete_and_self_bound(manifest))
        return ZCL_ERR(-2, "chain binding: manifest is not complete/self-bound");
    if (!observation->before_frontier_consistent ||
        !observation->after_frontier_consistent ||
        !observation->frontier_unchanged)
        return ZCL_ERR(-3, "chain binding: selected frontier changed or is not durable");
    if (observation->durable_served_height < manifest->height)
        return ZCL_ERR(-4, "chain binding: bundle height=%d exceeds durable H*=%d",
                       manifest->height, observation->durable_served_height);
    if (!observation->selected_bundle_known ||
        observation->selected_bundle_height != manifest->height ||
        memcmp(observation->selected_bundle_hash,
               manifest->block_hash, 32) != 0)
        return ZCL_ERR(-5, "chain binding: bundle block is not the selected-chain block");
    if (!observation->selected_bundle_valid_scripts ||
        !observation->selected_bundle_failure_free ||
        !observation->bundle_header_pass_record)
        return ZCL_ERR(-6, "chain binding: bundle block lacks durable validation");
    if (!observation->selected_sapling_source_known ||
        observation->selected_sapling_source_height !=
            manifest->sapling_frontier_height ||
        !bytes_nonzero(observation->selected_sapling_source_hash))
        return ZCL_ERR(-7, "chain binding: Sapling source height unavailable");
    if (!observation->selected_sapling_source_valid_scripts ||
        !observation->selected_sapling_source_failure_free ||
        !observation->sapling_source_header_pass_record)
        return ZCL_ERR(-8, "chain binding: Sapling source lacks durable validation");
    if (memcmp(observation->selected_sapling_source_root,
               manifest->sapling_frontier_root, 32) != 0)
        return ZCL_ERR(-9, "chain binding: Sapling frontier disagrees with source header");
    /* A sparse frontier can originate below H only when no later block has
     * changed it. Requiring the same root in H's header closes that gap. */
    if (memcmp(observation->selected_bundle_sapling_root,
               manifest->sapling_frontier_root, 32) != 0)
        return ZCL_ERR(-10, "chain binding: Sapling frontier is not current at bundle height");
    if (!observation->selected_header_known ||
        observation->selected_header_height < manifest->height ||
        !bytes_nonzero(observation->selected_header_hash) ||
        !bytes_nonzero(observation->selected_header_chainwork) ||
        !observation->selected_header_valid_tree ||
        !observation->selected_header_failure_free ||
        !observation->header_descends_from_bundle ||
        !observation->bundle_descends_from_sapling_source)
        return ZCL_ERR(-11, "chain binding: selected-header ancestry/work/validity missing");
    return ZCL_OK;
}

static bool capture_equal(const struct chain_state_frontier_view *a,
                          const struct chain_state_frontier_view *b)
{
    return a && b && a->initialized == b->initialized &&
        a->bound_to_expected_state == b->bound_to_expected_state &&
        a->window.height == b->window.height &&
        a->window.requested_height == b->window.requested_height &&
        a->window.tip == b->window.tip &&
        a->window.requested == b->window.requested &&
        a->header_tip == b->header_tip;
}

static bool index_matches_frontier_value(
    const struct block_index *index, const struct chain_frontier_value *value)
{
    if (!index || !index->phashBlock || !value || !value->height_known ||
        !value->binding_known || index->nHeight != value->height)
        return false;
    char hash[65];
    char work[65];
    uint256_get_hex(index->phashBlock, hash);
    arith_uint256_get_hex(&index->nChainWork, work);
    return strcmp(hash, value->hash) == 0 &&
        strcmp(work, value->chain_work) == 0;
}

static bool capture_matches_frontier(
    const struct chain_state_frontier_view *view,
    const struct chain_frontier_snapshot *frontier)
{
    return view && frontier &&
        index_matches_frontier_value(view->window.tip, &frontier->indexed) &&
        index_matches_frontier_value(view->header_tip, &frontier->header);
}

static bool index_descends_from(struct block_index *higher,
                                struct block_index *lower)
{
    if (!higher || !lower || !higher->phashBlock || !lower->phashBlock ||
        higher->nHeight < lower->nHeight)
        return false;
    struct block_index *ancestor = block_index_get_ancestor(
        higher, lower->nHeight);
    return ancestor && ancestor->phashBlock &&
        uint256_eq(ancestor->phashBlock, lower->phashBlock);
}

static void encode_chainwork(const struct arith_uint256 *work, uint8_t out[32])
{
    for (size_t limb = 0; limb < ARITH_UINT256_WIDTH; limb++) {
        uint32_t value = work->pn[limb];
        for (size_t byte = 0; byte < 4; byte++)
            out[limb * 4u + byte] = (uint8_t)(value >> (8u * byte));
    }
}

static void capture_binding_predicates(
    const struct chain_state_frontier_view *view,
    const struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_chain_binding_observation *observation)
{
    memset(observation, 0, sizeof(*observation));
    struct block_index *bundle = view ? view->window.requested : NULL;
    struct block_index *header = view ? view->header_tip : NULL;
    struct block_index *sapling_source = bundle
        ? block_index_get_ancestor(
              bundle, (int)manifest->sapling_frontier_height)
        : NULL;
    if (bundle && bundle->phashBlock) {
        observation->selected_bundle_known = true;
        observation->selected_bundle_height = bundle->nHeight;
        memcpy(observation->selected_bundle_hash, bundle->phashBlock->data, 32);
        memcpy(observation->selected_bundle_sapling_root,
               bundle->hashFinalSaplingRoot.data, 32);
        observation->selected_bundle_valid_scripts =
            block_index_is_valid(bundle, BLOCK_VALID_SCRIPTS);
        observation->selected_bundle_failure_free =
            !block_has_any_failure(bundle);
        observation->bundle_header_pass_record =
            validate_headers_stage_has_pass_record(bundle->nHeight,
                                                    bundle->phashBlock);
    }
    if (sapling_source && sapling_source->phashBlock) {
        observation->selected_sapling_source_known = true;
        observation->selected_sapling_source_height = sapling_source->nHeight;
        memcpy(observation->selected_sapling_source_hash,
               sapling_source->phashBlock->data, 32);
        memcpy(observation->selected_sapling_source_root,
               sapling_source->hashFinalSaplingRoot.data, 32);
        observation->selected_sapling_source_valid_scripts =
            block_index_is_valid(sapling_source, BLOCK_VALID_SCRIPTS);
        observation->selected_sapling_source_failure_free =
            !block_has_any_failure(sapling_source);
        observation->sapling_source_header_pass_record =
            validate_headers_stage_has_pass_record(
                sapling_source->nHeight, sapling_source->phashBlock);
    }
    if (header && header->phashBlock) {
        observation->selected_header_known = true;
        observation->selected_header_height = header->nHeight;
        memcpy(observation->selected_header_hash, header->phashBlock->data, 32);
        encode_chainwork(&header->nChainWork,
                         observation->selected_header_chainwork);
        observation->selected_header_valid_tree =
            block_index_is_valid(header, BLOCK_VALID_TREE);
        observation->selected_header_failure_free =
            !block_has_any_failure(header);
    }
    observation->header_descends_from_bundle =
        index_descends_from(header, bundle);
    observation->bundle_descends_from_sapling_source =
        index_descends_from(bundle, sapling_source);
}

static void evidence_digest_build(
    const struct consensus_state_bundle_manifest *manifest,
    const uint8_t artifact_receipt_digest[32],
    const struct consensus_state_chain_binding_observation *observation,
    const struct chain_frontier_snapshot *frontier,
    const char *target_lane, uint8_t out[32])
{
    static const char domain[] =
        CONSENSUS_STATE_BUNDLE_SCHEMA "/selected-chain-evidence";
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&ctx, manifest->artifact_digest, 32);
    sha3_256_write(&ctx, artifact_receipt_digest, 32);
    digest_i32(&ctx, manifest->height);
    sha3_256_write(&ctx, observation->selected_bundle_hash, 32);
    digest_i32(&ctx, observation->selected_sapling_source_height);
    sha3_256_write(&ctx, observation->selected_sapling_source_hash, 32);
    sha3_256_write(&ctx, manifest->sapling_frontier_root, 32);
    digest_i32(&ctx, observation->durable_served_height);
    sha3_256_write(&ctx, (const uint8_t *)frontier->served.hash, 64);
    sha3_256_write(&ctx, (const uint8_t *)frontier->served.chain_work, 64);
    digest_i32(&ctx, observation->selected_header_height);
    sha3_256_write(&ctx, observation->selected_header_hash, 32);
    sha3_256_write(&ctx, observation->selected_header_chainwork, 32);
    size_t lane_len = strlen(target_lane);
    digest_u32(&ctx, (uint32_t)lane_len);
    sha3_256_write(&ctx, (const uint8_t *)target_lane, lane_len);
    sha3_256_finalize(&ctx, out);
}

struct zcl_result consensus_state_chain_evidence_build(
    const struct consensus_state_chain_binding_request *request,
    struct consensus_state_chain_evidence **out)
{
    if (out)
        *out = NULL;
    if (!request || !out || !request->main || !request->artifact)
        return ZCL_ERR(-20, "chain evidence: null request member");
    const char *target_lane = target_lane_name(request->target_lane);
    if (!target_lane)
        return ZCL_ERR(-21, "chain evidence: target lane is not canonical");
    /* This implementation reads the process-global reducer/progress authority.
     * Refuse an API-shaped copy context rather than mixing its chain pointers
     * with another store's H-star/proof rows. A future copy binder needs one opaque
     * target context owning both CSR and progress-store identity. */
    if (condition_engine_main_state() != request->main ||
        !progress_store_db())
        return ZCL_ERR(-22,
                       "chain evidence: request is not the open process singleton");
    /* Only the strict validator can create artifact evidence. The builder
     * reasons over its copied manifest while the exact descriptor/read txn
     * remains pinned by the caller-owned opaque receipt. */
    struct consensus_state_bundle_manifest manifest_copy;
    uint8_t artifact_receipt_digest[32];
    if (!consensus_state_artifact_evidence_manifest_copy(
            request->artifact, &manifest_copy) ||
        !consensus_state_artifact_evidence_receipt_digest(
            request->artifact, artifact_receipt_digest))
        return ZCL_ERR(-23, "chain evidence: artifact receipt is stale");
    const struct consensus_state_bundle_manifest *manifest = &manifest_copy;

    /* Lock order is progress -> CSR/window. Header-pass reads recursively take
     * this same progress lock, keeping every durable predicate in one store
     * generation throughout the two chain samples. */
    progress_store_tx_lock();
    struct chain_frontier_snapshot frontier_before;
    struct chain_frontier_snapshot frontier_after;
    chain_frontier_snapshot_collect(&frontier_before, request->main);

    struct chain_state_frontier_view view_before;
    struct zcl_result captured = csr_capture_frontiers(
        csr_instance(), &request->main->chain_active,
        &request->main->pindex_best_header, manifest->height,
        &view_before);
    if (!captured.ok) {
        progress_store_tx_unlock();
        return ZCL_ERR(-24, "chain evidence: initial capture failed: %s",
                       captured.message);
    }

    struct consensus_state_chain_binding_observation sampled_before;
    capture_binding_predicates(&view_before, manifest, &sampled_before);

    struct chain_state_frontier_view view_after;
    captured = csr_capture_frontiers(
        csr_instance(), &request->main->chain_active,
        &request->main->pindex_best_header, manifest->height,
        &view_after);
    if (!captured.ok) {
        progress_store_tx_unlock();
        return ZCL_ERR(-25, "chain evidence: final capture failed: %s",
                       captured.message);
    }
    chain_frontier_snapshot_collect(&frontier_after, request->main);
    struct consensus_state_chain_binding_observation sampled_after;
    capture_binding_predicates(&view_after, manifest, &sampled_after);

    /* Both samples were zero-initialized before field assignment, so the
     * byte comparison also covers every root/hash/validity/pass predicate
     * without padding carrying indeterminate data. */
    bool predicates_unchanged =
        memcmp(&sampled_before, &sampled_after,
               sizeof(sampled_before)) == 0;
    struct consensus_state_chain_binding_observation observation =
        sampled_after;
    observation.before_frontier_consistent =
        chain_frontier_snapshot_consistent(&frontier_before);
    observation.after_frontier_consistent =
        chain_frontier_snapshot_consistent(&frontier_after);
    observation.durable_served_height = frontier_after.served.height;
    observation.frontier_unchanged = capture_equal(&view_before, &view_after) &&
        chain_frontier_snapshot_equal(&frontier_before, &frontier_after) &&
        capture_matches_frontier(&view_before, &frontier_before) &&
        capture_matches_frontier(&view_after, &frontier_after) &&
        predicates_unchanged;

    struct zcl_result decision = consensus_state_chain_binding_decide(
        manifest, &observation);
    if (!decision.ok) {
        progress_store_tx_unlock();
        return ZCL_ERR(-26, "chain evidence refused: %s", decision.message);
    }
    progress_store_tx_unlock();

    uint8_t final_receipt_digest[32];
    if (!consensus_state_artifact_evidence_receipt_digest(
            request->artifact, final_receipt_digest) ||
        memcmp(final_receipt_digest, artifact_receipt_digest, 32) != 0)
        return ZCL_ERR(-27,
                       "chain evidence: artifact changed during chain capture");

    struct consensus_state_chain_evidence *evidence = zcl_malloc(
        sizeof(*evidence), "consensus state chain evidence");
    if (!evidence)
        return ZCL_ERR(-28, "chain evidence: allocation failed");
    memset(evidence, 0, sizeof(*evidence));
    memcpy(evidence->artifact_receipt_digest, artifact_receipt_digest, 32);
    evidence->target_lane = request->target_lane;
    evidence_digest_build(manifest, artifact_receipt_digest, &observation,
                          &frontier_before,
                          target_lane, evidence->evidence_digest);
    *out = evidence;
    return ZCL_OK;
}

void consensus_state_chain_evidence_free(
    struct consensus_state_chain_evidence *evidence)
{
    free(evidence);
}

bool consensus_state_chain_evidence_matches_artifact(
    const struct consensus_state_chain_evidence *evidence,
    const struct consensus_state_artifact_evidence *artifact,
    enum consensus_state_target_lane target_lane)
{
    uint8_t artifact_receipt_digest[32];
    if (!evidence || !artifact || !target_lane_name(target_lane) ||
        !consensus_state_artifact_evidence_receipt_digest(
            artifact, artifact_receipt_digest))
        return false;
    return memcmp(evidence->artifact_receipt_digest,
                  artifact_receipt_digest, 32) == 0 &&
        evidence->target_lane == target_lane;
}

bool consensus_state_chain_evidence_digest(
    const struct consensus_state_chain_evidence *evidence, uint8_t out[32])
{
    if (!evidence || !out)
        return false;
    memcpy(out, evidence->evidence_digest, 32);
    return true;
}
