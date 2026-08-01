/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical SHA3-addressed ZCODE development object wires. */

#include "vcs/zcode_dev.h"

#include "base/serialize_le.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"

#include <string.h>

static const uint8_t task_magic[8] = {'Z','C','T','A','S','K','\r','\n'};
static const uint8_t candidate_magic[8] = {'Z','C','C','A','N','D','\r','\n'};
static const uint8_t policy_magic[8] = {'Z','C','P','O','L','Y','\r','\n'};
static const uint8_t review_magic[8] = {'Z','C','R','E','V','W','\r','\n'};
static const uint8_t receipt_magic[8] = {'Z','C','W','R','C','P','\r','\n'};

static bool nonzero(const uint8_t root[32])
{
    uint8_t any = 0;
    if (!root) return false;
    for (size_t i = 0; i < 32; i++) any |= root[i];
    return any != 0;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t len)
{
    uint8_t any = 0;
    if (!bytes) return false;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

const char *vcs_zcode_dev_error_string(enum vcs_zcode_dev_error error)
{
    switch (error) {
    case VCS_ZCODE_DEV_OK: return "ok";
    case VCS_ZCODE_DEV_ERR_NULL: return "null-argument";
    case VCS_ZCODE_DEV_ERR_VERSION: return "schema-version";
    case VCS_ZCODE_DEV_ERR_WIRE_SIZE: return "wire-size";
    case VCS_ZCODE_DEV_ERR_WIRE_MAGIC: return "wire-magic";
    case VCS_ZCODE_DEV_ERR_ROOT_ZERO: return "root-zero";
    case VCS_ZCODE_DEV_ERR_PUBKEY_ZERO: return "pubkey-zero";
    case VCS_ZCODE_DEV_ERR_SIGNATURE: return "signature-invalid";
    case VCS_ZCODE_DEV_ERR_CAPABILITY: return "capability-invalid";
    case VCS_ZCODE_DEV_ERR_LIMIT: return "limit-invalid";
    case VCS_ZCODE_DEV_ERR_EXPIRY: return "task-expired";
    case VCS_ZCODE_DEV_ERR_POLICY: return "proof-policy-invalid";
    case VCS_ZCODE_DEV_ERR_VERDICT: return "review-verdict-invalid";
    case VCS_ZCODE_DEV_ERR_WORK_KIND: return "work-kind-invalid";
    case VCS_ZCODE_DEV_ERR_WORK_STATUS: return "work-status-invalid";
    case VCS_ZCODE_DEV_ERR_TIME_ORDER: return "time-order-invalid";
    case VCS_ZCODE_DEV_ERR_TASK_MISMATCH: return "task-root-mismatch";
    case VCS_ZCODE_DEV_ERR_SOURCE_STALE: return "source-root-stale";
    case VCS_ZCODE_DEV_ERR_POLICY_MISMATCH: return "proof-policy-mismatch";
    case VCS_ZCODE_DEV_ERR_TOOLCHAIN_STALE: return "toolchain-capsule-stale";
    case VCS_ZCODE_DEV_ERR_OUTPUT_MISMATCH: return "output-root-mismatch";
    }
    return "unknown";
}

enum vcs_zcode_dev_error vcs_zcode_task_validate(
    const struct vcs_zcode_task_v1 *task)
{
    if (!task) return VCS_ZCODE_DEV_ERR_NULL;
    if (task->schema_version != VCS_ZCODE_DEV_VERSION)
        return VCS_ZCODE_DEV_ERR_VERSION;
    const uint8_t *roots[] = {
        task->source_root, task->dependency_lock_root,
        task->toolchain_capsule_root, task->write_scope_root,
        task->acceptance_tests_root, task->proof_policy_root,
        task->model_policy_root, task->goal_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!nonzero(roots[i])) return VCS_ZCODE_DEV_ERR_ROOT_ZERO;
    const uint32_t required = VCS_ZCODE_TASK_CAP_SOURCE_READ |
                              VCS_ZCODE_TASK_CAP_CANDIDATE_WRITE;
    if ((task->capabilities & required) != required ||
        (task->capabilities & ~VCS_ZCODE_TASK_CAP_V1_MASK) != 0)
        return VCS_ZCODE_DEV_ERR_CAPABILITY;
    if (task->max_changed_files == 0 || task->max_changed_files > 4096 ||
        task->max_patch_bytes == 0 ||
        task->max_patch_bytes > VCS_ZCODE_TASK_MAX_PATCH_BYTES ||
        task->max_context_bytes == 0 ||
        task->max_context_bytes > VCS_ZCODE_TASK_MAX_CONTEXT_BYTES ||
        task->max_cpu_seconds == 0 || task->max_cpu_seconds > 3600 ||
        task->max_memory_bytes < UINT64_C(1024) * 1024u ||
        task->max_memory_bytes > VCS_ZCODE_TASK_MAX_MEMORY_BYTES ||
        task->max_output_bytes == 0 ||
        task->max_output_bytes > VCS_ZCODE_TASK_MAX_OUTPUT_BYTES)
        return VCS_ZCODE_DEV_ERR_LIMIT;
    if (task->expires_unix <= 0) return VCS_ZCODE_DEV_ERR_EXPIRY;
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_task_validate_at(
    const struct vcs_zcode_task_v1 *task, int64_t now_unix)
{
    enum vcs_zcode_dev_error err = vcs_zcode_task_validate(task);
    if (err != VCS_ZCODE_DEV_OK) return err;
    if (now_unix <= 0 || now_unix >= task->expires_unix)
        return VCS_ZCODE_DEV_ERR_EXPIRY;
    return VCS_ZCODE_DEV_OK;
}

static bool required_count(uint32_t proofs, uint32_t bit, uint16_t count)
{
    return (proofs & bit) ? count > 0 : count == 0;
}

enum vcs_zcode_dev_error vcs_zcode_proof_policy_validate(
    const struct vcs_zcode_proof_policy_v1 *policy)
{
    if (!policy) return VCS_ZCODE_DEV_ERR_NULL;
    if (policy->schema_version != VCS_ZCODE_DEV_VERSION)
        return VCS_ZCODE_DEV_ERR_VERSION;
    if (policy->required_proofs == 0 ||
        (policy->required_proofs & ~VCS_ZCODE_PROOF_V1_MASK) != 0 ||
        (policy->flags & ~VCS_ZCODE_POLICY_V1_FLAG_MASK) != 0 ||
        !required_count(policy->required_proofs, VCS_ZCODE_PROOF_COMPILE,
                        policy->minimum_compile_receipts) ||
        !required_count(policy->required_proofs, VCS_ZCODE_PROOF_TEST,
                        policy->minimum_test_receipts) ||
        !required_count(policy->required_proofs, VCS_ZCODE_PROOF_FUZZ,
                        policy->minimum_fuzz_receipts) ||
        !required_count(policy->required_proofs, VCS_ZCODE_PROOF_REVIEW,
                        policy->minimum_reviews) ||
        policy->minimum_matching_receipts == 0 ||
        policy->minimum_matching_receipts > 64 ||
        policy->audit_basis_points > 10000 ||
        policy->maximum_proof_age_seconds == 0 ||
        ((policy->required_proofs & VCS_ZCODE_PROOF_FUZZ) != 0) !=
            (policy->deterministic_fuzz_seeds > 0))
        return VCS_ZCODE_DEV_ERR_POLICY;
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_candidate_validate(
    const struct vcs_zcode_candidate_v1 *candidate)
{
    if (!candidate) return VCS_ZCODE_DEV_ERR_NULL;
    if (candidate->schema_version != VCS_ZCODE_DEV_VERSION)
        return VCS_ZCODE_DEV_ERR_VERSION;
    const uint8_t *roots[] = {
        candidate->task_root, candidate->base_source_root,
        candidate->patch_root, candidate->candidate_source_root,
        candidate->adapter_policy_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!nonzero(roots[i])) return VCS_ZCODE_DEV_ERR_ROOT_ZERO;
    if (!nonzero(candidate->author_pubkey))
        return VCS_ZCODE_DEV_ERR_PUBKEY_ZERO;
    if (candidate->sequence == 0 || candidate->created_unix <= 0)
        return VCS_ZCODE_DEV_ERR_LIMIT;
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_review_validate(
    const struct vcs_zcode_review_v1 *review)
{
    if (!review) return VCS_ZCODE_DEV_ERR_NULL;
    if (review->schema_version != VCS_ZCODE_DEV_VERSION)
        return VCS_ZCODE_DEV_ERR_VERSION;
    const uint8_t *roots[] = {
        review->task_root, review->candidate_root,
        review->proof_policy_root, review->proof_set_root,
        review->findings_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!nonzero(roots[i])) return VCS_ZCODE_DEV_ERR_ROOT_ZERO;
    if (!nonzero(review->reviewer_pubkey))
        return VCS_ZCODE_DEV_ERR_PUBKEY_ZERO;
    if (review->verdict < VCS_ZCODE_REVIEW_APPROVE ||
        review->verdict > VCS_ZCODE_REVIEW_REJECT)
        return VCS_ZCODE_DEV_ERR_VERDICT;
    if (review->sequence == 0 || review->created_unix <= 0)
        return VCS_ZCODE_DEV_ERR_LIMIT;
    return VCS_ZCODE_DEV_OK;
}

static enum vcs_zcode_dev_error receipt_fields(
    const struct vcs_zcode_work_receipt_v1 *receipt, bool require_signature)
{
    if (!receipt) return VCS_ZCODE_DEV_ERR_NULL;
    if (receipt->schema_version != VCS_ZCODE_DEV_VERSION)
        return VCS_ZCODE_DEV_ERR_VERSION;
    const uint8_t *roots[] = {
        receipt->task_root, receipt->candidate_root, receipt->action_root,
        receipt->input_root, receipt->output_root,
        receipt->proof_policy_root, receipt->toolchain_capsule_root,
        receipt->lease_id, receipt->evidence_root, receipt->confinement_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!nonzero(roots[i])) return VCS_ZCODE_DEV_ERR_ROOT_ZERO;
    if (!nonzero(receipt->signer_pubkey))
        return VCS_ZCODE_DEV_ERR_PUBKEY_ZERO;
    if (receipt->work_kind < VCS_ZCODE_WORK_PROPOSE ||
        receipt->work_kind > VCS_ZCODE_WORK_DIAGNOSE)
        return VCS_ZCODE_DEV_ERR_WORK_KIND;
    if (receipt->status < VCS_ZCODE_WORK_PASS ||
        receipt->status > VCS_ZCODE_WORK_REFUSED ||
        (receipt->status == VCS_ZCODE_WORK_PASS && receipt->exit_status != 0))
        return VCS_ZCODE_DEV_ERR_WORK_STATUS;
    if (receipt->started_unix <= 0 ||
        receipt->finished_unix < receipt->started_unix)
        return VCS_ZCODE_DEV_ERR_TIME_ORDER;
    if (require_signature &&
        !bytes_nonzero(receipt->signature, sizeof(receipt->signature)))
        return VCS_ZCODE_DEV_ERR_SIGNATURE;
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_work_receipt_validate(
    const struct vcs_zcode_work_receipt_v1 *receipt)
{
    return receipt_fields(receipt, true);
}

static void put_bytes(uint8_t *wire, size_t *off, const void *src, size_t len)
{
    memcpy(wire + *off, src, len);
    *off += len;
}

static void put_u16(uint8_t *wire, size_t *off, uint16_t v)
{
    zcl_write_u16_le(wire + *off, v); *off += 2;
}

static void put_u32(uint8_t *wire, size_t *off, uint32_t v)
{
    zcl_write_u32_le(wire + *off, v); *off += 4;
}

static void put_u64(uint8_t *wire, size_t *off, uint64_t v)
{
    zcl_write_u64_le(wire + *off, v); *off += 8;
}

static void get_bytes(const uint8_t *wire, size_t *off, void *dst, size_t len)
{
    memcpy(dst, wire + *off, len);
    *off += len;
}

static uint16_t get_u16(const uint8_t *wire, size_t *off)
{
    uint16_t v = zcl_read_u16_le(wire + *off); *off += 2; return v;
}

static uint32_t get_u32(const uint8_t *wire, size_t *off)
{
    uint32_t v = zcl_read_u32_le(wire + *off); *off += 4; return v;
}

static uint64_t get_u64(const uint8_t *wire, size_t *off)
{
    uint64_t v = zcl_read_u64_le(wire + *off); *off += 8; return v;
}

static void object_root(const char *domain, size_t domain_len,
                        const uint8_t *wire, size_t wire_len, uint8_t out[32])
{
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, domain_len);
    sha3_256_write(&sha, wire, wire_len);
    sha3_256_finalize(&sha, out);
}

enum vcs_zcode_dev_error vcs_zcode_task_serialize(
    const struct vcs_zcode_task_v1 *task,
    uint8_t out[VCS_ZCODE_TASK_WIRE_BYTES])
{
    enum vcs_zcode_dev_error err = vcs_zcode_task_validate(task);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    size_t o = 0;
    put_bytes(out, &o, task_magic, sizeof(task_magic));
    put_u16(out, &o, task->schema_version);
    put_bytes(out, &o, task->source_root, 32);
    put_bytes(out, &o, task->dependency_lock_root, 32);
    put_bytes(out, &o, task->toolchain_capsule_root, 32);
    put_bytes(out, &o, task->write_scope_root, 32);
    put_bytes(out, &o, task->acceptance_tests_root, 32);
    put_bytes(out, &o, task->proof_policy_root, 32);
    put_bytes(out, &o, task->model_policy_root, 32);
    put_bytes(out, &o, task->goal_root, 32);
    put_u32(out, &o, task->capabilities);
    put_u32(out, &o, task->max_changed_files);
    put_u64(out, &o, task->max_patch_bytes);
    put_u64(out, &o, task->max_context_bytes);
    put_u32(out, &o, task->max_cpu_seconds);
    put_u64(out, &o, task->max_memory_bytes);
    put_u64(out, &o, task->max_output_bytes);
    put_u64(out, &o, (uint64_t)task->expires_unix);
    return o == VCS_ZCODE_TASK_WIRE_BYTES ? VCS_ZCODE_DEV_OK
                                          : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
}

enum vcs_zcode_dev_error vcs_zcode_task_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_task_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_DEV_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_TASK_WIRE_BYTES) return VCS_ZCODE_DEV_ERR_WIRE_SIZE;
    if (memcmp(wire, task_magic, sizeof(task_magic)) != 0)
        return VCS_ZCODE_DEV_ERR_WIRE_MAGIC;
    size_t o = sizeof(task_magic);
    out->schema_version = get_u16(wire, &o);
    get_bytes(wire, &o, out->source_root, 32);
    get_bytes(wire, &o, out->dependency_lock_root, 32);
    get_bytes(wire, &o, out->toolchain_capsule_root, 32);
    get_bytes(wire, &o, out->write_scope_root, 32);
    get_bytes(wire, &o, out->acceptance_tests_root, 32);
    get_bytes(wire, &o, out->proof_policy_root, 32);
    get_bytes(wire, &o, out->model_policy_root, 32);
    get_bytes(wire, &o, out->goal_root, 32);
    out->capabilities = get_u32(wire, &o);
    out->max_changed_files = get_u32(wire, &o);
    out->max_patch_bytes = get_u64(wire, &o);
    out->max_context_bytes = get_u64(wire, &o);
    out->max_cpu_seconds = get_u32(wire, &o);
    out->max_memory_bytes = get_u64(wire, &o);
    out->max_output_bytes = get_u64(wire, &o);
    out->expires_unix = (int64_t)get_u64(wire, &o);
    enum vcs_zcode_dev_error err = vcs_zcode_task_validate(out);
    if (err != VCS_ZCODE_DEV_OK) memset(out, 0, sizeof(*out));
    return err;
}

enum vcs_zcode_dev_error vcs_zcode_task_root(
    const struct vcs_zcode_task_v1 *task, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_TASK_WIRE_BYTES];
    enum vcs_zcode_dev_error err = vcs_zcode_task_serialize(task, wire);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    static const char domain[] = VCS_ZCODE_TASK_DOMAIN;
    object_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_proof_policy_serialize(
    const struct vcs_zcode_proof_policy_v1 *p,
    uint8_t out[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES])
{
    enum vcs_zcode_dev_error err = vcs_zcode_proof_policy_validate(p);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    size_t o = 0;
    put_bytes(out, &o, policy_magic, sizeof(policy_magic));
    put_u16(out, &o, p->schema_version);
    put_u32(out, &o, p->required_proofs);
    put_u16(out, &o, p->minimum_compile_receipts);
    put_u16(out, &o, p->minimum_test_receipts);
    put_u16(out, &o, p->minimum_fuzz_receipts);
    put_u16(out, &o, p->minimum_reviews);
    put_u16(out, &o, p->minimum_matching_receipts);
    put_u16(out, &o, p->flags);
    put_u32(out, &o, p->deterministic_fuzz_seeds);
    put_u16(out, &o, p->audit_basis_points);
    put_u32(out, &o, p->maximum_proof_age_seconds);
    return o == VCS_ZCODE_PROOF_POLICY_WIRE_BYTES ? VCS_ZCODE_DEV_OK
                                                  : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
}

enum vcs_zcode_dev_error vcs_zcode_proof_policy_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_proof_policy_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_DEV_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_PROOF_POLICY_WIRE_BYTES)
        return VCS_ZCODE_DEV_ERR_WIRE_SIZE;
    if (memcmp(wire, policy_magic, sizeof(policy_magic)) != 0)
        return VCS_ZCODE_DEV_ERR_WIRE_MAGIC;
    size_t o = sizeof(policy_magic);
    out->schema_version = get_u16(wire, &o);
    out->required_proofs = get_u32(wire, &o);
    out->minimum_compile_receipts = get_u16(wire, &o);
    out->minimum_test_receipts = get_u16(wire, &o);
    out->minimum_fuzz_receipts = get_u16(wire, &o);
    out->minimum_reviews = get_u16(wire, &o);
    out->minimum_matching_receipts = get_u16(wire, &o);
    out->flags = get_u16(wire, &o);
    out->deterministic_fuzz_seeds = get_u32(wire, &o);
    out->audit_basis_points = get_u16(wire, &o);
    out->maximum_proof_age_seconds = get_u32(wire, &o);
    enum vcs_zcode_dev_error err = vcs_zcode_proof_policy_validate(out);
    if (err != VCS_ZCODE_DEV_OK) memset(out, 0, sizeof(*out));
    return err;
}

enum vcs_zcode_dev_error vcs_zcode_proof_policy_root(
    const struct vcs_zcode_proof_policy_v1 *p, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
    enum vcs_zcode_dev_error err = vcs_zcode_proof_policy_serialize(p, wire);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    static const char domain[] = VCS_ZCODE_PROOF_POLICY_DOMAIN;
    object_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_candidate_serialize(
    const struct vcs_zcode_candidate_v1 *c,
    uint8_t out[VCS_ZCODE_CANDIDATE_WIRE_BYTES])
{
    enum vcs_zcode_dev_error err = vcs_zcode_candidate_validate(c);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    size_t o = 0;
    put_bytes(out, &o, candidate_magic, sizeof(candidate_magic));
    put_u16(out, &o, c->schema_version);
    put_bytes(out, &o, c->task_root, 32);
    put_bytes(out, &o, c->base_source_root, 32);
    put_bytes(out, &o, c->patch_root, 32);
    put_bytes(out, &o, c->candidate_source_root, 32);
    put_bytes(out, &o, c->adapter_policy_root, 32);
    put_bytes(out, &o, c->author_pubkey, 32);
    put_u64(out, &o, c->sequence);
    put_u64(out, &o, (uint64_t)c->created_unix);
    return o == VCS_ZCODE_CANDIDATE_WIRE_BYTES ? VCS_ZCODE_DEV_OK
                                               : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
}

enum vcs_zcode_dev_error vcs_zcode_candidate_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_candidate_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_DEV_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_CANDIDATE_WIRE_BYTES)
        return VCS_ZCODE_DEV_ERR_WIRE_SIZE;
    if (memcmp(wire, candidate_magic, sizeof(candidate_magic)) != 0)
        return VCS_ZCODE_DEV_ERR_WIRE_MAGIC;
    size_t o = sizeof(candidate_magic);
    out->schema_version = get_u16(wire, &o);
    get_bytes(wire, &o, out->task_root, 32);
    get_bytes(wire, &o, out->base_source_root, 32);
    get_bytes(wire, &o, out->patch_root, 32);
    get_bytes(wire, &o, out->candidate_source_root, 32);
    get_bytes(wire, &o, out->adapter_policy_root, 32);
    get_bytes(wire, &o, out->author_pubkey, 32);
    out->sequence = get_u64(wire, &o);
    out->created_unix = (int64_t)get_u64(wire, &o);
    enum vcs_zcode_dev_error err = vcs_zcode_candidate_validate(out);
    if (err != VCS_ZCODE_DEV_OK) memset(out, 0, sizeof(*out));
    return err;
}

enum vcs_zcode_dev_error vcs_zcode_candidate_root(
    const struct vcs_zcode_candidate_v1 *c, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_CANDIDATE_WIRE_BYTES];
    enum vcs_zcode_dev_error err = vcs_zcode_candidate_serialize(c, wire);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    static const char domain[] = VCS_ZCODE_CANDIDATE_DOMAIN;
    object_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_review_serialize(
    const struct vcs_zcode_review_v1 *r,
    uint8_t out[VCS_ZCODE_REVIEW_WIRE_BYTES])
{
    enum vcs_zcode_dev_error err = vcs_zcode_review_validate(r);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    size_t o = 0;
    put_bytes(out, &o, review_magic, sizeof(review_magic));
    put_u16(out, &o, r->schema_version);
    put_bytes(out, &o, r->task_root, 32);
    put_bytes(out, &o, r->candidate_root, 32);
    put_bytes(out, &o, r->proof_policy_root, 32);
    put_bytes(out, &o, r->proof_set_root, 32);
    put_bytes(out, &o, r->findings_root, 32);
    put_bytes(out, &o, r->reviewer_pubkey, 32);
    out[o++] = r->verdict;
    put_u64(out, &o, r->sequence);
    put_u64(out, &o, (uint64_t)r->created_unix);
    return o == VCS_ZCODE_REVIEW_WIRE_BYTES ? VCS_ZCODE_DEV_OK
                                            : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
}

enum vcs_zcode_dev_error vcs_zcode_review_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_review_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_DEV_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_REVIEW_WIRE_BYTES)
        return VCS_ZCODE_DEV_ERR_WIRE_SIZE;
    if (memcmp(wire, review_magic, sizeof(review_magic)) != 0)
        return VCS_ZCODE_DEV_ERR_WIRE_MAGIC;
    size_t o = sizeof(review_magic);
    out->schema_version = get_u16(wire, &o);
    get_bytes(wire, &o, out->task_root, 32);
    get_bytes(wire, &o, out->candidate_root, 32);
    get_bytes(wire, &o, out->proof_policy_root, 32);
    get_bytes(wire, &o, out->proof_set_root, 32);
    get_bytes(wire, &o, out->findings_root, 32);
    get_bytes(wire, &o, out->reviewer_pubkey, 32);
    out->verdict = wire[o++];
    out->sequence = get_u64(wire, &o);
    out->created_unix = (int64_t)get_u64(wire, &o);
    enum vcs_zcode_dev_error err = vcs_zcode_review_validate(out);
    if (err != VCS_ZCODE_DEV_OK) memset(out, 0, sizeof(*out));
    return err;
}

enum vcs_zcode_dev_error vcs_zcode_review_root(
    const struct vcs_zcode_review_v1 *r, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_REVIEW_WIRE_BYTES];
    enum vcs_zcode_dev_error err = vcs_zcode_review_serialize(r, wire);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    static const char domain[] = VCS_ZCODE_REVIEW_DOMAIN;
    object_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_DEV_OK;
}

static enum vcs_zcode_dev_error receipt_body(
    const struct vcs_zcode_work_receipt_v1 *r,
    uint8_t out[VCS_ZCODE_WORK_RECEIPT_BODY_BYTES])
{
    enum vcs_zcode_dev_error err = receipt_fields(r, false);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    size_t o = 0;
    put_bytes(out, &o, receipt_magic, sizeof(receipt_magic));
    put_u16(out, &o, r->schema_version);
    put_bytes(out, &o, r->task_root, 32);
    put_bytes(out, &o, r->candidate_root, 32);
    put_bytes(out, &o, r->action_root, 32);
    put_bytes(out, &o, r->input_root, 32);
    put_bytes(out, &o, r->output_root, 32);
    put_bytes(out, &o, r->proof_policy_root, 32);
    put_bytes(out, &o, r->toolchain_capsule_root, 32);
    put_bytes(out, &o, r->lease_id, 32);
    put_bytes(out, &o, r->evidence_root, 32);
    put_bytes(out, &o, r->confinement_root, 32);
    out[o++] = r->work_kind;
    out[o++] = r->status;
    put_u32(out, &o, (uint32_t)r->exit_status);
    put_u64(out, &o, (uint64_t)r->started_unix);
    put_u64(out, &o, (uint64_t)r->finished_unix);
    put_bytes(out, &o, r->signer_pubkey, 32);
    return o == VCS_ZCODE_WORK_RECEIPT_BODY_BYTES ? VCS_ZCODE_DEV_OK
                                                  : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
}

enum vcs_zcode_dev_error vcs_zcode_work_receipt_serialize(
    const struct vcs_zcode_work_receipt_v1 *r,
    uint8_t out[VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES])
{
    enum vcs_zcode_dev_error err = vcs_zcode_work_receipt_validate(r);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    err = receipt_body(r, out);
    if (err != VCS_ZCODE_DEV_OK) return err;
    memcpy(out + VCS_ZCODE_WORK_RECEIPT_BODY_BYTES, r->signature,
           sizeof(r->signature));
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_work_receipt_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_work_receipt_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_DEV_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES)
        return VCS_ZCODE_DEV_ERR_WIRE_SIZE;
    if (memcmp(wire, receipt_magic, sizeof(receipt_magic)) != 0)
        return VCS_ZCODE_DEV_ERR_WIRE_MAGIC;
    size_t o = sizeof(receipt_magic);
    out->schema_version = get_u16(wire, &o);
    get_bytes(wire, &o, out->task_root, 32);
    get_bytes(wire, &o, out->candidate_root, 32);
    get_bytes(wire, &o, out->action_root, 32);
    get_bytes(wire, &o, out->input_root, 32);
    get_bytes(wire, &o, out->output_root, 32);
    get_bytes(wire, &o, out->proof_policy_root, 32);
    get_bytes(wire, &o, out->toolchain_capsule_root, 32);
    get_bytes(wire, &o, out->lease_id, 32);
    get_bytes(wire, &o, out->evidence_root, 32);
    get_bytes(wire, &o, out->confinement_root, 32);
    out->work_kind = wire[o++];
    out->status = wire[o++];
    out->exit_status = (int32_t)get_u32(wire, &o);
    out->started_unix = (int64_t)get_u64(wire, &o);
    out->finished_unix = (int64_t)get_u64(wire, &o);
    get_bytes(wire, &o, out->signer_pubkey, 32);
    get_bytes(wire, &o, out->signature, 64);
    enum vcs_zcode_dev_error err = vcs_zcode_work_receipt_validate(out);
    if (err != VCS_ZCODE_DEV_OK) memset(out, 0, sizeof(*out));
    return err;
}

enum vcs_zcode_dev_error vcs_zcode_work_receipt_id(
    const struct vcs_zcode_work_receipt_v1 *r, uint8_t out[32])
{
    uint8_t body[VCS_ZCODE_WORK_RECEIPT_BODY_BYTES];
    enum vcs_zcode_dev_error err = receipt_body(r, body);
    if (err != VCS_ZCODE_DEV_OK || !out)
        return out ? err : VCS_ZCODE_DEV_ERR_NULL;
    static const char domain[] = VCS_ZCODE_WORK_RECEIPT_DOMAIN;
    object_root(domain, sizeof(domain), body, sizeof(body), out);
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_work_receipt_seal(
    struct vcs_zcode_work_receipt_v1 *r, const uint8_t secret[32],
    const uint8_t pubkey[32])
{
    if (!r || !secret || !pubkey) return VCS_ZCODE_DEV_ERR_NULL;
    if (!nonzero(pubkey)) return VCS_ZCODE_DEV_ERR_PUBKEY_ZERO;
    memcpy(r->signer_pubkey, pubkey, 32);
    uint8_t id[32];
    enum vcs_zcode_dev_error err = vcs_zcode_work_receipt_id(r, id);
    if (err != VCS_ZCODE_DEV_OK) return err;
    ed25519_sign(r->signature, id, sizeof(id), secret, pubkey);
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_work_receipt_verify(
    const struct vcs_zcode_work_receipt_v1 *r,
    const uint8_t expected_signer[32])
{
    enum vcs_zcode_dev_error err = vcs_zcode_work_receipt_validate(r);
    if (err != VCS_ZCODE_DEV_OK) return err;
    if (!expected_signer || memcmp(r->signer_pubkey, expected_signer, 32) != 0)
        return VCS_ZCODE_DEV_ERR_SIGNATURE;
    uint8_t id[32];
    err = vcs_zcode_work_receipt_id(r, id);
    if (err != VCS_ZCODE_DEV_OK) return err;
    return ed25519_verify(r->signature, id, sizeof(id), r->signer_pubkey)
               ? VCS_ZCODE_DEV_OK : VCS_ZCODE_DEV_ERR_SIGNATURE;
}

enum vcs_zcode_dev_error vcs_zcode_candidate_validate_for_task(
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate, int64_t now)
{
    enum vcs_zcode_dev_error err = vcs_zcode_task_validate_at(task, now);
    if (err != VCS_ZCODE_DEV_OK) return err;
    err = vcs_zcode_candidate_validate(candidate);
    if (err != VCS_ZCODE_DEV_OK) return err;
    uint8_t task_root[32];
    if (vcs_zcode_task_root(task, task_root) != VCS_ZCODE_DEV_OK ||
        memcmp(task_root, candidate->task_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_TASK_MISMATCH;
    if (memcmp(task->source_root, candidate->base_source_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_SOURCE_STALE;
    if (candidate->created_unix >= task->expires_unix ||
        candidate->created_unix > now)
        return VCS_ZCODE_DEV_ERR_EXPIRY;
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_review_validate_for_candidate(
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct vcs_zcode_review_v1 *review, int64_t now)
{
    enum vcs_zcode_dev_error err =
        vcs_zcode_candidate_validate_for_task(task, candidate, now);
    if (err != VCS_ZCODE_DEV_OK) return err;
    err = vcs_zcode_review_validate(review);
    if (err != VCS_ZCODE_DEV_OK) return err;
    uint8_t candidate_root[32];
    if (memcmp(review->task_root, candidate->task_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_TASK_MISMATCH;
    if (vcs_zcode_candidate_root(candidate, candidate_root) !=
            VCS_ZCODE_DEV_OK ||
        memcmp(review->candidate_root, candidate_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_OUTPUT_MISMATCH;
    if (memcmp(review->proof_policy_root, task->proof_policy_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_POLICY_MISMATCH;
    if (review->created_unix >= task->expires_unix ||
        review->created_unix > now)
        return VCS_ZCODE_DEV_ERR_EXPIRY;
    return VCS_ZCODE_DEV_OK;
}

enum vcs_zcode_dev_error vcs_zcode_work_receipt_validate_for_candidate(
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct vcs_zcode_work_receipt_v1 *receipt, int64_t now)
{
    enum vcs_zcode_dev_error err =
        vcs_zcode_candidate_validate_for_task(task, candidate, now);
    if (err != VCS_ZCODE_DEV_OK) return err;
    err = vcs_zcode_work_receipt_validate(receipt);
    if (err != VCS_ZCODE_DEV_OK) return err;
    uint8_t task_root[32], candidate_root[32];
    if (vcs_zcode_task_root(task, task_root) != VCS_ZCODE_DEV_OK ||
        memcmp(receipt->task_root, task_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_TASK_MISMATCH;
    if (vcs_zcode_candidate_root(candidate, candidate_root) !=
            VCS_ZCODE_DEV_OK ||
        memcmp(receipt->candidate_root, candidate_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_OUTPUT_MISMATCH;
    if (memcmp(receipt->proof_policy_root, task->proof_policy_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_POLICY_MISMATCH;
    if (memcmp(receipt->toolchain_capsule_root,
               task->toolchain_capsule_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_TOOLCHAIN_STALE;
    if (receipt->finished_unix >= task->expires_unix ||
        receipt->finished_unix > now)
        return VCS_ZCODE_DEV_ERR_EXPIRY;
    const uint8_t *expected_input =
        receipt->work_kind == VCS_ZCODE_WORK_PROPOSE
            ? task->source_root
            : candidate->candidate_source_root;
    if (receipt->work_kind == VCS_ZCODE_WORK_REVIEW)
        expected_input = candidate_root;
    if (memcmp(receipt->input_root, expected_input, 32) != 0)
        return VCS_ZCODE_DEV_ERR_SOURCE_STALE;
    if (receipt->work_kind == VCS_ZCODE_WORK_PROPOSE &&
        memcmp(receipt->output_root, candidate_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_OUTPUT_MISMATCH;
    return VCS_ZCODE_DEV_OK;
}
