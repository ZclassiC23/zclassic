/* Copyright (c) 2013-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "core/hash.h"
#include "crypto/common.h"
#include <string.h>

void hash256(const unsigned char *data, size_t len, unsigned char hash[SHA256_OUTPUT_SIZE])
{
    unsigned char buf[SHA256_OUTPUT_SIZE];
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_write(&ctx, data, len);
    sha256_finalize(&ctx, buf);
    sha256_init(&ctx);
    sha256_write(&ctx, buf, SHA256_OUTPUT_SIZE);
    sha256_finalize(&ctx, hash);
}

void hash160(const unsigned char *data, size_t len, unsigned char hash[RIPEMD160_OUTPUT_SIZE])
{
    unsigned char buf[SHA256_OUTPUT_SIZE];
    struct sha256_ctx sha;
    sha256_init(&sha);
    sha256_write(&sha, data, len);
    sha256_finalize(&sha, buf);
    struct ripemd160_ctx rmd;
    ripemd160_init(&rmd);
    ripemd160_write(&rmd, buf, SHA256_OUTPUT_SIZE);
    ripemd160_finalize(&rmd, hash);
}

static inline uint32_t rotl32(uint32_t x, int8_t r)
{
    return (x << r) | (x >> (32 - r));
}

uint32_t murmur_hash3(uint32_t seed, const unsigned char *data, size_t len)
{
    uint32_t h1 = seed;

    if (len > 0) {
        const uint32_t c1 = 0xcc9e2d51;
        const uint32_t c2 = 0x1b873593;
        const int nblocks = (int)(len / 4);
        const uint8_t *blocks = data + nblocks * 4;

        for (int i = -nblocks; i; i++) {
            uint32_t k1 = ReadLE32(blocks + i * 4);
            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;
            h1 ^= k1;
            h1 = rotl32(h1, 13);
            h1 = h1 * 5 + 0xe6546b64;
        }

        const uint8_t *tail = data + nblocks * 4;
        uint32_t k1 = 0;
        switch (len & 3) {
        case 3: k1 ^= (uint32_t)tail[2] << 16; /* fallthrough */
        case 2: k1 ^= (uint32_t)tail[1] << 8;  /* fallthrough */
        case 1: k1 ^= tail[0];
                k1 *= c1;
                k1 = rotl32(k1, 15);
                k1 *= c2;
                h1 ^= k1;
        }
    }

    h1 ^= (uint32_t)len;
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6b;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35;
    h1 ^= h1 >> 16;

    return h1;
}

void bip32_hash(const unsigned char chain_code[32], uint32_t child,
                unsigned char header, const unsigned char data[32],
                unsigned char output[64])
{
    unsigned char num[4];
    num[0] = (child >> 24) & 0xFF;
    num[1] = (child >> 16) & 0xFF;
    num[2] = (child >>  8) & 0xFF;
    num[3] = (child >>  0) & 0xFF;

    struct hmac_sha512_ctx ctx;
    hmac_sha512_init(&ctx, chain_code, 32);
    hmac_sha512_write(&ctx, &header, 1);
    hmac_sha512_write(&ctx, data, 32);
    hmac_sha512_write(&ctx, num, 4);
    hmac_sha512_finalize(&ctx, output);
}
