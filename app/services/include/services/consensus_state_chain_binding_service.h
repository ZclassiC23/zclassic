/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Fail-closed binding of a contained consensus-state bundle to the node's
 * selected, durably validated ZClassic header chain. */

#ifndef ZCL_SERVICES_CONSENSUS_STATE_CHAIN_BINDING_SERVICE_H
#define ZCL_SERVICES_CONSENSUS_STATE_CHAIN_BINDING_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/consensus_state_bundle_codec.h"
#include "util/result.h"

struct main_state;
struct consensus_state_artifact_evidence;
struct consensus_state_chain_evidence;

enum consensus_state_target_lane {
    CONSENSUS_STATE_TARGET_LANE_UNKNOWN = 0,
    CONSENSUS_STATE_TARGET_LANE_COPY_PROOF,
    CONSENSUS_STATE_TARGET_LANE_DEV,
    CONSENSUS_STATE_TARGET_LANE_SOAK,
    CONSENSUS_STATE_TARGET_LANE_CANONICAL,
};

/* Public only so the fail-closed decision matrix can be tested without a
 * process-global progress store.  This structure is never publication
 * authority: only consensus_state_chain_evidence_build() can create the
 * opaque evidence object, and no publisher currently consumes it. */
struct consensus_state_chain_binding_observation {
    bool before_frontier_consistent;
    bool after_frontier_consistent;
    bool frontier_unchanged;
    int32_t durable_served_height;

    bool selected_bundle_known;
    int32_t selected_bundle_height;
    uint8_t selected_bundle_hash[32];
    bool selected_bundle_valid_scripts;
    bool selected_bundle_failure_free;
    bool bundle_header_pass_record;

    bool selected_sapling_source_known;
    int32_t selected_sapling_source_height;
    uint8_t selected_sapling_source_hash[32];
    bool selected_sapling_source_valid_scripts;
    bool selected_sapling_source_failure_free;
    bool sapling_source_header_pass_record;
    uint8_t selected_sapling_source_root[32];
    uint8_t selected_bundle_sapling_root[32];

    bool selected_header_known;
    int32_t selected_header_height;
    uint8_t selected_header_hash[32];
    uint8_t selected_header_chainwork[32];
    bool selected_header_valid_tree;
    bool selected_header_failure_free;
    bool header_descends_from_bundle;
    bool bundle_descends_from_sapling_source;
};

struct consensus_state_chain_binding_request {
    struct main_state *main;
    /* Must originate from the strict external-bundle validator. A manifest
     * copy is intentionally insufficient to construct chain evidence. */
    const struct consensus_state_artifact_evidence *artifact;
    /* A canonical lane tag is included in the evidence digest. It is
     * descriptive binding, not permission to activate that lane. */
    enum consensus_state_target_lane target_lane;
};

/* Pure refusal decision. A passing result is diagnostic only and cannot be
 * converted into publication evidence by callers. */
struct zcl_result consensus_state_chain_binding_decide(
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_chain_binding_observation *observation);

/* Process-singleton selected-chain evidence. The artifact must be an opaque
 * strict-validator receipt. Capture selected-chain state before and after all
 * checks, require durable exact-hash header-pass rows, and issue immutable
 * opaque evidence only when every predicate passes. No bundle or node state is
 * mutated. This API does not support arbitrary copy/lane store contexts: its
 * reducer/progress authority is the open process singleton. The evidence is
 * diagnostic, not activation authority and can become stale immediately. A
 * publisher must re-capture inside its own quiesced publication transaction. */
struct zcl_result consensus_state_chain_evidence_build(
    const struct consensus_state_chain_binding_request *request,
    struct consensus_state_chain_evidence **out);

void consensus_state_chain_evidence_free(
    struct consensus_state_chain_evidence *evidence);
bool consensus_state_chain_evidence_matches_artifact(
    const struct consensus_state_chain_evidence *evidence,
    const struct consensus_state_artifact_evidence *artifact,
    enum consensus_state_target_lane target_lane);
bool consensus_state_chain_evidence_digest(
    const struct consensus_state_chain_evidence *evidence,
    uint8_t out[32]);

#endif /* ZCL_SERVICES_CONSENSUS_STATE_CHAIN_BINDING_SERVICE_H */
