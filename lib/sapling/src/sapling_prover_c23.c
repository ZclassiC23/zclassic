/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Pure C23 replacements for native C23 prover FFI.
 * Delegates to native Sapling/Groth16 implementations. */

#include "sapling/sapling_prover.h"
#include "sapling/sapling.h"
#include "sapling/sapling_circuit.h"
#include "sapling/fr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "support/cleanse.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

/* --- Proving key file cache --- */

static uint8_t *g_spend_pk_data = NULL;
static size_t g_spend_pk_len = 0;
static uint8_t *g_output_pk_data = NULL;
static size_t g_output_pk_len = 0;
static char g_params_dir[512] = {0};

static bool load_pk_file(const char *path, uint8_t **data, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        LOG_FAIL("sapling_prover", "load_pk_file: open(%s) failed", path);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        LOG_FAIL("sapling_prover", "load_pk_file: fstat(%s) failed", path);
    }
    *len = (size_t)st.st_size;
    *data = mmap(NULL, *len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (*data == MAP_FAILED) {
        *data = NULL;
        LOG_FAIL("sapling_prover",
                 "load_pk_file: mmap(%s, %zu) failed", path, *len);
    }
    return true;
}

static bool ensure_spend_pk(void)
{
    if (g_spend_pk_data) return true;
    if (!g_params_dir[0])
        LOG_FAIL("sapling_prover",
                 "ensure_spend_pk: params_dir not configured (call zclassic_sapling_prover_init first)");
    char path[512];
    snprintf(path, sizeof(path), "%s/sapling-spend.params", g_params_dir);
    return load_pk_file(path, &g_spend_pk_data, &g_spend_pk_len);
}

static bool ensure_output_pk(void)
{
    if (g_output_pk_data) return true;
    if (!g_params_dir[0])
        LOG_FAIL("sapling_prover",
                 "ensure_output_pk: params_dir not configured (call zclassic_sapling_prover_init first)");
    char path[512];
    snprintf(path, sizeof(path), "%s/sapling-output.params", g_params_dir);
    return load_pk_file(path, &g_output_pk_data, &g_output_pk_len);
}

/* --- Verification: delegate to sapling.c native C23 --- */

void *zclassic_sapling_verification_ctx_init(void)
{
    struct sapling_verification_ctx *ctx =
        zcl_calloc(1, sizeof(struct sapling_verification_ctx), "sapling_verify_ctx");
    if (ctx) sapling_verification_ctx_init(ctx);
    return ctx;
}

void zclassic_sapling_verification_ctx_free(void *ctx) { free(ctx); }

bool zclassic_sapling_check_spend(
    void *ctx, const uint8_t *cv, const uint8_t *anchor,
    const uint8_t *nullifier, const uint8_t *rk,
    const uint8_t *zkproof, const uint8_t *spend_auth_sig,
    const uint8_t *sighash_value)
{
    /* Full Groth16 + SpendAuth + binding sig accumulation.
     * Delegates to sapling.c which handles bvk accumulation internally. */
    return sapling_check_spend(ctx, cv, anchor, nullifier, rk,
                                zkproof, spend_auth_sig, sighash_value);
}

bool zclassic_sapling_check_output(
    void *ctx, const uint8_t *cv, const uint8_t *cm,
    const uint8_t *epk, const uint8_t *zkproof)
{
    /* Full Groth16 + binding sig accumulation.
     * Delegates to sapling.c which handles bvk accumulation internally. */
    return sapling_check_output(ctx, cv, cm, epk, zkproof);
}

bool zclassic_sapling_final_check(
    void *ctx, int64_t value_balance,
    const uint8_t *binding_sig, const uint8_t *sighash_value)
{
    return sapling_final_check(ctx, value_balance, binding_sig, sighash_value);
}

/* --- Proving: use C23 Groth16 prover --- */

struct zclassic_proving_ctx {
    struct fs bsk;  /* accumulated binding signing key (Fs field element) */
    bool has_bsk;
};

void *zclassic_sapling_proving_ctx_init(void)
{
    return zcl_calloc(1, sizeof(struct zclassic_proving_ctx), "sapling_proving_ctx");
}

void zclassic_sapling_proving_ctx_free(void *ctx) { free(ctx); }

bool sapling_spend_parse_witness(const uint8_t *witness,
                                  size_t witness_len,
                                  struct sapling_spend_witness *wit)
{
    /* enforce the fixed wire length before any read. A caller
     * that hands us a buffer shorter than the 1057-byte layout used
     * to walk off the end inside the loop below (memcpy + byte read
     * up to witness[1055]). Reject early with a clean false. */
    if (witness_len < (size_t)(1 + SAPLING_MERKLE_DEPTH * 33))
        return false;
    uint8_t depth = witness[0];
    if (depth != SAPLING_MERKLE_DEPTH)
        return false;
    for (int i = 0; i < SAPLING_MERKLE_DEPTH; i++) {
        memcpy(wit->auth_path[i], witness + 1 + i * 33, 32);
        wit->auth_path_bits[i] = witness[1 + i * 33 + 32] != 0;
    }
    return true;
}

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
    unsigned char *zkproof)
{
    /* Secret witness material — declared up front so every exit (success
     * and every LOG_FAIL error path) routes through `cleanup:` and wipes
     * them. All cleanses happen AFTER the last read of each secret (the
     * proof/nullifier/bsk are already produced), so this is output-neutral. */
    uint8_t rcv[32] = {0};
    struct sapling_spend_witness wit;
    uint8_t nk[32] = {0}, ivk[32] = {0};
    bool ok = false;

    if (!ensure_spend_pk()) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "spend_proof: ensure_spend_pk failed (params_dir=%s)\n",
                __FILE__, __LINE__, __func__, g_params_dir);
        goto cleanup;
    }

    /* Generate rcv for value commitment */
    if (!sapling_generate_r(rcv)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "spend_proof: sapling_generate_r(rcv) failed (RNG hygiene)\n",
                __FILE__, __LINE__, __func__);
        goto cleanup;
    }

    /* cv = value_commit(value, rcv) */
    if (!sapling_value_commit(value, rcv, cv)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "spend_proof: sapling_value_commit failed\n",
                __FILE__, __LINE__, __func__);
        goto cleanup;
    }

    /* rk = randomize(ak, ar) via spend auth generator */
    if (!sapling_compute_rk(ak, ar, rk)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "spend_proof: sapling_compute_rk failed\n",
                __FILE__, __LINE__, __func__);
        goto cleanup;
    }

    memcpy(wit.ak, ak, 32);
    memcpy(wit.nsk, nsk, 32);
    memcpy(wit.ar, ar, 32);
    wit.value = value;
    memcpy(wit.diversifier, diversifier, 11);

    /* Derive pk_d from nsk → nk, then crh_ivk(ak, nk) → ivk, then ivk_to_pkd */
    sapling_nsk_to_nk(nsk, nk);
    sapling_crh_ivk(ak, nk, ivk);
    sapling_ivk_to_pkd(ivk, diversifier, wit.pk_d);

    memcpy(wit.rcm, rcm, 32);
    memcpy(wit.rcv, rcv, 32);

    /* Parse merkle path: depth(1) || 32 × (sibling(32) || bit(1)).
     * The helper bounds-checks witness_len against the fixed 1057-byte
     * layout before reading anything — see sapling_spend_parse_witness
     * for the rationale. */
    if (!sapling_spend_parse_witness(witness, witness_len, &wit)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "spend_proof: malformed merkle path (witness_len=%zu, expected >= %zu)\n",
                __FILE__, __LINE__, __func__,
                witness_len,
                (size_t)(1 + SAPLING_MERKLE_DEPTH * 33));
        goto cleanup;
    }

    /* Compute nullifier */
    struct sapling_spend_inputs pub;
    memcpy(pub.rk, rk, 32);
    memcpy(pub.cv, cv, 32);
    memcpy(pub.anchor, anchor, 32);

    uint64_t position = 0;
    for (int i = 0; i < 32; i++)
        if (wit.auth_path_bits[i])
            position |= (uint64_t)1 << i;
    sapling_compute_nf(diversifier, wit.pk_d, value, rcm,
                        ak, nk, position, pub.nullifier);

    /* Groth16 prove */
    if (!sapling_create_spend_proof(g_spend_pk_data, g_spend_pk_len,
                                     &wit, &pub, zkproof)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "spend_proof: sapling_create_spend_proof failed\n",
                __FILE__, __LINE__, __func__);
        goto cleanup;
    }

    /* Accumulate bsk: bsk += rcv (spends add). This is the last read of
     * rcv; all secret reads are now complete. */
    struct zclassic_proving_ctx *pctx = ctx;
    if (pctx) {
        struct fs rcv_fs;
        fs_from_bytes(&rcv_fs, rcv);
        struct fs new_bsk;
        fs_add(&new_bsk, &pctx->bsk, &rcv_fs);
        pctx->bsk = new_bsk;
        pctx->has_bsk = true;
        memory_cleanse(&rcv_fs, sizeof(rcv_fs));
    }

    ok = true;

cleanup:
    /* Wipe the spend secrets on every path: rcv randomness, the witness
     * struct (ak/nsk/ar/rcm/rcv + derived pk_d), and the derived nk/ivk. */
    memory_cleanse(rcv, sizeof(rcv));
    memory_cleanse(&wit, sizeof(wit));
    memory_cleanse(nk, sizeof(nk));
    memory_cleanse(ivk, sizeof(ivk));
    return ok;
}

bool zclassic_sapling_output_proof(
    void *ctx,
    const unsigned char *esk,
    const unsigned char *diversifier,
    const unsigned char *pk_d,
    const unsigned char *rcm,
    const uint64_t value,
    unsigned char *cv,
    unsigned char *zkproof)
{
    /* Secret witness material — declared up front so every exit (success
     * and every LOG_FAIL error path) routes through `cleanup:` and wipes
     * them. All cleanses happen AFTER the last read (proof + bsk already
     * produced), so this is output-neutral. */
    uint8_t rcv[32] = {0};
    struct sapling_output_witness wit;
    bool ok = false;

    if (!ensure_output_pk()) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "output_proof: ensure_output_pk failed (params_dir=%s)\n",
                __FILE__, __LINE__, __func__, g_params_dir);
        goto cleanup;
    }

    if (!sapling_generate_r(rcv)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "output_proof: sapling_generate_r(rcv) failed (RNG hygiene)\n",
                __FILE__, __LINE__, __func__);
        goto cleanup;
    }

    if (!sapling_value_commit(value, rcv, cv)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "output_proof: sapling_value_commit failed\n",
                __FILE__, __LINE__, __func__);
        goto cleanup;
    }

    wit.value = value;
    memcpy(wit.diversifier, diversifier, 11);
    memcpy(wit.pk_d, pk_d, 32);
    memcpy(wit.rcm, rcm, 32);
    memcpy(wit.esk, esk, 32);
    memcpy(wit.rcv, rcv, 32);

    struct sapling_output_inputs pub;
    memcpy(pub.cv, cv, 32);
    sapling_ka_derivepublic(diversifier, esk, pub.epk);
    sapling_compute_cm(diversifier, pk_d, value, rcm, pub.cm);

    if (!sapling_create_output_proof(g_output_pk_data, g_output_pk_len,
                                      &wit, &pub, zkproof)) {
        fprintf(stderr, "[sapling_prover] %s:%d %s(): "  // obs-ok:proof-error-terminates-via-goto-cleanup
                "output_proof: sapling_create_output_proof failed\n",
                __FILE__, __LINE__, __func__);
        goto cleanup;
    }

    /* Accumulate bsk: bsk -= rcv (outputs subtract). Last read of rcv. */
    struct zclassic_proving_ctx *pctx = ctx;
    if (pctx) {
        struct fs rcv_fs, neg_rcv;
        fs_from_bytes(&rcv_fs, rcv);
        fs_neg(&neg_rcv, &rcv_fs);
        struct fs new_bsk;
        fs_add(&new_bsk, &pctx->bsk, &neg_rcv);
        pctx->bsk = new_bsk;
        pctx->has_bsk = true;
        memory_cleanse(&rcv_fs, sizeof(rcv_fs));
        memory_cleanse(&neg_rcv, sizeof(neg_rcv));
    }

    ok = true;

cleanup:
    /* Wipe the output secrets on every path: rcv randomness and the
     * witness struct (esk/rcm/rcv). */
    memory_cleanse(rcv, sizeof(rcv));
    memory_cleanse(&wit, sizeof(wit));
    return ok;
}

bool zclassic_sapling_binding_sig(
    const void *ctx, int64_t value_balance,
    const unsigned char *sighash, unsigned char *result)
{
    (void)value_balance;
    const struct zclassic_proving_ctx *pctx = ctx;
    if (!pctx || !pctx->has_bsk)
        LOG_FAIL("sapling_prover",
                 "binding_sig: proving ctx missing bsk (spend/output not called first?)");
    uint8_t bsk_bytes[32];
    fs_to_bytes(bsk_bytes, &pctx->bsk);
    /* generator_idx=1 for binding signature (uses value commitment generator) */
    bool ok = redjubjub_sign(bsk_bytes, sighash, 32, result, 1);
    /* Wipe the binding signing key after the signature is produced
     * (last read), on both success and failure. Output-neutral. */
    memory_cleanse(bsk_bytes, sizeof(bsk_bytes));
    return ok;
}

void zclassic_init_zksnark_params(
    const uint8_t *spend_path, size_t spend_path_len,
    const char *spend_hash,
    const uint8_t *output_path, size_t output_path_len,
    const char *output_hash,
    const uint8_t *sprout_path, size_t sprout_path_len,
    const char *sprout_hash)
{
    (void)spend_hash; (void)output_hash;
    (void)sprout_path; (void)sprout_path_len; (void)sprout_hash;
    (void)output_path; (void)output_path_len;

    if (spend_path && spend_path_len > 0) {
        size_t copy_len = spend_path_len < 511 ? spend_path_len : 511;
        memcpy(g_params_dir, spend_path, copy_len);
        g_params_dir[copy_len] = 0;
        char *slash = strrchr(g_params_dir, '/');
        if (slash) *slash = 0;
    }
}

#pragma GCC diagnostic pop
