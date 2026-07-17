/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * consensus_state_snapshot_install_checkpoint_authority.c — the ACTIVATE
 * authority lattice for the sovereign shielded-state cure. Split from
 * consensus_state_snapshot_install_activate.c (one focused responsibility): it
 * decides WHICH authority, if any, may lift ACTIVATE containment. The atomic
 * cutover engine lives in install_activate.c; this file never touches state.
 *
 * Two independent authorities can lift containment, each binding the bundle to
 * something OUTSIDE the bundle's own self-asserted digests. ZClassic headers do
 * not commit the transparent/shielded state sets, so the bundle's self-recomputed
 * digests cannot authorize a live state replacement even when every internal
 * digest and selected-chain height/hash check passes:
 *
 *   RECEIPT — an independent replay-derived receipt on THIS datadir re-derived
 *   every component digest from its own folded tables and bound this exact
 *   bundle whole-file digest + artifact + anchor + component digests + the
 *   running binary image (config/consensus_state_replay_receipt.h).
 *
 *   CHECKPOINT_CONTENT — the bundle's coins reproduce the compiled SHA3 UTXO
 *   checkpoint (sha3 + count at the checkpoint height) AND its Sapling tip
 *   frontier Pedersen-roots to the caller's validated-header committed
 *   hashFinalSaplingRoot. Bound to the compiled binary + PoW, this is
 *   cryptographically STRONGER than a fold-process receipt — the state's
 *   authority is the compiled checkpoint content, not a producer attestation.
 *
 * Precedence: RECEIPT first (unchanged byte-for-byte), then CHECKPOINT_CONTENT.
 * Without either the install stays VERIFIED_CONTAINED and writes nothing. */

#include "consensus_state_snapshot_install_internal.h"

#include "config/consensus_state_replay_receipt.h" /* replay-receipt authority */
#include "chain/checkpoints.h"           /* get_sha3_utxo_checkpoint */
#include "storage/consensus_state_bundle_codec.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

const char *consensus_state_activate_authority_name(
    enum consensus_state_activate_authority authority)
{
    switch (authority) {
    case CONSENSUS_STATE_ACTIVATE_AUTHORITY_RECEIPT:
        return "receipt";
    case CONSENSUS_STATE_ACTIVATE_AUTHORITY_CHECKPOINT_CONTENT:
        return "checkpoint_content";
    default:
        return "none";
    }
}

#ifdef ZCL_TESTING
static _Atomic bool g_activate_test_independent_authority = false;

void consensus_state_snapshot_install_activate_test_set_independent_authority(
    bool available)
{
    atomic_store(&g_activate_test_independent_authority, available);
}
#endif

/* RECEIPT authority: a valid on-disk replay receipt binding this exact bundle
 * whole-file digest. The ZCL_TESTING seam lets the hermetic copy-proof harness
 * stand in for one. */
static bool activate_receipt_authority_available(
    const struct consensus_state_artifact_evidence *evidence, int datadir_fd,
    const struct consensus_state_bundle_manifest *manifest)
{
#ifdef ZCL_TESTING
    if (atomic_load(&g_activate_test_independent_authority))
        return true;
#endif
    uint8_t bundle_file_digest[32];
    if (!consensus_state_artifact_evidence_file_digest(evidence,
                                                       bundle_file_digest))
        return false;
    return consensus_state_replay_receipt_authority_available(
        manifest, bundle_file_digest, datadir_fd);
}

/* CHECKPOINT_CONTENT verdict for a receipt-less activation. */
enum activate_checkpoint_content_verdict {
    /* Not a checkpoint-content bundle here: no compiled checkpoint, a height off
     * the checkpoint, or coins that do not reproduce the compiled SHA3. */
    ACTIVATE_CC_NONMATCH = 0,
    /* Coins reproduce the compiled checkpoint, but the Sapling tip frontier is
     * not bound to a validated-header hashFinalSaplingRoot on this datadir. */
    ACTIVATE_CC_CONTAIN,
    /* Coins reproduce the compiled checkpoint AND the Sapling tip frontier
     * Pedersen-roots to the caller's validated-header hashFinalSaplingRoot. */
    ACTIVATE_CC_ACTIVATE,
};

/* Evaluate the CHECKPOINT_CONTENT authority from the content-bound manifest.
 *
 * manifest->utxo_root / utxo_count / sapling_frontier_root were re-derived from
 * the bundle's OWN coins + Sapling anchor rows by the admission validator
 * (consensus_state_bundle_validate: utxo_commitment_sha3_write_record over the
 * coins, incremental_tree_root over the highest Sapling frontier — the SAME
 * digest primitives the exporter's coins_kv_commitment / anchor tree-root use),
 * and consensus_state_artifact_evidence_revalidate pins that whole-file digest.
 * install_activate.c's activate_verify_destination independently re-folds the
 * INSTALLED destination (coins_kv_commitment + destination anchor tree roots)
 * and requires it to reproduce these exact manifest values before COMMIT. So
 * asserting the manifest reproduces the compiled checkpoint transitively binds
 * the live installed coins/frontier to the compiled binary's SHA3 UTXO
 * checkpoint and the PoW header's final Sapling root — one canonical fold, no
 * second digest implementation, no producer-trusted claim.
 *
 * The Sapling root binding requires a validated-header source: ZClassic headers
 * commit hashFinalSaplingRoot (PoW-protected) but the compiled UTXO checkpoint
 * does not carry it, so an installer without a validated header at the
 * checkpoint height can only CONTAIN, never activate on an unverifiable root. */
static enum activate_checkpoint_content_verdict
activate_checkpoint_content_evaluate(
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_activate_request *request)
{
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp || manifest->height != cp->height ||
        manifest->utxo_count != cp->utxo_count ||
        memcmp(manifest->utxo_root, cp->sha3_hash, 32) != 0)
        return ACTIVATE_CC_NONMATCH;
    /* Coins reproduce the compiled checkpoint. The Sapling tip frontier must
     * Pedersen-root to a PoW-committed hashFinalSaplingRoot the CALLER read
     * from its own validated header chain — never a bundle-carried root. */
    if (!request->checkpoint_sapling_root_from_validated_header ||
        memcmp(manifest->sapling_frontier_root,
               request->checkpoint_sapling_root, 32) != 0)
        return ACTIVATE_CC_CONTAIN;
    return ACTIVATE_CC_ACTIVATE;
}

enum consensus_state_activate_authority
consensus_state_activate_resolve_authority(
    const struct consensus_state_artifact_evidence *evidence, int datadir_fd,
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_activate_request *request,
    char *contained_reason, size_t reason_cap)
{
    if (contained_reason && reason_cap)
        contained_reason[0] = '\0';
    if (!evidence || !manifest || !request)
        return CONSENSUS_STATE_ACTIVATE_AUTHORITY_NONE;
    if (activate_receipt_authority_available(evidence, datadir_fd, manifest))
        return CONSENSUS_STATE_ACTIVATE_AUTHORITY_RECEIPT;

    switch (activate_checkpoint_content_evaluate(manifest, request)) {
    case ACTIVATE_CC_ACTIVATE:
        return CONSENSUS_STATE_ACTIVATE_AUTHORITY_CHECKPOINT_CONTENT;
    case ACTIVATE_CC_CONTAIN:
        if (contained_reason && reason_cap)
            snprintf(contained_reason, reason_cap,
                     "bundle coins reproduce the compiled SHA3 UTXO checkpoint "
                     "but its Sapling tip frontier is not bound to a validated-"
                     "header hashFinalSaplingRoot on this datadir; import the "
                     "header chain to the checkpoint height, or supply an "
                     "independent replay-derived receipt (install stays "
                     "contained)");
        return CONSENSUS_STATE_ACTIVATE_AUTHORITY_NONE;
    default:
        if (contained_reason && reason_cap)
            snprintf(contained_reason, reason_cap,
                     "no independent replay-derived receipt or checkpoint-"
                     "content proof authorizes this bundle; run "
                     "-verify-consensus-bundle against a datadir folded to the "
                     "anchor first (install stays contained)");
        return CONSENSUS_STATE_ACTIVATE_AUTHORITY_NONE;
    }
}
