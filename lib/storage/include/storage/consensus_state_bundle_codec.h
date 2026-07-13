/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Canonical logical encoding for zcl.consensus_state_bundle.v1. */

#ifndef ZCL_STORAGE_CONSENSUS_STATE_BUNDLE_CODEC_H
#define ZCL_STORAGE_CONSENSUS_STATE_BUNDLE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONSENSUS_STATE_BUNDLE_SCHEMA "zcl.consensus_state_bundle.v1"
#define CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA \
    "zcl.consensus_state_source_receipt.v1"
#define CONSENSUS_STATE_SOURCE_EPOCH_META_KEY \
    "consensus_state.source_epoch_digest"
#define CONSENSUS_STATE_BUNDLE_PROOF_COUNT 8u
#define CONSENSUS_STATE_BUNDLE_PROOF_NAME_MAX 31u

/* Proof work actually performed by the producer.  These values are part of
 * the canonical bundle digest; a checkpoint-matched state fold must never be
 * mistaken for a full local replay of scripts and shielded proofs. */
enum consensus_state_validation_profile {
    CONSENSUS_STATE_VALIDATION_INVALID = 0,
    CONSENSUS_STATE_VALIDATION_FULL = 1,
    CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD = 2,
};

struct sha3_256_ctx;

/* Producer claims bound to an exact running executable and chain corpus. The
 * bundle preserves these fields so offline verification can recompute the
 * receipt digest. source_tree_root and toolchain_digest are claims, not an
 * independent rebuild proof; their authority comes from the known executable
 * that emitted and durably recorded this receipt. */
struct consensus_state_source_receipt {
    uint8_t source_epoch_digest[32];
    uint8_t source_tree_root[32];
    uint8_t running_binary_digest[32];
    uint8_t toolchain_digest[32];
    uint8_t build_inputs_digest[32];
    uint8_t chain_corpus_digest[32];
    bool source_clean;
    uint8_t validation_profile;
    char producer_commit[41]; /* exact lowercase full Git SHA-1 */
    int64_t fold_cursor;
    uint8_t receipt_digest[32];
};

/* Canonical summaries of producer evidence: header chain plus seven reducer
 * stages. component_digest commits the complete source rows inspected by the
 * exporter. These are evidence emitted by the receipt-bound executable, not
 * ZClassic header commitments and never peer-state PoW authentication. */
struct consensus_state_bundle_proof_summary {
    char component[CONSENSUS_STATE_BUNDLE_PROOF_NAME_MAX + 1u];
    uint64_t cursor;
    int64_t first_height;
    int64_t last_height;
    uint64_t row_count;
    uint64_t hash_bound_count;
    uint8_t component_digest[32];
};

struct consensus_state_bundle_manifest {
    int32_t height;
    uint8_t block_hash[32];
    bool history_complete;
    bool source_clean;
    uint8_t validation_profile;
    int64_t activation_boundary;
    uint8_t utxo_root[32];
    uint64_t utxo_count;
    int64_t total_supply;
    uint8_t anchor_digest[32];
    uint64_t anchor_count;
    uint8_t sprout_frontier_root[32];
    int64_t sprout_frontier_height;
    uint8_t sapling_frontier_root[32];
    int64_t sapling_frontier_height;
    uint8_t nullifier_digest[32];
    uint64_t nullifier_count;
    int64_t sprout_source_cursor;
    int64_t sapling_source_cursor;
    int64_t nullifier_source_cursor;
    int64_t source_fold_cursor;
    uint8_t proof_manifest_digest[32];
    uint8_t source_digest[32];
    uint8_t artifact_digest[32];
};

void consensus_state_bundle_anchor_digest_begin(struct sha3_256_ctx *ctx);
void consensus_state_bundle_anchor_digest_row(
    struct sha3_256_ctx *ctx, uint8_t pool, const uint8_t root[32],
    uint64_t height, const uint8_t *tree, uint32_t tree_len);
void consensus_state_bundle_nullifier_digest_begin(struct sha3_256_ctx *ctx);
void consensus_state_bundle_nullifier_digest_row(
    struct sha3_256_ctx *ctx, uint8_t pool, const uint8_t nf[32],
    uint64_t height);
void consensus_state_bundle_artifact_digest(
    const struct consensus_state_bundle_manifest *manifest, uint8_t out[32]);
void consensus_state_source_receipt_digest(
    const struct consensus_state_source_receipt *receipt, uint8_t out[32]);
void consensus_state_source_epoch_digest(
    const struct consensus_state_source_receipt *receipt, uint8_t out[32]);
void consensus_state_bundle_proof_manifest_digest(
    const struct consensus_state_bundle_proof_summary *summaries,
    size_t count, uint8_t out[32]);

#endif /* ZCL_STORAGE_CONSENSUS_STATE_BUNDLE_CODEC_H */
