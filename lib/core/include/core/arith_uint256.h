/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_ARITH_UINT256_H
#define BITCOIN_ARITH_UINT256_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ARITH_UINT256_WIDTH 8

struct arith_uint256 {
    uint32_t pn[ARITH_UINT256_WIDTH];
};

struct uint256;

static inline void arith_uint256_set_zero(struct arith_uint256 *a)
{
    memset(a->pn, 0, sizeof(a->pn));
}

static inline void arith_uint256_set_u64(struct arith_uint256 *a, uint64_t v)
{
    a->pn[0] = (uint32_t)v;
    a->pn[1] = (uint32_t)(v >> 32);
    for (int i = 2; i < ARITH_UINT256_WIDTH; i++)
        a->pn[i] = 0;
}

static inline uint64_t arith_uint256_get_low64(const struct arith_uint256 *a)
{
    return a->pn[0] | (uint64_t)a->pn[1] << 32;
}

static inline bool arith_uint256_is_zero(const struct arith_uint256 *a)
{
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++)
        if (a->pn[i] != 0) return false;
    return true;
}

static inline int arith_uint256_compare(const struct arith_uint256 *a, const struct arith_uint256 *b)
{
    for (int i = ARITH_UINT256_WIDTH - 1; i >= 0; i--) {
        if (a->pn[i] < b->pn[i]) return -1;
        if (a->pn[i] > b->pn[i]) return 1;
    }
    return 0;
}

static inline bool arith_uint256_equal_u64(const struct arith_uint256 *a, uint64_t b)
{
    for (int i = ARITH_UINT256_WIDTH - 1; i >= 2; i--)
        if (a->pn[i]) return false;
    if (a->pn[1] != (uint32_t)(b >> 32)) return false;
    if (a->pn[0] != (uint32_t)(b & 0xfffffffful)) return false;
    return true;
}

static inline void arith_uint256_negate(struct arith_uint256 *r, const struct arith_uint256 *a)
{
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++)
        r->pn[i] = ~a->pn[i];
    int i = 0;
    while (i < ARITH_UINT256_WIDTH && ++r->pn[i] == 0)
        i++;
}

static inline void arith_uint256_complement(struct arith_uint256 *r, const struct arith_uint256 *a)
{
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++)
        r->pn[i] = ~a->pn[i];
}

static inline void arith_uint256_add(struct arith_uint256 *r,
                                      const struct arith_uint256 *a,
                                      const struct arith_uint256 *b)
{
    uint64_t carry = 0;
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++) {
        uint64_t n = carry + a->pn[i] + b->pn[i];
        r->pn[i] = n & 0xffffffff;
        carry = n >> 32;
    }
}

static inline void arith_uint256_sub(struct arith_uint256 *r,
                                      const struct arith_uint256 *a,
                                      const struct arith_uint256 *b)
{
    struct arith_uint256 neg;
    arith_uint256_negate(&neg, b);
    arith_uint256_add(r, a, &neg);
}

static inline void arith_uint256_mul_u32(struct arith_uint256 *r,
                                          const struct arith_uint256 *a, uint32_t b)
{
    uint64_t carry = 0;
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++) {
        uint64_t n = carry + (uint64_t)b * a->pn[i];
        r->pn[i] = n & 0xffffffff;
        carry = n >> 32;
    }
}

static inline void arith_uint256_or(struct arith_uint256 *r,
                                     const struct arith_uint256 *a,
                                     const struct arith_uint256 *b)
{
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++)
        r->pn[i] = a->pn[i] | b->pn[i];
}

static inline void arith_uint256_and(struct arith_uint256 *r,
                                      const struct arith_uint256 *a,
                                      const struct arith_uint256 *b)
{
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++)
        r->pn[i] = a->pn[i] & b->pn[i];
}

static inline void arith_uint256_xor(struct arith_uint256 *r,
                                      const struct arith_uint256 *a,
                                      const struct arith_uint256 *b)
{
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++)
        r->pn[i] = a->pn[i] ^ b->pn[i];
}

void arith_uint256_shl(struct arith_uint256 *r, const struct arith_uint256 *a, unsigned int shift);
void arith_uint256_shr(struct arith_uint256 *r, const struct arith_uint256 *a, unsigned int shift);
void arith_uint256_mul(struct arith_uint256 *r, const struct arith_uint256 *a, const struct arith_uint256 *b);
void arith_uint256_div(struct arith_uint256 *r, const struct arith_uint256 *a, const struct arith_uint256 *b);
unsigned int arith_uint256_bits(const struct arith_uint256 *a);

void arith_uint256_set_compact(struct arith_uint256 *a, uint32_t compact, bool *negative, bool *overflow);
uint32_t arith_uint256_get_compact(const struct arith_uint256 *a, bool negative);

void arith_to_uint256(struct uint256 *r, const struct arith_uint256 *a);
void uint256_to_arith(struct arith_uint256 *r, const struct uint256 *a);

void arith_uint256_set_hex(struct arith_uint256 *a, const char *psz);
void arith_uint256_get_hex(const struct arith_uint256 *a, char out[65]);

#endif
