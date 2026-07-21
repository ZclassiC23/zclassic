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

/* Compiled-checkpoint content authority matching the given manifest exactly:
 * same height, block_hash, and Sapling frontier (root + height). This is what
 * the boot install verb fills from get_sha3_utxo_checkpoint()/
 * get_rom_state_checkpoint() when the bundle IS the compiled checkpoint. */
static struct consensus_state_checkpoint_authority matching_authority(
    const struct consensus_state_bundle_manifest *manifest)
{
    struct consensus_state_checkpoint_authority cp;
    memset(&cp, 0, sizeof(cp));
    cp.available = true;
    cp.height = manifest->height;
    memcpy(cp.block_hash, manifest->block_hash, 32);
    cp.sapling_frontier_height = (int32_t)manifest->sapling_frontier_height;
    memcpy(cp.sapling_frontier_root, manifest->sapling_frontier_root, 32);
    return cp;
}

/* A snapshot-seeded target: its block index / header-pass / script-validate
 * logs floor ABOVE the checkpoint height, so the checkpoint block and its
 * Sapling source are NEVER materialized (unknown / absent ancestry). The
 * target's OWN frontier is durable and its materialized header tip is real and
 * valid ABOVE the bundle height — exactly the shape capture_binding_predicates
 * produces on such a target. Every below-checkpoint materialized predicate is
 * left false; the checkpoint authority is the only thing that can bind them. */
static struct consensus_state_chain_binding_observation seeded_target_observation(
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_checkpoint_authority *authority)
{
    struct consensus_state_chain_binding_observation observation;
    memset(&observation, 0, sizeof(observation));
    /* Target-derived predicates that stay in force under authority. */
    observation.before_frontier_consistent = true;
    observation.after_frontier_consistent = true;
    observation.frontier_unchanged = true;
    observation.durable_served_height = manifest->height + 20;
    observation.selected_header_known = true;
    observation.selected_header_height = manifest->height + 20;
    observation.selected_header_valid_tree = true;
    observation.selected_header_failure_free = true;
    for (size_t i = 0; i < 32; i++) {
        observation.selected_header_hash[i] = (uint8_t)(49u + i);
        observation.selected_header_chainwork[i] = (uint8_t)(81u + i);
    }
    /* Below-checkpoint materialized-index evidence is ABSENT on a seeded
     * target: bundle unknown, sapling source unknown, ancestry unprovable. */
    observation.selected_bundle_known = false;
    observation.selected_sapling_source_known = false;
    observation.header_descends_from_bundle = false;
    observation.bundle_descends_from_sapling_source = false;
    if (authority)
        observation.checkpoint_authority = *authority;
    return observation;
}

/* A headers-first ("instant-on") node: it has PoW-validated the FULL header
 * chain (a selected header tip well above the checkpoint) but folded ZERO
 * bodies — durable served H* is genesis (0). The block at the checkpoint height
 * IS present on its selected chain carrying the baked block_hash and a durable
 * validate_headers pass record (full Equihash PoW), but no body has connected so
 * scripts are unvalidated. This is exactly the shape that must ADMIT the baked
 * checkpoint via -4 path (b) WITHOUT a fold, and it is DISTINCT from the
 * seeded_target shape above (there the checkpoint block is absent; here it is
 * present-but-bodiless with served H* below the checkpoint). */
static struct consensus_state_chain_binding_observation
header_bootstrap_observation(
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_checkpoint_authority *authority)
{
    struct consensus_state_chain_binding_observation observation;
    memset(&observation, 0, sizeof(observation));
    /* -3 frontier durability holds (the target frontier is durable/consistent). */
    observation.before_frontier_consistent = true;
    observation.after_frontier_consistent = true;
    observation.frontier_unchanged = true;
    /* Zero bodies folded: durable served H* is genesis, BELOW the checkpoint, so
     * -4 path (a) cannot fire — only the header-bootstrap path (b) can. */
    observation.durable_served_height = 0;
    /* The checkpoint-height block is on the selected chain with the baked hash
     * and a durable PoW pass record, but scripts are NOT validated (no body). */
    observation.selected_bundle_known = true;
    observation.selected_bundle_height = manifest->height;
    memcpy(observation.selected_bundle_hash, manifest->block_hash, 32);
    observation.selected_bundle_valid_scripts = false;
    observation.selected_bundle_failure_free = true;
    observation.bundle_header_pass_record = true;
    /* Materialized selected header tip ABOVE the checkpoint with real work —
     * -11 header-tip validity must still hold on the bootstrap path. */
    observation.selected_header_known = true;
    observation.selected_header_height = manifest->height + 20;
    observation.selected_header_valid_tree = true;
    observation.selected_header_failure_free = true;
    for (size_t i = 0; i < 32; i++) {
        observation.selected_header_hash[i] = (uint8_t)(49u + i);
        observation.selected_header_chainwork[i] = (uint8_t)(81u + i);
    }
    if (authority)
        observation.checkpoint_authority = *authority;
    return observation;
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
    CSB_CHECK("checkpoint-authority accessor rejects null",
              !consensus_state_chain_evidence_used_checkpoint_authority(NULL));

    /* ── Compiled-checkpoint content authority (FIX-A) ─────────────────────
     * On a snapshot-seeded target the checkpoint block is never materialized,
     * so the below-checkpoint predicates would refuse on absent index evidence.
     * When the bundle IS the compiled checkpoint (height + block_hash + Sapling
     * frontier match byte-for-byte), the compiled checkpoint authorizes them. */
    struct consensus_state_checkpoint_authority authority =
        matching_authority(&manifest);

    /* (a) Seeded target (index floors above checkpoint) + bundle at checkpoint
     * height with matching compiled content → ADMIT via checkpoint authority. */
    struct consensus_state_chain_binding_observation seeded =
        seeded_target_observation(&manifest, &authority);
    CSB_CHECK("checkpoint-height bundle on seeded target ADMITs via authority",
              consensus_state_chain_binding_decide(&manifest, &seeded).ok);
    CSB_CHECK("authority predicate reports the substitution was used",
              consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &seeded));

    /* Same seeded target WITHOUT the authority (available=false) → the pure
     * target-derived gate refuses at -5 exactly as before (bundle absent). */
    struct consensus_state_chain_binding_observation seeded_no_auth =
        seeded_target_observation(&manifest, NULL);
    struct zcl_result no_auth = consensus_state_chain_binding_decide(
        &manifest, &seeded_no_auth);
    CSB_CHECK("seeded target with no authority refuses at -5",
              !no_auth.ok && no_auth.code == -5 &&
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &seeded_no_auth));

    /* (b) Bundle content (block_hash) mismatches the compiled checkpoint →
     * authority does NOT apply → refuse exactly as today (bundle absent → -5). */
    struct consensus_state_checkpoint_authority bad_hash_auth = authority;
    bad_hash_auth.block_hash[0] ^= 1u;
    struct consensus_state_chain_binding_observation seeded_bad_hash =
        seeded_target_observation(&manifest, &bad_hash_auth);
    struct zcl_result bad_hash = consensus_state_chain_binding_decide(
        &manifest, &seeded_bad_hash);
    CSB_CHECK("checkpoint block_hash mismatch refuses (no authority) at -5",
              !bad_hash.ok && bad_hash.code == -5 &&
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &seeded_bad_hash));

    /* Sapling frontier root mismatch is likewise not authorized. */
    struct consensus_state_checkpoint_authority bad_root_auth = authority;
    bad_root_auth.sapling_frontier_root[0] ^= 1u;
    struct consensus_state_chain_binding_observation seeded_bad_root =
        seeded_target_observation(&manifest, &bad_root_auth);
    CSB_CHECK("checkpoint Sapling-root mismatch refuses (no authority)",
              rejected(&manifest, &seeded_bad_root) &&
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &seeded_bad_root));

    /* Sapling frontier HEIGHT mismatch is not authorized either. */
    struct consensus_state_checkpoint_authority bad_sh_auth = authority;
    bad_sh_auth.sapling_frontier_height ^= 1;
    struct consensus_state_chain_binding_observation seeded_bad_sh =
        seeded_target_observation(&manifest, &bad_sh_auth);
    CSB_CHECK("checkpoint Sapling-height mismatch refuses (no authority)",
              rejected(&manifest, &seeded_bad_sh) &&
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &seeded_bad_sh));

    /* (c) Bundle at a NON-checkpoint height on the same seeded target → the
     * authority (pinned to the checkpoint height) does not apply → -5 exactly
     * as before. Model this by an authority whose height differs from the
     * manifest, which is what get_*_checkpoint() yields for a non-checkpoint
     * bundle. */
    struct consensus_state_checkpoint_authority off_height_auth = authority;
    off_height_auth.height = manifest.height + 1;
    struct consensus_state_chain_binding_observation seeded_off_height =
        seeded_target_observation(&manifest, &off_height_auth);
    struct zcl_result off_height = consensus_state_chain_binding_decide(
        &manifest, &seeded_off_height);
    CSB_CHECK("non-checkpoint-height bundle refuses at -5 (authority inert)",
              !off_height.ok && off_height.code == -5 &&
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &seeded_off_height));

    /* Authority available=false is inert even at the checkpoint height. */
    struct consensus_state_checkpoint_authority unavailable_auth = authority;
    unavailable_auth.available = false;
    CSB_CHECK("unavailable authority does not substitute",
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest,
                  &(struct consensus_state_chain_binding_observation){
                      .checkpoint_authority = unavailable_auth}));

    /* Target-derived guards stay in force UNDER authority: -3 (frontier
     * durability), -4 (durable served H*), and the header-tip validity part of
     * -11 must still refuse even when checkpoint content is authorized. */
    struct consensus_state_chain_binding_observation auth_bad_frontier = seeded;
    auth_bad_frontier.frontier_unchanged = false;
    CSB_CHECK("authority does not relax -3 frontier durability",
              rejected(&manifest, &auth_bad_frontier));
    struct consensus_state_chain_binding_observation auth_bad_hstar = seeded;
    auth_bad_hstar.durable_served_height = manifest.height - 1;
    CSB_CHECK("authority does not relax -4 durable served H*",
              rejected(&manifest, &auth_bad_hstar));
    struct consensus_state_chain_binding_observation auth_bad_header = seeded;
    auth_bad_header.selected_header_valid_tree = false;
    CSB_CHECK("authority does not relax -11 header-tip validity",
              rejected(&manifest, &auth_bad_header));
    struct consensus_state_chain_binding_observation auth_no_header = seeded;
    auth_no_header.selected_header_known = false;
    CSB_CHECK("authority still requires a materialized header tip",
              rejected(&manifest, &auth_no_header));

    /* A fully-materialized (from-genesis) target binds through the NORMAL
     * predicates. With an authority present but pinned to a different height
     * (so the substitution is inert), the real index evidence must still pass
     * every below-checkpoint predicate on its own. */
    struct consensus_state_chain_binding_observation materialized_inert_auth =
        observation;
    materialized_inert_auth.checkpoint_authority = off_height_auth;
    CSB_CHECK("materialized target binds via normal path (authority inert)",
              consensus_state_chain_binding_decide(
                  &manifest, &materialized_inert_auth).ok &&
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &materialized_inert_auth));

    /* ── Header-bootstrap install (the instant-on keystone, FIX-B) ─────────
     * A blank node fast-syncs the header chain (PoW-validated to tip) and then
     * installs the baked checkpoint state WITHOUT first folding bodies through
     * the checkpoint height (durable served H* = 0). -4 path (b) admits this iff
     * — under compiled-checkpoint authority — the node has PoW-validated the
     * header at EXACTLY the checkpoint height whose hash equals the baked
     * block_hash (selected_bundle_* at the checkpoint height + a durable
     * validate_headers pass record). Gated on cp_auth so a non-checkpoint bundle
     * can never bootstrap; -3 and -11 stay enforced on the bootstrap path. */
    struct consensus_state_chain_binding_observation bootstrap =
        header_bootstrap_observation(&manifest, &authority);

    /* (1) Headers-first bootstrap ADMITTED without a fold. */
    CSB_CHECK("headers-first bootstrap admits install without a fold (H*=0)",
              consensus_state_chain_binding_decide(&manifest, &bootstrap).ok);
    CSB_CHECK("bootstrap admit reports checkpoint authority was used",
              consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &bootstrap));

    /* (2) Wrong block at the checkpoint height (hash != baked block_hash) →
     * refused: the pass record no longer keys the baked value. */
    struct consensus_state_chain_binding_observation bootstrap_wrong_block =
        bootstrap;
    bootstrap_wrong_block.selected_bundle_hash[0] ^= 1u;
    struct zcl_result wrong_block = consensus_state_chain_binding_decide(
        &manifest, &bootstrap_wrong_block);
    CSB_CHECK("bootstrap wrong-block header refuses at -4",
              !wrong_block.ok && wrong_block.code == -4);

    /* (3) Header present + hash-matching but NOT PoW-validated (no durable
     * validate_headers pass record) → refused at -4: the sovereign fact (full
     * Equihash PoW pass at this exact height+hash) is absent. */
    struct consensus_state_chain_binding_observation bootstrap_unvalidated =
        bootstrap;
    bootstrap_unvalidated.bundle_header_pass_record = false;
    struct zcl_result unvalidated = consensus_state_chain_binding_decide(
        &manifest, &bootstrap_unvalidated);
    CSB_CHECK("bootstrap unvalidated header (no pass record) refuses at -4",
              !unvalidated.ok && unvalidated.code == -4);

    /* (4) NON-checkpoint bundle cannot bootstrap: cp_auth false (manifest does
     * not byte-reproduce the baked checkpoint) → path (b) unavailable, and
     * served H*=0 fails path (a) → refused at -4. */
    struct consensus_state_checkpoint_authority non_checkpoint_auth = authority;
    non_checkpoint_auth.block_hash[0] ^= 1u;  /* manifest != baked */
    struct consensus_state_chain_binding_observation bootstrap_non_cp =
        header_bootstrap_observation(&manifest, &non_checkpoint_auth);
    struct zcl_result non_cp = consensus_state_chain_binding_decide(
        &manifest, &bootstrap_non_cp);
    CSB_CHECK("non-checkpoint bundle cannot bootstrap (refused at -4)",
              !non_cp.ok && non_cp.code == -4 &&
              !consensus_state_chain_binding_uses_checkpoint_authority(
                  &manifest, &bootstrap_non_cp));

    /* And an unavailable authority (no checkpoint compiled) is likewise inert:
     * the bootstrap path needs cp_auth, so served H*=0 refuses at -4. */
    struct consensus_state_checkpoint_authority unavailable_boot = authority;
    unavailable_boot.available = false;
    struct consensus_state_chain_binding_observation bootstrap_no_auth =
        header_bootstrap_observation(&manifest, &unavailable_boot);
    struct zcl_result boot_no_auth = consensus_state_chain_binding_decide(
        &manifest, &bootstrap_no_auth);
    CSB_CHECK("bootstrap with unavailable authority refuses at -4",
              !boot_no_auth.ok && boot_no_auth.code == -4);

    /* (5) No regression: the state-replacement path (durable served H* >= the
     * checkpoint height) still admits — path (a) is unchanged by path (b). */
    struct consensus_state_chain_binding_observation bootstrap_replaced =
        bootstrap;
    bootstrap_replaced.durable_served_height = manifest.height;
    CSB_CHECK("state-replacement path still admits (served H* >= height)",
              consensus_state_chain_binding_decide(
                  &manifest, &bootstrap_replaced).ok);

    printf("consensus_state_chain_binding: %s\n",
           failures ? "FAILED" : "ALL PASSED");
    return failures;
}
