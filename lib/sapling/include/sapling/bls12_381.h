/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BLS12-381 pairing — pure C23 implementation.
 * Fp (381-bit), Fp2, Fp6, Fp12, G1, G2, optimal Ate pairing. */

#ifndef ZCL_SAPLING_BLS12_381_H
#define ZCL_SAPLING_BLS12_381_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Fp: 381-bit prime field, 6 x 64-bit limbs, Montgomery form.
 * q = 0x1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab */
struct fp {
    uint64_t d[6];
};

void fp_zero(struct fp *r);
void fp_one(struct fp *r);
bool fp_is_zero(const struct fp *a);
bool fp_eq(const struct fp *a, const struct fp *b);

void fp_add(struct fp *r, const struct fp *a, const struct fp *b);
void fp_sub(struct fp *r, const struct fp *a, const struct fp *b);
void fp_neg(struct fp *r, const struct fp *a);
void fp_mul(struct fp *r, const struct fp *a, const struct fp *b);
void fp_sq(struct fp *r, const struct fp *a);
void fp_inv(struct fp *r, const struct fp *a);
bool fp_sqrt(struct fp *r, const struct fp *a);
bool fp_lexicographically_largest(const struct fp *a);

bool fp_from_bytes(struct fp *r, const uint8_t s[48]);
void fp_to_bytes(uint8_t s[48], const struct fp *a);

/* Fp2 = Fp[u] / (u^2 + 1) */
struct fp2 {
    struct fp c0, c1; /* c0 + c1 * u */
};

void fp2_zero(struct fp2 *r);
void fp2_one(struct fp2 *r);
bool fp2_is_zero(const struct fp2 *a);
bool fp2_eq(const struct fp2 *a, const struct fp2 *b);

void fp2_add(struct fp2 *r, const struct fp2 *a, const struct fp2 *b);
void fp2_sub(struct fp2 *r, const struct fp2 *a, const struct fp2 *b);
void fp2_neg(struct fp2 *r, const struct fp2 *a);
void fp2_mul(struct fp2 *r, const struct fp2 *a, const struct fp2 *b);
void fp2_sq(struct fp2 *r, const struct fp2 *a);
void fp2_inv(struct fp2 *r, const struct fp2 *a);
void fp2_mul_by_nonresidue(struct fp2 *r, const struct fp2 *a);
void fp2_frobenius_map(struct fp2 *a, int power);
bool fp2_sqrt(struct fp2 *r, const struct fp2 *a);
bool fp2_lexicographically_largest(const struct fp2 *a);

/* Fp6 = Fp2[v] / (v^3 - (u+1)) */
struct fp6 {
    struct fp2 c0, c1, c2; /* c0 + c1*v + c2*v^2 */
};

void fp6_zero(struct fp6 *r);
void fp6_one(struct fp6 *r);
bool fp6_is_zero(const struct fp6 *a);
void fp6_add(struct fp6 *r, const struct fp6 *a, const struct fp6 *b);
void fp6_sub(struct fp6 *r, const struct fp6 *a, const struct fp6 *b);
void fp6_neg(struct fp6 *r, const struct fp6 *a);
void fp6_mul(struct fp6 *r, const struct fp6 *a, const struct fp6 *b);
void fp6_sq(struct fp6 *r, const struct fp6 *a);
void fp6_inv(struct fp6 *r, const struct fp6 *a);
void fp6_mul_by_nonresidue(struct fp6 *r, const struct fp6 *a);
void fp6_mul_by_1(struct fp6 *r, const struct fp6 *a, const struct fp2 *c1);
void fp6_mul_by_01(struct fp6 *r, const struct fp6 *a, const struct fp2 *c0, const struct fp2 *c1);
void fp6_frobenius_map(struct fp6 *a, int power);

/* Fp12 = Fp6[w] / (w^2 - v) */
struct fp12 {
    struct fp6 c0, c1; /* c0 + c1*w */
};

void fp12_zero(struct fp12 *r);
void fp12_one(struct fp12 *r);
bool fp12_is_zero(const struct fp12 *a);
void fp12_add(struct fp12 *r, const struct fp12 *a, const struct fp12 *b);
void fp12_sub(struct fp12 *r, const struct fp12 *a, const struct fp12 *b);
void fp12_neg(struct fp12 *r, const struct fp12 *a);
void fp12_mul(struct fp12 *r, const struct fp12 *a, const struct fp12 *b);
void fp12_sq(struct fp12 *r, const struct fp12 *a);
void fp12_inv(struct fp12 *r, const struct fp12 *a);
void fp12_conjugate(struct fp12 *a);
void fp12_frobenius_map(struct fp12 *a, int power);
void fp12_mul_by_014(struct fp12 *a, const struct fp2 *c0, const struct fp2 *c1, const struct fp2 *c4);

/* G1: y^2 = x^3 + 4 over Fp (Jacobian coordinates) */
struct g1_point {
    struct fp x, y, z;
};

void g1_identity(struct g1_point *p);
bool g1_is_identity(const struct g1_point *p);
void g1_neg(struct g1_point *r, const struct g1_point *p);
void g1_add(struct g1_point *r, const struct g1_point *a, const struct g1_point *b);
void g1_double(struct g1_point *r, const struct g1_point *a);
bool g1_from_compressed(struct g1_point *p, const uint8_t in[48]);
bool g1_from_uncompressed(struct g1_point *p, const uint8_t in[96]);
void g1_to_affine(struct fp *ax, struct fp *ay, const struct g1_point *p);
void g1_scalar_mul(struct g1_point *r, const struct g1_point *p, const uint64_t scalar[4]);

/* Prime-order subgroup membership: true iff [r]P == O. G1/G2 have large
 * cofactors, so an on-curve point may carry a torsion component; the Groth16
 * verifier requires every proof point to lie in the prime-order-r subgroup
 * (a soundness requirement). The identity passes ([r]O == O). */
bool g1_in_subgroup(const struct g1_point *p);

/* G2: y^2 = x^3 + 4(u+1) over Fp2 (Jacobian coordinates) */
struct g2_point {
    struct fp2 x, y, z;
};

void g2_identity(struct g2_point *p);
bool g2_is_identity(const struct g2_point *p);
void g2_neg(struct g2_point *r, const struct g2_point *p);
void g2_add(struct g2_point *r, const struct g2_point *a, const struct g2_point *b);
void g2_double(struct g2_point *r, const struct g2_point *a);
bool g2_from_compressed(struct g2_point *p, const uint8_t in[96]);
bool g2_from_uncompressed(struct g2_point *p, const uint8_t in[192]);
void g2_to_affine(struct fp2 *ax, struct fp2 *ay, const struct g2_point *p);

/* G2 prime-order subgroup membership (see g1_in_subgroup above). */
bool g2_in_subgroup(const struct g2_point *p);

/* Optimal Ate pairing */
void bls12_381_pairing(struct fp12 *result,
                        const struct g1_point *p,
                        const struct g2_point *q);

/* Multi-pairing for Groth16 verification (more efficient) */
bool bls12_381_multi_pairing_check(
    const struct g1_point *a_pts, const struct g2_point *b_pts,
    size_t n_pairs);

/* Groth16 proof verification */
struct groth16_proof {
    struct g1_point a;
    struct g2_point b;
    struct g1_point c;
};

struct groth16_vk {
    struct g1_point alpha_g1;
    struct g2_point beta_g2;
    struct g2_point gamma_g2;
    struct g2_point delta_g2;
    struct g1_point *ic;
    size_t ic_len;
};

bool groth16_proof_read(struct groth16_proof *proof, const uint8_t data[192]);

/* Verify: e(A, B) == e(alpha, beta) * e(vk_x, gamma) * e(C, delta)
 * public_inputs: array of Fr scalars (4 x uint64_t each, little-endian limbs)
 * n_inputs: number of public inputs (must equal vk->ic_len - 1) */
bool groth16_verify(const struct groth16_vk *vk,
                    const struct groth16_proof *proof,
                    const uint64_t (*public_inputs)[4],
                    size_t n_inputs);

/* Pack bytes into Fr scalars (253 bits per scalar, little-endian bit ordering).
 * Use for Sapling nullifier packing. */
void multipack_bytes_to_fr(uint64_t (*out)[4], size_t *n_out,
                           const uint8_t *bytes, size_t n_bytes);

/* Pack bytes into Fr scalars (253 bits per scalar, big-endian bit ordering).
 * Use for Sprout public input packing. */
void multipack_bytes_to_fr_be(uint64_t (*out)[4], size_t *n_out,
                               const uint8_t *bytes, size_t n_bytes);

/* Read Groth16 verifying key from bellman Parameters file.
 * Reads the VK portion only (skips proving key).
 * Format: uncompressed G1 (96 bytes) and G2 (192 bytes) points.
 * Caller must free vk->ic when done. */
bool groth16_vk_read(struct groth16_vk *vk, const uint8_t *data, size_t len);

/* Read just the VerifyingKey from raw bytes (standalone VK, not wrapped in Parameters) */
bool groth16_vk_read_raw(struct groth16_vk *vk, const uint8_t *data, size_t len);

#endif
