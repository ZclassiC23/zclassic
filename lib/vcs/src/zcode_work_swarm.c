/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical codec and quorum verification for P2P ZCODE work. */

#include "vcs/zcode_work_swarm.h"

#include "vcs_priv.h"

#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"

#include <string.h>

#define ZCWS_CAPABILITY_BODY_BYTES 120u
#define ZCWS_CAPABILITY_BYTES 184u
#define ZCWS_REQUEST_BODY_BYTES 308u
#define ZCWS_REQUEST_BYTES 372u
#define ZCWS_RESULT_BYTES 592u
#define ZCWS_CANCEL_BODY_BYTES 80u
#define ZCWS_CANCEL_BYTES 144u

static const uint8_t zcws_magic[4] = { 'Z', 'C', 'W', 'S' };

static bool zcws_nonzero(const uint8_t *value, size_t len)
{
    uint8_t any = 0;
    for (size_t i = 0; i < len; i++) any |= value[i];
    return any != 0;
}

static bool zcws_zero(const uint8_t *value, size_t len)
{
    return !zcws_nonzero(value, len);
}

static bool zcws_work_kind(uint8_t kind)
{
    return kind >= VCS_ZCODE_WORK_PROPOSE &&
           kind <= VCS_ZCODE_WORK_DIAGNOSE;
}

static bool zcws_capability_valid(
    const struct vcs_zcode_work_capability_v1 *c)
{
    const uint32_t known = (UINT32_C(1) << VCS_ZCODE_WORK_PROPOSE) |
        (UINT32_C(1) << VCS_ZCODE_WORK_BUILD) |
        (UINT32_C(1) << VCS_ZCODE_WORK_TEST) |
        (UINT32_C(1) << VCS_ZCODE_WORK_FUZZ) |
        (UINT32_C(1) << VCS_ZCODE_WORK_REVIEW) |
        (UINT32_C(1) << VCS_ZCODE_WORK_REPRODUCE) |
        (UINT32_C(1) << VCS_ZCODE_WORK_DIAGNOSE);
    return c && zcws_nonzero(c->signer_pubkey, 32) &&
           zcws_nonzero(c->toolchain_capsule_root, 32) &&
           c->work_kinds != 0 && (c->work_kinds & ~known) == 0 &&
           c->target == VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3 &&
           (c->confinement & ~VCS_ZCODE_WORK_CONFINEMENT_V1_MASK) == 0 &&
           c->max_cpu_seconds > 0 &&
           c->max_memory_bytes > 0 &&
           c->max_memory_bytes <= VCS_ZCODE_TASK_MAX_MEMORY_BYTES &&
           c->max_output_bytes > 0 &&
           c->max_output_bytes <= VCS_ZCODE_TASK_MAX_OUTPUT_BYTES &&
           c->max_lease_seconds >= 5 && c->max_lease_seconds <= 600 &&
           c->slots > 0 && c->slots <= 64 &&
           c->queue_headroom <= c->slots && c->expires_unix > 0;
}

static bool zcws_request_valid(const struct vcs_zcode_work_request_v1 *r)
{
    if (!r || r->request_id == 0 ||
        !zcws_nonzero(r->requester_pubkey, 32) ||
        !zcws_nonzero(r->task_root, 32) || !zcws_work_kind(r->work_kind) ||
        !zcws_nonzero(r->action_root, 32) ||
        !zcws_nonzero(r->input_root, 32) ||
        !zcws_nonzero(r->context_root, 32) ||
        !zcws_nonzero(r->proof_policy_root, 32) ||
        !zcws_nonzero(r->toolchain_capsule_root, 32) ||
        r->target != VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3 ||
        r->max_cpu_seconds == 0 || r->max_memory_bytes == 0 ||
        r->max_memory_bytes > VCS_ZCODE_TASK_MAX_MEMORY_BYTES ||
        r->max_output_bytes == 0 ||
        r->max_output_bytes > VCS_ZCODE_TASK_MAX_OUTPUT_BYTES ||
        r->deadline_unix <= 0)
        return false;
    return r->work_kind == VCS_ZCODE_WORK_PROPOSE
        ? zcws_zero(r->candidate_root, 32)
        : zcws_nonzero(r->candidate_root, 32);
}

static bool zcws_result_shape(const struct vcs_zcode_work_result_v1 *r)
{
    return r && r->request_id != 0 && zcws_nonzero(r->task_root, 32) &&
           zcws_nonzero(r->candidate_root, 32) &&
           zcws_nonzero(r->action_root, 32) &&
           zcws_nonzero(r->output_root, 32) &&
           vcs_zcode_work_receipt_validate(&r->receipt) == VCS_ZCODE_DEV_OK;
}

static bool zcws_cancel_valid(const struct vcs_zcode_work_cancel_v1 *c)
{
    return c && c->request_id != 0 && zcws_nonzero(c->task_root, 32) &&
           zcws_nonzero(c->requester_pubkey, 32);
}

size_t vcs_zcode_work_swarm_wire_size(
    const struct vcs_zcode_work_swarm_message *m)
{
    if (!m) return 0;
    if (m->type == VCS_ZCODE_WORK_SWARM_CAPABILITY)
        return zcws_capability_valid(&m->body.capability)
            ? ZCWS_CAPABILITY_BYTES : 0;
    if (m->type == VCS_ZCODE_WORK_SWARM_REQUEST)
        return zcws_request_valid(&m->body.request) ? ZCWS_REQUEST_BYTES : 0;
    if (m->type == VCS_ZCODE_WORK_SWARM_RESULT)
        return zcws_result_shape(&m->body.result) ? ZCWS_RESULT_BYTES : 0;
    if (m->type == VCS_ZCODE_WORK_SWARM_CANCEL)
        return zcws_cancel_valid(&m->body.cancel) ? ZCWS_CANCEL_BYTES : 0;
    return 0;
}

static void zcws_header(uint8_t *out, uint8_t type)
{
    memcpy(out, zcws_magic, sizeof(zcws_magic));
    vcs_wr_u16le(out + 4, VCS_ZCODE_WORK_SWARM_VERSION);
    out[6] = type;
    out[7] = 0;
}

bool vcs_zcode_work_swarm_serialize(
    const struct vcs_zcode_work_swarm_message *m,
    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!out_len)
        LOG_FAIL("vcs.work_swarm", "null serialization length");
    *out_len = 0;
    size_t need = vcs_zcode_work_swarm_wire_size(m);
    if (!need || !out || out_cap < need)
        LOG_FAIL("vcs.work_swarm", "invalid message or short output");
    zcws_header(out, m->type);
    size_t off = VCS_ZCODE_WORK_SWARM_HEADER_BYTES;
    if (m->type == VCS_ZCODE_WORK_SWARM_CAPABILITY) {
        const struct vcs_zcode_work_capability_v1 *c = &m->body.capability;
        memcpy(out + off, c->signer_pubkey, 32); off += 32;
        memcpy(out + off, c->toolchain_capsule_root, 32); off += 32;
        vcs_wr_u32le(out + off, c->work_kinds); off += 4;
        vcs_wr_u32le(out + off, c->target); off += 4;
        vcs_wr_u32le(out + off, c->confinement); off += 4;
        vcs_wr_u32le(out + off, c->max_cpu_seconds); off += 4;
        vcs_wr_u64le(out + off, c->max_memory_bytes); off += 8;
        vcs_wr_u64le(out + off, c->max_output_bytes); off += 8;
        vcs_wr_u32le(out + off, c->max_lease_seconds); off += 4;
        vcs_wr_u16le(out + off, c->slots); off += 2;
        vcs_wr_u16le(out + off, c->queue_headroom); off += 2;
        vcs_wr_u64le(out + off, (uint64_t)c->expires_unix); off += 8;
        memcpy(out + off, c->signature, 64); off += 64;
    } else if (m->type == VCS_ZCODE_WORK_SWARM_REQUEST) {
        const struct vcs_zcode_work_request_v1 *r = &m->body.request;
        vcs_wr_u64le(out + off, r->request_id); off += 8;
        memcpy(out + off, r->requester_pubkey, 32); off += 32;
        memcpy(out + off, r->task_root, 32); off += 32;
        memcpy(out + off, r->candidate_root, 32); off += 32;
        memcpy(out + off, r->action_root, 32); off += 32;
        memcpy(out + off, r->input_root, 32); off += 32;
        memcpy(out + off, r->context_root, 32); off += 32;
        memcpy(out + off, r->proof_policy_root, 32); off += 32;
        memcpy(out + off, r->toolchain_capsule_root, 32); off += 32;
        out[off++] = r->work_kind;
        out[off++] = r->target;
        memset(out + off, 0, 6); off += 6;
        vcs_wr_u32le(out + off, r->max_cpu_seconds); off += 4;
        vcs_wr_u64le(out + off, r->max_memory_bytes); off += 8;
        vcs_wr_u64le(out + off, r->max_output_bytes); off += 8;
        vcs_wr_u64le(out + off, (uint64_t)r->deadline_unix); off += 8;
        memcpy(out + off, r->signature, 64); off += 64;
    } else if (m->type == VCS_ZCODE_WORK_SWARM_RESULT) {
        const struct vcs_zcode_work_result_v1 *r = &m->body.result;
        vcs_wr_u64le(out + off, r->request_id); off += 8;
        memcpy(out + off, r->task_root, 32); off += 32;
        memcpy(out + off, r->candidate_root, 32); off += 32;
        memcpy(out + off, r->action_root, 32); off += 32;
        memcpy(out + off, r->output_root, 32); off += 32;
        if (vcs_zcode_work_receipt_serialize(&r->receipt, out + off) !=
            VCS_ZCODE_DEV_OK)
            LOG_FAIL("vcs.work_swarm", "receipt serialization failed");
        off += VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES;
    } else {
        vcs_wr_u64le(out + off, m->body.cancel.request_id); off += 8;
        memcpy(out + off, m->body.cancel.task_root, 32); off += 32;
        memcpy(out + off, m->body.cancel.requester_pubkey, 32); off += 32;
        memcpy(out + off, m->body.cancel.signature, 64); off += 64;
    }
    if (off != need)
        LOG_FAIL("vcs.work_swarm", "wire size invariant failed");
    *out_len = off;
    return true;
}

static bool zcws_signed_id(uint8_t type, const void *object, uint8_t out[32])
{
    if (!object || !out) return false;
    struct vcs_zcode_work_swarm_message message = { .type = type };
    const char *domain = NULL;
    size_t domain_len = 0, body_len = 0;
    if (type == VCS_ZCODE_WORK_SWARM_CAPABILITY) {
        message.body.capability =
            *(const struct vcs_zcode_work_capability_v1 *)object;
        static const char d[] = "zcl.zcode.work_capability.v1";
        domain = d; domain_len = sizeof(d); body_len = ZCWS_CAPABILITY_BODY_BYTES;
    } else if (type == VCS_ZCODE_WORK_SWARM_REQUEST) {
        message.body.request =
            *(const struct vcs_zcode_work_request_v1 *)object;
        static const char d[] = "zcl.zcode.work_request.v1";
        domain = d; domain_len = sizeof(d); body_len = ZCWS_REQUEST_BODY_BYTES;
    } else if (type == VCS_ZCODE_WORK_SWARM_CANCEL) {
        message.body.cancel =
            *(const struct vcs_zcode_work_cancel_v1 *)object;
        static const char d[] = "zcl.zcode.work_cancel.v1";
        domain = d; domain_len = sizeof(d); body_len = ZCWS_CANCEL_BODY_BYTES;
    } else {
        return false;
    }
    uint8_t wire[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES];
    size_t wire_len = 0;
    if (!vcs_zcode_work_swarm_serialize(&message, wire, sizeof(wire),
                                        &wire_len) || wire_len < body_len)
        return false;
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, domain_len);
    sha3_256_write(&sha, wire, body_len);
    sha3_256_finalize(&sha, out);
    return true;
}

bool vcs_zcode_work_capability_seal(
    struct vcs_zcode_work_capability_v1 *c,
    const uint8_t secret[32], const uint8_t pubkey[32])
{
    if (!c || !secret || !pubkey || !zcws_nonzero(pubkey, 32)) return false;
    memcpy(c->signer_pubkey, pubkey, 32);
    uint8_t id[32];
    if (!zcws_signed_id(VCS_ZCODE_WORK_SWARM_CAPABILITY, c, id)) return false;
    ed25519_sign(c->signature, id, sizeof(id), secret, pubkey);
    return true;
}

bool vcs_zcode_work_capability_verify(
    const struct vcs_zcode_work_capability_v1 *c)
{
    uint8_t id[32];
    return zcws_capability_valid(c) &&
           zcws_signed_id(VCS_ZCODE_WORK_SWARM_CAPABILITY, c, id) &&
           ed25519_verify(c->signature, id, sizeof(id), c->signer_pubkey);
}

bool vcs_zcode_work_request_seal(
    struct vcs_zcode_work_request_v1 *r,
    const uint8_t secret[32], const uint8_t pubkey[32])
{
    if (!r || !secret || !pubkey || !zcws_nonzero(pubkey, 32)) return false;
    memcpy(r->requester_pubkey, pubkey, 32);
    uint8_t id[32];
    if (!zcws_signed_id(VCS_ZCODE_WORK_SWARM_REQUEST, r, id)) return false;
    ed25519_sign(r->signature, id, sizeof(id), secret, pubkey);
    return true;
}

bool vcs_zcode_work_request_verify(const struct vcs_zcode_work_request_v1 *r)
{
    uint8_t id[32];
    return zcws_request_valid(r) &&
           zcws_signed_id(VCS_ZCODE_WORK_SWARM_REQUEST, r, id) &&
           ed25519_verify(r->signature, id, sizeof(id), r->requester_pubkey);
}

bool vcs_zcode_work_cancel_seal(
    struct vcs_zcode_work_cancel_v1 *c,
    const uint8_t secret[32], const uint8_t pubkey[32])
{
    if (!c || !secret || !pubkey || !zcws_nonzero(pubkey, 32)) return false;
    memcpy(c->requester_pubkey, pubkey, 32);
    uint8_t id[32];
    if (!zcws_signed_id(VCS_ZCODE_WORK_SWARM_CANCEL, c, id)) return false;
    ed25519_sign(c->signature, id, sizeof(id), secret, pubkey);
    return true;
}

bool vcs_zcode_work_cancel_verify(const struct vcs_zcode_work_cancel_v1 *c)
{
    uint8_t id[32];
    return zcws_cancel_valid(c) &&
           zcws_signed_id(VCS_ZCODE_WORK_SWARM_CANCEL, c, id) &&
           ed25519_verify(c->signature, id, sizeof(id), c->requester_pubkey);
}

static bool zcws_header_valid(const uint8_t *wire, size_t len, uint8_t *type)
{
    if (!wire || len < VCS_ZCODE_WORK_SWARM_HEADER_BYTES ||
        len > VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES ||
        memcmp(wire, zcws_magic, sizeof(zcws_magic)) != 0 ||
        vcs_rd_u16le(wire + 4) != VCS_ZCODE_WORK_SWARM_VERSION || wire[7])
        return false;
    *type = wire[6];
    return true;
}

bool vcs_zcode_work_swarm_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_work_swarm_message *out)
{
    if (!out) LOG_FAIL("vcs.work_swarm", "null parse output");
    memset(out, 0, sizeof(*out));
    uint8_t type = 0;
    if (!zcws_header_valid(wire, len, &type))
        LOG_FAIL("vcs.work_swarm", "invalid work swarm header");
    out->type = type;
    size_t off = VCS_ZCODE_WORK_SWARM_HEADER_BYTES;
    if (type == VCS_ZCODE_WORK_SWARM_CAPABILITY &&
        len == ZCWS_CAPABILITY_BYTES) {
        struct vcs_zcode_work_capability_v1 *c = &out->body.capability;
        memcpy(c->signer_pubkey, wire + off, 32); off += 32;
        memcpy(c->toolchain_capsule_root, wire + off, 32); off += 32;
        c->work_kinds = vcs_rd_u32le(wire + off); off += 4;
        c->target = vcs_rd_u32le(wire + off); off += 4;
        c->confinement = vcs_rd_u32le(wire + off); off += 4;
        c->max_cpu_seconds = vcs_rd_u32le(wire + off); off += 4;
        c->max_memory_bytes = vcs_rd_u64le(wire + off); off += 8;
        c->max_output_bytes = vcs_rd_u64le(wire + off); off += 8;
        c->max_lease_seconds = vcs_rd_u32le(wire + off); off += 4;
        c->slots = vcs_rd_u16le(wire + off); off += 2;
        c->queue_headroom = vcs_rd_u16le(wire + off); off += 2;
        c->expires_unix = (int64_t)vcs_rd_u64le(wire + off); off += 8;
        memcpy(c->signature, wire + off, 64); off += 64;
        if (!zcws_capability_valid(c) ||
            !vcs_zcode_work_capability_verify(c)) goto reject;
    } else if (type == VCS_ZCODE_WORK_SWARM_REQUEST &&
               len == ZCWS_REQUEST_BYTES) {
        struct vcs_zcode_work_request_v1 *r = &out->body.request;
        r->request_id = vcs_rd_u64le(wire + off); off += 8;
        memcpy(r->requester_pubkey, wire + off, 32); off += 32;
        memcpy(r->task_root, wire + off, 32); off += 32;
        memcpy(r->candidate_root, wire + off, 32); off += 32;
        memcpy(r->action_root, wire + off, 32); off += 32;
        memcpy(r->input_root, wire + off, 32); off += 32;
        memcpy(r->context_root, wire + off, 32); off += 32;
        memcpy(r->proof_policy_root, wire + off, 32); off += 32;
        memcpy(r->toolchain_capsule_root, wire + off, 32); off += 32;
        r->work_kind = wire[off++];
        r->target = wire[off++];
        for (size_t i = 0; i < 6; i++) if (wire[off + i]) goto reject;
        off += 6;
        r->max_cpu_seconds = vcs_rd_u32le(wire + off); off += 4;
        r->max_memory_bytes = vcs_rd_u64le(wire + off); off += 8;
        r->max_output_bytes = vcs_rd_u64le(wire + off); off += 8;
        r->deadline_unix = (int64_t)vcs_rd_u64le(wire + off); off += 8;
        memcpy(r->signature, wire + off, 64); off += 64;
        if (!zcws_request_valid(r) || !vcs_zcode_work_request_verify(r))
            goto reject;
    } else if (type == VCS_ZCODE_WORK_SWARM_RESULT &&
               len == ZCWS_RESULT_BYTES) {
        struct vcs_zcode_work_result_v1 *r = &out->body.result;
        r->request_id = vcs_rd_u64le(wire + off); off += 8;
        memcpy(r->task_root, wire + off, 32); off += 32;
        memcpy(r->candidate_root, wire + off, 32); off += 32;
        memcpy(r->action_root, wire + off, 32); off += 32;
        memcpy(r->output_root, wire + off, 32); off += 32;
        if (vcs_zcode_work_receipt_parse(
                wire + off, VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES,
                &r->receipt) != VCS_ZCODE_DEV_OK)
            goto reject;
        off += VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES;
        if (!zcws_result_shape(r)) goto reject;
    } else if (type == VCS_ZCODE_WORK_SWARM_CANCEL &&
               len == ZCWS_CANCEL_BYTES) {
        out->body.cancel.request_id = vcs_rd_u64le(wire + off); off += 8;
        memcpy(out->body.cancel.task_root, wire + off, 32); off += 32;
        memcpy(out->body.cancel.requester_pubkey, wire + off, 32); off += 32;
        memcpy(out->body.cancel.signature, wire + off, 64); off += 64;
        if (!zcws_cancel_valid(&out->body.cancel) ||
            !vcs_zcode_work_cancel_verify(&out->body.cancel)) goto reject;
    } else {
        goto reject;
    }
    if (off != len) goto reject;
    return true;
reject:
    memset(out, 0, sizeof(*out));
    LOG_FAIL("vcs.work_swarm", "noncanonical work swarm payload");
}

bool vcs_zcode_work_result_verify(
    const struct vcs_zcode_work_request_v1 *q,
    const struct vcs_zcode_work_result_v1 *r,
    const uint8_t expected_signer[32])
{
    if (!zcws_request_valid(q) || !zcws_result_shape(r) ||
        !expected_signer || q->request_id != r->request_id ||
        memcmp(q->task_root, r->task_root, 32) != 0 ||
        memcmp(q->action_root, r->action_root, 32) != 0 ||
        memcmp(r->task_root, r->receipt.task_root, 32) != 0 ||
        memcmp(r->candidate_root, r->receipt.candidate_root, 32) != 0 ||
        memcmp(r->action_root, r->receipt.action_root, 32) != 0 ||
        memcmp(q->input_root, r->receipt.input_root, 32) != 0 ||
        memcmp(r->output_root, r->receipt.output_root, 32) != 0 ||
        memcmp(q->proof_policy_root, r->receipt.proof_policy_root, 32) != 0 ||
        memcmp(q->toolchain_capsule_root,
               r->receipt.toolchain_capsule_root, 32) != 0 ||
        r->receipt.work_kind != q->work_kind ||
        r->receipt.status != VCS_ZCODE_WORK_PASS)
        return false;
    if (q->work_kind != VCS_ZCODE_WORK_PROPOSE &&
        memcmp(q->candidate_root, r->candidate_root, 32) != 0)
        return false;
    return vcs_zcode_work_receipt_verify(&r->receipt, expected_signer) ==
           VCS_ZCODE_DEV_OK;
}

size_t vcs_zcode_work_result_quorum(
    const struct vcs_zcode_work_request_v1 *request,
    const struct vcs_zcode_work_result_v1 *results, size_t result_count,
    const uint8_t (*approved)[32], size_t approved_count,
    size_t required, uint8_t output_root[32])
{
    if (output_root) memset(output_root, 0, 32);
    if (!request || !results || !approved || !output_root || required == 0 ||
        result_count > VCS_ZCODE_WORK_SWARM_MAX_RESULTS ||
        approved_count > VCS_ZCODE_WORK_SWARM_MAX_APPROVED)
        return 0;
    uint8_t counted[VCS_ZCODE_WORK_SWARM_MAX_APPROVED][32];
    size_t count = 0;
    for (size_t i = 0; i < result_count; i++) {
        const uint8_t *signer = results[i].receipt.signer_pubkey;
        bool allowed = false, duplicate = false;
        for (size_t j = 0; j < approved_count; j++)
            if (memcmp(signer, approved[j], 32) == 0) allowed = true;
        for (size_t j = 0; j < count; j++)
            if (memcmp(signer, counted[j], 32) == 0) duplicate = true;
        if (!allowed || duplicate ||
            !vcs_zcode_work_result_verify(request, &results[i], signer))
            continue;
        if (count == 0)
            memcpy(output_root, results[i].output_root, 32);
        else if (memcmp(output_root, results[i].output_root, 32) != 0)
            continue;
        memcpy(counted[count++], signer, 32);
    }
    if (count < required) memset(output_root, 0, 32);
    return count;
}
