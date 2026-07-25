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

/* ── Strict bit decomposition / point representation ────────────── */

/* Number of bits a strict decomposition of a BLS12-381 Fr element emits:
 * one per bit position of (r - 1) at or below its most significant set bit. */
#define FR_STRICT_BITS 255

/* bellman AllocatedNum::into_bits_le_strict. Decomposes the value on wire
 * `var` into FR_STRICT_BITS boolean wires, LITTLE-endian (index 0 = LSB), and
 * constrains the decomposition to be the in-field representation — a
 * congruency >= r is rejected. 388 constraints / 387 aux per call; the count
 * is emergent from the bit pattern of r-1, not a tunable. */
void gadget_into_bits_le_strict(struct constraint_system *cs, size_t var,
                                size_t bits_out[FR_STRICT_BITS]);

/* bellman ecc::EdwardsPoint::repr. Unpacks x then y (order is load-bearing)
 * and writes 256 bits: y's 255 little-endian bits followed by x's sign bit
 * x[0]. 776 constraints. */
void gadget_point_repr(struct constraint_system *cs,
                       size_t x_var, size_t y_var, size_t bits_out[256]);

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
