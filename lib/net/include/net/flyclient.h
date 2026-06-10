/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * FlyClient — probabilistic chain verification for fast sync.
 * A fresh ZCL23 node can verify 3M+ blocks of PoW by sampling
 * O(log n) random headers and checking their MMB inclusion proofs.
 *
 * This is a ZCL23 overlay protocol — no consensus changes needed.
 * Legacy zclassicd nodes never see FlyClient messages.
 *
 * Protocol:
 *   Node2 (fresh)              Node1 (at tip)
 *        ← zsnapshot (height, utxo_root, mmb_root)
 *        → zfcchallenge (random_seed)
 *        ← zfcproofs (N headers + MMB proofs)
 *        [verify each: PoW valid? proof valid?]
 *        → zsnapreq — if all verified
 */

#ifndef ZCL_NET_FLYCLIENT_H
#define ZCL_NET_FLYCLIENT_H

#include "chain/mmb.h"
#include <stdint.h>
#include <stdbool.h>

/* Security parameter: number of random samples.
 * Under the FlyClient weighted distribution, an adversary with <50%
 * hashpower must forge PoW for each sampled block independently.
 * With k samples and difficulty d, forgery probability ≤ (1/d)^k.
 * ZClassic minimum difficulty ≈ 2^-13; 50 samples gives security:
 *   50 * 13 = 650 bits against full-difficulty chain
 *   50 * 3  = 150 bits against adversary with 1/8 hashpower
 * This exceeds the 150-bit target for all realistic adversaries. */
#define FC_NUM_SAMPLES  50
#define FC_MAX_SAMPLES  64

/* P2P message names (≤12 chars for ZCL P2P protocol) */
#define MSG_FC_CHALLENGE  "zfcchallenge"
#define MSG_FC_PROOFS     "zfcproofs"

/* ── Challenge (verifier → prover) ──────────────────────── */

struct fc_challenge {
    uint8_t  seed[32];          /* random challenge nonce */
    uint64_t chain_length;      /* claimed number of blocks */
    uint8_t  mmb_root[32];      /* claimed MMB root to verify */
};

/* ── Sample (one header + MMB proof) ────────────────────── */

struct fc_sample {
    struct mmb_leaf  leaf;      /* rich block header data */
    struct mmb_proof proof;     /* MMB inclusion proof */
};

/* ── Response (prover → verifier) ───────────────────────── */

struct fc_response {
    struct fc_sample samples[FC_MAX_SAMPLES];
    uint32_t num_samples;
};

/* Generate deterministic random sample indices from a shared seed.
 * Uses FlyClient weighted distribution: biased toward recent blocks.
 * Output: indices[0..num_indices-1], each in [0, chain_length). */
void fc_generate_indices(const uint8_t seed[32], uint64_t chain_length,
                         uint64_t *indices, uint32_t *num_indices);

/* Verify a single sample: check MMB proof against root.
 * PoW verification (Equihash) is done separately by the caller
 * since it requires the full block header with solution. */
bool fc_verify_sample(const struct fc_sample *sample,
                      const uint8_t mmb_root[32]);

/* Verify complete FlyClient response: all samples valid? */
bool fc_verify_response(const struct fc_response *resp,
                        const struct fc_challenge *challenge);

/* Build response (prover side): given the challenge, generate
 * sample indices and build MMB proofs for each.
 * Requires all_leaf_hashes from the MMB (pre-computed).
 * Returns false if any proof generation fails. */
bool fc_build_response(const struct fc_challenge *challenge,
                       const struct mmb *mmb,
                       const struct mmb_leaf *all_leaves,
                       const uint8_t (*all_leaf_hashes)[32],
                       struct fc_response *resp);

#endif
