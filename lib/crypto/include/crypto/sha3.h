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

/* 4-way parallel SHA3-512 via AVX-512 (4x keystream throughput).
 * Generates 256 bytes per call. Falls back to sequential if no AVX-512. */
void sha3_512_x4(const uint8_t key[32], const uint8_t nonce[32],
                  uint64_t counter_base, uint8_t out[256]);

/* Single-stream Keccak-f[1600] permutation — the core of SHA3-256/512.
 * `sha3_keccakf_scalar` is the always-available reference. `sha3_keccakf_avx512`
 * is an AVX-512 variant (avx512f/vl/dq) that is bit-identical to the scalar path
 * (proven by the `keccak_avx512` differential test group); it compiles into the
 * x86-64-v3 baseline via target attributes and is safe to invoke only when
 * `sha3_keccakf_avx512_available()` returns true (else it degrades to scalar).
 * All sha3_*_write/finalize route through the selected permutation. */
void sha3_keccakf_scalar(uint64_t st[25]);
void sha3_keccakf_avx512(uint64_t st[25]);
bool sha3_keccakf_avx512_available(void);

/* Permutation selection for the whole sha3_* surface. AVX-512 is honored only
 * when available (else scalar). AUTO installs the measured-fastest path (scalar
 * on this host class). Returns the impl actually installed (SHA3_IMPL_SCALAR or
 * SHA3_IMPL_AVX512). Not thread-safe against concurrent hashing — intended for
 * one-time init, and for the parity oracle / benchmark to force a path. */
enum sha3_impl { SHA3_IMPL_AUTO = -1, SHA3_IMPL_SCALAR = 0, SHA3_IMPL_AVX512 = 1 };
int sha3_select_impl(enum sha3_impl which);

/* 4-way batched SHA3-256: hash FOUR independent messages, produce FOUR digests.
 * msgs[k]/lens[k] describe message k (lens[k]==0 is allowed; msgs[k] may be NULL
 * only when lens[k]==0). out[k] receives the 32-byte SHA3-256 of message k,
 * byte-identical to sha3_256(msgs[k], lens[k], out[k]). This is the correct
 * shape for batching MANY independent hashes (manifest chunk hashes, Merkle
 * layer combines) — distinct from sha3_512_x4 (a keystream generator). Uses an
 * AVX-512 4-lane Keccak when the CPU supports it and the batch path is enabled;
 * the scalar fallback calls sha3_256 four times and is the differential-oracle
 * reference (`sha3_256_x4` test group). */
void sha3_256_x4(const uint8_t *const msgs[4], const size_t lens[4],
                 uint8_t out[4][32]);

/* Force/select the sha3_256_x4 implementation (parity oracle + benchmark).
 * SHA3_IMPL_AVX512 falls back to scalar when AVX-512 is unavailable; returns the
 * impl actually installed. Not thread-safe against concurrent batched hashing. */
int sha3_256_x4_select_impl(enum sha3_impl which);

#endif
