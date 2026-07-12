/* Copyright (c) 2020 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Based on https://github.com/mjosaarinen/tiny_sha3/blob/master/sha3.c
 * by Markku-Juhani O. Saarinen <mjos@iki.fi> */

#include "crypto/sha3.h"
#include "crypto/common.h"
#include "support/cleanse.h"
#include <string.h>

static inline uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

static void keccakf(uint64_t st[25])
{
    static const uint64_t RNDC[24] = {
        0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
        0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
        0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
        0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
        0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
        0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008
    };

    for (int round = 0; round < 24; ++round) {
        uint64_t bc0, bc1, bc2, bc3, bc4, t;

        bc0 = st[0] ^ st[5] ^ st[10] ^ st[15] ^ st[20];
        bc1 = st[1] ^ st[6] ^ st[11] ^ st[16] ^ st[21];
        bc2 = st[2] ^ st[7] ^ st[12] ^ st[17] ^ st[22];
        bc3 = st[3] ^ st[8] ^ st[13] ^ st[18] ^ st[23];
        bc4 = st[4] ^ st[9] ^ st[14] ^ st[19] ^ st[24];
        t = bc4 ^ rotl64(bc1, 1); st[0] ^= t; st[5] ^= t; st[10] ^= t; st[15] ^= t; st[20] ^= t;
        t = bc0 ^ rotl64(bc2, 1); st[1] ^= t; st[6] ^= t; st[11] ^= t; st[16] ^= t; st[21] ^= t;
        t = bc1 ^ rotl64(bc3, 1); st[2] ^= t; st[7] ^= t; st[12] ^= t; st[17] ^= t; st[22] ^= t;
        t = bc2 ^ rotl64(bc4, 1); st[3] ^= t; st[8] ^= t; st[13] ^= t; st[18] ^= t; st[23] ^= t;
        t = bc3 ^ rotl64(bc0, 1); st[4] ^= t; st[9] ^= t; st[14] ^= t; st[19] ^= t; st[24] ^= t;

        t = st[1];
        bc0 = st[10]; st[10] = rotl64(t, 1); t = bc0;
        bc0 = st[7]; st[7] = rotl64(t, 3); t = bc0;
        bc0 = st[11]; st[11] = rotl64(t, 6); t = bc0;
        bc0 = st[17]; st[17] = rotl64(t, 10); t = bc0;
        bc0 = st[18]; st[18] = rotl64(t, 15); t = bc0;
        bc0 = st[3]; st[3] = rotl64(t, 21); t = bc0;
        bc0 = st[5]; st[5] = rotl64(t, 28); t = bc0;
        bc0 = st[16]; st[16] = rotl64(t, 36); t = bc0;
        bc0 = st[8]; st[8] = rotl64(t, 45); t = bc0;
        bc0 = st[21]; st[21] = rotl64(t, 55); t = bc0;
        bc0 = st[24]; st[24] = rotl64(t, 2); t = bc0;
        bc0 = st[4]; st[4] = rotl64(t, 14); t = bc0;
        bc0 = st[15]; st[15] = rotl64(t, 27); t = bc0;
        bc0 = st[23]; st[23] = rotl64(t, 41); t = bc0;
        bc0 = st[19]; st[19] = rotl64(t, 56); t = bc0;
        bc0 = st[13]; st[13] = rotl64(t, 8); t = bc0;
        bc0 = st[12]; st[12] = rotl64(t, 25); t = bc0;
        bc0 = st[2]; st[2] = rotl64(t, 43); t = bc0;
        bc0 = st[20]; st[20] = rotl64(t, 62); t = bc0;
        bc0 = st[14]; st[14] = rotl64(t, 18); t = bc0;
        bc0 = st[22]; st[22] = rotl64(t, 39); t = bc0;
        bc0 = st[9]; st[9] = rotl64(t, 61); t = bc0;
        bc0 = st[6]; st[6] = rotl64(t, 20); t = bc0;
        st[1] = rotl64(t, 44);

        bc0 = st[0]; bc1 = st[1]; bc2 = st[2]; bc3 = st[3]; bc4 = st[4];
        st[0] = bc0 ^ (~bc1 & bc2) ^ RNDC[round];
        st[1] = bc1 ^ (~bc2 & bc3);
        st[2] = bc2 ^ (~bc3 & bc4);
        st[3] = bc3 ^ (~bc4 & bc0);
        st[4] = bc4 ^ (~bc0 & bc1);
        bc0 = st[5]; bc1 = st[6]; bc2 = st[7]; bc3 = st[8]; bc4 = st[9];
        st[5] = bc0 ^ (~bc1 & bc2);
        st[6] = bc1 ^ (~bc2 & bc3);
        st[7] = bc2 ^ (~bc3 & bc4);
        st[8] = bc3 ^ (~bc4 & bc0);
        st[9] = bc4 ^ (~bc0 & bc1);
        bc0 = st[10]; bc1 = st[11]; bc2 = st[12]; bc3 = st[13]; bc4 = st[14];
        st[10] = bc0 ^ (~bc1 & bc2);
        st[11] = bc1 ^ (~bc2 & bc3);
        st[12] = bc2 ^ (~bc3 & bc4);
        st[13] = bc3 ^ (~bc4 & bc0);
        st[14] = bc4 ^ (~bc0 & bc1);
        bc0 = st[15]; bc1 = st[16]; bc2 = st[17]; bc3 = st[18]; bc4 = st[19];
        st[15] = bc0 ^ (~bc1 & bc2);
        st[16] = bc1 ^ (~bc2 & bc3);
        st[17] = bc2 ^ (~bc3 & bc4);
        st[18] = bc3 ^ (~bc4 & bc0);
        st[19] = bc4 ^ (~bc0 & bc1);
        bc0 = st[20]; bc1 = st[21]; bc2 = st[22]; bc3 = st[23]; bc4 = st[24];
        st[20] = bc0 ^ (~bc1 & bc2);
        st[21] = bc1 ^ (~bc2 & bc3);
        st[22] = bc2 ^ (~bc3 & bc4);
        st[23] = bc3 ^ (~bc4 & bc0);
        st[24] = bc4 ^ (~bc0 & bc1);
    }
}

void sha3_256_init(struct sha3_256_ctx *ctx)
{
    ctx->bufsize = 0;
    ctx->pos = 0;
    memset(ctx->state, 0, sizeof(ctx->state));
}

void sha3_256_write(struct sha3_256_ctx *ctx, const unsigned char *data, size_t len)
{
    if (ctx->bufsize && ctx->bufsize + len >= sizeof(ctx->buffer)) {
        memcpy(ctx->buffer + ctx->bufsize, data, sizeof(ctx->buffer) - ctx->bufsize);
        data += sizeof(ctx->buffer) - ctx->bufsize;
        len -= sizeof(ctx->buffer) - ctx->bufsize;
        ctx->state[ctx->pos++] ^= ReadLE64(ctx->buffer);
        ctx->bufsize = 0;
        if (ctx->pos == SHA3_256_RATE_BUFFERS) {
            keccakf(ctx->state);
            ctx->pos = 0;
        }
    }
    while (len >= sizeof(ctx->buffer)) {
        ctx->state[ctx->pos++] ^= ReadLE64(data);
        data += 8;
        len -= 8;
        if (ctx->pos == SHA3_256_RATE_BUFFERS) {
            keccakf(ctx->state);
            ctx->pos = 0;
        }
    }
    if (len) {
        memcpy(ctx->buffer + ctx->bufsize, data, len);
        ctx->bufsize += len;
    }
}

void sha3_256_finalize(struct sha3_256_ctx *ctx, unsigned char *output)
{
    memset(ctx->buffer + ctx->bufsize, 0, sizeof(ctx->buffer) - ctx->bufsize);
    ctx->buffer[ctx->bufsize] ^= 0x06;
    ctx->state[ctx->pos] ^= ReadLE64(ctx->buffer);
    ctx->state[SHA3_256_RATE_BUFFERS - 1] ^= 0x8000000000000000ull;
    keccakf(ctx->state);
    for (unsigned i = 0; i < 4; ++i) {
        WriteLE64(output + 8 * i, ctx->state[i]);
    }
}

/* SHA3-512: rate = 576 bits (9 uint64s), capacity = 1024, output = 64 bytes */

void sha3_512_init(struct sha3_512_ctx *ctx)
{
    ctx->bufsize = 0;
    ctx->pos = 0;
    memset(ctx->state, 0, sizeof(ctx->state));
}

void sha3_512_write(struct sha3_512_ctx *ctx, const unsigned char *data, size_t len)
{
    if (ctx->bufsize && ctx->bufsize + len >= sizeof(ctx->buffer)) {
        memcpy(ctx->buffer + ctx->bufsize, data, sizeof(ctx->buffer) - ctx->bufsize);
        data += sizeof(ctx->buffer) - ctx->bufsize;
        len -= sizeof(ctx->buffer) - ctx->bufsize;
        ctx->state[ctx->pos++] ^= ReadLE64(ctx->buffer);
        ctx->bufsize = 0;
        if (ctx->pos == SHA3_512_RATE_BUFFERS) {
            keccakf(ctx->state);
            ctx->pos = 0;
        }
    }
    while (len >= sizeof(ctx->buffer)) {
        ctx->state[ctx->pos++] ^= ReadLE64(data);
        data += 8;
        len -= 8;
        if (ctx->pos == SHA3_512_RATE_BUFFERS) {
            keccakf(ctx->state);
            ctx->pos = 0;
        }
    }
    if (len) {
        memcpy(ctx->buffer + ctx->bufsize, data, len);
        ctx->bufsize += len;
    }
}

void sha3_512_finalize(struct sha3_512_ctx *ctx, unsigned char output[64])
{
    memset(ctx->buffer + ctx->bufsize, 0, sizeof(ctx->buffer) - ctx->bufsize);
    ctx->buffer[ctx->bufsize] ^= 0x06;
    ctx->state[ctx->pos] ^= ReadLE64(ctx->buffer);
    ctx->state[SHA3_512_RATE_BUFFERS - 1] ^= 0x8000000000000000ull;
    keccakf(ctx->state);
    for (unsigned i = 0; i < 8; ++i)
        WriteLE64(output + 8 * i, ctx->state[i]);
}

void zcl_sha3_256(const unsigned char *data, size_t len, unsigned char output[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, data, len);
    sha3_256_finalize(&ctx, output);
}

void zcl_sha3_512(const unsigned char *data, size_t len, unsigned char output[64])
{
    struct sha3_512_ctx ctx;
    sha3_512_init(&ctx);
    sha3_512_write(&ctx, data, len);
    sha3_512_finalize(&ctx, output);
}
