/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Ed25519 signature verification — pure C23 implementation.
 * Replaces libsodium crypto_sign_verify_detached. */

#ifndef ZCL_CRYPTO_ED25519_H
#define ZCL_CRYPTO_ED25519_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Verify an Ed25519 signature (RFC 8032 Ed25519, SHA-512 hash). Returns
 * true IFF the 64-byte `sig` is a valid signature by `pk` over `msg`.
 *
 * Verify-only (no signing in-tree); consensus uses this for JoinSplit
 * ed25519 signatures, where exact agreement with zcashd is mandatory.
 * A true return certifies ALL of:
 *   - `pk` is not the all-zero identity point (rejected up front);
 *   - S (sig[32..63]) is canonical, i.e. S < L the group order — malleable
 *     high-S signatures are rejected BEFORE any point math (RFC 8032
 *     §5.1.7; a non-canonical S would otherwise split consensus);
 *   - `pk` decompresses to a valid curve point, and [S]B == R + [h]A where
 *     h = SHA-512(R || pk || msg) mod L, compared via a constant-time
 *     XOR-accumulate (no early-exit memcmp).
 * Any failed check returns false (it logs the reason; it never aborts).
 * `msg_len` may be 0. */
bool ed25519_verify(const uint8_t sig[64],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t pk[32]);

#endif
