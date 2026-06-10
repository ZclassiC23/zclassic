/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "crypto/common.h"
#include <assert.h>
#include <string.h>

void arith_uint256_shl(struct arith_uint256 *r, const struct arith_uint256 *a, unsigned int shift)
{
    struct arith_uint256 tmp = *a;
    memset(r->pn, 0, sizeof(r->pn));
    int k = shift / 32;
    shift = shift % 32;
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++) {
        if (i + k + 1 < ARITH_UINT256_WIDTH && shift != 0)
            r->pn[i + k + 1] |= (tmp.pn[i] >> (32 - shift));
        if (i + k < ARITH_UINT256_WIDTH)
            r->pn[i + k] |= (tmp.pn[i] << shift);
    }
}

void arith_uint256_shr(struct arith_uint256 *r, const struct arith_uint256 *a, unsigned int shift)
{
    struct arith_uint256 tmp = *a;
    memset(r->pn, 0, sizeof(r->pn));
    int k = shift / 32;
    shift = shift % 32;
    for (int i = 0; i < ARITH_UINT256_WIDTH; i++) {
        if (i - k - 1 >= 0 && shift != 0)
            r->pn[i - k - 1] |= (tmp.pn[i] << (32 - shift));
        if (i - k >= 0)
            r->pn[i - k] |= (tmp.pn[i] >> shift);
    }
}

void arith_uint256_mul(struct arith_uint256 *r,
                        const struct arith_uint256 *a,
                        const struct arith_uint256 *b)
{
    memset(r->pn, 0, sizeof(r->pn));
    for (int j = 0; j < ARITH_UINT256_WIDTH; j++) {
        uint64_t carry = 0;
        for (int i = 0; i + j < ARITH_UINT256_WIDTH; i++) {
            uint64_t n = carry + r->pn[i + j] + (uint64_t)a->pn[j] * b->pn[i];
            r->pn[i + j] = n & 0xffffffff;
            carry = n >> 32;
        }
    }
}

void arith_uint256_div(struct arith_uint256 *r,
                        const struct arith_uint256 *a,
                        const struct arith_uint256 *b)
{
    struct arith_uint256 div = *b;
    struct arith_uint256 num = *a;
    memset(r->pn, 0, sizeof(r->pn));
    int num_bits = arith_uint256_bits(&num);
    int div_bits = arith_uint256_bits(&div);
    if (div_bits == 0)
        return;
    if (div_bits > num_bits)
        return;
    int shift = num_bits - div_bits;
    arith_uint256_shl(&div, &div, shift);
    while (shift >= 0) {
        if (arith_uint256_compare(&num, &div) >= 0) {
            arith_uint256_sub(&num, &num, &div);
            r->pn[shift / 32] |= (1u << (shift & 31));
        }
        arith_uint256_shr(&div, &div, 1);
        shift--;
    }
}

unsigned int arith_uint256_bits(const struct arith_uint256 *a)
{
    for (int pos = ARITH_UINT256_WIDTH - 1; pos >= 0; pos--) {
        if (a->pn[pos]) {
            for (int nbits = 31; nbits > 0; nbits--) {
                if (a->pn[pos] & (1u << nbits))
                    return 32 * pos + nbits + 1;
            }
            return 32 * pos + 1;
        }
    }
    return 0;
}

void arith_uint256_set_compact(struct arith_uint256 *a, uint32_t compact,
                                bool *negative, bool *overflow)
{
    int size = compact >> 24;
    uint32_t word = compact & 0x007fffff;
    if (size <= 3) {
        word >>= 8 * (3 - size);
        arith_uint256_set_u64(a, word);
    } else {
        arith_uint256_set_u64(a, word);
        arith_uint256_shl(a, a, 8 * (size - 3));
    }
    if (negative)
        *negative = word != 0 && (compact & 0x00800000) != 0;
    if (overflow)
        *overflow = word != 0 && ((size > 34) ||
                                   (word > 0xff && size > 33) ||
                                   (word > 0xffff && size > 32));
}

uint32_t arith_uint256_get_compact(const struct arith_uint256 *a, bool negative)
{
    int size = (arith_uint256_bits(a) + 7) / 8;
    uint32_t compact = 0;
    if (size <= 3) {
        compact = (uint32_t)(arith_uint256_get_low64(a) << (8 * (3 - size)));
    } else {
        struct arith_uint256 bn;
        arith_uint256_shr(&bn, a, 8 * (size - 3));
        compact = (uint32_t)arith_uint256_get_low64(&bn);
    }
    if (compact & 0x00800000) {
        compact >>= 8;
        size++;
    }
    assert((compact & ~0x007fffffU) == 0);
    assert(size < 256);
    compact |= (uint32_t)size << 24;
    compact |= (negative && (compact & 0x007fffff) ? 0x00800000U : 0);
    return compact;
}

void arith_to_uint256(struct uint256 *r, const struct arith_uint256 *a)
{
    for (int x = 0; x < ARITH_UINT256_WIDTH; x++)
        WriteLE32(r->data + x * 4, a->pn[x]);
}

void uint256_to_arith(struct arith_uint256 *r, const struct uint256 *a)
{
    for (int x = 0; x < ARITH_UINT256_WIDTH; x++)
        r->pn[x] = ReadLE32(a->data + x * 4);
}

void arith_uint256_set_hex(struct arith_uint256 *a, const char *psz)
{
    struct uint256 u;
    uint256_set_hex(&u, psz);
    uint256_to_arith(a, &u);
}

void arith_uint256_get_hex(const struct arith_uint256 *a, char out[65])
{
    struct uint256 u;
    arith_to_uint256(&u, a);
    uint256_get_hex(&u, out);
}
