/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CRYPTO_SHA256_H
#define BITCOIN_CRYPTO_SHA256_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define SHA256_OUTPUT_SIZE 32
#define SHA256_BLOCK_SIZE 64

struct sha256_ctx {
    uint32_t s[8];
    unsigned char buf[SHA256_BLOCK_SIZE];
    size_t bytes;
};

/* Streaming SHA-256 (FIPS 180-4). init → write* → finalize yields the
 * 32-byte digest of the concatenation of all write() data; write() accepts
 * arbitrary lengths and may be called repeatedly. The compression transform
 * is SHA-NI when available and verified (see sha256_selftest), else portable
 * C — both produce identical output. */
void sha256_init(struct sha256_ctx *ctx);
void sha256_write(struct sha256_ctx *ctx, const unsigned char *data, size_t len);
void sha256_finalize(struct sha256_ctx *ctx, unsigned char hash[SHA256_OUTPUT_SIZE]);

/* Emit the current internal state (the 8 chaining words, big-endian) as the
 * 32-byte `hash` WITHOUT appending the SHA-256 length/0x80 padding. This is
 * NOT a SHA-256 digest — it exposes the raw compression-function output, for
 * midstate / single-block constructions that do their own padding (e.g.
 * BIP-340-style tagged hashing). When `enforce_compression` is nonzero it
 * requires exactly one 64-byte block to have been absorbed (ctx->bytes==64)
 * and returns -1 (logging) otherwise; with it 0 the check is skipped.
 * Returns 0 on success. */
int sha256_finalize_no_padding(struct sha256_ctx *ctx, unsigned char hash[SHA256_OUTPUT_SIZE],
                               int enforce_compression);

/* Runtime self-test: verifies SHA-NI matches portable. Returns true if OK.
 * Call once at startup. If false, SHA-NI is auto-disabled. */
bool sha256_selftest(void);

/* Returns "SHA-NI (hardware)" or "portable C" */
const char *sha256_implementation(void);

#endif
