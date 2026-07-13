/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Canonical logical encoding for zcl.consensus_state_bundle.v1. */

#include "storage/consensus_state_bundle_codec.h"

#include "crypto/sha3.h"

#include <string.h>

static void digest_u32(struct sha3_256_ctx *ctx, uint32_t value)
{
    uint8_t encoded[4];
    for (size_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t)(value >> (8u * i));
    sha3_256_write(ctx, encoded, sizeof(encoded));
}

static void digest_u64(struct sha3_256_ctx *ctx, uint64_t value)
{
    uint8_t encoded[8];
    for (size_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t)(value >> (8u * i));
    sha3_256_write(ctx, encoded, sizeof(encoded));
}

void consensus_state_bundle_anchor_digest_begin(struct sha3_256_ctx *ctx)
{
    static const char domain[] = "zcl.consensus_state_bundle.v1/anchors";
    sha3_256_init(ctx);
    sha3_256_write(ctx, (const uint8_t *)domain, sizeof(domain));
}

void consensus_state_bundle_anchor_digest_row(
    struct sha3_256_ctx *ctx, uint8_t pool, const uint8_t root[32],
    uint64_t height, const uint8_t *tree, uint32_t tree_len)
{
    sha3_256_write(ctx, &pool, 1);
    sha3_256_write(ctx, root, 32);
    digest_u64(ctx, height);
    digest_u32(ctx, tree_len);
    if (tree_len > 0)
        sha3_256_write(ctx, tree, tree_len);
}

void consensus_state_bundle_nullifier_digest_begin(struct sha3_256_ctx *ctx)
{
    static const char domain[] = "zcl.consensus_state_bundle.v1/nullifiers";
    sha3_256_init(ctx);
    sha3_256_write(ctx, (const uint8_t *)domain, sizeof(domain));
}

void consensus_state_bundle_nullifier_digest_row(
    struct sha3_256_ctx *ctx, uint8_t pool, const uint8_t nf[32],
    uint64_t height)
{
    sha3_256_write(ctx, &pool, 1);
    sha3_256_write(ctx, nf, 32);
    digest_u64(ctx, height);
}

void consensus_state_bundle_artifact_digest(
    const struct consensus_state_bundle_manifest *m, uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    static const char domain[] = "zcl.consensus_state_bundle.v1/artifact";
    sha3_256_write(&ctx, (const uint8_t *)domain, sizeof(domain));
    digest_u64(&ctx, (uint64_t)m->height);
    sha3_256_write(&ctx, m->block_hash, 32);
    uint8_t complete = m->history_complete ? 1 : 0;
    sha3_256_write(&ctx, &complete, 1);
    uint8_t source_clean = m->source_clean ? 1 : 0;
    sha3_256_write(&ctx, &source_clean, 1);
    sha3_256_write(&ctx, &m->validation_profile, 1);
    digest_u64(&ctx, (uint64_t)m->activation_boundary);
    sha3_256_write(&ctx, m->utxo_root, 32);
    digest_u64(&ctx, m->utxo_count);
    digest_u64(&ctx, (uint64_t)m->total_supply);
    sha3_256_write(&ctx, m->anchor_digest, 32);
    digest_u64(&ctx, m->anchor_count);
    sha3_256_write(&ctx, m->sprout_frontier_root, 32);
    digest_u64(&ctx, (uint64_t)m->sprout_frontier_height);
    sha3_256_write(&ctx, m->sapling_frontier_root, 32);
    digest_u64(&ctx, (uint64_t)m->sapling_frontier_height);
    sha3_256_write(&ctx, m->nullifier_digest, 32);
    digest_u64(&ctx, m->nullifier_count);
    digest_u64(&ctx, (uint64_t)m->sprout_source_cursor);
    digest_u64(&ctx, (uint64_t)m->sapling_source_cursor);
    digest_u64(&ctx, (uint64_t)m->nullifier_source_cursor);
    digest_u64(&ctx, (uint64_t)m->source_fold_cursor);
    sha3_256_write(&ctx, m->proof_manifest_digest, 32);
    sha3_256_write(&ctx, m->source_digest, 32);
    sha3_256_finalize(&ctx, out);
}

void consensus_state_source_receipt_digest(
    const struct consensus_state_source_receipt *receipt, uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    static const char domain[] =
        "zcl.consensus_state_source_receipt.v1/receipt";
    sha3_256_write(&ctx, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&ctx, receipt->source_epoch_digest, 32);
    sha3_256_write(&ctx, receipt->source_tree_root, 32);
    sha3_256_write(&ctx, receipt->running_binary_digest, 32);
    sha3_256_write(&ctx, receipt->toolchain_digest, 32);
    sha3_256_write(&ctx, receipt->build_inputs_digest, 32);
    sha3_256_write(&ctx, receipt->chain_corpus_digest, 32);
    uint8_t source_clean = receipt->source_clean ? 1 : 0;
    sha3_256_write(&ctx, &source_clean, 1);
    sha3_256_write(&ctx, &receipt->validation_profile, 1);
    digest_u64(&ctx, 40);
    sha3_256_write(&ctx, (const uint8_t *)receipt->producer_commit, 40);
    digest_u64(&ctx, (uint64_t)receipt->fold_cursor);
    sha3_256_finalize(&ctx, out);
}

void consensus_state_source_epoch_digest(
    const struct consensus_state_source_receipt *receipt, uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    static const char domain[] =
        "zcl.consensus_state_source_epoch.v1/identity";
    sha3_256_write(&ctx, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&ctx, receipt->source_tree_root, 32);
    sha3_256_write(&ctx, receipt->toolchain_digest, 32);
    sha3_256_write(&ctx, receipt->build_inputs_digest, 32);
    uint8_t source_clean = receipt->source_clean ? 1 : 0;
    sha3_256_write(&ctx, &source_clean, 1);
    digest_u64(&ctx, 40);
    sha3_256_write(&ctx, (const uint8_t *)receipt->producer_commit, 40);
    sha3_256_finalize(&ctx, out);
}

void consensus_state_bundle_proof_manifest_digest(
    const struct consensus_state_bundle_proof_summary *summaries,
    size_t count, uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    static const char domain[] =
        "zcl.consensus_state_bundle.v1/proof-summary";
    sha3_256_write(&ctx, (const uint8_t *)domain, sizeof(domain));
    digest_u64(&ctx, (uint64_t)count);
    for (size_t i = 0; i < count; i++) {
        size_t name_len = strnlen(summaries[i].component,
                                  CONSENSUS_STATE_BUNDLE_PROOF_NAME_MAX + 1u);
        digest_u64(&ctx, (uint64_t)name_len);
        sha3_256_write(&ctx, (const uint8_t *)summaries[i].component,
                       name_len);
        digest_u64(&ctx, summaries[i].cursor);
        digest_u64(&ctx, (uint64_t)summaries[i].first_height);
        digest_u64(&ctx, (uint64_t)summaries[i].last_height);
        digest_u64(&ctx, summaries[i].row_count);
        digest_u64(&ctx, summaries[i].hash_bound_count);
        sha3_256_write(&ctx, summaries[i].component_digest, 32);
    }
    sha3_256_finalize(&ctx, out);
}
