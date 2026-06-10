/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include <stdio.h>
#include <ctype.h>

static void blob_get_hex(const uint8_t *data, unsigned int width, char *out)
{
    for (unsigned int i = 0; i < width; i++)
        sprintf(out + i * 2, "%02x", data[width - i - 1]);
    out[width * 2] = '\0';
}

static void blob_set_hex(uint8_t *data, unsigned int width, const char *psz)
{
    memset(data, 0, width);

    while (isspace((unsigned char)*psz))
        psz++;

    if (psz[0] == '0' && (psz[1] == 'x' || psz[1] == 'X'))
        psz += 2;

    const char *pbegin = psz;
    while (HexDigit(*psz) != -1)
        psz++;
    psz--;

    uint8_t *p1 = data;
    uint8_t *pend = p1 + width;
    while (psz >= pbegin && p1 < pend) {
        *p1 = (uint8_t)HexDigit(*psz--);
        if (psz >= pbegin) {
            *p1 |= (uint8_t)((uint8_t)HexDigit(*psz--) << 4);
            p1++;
        }
    }
}

void uint256_get_hex(const struct uint256 *v, char out[65])
{
    blob_get_hex(v->data, 32, out);
}

void uint256_set_hex(struct uint256 *v, const char *psz)
{
    blob_set_hex(v->data, 32, psz);
}

void uint160_get_hex(const struct uint160 *v, char out[41])
{
    blob_get_hex(v->data, 20, out);
}

void uint160_set_hex(struct uint160 *v, const char *psz)
{
    blob_set_hex(v->data, 20, psz);
}

static inline void hash_mix(uint32_t *a, uint32_t *b, uint32_t *c)
{
    *a -= *c; *a ^= ((*c << 4) | (*c >> 28)); *c += *b;
    *b -= *a; *b ^= ((*a << 6) | (*a >> 26)); *a += *c;
    *c -= *b; *c ^= ((*b << 8) | (*b >> 24)); *b += *a;
    *a -= *c; *a ^= ((*c << 16) | (*c >> 16)); *c += *b;
    *b -= *a; *b ^= ((*a << 19) | (*a >> 13)); *a += *c;
    *c -= *b; *c ^= ((*b << 4) | (*b >> 28)); *b += *a;
}

static inline void hash_final(uint32_t *a, uint32_t *b, uint32_t *c)
{
    *c ^= *b; *c -= ((*b << 14) | (*b >> 18));
    *a ^= *c; *a -= ((*c << 11) | (*c >> 21));
    *b ^= *a; *b -= ((*a << 25) | (*a >> 7));
    *c ^= *b; *c -= ((*b << 16) | (*b >> 16));
    *a ^= *c; *a -= ((*c << 4) | (*c >> 28));
    *b ^= *a; *b -= ((*a << 14) | (*a >> 18));
    *c ^= *b; *c -= ((*b << 24) | (*b >> 8));
}

uint64_t uint256_get_hash(const struct uint256 *v, const struct uint256 *salt)
{
    uint32_t a, b, c;
    const uint32_t *pn = (const uint32_t *)v->data;
    const uint32_t *salt_pn = (const uint32_t *)salt->data;
    a = b = c = 0xdeadbeef + 32;

    a += pn[0] ^ salt_pn[0];
    b += pn[1] ^ salt_pn[1];
    c += pn[2] ^ salt_pn[2];
    hash_mix(&a, &b, &c);
    a += pn[3] ^ salt_pn[3];
    b += pn[4] ^ salt_pn[4];
    c += pn[5] ^ salt_pn[5];
    hash_mix(&a, &b, &c);
    a += pn[6] ^ salt_pn[6];
    b += pn[7] ^ salt_pn[7];
    hash_final(&a, &b, &c);

    return (((uint64_t)b) << 32) | c;
}
