/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * R1CS circuit gadgets matching Zcash bellman/sapling-crypto exactly.
 * Edwards add (6 constraints), double (5), windowed fixed-base mul,
 * Montgomery-based Pedersen hash, strict bit decomposition. */

#include "sapling/circuit_gadgets.h"
#include "sapling/pedersen_hash.h"
#include "sapling/sapling.h"
#include "crypto/blake2s.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "util/safe_alloc.h"
#include "support/cleanse.h"

#define CS_ONE 0

/* ── Boolean Gadgets ────────────────────────────────────────────── */

void gadget_boolean(struct constraint_system *cs, size_t var)
{
    struct linear_combination a, b, c;
    struct fr one_val;
    fr_one(&one_val);
    struct fr neg_one;
    fr_neg(&neg_one, &one_val);

    lc_init(&a);
    lc_add_term(&a, CS_ONE, &one_val);
    lc_add_term(&a, var, &neg_one);

    lc_init(&b);
    lc_add_term(&b, var, &one_val);

    lc_init(&c);

    cs_enforce(cs, &a, &b, &c);
    lc_free(&a);
    lc_free(&b);
    lc_free(&c);
}

size_t gadget_alloc_boolean(struct constraint_system *cs, bool value)
{
    struct fr val;
    if (value) fr_one(&val); else fr_zero(&val);
    size_t var = cs_alloc_aux(cs, &val);
    gadget_boolean(cs, var);
    return var;
}

void gadget_unpack_bits(struct constraint_system *cs,
                        size_t *bits_out, size_t n_bits,
                        const struct fr *value)
{
    uint8_t bytes[32];
    fr_to_bytes(bytes, value);

    for (size_t i = 0; i < n_bits; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = i % 8;
        bool bit = byte_idx < 32 && ((bytes[byte_idx] >> bit_idx) & 1);
        bits_out[i] = gadget_alloc_boolean(cs, bit);
    }

    size_t value_var = cs_alloc_aux(cs, value);
    struct linear_combination a, b, c;
    lc_init(&a);
    struct fr coeff;
    fr_one(&coeff);
    for (size_t i = 0; i < n_bits; i++) {
        lc_add_term(&a, bits_out[i], &coeff);
        fr_add(&coeff, &coeff, &coeff);
    }
    struct fr one_val;
    fr_one(&one_val);
    lc_init(&b);
    lc_add_term(&b, CS_ONE, &one_val);
    lc_init(&c);
    lc_add_term(&c, value_var, &one_val);
    cs_enforce(cs, &a, &b, &c);
    lc_free(&a);
    lc_free(&b);
    lc_free(&c);
}

size_t gadget_pack_bits(struct constraint_system *cs,
                        const size_t *bits, size_t n_bits)
{
    struct fr packed;
    fr_zero(&packed);
    struct fr coeff;
    fr_one(&coeff);
    for (size_t i = 0; i < n_bits; i++) {
        struct fr bit_val = cs->witness[bits[i]];
        struct fr term;
        fr_mul(&term, &bit_val, &coeff);
        fr_add(&packed, &packed, &term);
        fr_add(&coeff, &coeff, &coeff);
    }
    size_t result = cs_alloc_aux(cs, &packed);
    struct linear_combination a, b, c;
    lc_init(&a);
    fr_one(&coeff);
    for (size_t i = 0; i < n_bits; i++) {
        lc_add_term(&a, bits[i], &coeff);
        fr_add(&coeff, &coeff, &coeff);
    }
    struct fr one_val;
    fr_one(&one_val);
    lc_init(&b);
    lc_add_term(&b, CS_ONE, &one_val);
    lc_init(&c);
    lc_add_term(&c, result, &one_val);
    cs_enforce(cs, &a, &b, &c);
    lc_free(&a);
    lc_free(&b);
    lc_free(&c);
    return result;
}

/* ── Field Arithmetic Gadgets ───────────────────────────────────── */

void gadget_mul(struct constraint_system *cs, size_t a, size_t b, size_t c)
{
    struct linear_combination la, lb, lc;
    struct fr one_val;
    fr_one(&one_val);
    lc_init(&la); lc_add_term(&la, a, &one_val);
    lc_init(&lb); lc_add_term(&lb, b, &one_val);
    lc_init(&lc); lc_add_term(&lc, c, &one_val);
    cs_enforce(cs, &la, &lb, &lc);
    lc_free(&la); lc_free(&lb); lc_free(&lc);
}

size_t gadget_alloc_mul(struct constraint_system *cs, size_t a, size_t b)
{
    struct fr product;
    fr_mul(&product, &cs->witness[a], &cs->witness[b]);
    size_t c = cs_alloc_aux(cs, &product);
    gadget_mul(cs, a, b, c);
    return c;
}

size_t gadget_select(struct constraint_system *cs,
                     size_t condition, size_t a, size_t b)
{
    struct fr cond_val = cs->witness[condition];
    struct fr a_val = cs->witness[a];
    struct fr b_val = cs->witness[b];
    struct fr diff;
    fr_sub(&diff, &a_val, &b_val);
    struct fr selected;
    fr_mul(&selected, &cond_val, &diff);
    fr_add(&selected, &selected, &b_val);
    size_t result = cs_alloc_aux(cs, &selected);

    struct linear_combination la, lb, lc;
    struct fr one_val, neg_one;
    fr_one(&one_val);
    fr_neg(&neg_one, &one_val);
    lc_init(&la); lc_add_term(&la, condition, &one_val);
    lc_init(&lb); lc_add_term(&lb, a, &one_val); lc_add_term(&lb, b, &neg_one);
    lc_init(&lc); lc_add_term(&lc, result, &one_val); lc_add_term(&lc, b, &neg_one);
    cs_enforce(cs, &la, &lb, &lc);
    lc_free(&la); lc_free(&lb); lc_free(&lc);
    return result;
}

/* ── Jubjub d constant ─────────────────────────────────────────── */

static void jubjub_d(struct fr *d)
{
    static const uint8_t D_BYTES[32] = {
        0xb1,0x3e,0x34,0xd6,0xd6,0x5f,0x06,0x01,
        0x26,0x9d,0x57,0x37,0x6d,0x7f,0x2d,0x29,
        0xd4,0x7f,0xbd,0xe6,0x07,0x92,0xfd,0xf5,
        0x48,0x2b,0xfa,0x4b,0xe7,0x18,0x93,0x2a
    };
    fr_from_bytes(d, D_BYTES);
}

/* ── Edwards Addition (6 constraints) ──────────────────────────── */
/* Zcash bellman formula:
 * U = (x1+y1)*(x2+y2)  — 1 constraint
 * A = y2*x1             — 1 constraint
 * B = x2*y1             — 1 constraint
 * C = d*A*B             — 1 constraint: (d*A)*B = C
 * (1+C)*x3 = A+B        — 1 constraint
 * (1-C)*y3 = U-A-B      — 1 constraint
 * Total: 6 constraints */

void gadget_edwards_add(struct constraint_system *cs,
                        size_t x1, size_t y1,
                        size_t x2, size_t y2,
                        size_t *x3, size_t *y3)
{
    struct fr x1v = cs->witness[x1], y1v = cs->witness[y1];
    struct fr x2v = cs->witness[x2], y2v = cs->witness[y2];
    struct fr one_val, neg_one;
    fr_one(&one_val);
    fr_neg(&neg_one, &one_val);
    struct fr d_val;
    jubjub_d(&d_val);

    /* U = (x1+y1)*(x2+y2) */
    struct fr u_val;
    {
        struct fr s1, s2;
        fr_add(&s1, &x1v, &y1v);
        fr_add(&s2, &x2v, &y2v);
        fr_mul(&u_val, &s1, &s2);
    }
    size_t u = cs_alloc_aux(cs, &u_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, x1, &one_val); lc_add_term(&la, y1, &one_val);
        lc_init(&lb); lc_add_term(&lb, x2, &one_val); lc_add_term(&lb, y2, &one_val);
        lc_init(&lc); lc_add_term(&lc, u, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* A = y2*x1 */
    struct fr a_val;
    fr_mul(&a_val, &y2v, &x1v);
    size_t a = cs_alloc_aux(cs, &a_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, y2, &one_val);
        lc_init(&lb); lc_add_term(&lb, x1, &one_val);
        lc_init(&lc); lc_add_term(&lc, a, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* B = x2*y1 */
    struct fr b_val;
    fr_mul(&b_val, &x2v, &y1v);
    size_t b = cs_alloc_aux(cs, &b_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, x2, &one_val);
        lc_init(&lb); lc_add_term(&lb, y1, &one_val);
        lc_init(&lc); lc_add_term(&lc, b, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* C = d*A*B: constrained as (d*A)*B = C */
    struct fr c_val;
    {
        struct fr da;
        fr_mul(&da, &d_val, &a_val);
        fr_mul(&c_val, &da, &b_val);
    }
    size_t c = cs_alloc_aux(cs, &c_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, a, &d_val);
        lc_init(&lb); lc_add_term(&lb, b, &one_val);
        lc_init(&lc); lc_add_term(&lc, c, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* x3 = (A+B) / (1+C) → (1+C)*x3 = A+B */
    struct fr x3v;
    {
        struct fr num, denom;
        fr_add(&num, &a_val, &b_val);
        fr_add(&denom, &one_val, &c_val);
        fr_inv(&x3v, &denom);
        fr_mul(&x3v, &x3v, &num);
    }
    *x3 = cs_alloc_aux(cs, &x3v);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, CS_ONE, &one_val); lc_add_term(&la, c, &one_val);
        lc_init(&lb); lc_add_term(&lb, *x3, &one_val);
        lc_init(&lc); lc_add_term(&lc, a, &one_val); lc_add_term(&lc, b, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* y3 = (U-A-B) / (1-C) → (1-C)*y3 = U-A-B */
    struct fr y3v;
    {
        struct fr num, denom;
        fr_sub(&num, &u_val, &a_val);
        fr_sub(&num, &num, &b_val);
        fr_sub(&denom, &one_val, &c_val);
        fr_inv(&y3v, &denom);
        fr_mul(&y3v, &y3v, &num);
    }
    *y3 = cs_alloc_aux(cs, &y3v);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, CS_ONE, &one_val); lc_add_term(&la, c, &neg_one);
        lc_init(&lb); lc_add_term(&lb, *y3, &one_val);
        lc_init(&lc); lc_add_term(&lc, u, &one_val); lc_add_term(&lc, a, &neg_one); lc_add_term(&lc, b, &neg_one);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
}

/* ── Edwards Doubling (5 constraints) ──────────────────────────── */
/* Zcash bellman: T=(x+y)^2, A=x*y, C=d*A^2, (1+C)*x3=2A, (1-C)*y3=T-2A */

void gadget_edwards_double(struct constraint_system *cs,
                            size_t x1, size_t y1,
                            size_t *x3, size_t *y3)
{
    struct fr xv = cs->witness[x1], yv = cs->witness[y1];
    struct fr one_val, neg_one;
    fr_one(&one_val);
    fr_neg(&neg_one, &one_val);
    struct fr d_val;
    jubjub_d(&d_val);
    struct fr two;
    fr_add(&two, &one_val, &one_val);

    /* T = (x+y)^2 */
    struct fr t_val;
    {
        struct fr s;
        fr_add(&s, &xv, &yv);
        fr_mul(&t_val, &s, &s);
    }
    size_t t = cs_alloc_aux(cs, &t_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, x1, &one_val); lc_add_term(&la, y1, &one_val);
        lc_init(&lb); lc_add_term(&lb, x1, &one_val); lc_add_term(&lb, y1, &one_val);
        lc_init(&lc); lc_add_term(&lc, t, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* A = x*y */
    struct fr a_val;
    fr_mul(&a_val, &xv, &yv);
    size_t a = cs_alloc_aux(cs, &a_val);
    gadget_mul(cs, x1, y1, a);

    /* C = d*A^2: (d*A)*A = C */
    struct fr c_val;
    {
        struct fr da;
        fr_mul(&da, &d_val, &a_val);
        fr_mul(&c_val, &da, &a_val);
    }
    size_t c = cs_alloc_aux(cs, &c_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, a, &d_val);
        lc_init(&lb); lc_add_term(&lb, a, &one_val);
        lc_init(&lc); lc_add_term(&lc, c, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* (1+C)*x3 = 2*A */
    struct fr x3v;
    {
        struct fr num, denom;
        fr_mul(&num, &two, &a_val);
        fr_add(&denom, &one_val, &c_val);
        fr_inv(&x3v, &denom);
        fr_mul(&x3v, &x3v, &num);
    }
    *x3 = cs_alloc_aux(cs, &x3v);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, CS_ONE, &one_val); lc_add_term(&la, c, &one_val);
        lc_init(&lb); lc_add_term(&lb, *x3, &one_val);
        lc_init(&lc); lc_add_term(&lc, a, &two);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* (1-C)*y3 = T-2A */
    struct fr y3v;
    {
        struct fr num, denom;
        fr_mul(&num, &two, &a_val);
        fr_sub(&num, &t_val, &num);
        fr_sub(&denom, &one_val, &c_val);
        fr_inv(&y3v, &denom);
        fr_mul(&y3v, &y3v, &num);
    }
    *y3 = cs_alloc_aux(cs, &y3v);
    {
        struct linear_combination la, lb, lc;
        struct fr neg_two;
        fr_neg(&neg_two, &two);
        lc_init(&la); lc_add_term(&la, CS_ONE, &one_val); lc_add_term(&la, c, &neg_one);
        lc_init(&lb); lc_add_term(&lb, *y3, &one_val);
        lc_init(&lc); lc_add_term(&lc, t, &one_val); lc_add_term(&lc, a, &neg_two);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
}

/* ── 3-bit Lookup Table (synth interpolation) ──────────────────── */

/* Compute interpolation coefficients for 2^window_size values.
 * Given constants[0..2^w-1], computes assignment[0..2^w-1] such that:
 * constants[index] = sum over j (assignment[j] for j where j is subset of index bits)
 * This is the Möbius/subset-sum interpolation used by bellman. */
static void synth_coeffs(size_t window_size, const struct fr *constants,
                          struct fr *assignment)
{
    size_t n = (size_t)1 << window_size;
    for (size_t i = 0; i < n; i++)
        fr_zero(&assignment[i]);

    for (size_t i = 0; i < n; i++) {
        struct fr cur = assignment[i];
        fr_neg(&cur, &cur);
        fr_add(&cur, &cur, &constants[i]);
        assignment[i] = cur;
        for (size_t j = i + 1; j < n; j++) {
            if ((j & i) == i) {
                fr_add(&assignment[j], &assignment[j], &cur);
            }
        }
    }
}

/* lookup3_xy: 3-bit window table lookup for fixed-base mul.
 * bits = [b0, b1, b2], coords = 8 (x,y) pairs.
 * 3 constraints: 1 AND(b1,b2) + 1 x-enforce + 1 y-enforce. */
void gadget_lookup3_xy(struct constraint_system *cs,
                        size_t b0, size_t b1, size_t b2,
                        const struct fr coords_x[8],
                        const struct fr coords_y[8],
                        size_t *rx, size_t *ry)
{
    struct fr one_val;
    fr_one(&one_val);
    struct fr neg_one;
    fr_neg(&neg_one, &one_val);

    /* Compute precomp = b1 AND b2 (1 constraint) */
    struct fr b1v = cs->witness[b1];
    struct fr b2v = cs->witness[b2];
    struct fr b0v = cs->witness[b0];
    struct fr precomp_val;
    fr_mul(&precomp_val, &b1v, &b2v);
    size_t precomp = cs_alloc_aux(cs, &precomp_val);
    gadget_mul(cs, b1, b2, precomp);

    /* Compute synth coefficients */
    struct fr xc[8], yc[8];
    synth_coeffs(3, coords_x, xc);
    synth_coeffs(3, coords_y, yc);

    /* Determine index and allocate result */
    int idx = 0;
    if (!fr_is_zero(&b0v)) idx |= 1;
    if (!fr_is_zero(&b1v)) idx |= 2;
    if (!fr_is_zero(&b2v)) idx |= 4;

    *rx = cs_alloc_aux(cs, &coords_x[idx]);
    *ry = cs_alloc_aux(cs, &coords_y[idx]);

    /* x-coordinate constraint (1 constraint):
     * (xc[001] + b1*xc[011] + b2*xc[101] + precomp*xc[111]) * b0
     *   = rx - xc[000] - b1*xc[010] - b2*xc[100] - precomp*xc[110] */
    {
        struct linear_combination la, lb, lc;
        lc_init(&la);
        lc_add_term(&la, CS_ONE, &xc[0b001]);
        lc_add_term(&la, b1, &xc[0b011]);
        lc_add_term(&la, b2, &xc[0b101]);
        lc_add_term(&la, precomp, &xc[0b111]);

        lc_init(&lb);
        lc_add_term(&lb, b0, &one_val);

        lc_init(&lc);
        lc_add_term(&lc, *rx, &one_val);
        struct fr neg_xc0; fr_neg(&neg_xc0, &xc[0b000]);
        lc_add_term(&lc, CS_ONE, &neg_xc0);
        struct fr neg_xc2; fr_neg(&neg_xc2, &xc[0b010]);
        lc_add_term(&lc, b1, &neg_xc2);
        struct fr neg_xc4; fr_neg(&neg_xc4, &xc[0b100]);
        lc_add_term(&lc, b2, &neg_xc4);
        struct fr neg_xc6; fr_neg(&neg_xc6, &xc[0b110]);
        lc_add_term(&lc, precomp, &neg_xc6);

        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* y-coordinate constraint (1 constraint): same structure */
    {
        struct linear_combination la, lb, lc;
        lc_init(&la);
        lc_add_term(&la, CS_ONE, &yc[0b001]);
        lc_add_term(&la, b1, &yc[0b011]);
        lc_add_term(&la, b2, &yc[0b101]);
        lc_add_term(&la, precomp, &yc[0b111]);

        lc_init(&lb);
        lc_add_term(&lb, b0, &one_val);

        lc_init(&lc);
        lc_add_term(&lc, *ry, &one_val);
        struct fr neg_yc0; fr_neg(&neg_yc0, &yc[0b000]);
        lc_add_term(&lc, CS_ONE, &neg_yc0);
        struct fr neg_yc2; fr_neg(&neg_yc2, &yc[0b010]);
        lc_add_term(&lc, b1, &neg_yc2);
        struct fr neg_yc4; fr_neg(&neg_yc4, &yc[0b100]);
        lc_add_term(&lc, b2, &neg_yc4);
        struct fr neg_yc6; fr_neg(&neg_yc6, &yc[0b110]);
        lc_add_term(&lc, precomp, &neg_yc6);

        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
}

/* ── Fixed-Base Scalar Multiplication (windowed) ───────────────── */

void gadget_fixed_base_mul(struct constraint_system *cs,
                           const size_t *scalar_bits, size_t n_bits,
                           const struct fr *base_x, const struct fr *base_y,
                           size_t *x_out, size_t *y_out)
{
    /* Reconstruct the Jubjub base point */
    struct jub_point gen;
    {
        uint8_t bx[32], by[32];
        fr_to_bytes(bx, base_x);
        fr_to_bytes(by, base_y);
        /* Build point from x,y coordinates */
        struct fr gx, gy, gz, gt;
        gx = *base_x;
        gy = *base_y;
        fr_one(&gz);
        fr_mul(&gt, &gx, &gy);
        gen.x = gx; gen.y = gy; gen.z = gz; gen.t = gt;
    }

    size_t acc_x = SIZE_MAX, acc_y = SIZE_MAX;
    size_t n_windows = (n_bits + 2) / 3;

    struct jub_point cur_gen = gen;

    for (size_t w = 0; w < n_windows; w++) {
        /* Get the 3 bit indices (pad with dummy zero-valued boolean if needed) */
        size_t b0 = (w * 3 + 0 < n_bits) ? scalar_bits[w * 3 + 0] : SIZE_MAX;
        size_t b1 = (w * 3 + 1 < n_bits) ? scalar_bits[w * 3 + 1] : SIZE_MAX;
        size_t b2 = (w * 3 + 2 < n_bits) ? scalar_bits[w * 3 + 2] : SIZE_MAX;

        /* Precompute 8 multiples: 0*G, 1*G, 2*G, ..., 7*G */
        struct fr coords_x[8], coords_y[8];
        /* 0*G = identity (0, 1) */
        fr_zero(&coords_x[0]);
        fr_one(&coords_y[0]);
        /* 1*G through 7*G */
        struct jub_point pt = cur_gen;
        for (int k = 1; k < 8; k++) {
            jub_get_x(&coords_x[k], &pt);
            jub_get_y(&coords_y[k], &pt);
            if (k < 7) {
                struct jub_point next;
                jub_add(&next, &pt, &cur_gen);
                pt = next;
            }
        }

        /* Handle padding with constant false booleans */
        bool b0_const = (w * 3 + 0 >= n_bits);
        bool b1_const = (w * 3 + 1 >= n_bits);
        bool b2_const = (w * 3 + 2 >= n_bits);

        if (b0_const || b1_const || b2_const) {
            /* Some bits are constant false — use simplified lookup */
            struct fr zero_fr;
            fr_zero(&zero_fr);
            if (b0_const) b0 = 0; /* will use constant zero */
            if (b1_const) b1 = 0;
            if (b2_const) b2 = 0;

            /* For constant bits, we compute the index directly */
            bool b0v = !b0_const && !fr_is_zero(&cs->witness[b0]);
            bool b1v = !b1_const && !fr_is_zero(&cs->witness[b1]);
            bool b2v = !b2_const && !fr_is_zero(&cs->witness[b2]);
            int idx = (b0v ? 1 : 0) | (b1v ? 2 : 0) | (b2v ? 4 : 0);

            size_t wx = cs_alloc_aux(cs, &coords_x[idx]);
            size_t wy = cs_alloc_aux(cs, &coords_y[idx]);

            /* Simplified constraints based on which bits are constant */
            if (!b0_const && b1_const && b2_const) {
                /* Only b0 is variable. x = x0 + b0*(x1-x0), y = y0 + b0*(y1-y0) */
                /* x constraint: b0 * (x1-x0) = wx - x0 */
                struct fr one_val; fr_one(&one_val);
                struct linear_combination la, lb, lc;
                struct fr dx; fr_sub(&dx, &coords_x[1], &coords_x[0]);
                lc_init(&la); lc_add_term(&la, b0, &one_val);
                lc_init(&lb); lc_add_term(&lb, CS_ONE, &dx);
                struct fr neg_x0; fr_neg(&neg_x0, &coords_x[0]);
                lc_init(&lc); lc_add_term(&lc, wx, &one_val); lc_add_term(&lc, CS_ONE, &neg_x0);
                cs_enforce(cs, &la, &lb, &lc);
                lc_free(&la); lc_free(&lb); lc_free(&lc);

                struct fr dy; fr_sub(&dy, &coords_y[1], &coords_y[0]);
                lc_init(&la); lc_add_term(&la, b0, &one_val);
                lc_init(&lb); lc_add_term(&lb, CS_ONE, &dy);
                struct fr neg_y0; fr_neg(&neg_y0, &coords_y[0]);
                lc_init(&lc); lc_add_term(&lc, wy, &one_val); lc_add_term(&lc, CS_ONE, &neg_y0);
                cs_enforce(cs, &la, &lb, &lc);
                lc_free(&la); lc_free(&lb); lc_free(&lc);
            } else {
                /* Fallback: use full lookup even for partially-constant case */
                /* This costs 3 constraints but handles all cases */
                if (b0_const) b0 = cs_alloc_aux(cs, &zero_fr);
                if (b1_const) b1 = cs_alloc_aux(cs, &zero_fr);
                if (b2_const) b2 = cs_alloc_aux(cs, &zero_fr);
                /* Redo the lookup with the allocated variables */
                size_t tmp_x, tmp_y;
                gadget_lookup3_xy(cs, b0, b1, b2, coords_x, coords_y, &tmp_x, &tmp_y);
                wx = tmp_x;
                wy = tmp_y;
            }

            if (acc_x == SIZE_MAX) {
                acc_x = wx;
                acc_y = wy;
            } else {
                size_t new_x, new_y;
                gadget_edwards_add(cs, acc_x, acc_y, wx, wy, &new_x, &new_y);
                acc_x = new_x;
                acc_y = new_y;
            }
        } else {
            /* Normal case: all 3 bits are allocated variables */
            size_t wx, wy;
            gadget_lookup3_xy(cs, b0, b1, b2, coords_x, coords_y, &wx, &wy);

            if (acc_x == SIZE_MAX) {
                acc_x = wx;
                acc_y = wy;
            } else {
                size_t new_x, new_y;
                gadget_edwards_add(cs, acc_x, acc_y, wx, wy, &new_x, &new_y);
                acc_x = new_x;
                acc_y = new_y;
            }
        }

        /* Advance generator: cur_gen *= 2^3 = 8 */
        if (w + 1 < n_windows) {
            struct jub_point tmp;
            jub_double(&tmp, &cur_gen);
            jub_double(&cur_gen, &tmp);
            jub_double(&tmp, &cur_gen);
            cur_gen = tmp;
        }
    }

    *x_out = acc_x;
    *y_out = acc_y;
}

/* ── Montgomery Point Addition (3 constraints) ─────────────────── */

/* Jubjub Montgomery form: B*y^2 = x^3 + A*x^2 + x
 * where A = 40962, B = -40960 (mod Fr)
 * Montgomery A as Fr constant */
static void montgomery_a(struct fr *a_out)
{
    uint8_t bytes[32] = {0};
    /* 40962 = 0xA002 */
    bytes[0] = 0x02;
    bytes[1] = 0xA0;
    fr_from_bytes(a_out, bytes);
}

/* Montgomery addition:
 * lambda = (y2-y1)/(x2-x1)
 * x3 = lambda^2 - A - x1 - x2
 * y3 = lambda*(x1-x3) - y1
 * 3 constraints: lambda eval, x3 eval, y3 eval */
static void gadget_montgomery_add(struct constraint_system *cs,
                                   /* self (x1,y1) as LC term arrays */
                                   size_t self_x_nterms,
                                   const size_t *self_x_vars,
                                   const struct fr *self_x_coeffs,
                                   size_t self_y_nterms,
                                   const size_t *self_y_vars,
                                   const struct fr *self_y_coeffs,
                                   /* other (x2,y2) as LC term arrays */
                                   size_t other_x_nterms,
                                   const size_t *other_x_vars,
                                   const struct fr *other_x_coeffs,
                                   size_t other_y_nterms,
                                   const size_t *other_y_vars,
                                   const struct fr *other_y_coeffs,
                                   /* output */
                                   size_t *out_x, size_t *out_y)
{
    struct fr one_val, neg_one;
    fr_one(&one_val);
    fr_neg(&neg_one, &one_val);
    struct fr mont_a;
    montgomery_a(&mont_a);

    /* Evaluate self and other LCs */
    struct fr x1v, y1v, x2v, y2v;
    fr_zero(&x1v);
    for (size_t i = 0; i < self_x_nterms; i++) {
        struct fr t;
        fr_mul(&t, &cs->witness[self_x_vars[i]], &self_x_coeffs[i]);
        fr_add(&x1v, &x1v, &t);
    }
    fr_zero(&y1v);
    for (size_t i = 0; i < self_y_nterms; i++) {
        struct fr t;
        fr_mul(&t, &cs->witness[self_y_vars[i]], &self_y_coeffs[i]);
        fr_add(&y1v, &y1v, &t);
    }
    fr_zero(&x2v);
    for (size_t i = 0; i < other_x_nterms; i++) {
        struct fr t;
        fr_mul(&t, &cs->witness[other_x_vars[i]], &other_x_coeffs[i]);
        fr_add(&x2v, &x2v, &t);
    }
    fr_zero(&y2v);
    for (size_t i = 0; i < other_y_nterms; i++) {
        struct fr t;
        fr_mul(&t, &cs->witness[other_y_vars[i]], &other_y_coeffs[i]);
        fr_add(&y2v, &y2v, &t);
    }

    /* lambda = (y2-y1)/(x2-x1) */
    struct fr lambda_val;
    {
        struct fr num, den;
        fr_sub(&num, &y2v, &y1v);
        fr_sub(&den, &x2v, &x1v);
        fr_inv(&lambda_val, &den);
        fr_mul(&lambda_val, &lambda_val, &num);
    }
    size_t lambda = cs_alloc_aux(cs, &lambda_val);

    /* Constraint 1: (x2-x1)*lambda = y2-y1 */
    {
        struct linear_combination la, lb, lc;
        lc_init(&la);
        for (size_t i = 0; i < other_x_nterms; i++)
            lc_add_term(&la, other_x_vars[i], &other_x_coeffs[i]);
        for (size_t i = 0; i < self_x_nterms; i++) {
            struct fr neg_c;
            fr_neg(&neg_c, &self_x_coeffs[i]);
            lc_add_term(&la, self_x_vars[i], &neg_c);
        }
        lc_init(&lb);
        lc_add_term(&lb, lambda, &one_val);
        lc_init(&lc);
        for (size_t i = 0; i < other_y_nterms; i++)
            lc_add_term(&lc, other_y_vars[i], &other_y_coeffs[i]);
        for (size_t i = 0; i < self_y_nterms; i++) {
            struct fr neg_c;
            fr_neg(&neg_c, &self_y_coeffs[i]);
            lc_add_term(&lc, self_y_vars[i], &neg_c);
        }
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* x3 = lambda^2 - A - x1 - x2 */
    struct fr x3v;
    {
        fr_mul(&x3v, &lambda_val, &lambda_val);
        fr_sub(&x3v, &x3v, &mont_a);
        fr_sub(&x3v, &x3v, &x1v);
        fr_sub(&x3v, &x3v, &x2v);
    }
    *out_x = cs_alloc_aux(cs, &x3v);

    /* Constraint 2: lambda*lambda = A + x1 + x2 + x3 */
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, lambda, &one_val);
        lc_init(&lb); lc_add_term(&lb, lambda, &one_val);
        lc_init(&lc);
        lc_add_term(&lc, CS_ONE, &mont_a);
        for (size_t i = 0; i < self_x_nterms; i++)
            lc_add_term(&lc, self_x_vars[i], &self_x_coeffs[i]);
        for (size_t i = 0; i < other_x_nterms; i++)
            lc_add_term(&lc, other_x_vars[i], &other_x_coeffs[i]);
        lc_add_term(&lc, *out_x, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* y3 = -(y1 + lambda*(x3-x1)) = lambda*(x1-x3) - y1 */
    struct fr y3v;
    {
        struct fr diff;
        fr_sub(&diff, &x3v, &x1v);
        fr_mul(&y3v, &lambda_val, &diff);
        fr_add(&y3v, &y3v, &y1v);
        fr_neg(&y3v, &y3v);
    }
    *out_y = cs_alloc_aux(cs, &y3v);

    /* Constraint 3: (x1-x3)*lambda = y3+y1 */
    {
        struct linear_combination la, lb, lc;
        lc_init(&la);
        for (size_t i = 0; i < self_x_nterms; i++)
            lc_add_term(&la, self_x_vars[i], &self_x_coeffs[i]);
        lc_add_term(&la, *out_x, &neg_one);

        lc_init(&lb);
        lc_add_term(&lb, lambda, &one_val);

        lc_init(&lc);
        lc_add_term(&lc, *out_y, &one_val);
        for (size_t i = 0; i < self_y_nterms; i++)
            lc_add_term(&lc, self_y_vars[i], &self_y_coeffs[i]);

        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
}

/* Montgomery to Edwards conversion (2 constraints):
 * Edwards (u, v) from Montgomery (x, y):
 *   u = scale * x / y
 *   v = (x - 1) / (x + 1)
 * where scale = sqrt(-40964) (the Jubjub "scale" parameter) */
static void montgomery_scale(struct fr *s)
{
    /* scale = sqrt(-40964) mod Fr. Precomputed constant. */
    static const uint8_t SCALE_BYTES[32] = {
        0xe4,0xf6,0xa0,0xfe,0x4f,0x82,0xb3,0x57,
        0xab,0x32,0x83,0x31,0xa2,0xf8,0x1d,0x24,
        0x4a,0xf8,0x83,0x3e,0x4e,0x31,0x12,0x84,
        0x7a,0x53,0xcf,0x9a,0x07,0x1e,0x5c,0x17
    };
    fr_from_bytes(s, SCALE_BYTES);
}

static void gadget_montgomery_to_edwards(struct constraint_system *cs,
                                           /* Montgomery point as LC terms */
                                           size_t mx_nterms,
                                           const size_t *mx_vars,
                                           const struct fr *mx_coeffs,
                                           size_t my_nterms,
                                           const size_t *my_vars,
                                           const struct fr *my_coeffs,
                                           /* output Edwards point */
                                           size_t *ex, size_t *ey)
{
    struct fr one_val, neg_one;
    fr_one(&one_val);
    fr_neg(&neg_one, &one_val);
    struct fr scale;
    montgomery_scale(&scale);

    /* Evaluate Montgomery LCs */
    struct fr mx_val, my_val;
    fr_zero(&mx_val);
    for (size_t i = 0; i < mx_nterms; i++) {
        struct fr t;
        fr_mul(&t, &cs->witness[mx_vars[i]], &mx_coeffs[i]);
        fr_add(&mx_val, &mx_val, &t);
    }
    fr_zero(&my_val);
    for (size_t i = 0; i < my_nterms; i++) {
        struct fr t;
        fr_mul(&t, &cs->witness[my_vars[i]], &my_coeffs[i]);
        fr_add(&my_val, &my_val, &t);
    }

    /* u = scale * x / y */
    struct fr u_val;
    {
        struct fr sx;
        fr_mul(&sx, &scale, &mx_val);
        fr_inv(&u_val, &my_val);
        fr_mul(&u_val, &u_val, &sx);
    }
    *ex = cs_alloc_aux(cs, &u_val);

    /* Constraint: y * u = scale * x */
    {
        struct linear_combination la, lb, lc;
        lc_init(&la);
        for (size_t i = 0; i < my_nterms; i++)
            lc_add_term(&la, my_vars[i], &my_coeffs[i]);
        lc_init(&lb);
        lc_add_term(&lb, *ex, &one_val);
        lc_init(&lc);
        for (size_t i = 0; i < mx_nterms; i++) {
            struct fr sc;
            fr_mul(&sc, &scale, &mx_coeffs[i]);
            lc_add_term(&lc, mx_vars[i], &sc);
        }
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    /* v = (x - 1) / (x + 1) */
    struct fr v_val;
    {
        struct fr num, den;
        fr_sub(&num, &mx_val, &one_val);
        fr_add(&den, &mx_val, &one_val);
        fr_inv(&v_val, &den);
        fr_mul(&v_val, &v_val, &num);
    }
    *ey = cs_alloc_aux(cs, &v_val);

    /* Constraint: (x + 1) * v = (x - 1) */
    {
        struct linear_combination la, lb, lc;
        lc_init(&la);
        for (size_t i = 0; i < mx_nterms; i++)
            lc_add_term(&la, mx_vars[i], &mx_coeffs[i]);
        lc_add_term(&la, CS_ONE, &one_val);
        lc_init(&lb);
        lc_add_term(&lb, *ey, &one_val);
        lc_init(&lc);
        for (size_t i = 0; i < mx_nterms; i++)
            lc_add_term(&lc, mx_vars[i], &mx_coeffs[i]);
        lc_add_term(&lc, CS_ONE, &neg_one);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
}

/* ── Pedersen Hash (Montgomery-based) ──────────────────────────── */

#define PEDERSEN_CHUNKS_PER_GEN 63
#define PEDERSEN_NUM_GEN 6

static struct jub_point ph_generators_cache[PEDERSEN_NUM_GEN];
static bool ph_generators_loaded = false;

static void ensure_ph_generators(void)
{
    if (ph_generators_loaded) return;
    const uint8_t pers[8] = {'Z','c','a','s','h','_','P','H'};
    for (int i = 0; i < PEDERSEN_NUM_GEN; i++) {
        uint8_t tag[5] = {(uint8_t)i, 0, 0, 0, 0};
        for (int c = 0; c < 256; c++) {
            tag[4] = (uint8_t)c;
            if (group_hash(&ph_generators_cache[i], tag, 5, pers))
                break;
        }
    }
    ph_generators_loaded = true;
}

/* Convert Edwards point to Montgomery coordinates.
 * Montgomery x = (1+y)/(1-y), Montgomery y = scale*x_mont/x_edwards */
static void edwards_to_montgomery(struct fr *mx, struct fr *my,
                                    const struct jub_point *p)
{
    struct fr ex, ey;
    jub_get_x(&ex, p);
    jub_get_y(&ey, p);
    struct fr one_val;
    fr_one(&one_val);

    /* mx = (1+y)/(1-y) */
    struct fr num, den;
    fr_add(&num, &one_val, &ey);
    fr_sub(&den, &one_val, &ey);
    fr_inv(mx, &den);
    fr_mul(mx, mx, &num);

    /* my = scale * mx / ex */
    struct fr scale;
    montgomery_scale(&scale);
    struct fr inv_ex;
    fr_inv(&inv_ex, &ex);
    fr_mul(my, &scale, mx);
    fr_mul(my, my, &inv_ex);
}

void gadget_pedersen_hash(struct constraint_system *cs,
                          const size_t *input_bits, size_t n_bits,
                          const char *personalization,
                          size_t *x_out, size_t *y_out)
{
    ensure_ph_generators();

    /* Build full bit stream: 6-bit personalization prefix + input bits */
    uint8_t pers_bits[6] = {0};
    if (personalization && strcmp(personalization, "Zcash_PH") == 0) {
        /* NoteCommitment personalization = 0b110110 */
        pers_bits[0] = 0; pers_bits[1] = 1; pers_bits[2] = 1;
        pers_bits[3] = 0; pers_bits[4] = 1; pers_bits[5] = 1;
    }

    size_t total_bits = 6 + n_bits;
    size_t bit_pos = 0;

    /* Edwards accumulator for cross-segment additions */
    size_t edwards_acc_x = SIZE_MAX, edwards_acc_y = SIZE_MAX;
    struct fr one_val;
    fr_one(&one_val);

    int seg = 0;
    while (bit_pos < total_bits && seg < PEDERSEN_NUM_GEN) {
        /* Montgomery accumulator within segment */
        size_t seg_x_nterms = 0, seg_y_nterms = 0;
        size_t seg_x_vars[128], seg_y_vars[128];
        struct fr seg_x_coeffs[128], seg_y_coeffs[128];
        bool first_in_seg = true;

        struct jub_point base = ph_generators_cache[seg];

        for (int chunk = 0; chunk < PEDERSEN_CHUNKS_PER_GEN && bit_pos < total_bits; chunk++) {
            /* Get 3 bits */
            size_t b0_var = SIZE_MAX, b1_var = SIZE_MAX, b2_var = SIZE_MAX;
            bool b0_const = false, b1_const = false, b2_const = false;
            bool b0v = false, b1v = false, b2v = false;

            if (bit_pos < 6) {
                b0_const = true;
                b0v = pers_bits[bit_pos];
            } else {
                b0_var = input_bits[bit_pos - 6];
                b0v = !fr_is_zero(&cs->witness[b0_var]);
            }
            bit_pos++;

            if (bit_pos < total_bits) {
                if (bit_pos < 6) {
                    b1_const = true;
                    b1v = pers_bits[bit_pos];
                } else {
                    b1_var = input_bits[bit_pos - 6];
                    b1v = !fr_is_zero(&cs->witness[b1_var]);
                }
                bit_pos++;
            } else { b1_const = true; b1v = false; }

            if (bit_pos < total_bits) {
                if (bit_pos < 6) {
                    b2_const = true;
                    b2v = pers_bits[bit_pos];
                } else {
                    b2_var = input_bits[bit_pos - 6];
                    b2v = !fr_is_zero(&cs->witness[b2_var]);
                }
                bit_pos++;
            } else { b2_const = true; b2v = false; }

            /* Precompute 4 Montgomery points: base, 2*base, 3*base, 4*base */
            struct fr m_x[4], m_y[4];
            {
                struct jub_point pts[4];
                pts[0] = base;
                jub_double(&pts[1], &base);
                jub_add(&pts[2], &pts[1], &base);
                jub_double(&pts[3], &pts[1]);
                for (int k = 0; k < 4; k++)
                    edwards_to_montgomery(&m_x[k], &m_y[k], &pts[k]);
            }

            /* lookup3_xy_with_conditional_negation:
             * Index from (b0, b1): select point. b2 = sign bit (negate y).
             * Returns x as linear combination (0 constraints), y as allocated (1 constraint).
             * Plus 1 constraint for AND(b0, b1) if both are allocated. */

            /* Compute precomp = b0 AND b1 */
            int lookup_idx = (b0v ? 1 : 0) | (b1v ? 2 : 0);
            struct fr sel_y = m_y[lookup_idx];
            if (b2v) fr_neg(&sel_y, &sel_y);

            /* Compute synth coefficients for x (4 values, 2-bit lookup) */
            struct fr xc[4], yc[4];
            synth_coeffs(2, m_x, xc);
            synth_coeffs(2, m_y, yc);

            /* AND constraint for precomp (only if both b0, b1 are allocated) */
            size_t precomp_var = SIZE_MAX;
            bool precomp_const = false;
            bool precomp_val = b0v && b1v;

            if (!b0_const && !b1_const) {
                struct fr pv;
                if (precomp_val) fr_one(&pv); else fr_zero(&pv);
                precomp_var = cs_alloc_aux(cs, &pv);
                gadget_mul(cs, b0_var, b1_var, precomp_var); /* 1 constraint */
            } else {
                precomp_const = true;
            }

            /* Build x as linear combination (no constraint needed) */
            /* x = xc[00]*1 + xc[01]*b0 + xc[10]*b1 + xc[11]*precomp */
            size_t new_x_nterms = 0;
            size_t new_x_vars[4];
            struct fr new_x_coeffs[4];

            /* Constant term xc[00] */
            new_x_vars[new_x_nterms] = CS_ONE;
            new_x_coeffs[new_x_nterms] = xc[0b00];
            new_x_nterms++;

            if (!b0_const) {
                new_x_vars[new_x_nterms] = b0_var;
                new_x_coeffs[new_x_nterms] = xc[0b01];
                new_x_nterms++;
            } else if (b0v) {
                fr_add(&new_x_coeffs[0], &new_x_coeffs[0], &xc[0b01]);
            }

            if (!b1_const) {
                new_x_vars[new_x_nterms] = b1_var;
                new_x_coeffs[new_x_nterms] = xc[0b10];
                new_x_nterms++;
            } else if (b1v) {
                fr_add(&new_x_coeffs[0], &new_x_coeffs[0], &xc[0b10]);
            }

            if (!precomp_const) {
                new_x_vars[new_x_nterms] = precomp_var;
                new_x_coeffs[new_x_nterms] = xc[0b11];
                new_x_nterms++;
            } else if (precomp_val) {
                fr_add(&new_x_coeffs[0], &new_x_coeffs[0], &xc[0b11]);
            }

            /* y: allocated variable with conditional negation.
             * y_lc = yc[00] + yc[01]*b0 + yc[10]*b1 + yc[11]*precomp
             * Constraint: 2*y_lc * b2 = y_lc - y_allocated
             * If b2=0: y = y_lc. If b2=1: y = -y_lc. */
            struct fr y_alloc_val = sel_y;
            size_t y_alloc = cs_alloc_aux(cs, &y_alloc_val);

            /* Build the y_lc for the constraint */
            /* 2*y_lc * b2 = y_lc - y */
            {
                struct linear_combination la, lb, lc;
                struct fr two; fr_add(&two, &one_val, &one_val);

                lc_init(&la);
                /* 2 * y_lc terms */
                struct fr t2;
                fr_mul(&t2, &two, &yc[0b00]);
                lc_add_term(&la, CS_ONE, &t2);
                if (!b0_const) {
                    fr_mul(&t2, &two, &yc[0b01]);
                    lc_add_term(&la, b0_var, &t2);
                } else if (b0v) {
                    struct fr add;
                    fr_mul(&add, &two, &yc[0b01]);
                    /* Add to constant term */
                    struct fr cur;
                    fr_mul(&cur, &two, &yc[0b00]);
                    fr_add(&cur, &cur, &add);
                    la.terms[0].coeff = cur; /* update first term */
                }
                if (!b1_const) {
                    fr_mul(&t2, &two, &yc[0b10]);
                    lc_add_term(&la, b1_var, &t2);
                } else if (b1v) {
                    struct fr add;
                    fr_mul(&add, &two, &yc[0b10]);
                    fr_add(&la.terms[0].coeff, &la.terms[0].coeff, &add);
                }
                if (!precomp_const) {
                    fr_mul(&t2, &two, &yc[0b11]);
                    lc_add_term(&la, precomp_var, &t2);
                } else if (precomp_val) {
                    struct fr add;
                    fr_mul(&add, &two, &yc[0b11]);
                    fr_add(&la.terms[0].coeff, &la.terms[0].coeff, &add);
                }

                lc_init(&lb);
                if (!b2_const) {
                    lc_add_term(&lb, b2_var, &one_val);
                } else if (b2v) {
                    lc_add_term(&lb, CS_ONE, &one_val);
                }
                /* else b2=constant false: lb = 0 */

                lc_init(&lc);
                /* y_lc - y */
                lc_add_term(&lc, CS_ONE, &yc[0b00]);
                if (!b0_const) {
                    lc_add_term(&lc, b0_var, &yc[0b01]);
                } else if (b0v) {
                    fr_add(&lc.terms[0].coeff, &lc.terms[0].coeff, &yc[0b01]);
                }
                if (!b1_const) {
                    lc_add_term(&lc, b1_var, &yc[0b10]);
                } else if (b1v) {
                    fr_add(&lc.terms[0].coeff, &lc.terms[0].coeff, &yc[0b10]);
                }
                if (!precomp_const) {
                    lc_add_term(&lc, precomp_var, &yc[0b11]);
                } else if (precomp_val) {
                    fr_add(&lc.terms[0].coeff, &lc.terms[0].coeff, &yc[0b11]);
                }
                struct fr neg; fr_neg(&neg, &one_val);
                lc_add_term(&lc, y_alloc, &neg);

                cs_enforce(cs, &la, &lb, &lc);
                lc_free(&la); lc_free(&lb); lc_free(&lc);
            }

            /* Build y LC for Montgomery operations */
            size_t new_y_nterms = 1;
            size_t new_y_vars_arr[1] = { y_alloc };
            struct fr new_y_coeffs_arr[1];
            fr_one(&new_y_coeffs_arr[0]);

            /* Montgomery addition with segment accumulator */
            if (first_in_seg) {
                memcpy(seg_x_vars, new_x_vars, new_x_nterms * sizeof(size_t));
                memcpy(seg_x_coeffs, new_x_coeffs, new_x_nterms * sizeof(struct fr));
                seg_x_nterms = new_x_nterms;
                memcpy(seg_y_vars, new_y_vars_arr, new_y_nterms * sizeof(size_t));
                memcpy(seg_y_coeffs, new_y_coeffs_arr, new_y_nterms * sizeof(struct fr));
                seg_y_nterms = new_y_nterms;
                first_in_seg = false;
            } else {
                size_t res_x, res_y;
                gadget_montgomery_add(cs,
                    seg_x_nterms, seg_x_vars, seg_x_coeffs,
                    seg_y_nterms, seg_y_vars, seg_y_coeffs,
                    new_x_nterms, new_x_vars, new_x_coeffs,
                    new_y_nterms, new_y_vars_arr, new_y_coeffs_arr,
                    &res_x, &res_y);
                /* Result is now an AllocatedNum (single variable) */
                seg_x_nterms = 1;
                seg_x_vars[0] = res_x;
                fr_one(&seg_x_coeffs[0]);
                seg_y_nterms = 1;
                seg_y_vars[0] = res_y;
                fr_one(&seg_y_coeffs[0]);
            }

            /* Advance base by ×16 (4 doublings) for next chunk in segment */
            if (chunk + 1 < PEDERSEN_CHUNKS_PER_GEN && bit_pos < total_bits) {
                struct jub_point tmp;
                jub_double(&tmp, &base);
                jub_double(&base, &tmp);
                jub_double(&tmp, &base);
                jub_double(&base, &tmp);
            }
        }

        /* Convert segment result from Montgomery to Edwards */
        size_t seg_ed_x, seg_ed_y;
        gadget_montgomery_to_edwards(cs,
            seg_x_nterms, seg_x_vars, seg_x_coeffs,
            seg_y_nterms, seg_y_vars, seg_y_coeffs,
            &seg_ed_x, &seg_ed_y);

        /* Edwards addition to global accumulator */
        if (edwards_acc_x == SIZE_MAX) {
            edwards_acc_x = seg_ed_x;
            edwards_acc_y = seg_ed_y;
        } else {
            size_t new_x, new_y;
            gadget_edwards_add(cs, edwards_acc_x, edwards_acc_y,
                               seg_ed_x, seg_ed_y, &new_x, &new_y);
            edwards_acc_x = new_x;
            edwards_acc_y = new_y;
        }

        seg++;
    }

    *x_out = edwards_acc_x;
    *y_out = edwards_acc_y;
}

/* ── Blake2s Gadget ─────────────────────────────────────────────── */

void gadget_blake2s(struct constraint_system *cs,
                    const size_t *input_bits, size_t n_input_bits,
                    const uint8_t *personalization,
                    size_t *output_bits)
{
    uint8_t input_bytes[256];
    memset(input_bytes, 0, sizeof(input_bytes));
    size_t n_bytes = (n_input_bits + 7) / 8;
    if (n_bytes > sizeof(input_bytes)) n_bytes = sizeof(input_bytes);

    for (size_t i = 0; i < n_input_bits && i / 8 < n_bytes; i++) {
        struct fr bit_val = cs->witness[input_bits[i]];
        if (!fr_is_zero(&bit_val))
            input_bytes[i / 8] |= (uint8_t)(1 << (i % 8));
    }

    uint8_t hash_out[32];
    uint8_t pers[BLAKE2S_PERSONALBYTES];
    memset(pers, 0, sizeof(pers));
    if (personalization)
        memcpy(pers, personalization,
               strlen((const char *)personalization) < BLAKE2S_PERSONALBYTES
               ? strlen((const char *)personalization) : BLAKE2S_PERSONALBYTES);

    struct blake2s_ctx bctx;
    blake2s_init_personal(&bctx, 32, pers);
    blake2s_update(&bctx, input_bytes, n_bytes);
    blake2s_final(&bctx, hash_out, 32);

    /* input_bytes holds a witness-derived byte decomposition; it has been
     * fully consumed by blake2s_update above and is never read again. */
    memory_cleanse(input_bytes, sizeof(input_bytes));

    for (size_t i = 0; i < 256; i++) {
        bool bit = (hash_out[i / 8] >> (i % 8)) & 1;
        output_bits[i] = gadget_alloc_boolean(cs, bit);
    }
}

/* ── Merkle Path Verification ───────────────────────────────────── */

size_t gadget_merkle_path(struct constraint_system *cs,
                          size_t leaf,
                          const size_t *path_bits,
                          const size_t *siblings,
                          size_t depth)
{
    size_t current = leaf;
    for (size_t i = 0; i < depth; i++) {
        size_t left = gadget_select(cs, path_bits[i], siblings[i], current);
        size_t right = gadget_select(cs, path_bits[i], current, siblings[i]);
        size_t left_bits[256], right_bits[256];
        gadget_unpack_bits(cs, left_bits, 256, &cs->witness[left]);
        gadget_unpack_bits(cs, right_bits, 256, &cs->witness[right]);
        size_t hash_bits[512];
        memcpy(hash_bits, left_bits, 256 * sizeof(size_t));
        memcpy(hash_bits + 256, right_bits, 256 * sizeof(size_t));
        size_t hash_x, hash_y;
        gadget_pedersen_hash(cs, hash_bits, 512, "Zcash_PH", &hash_x, &hash_y);
        current = hash_x;
    }
    return current;
}

/* ── Note Commitment Gadget ─────────────────────────────────────── */

size_t gadget_note_commitment(struct constraint_system *cs,
                              size_t *gd_bits, size_t n_gd_bits,
                              size_t *pkd_bits, size_t n_pkd_bits,
                              size_t *value_bits,
                              size_t *rcm_bits)
{
    size_t total_bits = n_gd_bits + n_pkd_bits + 64 + 256;
    size_t *all_bits = zcl_malloc(total_bits * sizeof(size_t), "circuit_note_bits");
    if (!all_bits) return 0;
    size_t offset = 0;
    memcpy(all_bits + offset, gd_bits, n_gd_bits * sizeof(size_t)); offset += n_gd_bits;
    memcpy(all_bits + offset, pkd_bits, n_pkd_bits * sizeof(size_t)); offset += n_pkd_bits;
    memcpy(all_bits + offset, value_bits, 64 * sizeof(size_t)); offset += 64;
    memcpy(all_bits + offset, rcm_bits, 256 * sizeof(size_t));
    size_t cm_x, cm_y;
    gadget_pedersen_hash(cs, all_bits, total_bits, "Zcash_PH", &cm_x, &cm_y);
    /* all_bits holds a witness-derived bit decomposition; consumed by the
     * pedersen hash above. Cleanse before free (value already consumed). */
    memory_cleanse(all_bits, total_bits * sizeof(size_t));
    free(all_bits);
    return cm_x;
}

/* ── Nullifier Derivation ───────────────────────────────────────── */

void gadget_nullifier(struct constraint_system *cs,
                      size_t *nk_bits, size_t n_nk_bits,
                      size_t rho_x, size_t rho_y,
                      size_t *nf_x, size_t *nf_y)
{
    size_t hash_x, hash_y;
    gadget_pedersen_hash(cs, nk_bits, n_nk_bits, "Zcash_J_", &hash_x, &hash_y);
    gadget_edwards_add(cs, hash_x, hash_y, rho_x, rho_y, nf_x, nf_y);
}

/* ── Point On-Curve Check (4 constraints) ──────────────────────── */

void gadget_point_interpret(struct constraint_system *cs, size_t x, size_t y)
{
    size_t x2 = gadget_alloc_mul(cs, x, x);
    size_t y2 = gadget_alloc_mul(cs, y, y);
    size_t x2y2 = gadget_alloc_mul(cs, x2, y2);

    struct fr d_val;
    jubjub_d(&d_val);
    struct fr one_val, neg_one;
    fr_one(&one_val);
    fr_neg(&neg_one, &one_val);

    struct linear_combination la, lb, lc;
    lc_init(&la);
    lc_add_term(&la, x2, &neg_one);
    lc_add_term(&la, y2, &one_val);
    lc_init(&lb);
    lc_add_term(&lb, CS_ONE, &one_val);
    lc_init(&lc);
    lc_add_term(&lc, CS_ONE, &one_val);
    lc_add_term(&lc, x2y2, &d_val);
    cs_enforce(cs, &la, &lb, &lc);
    lc_free(&la); lc_free(&lb); lc_free(&lc);
}

/* ── Assert Not Small Order (16 constraints) ──────────────────── */

void gadget_assert_not_small_order(struct constraint_system *cs,
                                     size_t x, size_t y)
{
    size_t cur_x = x, cur_y = y;
    for (int i = 0; i < 3; i++) {
        size_t dx, dy;
        gadget_edwards_double(cs, cur_x, cur_y, &dx, &dy);
        cur_x = dx;
        cur_y = dy;
    }

    struct fr x_val = cs->witness[cur_x];
    struct fr inv_val;
    fr_inv(&inv_val, &x_val);
    size_t inv_var = cs_alloc_aux(cs, &inv_val);

    struct linear_combination la, lb, lc;
    struct fr one_val;
    fr_one(&one_val);
    lc_init(&la); lc_add_term(&la, cur_x, &one_val);
    lc_init(&lb); lc_add_term(&lb, inv_var, &one_val);
    lc_init(&lc); lc_add_term(&lc, CS_ONE, &one_val);
    cs_enforce(cs, &la, &lb, &lc);
    lc_free(&la); lc_free(&lb); lc_free(&lc);
}

/* ── Conditionally Select Point (2 constraints) ──────────────── */

void gadget_conditionally_select_point(struct constraint_system *cs,
                                         size_t cond, size_t px, size_t py,
                                         size_t *rx, size_t *ry)
{
    struct fr cond_val = cs->witness[cond];
    struct fr px_val = cs->witness[px];
    struct fr py_val = cs->witness[py];
    struct fr one_val, neg_one;
    fr_one(&one_val);
    fr_neg(&neg_one, &one_val);

    struct fr rx_val;
    fr_mul(&rx_val, &cond_val, &px_val);
    *rx = cs_alloc_aux(cs, &rx_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, px, &one_val);
        lc_init(&lb); lc_add_term(&lb, cond, &one_val);
        lc_init(&lc); lc_add_term(&lc, *rx, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }

    struct fr py_minus_1;
    fr_sub(&py_minus_1, &py_val, &one_val);
    struct fr ry_val;
    fr_mul(&ry_val, &cond_val, &py_minus_1);
    fr_add(&ry_val, &ry_val, &one_val);
    *ry = cs_alloc_aux(cs, &ry_val);
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, py, &one_val);
        lc_init(&lb); lc_add_term(&lb, cond, &one_val);
        lc_init(&lc);
        lc_add_term(&lc, *ry, &one_val);
        lc_add_term(&lc, cond, &neg_one);
        lc_add_term(&lc, CS_ONE, &neg_one);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
}

/* ── Variable-Base Scalar Multiplication ───────────────────────── */

void gadget_variable_base_mul(struct constraint_system *cs,
                                size_t base_x, size_t base_y,
                                const size_t *scalar_bits, size_t n_bits,
                                size_t *out_x, size_t *out_y)
{
    size_t cur_x = base_x, cur_y = base_y;
    size_t acc_x = SIZE_MAX, acc_y = SIZE_MAX;

    for (size_t i = 0; i < n_bits; i++) {
        if (i > 0) {
            size_t dbl_x, dbl_y;
            gadget_edwards_double(cs, cur_x, cur_y, &dbl_x, &dbl_y);
            cur_x = dbl_x;
            cur_y = dbl_y;
        }

        size_t sel_x, sel_y;
        gadget_conditionally_select_point(cs, scalar_bits[i],
                                           cur_x, cur_y, &sel_x, &sel_y);

        if (acc_x == SIZE_MAX) {
            acc_x = sel_x;
            acc_y = sel_y;
        } else {
            size_t new_x, new_y;
            gadget_edwards_add(cs, acc_x, acc_y, sel_x, sel_y,
                               &new_x, &new_y);
            acc_x = new_x;
            acc_y = new_y;
        }
    }

    *out_x = acc_x;
    *out_y = acc_y;
}

/* ── Point Inputize (2 constraints) ────────────────────────────── */

void gadget_point_inputize(struct constraint_system *cs, size_t x, size_t y)
{
    struct fr x_val = cs->witness[x];
    struct fr y_val = cs->witness[y];
    size_t ix = cs_alloc_input(cs, &x_val);
    size_t iy = cs_alloc_input(cs, &y_val);
    struct fr one_val;
    fr_one(&one_val);

    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, x, &one_val);
        lc_init(&lb); lc_add_term(&lb, CS_ONE, &one_val);
        lc_init(&lc); lc_add_term(&lc, ix, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
    {
        struct linear_combination la, lb, lc;
        lc_init(&la); lc_add_term(&la, y, &one_val);
        lc_init(&lb); lc_add_term(&lb, CS_ONE, &one_val);
        lc_init(&lc); lc_add_term(&lc, iy, &one_val);
        cs_enforce(cs, &la, &lb, &lc);
        lc_free(&la); lc_free(&lb); lc_free(&lc);
    }
}

/* ── Scalar Inputize (1 constraint) ────────────────────────────── */

void gadget_scalar_inputize(struct constraint_system *cs, size_t var)
{
    struct fr val = cs->witness[var];
    size_t ivar = cs_alloc_input(cs, &val);
    struct fr one_val;
    fr_one(&one_val);
    struct linear_combination la, lb, lc;
    lc_init(&la); lc_add_term(&la, var, &one_val);
    lc_init(&lb); lc_add_term(&lb, CS_ONE, &one_val);
    lc_init(&lc); lc_add_term(&lc, ivar, &one_val);
    cs_enforce(cs, &la, &lb, &lc);
    lc_free(&la); lc_free(&lb); lc_free(&lc);
}

/* ── Strict Bit Decomposition (into_bits_le_strict) ────────────── */

/* BLS12-381 Fr modulus - 1 as 256 raw bits (MSB first) */
/* ── field_into_boolean_vec_le (simple, no packing) ────────────── */

