/* Copyright (c) 2020 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CRYPTO_SHA3_H
#define BITCOIN_CRYPTO_SHA3_H

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#define SHA3_256_OUTPUT_SIZE 32
#define SHA3_256_RATE_BITS 1088
#define SHA3_256_RATE_BUFFERS (SHA3_256_RATE_BITS / 64)

#define SHA3_512_OUTPUT_SIZE 64
#define SHA3_512_RATE_BITS 576
#define SHA3_512_RATE_BUFFERS (SHA3_512_RATE_BITS / 64)

struct sha3_256_ctx {
    uint64_t state[25];
    unsigned char buffer[8];
    unsigned bufsize;
    unsigned pos;
};

struct sha3_512_ctx {
    uint64_t state[25];
    unsigned char buffer[8];
    unsigned bufsize;
    unsigned pos;
};

/* Streaming SHA3-256 (FIPS 202, 0x06 domain suffix; this is true SHA3, not
 * legacy Keccak). init → write* → finalize produces a 32-byte digest. `ctx`
 * carries an 8-byte little-endian absorb buffer, so write() may be called
 * any number of times with arbitrary lengths and the result equals hashing
 * the concatenation. finalize() consumes `ctx` (pads + permutes in place);
 * do not write() after it. This is the commitment hash used across the node
 * (UTXO/data-integrity roots, sha3_crypt keystream). */
void sha3_256_init(struct sha3_256_ctx *ctx);
void sha3_256_write(struct sha3_256_ctx *ctx, const unsigned char *data, size_t len);
void sha3_256_finalize(struct sha3_256_ctx *ctx, unsigned char *output);

void sha3_512_init(struct sha3_512_ctx *ctx);
void sha3_512_write(struct sha3_512_ctx *ctx, const unsigned char *data, size_t len);
void sha3_512_finalize(struct sha3_512_ctx *ctx, unsigned char output[64]);

/* One-shot convenience: SHA3-256/512 of `data[0..len)` in a single call
 * (exactly init+write+finalize). The `sha3_256`/`sha3_512` macros below let
 * call sites use the unprefixed names; the symbols are zcl_-prefixed to
 * avoid colliding with Tor's vendored keccak-tiny, which exports
 * sha3_256/sha3_512 with different signatures into the same binary. Always
 * reach for these macro names so the right (FIPS 202) implementation is
 * linked. */
void zcl_sha3_256(const unsigned char *data, size_t len, unsigned char output[32]);
void zcl_sha3_512(const unsigned char *data, size_t len, unsigned char output[64]);
#define sha3_256 zcl_sha3_256
#define sha3_512 zcl_sha3_512

/* HMAC-SHA3-512 for keyed authentication */
void hmac_sha3_512(const unsigned char *key, size_t key_len,
                   const unsigned char *data, size_t data_len,
                   unsigned char output[64]);

/* 4-way parallel SHA3-512 via AVX-512 (4x keystream throughput).
 * Generates 256 bytes per call. Falls back to sequential if no AVX-512. */
void sha3_512_x4(const uint8_t key[32], const uint8_t nonce[32],
                  uint64_t counter_base, uint8_t out[256]);
bool sha3_avx512_available(void);

#endif
