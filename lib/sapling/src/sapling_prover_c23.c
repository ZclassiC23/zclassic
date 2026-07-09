/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Stable C facade for Sapling proving and verification.
 *
 * Consensus verification remains in the independent C23 implementation in
 * sapling.c. Wallet-side proving delegates to the SHA256-pinned
 * librustzcash revision used by canonical ZClassic. The backend is not made
 * available to callers until a real Spend + Output + binding-signature bundle
 * produced by Rust is accepted by the C23 consensus verifier.
 */

#include "sapling/sapling_prover.h"

#include "sapling/fr.h"
#include "sapling/incremental_merkle_tree.h"
#include "sapling/sapling.h"
#include "sapling/sapling_circuit.h"
#include "support/cleanse.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SAPLING_COMPACT_WITNESS_LEN \
    ((size_t)(1 + SAPLING_MERKLE_DEPTH * 33))
#define RUSTZCASH_WITNESS_LEN \
    ((size_t)(1 + SAPLING_MERKLE_DEPTH * 33 + 8))

/* Minimal C declarations for the pinned upstream static library. Keeping
 * these private prevents the third-party header (which is C++-oriented) from
 * leaking across the repository's public C API. */
extern void librustzcash_init_zksnark_params(
    const uint8_t *spend_path, size_t spend_path_len,
    const char *spend_hash,
    const uint8_t *output_path, size_t output_path_len,
    const char *output_hash,
    const uint8_t *sprout_path, size_t sprout_path_len,
    const char *sprout_hash);

extern void *librustzcash_sapling_proving_ctx_init(void);
extern void librustzcash_sapling_proving_ctx_free(void *ctx);
extern bool librustzcash_sapling_output_proof(
    void *ctx,
    const uint8_t *esk,
    const uint8_t *diversifier,
    const uint8_t *pk_d,
    const uint8_t *rcm,
    uint64_t value,
    uint8_t *cv,
    uint8_t *zkproof);
extern bool librustzcash_sapling_spend_proof(
    void *ctx,
    const uint8_t *ak,
    const uint8_t *nsk,
    const uint8_t *diversifier,
    const uint8_t *rcm,
    const uint8_t *ar,
    uint64_t value,
    const uint8_t *anchor,
    const uint8_t *witness,
    uint8_t *cv,
    uint8_t *rk,
    uint8_t *zkproof);
extern bool librustzcash_sapling_binding_sig(
    const void *ctx,
    int64_t value_balance,
    const uint8_t *sighash,
    uint8_t *result);

enum prover_state {
    PROVER_UNINITIALIZED = 0,
    PROVER_BACKEND_INITIALIZED,
    PROVER_SELF_TESTING,
    PROVER_READY,
    PROVER_FAILED,
};

static _Atomic int g_prover_state = PROVER_UNINITIALIZED;
static _Atomic bool g_rust_params_initialized = false;

static const char *prover_state_name(int state)
{
    switch (state) {
    case PROVER_UNINITIALIZED: return "params_not_initialized";
    case PROVER_BACKEND_INITIALIZED: return "self_test_pending";
    case PROVER_SELF_TESTING: return "self_test_running";
    case PROVER_READY: return "ready";
    case PROVER_FAILED: return "self_test_failed";
    default: return "invalid_state";
    }
}

bool zclassic_sapling_prover_is_ready(void)
{
    return atomic_load(&g_prover_state) == PROVER_READY;
}

const char *zclassic_sapling_prover_status(void)
{
    return prover_state_name(atomic_load(&g_prover_state));
}

const char *zclassic_sapling_prover_backend(void)
{
    return "librustzcash-06da3b9ac8f2";
}

/* --- Verification: the consensus C23 implementation -------------------- */

void *zclassic_sapling_verification_ctx_init(void)
{
    struct sapling_verification_ctx *ctx =
        zcl_calloc(1, sizeof(*ctx), "sapling_verify_ctx");
    if (ctx)
        sapling_verification_ctx_init(ctx);
    return ctx;
}

void zclassic_sapling_verification_ctx_free(void *ctx)
{
    free(ctx);
}

bool zclassic_sapling_check_spend(
    void *ctx, const uint8_t *cv, const uint8_t *anchor,
    const uint8_t *nullifier, const uint8_t *rk,
    const uint8_t *zkproof, const uint8_t *spend_auth_sig,
    const uint8_t *sighash_value)
{
    return sapling_check_spend(ctx, cv, anchor, nullifier, rk,
                               zkproof, spend_auth_sig, sighash_value);
}

bool zclassic_sapling_check_output(
    void *ctx, const uint8_t *cv, const uint8_t *cm,
    const uint8_t *epk, const uint8_t *zkproof)
{
    return sapling_check_output(ctx, cv, cm, epk, zkproof);
}

bool zclassic_sapling_final_check(
    void *ctx, int64_t value_balance,
    const uint8_t *binding_sig, const uint8_t *sighash_value)
{
    return sapling_final_check(ctx, value_balance, binding_sig,
                               sighash_value);
}

/* --- Witness ABI conversion --------------------------------------------- */

bool sapling_spend_parse_witness(const uint8_t *witness,
                                 size_t witness_len,
                                 struct sapling_spend_witness *wit)
{
    if (!witness || !wit)
        LOG_FAIL("sapling_prover", "parse_witness: NULL input");
    if (witness_len < SAPLING_COMPACT_WITNESS_LEN)
        LOG_FAIL("sapling_prover",
                 "parse_witness: short input: got=%zu need=%zu",
                 witness_len, SAPLING_COMPACT_WITNESS_LEN);
    if (witness[0] != SAPLING_MERKLE_DEPTH)
        LOG_FAIL("sapling_prover",
                 "parse_witness: depth=%u expected=%u",
                 witness[0], SAPLING_MERKLE_DEPTH);

    for (size_t i = 0; i < SAPLING_MERKLE_DEPTH; i++) {
        const size_t off = 1 + i * 33;
        memcpy(wit->auth_path[i], witness + off, 32);
        if (witness[off + 32] > 1)
            LOG_FAIL("sapling_prover",
                     "parse_witness: invalid direction byte at level=%zu",
                     i);
        wit->auth_path_bits[i] = witness[off + 32] != 0;
    }
    return true;
}

/* zclassic23's compact witness is leaf-to-root and carries one direction
 * byte per sibling. The legacy Zcash Rust ABI serializes the sibling vector
 * root-to-leaf (each item prefixed with its byte length) and appends the leaf
 * position as little-endian u64. Convert only after fully validating bounds
 * and direction bytes; upstream contains asserts for this legacy layout. */
static bool witness_to_rust(const uint8_t *witness, size_t witness_len,
                            uint8_t rust_witness[RUSTZCASH_WITNESS_LEN])
{
    struct sapling_spend_witness parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (!sapling_spend_parse_witness(witness, witness_len, &parsed))
        return false;

    size_t off = 0;
    rust_witness[off++] = SAPLING_MERKLE_DEPTH;
    for (size_t stream_i = 0; stream_i < SAPLING_MERKLE_DEPTH;
         stream_i++) {
        const size_t level = SAPLING_MERKLE_DEPTH - 1 - stream_i;
        rust_witness[off++] = 32;
        memcpy(rust_witness + off, parsed.auth_path[level], 32);
        off += 32;
    }

    uint64_t position = 0;
    for (size_t i = 0; i < SAPLING_MERKLE_DEPTH; i++) {
        if (parsed.auth_path_bits[i])
            position |= UINT64_C(1) << i;
    }
    for (size_t i = 0; i < 8; i++)
        rust_witness[off++] = (uint8_t)(position >> (8 * i));

    memory_cleanse(&parsed, sizeof(parsed));
    if (off != RUSTZCASH_WITNESS_LEN)
        LOG_FAIL("sapling_prover",
                 "witness conversion size mismatch: got=%zu expected=%zu",
                 off, RUSTZCASH_WITNESS_LEN);
    return true;
}

static bool rust_spend_proof_raw(
    void *ctx,
    const uint8_t *ak,
    const uint8_t *nsk,
    const uint8_t *diversifier,
    const uint8_t *rcm,
    const uint8_t *ar,
    uint64_t value,
    const uint8_t *anchor,
    const uint8_t *witness,
    size_t witness_len,
    uint8_t *cv,
    uint8_t *rk,
    uint8_t *zkproof)
{
    uint8_t rust_witness[RUSTZCASH_WITNESS_LEN];
    memset(rust_witness, 0, sizeof(rust_witness));
    if (!witness_to_rust(witness, witness_len, rust_witness))
        return false;

    const bool ok = librustzcash_sapling_spend_proof(
        ctx, ak, nsk, diversifier, rcm, ar, value, anchor,
        rust_witness, cv, rk, zkproof);
    memory_cleanse(rust_witness, sizeof(rust_witness));
    return ok;
}

/* --- Proving context and calls ------------------------------------------ */

void *zclassic_sapling_proving_ctx_init(void)
{
    if (!zclassic_sapling_prover_is_ready())
        LOG_NULL("sapling_prover",
                 "proving disabled: backend=%s status=%s",
                 zclassic_sapling_prover_backend(),
                 zclassic_sapling_prover_status());

    void *ctx = librustzcash_sapling_proving_ctx_init();
    if (!ctx)
        LOG_NULL("sapling_prover", "Rust proving context allocation failed");
    return ctx;
}

void zclassic_sapling_proving_ctx_free(void *ctx)
{
    if (ctx)
        librustzcash_sapling_proving_ctx_free(ctx);
}

bool zclassic_sapling_output_proof(
    void *ctx,
    const unsigned char *esk,
    const unsigned char *diversifier,
    const unsigned char *pk_d,
    const unsigned char *rcm,
    uint64_t value,
    unsigned char *cv,
    unsigned char *zkproof)
{
    if (!zclassic_sapling_prover_is_ready())
        LOG_FAIL("sapling_prover",
                 "output proof disabled: status=%s",
                 zclassic_sapling_prover_status());
    if (!ctx || !esk || !diversifier || !pk_d || !rcm || !cv || !zkproof)
        LOG_FAIL("sapling_prover", "output proof: NULL argument");
    if (!librustzcash_sapling_output_proof(
            ctx, esk, diversifier, pk_d, rcm, value, cv, zkproof))
        LOG_FAIL("sapling_prover", "Rust output proof construction failed");
    return true;
}

bool zclassic_sapling_spend_proof(
    void *ctx,
    const unsigned char *ak,
    const unsigned char *nsk,
    const unsigned char *diversifier,
    const unsigned char *rcm,
    const unsigned char *ar,
    uint64_t value,
    const unsigned char *anchor,
    const unsigned char *witness,
    size_t witness_len,
    unsigned char *cv,
    unsigned char *rk,
    unsigned char *zkproof)
{
    if (!zclassic_sapling_prover_is_ready())
        LOG_FAIL("sapling_prover",
                 "spend proof disabled: status=%s",
                 zclassic_sapling_prover_status());
    if (!ctx || !ak || !nsk || !diversifier || !rcm || !ar || !anchor ||
        !witness || !cv || !rk || !zkproof)
        LOG_FAIL("sapling_prover", "spend proof: NULL argument");
    if (!rust_spend_proof_raw(ctx, ak, nsk, diversifier, rcm, ar,
                              value, anchor, witness, witness_len,
                              cv, rk, zkproof))
        LOG_FAIL("sapling_prover", "Rust spend proof construction failed");
    return true;
}

bool zclassic_sapling_binding_sig(
    const void *ctx, int64_t value_balance,
    const unsigned char *sighash, unsigned char *result)
{
    if (!zclassic_sapling_prover_is_ready())
        LOG_FAIL("sapling_prover",
                 "binding signature disabled: status=%s",
                 zclassic_sapling_prover_status());
    if (!ctx || !sighash || !result)
        LOG_FAIL("sapling_prover", "binding signature: NULL argument");
    if (!librustzcash_sapling_binding_sig(
            ctx, value_balance, sighash, result))
        LOG_FAIL("sapling_prover",
                 "Rust binding signature consistency check failed");
    return true;
}

/* --- Positive prover -> consensus verifier capability gate -------------- */

static bool find_diversifier(uint8_t diversifier[11])
{
    memset(diversifier, 0, 11);
    for (unsigned int i = 0; i < 256; i++) {
        diversifier[0] = (uint8_t)i;
        if (sapling_check_diversifier(diversifier))
            return true;
    }
    LOG_FAIL("sapling_prover", "self-test could not find a diversifier");
}

static bool self_test_bundle(void)
{
    const uint64_t value = UINT64_C(12345);
    bool ok = false;
    const char *failed_at = "rng";
    void *pctx = NULL;

    uint8_t ask[32] = {0}, nsk[32] = {0}, ak[32] = {0}, nk[32] = {0};
    uint8_t ivk[32] = {0}, diversifier[11] = {0}, pk_d[32] = {0};
    uint8_t spend_rcm[32] = {0}, ar[32] = {0}, nullifier[32] = {0};
    uint8_t spend_cv[32] = {0}, rk[32] = {0}, spend_proof[192] = {0};
    uint8_t spend_cm[32] = {0}, spend_sig[64] = {0};
    uint8_t output_rcm[32] = {0}, esk[32] = {0};
    uint8_t output_cv[32] = {0}, output_cm[32] = {0};
    uint8_t epk[32] = {0}, output_proof[192] = {0};
    uint8_t binding_sig[64] = {0};
    uint8_t sighash[32] = {0};
    uint8_t compact_witness[SAPLING_COMPACT_WITNESS_LEN];
    memset(compact_witness, 0, sizeof(compact_witness));

    if (!sapling_generate_r(ask) || !sapling_generate_r(nsk) ||
        !sapling_generate_r(spend_rcm) || !sapling_generate_r(ar) ||
        !sapling_generate_r(output_rcm) || !sapling_generate_r(esk))
        goto cleanup;
    failed_at = "diversifier";
    if (!find_diversifier(diversifier))
        goto cleanup;

    sapling_ask_to_ak(ask, ak);
    sapling_nsk_to_nk(nsk, nk);
    sapling_crh_ivk(ak, nk, ivk);
    failed_at = "spend_note";
    if (!sapling_ivk_to_pkd(ivk, diversifier, pk_d) ||
        !sapling_compute_cm(diversifier, pk_d, value,
                            spend_rcm, spend_cm))
        goto cleanup;

    struct incremental_merkle_tree tree;
    struct incremental_witness witness;
    struct uint256 leaf;
    struct uint256 anchor;
    memcpy(leaf.data, spend_cm, 32);
    sapling_tree_init(&tree);
    incremental_tree_append(&tree, &leaf);
    incremental_witness_init(&witness, &tree);
    incremental_witness_root(&witness, &anchor);
    size_t compact_len = 0;
    failed_at = "merkle_witness";
    if (!incremental_witness_merkle_path(
            &witness, compact_witness, &compact_len) ||
        compact_len != sizeof(compact_witness))
        goto cleanup;

    failed_at = "proving_ctx";
    pctx = librustzcash_sapling_proving_ctx_init();
    if (!pctx)
        goto cleanup;
    failed_at = "spend_proof";
    if (!rust_spend_proof_raw(
            pctx, ak, nsk, diversifier, spend_rcm, ar, value,
            anchor.data, compact_witness, compact_len,
            spend_cv, rk, spend_proof))
        goto cleanup;
    failed_at = "nullifier";
    if (!sapling_compute_nf(diversifier, pk_d, value, spend_rcm,
                            ak, nk, 0, nullifier))
        goto cleanup;

    struct fs ask_fs, ar_fs, rsk_fs;
    uint8_t rsk[32] = {0};
    fs_from_bytes(&ask_fs, ask);
    fs_from_bytes(&ar_fs, ar);
    fs_add(&rsk_fs, &ask_fs, &ar_fs);
    fs_to_bytes(rsk, &rsk_fs);
    memset(sighash, 0x5a, sizeof(sighash));
    failed_at = "spend_signature";
    if (!redjubjub_sign(rsk, sighash, sizeof(sighash), spend_sig, 5)) {
        memory_cleanse(rsk, sizeof(rsk));
        memory_cleanse(&ask_fs, sizeof(ask_fs));
        memory_cleanse(&ar_fs, sizeof(ar_fs));
        memory_cleanse(&rsk_fs, sizeof(rsk_fs));
        goto cleanup;
    }
    memory_cleanse(rsk, sizeof(rsk));
    memory_cleanse(&ask_fs, sizeof(ask_fs));
    memory_cleanse(&ar_fs, sizeof(ar_fs));
    memory_cleanse(&rsk_fs, sizeof(rsk_fs));

    failed_at = "output_bundle";
    if (!librustzcash_sapling_output_proof(
            pctx, esk, diversifier, pk_d, output_rcm, value,
            output_cv, output_proof) ||
        !sapling_compute_cm(diversifier, pk_d, value,
                            output_rcm, output_cm) ||
        !sapling_ka_derivepublic(diversifier, esk, epk) ||
        !librustzcash_sapling_binding_sig(
            pctx, 0, sighash, binding_sig))
        goto cleanup;

    struct sapling_verification_ctx vctx;
    sapling_verification_ctx_init(&vctx);
    failed_at = "consensus_verifier";
    if (!sapling_check_spend(&vctx, spend_cv, anchor.data, nullifier,
                             rk, spend_proof, spend_sig, sighash) ||
        !sapling_check_output(&vctx, output_cv, output_cm, epk,
                              output_proof) ||
        !sapling_final_check(&vctx, 0, binding_sig, sighash))
        goto cleanup;

    ok = true;

cleanup:
    if (!ok)
        LOG_WARN("sapling_prover", "positive self-test failed at stage=%s",
                 failed_at);
    if (pctx)
        librustzcash_sapling_proving_ctx_free(pctx);
    memory_cleanse(ask, sizeof(ask));
    memory_cleanse(nsk, sizeof(nsk));
    memory_cleanse(ak, sizeof(ak));
    memory_cleanse(nk, sizeof(nk));
    memory_cleanse(ivk, sizeof(ivk));
    memory_cleanse(pk_d, sizeof(pk_d));
    memory_cleanse(spend_rcm, sizeof(spend_rcm));
    memory_cleanse(ar, sizeof(ar));
    memory_cleanse(output_rcm, sizeof(output_rcm));
    memory_cleanse(esk, sizeof(esk));
    memory_cleanse(compact_witness, sizeof(compact_witness));
    return ok;
}

bool zclassic_sapling_prover_run_self_test(void)
{
    int expected = PROVER_BACKEND_INITIALIZED;
    if (!atomic_compare_exchange_strong(
            &g_prover_state, &expected, PROVER_SELF_TESTING)) {
        if (expected == PROVER_READY)
            return true;
        LOG_FAIL("sapling_prover",
                 "cannot run self-test from state=%s",
                 prover_state_name(expected));
    }

    if (!self_test_bundle()) {
        atomic_store(&g_prover_state, PROVER_FAILED);
        LOG_FAIL("sapling_prover",
                 "positive Spend/Output/binding prover->verifier gate failed; proving remains disabled");
    }

    atomic_store(&g_prover_state, PROVER_READY);
    LOG_INFO("sapling_prover",
             "positive Spend/Output/binding prover->C23-verifier gate passed; backend=%s",
             zclassic_sapling_prover_backend());
    return true;
}

void zclassic_init_zksnark_params(
    const uint8_t *spend_path, size_t spend_path_len,
    const char *spend_hash,
    const uint8_t *output_path, size_t output_path_len,
    const char *output_hash,
    const uint8_t *sprout_path, size_t sprout_path_len,
    const char *sprout_hash)
{
    if (atomic_load(&g_rust_params_initialized))
        return;
    if (!spend_path || spend_path_len == 0 || !spend_hash ||
        !output_path || output_path_len == 0 || !output_hash ||
        !sprout_path || sprout_path_len == 0 || !sprout_hash) {
        atomic_store(&g_prover_state, PROVER_FAILED);
        LOG_WARN("sapling_prover",
                 "Rust parameter initialization rejected incomplete paths/hashes");
        return;
    }

    librustzcash_init_zksnark_params(
        spend_path, spend_path_len, spend_hash,
        output_path, output_path_len, output_hash,
        sprout_path, sprout_path_len, sprout_hash);
    atomic_store(&g_rust_params_initialized, true);
    atomic_store(&g_prover_state, PROVER_BACKEND_INITIALIZED);
}
