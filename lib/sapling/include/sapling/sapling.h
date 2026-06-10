/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sapling key operations — pure C23 implementation.
 * group_hash, key derivation, commitment, nullifier. */

#ifndef ZCL_SAPLING_SAPLING_H
#define ZCL_SAPLING_SAPLING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sapling/fr.h"

/* Derive a Jubjub point via group_hash:
 * BLAKE2s-256(personalization, GH_FIRST_BLOCK || tag) → decompress → mul_by_cofactor.
 * Returns false if the result is the identity point (invalid). */
bool group_hash(struct jub_point *result,
                const uint8_t *tag, size_t tag_len,
                const uint8_t personalization[8]);

/* Check if a diversifier is valid (i.e., group_hash("Zcash_gd", d) is not identity) */
bool sapling_check_diversifier(const uint8_t diversifier[11]);

/* Compute g_d = GH("Zcash_gd", diversifier) */
bool sapling_diversifier_to_gd(struct jub_point *g_d, const uint8_t diversifier[11]);

/* ask → ak: ak = ask * SpendingKeyGenerator */
void sapling_ask_to_ak(const uint8_t ask[32], uint8_t ak[32]);

/* nsk → nk: nk = nsk * ProofGenerationKey */
void sapling_nsk_to_nk(const uint8_t nsk[32], uint8_t nk[32]);

/* rk = ak + ar * SpendAuthSig.G (re-randomized verification key) */
bool sapling_compute_rk(const uint8_t ak[32], const uint8_t ar[32],
                          uint8_t rk[32]);

/* CRH^ivk(ak, nk) = BLAKE2s("Zcashivk", ak || nk) with top 5 bits dropped */
void sapling_crh_ivk(const uint8_t ak[32], const uint8_t nk[32], uint8_t ivk[32]);

/* ivk → pk_d: pk_d = ivk * g_d(diversifier) */
bool sapling_ivk_to_pkd(const uint8_t ivk[32], const uint8_t diversifier[11],
                         uint8_t pk_d[32]);

/* Sapling key agreement: result = [sk] [8] p */
bool sapling_ka_agree(const uint8_t p[32], const uint8_t sk[32], uint8_t result[32]);

/* Derive ephemeral public key: result = [esk] g_d(diversifier) */
bool sapling_ka_derivepublic(const uint8_t diversifier[11], const uint8_t esk[32],
                              uint8_t result[32]);

/* Compute Sapling note commitment cm = x-coord of the Jubjub point
 *   WindowedPedersenHash(NoteCommitment, value(8 LE) || g_d || pk_d)
 *     + rcm · NoteCommitmentRandomness
 * where g_d = GH("Zcash_gd", diversifier). The output `cm[32]` is the
 * affine x-coordinate (an Fr element) — this is exactly the leaf that
 * gets appended to the note-commitment merkle tree. Returns false only
 * if `diversifier` is invalid (g_d would be the identity). `value` is in
 * zatoshi; `rcm` must be a valid Fs scalar. Same note contents bound here
 * are re-derived inside the output proof, so a mismatched cm fails the
 * Groth16 check in sapling_check_output. */
bool sapling_compute_cm(const uint8_t diversifier[11], const uint8_t pk_d[32],
                         uint64_t value, const uint8_t rcm[32],
                         uint8_t cm[32]);

/* Compute the Sapling nullifier that double-spend protection keys on:
 *   nf = BLAKE2s-256("Zcash_nf", nk || rho)
 * where rho = cm_full_point + position · NullifierPosition, and
 * cm_full_point is the *uncompressed* note-commitment point (the same
 * point whose x-coord is cm). `position` is the leaf's 0-based index in
 * the note-commitment tree — it is what binds the nullifier to where the
 * note sits in the tree, so an honest spend must pass the real merkle
 * position. `ak` is unused for nf (it gates only the viewing key); only
 * `nk` enters the hash. Returns false only if `diversifier` is invalid. */
bool sapling_compute_nf(const uint8_t diversifier[11], const uint8_t pk_d[32],
                         uint64_t value, const uint8_t rcm[32],
                         const uint8_t ak[32], const uint8_t nk[32],
                         uint64_t position, uint8_t nf[32]);

/* Generate a random Fs scalar (for commitment/note randomness).
 * Returns false on RNG failure (see crypto/random_secret.h); on success
 * `result` holds 32 bytes of a uniformly-sampled Fs element. Test paths
 * may discard the return value; production callers must propagate. */
bool sapling_generate_r(uint8_t result[32]);

/* RedJubjub signature verification (Zcash spec §5.4.7).
 * Returns true iff (vk, msg, sig) is a valid signature, asserting:
 *   - vk and R = sig_rbar deserialize to valid Jubjub points;
 *   - S = sig_sbar is canonical, i.e. S < Fs subgroup order (a
 *     non-canonical S is rejected to match zcashd and prevent
 *     signature malleability that could split consensus);
 *   - the cofactored equation [8]·(R + c·vk - S·G) == identity holds,
 *     where c = H*(Rbar || vk_bytes || msg) and G is the fixed generator.
 * msg/msg_len: message bytes (32 for sighash in spend_auth/binding).
 * generator_idx: 5 for SpendingKey (spend_auth_sig),
 *                4 for ValueCommitmentRandomness (binding_sig).
 * Returns false (and logs) on any malformed input or rejected signature. */
bool redjubjub_verify(const uint8_t vk_bytes[32],
                       const uint8_t *msg, size_t msg_len,
                       const uint8_t sig_rbar[32],
                       const uint8_t sig_sbar[32],
                       int generator_idx);

/* RedJubjub signing.
 * sk: secret key scalar (32 bytes)
 * msg/msg_len: message to sign
 * sig_out: output signature (64 bytes: rbar || sbar)
 * generator_idx: 5 for SpendAuth, 4 for ValueCommitment */
bool redjubjub_sign(const uint8_t sk[32],
                     const uint8_t *msg, size_t msg_len,
                     uint8_t sig_out[64],
                     int generator_idx);

/* Compute value commitment: cv = value * G_v + rcv * G_rcv
 * rcv: randomness scalar (32 bytes, must be a valid Fs)
 * cv_out: compressed Jubjub point (32 bytes) */
bool sapling_value_commit(uint64_t value, const uint8_t rcv[32],
                           uint8_t cv_out[32]);

/* Build a complete Sapling OutputDescription.
 * ovk: outgoing viewing key for sender recovery (32 bytes)
 * to_d, to_pk_d: recipient diversifier and pk_d
 * value: amount in zatoshi
 * memo: optional memo (512 bytes), NULL for default (0xF6 padding)
 * od_cv, od_cm, od_epk, od_enc, od_out, od_proof: output fields
 * rcv_out: if non-NULL, receives the rcv scalar (for binding sig)
 * Returns false on failure. */
bool sapling_build_output_description(
    const uint8_t ovk[32],
    const uint8_t to_d[11], const uint8_t to_pk_d[32],
    uint64_t value, const uint8_t memo[512],
    uint8_t od_cv[32], uint8_t od_cm[32], uint8_t od_epk[32],
    uint8_t od_enc[580], uint8_t od_out[80], uint8_t od_proof[192],
    uint8_t rcv_out[32]);

/* Build output description using Sapling proving context.
 * proving_ctx: opaque proving context from zclassic_sapling_proving_ctx_init
 * This produces cv and zkproof via native C23 prover (matching the reference circuit).
 * cm, epk, and encryption are computed by our C23 code. */
bool sapling_build_output_with_ctx(
    void *proving_ctx,
    const uint8_t ovk[32],
    const uint8_t to_d[11], const uint8_t to_pk_d[32],
    uint64_t value, const uint8_t memo[512],
    uint8_t od_cv[32], uint8_t od_cm[32], uint8_t od_epk[32],
    uint8_t od_enc[580], uint8_t od_out[80], uint8_t od_proof[192]);

/* Create binding signature for Sapling transaction.
 * bsk: total binding secret key (sum of rcv for spends, negated for outputs)
 * sighash: transaction sighash (32 bytes)
 * binding_sig_out: output signature (64 bytes) */
bool sapling_create_binding_sig(const uint8_t bsk[32],
                                 const uint8_t sighash[32],
                                 uint8_t binding_sig_out[64]);

/* Build a Sapling spend description using Sapling proving context.
 * Generates spend proof, cv, rk, nullifier.
 * Returns ar (randomness for spend_auth_sig) in ar_out for later signing. */
bool sapling_build_spend_with_ctx(
    void *proving_ctx,
    const uint8_t ask[32], const uint8_t nsk[32],
    const uint8_t diversifier[11], const uint8_t pk_d[32],
    const uint8_t rcm[32], uint64_t value, uint64_t position,
    const uint8_t anchor[32],
    const uint8_t *witness_path, size_t witness_len,
    uint8_t sd_cv[32], uint8_t sd_nullifier[32],
    uint8_t sd_rk[32], uint8_t sd_zkproof[192],
    uint8_t ar_out[32]);

/* Sapling verification context — accumulates the value-commitment balance
 * point `bvk` across every spend/output in ONE transaction. The full
 * bundle verification is a 3-phase sequence and the phases are stateful and
 * ORDER-DEPENDENT:
 *   1. init       — bvk := identity
 *   2. check_spend per spend  (adds  cv to bvk)
 *   3. check_output per output (subtracts cv from bvk)
 *   4. final_check — asserts bvk - value_balance·G_v opens the binding sig
 * The per-description checks below verify the proof/sig for that one
 * description but DO NOT establish transaction balance on their own —
 * only sapling_final_check closes that. Use one ctx per transaction;
 * sharing a ctx across transactions corrupts the balance accumulator. */
struct sapling_verification_ctx {
    struct jub_point bvk; /* accumulated value commitment balance */
};

void sapling_verification_ctx_init(struct sapling_verification_ctx *ctx);

/* Set the global Groth16 verifying keys. MUST be called once at startup
 * (sapling_init_params) before any check_spend/check_output. The check
 * functions fail closed — a NULL VK makes them reject every proof rather
 * than silently accept it — so forgetting this is loud, not silent. */
struct groth16_vk;
void sapling_set_spend_vk(struct groth16_vk *vk);
void sapling_set_output_vk(struct groth16_vk *vk);

/* Verify one Sapling SpendDescription and fold it into the bundle balance.
 * On success (`true`) this asserts ALL of:
 *   - cv and rk deserialize to valid Jubjub points and are NOT small-order
 *     (rejects the cofactor-subgroup malleability attack);
 *   - spend_auth_sig is a valid RedJubjub signature over `sighash` under
 *     the re-randomized key rk (proves authority to spend without
 *     revealing ak);
 *   - the Groth16 spend proof verifies against the 7 public inputs
 *     {rk.x, rk.y, cv.x, cv.y, anchor, nullifier_packed[0..1]} — i.e. the
 *     note is committed under `anchor` (a past note-commitment tree root),
 *     its value matches cv, and `nullifier` is the correct nf for it.
 * SIDE EFFECT: on success cv is ADDED to ctx->bvk. NOTE: this does NOT
 * check that `anchor` is a recognized historical root, nor that
 * `nullifier` is unspent — those are the caller's (chainstate) job.
 * Returns false (and logs) on the first failed check. */
bool sapling_check_spend(struct sapling_verification_ctx *ctx,
                          const uint8_t cv[32],
                          const uint8_t anchor[32],
                          const uint8_t nullifier[32],
                          const uint8_t rk[32],
                          const uint8_t zkproof[192],
                          const uint8_t spend_auth_sig[64],
                          const uint8_t sighash[32]);

/* Verify one Sapling OutputDescription and fold it into the bundle balance.
 * On success (`true`) this asserts ALL of:
 *   - cv and epk deserialize to valid Jubjub points and are NOT small-order;
 *   - the Groth16 output proof verifies against the 5 public inputs
 *     {cv.x, cv.y, epk.x, epk.y, cm} — i.e. cv commits to the same value
 *     the note plaintext encodes and `cm` is the correct note commitment
 *     for epk's diversifier. (The new note's `cm` is what the caller
 *     appends to the commitment tree.)
 * SIDE EFFECT: on success cv is SUBTRACTED from ctx->bvk. Returns false
 * (and logs) on the first failed check. */
bool sapling_check_output(struct sapling_verification_ctx *ctx,
                           const uint8_t cv[32],
                           const uint8_t cm[32],
                           const uint8_t epk[32],
                           const uint8_t zkproof[192]);

/* Close the bundle: assert that the accumulated value commitments balance
 * to the declared `value_balance` (net zatoshi moving in/out of the shielded
 * pool). Computes final_bvk = ctx->bvk - value_balance·ValueCommitmentValue
 * and verifies `binding_sig` is a valid RedJubjub signature over `sighash`
 * under final_bvk as the verification key. This succeeds iff the prover knew
 * bsk = sum(rcv_spends) - sum(rcv_outputs), which is only possible when the
 * value commitments actually sum to value_balance — i.e. no value was minted
 * or burned. value_balance == INT64_MIN is rejected (matches Rust
 * checked_abs). MUST be called only after every check_spend/check_output for
 * the transaction has run; returns false (and logs) on rejection. */
bool sapling_final_check(struct sapling_verification_ctx *ctx,
                          int64_t value_balance,
                          const uint8_t binding_sig[64],
                          const uint8_t sighash[32]);

#endif
