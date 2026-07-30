/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sapling OUTPUT circuit synthesis (5 public inputs, ~16K constraints):
 *   proves correct note commitment and ephemeral key derivation — plus the
 *   Groth16 proof serialization and the two public prove entry points shared
 *   with the spend circuit.
 *
 * The SPEND circuit lives in circuit_spend.c: it is a section-by-section port
 * of bellman's Spend::synthesize whose per-section constraint boundaries are
 * pinned against the reference trace, and keeping it in its own translation
 * unit is what lets that port grow without this file becoming a mega-module. */

#include "sapling/sapling_circuit.h"
#include "sapling/circuit_gadgets.h"
#include "sapling/pedersen_hash.h"
#include "sapling/sapling.h"
#include "support/cleanse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"
#include "util/log_macros.h"

/* ── Helper: convert bytes to Fr ────────────────────────────────── */

static void bytes_to_fr(struct fr *out, const uint8_t bytes[32])
{
    fr_from_bytes(out, bytes);
}

/* ── Output Circuit Synthesis ───────────────────────────────────── */

/* NOT at parity with Zcash sapling-crypto Output::synthesize — measured, not
 * assumed. This synthesis produces 7571 constraints / 7567 aux / 5 inputs; the
 * trusted-setup output proving key says num_aux == pk.l_len == 7821, so the
 * native circuit is ~254 aux and ~256 constraints SHORT of the reference, and
 * one input slot off. It therefore does not round-trip against the real key
 * either. Confirm with the H1 baseline line in the `groth16_selfverify` test
 * group ("OUTPUT match: num_aux==pk.l_len? NO").
 *
 * This matters beyond the output circuit: the shared gadgets it leans on
 * (gadget_pedersen_hash, bit decomposition, gadget_variable_base_mul) are the
 * same ones spend sections 13/15/16/17/19/21 will reuse, so they must not be
 * assumed reference-exact. Reaching output parity is its own port task; the
 * step list below is the intended shape, not a parity claim.
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
