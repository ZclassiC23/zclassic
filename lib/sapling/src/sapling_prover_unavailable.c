/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet-side Sapling proving backend for the DEFAULT, Rust-free build.
 *
 * This is the translation unit the Makefile selects when ZCL_WITH_RUST is
 * empty, which is the default: `make` on a host with no cargo/rustc produces a
 * full node that validates the chain, serves the explorer, mines, and receives
 * shielded funds — all of that is native C23 and lives elsewhere. The single
 * capability this file stands in for is CREATING Sapling proofs, i.e. sending
 * shielded value (t->z, z->z and z->t alike, because an output description
 * needs its own Groth16 proof just as a spend description does).
 *
 * Every entry point here refuses in one shape: a typed, logged failure that
 * names the exact rebuild flag. No entry point can crash, silently succeed, or
 * return an unproven value — the capability is reported absent by
 * zclassic_sapling_prover_is_ready(), which every caller already consults.
 *
 * Opt back in with `make ZCL_WITH_RUST=1` (needs cargo + rustc once, to build
 * vendor/lib/librustzcash.a); sapling_prover_librustzcash.c is compiled in
 * this file's place.
 */

#include "sapling/sapling_prover.h"

#include "util/log_macros.h"

#include <stdint.h>
#include <stddef.h>

/* The one sentence every refusal carries. Kept as one definition so the
 * wallet error body, the log line and `ops state` cannot drift apart. */
#define PROVER_ABSENT_HINT \
    "no Sapling proving backend in this build; rebuild with " \
    "'make ZCL_WITH_RUST=1' to link librustzcash-06da3b9ac8f2"

bool zclassic_sapling_prover_is_ready(void)
{
    return false;
}

const char *zclassic_sapling_prover_status(void)
{
    return "no_proving_backend_in_build";
}

const char *zclassic_sapling_prover_backend(void)
{
    return PROVER_ABSENT_HINT;
}

void *zclassic_sapling_proving_ctx_init(void)
{
    LOG_NULL("sapling_prover", "proving context refused: %s",
             PROVER_ABSENT_HINT);
}

void zclassic_sapling_proving_ctx_free(void *ctx)
{
    /* No context can exist: ctx_init above never returns one. Accepting NULL
     * silently keeps every caller's cleanup path identical across builds. */
    if (ctx)
        LOG_WARN("sapling_prover",
                 "proving context free called with a non-NULL ctx in a build "
                 "with no proving backend; ignoring");
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
    (void)ctx; (void)esk; (void)diversifier; (void)pk_d; (void)rcm;
    (void)value; (void)cv; (void)zkproof;
    LOG_FAIL("sapling_prover", "output proof refused: %s",
             PROVER_ABSENT_HINT);
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
    (void)ctx; (void)ak; (void)nsk; (void)diversifier; (void)rcm; (void)ar;
    (void)value; (void)anchor; (void)witness; (void)witness_len;
    (void)cv; (void)rk; (void)zkproof;
    LOG_FAIL("sapling_prover", "spend proof refused: %s",
             PROVER_ABSENT_HINT);
}

bool zclassic_sapling_binding_sig(
    const void *ctx, int64_t value_balance,
    const unsigned char *sighash, unsigned char *result)
{
    (void)ctx; (void)value_balance; (void)sighash; (void)result;
    LOG_FAIL("sapling_prover", "binding signature refused: %s",
             PROVER_ABSENT_HINT);
}

bool zclassic_sapling_prover_run_self_test(void)
{
    LOG_FAIL("sapling_prover", "self-test not runnable: %s",
             PROVER_ABSENT_HINT);
}

void zclassic_init_zksnark_params(
    const uint8_t *spend_path, size_t spend_path_len,
    const char *spend_hash,
    const uint8_t *output_path, size_t output_path_len,
    const char *output_hash,
    const uint8_t *sprout_path, size_t sprout_path_len,
    const char *sprout_hash)
{
    (void)spend_path; (void)spend_path_len; (void)spend_hash;
    (void)output_path; (void)output_path_len; (void)output_hash;
    (void)sprout_path; (void)sprout_path_len; (void)sprout_hash;
    /* Not an error: the C23 verifying keys the consensus path needs were
     * already installed by the caller (sapling_init_params) before this
     * returns. Only PROVING is unavailable. */
    LOG_INFO("sapling_prover",
             "Sapling proving parameters not loaded: %s; consensus "
             "verification and shielded receive are unaffected",
             PROVER_ABSENT_HINT);
}
