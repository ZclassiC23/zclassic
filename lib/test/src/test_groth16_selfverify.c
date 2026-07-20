/* Positive Sapling prover capability gate.
 *
 * This test used to print FALSE for the native prover's self-verification and
 * deliberately return success. That made a broken prover indistinguishable
 * from a healthy one. The production parameter loader now runs a complete
 * Spend + Output + binding-signature bundle through the independent C23
 * consensus verifier before enabling proving. This test makes that result a
 * hard assertion and independently exercises the public Output API.
 */

#include "test/test_helpers.h"

#include "sapling/params_init.h"
#include "sapling/sapling.h"
#include "sapling/sapling_circuit.h"
#include "sapling/sapling_prover.h"
#include "sapling/groth16_prover.h"
#include "test/groth16_spend_oracle_kat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROVER_CHECK(name, expr) do {          \
    printf("  %s... ", (name));                \
    if ((expr)) printf("OK\n");                \
    else { printf("FAIL\n"); failures++; }     \
} while (0)

static bool find_diversifier(uint8_t d[11])
{
    memset(d, 0, 11);
    for (unsigned int i = 0; i < 256; i++) {
        d[0] = (uint8_t)i;
        if (sapling_check_diversifier(d))
            return true;
    }
    return false;
}

/* ── Native C23 Groth16 prover baseline (H1 harness) ──────────────────
 *
 * NON-GATING diagnostic. The production/gated prover is librustzcash (the
 * assertions above pin the Rust->C23-verifier round-trip). This section
 * measures the SEPARATE pure-C23 native circuits (sapling_output_synthesize /
 * sapling_spend_synthesize in lib/sapling/src/sapling_circuit.c) against the
 * trusted-setup proving keys, so the spend-prover campaign can track exact
 * var/constraint counts vs target without re-deriving them each lane.
 *
 * A native circuit only round-trips when its counts EXACTLY match the pk from
 * the trusted setup:  num_aux == pk.l_len  AND  num_vars == pk.a_len. The
 * printed table is the baseline; it asserts NOTHING (the native prover is a
 * known-incomplete work item — the OUTPUT native round-trip is documented as
 * rejecting in test_simnet_sapling_shielded_send.c, and the SPEND circuit is a
 * stub). Emitting the numbers here is the foundation, not a pass/fail gate. */
static void native_circuit_baseline(void)
{
    printf("\n--- H1 baseline: native C23 circuit counts (NON-GATING) ---\n");

    /* OUTPUT circuit vs sapling-output proving key ------------------- */
    size_t out_pk_len = 0;
    const uint8_t *out_pk_data = sapling_get_output_pk(&out_pk_len);
    if (out_pk_data && out_pk_len > 0) {
        struct groth16_pk opk;
        if (groth16_pk_read(&opk, out_pk_data, out_pk_len)) {
            printf("  OUTPUT pk: num_inputs=%zu a_len=%zu b_len=%zu "
                   "l_len=%zu h_len=%zu\n",
                   opk.num_inputs, opk.a_len, opk.b_len, opk.l_len, opk.h_len);

            /* Build a valid output witness (pk_d must be a real Jubjub point). */
            uint8_t d[11], ivk[32], pk_d[32];
            memset(ivk, 0x44, 32);
            bool have = false;
            for (unsigned i = 0; i < 256 && !have; i++) {
                memset(d, 0, 11);
                d[0] = (uint8_t)i;
                if (sapling_ivk_to_pkd(ivk, d, pk_d))
                    have = true;
            }
            if (have) {
                struct sapling_output_witness wit;
                memset(&wit, 0, sizeof wit);
                wit.value = UINT64_C(54321);
                memcpy(wit.diversifier, d, 11);
                memcpy(wit.pk_d, pk_d, 32);
                sapling_generate_r(wit.rcm);
                sapling_generate_r(wit.esk);
                sapling_generate_r(wit.rcv);
                struct sapling_output_inputs pub;
                memset(&pub, 0, sizeof pub);

                struct constraint_system cs;
                cs_init(&cs);
                if (sapling_output_synthesize(&cs, &wit, &pub)) {
                    size_t num_aux = (cs.num_vars > cs.num_inputs + 1)
                        ? cs.num_vars - cs.num_inputs - 1 : 0;
                    printf("  OUTPUT circuit: num_inputs=%zu num_vars=%zu "
                           "num_aux=%zu num_constraints=%zu\n",
                           cs.num_inputs, cs.num_vars, num_aux,
                           cs.num_constraints);
                    printf("  OUTPUT match: num_aux==pk.l_len? %s  "
                           "num_vars==pk.a_len? %s\n",
                           (num_aux == opk.l_len) ? "YES" : "NO",
                           (cs.num_vars == opk.a_len) ? "YES" : "NO");
                }
                cs_free(&cs);
            }
            groth16_pk_free(&opk);
        } else {
            printf("  OUTPUT pk: groth16_pk_read failed\n");
        }
    } else {
        printf("  OUTPUT pk: not loaded\n");
    }

    /* SPEND circuit vs sapling-spend proving key --------------------- */
    size_t sp_pk_len = 0;
    const uint8_t *sp_pk_data = sapling_get_spend_pk(&sp_pk_len);
    if (sp_pk_data && sp_pk_len > 0) {
        struct groth16_pk spk;
        if (groth16_pk_read(&spk, sp_pk_data, sp_pk_len)) {
            printf("  SPEND  pk: num_inputs=%zu a_len=%zu b_len=%zu "
                   "l_len=%zu h_len=%zu\n",
                   spk.num_inputs, spk.a_len, spk.b_len, spk.l_len, spk.h_len);

            /* Build a valid spend witness. ak/pk_d must be real Jubjub points;
             * rk/cv public inputs likewise (point_to_xy decodes them). */
            uint8_t ask[32] = {0};
            ask[0] = 0x07; ask[1] = 0xCC;
            uint8_t ak[32];
            sapling_ask_to_ak(ask, ak);

            struct sapling_spend_witness wit;
            memset(&wit, 0, sizeof wit);
            memcpy(wit.ak, ak, 32);
            wit.nsk[0] = 0x0B; wit.nsk[1] = 0x5A; wit.nsk[7] = 0x11;
            memcpy(wit.pk_d, ak, 32);
            memcpy(wit.diversifier, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b", 11);
            wit.value = UINT64_C(54321);

            struct sapling_spend_inputs pub;
            memset(&pub, 0, sizeof pub);
            memcpy(pub.rk, ak, 32);
            memcpy(pub.cv, ak, 32);

            struct constraint_system cs;
            cs_init(&cs);
            if (sapling_spend_synthesize(&cs, &wit, &pub)) {
                size_t num_aux = (cs.num_vars > cs.num_inputs + 1)
                    ? cs.num_vars - cs.num_inputs - 1 : 0;
                printf("  SPEND  circuit: num_inputs=%zu num_vars=%zu "
                       "num_aux=%zu num_constraints=%zu (target ~98777)\n",
                       cs.num_inputs, cs.num_vars, num_aux, cs.num_constraints);
                printf("  SPEND  match: num_aux==pk.l_len? %s  "
                       "num_vars==pk.a_len? %s\n",
                       (num_aux == spk.l_len) ? "YES" : "NO",
                       (cs.num_vars == spk.a_len) ? "YES" : "NO");
            }
            cs_free(&cs);
            groth16_pk_free(&spk);
        } else {
            printf("  SPEND  pk: groth16_pk_read failed\n");
        }
    } else {
        printf("  SPEND  pk: not loaded\n");
    }
    printf("--- end H1 baseline (informational) ---\n");
}

/* H3 lane: Sapling SPEND circuit port — shape + value + determinism gate.
 *
 * The spend circuit is ported gadget-by-gadget in bellman's Spend::synthesize
 * order. This gate is params-free (pure R1CS synthesis, no proving key) and
 * pins the ported prefix (sections 1..7) against ground truth:
 *   (1) cumulative constraint counts per section == the reference trace's
 *       cumulative boundaries (exact, verified by the salvage-plan legs);
 *   (2) the in-circuit nk / rk wires carry the reference-correct Jubjub points,
 *       with nk additionally pinned to the librustzcash reference vector (the
 *       H2 KAT) — validating the in-circuit fixed-base multiplication against
 *       ground truth end to end;
 *   (3) synthesis is deterministic (identical inputs => byte-identical witness).
 * Sections 8..28 are not yet ported, so this is a PARTIAL-prefix gate, not a
 * spend round-trip. Returns the number of failures (0 == green). */
static int spend_circuit_shape_gate(void)
{
    printf("\n--- H3: Sapling SPEND circuit port shape gate (sections 1-7) ---\n");
    int failures = 0;

    /* Fixed witness — reuses the H2 KAT scalars so the nk wire ties to the
     * pinned librustzcash reference vector. */
    uint8_t ak[32];
    sapling_ask_to_ak(SPEND_ORACLE_KAT_ASK, ak);

    struct sapling_spend_witness wit;
    memset(&wit, 0, sizeof wit);
    memcpy(wit.ak, ak, 32);
    memcpy(wit.nsk, SPEND_ORACLE_KAT_NSK, 32);
    wit.ar[0] = 0x03;               /* small fixed re-randomization scalar */
    memcpy(wit.pk_d, ak, 32);
    wit.value = UINT64_C(54321);

    uint8_t rk_bytes[32];
    bool rk_ok = sapling_compute_rk(ak, wit.ar, rk_bytes);
    PROVER_CHECK("compute_rk produced rk for the fixed witness", rk_ok);

    struct sapling_spend_inputs pub;
    memset(&pub, 0, sizeof pub);
    memcpy(pub.rk, rk_bytes, 32);
    memcpy(pub.cv, ak, 32);         /* any valid Jubjub point (bound later) */

    struct spend_section_shape sections[8];
    size_t nsec = 0;
    struct spend_wire_probe probe;
    struct constraint_system cs;
    cs_init(&cs);
    bool synth_ok = sapling_spend_synthesize_traced(
        &cs, &wit, &pub, sections, 8, &nsec, &probe);
    PROVER_CHECK("traced spend synthesis succeeded", synth_ok);

    /* (1) Per-section cumulative constraint counts vs the reference trace. */
    static const size_t REF_CUM[7] = {20, 272, 1022, 1028, 1030, 1282, 2032};
    static const char *REF_NAME[7] = {
        "S1 ak witness/on-curve/not-small-order (cum 20)",
        "S2 ar bits (cum 272)",
        "S3 randomization of signing key (cum 1022)",
        "S4 rk = ak + [ar]G (cum 1028)",
        "S5 rk inputize (cum 1030)",
        "S6 nsk bits (cum 1282)",
        "S7 nk = [nsk] ProofGenerationKey (cum 2032)",
    };
    PROVER_CHECK("synthesized all 7 ported sections", nsec == 7);
    for (size_t i = 0; i < 7 && i < nsec; i++)
        PROVER_CHECK(REF_NAME[i], sections[i].num_constraints == REF_CUM[i]);
    PROVER_CHECK("7 public inputs allocated (bellman-faithful low indices)",
                 cs.num_inputs == 7);
    PROVER_CHECK("ported-prefix constraint count == 2032",
                 cs.num_constraints == 2032);

    /* (2) Value gate: in-circuit wires carry reference-correct points; nk is
     *     pinned to the librustzcash reference (H2 KAT). */
    uint8_t nk_bytes[32];
    sapling_nsk_to_nk(wit.nsk, nk_bytes);
    PROVER_CHECK("out-of-circuit nk == pinned librustzcash reference (H2 KAT)",
                 memcmp(nk_bytes, SPEND_ORACLE_KAT_NK, 32) == 0);

    struct jub_point nk_pt, rk_pt;
    struct fr nk_x, nk_y, rk_x, rk_y;
    bool nk_dec = jub_from_bytes(&nk_pt, nk_bytes);
    bool rk_dec = rk_ok && jub_from_bytes(&rk_pt, rk_bytes);
    if (nk_dec) { jub_get_x(&nk_x, &nk_pt); jub_get_y(&nk_y, &nk_pt); }
    if (rk_dec) { jub_get_x(&rk_x, &rk_pt); jub_get_y(&rk_y, &rk_pt); }

    bool nk_wire_ok = synth_ok && nk_dec
        && probe.nk_x < cs.num_vars && probe.nk_y < cs.num_vars
        && fr_eq(&cs.witness[probe.nk_x], &nk_x)
        && fr_eq(&cs.witness[probe.nk_y], &nk_y);
    PROVER_CHECK("in-circuit nk wire == [nsk] ProofGenerationKeyGenerator",
                 nk_wire_ok);

    bool rk_wire_ok = synth_ok && rk_dec
        && probe.rk_x < cs.num_vars && probe.rk_y < cs.num_vars
        && fr_eq(&cs.witness[probe.rk_x], &rk_x)
        && fr_eq(&cs.witness[probe.rk_y], &rk_y);
    PROVER_CHECK("in-circuit rk wire == ak + [ar] SpendAuthGenerator",
                 rk_wire_ok);

    /* (3) Determinism: identical inputs => byte-identical witness. */
    struct constraint_system cs2;
    cs_init(&cs2);
    bool synth2 = sapling_spend_synthesize_traced(
        &cs2, &wit, &pub, NULL, 0, NULL, NULL);
    bool det_ok = synth2 && cs.num_vars == cs2.num_vars
        && cs.num_constraints == cs2.num_constraints
        && memcmp(cs.witness, cs2.witness,
                  cs.num_vars * sizeof(struct fr)) == 0;
    PROVER_CHECK("synthesis is deterministic (byte-identical witness)", det_ok);

    cs_free(&cs);
    cs_free(&cs2);

    printf("--- end H3 shape gate (%d failure[s]) ---\n", failures);
    return failures;
}

/* H2 lane: reference differential oracle (test-only librustzcash bridge).
 * Runs FIRST and unconditionally — it is params-free, so it gates even when
 * ~/.zcash-params is absent and the prover self-test below SKIPs. */
int groth16_spend_reference_oracle(void);

/* H4 lane: standing differential parity oracle over a corpus of witnesses.
 * Params-free; auto-tightens off the reference section-boundary table as the
 * H3 port advances. Lives in lib/test/src/groth16_spend_parity.c. */
int groth16_spend_parity_oracle(void);

int test_groth16_selfverify(void);
int test_groth16_selfverify(void)
{
    printf("\n=== Sapling prover -> consensus verifier capability ===\n");
    int failures = 0;

    failures += groth16_spend_reference_oracle();
    failures += spend_circuit_shape_gate();
    failures += groth16_spend_parity_oracle();

    const char *home = getenv("HOME");
    char params_dir[512];
    char output_path[640];
    snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
             (home && *home) ? home : ".");
    snprintf(output_path, sizeof(output_path),
             "%s/sapling-output.params", params_dir);

    FILE *probe = fopen(output_path, "rb");
    if (!probe) {
        printf("  params absent — SKIP (prover self-test); "
               "H2 oracle already ran above\n");
        return failures;
    }
    fclose(probe);

    const bool initialized = sapling_init_params(params_dir);
    PROVER_CHECK("parameter loader completed", initialized);
    PROVER_CHECK("backend provenance is pinned librustzcash",
                 strcmp(zclassic_sapling_prover_backend(),
                        "librustzcash-06da3b9ac8f2") == 0);
    PROVER_CHECK("full Spend+Output+binding self-test returned true",
                 initialized && zclassic_sapling_prover_run_self_test());
    PROVER_CHECK("proving capability is READY",
                 zclassic_sapling_prover_is_ready() &&
                 strcmp(zclassic_sapling_prover_status(), "ready") == 0);

    if (zclassic_sapling_prover_is_ready()) {
        uint8_t diversifier[11];
        uint8_t ask[32], nsk[32], ovk[32];
        uint8_t ak[32], nk[32], ivk[32], pk_d[32];
        bool keys_ok = find_diversifier(diversifier) &&
                       sapling_generate_r(ask) &&
                       sapling_generate_r(nsk) &&
                       sapling_generate_r(ovk);
        if (keys_ok) {
            sapling_ask_to_ak(ask, ak);
            sapling_nsk_to_nk(nsk, nk);
            sapling_crh_ivk(ak, nk, ivk);
            keys_ok = sapling_ivk_to_pkd(ivk, diversifier, pk_d);
        }
        PROVER_CHECK("independent output recipient constructed", keys_ok);

        void *pctx = keys_ok ? zclassic_sapling_proving_ctx_init() : NULL;
        uint8_t cv[32], cm[32], epk[32], proof[192];
        uint8_t enc[580], out[80];
        bool built = pctx && sapling_build_output_with_ctx(
            pctx, ovk, diversifier, pk_d, UINT64_C(54321), NULL,
            cv, cm, epk, enc, out, proof);
        PROVER_CHECK("public proving API produced an output proof", built);

        if (built) {
            struct sapling_verification_ctx vctx;
            sapling_verification_ctx_init(&vctx);
            PROVER_CHECK("independent C23 consensus verifier accepts proof",
                         sapling_check_output(&vctx, cv, cm, epk, proof));

            uint8_t bad_proof[192];
            memcpy(bad_proof, proof, sizeof(bad_proof));
            bad_proof[191] ^= 1;
            sapling_verification_ctx_init(&vctx);
            PROVER_CHECK("tampered proof is rejected",
                         !sapling_check_output(
                             &vctx, cv, cm, epk, bad_proof));
        }
        if (pctx)
            zclassic_sapling_proving_ctx_free(pctx);
    }

    /* Non-gating: emit native C23 circuit baseline counts for the
     * spend-prover campaign. Only meaningful once params are loaded. */
    if (initialized)
        native_circuit_baseline();

    printf("Sapling prover capability: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}
