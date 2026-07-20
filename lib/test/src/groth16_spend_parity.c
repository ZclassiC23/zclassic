/* Sapling SPEND-circuit standing differential parity oracle (test-only, H4 lane).
 *
 * Portions interoperate with librustzcash / bellman / sapling-crypto
 * (The Zcash developers / Electric Coin Company), pinned commit
 * 06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5, MIT / Apache-2.0. Only the
 * extern-"C" FFI surface of the pinned static archive is used here, and ONLY
 * from the test binary — no reference code is linked into the production node.
 *
 * WHAT THIS IS
 * ------------
 * The C-must-beat-Rust ratchet requires a differential parity oracle before any
 * native-crypto claim. The H2 oracle (groth16_spend_oracle.c) proves the native
 * key-derivation / commitment / nullifier building blocks match librustzcash for
 * ONE fixed KAT witness. The H3 shape gate (test_groth16_selfverify.c) pins the
 * ported prefix's per-section constraint boundaries for that SAME single witness.
 *
 * This oracle is the STANDING safety net that generalizes both: over a CORPUS of
 * deterministic spend witnesses, it re-runs the native C23 spend synthesis and
 * asserts, for every witness:
 *
 *   (A) SECTION-BOUNDARY PARITY (auto-tightening). Each recorded section's
 *       cumulative constraint count equals the pinned REFERENCE trace boundary
 *       (the full 28-section table from commit 06da3b9..., cross-checked 3x in
 *       the salvage plan). The oracle drives its assertions off the reference
 *       table and compares only the sections the native circuit ACTUALLY
 *       recorded (n_sections). So when the H3 port advances from 7 sections to
 *       8, 9, ... this oracle automatically validates the new section's boundary
 *       against REF_SECTIONS[i] with NO edit here — it tightens itself.
 *
 *   (B) STRUCTURAL INVARIANCE. An R1CS circuit's shape must not depend on the
 *       witness values — a witness-dependent constraint/var/input count is an
 *       unsound circuit. Every corpus witness must produce a byte-identical
 *       section shape (constraints/vars/inputs per section) to witness 0. This
 *       is a class of divergence the single-witness H2/H3 gates cannot see.
 *
 *   (C) PER-WIRE VALUE PARITY vs the reference archive. For every witness, the
 *       in-circuit nk wire ([nsk] ProofGenerationKeyGenerator, section 7) is
 *       compared byte-for-byte against librustzcash_nsk_to_nk — the differential
 *       against ground truth, per witness, not just the KAT. The in-circuit ak
 *       (section 1) and rk (section 4) wires are cross-checked against the native
 *       scalar derivations for self-consistency (the reference archive exports no
 *       ak/rk FFI — ak is a private circuit input, rk = ak + [ar]G is internal).
 *
 *   (D) DETERMINISM. Re-synthesizing an identical witness yields a byte-identical
 *       witness vector.
 *
 * On the FIRST divergence in any category the oracle prints the offending
 * (witness index, section name, expected vs actual) — it flags, never hides.
 *
 * The oracle is params-free and hermetic (pure Jubjub/blake2s/Pedersen crypto +
 * R1CS synthesis); it needs no ~/.zcash-params and no proving key, so it gates
 * unconditionally. It proves parity ONLY over the sections currently ported; the
 * honest scoreboard (ported prefix vs the 98777-constraint target) is printed and
 * documented in docs/work/GROTH16-SPEND-PARITY.md.
 */

#include "test/test_helpers.h"
#include "test/groth16_spend_oracle_kat.h"

#include "sapling/sapling.h"
#include "sapling/sapling_circuit.h"
#include "sapling/fr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test-only bridge to the pinned reference archive (vendor/lib/librustzcash.a,
 * already linked by the test binary). Declared locally so the third-party FFI
 * surface never leaks into the repo's C API. */
extern void librustzcash_nsk_to_nk(const uint8_t *nsk, uint8_t *result);

/* ── Pinned reference section-boundary table ────────────────────────────────
 * Cumulative num_constraints after each of bellman's 28 Spend::synthesize
 * sections, from the reference trace at commit 06da3b9ac8f278e5d4ae13088cf0a4c
 * 03d2c13f5 (num_constraints=98777, num_aux=98638, num_inputs=8 = 7 public +
 * ONE). Boundaries verified 3x against reference/groth16-traces/spend_circuit.
 * trace in the salvage plan. This table is the AUTHORITY the oracle diffs
 * against; the native port is correct for a prefix iff its recorded per-section
 * cumulative counts equal this table entry-for-entry over that prefix. */
static const struct ref_section {
    const char *name;
    size_t cum_constraints;
} REF_SECTIONS[28] = {
    { "1:ak witness+on-curve+not-small-order",   20 },
    { "2:ar bits",                              272 },
    { "3:randomization of signing key",        1022 },
    { "4:computation of rk",                   1028 },
    { "5:rk inputize",                         1030 },
    { "6:nsk bits",                            1282 },
    { "7:computation of nk",                   2032 },
    { "8:representation of ak",                2808 },
    { "9:representation of nk",                3584 },
    { "10:computation of ivk",               24590 },
    { "11:witness g_d",                      24594 },
    { "12:g_d not small order",              24610 },
    { "13:compute pk_d",                     27862 },
    { "14:value commitment",                 29127 },
    { "15:representation of g_d",            29903 },
    { "16:representation of pk_d",           30679 },
    { "17:note content hash",               31661 },
    { "18:rcm bits",                        31913 },
    { "19:commitment randomness",           32663 },
    { "20:randomization of note commitment", 32669 },
    { "21:merkle tree hash 0..31",          76893 },
    { "22:conditionally enforce correct root", 76894 },
    { "23:anchor inputize",                 76895 },
    { "24:g^position",                      76987 },
    { "25:faerie gold prevention",          76993 },
    { "26:representation of rho",            77769 },
    { "27:nf computation",                  98775 },
    { "28:pack nullifier",                  98777 },
};
#define REF_TOTAL_CONSTRAINTS 98777
#define REF_TOTAL_AUX         98638
#define REF_TOTAL_INPUTS          8 /* 7 public + ONE */
#define REF_NUM_SECTIONS         28

/* Deterministic corpus. Index 0 is the pinned H2 KAT witness (ties the nk wire
 * to the checked-in librustzcash reference vector); 1..N-1 are distinct
 * canonical witnesses (small Fs scalars, guaranteed < 2^252 so the in-circuit
 * 252-bit decomposition and the reference full reduction agree). */
#define CORPUS_N 6

struct parity_witness {
    struct sapling_spend_witness wit;
    struct sapling_spend_inputs  pub;
    bool ok;
};

static void build_corpus_witness(struct parity_witness *pw, unsigned idx)
{
    memset(pw, 0, sizeof(*pw));
    struct sapling_spend_witness *w = &pw->wit;

    uint8_t ask[32];
    memset(ask, 0, sizeof(ask));
    if (idx == 0) {
        memcpy(ask, SPEND_ORACLE_KAT_ASK, 32);
        memcpy(w->nsk, SPEND_ORACLE_KAT_NSK, 32);
        w->ar[0] = 0x03;
    } else {
        /* Distinct, canonical, small scalars — deterministic per index. */
        ask[0] = (uint8_t)(0x11 + idx);
        ask[1] = 0x22;
        ask[2] = (uint8_t)(idx * 5u);
        w->nsk[0] = (uint8_t)(idx * 7u + 1u);
        w->nsk[1] = (uint8_t)idx;
        w->nsk[2] = 0x5A;
        w->ar[0]  = (uint8_t)(idx + 1u);
        w->ar[1]  = (uint8_t)(idx * 3u);
    }

    uint8_t ak[32];
    sapling_ask_to_ak(ask, ak);
    memcpy(w->ak, ak, 32);
    memcpy(w->pk_d, ak, 32);           /* any valid Jubjub point (bound later) */
    w->value = UINT64_C(54321) + idx;

    uint8_t rk[32];
    if (!sapling_compute_rk(ak, w->ar, rk))
        return;                         /* pw->ok stays false */
    memcpy(pw->pub.rk, rk, 32);
    memcpy(pw->pub.cv, ak, 32);         /* any valid point (bound later) */
    pw->ok = true;
}

/* Decode a compressed Jubjub point to (x,y) Fr coords. */
static bool decode_xy(const uint8_t comp[32], struct fr *x, struct fr *y)
{
    struct jub_point p;
    if (!jub_from_bytes(&p, comp))
        return false;
    jub_get_x(x, &p);
    jub_get_y(y, &p);
    return true;
}

#define PARITY_CHECK(name, expr) do {                 \
    printf("  %s... ", (name));                        \
    if ((expr)) { printf("OK\n"); }                    \
    else { printf("FAIL\n"); failures++; }             \
} while (0)

/* Public entry point: standing differential parity oracle for the Sapling spend
 * circuit. Returns the number of failures (0 == green). Non-skippable and
 * params-free. */
int groth16_spend_parity_oracle(void);
int groth16_spend_parity_oracle(void)
{
    printf("\n--- H4: Sapling SPEND standing differential parity oracle ---\n");
    int failures = 0;

    /* Reference table self-consistency (guards a typo in the pinned boundaries;
     * they must be strictly increasing and terminate at the trace total). */
    bool ref_monotone = true;
    for (size_t i = 1; i < REF_NUM_SECTIONS; i++)
        if (REF_SECTIONS[i].cum_constraints <= REF_SECTIONS[i - 1].cum_constraints)
            ref_monotone = false;
    PARITY_CHECK("reference section table is strictly increasing", ref_monotone);
    PARITY_CHECK("reference table terminates at trace total (98777)",
                 REF_SECTIONS[REF_NUM_SECTIONS - 1].cum_constraints
                     == REF_TOTAL_CONSTRAINTS);

    /* Canonical shape recorded from witness 0, used for the invariance check. */
    struct spend_section_shape shape0[REF_NUM_SECTIONS];
    size_t nsec0 = 0;
    bool have_shape0 = false;

    /* Track the first divergence for a single, precise flag line. */
    bool flagged = false;
    size_t max_ported = 0;

    for (unsigned c = 0; c < CORPUS_N; c++) {
        struct parity_witness pw;
        build_corpus_witness(&pw, c);
        char label[96];

        snprintf(label, sizeof(label),
                 "corpus[%u]: witness constructed (valid rk/ak points)", c);
        PARITY_CHECK(label, pw.ok);
        if (!pw.ok)
            continue;

        struct spend_section_shape sections[REF_NUM_SECTIONS];
        size_t nsec = 0;
        struct spend_wire_probe probe;
        struct constraint_system cs;
        cs_init(&cs);
        bool synth = sapling_spend_synthesize_traced(
            &cs, &pw.wit, &pw.pub, sections, REF_NUM_SECTIONS, &nsec, &probe);

        snprintf(label, sizeof(label),
                 "corpus[%u]: traced synthesis succeeded", c);
        PARITY_CHECK(label, synth);
        if (!synth) { cs_free(&cs); continue; }

        if (nsec > max_ported)
            max_ported = nsec;

        /* (A) Section-boundary parity vs the reference — auto-tightening: only
         *     the `nsec` sections actually recorded are diffed, so the coverage
         *     grows automatically as the port advances. */
        bool boundaries_ok = (nsec <= REF_NUM_SECTIONS);
        for (size_t i = 0; i < nsec && i < REF_NUM_SECTIONS; i++) {
            if (sections[i].num_constraints != REF_SECTIONS[i].cum_constraints) {
                boundaries_ok = false;
                if (!flagged) {
                    flagged = true;
                    printf("  >> FIRST DIVERGENCE: corpus[%u] section '%s': "
                           "native cum_constraints=%zu, reference=%zu\n",
                           c, REF_SECTIONS[i].name,
                           sections[i].num_constraints,
                           REF_SECTIONS[i].cum_constraints);
                }
            }
        }
        snprintf(label, sizeof(label),
                 "corpus[%u]: %zu section boundaries == reference trace",
                 c, nsec);
        PARITY_CHECK(label, boundaries_ok);

        /* (B) Structural invariance across the corpus. */
        if (!have_shape0) {
            memcpy(shape0, sections, nsec * sizeof(sections[0]));
            nsec0 = nsec;
            have_shape0 = true;
        } else {
            bool shape_same = (nsec == nsec0);
            for (size_t i = 0; i < nsec && i < nsec0 && shape_same; i++)
                shape_same = sections[i].num_constraints == shape0[i].num_constraints
                          && sections[i].num_vars       == shape0[i].num_vars
                          && sections[i].num_inputs     == shape0[i].num_inputs;
            if (!shape_same && !flagged) {
                flagged = true;
                printf("  >> FIRST DIVERGENCE: corpus[%u] R1CS shape differs "
                       "from corpus[0] (witness-dependent circuit)\n", c);
            }
            snprintf(label, sizeof(label),
                     "corpus[%u]: R1CS shape witness-invariant vs corpus[0]", c);
            PARITY_CHECK(label, shape_same);
        }

        /* (C) Per-wire value parity. */
        /* nk wire vs the librustzcash reference (the differential). */
        uint8_t nk_ref[32];
        librustzcash_nsk_to_nk(pw.wit.nsk, nk_ref);
        uint8_t nk_native[32];
        sapling_nsk_to_nk(pw.wit.nsk, nk_native);
        snprintf(label, sizeof(label),
                 "corpus[%u]: native nsk_to_nk == librustzcash reference", c);
        PARITY_CHECK(label, memcmp(nk_native, nk_ref, 32) == 0);

        struct fr nkx, nky;
        bool nk_dec = decode_xy(nk_ref, &nkx, &nky);
        bool nk_wire_ok = nk_dec
            && probe.nk_x < cs.num_vars && probe.nk_y < cs.num_vars
            && fr_eq(&cs.witness[probe.nk_x], &nkx)
            && fr_eq(&cs.witness[probe.nk_y], &nky);
        if (!nk_wire_ok && !flagged) {
            flagged = true;
            printf("  >> FIRST DIVERGENCE: corpus[%u] in-circuit nk wire != "
                   "librustzcash [nsk] ProofGenerationKeyGenerator\n", c);
        }
        snprintf(label, sizeof(label),
                 "corpus[%u]: in-circuit nk wire == reference nk (section 7)", c);
        PARITY_CHECK(label, nk_wire_ok);

        /* ak wire vs the decoded witness ak (self-consistent, section 1). */
        struct fr akx, aky;
        bool ak_dec = decode_xy(pw.wit.ak, &akx, &aky);
        bool ak_wire_ok = ak_dec
            && probe.ak_x < cs.num_vars && probe.ak_y < cs.num_vars
            && fr_eq(&cs.witness[probe.ak_x], &akx)
            && fr_eq(&cs.witness[probe.ak_y], &aky);
        snprintf(label, sizeof(label),
                 "corpus[%u]: in-circuit ak wire == witness ak (section 1)", c);
        PARITY_CHECK(label, ak_wire_ok);

        /* rk wire vs the native scalar rk (self-consistent, section 4). */
        struct fr rkx, rky;
        bool rk_dec = decode_xy(pw.pub.rk, &rkx, &rky);
        bool rk_wire_ok = rk_dec
            && probe.rk_x < cs.num_vars && probe.rk_y < cs.num_vars
            && fr_eq(&cs.witness[probe.rk_x], &rkx)
            && fr_eq(&cs.witness[probe.rk_y], &rky);
        snprintf(label, sizeof(label),
                 "corpus[%u]: in-circuit rk wire == ak + [ar]G (section 4)", c);
        PARITY_CHECK(label, rk_wire_ok);

        /* (D) Determinism: re-synthesize, expect a byte-identical witness. */
        struct constraint_system cs2;
        cs_init(&cs2);
        bool synth2 = sapling_spend_synthesize_traced(
            &cs2, &pw.wit, &pw.pub, NULL, 0, NULL, NULL);
        bool det_ok = synth2
            && cs.num_vars == cs2.num_vars
            && cs.num_constraints == cs2.num_constraints
            && memcmp(cs.witness, cs2.witness,
                      cs.num_vars * sizeof(struct fr)) == 0;
        snprintf(label, sizeof(label),
                 "corpus[%u]: synthesis deterministic (byte-identical witness)",
                 c);
        PARITY_CHECK(label, det_ok);

        cs_free(&cs2);
        cs_free(&cs);
    }

    /* Index-0 tie to the pinned checked-in KAT vector (belt-and-suspenders:
     * the corpus witness 0 nk MUST equal the baked SPEND_ORACLE_KAT_NK). */
    uint8_t nk_kat[32];
    sapling_nsk_to_nk(SPEND_ORACLE_KAT_NSK, nk_kat);
    PARITY_CHECK("corpus[0] nk == pinned SPEND_ORACLE_KAT_NK",
                 memcmp(nk_kat, SPEND_ORACLE_KAT_NK, 32) == 0);

    /* Honest scoreboard. */
    size_t cum = (max_ported > 0 && max_ported <= REF_NUM_SECTIONS)
                 ? REF_SECTIONS[max_ported - 1].cum_constraints : 0;
    printf("  parity coverage: %zu/%d reference sections ported, "
           "%zu/%d constraints proven at parity (%.1f%%)\n",
           max_ported, REF_NUM_SECTIONS, cum, REF_TOTAL_CONSTRAINTS,
           100.0 * (double)cum / (double)REF_TOTAL_CONSTRAINTS);
    printf("  reference target (full circuit): %d constraints, %d aux, "
           "%d inputs — remaining sections %zu..%d pending H3 port\n",
           REF_TOTAL_CONSTRAINTS, REF_TOTAL_AUX, REF_TOTAL_INPUTS,
           max_ported + 1, REF_NUM_SECTIONS);
    if (max_ported < (size_t)REF_NUM_SECTIONS)
        printf("  next unimplemented section (typed blocker): '%s' "
               "(+%zu constraints to cum %zu) — native spend prover cannot "
               "round-trip until sections %zu..%d land\n",
               REF_SECTIONS[max_ported].name,
               REF_SECTIONS[max_ported].cum_constraints - cum,
               REF_SECTIONS[max_ported].cum_constraints,
               max_ported + 1, REF_NUM_SECTIONS);

    printf("--- end H4 parity oracle (%d failure[s]) ---\n", failures);
    return failures;
}
