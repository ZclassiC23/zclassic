/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * R1CS circuit gadgets for Sapling spend and output circuits.
 * Boolean constraints, field arithmetic, Pedersen hash, Blake2s,
 * Jubjub curve operations, and Merkle tree authentication paths. */

#ifndef ZCL_SAPLING_CIRCUIT_GADGETS_H
#define ZCL_SAPLING_CIRCUIT_GADGETS_H

#include "sapling/groth16_prover.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Boolean Gadgets ────────────────────────────────────────────── */

/* Constrain variable to be boolean: var * (1 - var) = 0 */
void gadget_boolean(struct constraint_system *cs, size_t var);

/* Allocate a boolean variable with value and constrain it */
size_t gadget_alloc_boolean(struct constraint_system *cs, bool value);

/* Allocate n boolean variables from a scalar (LSB first) */
void gadget_unpack_bits(struct constraint_system *cs,
                        size_t *bits_out, size_t n_bits,
                        const struct fr *value);

/* Pack boolean variables back into a field element: result = sum(bits[i] * 2^i) */
size_t gadget_pack_bits(struct constraint_system *cs,
                        const size_t *bits, size_t n_bits);

/* ── Field Arithmetic Gadgets ───────────────────────────────────── */

/* Constrain a * b = c (multiplication gate) */
void gadget_mul(struct constraint_system *cs, size_t a, size_t b, size_t c);

/* Allocate result = a * b and constrain */
size_t gadget_alloc_mul(struct constraint_system *cs, size_t a, size_t b);

/* Conditional select: result = condition ? a : b
 * Constraint: result = b + condition * (a - b) */
size_t gadget_select(struct constraint_system *cs,
                     size_t condition, size_t a, size_t b);

/* ── Pedersen Hash Gadget (in-circuit) ──────────────────────────── */

/* Pedersen hash of bits using Jubjub generators.
 * Input: boolean variables representing the hash input bits.
 * Output: (x, y) coordinates of the Pedersen hash point.
 * The hash processes 3-bit windows via Jubjub scalar multiplication
 * with pre-computed generators. */
void gadget_pedersen_hash(struct constraint_system *cs,
                          const size_t *input_bits, size_t n_bits,
                          const char *personalization,
                          size_t *x_out, size_t *y_out);

/* ── Blake2s Gadget (in-circuit) ────────────────────────────────── */

/* Blake2s hash with boolean input/output variables.
 * Used for PRF computations (nk derivation, nullifier, etc.)
 * Input: boolean variables (LSB first per byte).
 * Output: 256 boolean variables representing the hash output. */
void gadget_blake2s(struct constraint_system *cs,
                    const size_t *input_bits, size_t n_input_bits,
                    const uint8_t *personalization,
                    size_t *output_bits);

/* ── Jubjub Curve Gadgets ───────────────────────────────────────── */

/* Edwards curve point addition in-circuit.
 * Twisted Edwards: -x^2 + y^2 = 1 + d*x^2*y^2
 * Takes (x1,y1) and (x2,y2) variable indices, outputs (x3,y3). */
void gadget_edwards_add(struct constraint_system *cs,
                        size_t x1, size_t y1,
                        size_t x2, size_t y2,
                        size_t *x3, size_t *y3);

/* Fixed-base scalar multiplication with Jubjub generator.
 * scalar_bits: boolean variable indices for the scalar (LSB first).
 * base: the fixed base point (x, y) as constants.
 * Output: (x, y) coordinates of scalar * base. */
void gadget_fixed_base_mul(struct constraint_system *cs,
                           const size_t *scalar_bits, size_t n_bits,
                           const struct fr *base_x, const struct fr *base_y,
                           size_t *x_out, size_t *y_out);

/* ── Merkle Tree Authentication Path Gadget ─────────────────────── */

/* Verify a Merkle tree authentication path using Pedersen hash.
 * leaf: variable index for the leaf value.
 * path_bits: boolean variables for direction (left/right) at each level.
 * siblings: variable indices for sibling hashes at each level.
 * depth: tree depth (Sapling uses 32).
 * root_out: computed root variable index. */
size_t gadget_merkle_path(struct constraint_system *cs,
                          size_t leaf,
                          const size_t *path_bits,
                          const size_t *siblings,
                          size_t depth);

/* ── Note Commitment Gadget ─────────────────────────────────────── */

/* Sapling note commitment: cm = PedersenHash("Zcash_PH", g_d || pk_d || v || rcm)
 * where g_d and pk_d are Jubjub points, v is 64-bit value, rcm is randomness. */
size_t gadget_note_commitment(struct constraint_system *cs,
                              size_t *gd_bits, size_t n_gd_bits,
                              size_t *pkd_bits, size_t n_pkd_bits,
                              size_t *value_bits,
                              size_t *rcm_bits);

/* ── Nullifier Derivation Gadget ────────────────────────────────── */

/* Sapling nullifier: nf = MixingPedersenHash(nk, rho)
 * where nk is the nullifier deriving key and rho = cm + position */
void gadget_nullifier(struct constraint_system *cs,
                      size_t *nk_bits, size_t n_nk_bits,
                      size_t rho_x, size_t rho_y,
                      size_t *nf_x, size_t *nf_y);

/* ── Edwards Double ────────────────────────────────────────────── */
void gadget_edwards_double(struct constraint_system *cs,
                            size_t x1, size_t y1,
                            size_t *x3, size_t *y3);

/* ── Point On-Curve Check ──────────────────────────────────────── */
void gadget_point_interpret(struct constraint_system *cs, size_t x, size_t y);

/* ── Assert Not Small Order ────────────────────────────────────── */
void gadget_assert_not_small_order(struct constraint_system *cs,
                                     size_t x, size_t y);

/* ── Conditionally Select Point ────────────────────────────────── */
void gadget_conditionally_select_point(struct constraint_system *cs,
                                         size_t cond, size_t px, size_t py,
                                         size_t *rx, size_t *ry);

/* ── Variable-Base Scalar Multiplication ───────────────────────── */
void gadget_variable_base_mul(struct constraint_system *cs,
                                size_t base_x, size_t base_y,
                                const size_t *scalar_bits, size_t n_bits,
                                size_t *out_x, size_t *out_y);

/* ── Point Inputize ────────────────────────────────────────────── */
void gadget_point_inputize(struct constraint_system *cs, size_t x, size_t y);

/* ── Scalar Inputize ───────────────────────────────────────────── */
void gadget_scalar_inputize(struct constraint_system *cs, size_t var);

#endif
