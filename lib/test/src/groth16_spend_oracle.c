/* Sapling SPEND-circuit reference differential oracle (test-only, H2 lane).
 *
 * Portions interoperate with librustzcash / bellman / sapling-crypto
 * (The Zcash developers / Electric Coin Company), pinned commit
 * 06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5, MIT / Apache-2.0. Only the
 * extern-"C" FFI surface of the pinned static archive is used here, and ONLY
 * from the test binary — no reference code is linked into the production node.
 *
 * WHAT THIS IS
 * ------------
 * The pinned vendor/lib/librustzcash.a does NOT export bellman's R1CS matrices
 * or the per-wire witness assignment for the Sapling spend circuit — those are
 * private to the Rust prover (`create_random_proof` consumes the synthesized
 * ConstraintSystem internally and only the 192-byte Groth16 proof crosses the
 * FFI). Confirmed by `nm vendor/lib/librustzcash.a`: the only spend-related
 * text symbol is `librustzcash_sapling_spend_proof` (full randomized proof
 * generation) — no synthesize / r1cs / witness entry point exists.
 *
 * What the archive DOES export as callable-from-C, deterministic ground truth
 * are the exact building blocks the spend circuit recomputes internally:
 *   librustzcash_nsk_to_nk          -> nk   (circuit section 7)
 *   librustzcash_crh_ivk            -> ivk  (circuit section 10)
 *   librustzcash_ivk_to_pkd         -> pk_d (circuit section 13)
 *   librustzcash_sapling_compute_cm -> cm   (circuit section 17)
 *   librustzcash_sapling_compute_nf -> nf   (circuit sections 27/28, the
 *                                            nullifier public input)
 * None of these require the proving `.params` files to be loaded — they are
 * pure Jubjub / Pedersen / blake2s crypto — so this oracle runs hermetically
 * even when ~/.zcash-params is absent.
 *
 * So the HONEST path taken by this lane (per the lane spec's fork): the
 * reference SYNTHESIS is not exposed, therefore we extract what IS — a
 * known-answer ground-truth vector for a fixed spend witness — bake it as a
 * checked-in KAT (groth16_spend_oracle_kat.h), and wire a test-only bridge
 * that (a) re-derives the vector from the reference archive and asserts it
 * equals the baked KAT (proves the KAT is faithful + reproducible), and (b)
 * runs the native C23 key-derivation / commitment / nullifier for the same
 * witness and asserts byte-equality against the reference. (b) is exactly the
 * substrate the parity lane (H4) extends: the spend circuit's witness
 * assignment for the nk / ivk / pk_d / cm / nf wires must diff clean against
 * these reference bytes.
 *
 * The `spend_proof` FFI is deliberately NOT baked as a KAT: it samples its
 * value-commitment randomness and Groth16 blinds from OsRng internally, so its
 * output bytes are non-reproducible and unfit for a checked-in vector.
 */

#include "test/test_helpers.h"
#include "test/groth16_spend_oracle_kat.h"

#include "sapling/sapling.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Test-only bridge to the pinned reference archive ──────────────────────
 * Declared locally (not in any public header) so the third-party FFI surface
 * never leaks into the repo's C API. These resolve against vendor/lib/
 * librustzcash.a, which the test binary already links (the sapling prover
 * references it). */
extern void librustzcash_nsk_to_nk(const uint8_t *nsk, uint8_t *result);
extern void librustzcash_crh_ivk(const uint8_t *ak, const uint8_t *nk,
                                 uint8_t *result);
extern bool librustzcash_ivk_to_pkd(const uint8_t *ivk,
                                    const uint8_t *diversifier,
                                    uint8_t *result);
extern bool librustzcash_sapling_compute_cm(const uint8_t *diversifier,
                                            const uint8_t *pk_d, uint64_t value,
                                            const uint8_t *r, uint8_t *result);
extern bool librustzcash_sapling_compute_nf(const uint8_t *diversifier,
                                            const uint8_t *pk_d, uint64_t value,
                                            const uint8_t *r, const uint8_t *ak,
                                            const uint8_t *nk, uint64_t position,
                                            uint8_t *result);

/* Reference ground truth for the fixed KAT witness, filled in by
 * spend_oracle_derive_reference(). */
struct spend_oracle_reference {
    uint8_t diversifier[11];
    uint8_t ak[32];
    uint8_t nk[32];
    uint8_t ivk[32];
    uint8_t pk_d[32];
    uint8_t cm[32];
    uint8_t nf[32];
    bool valid;
};

/* Derive the reference ground truth from the pinned archive for the checked-in
 * fixed witness. ak is produced by the C23 spend-auth generator (the reference
 * archive does not export ask->ak; ak is a private circuit *input*, not a
 * circuit-derived wire, so this does not weaken the differential). Every other
 * quantity comes straight from the reference FFI. */
static void spend_oracle_derive_reference(struct spend_oracle_reference *ref)
{
    memset(ref, 0, sizeof(*ref));

    /* Fixed diversifier: first valid one scanning d[0] = 0,1,2,... */
    uint8_t d[11];
    memset(d, 0, sizeof(d));
    bool have_d = false;
    for (unsigned i = 0; i < 256; i++) {
        d[0] = (uint8_t)i;
        if (sapling_check_diversifier(d)) { have_d = true; break; }
    }
    if (!have_d) {
        printf("  [oracle] no valid diversifier found — cannot derive\n");
        return;
    }
    memcpy(ref->diversifier, d, 11);

    sapling_ask_to_ak(SPEND_ORACLE_KAT_ASK, ref->ak);

    librustzcash_nsk_to_nk(SPEND_ORACLE_KAT_NSK, ref->nk);
    librustzcash_crh_ivk(ref->ak, ref->nk, ref->ivk);
    if (!librustzcash_ivk_to_pkd(ref->ivk, ref->diversifier, ref->pk_d)) {
        printf("  [oracle] reference ivk_to_pkd failed\n");
        return;
    }
    if (!librustzcash_sapling_compute_cm(ref->diversifier, ref->pk_d,
                                         SPEND_ORACLE_KAT_VALUE,
                                         SPEND_ORACLE_KAT_RCM, ref->cm)) {
        printf("  [oracle] reference compute_cm failed\n");
        return;
    }
    if (!librustzcash_sapling_compute_nf(ref->diversifier, ref->pk_d,
                                         SPEND_ORACLE_KAT_VALUE,
                                         SPEND_ORACLE_KAT_RCM, ref->ak, ref->nk,
                                         SPEND_ORACLE_KAT_POSITION, ref->nf)) {
        printf("  [oracle] reference compute_nf failed\n");
        return;
    }
    ref->valid = true;
}

static void emit_bytes(const char *name, const uint8_t *b, size_t n)
{
    printf("static const uint8_t %s[%zu] = {\n    ", name, n);
    for (size_t i = 0; i < n; i++) {
        printf("0x%02X,%s", b[i],
               ((i + 1) % 12 == 0 && i + 1 < n) ? "\n    " : " ");
    }
    printf("\n};\n");
}

/* Emit the reference vector as a ready-to-paste C block (used by
 * `make spend-oracle-kat`). */
static void spend_oracle_emit_kat(const struct spend_oracle_reference *ref)
{
    printf("\n/* ==== BEGIN generated spend-oracle KAT (paste into "
           "groth16_spend_oracle_kat.h) ==== */\n");
    emit_bytes("SPEND_ORACLE_KAT_DIVERSIFIER", ref->diversifier, 11);
    emit_bytes("SPEND_ORACLE_KAT_AK",   ref->ak,   32);
    emit_bytes("SPEND_ORACLE_KAT_NK",   ref->nk,   32);
    emit_bytes("SPEND_ORACLE_KAT_IVK",  ref->ivk,  32);
    emit_bytes("SPEND_ORACLE_KAT_PK_D", ref->pk_d, 32);
    emit_bytes("SPEND_ORACLE_KAT_CM",   ref->cm,   32);
    emit_bytes("SPEND_ORACLE_KAT_NF",   ref->nf,   32);
    printf("#define SPEND_ORACLE_KAT_BAKED 1\n");
    printf("/* ==== END generated spend-oracle KAT ==== */\n\n");
}

#define ORACLE_CHECK(name, expr) do {                 \
    printf("  %s... ", (name));                        \
    if ((expr)) { printf("OK\n"); }                    \
    else { printf("FAIL\n"); failures++; }             \
} while (0)

#define ORACLE_EQ(name, a, b, n) \
    ORACLE_CHECK(name, memcmp((a), (b), (n)) == 0)

/* Public entry point: reference differential oracle for the Sapling spend
 * circuit. Returns the number of failures (0 == green). Non-skippable and
 * params-free — it links the reference archive and needs no on-disk state. */
int groth16_spend_reference_oracle(void);
int groth16_spend_reference_oracle(void)
{
    printf("\n--- H2: Sapling SPEND reference differential oracle ---\n");
    int failures = 0;

    struct spend_oracle_reference ref;
    spend_oracle_derive_reference(&ref);
    ORACLE_CHECK("reference archive produced ground truth for fixed witness",
                 ref.valid);
    if (!ref.valid) {
        printf("--- end H2 oracle (%d failure[s]) ---\n", failures + 1);
        return failures + 1;
    }

    /* Emit mode: dump the reference vector and stop (regeneration path). */
    if (getenv("ZCL_EMIT_SPEND_ORACLE_KAT")) {
        spend_oracle_emit_kat(&ref);
        printf("--- end H2 oracle (emit mode, no assertions) ---\n");
        return 0;
    }

#if SPEND_ORACLE_KAT_BAKED
    /* (a) The reference re-derivation must equal the checked-in KAT. Proves the
     *     baked vector is faithful to the pinned archive AND that the reference
     *     FFI is deterministic across runs (no hidden RNG on this path). */
    ORACLE_EQ("reference diversifier == baked KAT",
              ref.diversifier, SPEND_ORACLE_KAT_DIVERSIFIER, 11);
    ORACLE_EQ("reference ak   == baked KAT", ref.ak,   SPEND_ORACLE_KAT_AK,   32);
    ORACLE_EQ("reference nk   == baked KAT", ref.nk,   SPEND_ORACLE_KAT_NK,   32);
    ORACLE_EQ("reference ivk  == baked KAT", ref.ivk,  SPEND_ORACLE_KAT_IVK,  32);
    ORACLE_EQ("reference pk_d == baked KAT", ref.pk_d, SPEND_ORACLE_KAT_PK_D, 32);
    ORACLE_EQ("reference cm   == baked KAT", ref.cm,   SPEND_ORACLE_KAT_CM,   32);
    ORACLE_EQ("reference nf   == baked KAT", ref.nf,   SPEND_ORACLE_KAT_NF,   32);

    /* (b) The native C23 spend-circuit building blocks must reproduce the
     *     reference wire values exactly — the differential substrate H4 grows
     *     into a full per-constraint witness diff. Each C23 stage is fed the
     *     preceding C23 stage's output, so an all-green chain proves end-to-end
     *     C23<->reference agreement on nk / ivk / pk_d / cm / nf. */
    uint8_t nk_c23[32], ivk_c23[32], pk_d_c23[32], cm_c23[32], nf_c23[32];
    sapling_nsk_to_nk(SPEND_ORACLE_KAT_NSK, nk_c23);
    sapling_crh_ivk(ref.ak, nk_c23, ivk_c23);
    bool pkd_ok = sapling_ivk_to_pkd(ivk_c23, ref.diversifier, pk_d_c23);
    ORACLE_CHECK("C23 sapling_ivk_to_pkd succeeded", pkd_ok);
    bool cm_ok = pkd_ok &&
        sapling_compute_cm(ref.diversifier, pk_d_c23, SPEND_ORACLE_KAT_VALUE,
                           SPEND_ORACLE_KAT_RCM, cm_c23);
    ORACLE_CHECK("C23 sapling_compute_cm succeeded", cm_ok);
    bool nf_ok = pkd_ok &&
        sapling_compute_nf(ref.diversifier, pk_d_c23, SPEND_ORACLE_KAT_VALUE,
                           SPEND_ORACLE_KAT_RCM, ref.ak, nk_c23,
                           SPEND_ORACLE_KAT_POSITION, nf_c23);
    ORACLE_CHECK("C23 sapling_compute_nf succeeded", nf_ok);

    ORACLE_EQ("C23 nk   == reference (section 7)",  nk_c23,   ref.nk,   32);
    ORACLE_EQ("C23 ivk  == reference (section 10)", ivk_c23,  ref.ivk,  32);
    if (pkd_ok)
        ORACLE_EQ("C23 pk_d == reference (section 13)", pk_d_c23, ref.pk_d, 32);
    if (cm_ok)
        ORACLE_EQ("C23 cm   == reference (section 17)", cm_c23,   ref.cm,   32);
    if (nf_ok)
        ORACLE_EQ("C23 nf   == reference (sections 27/28)", nf_c23, ref.nf, 32);
#else
    printf("  [oracle] KAT not yet baked — run `make spend-oracle-kat` and "
           "paste the emitted block into groth16_spend_oracle_kat.h\n");
    failures++;
#endif

    printf("--- end H2 oracle (%d failure[s]) ---\n", failures);
    return failures;
}
