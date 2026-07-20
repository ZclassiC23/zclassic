/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sapling circuit synthesis — generates R1CS constraints for Groth16.
 *
 * Spend circuit (7 public inputs, ~100K constraints):
 *   Proves knowledge of a note in the Merkle tree, derives nullifier,
 *   and commits to value without revealing it.
 *
 * Output circuit (5 public inputs, ~16K constraints):
 *   Proves correct note commitment and ephemeral key derivation. */

#include "sapling/sapling_circuit.h"
#include "sapling/circuit_gadgets.h"
#include "sapling/pedersen_hash.h"
#include "sapling/sapling.h"
#include "crypto/blake2s.h"
#include "support/cleanse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"
#include "util/log_macros.h"

/* Index of the constant-ONE variable in every constraint system. */
#define CS_ONE 0

/* ── Helper: convert bytes to Fr ────────────────────────────────── */

static void bytes_to_fr(struct fr *out, const uint8_t bytes[32])
{
    fr_from_bytes(out, bytes);
}

/* ── Helper: Jubjub point from compressed bytes ─────────────────── */

static bool point_to_xy(struct fr *x, struct fr *y, const uint8_t compressed[32])
{
    struct jub_point p;
    if (!jub_from_bytes(&p, compressed))
        LOG_FAIL("sapling_circuit",
                 "point_to_xy: jub_from_bytes failed on compressed input");
    jub_get_x(x, &p);
    jub_get_y(y, &p);
    return true;
}

/* ── Spend Circuit Synthesis ────────────────────────────────────── */

/* Port status (H3 lane): sections 1..7 of bellman's Spend::synthesize are
 * ported faithfully in the exact synthesis order (variable-allocation order is
 * load-bearing for QAP alignment). Cumulative constraint counts match the
 * reference trace's per-section boundaries (verified by the shape gate in
 * test_groth16_selfverify.c). Sections 8..28 (EdwardsPoint::repr helpers, the
 * two blake2s hashes ivk/nf at ~21006 constraints each, variable-base pk_d,
 * value commitment, note commitment, the 32-level Merkle path, and the
 * nullifier packing) remain to be ported — the circuit below is therefore a
 * faithful PARTIAL prefix (~2032/98777 constraints) and does not yet round-trip
 * against the trusted-setup proving key.
 *
 * The seven public inputs are allocated up front (indices 1..7) in bellman's
 * input order (rk.x, rk.y, cv.x, cv.y, anchor, nf[0], nf[1]). Under this
 * constraint system's shared variable counter, allocating all inputs first is
 * what places them at the low indices bellman reserves for its separate input
 * namespace, so this is the QAP-faithful placement — inline inputize() would
 * scatter inputs among aux and permanently misalign the QAP. Wires computed
 * later are bound to their input slot by a copy constraint (see rk below);
 * cv/anchor/nf are bound in the not-yet-ported sections. */

/* Local: boolean-only little-endian decomposition (bellman
 * boolean::field_into_boolean_vec_le) — n_bits boolean aux + n_bits boolean
 * constraints, NO packing constraint back to a field element (the scalar is
 * consumed only as bits by a fixed-base multiplication). */
static void field_into_boolean_vec_le(struct constraint_system *cs,
                                       size_t *bits_out, size_t n_bits,
                                       const struct fr *value)
{
    uint8_t bytes[32];
    fr_to_bytes(bytes, value);
    for (size_t i = 0; i < n_bits; i++) {
        size_t byte_idx = i / 8, bit_idx = i % 8;
        bool bit = byte_idx < 32 && ((bytes[byte_idx] >> bit_idx) & 1);
        bits_out[i] = gadget_alloc_boolean(cs, bit);
    }
}

/* Local: copy constraint `src * 1 = dst` (bellman inputize's equality
 * enforcement, applied against a pre-allocated input slot). One constraint. */
static void enforce_equal(struct constraint_system *cs, size_t src, size_t dst)
{
    struct linear_combination la, lb, lc;
    struct fr one_val;
    fr_one(&one_val);
    lc_init(&la); lc_add_term(&la, src, &one_val);
    lc_init(&lb); lc_add_term(&lb, CS_ONE, &one_val);
    lc_init(&lc); lc_add_term(&lc, dst, &one_val);
    cs_enforce(cs, &la, &lb, &lc);
    lc_free(&la); lc_free(&lb); lc_free(&lc);
}

/* Local: record a per-section shape checkpoint (no-op when sections==NULL). */
static void section_record(struct constraint_system *cs,
                           struct spend_section_shape *sections,
                           size_t max_sections, size_t *n, const char *name)
{
    if (!sections || *n >= max_sections)
        return;
    sections[*n].name = name;
    sections[*n].num_constraints = cs->num_constraints;
    sections[*n].num_vars = cs->num_vars;
    sections[*n].num_inputs = cs->num_inputs;
    (*n)++;
}

bool sapling_spend_synthesize_traced(struct constraint_system *cs,
                                     const struct sapling_spend_witness *wit,
                                     const struct sapling_spend_inputs *pub,
                                     struct spend_section_shape *sections,
                                     size_t max_sections,
                                     size_t *n_sections_out,
                                     struct spend_wire_probe *probe)
{
    size_t nsec = 0;
    if (n_sections_out)
        *n_sections_out = 0;
    if (probe) {
        probe->ak_x = probe->ak_y = SIZE_MAX;
        probe->rk_x = probe->rk_y = SIZE_MAX;
        probe->nk_x = probe->nk_y = SIZE_MAX;
    }

    /* ── Public inputs 1..7 (allocated up front — see header note) ── */
    struct fr rk_x, rk_y;
    if (!point_to_xy(&rk_x, &rk_y, pub->rk))
        LOG_FAIL("sapling_circuit", "spend: point_to_xy(rk) failed");
    struct fr cv_x, cv_y;
    if (!point_to_xy(&cv_x, &cv_y, pub->cv))
        LOG_FAIL("sapling_circuit", "spend: point_to_xy(cv) failed");
    struct fr anchor_fr;
    bytes_to_fr(&anchor_fr, pub->anchor);

    uint64_t nf_packed[2][4];
    size_t nf_count = 0;
    multipack_bytes_to_fr(nf_packed, &nf_count, pub->nullifier, 32);
    struct fr nf0, nf1;
    memcpy(nf0.d, nf_packed[0], 32);
    if (nf_count > 1)
        memcpy(nf1.d, nf_packed[1], 32);
    else
        fr_zero(&nf1);

    size_t in_rk_x = cs_alloc_input(cs, &rk_x);  /* input 1: rk.x */
    size_t in_rk_y = cs_alloc_input(cs, &rk_y);  /* input 2: rk.y */
    cs_alloc_input(cs, &cv_x);      /* input 3: cv.x   (bound in section 14) */
    cs_alloc_input(cs, &cv_y);      /* input 4: cv.y   (bound in section 14) */
    cs_alloc_input(cs, &anchor_fr); /* input 5: anchor (bound in section 23) */
    cs_alloc_input(cs, &nf0);       /* input 6: nf[0]  (bound in section 28) */
    cs_alloc_input(cs, &nf1);       /* input 7: nf[1]  (bound in section 28) */

    /* ════ Section 1: witness ak, on-curve + not-small-order (20) ════ */
    struct fr ak_x, ak_y;
    if (!point_to_xy(&ak_x, &ak_y, wit->ak))
        LOG_FAIL("sapling_circuit", "spend: point_to_xy(ak) failed");
    size_t ak_x_var = cs_alloc_aux(cs, &ak_x);
    size_t ak_y_var = cs_alloc_aux(cs, &ak_y);
    gadget_point_interpret(cs, ak_x_var, ak_y_var);         /* on-curve (4) */
    gadget_assert_not_small_order(cs, ak_x_var, ak_y_var);  /* (16) */
    if (probe) { probe->ak_x = ak_x_var; probe->ak_y = ak_y_var; }
    section_record(cs, sections, max_sections, &nsec,
                   "1:ak witness+on-curve+not-small-order");

    /* ════ Section 2: ar into 252 boolean bits (252) ════ */
    struct fr ar_fr;
    bytes_to_fr(&ar_fr, wit->ar);
    size_t ar_bits[252];
    field_into_boolean_vec_le(cs, ar_bits, 252, &ar_fr);
    memory_cleanse(&ar_fr, sizeof(ar_fr));
    section_record(cs, sections, max_sections, &nsec, "2:ar bits");

    /* ════ Section 3: ar_g = [ar] SpendAuthGenerator (750) ════ */
    struct fr sag_x, sag_y;
    sapling_spend_auth_generator(&sag_x, &sag_y);
    size_t arg_x, arg_y;
    gadget_fixed_base_mul(cs, ar_bits, 252, &sag_x, &sag_y, &arg_x, &arg_y);
    section_record(cs, sections, max_sections, &nsec,
                   "3:randomization of signing key");

    /* ════ Section 4: rk = ak + ar_g (6) ════ */
    size_t rk_var_x, rk_var_y;
    gadget_edwards_add(cs, ak_x_var, ak_y_var, arg_x, arg_y,
                       &rk_var_x, &rk_var_y);
    if (probe) { probe->rk_x = rk_var_x; probe->rk_y = rk_var_y; }
    section_record(cs, sections, max_sections, &nsec, "4:computation of rk");

    /* ════ Section 5: rk inputize — bind to input slots 1,2 (2) ════ */
    enforce_equal(cs, rk_var_x, in_rk_x);
    enforce_equal(cs, rk_var_y, in_rk_y);
    section_record(cs, sections, max_sections, &nsec, "5:rk inputize");

    /* ════ Section 6: nsk into 252 boolean bits (252) ════ */
    struct fr nsk_fr;
    bytes_to_fr(&nsk_fr, wit->nsk);
    size_t nsk_bits[252];
    field_into_boolean_vec_le(cs, nsk_bits, 252, &nsk_fr);
    memory_cleanse(&nsk_fr, sizeof(nsk_fr));
    section_record(cs, sections, max_sections, &nsec, "6:nsk bits");

    /* ════ Section 7: nk = [nsk] ProofGenerationKeyGenerator (750) ════ */
    struct fr pgg_x, pgg_y;
    sapling_proof_gen_key_generator(&pgg_x, &pgg_y);
    size_t nk_var_x, nk_var_y;
    gadget_fixed_base_mul(cs, nsk_bits, 252, &pgg_x, &pgg_y,
                          &nk_var_x, &nk_var_y);
    if (probe) { probe->nk_x = nk_var_x; probe->nk_y = nk_var_y; }
    section_record(cs, sections, max_sections, &nsec,
                   "7:computation of nk");

    /* Sections 8..28 remain to be ported (see the port-status note above).
     * The prefix synthesized here is deterministic and shape-matched to the
     * reference for sections 1..7. */

    if (n_sections_out)
        *n_sections_out = nsec;
    return true;
}

bool sapling_spend_synthesize(struct constraint_system *cs,
                               const struct sapling_spend_witness *wit,
                               const struct sapling_spend_inputs *pub)
{
    return sapling_spend_synthesize_traced(cs, wit, pub, NULL, 0, NULL, NULL);
}

/* Canonical section roadmap for bellman's Spend::synthesize. Names index the
 * NEXT unimplemented section for the honest blocker; index 0 is section 1, so
 * roadmap[sections_ported] is the first section NOT yet ported. Kept in the
 * production circuit as the authoritative port target (the test-side reference
 * trace in groth16_spend_parity.c is an INDEPENDENT differential fixture, not
 * this list — they must agree, and the parity oracle proves they do). */
static const char *const SPEND_SECTION_ROADMAP[SPEND_CIRCUIT_TOTAL_SECTIONS] = {
    "1:ak witness+on-curve+not-small-order",
    "2:ar bits",
    "3:randomization of signing key",
    "4:computation of rk",
    "5:rk inputize",
    "6:nsk bits",
    "7:computation of nk",
    "8:representation of ak",
    "9:representation of nk",
    "10:computation of ivk",
    "11:witness g_d",
    "12:g_d not small order",
    "13:compute pk_d",
    "14:value commitment",
    "15:representation of g_d",
    "16:representation of pk_d",
    "17:note content hash",
    "18:rcm bits",
    "19:commitment randomness",
    "20:randomization of note commitment",
    "21:merkle tree hash 0..31",
    "22:conditionally enforce correct root",
    "23:anchor inputize",
    "24:g^position",
    "25:faerie gold prevention",
    "26:representation of rho",
    "27:nf computation",
    "28:pack nullifier",
};

void sapling_spend_prover_native_status(struct spend_prover_native_status *out)
{
    if (!out)
        return;
    out->sections_ported = 0;
    out->sections_total = SPEND_CIRCUIT_TOTAL_SECTIONS;
    out->constraints_ported = 0;
    out->constraints_total = SPEND_CIRCUIT_TOTAL_CONSTRAINTS;
    out->roundtrip_ready = false;
    out->next_blocker = SPEND_SECTION_ROADMAP[0];

    /* Canonical, deterministic, non-secret witness: derive a valid ak/rk from a
     * fixed dummy ask/ar so synthesis reaches the ported prefix. No proving key
     * and no real spend secrets are touched — this is a coverage probe only. */
    struct sapling_spend_witness wit;
    struct sapling_spend_inputs pub;
    memset(&wit, 0, sizeof(wit));
    memset(&pub, 0, sizeof(pub));

    uint8_t ask[32] = {0};
    ask[0] = 0x2a;
    ask[1] = 0x17;
    uint8_t ak[32];
    sapling_ask_to_ak(ask, ak);
    memcpy(wit.ak, ak, 32);
    wit.nsk[0] = 0x11;
    wit.ar[0] = 0x03;
    wit.value = UINT64_C(12345);
    uint8_t rk[32];
    if (!sapling_compute_rk(ak, wit.ar, rk)) {
        memory_cleanse(ask, sizeof(ask));
        /* Leave the pessimistic defaults (0/blocked); a failure to build the
         * probe witness is itself an honest "not ready". */
        return;
    }
    memcpy(pub.rk, rk, 32);
    memcpy(pub.cv, ak, 32);   /* any valid point; unbound in the ported prefix */

    struct spend_section_shape sections[SPEND_CIRCUIT_TOTAL_SECTIONS];
    size_t nsec = 0;
    struct constraint_system cs;
    cs_init(&cs);
    bool synth = sapling_spend_synthesize_traced(
        &cs, &wit, &pub, sections, SPEND_CIRCUIT_TOTAL_SECTIONS, &nsec, NULL);
    if (synth) {
        out->sections_ported = nsec;
        out->constraints_ported = cs.num_constraints;
        if (nsec >= SPEND_CIRCUIT_TOTAL_SECTIONS) {
            /* All sections synthesize. round-trip readiness still requires a
             * verifier-accepted native proof; that promotion is done by the
             * self-test, not by mere section coverage — stay false here. */
            out->next_blocker = "port complete (round-trip verification pending)";
        } else {
            out->next_blocker = SPEND_SECTION_ROADMAP[nsec];
        }
    }
    if (cs.witness)
        memory_cleanse(cs.witness, cs.cap_vars * sizeof(struct fr));
    cs_free(&cs);
    memory_cleanse(ask, sizeof(ask));
    memory_cleanse(&wit, sizeof(wit));
}

/* ── Output Circuit Synthesis ───────────────────────────────────── */

/* Matching Zcash sapling-crypto Output::synthesize exactly:
 * 7827 constraints, 6 inputs (ONE + cv.x + cv.y + epk.x + epk.y + cm).
 *
 * Steps:
 *   1. expose_value_commitment: value_bits→fixed_base_mul(G_v) +
 *      rcv_bits→fixed_base_mul(G_rcv) + add → inputize cv
 *   2. witness g_d + on_curve + not_small_order + repr (256 bits)
 *   3. esk_bits → g_d.mul(esk) → inputize epk
 *   4. pk_d witness: y_bits(255) + x_sign_bit(1) = 256 bits
 *   5. pedersen_hash(NoteCommitment, value_bits||g_d_repr||pk_d_repr)
 *   6. rcm_bits→fixed_base_mul(G_rcm) + add → cm.x inputize */

bool sapling_output_synthesize(struct constraint_system *cs,
                                const struct sapling_output_witness *wit,
                                const struct sapling_output_inputs *pub)
{
    (void)pub; /* Public inputs are computed in-circuit, not passed in */

    /* note_contents accumulates boolean variable indices:
     * value(64) + g_d_repr(256) + pk_d_repr(256) = 576 bits */
    size_t *note_contents = zcl_malloc(576 * sizeof(size_t), "note_contents");
    if (!note_contents)
        LOG_FAIL("sapling_circuit",
                 "note_contents: zcl_malloc(%zu) failed", 576 * sizeof(size_t));
    size_t nc_idx = 0;

    /* ════════════════════════════════════════════════════════
     * Step 1: Value Commitment — expose_value_commitment()
     * ════════════════════════════════════════════════════════ */

    /* 1a. Booleanize value (64 bits) */
    struct fr value_fr;
    {
        uint8_t vbytes[32] = {0};
        for (int i = 0; i < 8; i++)
            vbytes[i] = (uint8_t)(wit->value >> (i * 8));
        bytes_to_fr(&value_fr, vbytes);
    }
    size_t value_bits[64];
    gadget_unpack_bits(cs, value_bits, 64, &value_fr);

    /* Store value bits into note_contents */
    for (size_t i = 0; i < 64; i++)
        note_contents[nc_idx++] = value_bits[i];

    /* 1b. fixed_base_mul(G_v, value_bits) → value_point */
    struct fr gv_x, gv_y;
    {
        struct jub_point gv;
        const uint8_t pers[8] = {'Z','c','a','s','h','_','c','v'};
        const uint8_t tag[1] = {'v'};
        group_hash(&gv, tag, 1, pers);
        jub_get_x(&gv_x, &gv);
        jub_get_y(&gv_y, &gv);
    }
    size_t val_pt_x, val_pt_y;
    gadget_fixed_base_mul(cs, value_bits, 64, &gv_x, &gv_y,
                           &val_pt_x, &val_pt_y);

    /* 1c. Booleanize rcv (252 bits — Fs::CAPACITY) */
    struct fr rcv_fr;
    bytes_to_fr(&rcv_fr, wit->rcv);
    size_t rcv_bits[252];
    gadget_unpack_bits(cs, rcv_bits, 252, &rcv_fr);

    /* 1d. fixed_base_mul(G_rcv, rcv_bits) → rcv_point */
    struct fr grcv_x, grcv_y;
    {
        struct jub_point grcv;
        const uint8_t pers[8] = {'Z','c','a','s','h','_','c','v'};
        const uint8_t tag[1] = {'r'};
        group_hash(&grcv, tag, 1, pers);
        jub_get_x(&grcv_x, &grcv);
        jub_get_y(&grcv_y, &grcv);
    }
    size_t rcv_pt_x, rcv_pt_y;
    gadget_fixed_base_mul(cs, rcv_bits, 252, &grcv_x, &grcv_y,
                           &rcv_pt_x, &rcv_pt_y);

    /* 1e. cv = value_point + rcv_point */
    size_t cv_x, cv_y;
    gadget_edwards_add(cs, val_pt_x, val_pt_y, rcv_pt_x, rcv_pt_y,
                        &cv_x, &cv_y);

    /* 1f. Inputize cv (public inputs 1,2: cv.x, cv.y) */
    gadget_point_inputize(cs, cv_x, cv_y);

    /* ════════════════════════════════════════════════════════
     * Step 2: Witness g_d, verify not small order, compute repr
     * ════════════════════════════════════════════════════════ */

    /* Compute g_d from diversifier outside circuit */
    struct jub_point gd_point;
    sapling_diversifier_to_gd(&gd_point, wit->diversifier);
    struct fr gd_x_val, gd_y_val;
    jub_get_x(&gd_x_val, &gd_point);
    jub_get_y(&gd_y_val, &gd_point);

    /* Witness g_d as (x, y) with on-curve check */
    size_t gd_x = cs_alloc_aux(cs, &gd_x_val);
    size_t gd_y = cs_alloc_aux(cs, &gd_y_val);
    gadget_point_interpret(cs, gd_x, gd_y);

    /* Assert g_d is not small order */
    gadget_assert_not_small_order(cs, gd_x, gd_y);

    /* Compute repr of g_d: y_bits(255) + x_sign_bit(1) = 256 bits */
    {
        /* Unpack x into bits to get the sign bit (LSB of x) */
        size_t gd_x_bits[256];
        gadget_unpack_bits(cs, gd_x_bits, 256, &gd_x_val);

        /* Unpack y into bits */
        size_t gd_y_bits[256];
        gadget_unpack_bits(cs, gd_y_bits, 256, &gd_y_val);

        /* repr = y_bits(first 255) + x_bit0 */
        for (size_t i = 0; i < 255; i++)
            note_contents[nc_idx++] = gd_y_bits[i];
        note_contents[nc_idx++] = gd_x_bits[0]; /* x sign bit */
    }

    /* ════════════════════════════════════════════════════════
     * Step 3: epk = esk * g_d → inputize
     * ════════════════════════════════════════════════════════ */

    /* Booleanize esk (252 bits) */
    struct fr esk_fr;
    bytes_to_fr(&esk_fr, wit->esk);
    size_t esk_bits[252];
    gadget_unpack_bits(cs, esk_bits, 252, &esk_fr);

    /* Variable-base scalar mul: epk = g_d * esk */
    size_t epk_x, epk_y;
    gadget_variable_base_mul(cs, gd_x, gd_y, esk_bits, 252,
                              &epk_x, &epk_y);

    /* Inputize epk (public inputs 3,4: epk.x, epk.y) */
    gadget_point_inputize(cs, epk_x, epk_y);

    /* ════════════════════════════════════════════════════════
     * Step 4: pk_d witness — 256 bits for note contents
     * ════════════════════════════════════════════════════════ */

    {
        /* pk_d is witnessable as any 256 bits (no constraints).
         * Representation: y_bits(255) + x_sign_bit(1) */
        struct jub_point pkd_point;
        jub_from_bytes(&pkd_point, wit->pk_d);
        struct fr pkd_x_val, pkd_y_val;
        jub_get_x(&pkd_x_val, &pkd_point);
        jub_get_y(&pkd_y_val, &pkd_point);

        /* Unpack y into boolean vars */
        size_t pkd_y_bits[256];
        gadget_unpack_bits(cs, pkd_y_bits, 256, &pkd_y_val);

        /* Get x sign bit */
        uint8_t pkd_x_bytes[32];
        fr_to_bytes(pkd_x_bytes, &pkd_x_val);
        bool x_is_odd = pkd_x_bytes[0] & 1;
        size_t pkd_x_sign = gadget_alloc_boolean(cs, x_is_odd);

        /* repr = y_bits(first 255) + x_sign_bit */
        for (size_t i = 0; i < 255; i++)
            note_contents[nc_idx++] = pkd_y_bits[i];
        note_contents[nc_idx++] = pkd_x_sign;
    }

    /* ════════════════════════════════════════════════════════
     * Step 5: Note commitment via Pedersen hash
     * ════════════════════════════════════════════════════════ */

    /* note_contents should now have 64+256+256 = 576 bits */
    size_t cm_hash_x, cm_hash_y;
    gadget_pedersen_hash(cs, note_contents, 576,
                          "Zcash_PH", &cm_hash_x, &cm_hash_y);

    /* ════════════════════════════════════════════════════════
     * Step 6: Randomize note commitment: cm = hash + rcm*G_rcm
     * ════════════════════════════════════════════════════════ */

    /* Booleanize rcm (252 bits) */
    struct fr rcm_fr;
    bytes_to_fr(&rcm_fr, wit->rcm);
    size_t rcm_bits[252];
    gadget_unpack_bits(cs, rcm_bits, 252, &rcm_fr);

    /* fixed_base_mul(G_rcm, rcm_bits) */
    struct fr grcm_x, grcm_y;
    {
        struct jub_point grcm;
        const uint8_t pers[8] = {'Z','c','a','s','h','_','P','H'};
        const uint8_t tag[1] = {'r'};
        group_hash(&grcm, tag, 1, pers);
        jub_get_x(&grcm_x, &grcm);
        jub_get_y(&grcm_y, &grcm);
    }
    size_t rcm_pt_x, rcm_pt_y;
    gadget_fixed_base_mul(cs, rcm_bits, 252, &grcm_x, &grcm_y,
                           &rcm_pt_x, &rcm_pt_y);

    /* cm = hash_point + rcm_point */
    size_t cm_x, cm_y;
    gadget_edwards_add(cs, cm_hash_x, cm_hash_y, rcm_pt_x, rcm_pt_y,
                        &cm_x, &cm_y);

    /* Inputize cm.x only (public input 5) */
    gadget_scalar_inputize(cs, cm_x);

    /* note_contents holds the boolean-variable layout of the secret note
     * (value/g_d/pk_d bit witnesses) — wipe before free; it is fully
     * consumed by gadget_pedersen_hash above (output-neutral). */
    memory_cleanse(note_contents, 576 * sizeof(size_t));
    free(note_contents);

    printf("Output circuit synthesized: %zu vars, %zu constraints, "
           "%zu inputs\n", cs->num_vars, cs->num_constraints,
           cs->num_inputs);

    return true;
}

/* ── Full Proof Generation ──────────────────────────────────────── */

/* Serialize a Groth16 proof to 192 bytes (compressed):
 * A (G1 compressed, 48 bytes) + B (G2 compressed, 96 bytes) + C (G1 compressed, 48 bytes)
 *
 * Note: Zcash uses a specific serialization where:
 * A = 32 bytes (BLS12-381 G1 compressed)
 * B = 64 bytes (BLS12-381 G2 compressed)
 * C = 32 bytes (BLS12-381 G1 compressed)
 * But the standard format uses 48+96+48 = 192 bytes. */

static bool serialize_proof(uint8_t out[192], const struct groth16_proof *proof)
{
    /* BLS12-381 compressed point format:
     * bit 7 (0x80) = compressed flag (always set)
     * bit 6 (0x40) = infinity flag
     * bit 5 (0x20) = y-coordinate sign (set if y is lexicographically largest) */

    /* G1 point A (48 bytes compressed) */
    struct fp ax, ay;
    g1_to_affine(&ax, &ay, &proof->a);
    fp_to_bytes(out, &ax);
    out[0] &= 0x1F;
    out[0] |= 0x80;
    if (fp_lexicographically_largest(&ay))
        out[0] |= 0x20;

    /* G2 point B (96 bytes compressed: c1 || c0) */
    struct fp2 bx, by;
    g2_to_affine(&bx, &by, &proof->b);
    fp_to_bytes(out + 48, &bx.c1);
    fp_to_bytes(out + 48 + 48, &bx.c0);
    out[48] &= 0x1F;
    out[48] |= 0x80;
    if (fp2_lexicographically_largest(&by))
        out[48] |= 0x20;

    /* G1 point C (48 bytes compressed) */
    struct fp cx, cy;
    g1_to_affine(&cx, &cy, &proof->c);
    fp_to_bytes(out + 144, &cx);
    out[144] &= 0x1F;
    out[144] |= 0x80;
    if (fp_lexicographically_largest(&cy))
        out[144] |= 0x20;

    return true;
}

bool sapling_create_spend_proof(const uint8_t *pk_data, size_t pk_len,
                                 const struct sapling_spend_witness *wit,
                                 const struct sapling_spend_inputs *pub,
                                 uint8_t proof_out[192])
{
    /* Load proving key */
    struct groth16_pk pk;
    if (!groth16_pk_read(&pk, pk_data, pk_len))
        LOG_FAIL("sapling_circuit",
                 "create_spend_proof: groth16_pk_read failed (pk_len=%zu)", pk_len);

    /* Synthesize circuit */
    struct constraint_system cs;
    cs_init(&cs);

    if (!sapling_spend_synthesize(&cs, wit, pub)) {
        if (cs.witness)
            memory_cleanse(cs.witness, cs.cap_vars * sizeof(struct fr));
        cs_free(&cs);
        groth16_pk_free(&pk);
        LOG_FAIL("sapling_circuit",
                 "create_spend_proof: sapling_spend_synthesize failed");
    }

    /* Generate proof */
    struct groth16_proof proof;
    if (!groth16_prove(&pk, &cs, &proof)) {
        if (cs.witness)
            memory_cleanse(cs.witness, cs.cap_vars * sizeof(struct fr));
        cs_free(&cs);
        groth16_pk_free(&pk);
        LOG_FAIL("sapling_circuit",
                 "create_spend_proof: groth16_prove failed");
    }

    /* Serialize */
    serialize_proof(proof_out, &proof);

    /* The constraint witness vector holds the secret spend assignments
     * (ar, nsk, value, rcm, rcv, auth path). The proof is now produced;
     * wipe the witness scalars before freeing (output-neutral). */
    if (cs.witness)
        memory_cleanse(cs.witness, cs.cap_vars * sizeof(struct fr));
    cs_free(&cs);
    groth16_pk_free(&pk);
    return true;
}

bool sapling_create_output_proof(const uint8_t *pk_data, size_t pk_len,
                                  const struct sapling_output_witness *wit,
                                  const struct sapling_output_inputs *pub,
                                  uint8_t proof_out[192])
{
    struct groth16_pk pk;
    if (!groth16_pk_read(&pk, pk_data, pk_len))
        LOG_FAIL("sapling_circuit",
                 "create_output_proof: groth16_pk_read failed (pk_len=%zu)", pk_len);

    struct constraint_system cs;
    cs_init(&cs);

    if (!sapling_output_synthesize(&cs, wit, pub)) {
        if (cs.witness)
            memory_cleanse(cs.witness, cs.cap_vars * sizeof(struct fr));
        cs_free(&cs);
        groth16_pk_free(&pk);
        LOG_FAIL("sapling_circuit",
                 "create_output_proof: sapling_output_synthesize failed");
    }

    struct groth16_proof proof;
    if (!groth16_prove(&pk, &cs, &proof)) {
        if (cs.witness)
            memory_cleanse(cs.witness, cs.cap_vars * sizeof(struct fr));
        cs_free(&cs);
        groth16_pk_free(&pk);
        LOG_FAIL("sapling_circuit",
                 "create_output_proof: groth16_prove failed");
    }

    serialize_proof(proof_out, &proof);

    /* The constraint witness vector holds the secret output assignments
     * (value, rcv, esk, rcm). The proof is now produced; wipe the witness
     * scalars before freeing (output-neutral). */
    if (cs.witness)
        memory_cleanse(cs.witness, cs.cap_vars * sizeof(struct fr));
    cs_free(&cs);
    groth16_pk_free(&pk);
    return true;
}
