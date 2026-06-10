/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure C23 API for Sapling proving and verification.
 * Drop-in replacement for the Rust native C23 prover FFI.
 * All functions implemented in lib/zcash/src/zclassic_c23.c */

#ifndef ZCL_SAPLING_PROVER_H
#define ZCL_SAPLING_PROVER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Parameter loading (no-op — VKs loaded by sapling_init_params) */
void zclassic_init_zksnark_params(
    const uint8_t *spend_path, size_t spend_path_len,
    const char *spend_hash,
    const uint8_t *output_path, size_t output_path_len,
    const char *output_hash,
    const uint8_t *sprout_path, size_t sprout_path_len,
    const char *sprout_hash);

/* Verification context */
void *zclassic_sapling_verification_ctx_init(void);
void zclassic_sapling_verification_ctx_free(void *ctx);

bool zclassic_sapling_check_spend(
    void *ctx, const uint8_t *cv, const uint8_t *anchor,
    const uint8_t *nullifier, const uint8_t *rk,
    const uint8_t *zkproof, const uint8_t *spend_auth_sig,
    const uint8_t *sighash_value);

bool zclassic_sapling_check_output(
    void *ctx, const uint8_t *cv, const uint8_t *cm,
    const uint8_t *epk, const uint8_t *zkproof);

bool zclassic_sapling_final_check(
    void *ctx, int64_t value_balance,
    const uint8_t *binding_sig, const uint8_t *sighash_value);

/* Proving context */
void *zclassic_sapling_proving_ctx_init(void);
void zclassic_sapling_proving_ctx_free(void *ctx);

bool zclassic_sapling_output_proof(
    void *ctx,
    const unsigned char *esk,
    const unsigned char *diversifier,
    const unsigned char *pk_d,
    const unsigned char *rcm,
    const uint64_t value,
    unsigned char *cv,
    unsigned char *zkproof);

/* `witness` points to a caller-supplied merkle authentication
 * path with the fixed wire layout
 *     depth (1) || 32 × (sibling (32) || bit (1))  = 1057 bytes
 * `witness_len` is the buffer length and must be >= 1057; shorter
 * buffers are rejected without any read past the first byte. */
bool zclassic_sapling_spend_proof(
    void *ctx,
    const unsigned char *ak,
    const unsigned char *nsk,
    const unsigned char *diversifier,
    const unsigned char *rcm,
    const unsigned char *ar,
    const uint64_t value,
    const unsigned char *anchor,
    const unsigned char *witness,
    size_t witness_len,
    unsigned char *cv,
    unsigned char *rk,
    unsigned char *zkproof);

bool zclassic_sapling_binding_sig(
    const void *ctx,
    int64_t valueBalance,
    const unsigned char *sighash,
    unsigned char *result);

#endif
