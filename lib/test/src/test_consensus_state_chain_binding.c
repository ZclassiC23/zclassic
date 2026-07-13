/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "services/consensus_state_chain_binding_service.h"
#include "validation/main_state.h"

#include <stdio.h>
#include <string.h>

#define CSB_CHECK(label, expr) do {                                     \
    printf("consensus_state_chain_binding: %s... ", (label));           \
    if (expr) printf("OK\n");                                          \
    else { printf("FAIL\n"); failures++; }                             \
} while (0)

static struct consensus_state_bundle_manifest valid_manifest(void)
{
    struct consensus_state_bundle_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.height = 100;
    manifest.history_complete = true;
    manifest.source_clean = true;
    manifest.validation_profile = CONSENSUS_STATE_VALIDATION_FULL;
    manifest.sapling_frontier_height = 80;
    manifest.sprout_frontier_height = 70;
    manifest.source_fold_cursor = 101;
    manifest.utxo_count = 5;
    manifest.total_supply = 50;
    manifest.anchor_count = 6;
    manifest.nullifier_count = 7;
    for (size_t i = 0; i < 32; i++) {
        manifest.block_hash[i] = (uint8_t)(1u + i);
        manifest.utxo_root[i] = (uint8_t)(33u + i);
        manifest.anchor_digest[i] = (uint8_t)(65u + i);
        manifest.sprout_frontier_root[i] = (uint8_t)(97u + i);
        manifest.sapling_frontier_root[i] = (uint8_t)(129u + i);
        manifest.nullifier_digest[i] = (uint8_t)(161u + i);
        manifest.proof_manifest_digest[i] = (uint8_t)(193u + i);
        manifest.source_digest[i] = (uint8_t)(225u + i);
    }
    consensus_state_bundle_artifact_digest(&manifest,
                                            manifest.artifact_digest);
    return manifest;
}

static struct consensus_state_chain_binding_observation valid_observation(
    const struct consensus_state_bundle_manifest *manifest)
{
    struct consensus_state_chain_binding_observation observation;
    memset(&observation, 0, sizeof(observation));
    observation.before_frontier_consistent = true;
    observation.after_frontier_consistent = true;
    observation.frontier_unchanged = true;
    observation.durable_served_height = 120;
    observation.selected_bundle_known = true;
    observation.selected_bundle_height = manifest->height;
    memcpy(observation.selected_bundle_hash, manifest->block_hash, 32);
    observation.selected_bundle_valid_scripts = true;
    observation.selected_bundle_failure_free = true;
    observation.bundle_header_pass_record = true;
    observation.selected_sapling_source_known = true;
    observation.selected_sapling_source_height =
        (int32_t)manifest->sapling_frontier_height;
    observation.selected_sapling_source_valid_scripts = true;
    observation.selected_sapling_source_failure_free = true;
    observation.sapling_source_header_pass_record = true;
    memcpy(observation.selected_sapling_source_root,
           manifest->sapling_frontier_root, 32);
    memcpy(observation.selected_bundle_sapling_root,
           manifest->sapling_frontier_root, 32);
    observation.selected_header_known = true;
    observation.selected_header_height = 130;
    observation.selected_header_valid_tree = true;
    observation.selected_header_failure_free = true;
    observation.header_descends_from_bundle = true;
    observation.bundle_descends_from_sapling_source = true;
    for (size_t i = 0; i < 32; i++) {
        observation.selected_sapling_source_hash[i] = (uint8_t)(17u + i);
        observation.selected_header_hash[i] = (uint8_t)(49u + i);
        observation.selected_header_chainwork[i] = (uint8_t)(81u + i);
    }
    return observation;
}

static bool rejected(
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_chain_binding_observation *observation)
{
    return !consensus_state_chain_binding_decide(manifest, observation).ok;
}

int test_consensus_state_chain_binding(void)
{
    int failures = 0;
    struct consensus_state_bundle_manifest manifest = valid_manifest();
    struct consensus_state_chain_binding_observation observation =
        valid_observation(&manifest);
    CSB_CHECK("complete exact selected-chain binding passes",
              consensus_state_chain_binding_decide(
                  &manifest, &observation).ok);

    struct consensus_state_bundle_manifest invalid_profile_manifest = manifest;
    invalid_profile_manifest.validation_profile =
        CONSENSUS_STATE_VALIDATION_INVALID;
    consensus_state_bundle_artifact_digest(
        &invalid_profile_manifest, invalid_profile_manifest.artifact_digest);
    CSB_CHECK("unknown validation profile refuses",
              rejected(&invalid_profile_manifest, &observation));

    struct consensus_state_chain_binding_observation bad = observation;
    bad.frontier_unchanged = false;
    CSB_CHECK("frontier movement refuses", rejected(&manifest, &bad));
    bad = observation;
    bad.durable_served_height = manifest.height - 1;
    CSB_CHECK("bundle above durable H star refuses", rejected(&manifest, &bad));
    bad = observation;
    bad.selected_bundle_hash[0] ^= 1u;
    CSB_CHECK("same-height selected hash mismatch refuses",
              rejected(&manifest, &bad));
    bad = observation;
    bad.selected_bundle_valid_scripts = false;
    CSB_CHECK("insufficient bundle validity refuses", rejected(&manifest, &bad));
    bad = observation;
    bad.bundle_header_pass_record = false;
    CSB_CHECK("missing exact bundle header proof refuses",
              rejected(&manifest, &bad));
    bad = observation;
    memset(bad.selected_sapling_source_hash, 0, 32);
    CSB_CHECK("unknown Sapling source hash refuses", rejected(&manifest, &bad));
    bad = observation;
    bad.selected_sapling_source_root[0] ^= 1u;
    CSB_CHECK("Sapling source-header root mismatch refuses",
              rejected(&manifest, &bad));
    bad = observation;
    bad.selected_bundle_sapling_root[0] ^= 1u;
    CSB_CHECK("stale sparse Sapling frontier at bundle height refuses",
              rejected(&manifest, &bad));
    bad = observation;
    bad.sapling_source_header_pass_record = false;
    CSB_CHECK("missing exact Sapling source proof refuses",
              rejected(&manifest, &bad));
    bad = observation;
    bad.header_descends_from_bundle = false;
    CSB_CHECK("off-chain selected header refuses", rejected(&manifest, &bad));
    bad = observation;
    bad.bundle_descends_from_sapling_source = false;
    CSB_CHECK("off-chain Sapling source refuses", rejected(&manifest, &bad));

    struct consensus_state_bundle_manifest bad_manifest = manifest;
    bad_manifest.artifact_digest[0] ^= 1u;
    CSB_CHECK("unbound manifest digest refuses",
              rejected(&bad_manifest, &observation));
    bad_manifest = manifest;
    bad_manifest.history_complete = false;
    consensus_state_bundle_artifact_digest(
        &bad_manifest, bad_manifest.artifact_digest);
    CSB_CHECK("current-only history refuses selected-chain evidence",
              rejected(&bad_manifest, &observation));

    struct consensus_state_chain_evidence *evidence = (void *)1;
    CSB_CHECK("null build refuses without evidence",
              !consensus_state_chain_evidence_build(NULL, &evidence).ok &&
              evidence == NULL);
    struct main_state uninitialized_main;
    memset(&uninitialized_main, 0, sizeof(uninitialized_main));
    struct consensus_state_chain_binding_request bad_lane_request = {
        .main = &uninitialized_main,
        .artifact = (void *)1,
        .target_lane = CONSENSUS_STATE_TARGET_LANE_UNKNOWN,
    };
    evidence = (void *)1;
    CSB_CHECK("unknown target lane refuses before chain capture",
              !consensus_state_chain_evidence_build(
                  &bad_lane_request, &evidence).ok && evidence == NULL);
    bad_lane_request.target_lane = CONSENSUS_STATE_TARGET_LANE_COPY_PROOF;
    evidence = (void *)1;
    CSB_CHECK("non-singleton chain context refuses before artifact access",
              !consensus_state_chain_evidence_build(
                  &bad_lane_request, &evidence).ok && evidence == NULL);
    uint8_t digest[32];
    CSB_CHECK("opaque evidence accessors reject null",
              !consensus_state_chain_evidence_digest(NULL, digest) &&
              !consensus_state_chain_evidence_matches_artifact(
                  NULL, NULL,
                  CONSENSUS_STATE_TARGET_LANE_COPY_PROOF));

    printf("consensus_state_chain_binding: %s\n",
           failures ? "FAILED" : "ALL PASSED");
    return failures;
}
