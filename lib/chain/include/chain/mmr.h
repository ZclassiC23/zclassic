/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Merkle Mountain Range (MMR) — append-only authenticated data structure
 * over block hashes. Enables O(log n) inclusion proofs between power nodes.
 *
 * Uses SHA3-256 with domain separation:
 *   Leaf:     SHA3-256(0x00 || block_hash)
 *   Internal: SHA3-256(0x01 || left || right)
 *   Root:     SHA3-256(0x02 || peak_0 || peak_1 || ... || peak_k)
 */

#ifndef ZCL_CHAIN_MMR_H
#define ZCL_CHAIN_MMR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MMR_HASH_SIZE 32
#define MMR_MAX_PEAKS 64  /* supports up to 2^64 leaves */

/* Domain separation tags */
#define MMR_TAG_LEAF     0x00
#define MMR_TAG_INTERNAL 0x01
#define MMR_TAG_ROOT     0x02

struct mmr {
    uint64_t num_leaves;
    uint8_t  peaks[MMR_MAX_PEAKS][MMR_HASH_SIZE];
    uint32_t num_peaks;
};

/* Initialize empty MMR */
void mmr_init(struct mmr *m);

/* Append a block hash as a new leaf. Updates peaks and returns
 * the number of new internal nodes created (for persistence). */
int mmr_append(struct mmr *m, const uint8_t block_hash[32]);

/* Compute the MMR root from current peaks */
void mmr_root(const struct mmr *m, uint8_t out[32]);

/* Hash a leaf (exposed for testing/persistence) */
void mmr_hash_leaf(const uint8_t block_hash[32], uint8_t out[32]);

/* Hash two children into a parent */
void mmr_hash_internal(const uint8_t left[32], const uint8_t right[32],
                       uint8_t out[32]);

/* ── Serialization ─────────────────────────────────────── */

/* Max serialized size: 8 (num_leaves) + 4 (num_peaks) + peaks */
#define MMR_SERIALIZED_MAX (8 + 4 + MMR_MAX_PEAKS * MMR_HASH_SIZE)

size_t mmr_serialize(const struct mmr *m, uint8_t *buf, size_t buflen);
bool mmr_deserialize(struct mmr *m, const uint8_t *buf, size_t len);

/* ── Inclusion proofs ──────────────────────────────────── */

struct mmr_proof {
    uint64_t leaf_index;                    /* 0-based leaf index */
    uint8_t  leaf_hash[MMR_HASH_SIZE];      /* SHA3(0x00 || block_hash) */
    uint8_t  siblings[MMR_MAX_PEAKS][MMR_HASH_SIZE];
    uint32_t num_siblings;
    uint8_t  peak_hashes[MMR_MAX_PEAKS][MMR_HASH_SIZE];
    uint32_t num_peaks;
    uint64_t mmr_size;                      /* num_leaves at proof time */
};

/* Generate an inclusion proof for the leaf at `leaf_index` within an MMR
 * of `num_leaves` leaves. `all_leaves` is the full ordered array of raw
 * leaf inputs (block hashes); the function hashes each with
 * mmr_hash_leaf and replays the append/peak-merge sequence to recover
 * the target's authentication path, filling `proof` with the target
 * leaf hash, the sibling hashes, and all peak hashes at proof time, then
 * returns true.
 *
 * Precondition: `leaf_index < num_leaves` and both pointers non-NULL;
 * otherwise the reason is logged and false is returned. O(n) — rebuilds
 * the full MMR, so it is a prover-side operation only.
 *
 * Source: src/mmr.c (mmr_prove_from_leaves). */
bool mmr_prove_from_leaves(const uint8_t (*all_leaves)[32],
                           uint64_t num_leaves,
                           uint64_t leaf_index,
                           struct mmr_proof *proof);

/* Verify an inclusion proof against an expected MMR root.
 *
 * A true return PROVES that `proof->leaf_hash` is committed under
 * `expected_root`: folding the leaf hash up through the recorded
 * siblings — choosing left/right at each level from the bits of
 * `proof->leaf_index` — reproduces one of the proof's peaks, AND bagging
 * those peaks (SHA3-256(0x02 || peak_0 || ... || peak_k), or the single
 * peak verbatim) equals `expected_root`. With SHA3-256 + domain
 * separation this is a cryptographic membership proof for a leaf that
 * was actually appended to the MMR producing `expected_root`.
 *
 * As with mmr_prove_from_leaves, the leaf's *meaning* is the caller's
 * responsibility: bind `proof->leaf_hash` to a trusted
 * mmr_hash_leaf(block_hash) (or mmr_hash_commitment) before relying on
 * it. Returns false (and logs) on a NULL proof, a non-matching peak, or
 * a root mismatch.
 *
 * Source: src/mmr.c (mmr_verify). */
bool mmr_verify(const struct mmr_proof *proof,
                const uint8_t expected_root[32]);

/* ── Unified commitment leaf ──────────────────────────── */
/* A commitment leaf binds block data + UTXO state + file chunks
 * at a given height. One leaf every COMMITMENT_INTERVAL blocks.
 * This is the minimum unit of trust for a new node:
 *   - block_hash:  proves PoW chain at this height
 *   - utxo_root:   proves UTXO set state (SHA3 over all UTXOs)
 *   - data_root:   proves raw file data (SHA3 over chunk hashes)
 *
 * A new node only needs: MMR root + latest commitment + delta blocks.
 * Energy cost: one SHA3 per commitment (microseconds). */

#define MMR_COMMITMENT_INTERVAL 100  /* ~20 minutes between commits */

struct mmr_commitment {
    int32_t  height;            /* block height of this commitment */
    uint8_t  block_hash[32];    /* block hash at height */
    uint8_t  utxo_root[32];     /* SHA3 of UTXO set at height */
    uint8_t  data_root[32];     /* SHA3 of file chunk hashes up to height */
};

/* Hash a commitment into an MMR leaf.
 * SHA3(0x00 || height || block_hash || utxo_root || data_root) */
void mmr_hash_commitment(const struct mmr_commitment *c, uint8_t out[32]);

/* Append a commitment to the MMR (convenience wrapper) */
int mmr_append_commitment(struct mmr *m, const struct mmr_commitment *c);

#endif
