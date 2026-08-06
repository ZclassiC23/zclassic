/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: derive and authenticate evidence-bound ZC23 Score receipts. */

#include "vcs/zcode_score_receipt.h"

#include "codec/cursor.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"

#include <string.h>

static const uint8_t score_magic[8] = {
    'Z', 'C', 'S', 'C', 'O', 'R', '\r', '\n'
};

static bool score_nonzero(const uint8_t root[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++) any |= root[i];
    return any != 0;
}

static uint8_t score_popcount(uint8_t value)
{
    uint8_t count = 0;
    while (value) { count += value & 1u; value >>= 1u; }
    return count;
}

const char *vcs_zcode_score_error_string(enum vcs_zcode_score_error error)
{
    switch (error) {
    case VCS_ZCODE_SCORE_OK: return "ok";
    case VCS_ZCODE_SCORE_NULL: return "null-argument";
    case VCS_ZCODE_SCORE_SHAPE: return "noncanonical-score";
    case VCS_ZCODE_SCORE_BINDING: return "score-binding-mismatch";
    case VCS_ZCODE_SCORE_PROOF: return "unverified-score-proof";
    case VCS_ZCODE_SCORE_DUPLICATE: return "duplicate-score-proof";
    case VCS_ZCODE_SCORE_SIGNATURE: return "score-signature-invalid";
    }
    return "unknown-score-error";
}

const char *vcs_zcode_score_unit_name(enum vcs_zcode_score_unit unit)
{
    switch (unit) {
    case VCS_ZCODE_SCORE_ACCEPTED_EXTRACTION: return "accepted_extraction";
    case VCS_ZCODE_SCORE_BORN_RED_DEFECT_TEST: return "born_red_defect_test";
    case VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION:
        return "independent_reproduction";
    case VCS_ZCODE_SCORE_EXACT_CAPSULE_REDERIVATION:
        return "exact_capsule_rederivation";
    case VCS_ZCODE_SCORE_COMPATIBILITY_MAINTENANCE:
        return "compatibility_maintenance";
    }
    return "unknown";
}

void vcs_zcode_score_action_root(enum vcs_zcode_score_unit unit,
                                 uint8_t out[32])
{
    if (!out) return;
    struct sha3_256_ctx sha;
    static const char domain[] = "zcl.zcode.score_action.v1";
    const char *name = vcs_zcode_score_unit_name(unit);
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, (const uint8_t *)name, strlen(name) + 1u);
    sha3_256_finalize(&sha, out);
}

bool vcs_zcode_score_offhost_reproducer_approved(const uint8_t pubkey[32])
{
    /* Fail closed until an owner-reviewed off-host key is registered. */
    (void)pubkey;
    return false;
}

static enum vcs_zcode_score_error score_fields(
    const struct vcs_zcode_score_receipt_v1 *receipt, bool signed_wire)
{
    if (!receipt) return VCS_ZCODE_SCORE_NULL;
    if (receipt->schema_version != VCS_ZCODE_SCORE_VERSION ||
        (receipt->awarded_mask & ~UINT8_C(0x1f)) != 0 ||
        receipt->score != score_popcount(receipt->awarded_mask))
        return VCS_ZCODE_SCORE_SHAPE;
    const uint8_t *roots[] = {
        receipt->task_root, receipt->candidate_root,
        receipt->proof_policy_root, receipt->proof_set_root,
        receipt->proven_lane_root, receipt->package_root,
        receipt->release_root, receipt->recipe_root,
        receipt->dependency_lock_root, receipt->api_capsule_root,
        receipt->lane_signer,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!score_nonzero(roots[i])) return VCS_ZCODE_SCORE_SHAPE;
    for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
        bool awarded = (receipt->awarded_mask & (UINT8_C(1) << i)) != 0;
        if (awarded && !score_nonzero(receipt->evidence_roots[i]))
            return VCS_ZCODE_SCORE_SHAPE;
    }
    if (receipt->awarded_mask &
        (UINT8_C(1) << VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION))
        return VCS_ZCODE_SCORE_SHAPE;
    if (signed_wire && !score_nonzero(receipt->signature))
        return VCS_ZCODE_SCORE_SIGNATURE;
    return VCS_ZCODE_SCORE_OK;
}

static enum vcs_zcode_score_error score_body(
    const struct vcs_zcode_score_receipt_v1 *receipt,
    uint8_t out[VCS_ZCODE_SCORE_BODY_BYTES])
{
    enum vcs_zcode_score_error err = score_fields(receipt, false);
    if (err != VCS_ZCODE_SCORE_OK || !out) return err;
    struct zcl_codec_writer writer;
    zcl_codec_writer_init(&writer, out, VCS_ZCODE_SCORE_BODY_BYTES);
    bool ok = zcl_codec_write_bytes(&writer, score_magic,
                                    sizeof(score_magic)) &&
        zcl_codec_write_u16le(&writer, receipt->schema_version) &&
        zcl_codec_write_u8(&writer, receipt->awarded_mask) &&
        zcl_codec_write_u8(&writer, receipt->score) &&
        zcl_codec_write_u32le(&writer, 0) &&
        zcl_codec_write_bytes(&writer, receipt->task_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->candidate_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->proof_policy_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->proof_set_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->proven_lane_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->package_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->release_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->recipe_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->dependency_lock_root, 32) &&
        zcl_codec_write_bytes(&writer, receipt->api_capsule_root, 32);
    for (size_t i = 0; ok && i < VCS_ZCODE_SCORE_UNITS; i++)
        ok = zcl_codec_write_bytes(&writer, receipt->evidence_roots[i], 32);
    ok = ok && zcl_codec_write_bytes(&writer, receipt->lane_signer, 32);
    size_t written = 0;
    return ok && zcl_codec_writer_finish(&writer, &written) &&
           written == VCS_ZCODE_SCORE_BODY_BYTES
        ? VCS_ZCODE_SCORE_OK : VCS_ZCODE_SCORE_SHAPE;
}

enum vcs_zcode_score_error vcs_zcode_score_receipt_body(
    const struct vcs_zcode_score_receipt_v1 *receipt,
    uint8_t out[VCS_ZCODE_SCORE_BODY_BYTES])
{
    if (!out) return VCS_ZCODE_SCORE_NULL;
    return score_body(receipt, out);
}

enum vcs_zcode_score_error vcs_zcode_score_receipt_id(
    const struct vcs_zcode_score_receipt_v1 *receipt, uint8_t out[32])
{
    if (!out) return VCS_ZCODE_SCORE_NULL;
    uint8_t body[VCS_ZCODE_SCORE_BODY_BYTES];
    enum vcs_zcode_score_error err = score_body(receipt, body);
    if (err != VCS_ZCODE_SCORE_OK) return err;
    struct sha3_256_ctx sha;
    static const char domain[] = VCS_ZCODE_SCORE_DOMAIN;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, body, sizeof(body));
    sha3_256_finalize(&sha, out);
    return VCS_ZCODE_SCORE_OK;
}

enum vcs_zcode_score_error vcs_zcode_score_receipt_serialize(
    const struct vcs_zcode_score_receipt_v1 *receipt,
    uint8_t out[VCS_ZCODE_SCORE_WIRE_BYTES])
{
    if (!out) return VCS_ZCODE_SCORE_NULL;
    enum vcs_zcode_score_error err = score_fields(receipt, true);
    if (err != VCS_ZCODE_SCORE_OK) return err;
    err = score_body(receipt, out);
    if (err == VCS_ZCODE_SCORE_OK)
        memcpy(out + VCS_ZCODE_SCORE_BODY_BYTES, receipt->signature, 64);
    return err;
}

enum vcs_zcode_score_error vcs_zcode_score_receipt_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_score_receipt_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_SCORE_NULL;
    if (wire_len != VCS_ZCODE_SCORE_WIRE_BYTES)
        return VCS_ZCODE_SCORE_SHAPE;
    memset(out, 0, sizeof(*out));
    struct zcl_codec_reader reader;
    uint8_t magic[8]; uint32_t reserved;
    zcl_codec_reader_init(&reader, wire, wire_len);
    bool ok = zcl_codec_read_bytes(&reader, magic, sizeof(magic)) &&
        zcl_codec_read_u16le(&reader, &out->schema_version) &&
        zcl_codec_read_u8(&reader, &out->awarded_mask) &&
        zcl_codec_read_u8(&reader, &out->score) &&
        zcl_codec_read_u32le(&reader, &reserved) &&
        zcl_codec_read_bytes(&reader, out->task_root, 32) &&
        zcl_codec_read_bytes(&reader, out->candidate_root, 32) &&
        zcl_codec_read_bytes(&reader, out->proof_policy_root, 32) &&
        zcl_codec_read_bytes(&reader, out->proof_set_root, 32) &&
        zcl_codec_read_bytes(&reader, out->proven_lane_root, 32) &&
        zcl_codec_read_bytes(&reader, out->package_root, 32) &&
        zcl_codec_read_bytes(&reader, out->release_root, 32) &&
        zcl_codec_read_bytes(&reader, out->recipe_root, 32) &&
        zcl_codec_read_bytes(&reader, out->dependency_lock_root, 32) &&
        zcl_codec_read_bytes(&reader, out->api_capsule_root, 32);
    for (size_t i = 0; ok && i < VCS_ZCODE_SCORE_UNITS; i++)
        ok = zcl_codec_read_bytes(&reader, out->evidence_roots[i], 32);
    ok = ok && zcl_codec_read_bytes(&reader, out->lane_signer, 32) &&
        zcl_codec_read_bytes(&reader, out->signature, 64) &&
        zcl_codec_reader_finish(&reader) && reserved == 0 &&
        memcmp(magic, score_magic, sizeof(magic)) == 0;
    if (!ok) { memset(out, 0, sizeof(*out)); return VCS_ZCODE_SCORE_SHAPE; }
    return score_fields(out, true);
}

enum vcs_zcode_score_error vcs_zcode_score_receipt_seal(
    struct vcs_zcode_score_receipt_v1 *receipt,
    const uint8_t secret[32], const uint8_t pubkey[32])
{
    if (!receipt || !secret || !pubkey) return VCS_ZCODE_SCORE_NULL;
    if (memcmp(receipt->lane_signer, pubkey, 32) != 0)
        return VCS_ZCODE_SCORE_BINDING;
    uint8_t id[32];
    enum vcs_zcode_score_error err = vcs_zcode_score_receipt_id(receipt, id);
    if (err != VCS_ZCODE_SCORE_OK) return err;
    ed25519_sign(receipt->signature, id, sizeof(id), secret, pubkey);
    return VCS_ZCODE_SCORE_OK;
}

enum vcs_zcode_score_error vcs_zcode_score_receipt_verify(
    const struct vcs_zcode_score_receipt_v1 *receipt)
{
    enum vcs_zcode_score_error err = score_fields(receipt, true);
    if (err != VCS_ZCODE_SCORE_OK) return err;
    uint8_t id[32];
    err = vcs_zcode_score_receipt_id(receipt, id);
    if (err != VCS_ZCODE_SCORE_OK) return err;
    return ed25519_verify(receipt->signature, id, sizeof(id),
                          receipt->lane_signer)
        ? VCS_ZCODE_SCORE_OK : VCS_ZCODE_SCORE_SIGNATURE;
}

static int score_unit_for_receipt(
    const struct vcs_zcode_work_receipt_v1 *receipt)
{
    for (int i = 0; i < (int)VCS_ZCODE_SCORE_UNITS; i++) {
        uint8_t action[32];
        vcs_zcode_score_action_root((enum vcs_zcode_score_unit)i, action);
        if (memcmp(action, receipt->action_root, 32) == 0) return i;
    }
    return -1;
}

static uint8_t score_unit_work_kind(enum vcs_zcode_score_unit unit)
{
    switch (unit) {
    case VCS_ZCODE_SCORE_ACCEPTED_EXTRACTION: return VCS_ZCODE_WORK_REVIEW;
    case VCS_ZCODE_SCORE_BORN_RED_DEFECT_TEST: return VCS_ZCODE_WORK_TEST;
    case VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION:
        return VCS_ZCODE_WORK_REPRODUCE;
    case VCS_ZCODE_SCORE_EXACT_CAPSULE_REDERIVATION:
    case VCS_ZCODE_SCORE_COMPATIBILITY_MAINTENANCE:
        return VCS_ZCODE_WORK_BUILD;
    }
    return 0;
}

enum vcs_zcode_score_error vcs_zcode_score_plan(
    const struct vcs_zcode_score_plan_input *input,
    struct vcs_zcode_score_receipt_v1 *out)
{
    if (!input || !out || !input->task || !input->candidate ||
        !input->proof_policy || !input->proven_lane ||
        !input->proof_receipt_roots || !input->work_receipts ||
        !input->package_root || !input->release_root || !input->recipe_root ||
        !input->dependency_lock_root || !input->api_capsule_root)
        return VCS_ZCODE_SCORE_NULL;
    if (input->work_receipt_count == 0 ||
        input->work_receipt_count > VCS_ZCODE_PROOF_SET_MAX_RECEIPTS)
        return VCS_ZCODE_SCORE_SHAPE;
    memset(out, 0, sizeof(*out));
    out->schema_version = VCS_ZCODE_SCORE_VERSION;
    if (vcs_zcode_task_root(input->task, out->task_root) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_root(input->candidate, out->candidate_root) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_policy_root(input->proof_policy,
                                    out->proof_policy_root) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_set_root(input->proof_receipt_roots,
                                 input->work_receipt_count,
                                 out->proof_set_root) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_id(input->proven_lane,
                                  out->proven_lane_root) != VCS_ZCODE_DEV_OK)
        return VCS_ZCODE_SCORE_SHAPE;
    if (input->proven_lane->lane != VCS_ZCODE_LANE_PROVEN ||
        vcs_zcode_lane_receipt_verify(
            input->proven_lane, input->proven_lane->signer_pubkey) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_validate_for_candidate(
            input->proven_lane, input->task, input->candidate,
            input->proof_policy) != VCS_ZCODE_DEV_OK ||
        memcmp(input->proven_lane->proof_set_root,
               out->proof_set_root, 32) != 0)
        return VCS_ZCODE_SCORE_BINDING;
    memcpy(out->package_root, input->package_root, 32);
    memcpy(out->release_root, input->release_root, 32);
    memcpy(out->recipe_root, input->recipe_root, 32);
    memcpy(out->dependency_lock_root, input->dependency_lock_root, 32);
    memcpy(out->api_capsule_root, input->api_capsule_root, 32);
    memcpy(out->lane_signer, input->proven_lane->signer_pubkey, 32);
    if (!score_nonzero(out->package_root) ||
        !score_nonzero(out->release_root) ||
        !score_nonzero(out->recipe_root) ||
        memcmp(out->dependency_lock_root,
               input->task->dependency_lock_root, 32) != 0 ||
        memcmp(out->api_capsule_root,
               input->task->toolchain_capsule_root, 32) != 0)
        return VCS_ZCODE_SCORE_BINDING;
    for (size_t i = 0; i < input->work_receipt_count; i++) {
        const struct vcs_zcode_work_receipt_v1 *work =
            &input->work_receipts[i];
        uint8_t id[32];
        if (vcs_zcode_work_receipt_id(work, id) != VCS_ZCODE_DEV_OK ||
            memcmp(id, input->proof_receipt_roots[i], 32) != 0 ||
            vcs_zcode_work_receipt_verify(work, work->signer_pubkey) !=
                VCS_ZCODE_DEV_OK ||
            work->status != VCS_ZCODE_WORK_PASS || work->exit_status != 0 ||
            memcmp(work->task_root, out->task_root, 32) != 0 ||
            memcmp(work->candidate_root, out->candidate_root, 32) != 0 ||
            memcmp(work->proof_policy_root, out->proof_policy_root, 32) != 0 ||
            memcmp(work->toolchain_capsule_root,
                   out->api_capsule_root, 32) != 0)
            return VCS_ZCODE_SCORE_PROOF;
        int unit = score_unit_for_receipt(work);
        if (unit < 0) continue; /* Opaque/self-reported work earns nothing. */
        if (work->work_kind !=
            score_unit_work_kind((enum vcs_zcode_score_unit)unit))
            continue;
        if (score_nonzero(out->evidence_roots[unit]))
            return VCS_ZCODE_SCORE_DUPLICATE;
        memcpy(out->evidence_roots[unit], work->evidence_root, 32);
        bool awarded = unit != VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION ||
            vcs_zcode_score_offhost_reproducer_approved(work->signer_pubkey);
        if (awarded) out->awarded_mask |= UINT8_C(1) << unit;
    }
    out->score = score_popcount(out->awarded_mask);
    return score_fields(out, false);
}
