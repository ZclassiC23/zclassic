/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * consensus_state_replay_receipt — the independent replay-derived receipt that
 * lifts the bundle-ACTIVATE production containment.
 *
 * A zcl.consensus_state_bundle.v1 file is self-asserted: it can recompute its
 * own digests, and a ZClassic header commits neither the UTXO set nor the
 * Sprout/Sapling anchor or nullifier CONTENTS, so matching a bundle's
 * height/hash to a validated local header authenticates only WHERE the bundle
 * claims to sit, never WHAT it contains. This module closes that gap: given a
 * datadir whose OWN genesis->anchor reducer fold has completed and parked at
 * the bundle's anchor height, it re-derives the transparent + shielded
 * component digests from that datadir's own folded tables (the bundle's tables
 * are NEVER read as derivation input) and, only if every re-derived digest
 * equals the bundle's, writes a receipt OUTSIDE the bundle binding the bundle
 * whole-file digest, the anchor, each independently re-derived component
 * digest, and the verifying binary's own image digest.
 *
 * ACTIVATE consults consensus_state_replay_receipt_authority_available(): the
 * transactional install stays CONTAINED (VERIFIED_CONTAINED, nothing written)
 * unless a valid receipt for EXACTLY this bundle + anchor + component digests
 * exists in the datadir. */

#ifndef ZCL_CONSENSUS_STATE_REPLAY_RECEIPT_H
#define ZCL_CONSENSUS_STATE_REPLAY_RECEIPT_H

#include <stdbool.h>
#include <stdint.h>

#include "storage/consensus_state_bundle_codec.h"

struct sqlite3;

#define CONSENSUS_STATE_REPLAY_RECEIPT_SCHEMA \
    "zcl.consensus_state_replay_receipt.v1"
/* Basename of the keyed receipt file written under the datadir. */
#define CONSENSUS_STATE_REPLAY_RECEIPT_NAME \
    "consensus_state_replay_receipt.v1"

struct consensus_state_replay_result {
    bool verified;
    int32_t height;
    uint64_t utxo_count;
    int64_t total_supply;
    uint64_t anchor_count;
    uint64_t nullifier_count;
    uint8_t bundle_file_digest[32];
    uint8_t verifier_binary_digest[32];
    char receipt_path[256];
    char reason[192];
};

/* Offline replay verifier (the -verify-consensus-bundle=PATH verb).
 *
 * `progress_db` is the datadir's OWN open progress store — the state produced
 * by this node's genesis->anchor reducer fold, parked EXACTLY at the bundle's
 * anchor height (coins_applied == height + 1). The verifier:
 *   1. admits + strictly validates the immutable bundle read-only (reusing the
 *      artifact-evidence validator) and copies its manifest;
 *   2. requires a complete genesis-derived history bundle (no mixed provenance);
 *   3. independently re-derives utxo root/count/supply, the pool-ordered anchor
 *      digest + frontiers, and the nullifier digest from `progress_db`'s own
 *      coins/sprout_anchors/sapling_anchors/nullifiers tables;
 *   4. requires every re-derived value to equal the bundle manifest;
 *   5. writes the fsync'd receipt binding all of the above plus SHA3 of the
 *      running executable image.
 * Fail-closed: any mismatch sets a typed reason and writes NOTHING. Returns
 * true only on a fully verified + persisted receipt. */
bool consensus_state_replay_verify_and_write_receipt(
    struct sqlite3 *progress_db, const char *bundle_path, const char *datadir,
    struct consensus_state_replay_result *out);

/* Fail-closed authority check consulted by ACTIVATE. Returns true iff the
 * datadir holds a well-formed receipt that binds EXACTLY this bundle whole-file
 * digest, this manifest's anchor + component digests, and the currently running
 * executable's image digest. Any missing/tampered/foreign receipt returns
 * false, keeping the install CONTAINED. */
bool consensus_state_replay_receipt_authority_available(
    const struct consensus_state_bundle_manifest *manifest,
    const uint8_t bundle_file_digest[32], const char *datadir);

#endif /* ZCL_CONSENSUS_STATE_REPLAY_RECEIPT_H */
