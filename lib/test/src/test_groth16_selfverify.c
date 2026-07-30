/* Positive Sapling prover capability gate.
 *
 * This test used to print FALSE for the native prover's self-verification and
 * deliberately return success. That made a broken prover indistinguishable
 * from a healthy one. The production parameter loader now runs a complete
 * Spend + Output + binding-signature bundle through the independent C23
 * consensus verifier before enabling proving. This test makes that result a
 * hard assertion and independently exercises the public Output API.
 */

#include "test/test_core.h"

#include "sapling/params_init.h"
#include "sapling/sapling.h"
#include "sapling/sapling_circuit.h"
#include "sapling/sapling_prover.h"
#include "sapling/groth16_prover.h"
#include "sapling/fr.h"
#include "crypto/blake2s.h"
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

/* Both statics below are used only by the prover-backed section at the bottom
 * of this file, which the default Rust-free build skips; guarding them keeps
 * -Werror=unused-function honest instead of suppressed. */
#ifdef ZCL_WITH_RUST
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
#endif /* ZCL_WITH_RUST */

/* H3 lane: Sapling SPEND circuit port — shape + value + determinism gate.
 *
 * The spend circuit is ported gadget-by-gadget in bellman's Spend::synthesize
 * order. This gate is params-free (pure R1CS synthesis, no proving key) and
 * pins the ported prefix (sections 1..10) against ground truth:
 *   (1) cumulative constraint counts per section == the reference trace's
 *       cumulative boundaries (exact, verified by the salvage-plan legs);
 *   (2) the in-circuit nk / rk wires carry the reference-correct Jubjub points,
 *       with nk additionally pinned to the librustzcash reference vector (the
 *       H2 KAT) — validating the in-circuit fixed-base multiplication against
 *       ground truth end to end;
 *  (2b) section 10's 256 in-circuit blake2s digest bits == the out-of-circuit
 *       CRH^ivk over the same preimage, and its 251 truncated bits == the
 *       pinned librustzcash ivk. A matching constraint COUNT cannot see a wrong
 *       rotation constant or SIGMA row: mutation-testing confirms a wrong
 *       BLAKE2s rotation leaves the count at 24590 and the whole system
 *       satisfied, with only this check going red;
 *  (2c) every section-10 wire is BOUND — flipping any one of them (0<->1, which
 *       keeps booleanity intact) must make the R1CS unsatisfiable. Counts and
 *       values both read only the honest witness, so neither can see an
 *       UNDER-constrained gadget, which is the soundness-relevant failure: a
 *       free digest wire would let a prover choose its own ivk. Mutation-tested
 *       by making the XOR constraint vacuous — count, digest value and
 *       satisfaction all stay green and only this check fires;
 *   (3) synthesis is deterministic (identical inputs => byte-identical witness).
 * Sections 11..28 are not yet ported, so this is a PARTIAL-prefix gate, not a
 * spend round-trip. Returns the number of failures (0 == green). */
static int spend_circuit_shape_gate(void)
{
    printf("\n--- H3: Sapling SPEND circuit port shape gate (sections 1-10) ---\n");
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

    struct spend_section_shape sections[11];
    size_t nsec = 0;
    struct spend_wire_probe probe;
    struct constraint_system cs;
    cs_init(&cs);
    bool synth_ok = sapling_spend_synthesize_traced(
        &cs, &wit, &pub, sections, 11, &nsec, &probe);
    PROVER_CHECK("traced spend synthesis succeeded", synth_ok);

    /* (1) Per-section cumulative constraint counts vs the reference trace. */
    static const size_t REF_CUM[10] =
        {20, 272, 1022, 1028, 1030, 1282, 2032, 2808, 3584, 24590};
    static const char *REF_NAME[10] = {
        "S1 ak witness/on-curve/not-small-order (cum 20)",
        "S2 ar bits (cum 272)",
        "S3 randomization of signing key (cum 1022)",
        "S4 rk = ak + [ar]G (cum 1028)",
        "S5 rk inputize (cum 1030)",
        "S6 nsk bits (cum 1282)",
        "S7 nk = [nsk] ProofGenerationKey (cum 2032)",
        "S8 representation of ak (cum 2808)",
        "S9 representation of nk (cum 3584)",
        "S10 computation of ivk — in-circuit blake2s (cum 24590)",
    };
    PROVER_CHECK("synthesized all 10 ported sections", nsec == 10);
    for (size_t i = 0; i < 10 && i < nsec; i++)
        PROVER_CHECK(REF_NAME[i], sections[i].num_constraints == REF_CUM[i]);
    PROVER_CHECK("7 public inputs allocated (bellman-faithful low indices)",
                 cs.num_inputs == 7);
    PROVER_CHECK("ported-prefix constraint count == 24590",
                 cs.num_constraints == 24590);
    /* Section 10 alone must cost exactly what the reference's blake2s costs for
     * a 512-bit all-allocated input. bellman's own blake2s test asserts 21518
     * constraints for that shape, of which 512 are the input AllocatedBit::alloc
     * constraints the caller pays — leaving 21006 for the hash itself, which is
     * exactly the reference spend trace's section-10 delta. */
    PROVER_CHECK("S10 delta == 21006 (bellman blake2s, 512-bit input)",
                 nsec == 10 && sections[9].num_constraints
                             - sections[8].num_constraints == 21006);

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

    /* (2b) Section 10 VALUE gate. A matching constraint COUNT says nothing
     *      about what the gadget computes, so read the digest back off the
     *      circuit's own wires and diff it against ground truth twice:
     *
     *        - all 256 bits vs the out-of-circuit C23 BLAKE2s over the same
     *          preimage (the scalar implementation, KAT-pinned elsewhere), and
     *        - the 251 bits bellman keeps after `truncate(Fs::CAPACITY)` vs the
     *          checked-in librustzcash `SPEND_ORACLE_KAT_IVK` vector.
     *
     *      bellman's blake2s returns Booleans that may be NEGATED views of a
     *      wire, so the probe's negation flag has to be applied — reading the
     *      raw wire would invert bits and fail for the wrong reason. */
    uint8_t ivk_full[32];
    {
        struct blake2s_ctx bctx;
        blake2s_init_personal(&bctx, 32, (const uint8_t *)"Zcashivk");
        blake2s_update(&bctx, ak, 32);
        blake2s_update(&bctx, nk_bytes, 32);
        blake2s_final(&bctx, ivk_full, 32);
    }
    uint8_t ivk_truncated[32];
    sapling_crh_ivk(ak, nk_bytes, ivk_truncated);
    PROVER_CHECK("out-of-circuit CRH^ivk == pinned librustzcash ivk (H2 KAT)",
                 memcmp(ivk_truncated, SPEND_ORACLE_KAT_IVK, 32) == 0);

    struct fr one_fr;
    fr_one(&one_fr);
    bool ivk_bits_ok = synth_ok;
    bool ivk_trunc_ok = synth_ok;
    size_t first_bad_ivk_bit = SIZE_MAX;
    for (size_t b = 0; b < 256; b++) {
        const size_t v = probe.ivk_bit[b];
        if (v >= cs.num_vars) { ivk_bits_ok = false; ivk_trunc_ok = false;
                                if (first_bad_ivk_bit == SIZE_MAX)
                                    first_bad_ivk_bit = b;
                                continue; }
        bool wire = fr_eq(&cs.witness[v], &one_fr);
        bool got = probe.ivk_bit_negated[b] ? !wire : wire;
        bool want_full = ((ivk_full[b / 8] >> (b % 8)) & 1) == 1;
        if (got != want_full) {
            ivk_bits_ok = false;
            if (first_bad_ivk_bit == SIZE_MAX)
                first_bad_ivk_bit = b;
        }
        if (b < SPEND_IVK_TRUNCATED_BITS) {
            bool want_trunc =
                ((SPEND_ORACLE_KAT_IVK[b / 8] >> (b % 8)) & 1) == 1;
            if (got != want_trunc)
                ivk_trunc_ok = false;
        }
    }
    if (!ivk_bits_ok && first_bad_ivk_bit != SIZE_MAX)
        printf("  >> section 10 digest bit %zu diverges from "
               "BLAKE2s(\"Zcashivk\", repr(ak)||repr(nk))\n", first_bad_ivk_bit);
    PROVER_CHECK("in-circuit blake2s digest (256 bits) == out-of-circuit "
                 "CRH^ivk preimage hash", ivk_bits_ok);
    PROVER_CHECK("in-circuit ivk truncated to 251 bits == pinned "
                 "librustzcash ivk", ivk_trunc_ok);

    /* (2c) ADVERSARIAL: are section 10's wires actually BOUND, or merely
     *      present? A count gate and a value gate both pass for an
     *      UNDER-constrained gadget — the dangerous failure here — because both
     *      only ever look at the honest witness. So mutate the witness: flip one
     *      section-10 wire at a time from 0 to 1 (or back), which keeps every
     *      booleanity constraint satisfied, and require the system to become
     *      UNSATISFIED. A wire that can be flipped freely is a soundness hole:
     *      it would let a prover choose a digest bit, and CRH^ivk is what binds
     *      the spend to its viewing key.
     *
     *      Two populations are probed: the 256 digest wires (the gadget's
     *      output, where a free bit is directly exploitable) and a deterministic
     *      stride across every wire section 10 allocated (its internal
     *      round state, carries and XOR results). */
    size_t sec10_first_var = (nsec == 10) ? sections[8].num_vars : 0;
    size_t sec10_last_var  = (nsec == 10) ? sections[9].num_vars : 0;
    size_t flips_tried = 0, flips_detected = 0;
    size_t digest_tried = 0, digest_detected = 0;
    if (synth_ok && nsec == 10 && sec10_last_var > sec10_first_var) {
        struct fr zero_fr;
        fr_zero(&zero_fr);
        size_t ignored = SIZE_MAX;

        /* Sanity: the honest witness satisfies the system before any mutation,
         * otherwise "flip detected" would be vacuous. */
        PROVER_CHECK("honest witness satisfies the 24590-constraint prefix",
                     cs_is_satisfied(&cs, &ignored));

        for (size_t b = 0; b < 256; b++) {
            const size_t v = probe.ivk_bit[b];
            if (v >= cs.num_vars)
                continue;
            struct fr saved = cs.witness[v];
            cs.witness[v] = fr_eq(&saved, &one_fr) ? zero_fr : one_fr;
            digest_tried++;
            if (!cs_is_satisfied(&cs, &ignored))
                digest_detected++;
            cs.witness[v] = saved;
        }

        const size_t span = sec10_last_var - sec10_first_var;
        const size_t stride = (span / 96) ? (span / 96) : 1;
        for (size_t v = sec10_first_var; v < sec10_last_var; v += stride) {
            struct fr saved = cs.witness[v];
            cs.witness[v] = fr_eq(&saved, &one_fr) ? zero_fr : one_fr;
            flips_tried++;
            if (!cs_is_satisfied(&cs, &ignored))
                flips_detected++;
            cs.witness[v] = saved;
        }

        /* Restoring must return the system to satisfied — proves the probe
         * itself did not corrupt the witness it was measuring. */
        PROVER_CHECK("witness restored after mutation probe",
                     cs_is_satisfied(&cs, &ignored));
    }
    printf("  section 10 wires %zu..%zu; single-bit flips detected: "
           "%zu/%zu digest, %zu/%zu strided internal\n",
           sec10_first_var, sec10_last_var,
           digest_detected, digest_tried, flips_detected, flips_tried);
    PROVER_CHECK("every section-10 digest wire is bound (a flipped digest bit "
                 "breaks the R1CS)",
                 digest_tried == 256 && digest_detected == 256);
    PROVER_CHECK("every probed section-10 internal wire is bound (no free "
                 "wire = no under-constrained gadget)",
                 flips_tried > 0 && flips_detected == flips_tried);

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

/* H5 lane: adversarial + negative-control gate over the production SPEND
 * prove (reference oracle) -> verify (native C23) round-trip, plus a
 * proving-key-parser fuzz spot-check and zeroization spot-checks. Requires
 * proving params (guarded below by the same is-ready check the rest of this
 * self-test block uses). Lives in lib/test/src/groth16_spend_adversarial.c. */
int groth16_spend_adversarial_gate(void);

int test_groth16_selfverify(void);
int test_groth16_selfverify(void)
{
    printf("\n=== Sapling prover -> consensus verifier capability ===\n");
    int failures = 0;

    failures += groth16_spend_reference_oracle();
    failures += spend_circuit_shape_gate();
    failures += groth16_spend_parity_oracle();

#ifndef ZCL_WITH_RUST
    /* The DEFAULT build links no proving backend (see ZCL_WITH_RUST at the top
     * of the Makefile). Everything above this point is pure C23 and has
     * already run and gated: the H2 reference differential against the baked
     * KAT, the H3 spend-circuit shape gate, and the H4 parity oracle. What
     * remains below needs a prover, so skip it by name — and skip BEFORE the
     * first PROVER_CHECK, because "backend provenance is pinned librustzcash"
     * and the self-test assertion would hard-fail on a host that merely
     * happens to have ~/.zcash-params on disk. */
    printf("  SKIP (prover self-test) — %s (status=%s). The H2 reference "
           "differential, the H3 shape gate and the H4 parity oracle above are "
           "pure C23 and already ran.\n",
           zclassic_sapling_prover_backend(),
           zclassic_sapling_prover_status());
    printf("Sapling prover capability: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
#else
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

    /* H5: adversarial + negative-control gate over the production SPEND
     * prove->verify round-trip. Gated on proving readiness (needs a real
     * proof to tamper with), independent of the OUTPUT-only checks above. */
    if (zclassic_sapling_prover_is_ready())
        failures += groth16_spend_adversarial_gate();

    /* Non-gating: emit native C23 circuit baseline counts for the
     * spend-prover campaign. Only meaningful once params are loaded. */
    if (initialized)
        native_circuit_baseline();

    printf("Sapling prover capability: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
#endif /* ZCL_WITH_RUST */
}
