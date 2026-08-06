/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical SHA3-addressed ZCODE development object wires. */

#include "vcs/zcode_dev.h"

#include "codec/cursor.h"
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
        policy->deterministic_fuzz_seeds > VCS_ZCODE_FUZZ_SEEDS_MAX ||
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
    struct zcl_codec_writer w;
    zcl_codec_writer_init(&w, out, VCS_ZCODE_TASK_WIRE_BYTES);
    bool ok = zcl_codec_write_bytes(&w, task_magic, sizeof(task_magic)) &&
        zcl_codec_write_u16le(&w, task->schema_version) &&
        zcl_codec_write_bytes(&w, task->source_root, 32) &&
        zcl_codec_write_bytes(&w, task->dependency_lock_root, 32) &&
        zcl_codec_write_bytes(&w, task->toolchain_capsule_root, 32) &&
        zcl_codec_write_bytes(&w, task->write_scope_root, 32) &&
        zcl_codec_write_bytes(&w, task->acceptance_tests_root, 32) &&
        zcl_codec_write_bytes(&w, task->proof_policy_root, 32) &&
        zcl_codec_write_bytes(&w, task->model_policy_root, 32) &&
        zcl_codec_write_bytes(&w, task->goal_root, 32) &&
        zcl_codec_write_u32le(&w, task->capabilities) &&
        zcl_codec_write_u32le(&w, task->max_changed_files) &&
        zcl_codec_write_u64le(&w, task->max_patch_bytes) &&
        zcl_codec_write_u64le(&w, task->max_context_bytes) &&
        zcl_codec_write_u32le(&w, task->max_cpu_seconds) &&
        zcl_codec_write_u64le(&w, task->max_memory_bytes) &&
        zcl_codec_write_u64le(&w, task->max_output_bytes) &&
        zcl_codec_write_i64le(&w, task->expires_unix);
    size_t written = 0;
    return ok && zcl_codec_writer_finish(&w, &written) &&
           written == VCS_ZCODE_TASK_WIRE_BYTES
        ? VCS_ZCODE_DEV_OK : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
}

enum vcs_zcode_dev_error vcs_zcode_task_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_task_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_DEV_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_TASK_WIRE_BYTES) return VCS_ZCODE_DEV_ERR_WIRE_SIZE;
    if (memcmp(wire, task_magic, sizeof(task_magic)) != 0)
        return VCS_ZCODE_DEV_ERR_WIRE_MAGIC;
    struct zcl_codec_reader r;
    zcl_codec_reader_init(&r, wire + sizeof(task_magic),
                          len - sizeof(task_magic));
    bool ok = zcl_codec_read_u16le(&r, &out->schema_version) &&
        zcl_codec_read_bytes(&r, out->source_root, 32) &&
        zcl_codec_read_bytes(&r, out->dependency_lock_root, 32) &&
        zcl_codec_read_bytes(&r, out->toolchain_capsule_root, 32) &&
        zcl_codec_read_bytes(&r, out->write_scope_root, 32) &&
        zcl_codec_read_bytes(&r, out->acceptance_tests_root, 32) &&
        zcl_codec_read_bytes(&r, out->proof_policy_root, 32) &&
        zcl_codec_read_bytes(&r, out->model_policy_root, 32) &&
        zcl_codec_read_bytes(&r, out->goal_root, 32) &&
        zcl_codec_read_u32le(&r, &out->capabilities) &&
        zcl_codec_read_u32le(&r, &out->max_changed_files) &&
        zcl_codec_read_u64le(&r, &out->max_patch_bytes) &&
        zcl_codec_read_u64le(&r, &out->max_context_bytes) &&
        zcl_codec_read_u32le(&r, &out->max_cpu_seconds) &&
        zcl_codec_read_u64le(&r, &out->max_memory_bytes) &&
        zcl_codec_read_u64le(&r, &out->max_output_bytes) &&
        zcl_codec_read_i64le(&r, &out->expires_unix) &&
        zcl_codec_reader_finish(&r);
    if (!ok) { memset(out, 0, sizeof(*out)); return VCS_ZCODE_DEV_ERR_WIRE_SIZE; }
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
    struct zcl_codec_writer w;
    zcl_codec_writer_init(&w, out, VCS_ZCODE_PROOF_POLICY_WIRE_BYTES);
    bool ok = zcl_codec_write_bytes(&w, policy_magic, sizeof(policy_magic)) &&
        zcl_codec_write_u16le(&w, p->schema_version) &&
        zcl_codec_write_u32le(&w, p->required_proofs) &&
        zcl_codec_write_u16le(&w, p->minimum_compile_receipts) &&
        zcl_codec_write_u16le(&w, p->minimum_test_receipts) &&
        zcl_codec_write_u16le(&w, p->minimum_fuzz_receipts) &&
        zcl_codec_write_u16le(&w, p->minimum_reviews) &&
        zcl_codec_write_u16le(&w, p->minimum_matching_receipts) &&
        zcl_codec_write_u16le(&w, p->flags) &&
        zcl_codec_write_u32le(&w, p->deterministic_fuzz_seeds) &&
        zcl_codec_write_u16le(&w, p->audit_basis_points) &&
        zcl_codec_write_u32le(&w, p->maximum_proof_age_seconds);
    size_t written = 0;
    return ok && zcl_codec_writer_finish(&w, &written) &&
           written == VCS_ZCODE_PROOF_POLICY_WIRE_BYTES
        ? VCS_ZCODE_DEV_OK : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
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
    struct zcl_codec_reader r;
    zcl_codec_reader_init(&r, wire + sizeof(policy_magic),
                          len - sizeof(policy_magic));
    bool ok = zcl_codec_read_u16le(&r, &out->schema_version) &&
        zcl_codec_read_u32le(&r, &out->required_proofs) &&
        zcl_codec_read_u16le(&r, &out->minimum_compile_receipts) &&
        zcl_codec_read_u16le(&r, &out->minimum_test_receipts) &&
        zcl_codec_read_u16le(&r, &out->minimum_fuzz_receipts) &&
        zcl_codec_read_u16le(&r, &out->minimum_reviews) &&
        zcl_codec_read_u16le(&r, &out->minimum_matching_receipts) &&
        zcl_codec_read_u16le(&r, &out->flags) &&
        zcl_codec_read_u32le(&r, &out->deterministic_fuzz_seeds) &&
        zcl_codec_read_u16le(&r, &out->audit_basis_points) &&
        zcl_codec_read_u32le(&r, &out->maximum_proof_age_seconds) &&
        zcl_codec_reader_finish(&r);
    if (!ok) { memset(out, 0, sizeof(*out)); return VCS_ZCODE_DEV_ERR_WIRE_SIZE; }
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
    struct zcl_codec_writer w;
    zcl_codec_writer_init(&w, out, VCS_ZCODE_CANDIDATE_WIRE_BYTES);
    bool ok = zcl_codec_write_bytes(&w, candidate_magic,
                                    sizeof(candidate_magic)) &&
        zcl_codec_write_u16le(&w, c->schema_version) &&
        zcl_codec_write_bytes(&w, c->task_root, 32) &&
        zcl_codec_write_bytes(&w, c->base_source_root, 32) &&
        zcl_codec_write_bytes(&w, c->patch_root, 32) &&
        zcl_codec_write_bytes(&w, c->candidate_source_root, 32) &&
        zcl_codec_write_bytes(&w, c->adapter_policy_root, 32) &&
        zcl_codec_write_bytes(&w, c->author_pubkey, 32) &&
        zcl_codec_write_u64le(&w, c->sequence) &&
        zcl_codec_write_i64le(&w, c->created_unix);
    size_t written = 0;
    return ok && zcl_codec_writer_finish(&w, &written) &&
           written == VCS_ZCODE_CANDIDATE_WIRE_BYTES
        ? VCS_ZCODE_DEV_OK : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
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
    struct zcl_codec_reader r;
    zcl_codec_reader_init(&r, wire + sizeof(candidate_magic),
                          len - sizeof(candidate_magic));
    bool ok = zcl_codec_read_u16le(&r, &out->schema_version) &&
        zcl_codec_read_bytes(&r, out->task_root, 32) &&
        zcl_codec_read_bytes(&r, out->base_source_root, 32) &&
        zcl_codec_read_bytes(&r, out->patch_root, 32) &&
        zcl_codec_read_bytes(&r, out->candidate_source_root, 32) &&
        zcl_codec_read_bytes(&r, out->adapter_policy_root, 32) &&
        zcl_codec_read_bytes(&r, out->author_pubkey, 32) &&
        zcl_codec_read_u64le(&r, &out->sequence) &&
        zcl_codec_read_i64le(&r, &out->created_unix) &&
        zcl_codec_reader_finish(&r);
    if (!ok) { memset(out, 0, sizeof(*out)); return VCS_ZCODE_DEV_ERR_WIRE_SIZE; }
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
    struct zcl_codec_writer w;
    zcl_codec_writer_init(&w, out, VCS_ZCODE_REVIEW_WIRE_BYTES);
    bool ok = zcl_codec_write_bytes(&w, review_magic, sizeof(review_magic)) &&
        zcl_codec_write_u16le(&w, r->schema_version) &&
        zcl_codec_write_bytes(&w, r->task_root, 32) &&
        zcl_codec_write_bytes(&w, r->candidate_root, 32) &&
        zcl_codec_write_bytes(&w, r->proof_policy_root, 32) &&
        zcl_codec_write_bytes(&w, r->proof_set_root, 32) &&
        zcl_codec_write_bytes(&w, r->findings_root, 32) &&
        zcl_codec_write_bytes(&w, r->reviewer_pubkey, 32) &&
        zcl_codec_write_u8(&w, r->verdict) &&
        zcl_codec_write_u64le(&w, r->sequence) &&
        zcl_codec_write_i64le(&w, r->created_unix);
    size_t written = 0;
    return ok && zcl_codec_writer_finish(&w, &written) &&
           written == VCS_ZCODE_REVIEW_WIRE_BYTES
        ? VCS_ZCODE_DEV_OK : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
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
    struct zcl_codec_reader reader;
    zcl_codec_reader_init(&reader, wire + sizeof(review_magic),
                          len - sizeof(review_magic));
    bool ok = zcl_codec_read_u16le(&reader, &out->schema_version) &&
        zcl_codec_read_bytes(&reader, out->task_root, 32) &&
        zcl_codec_read_bytes(&reader, out->candidate_root, 32) &&
        zcl_codec_read_bytes(&reader, out->proof_policy_root, 32) &&
        zcl_codec_read_bytes(&reader, out->proof_set_root, 32) &&
        zcl_codec_read_bytes(&reader, out->findings_root, 32) &&
        zcl_codec_read_bytes(&reader, out->reviewer_pubkey, 32) &&
        zcl_codec_read_u8(&reader, &out->verdict) &&
        zcl_codec_read_u64le(&reader, &out->sequence) &&
        zcl_codec_read_i64le(&reader, &out->created_unix) &&
        zcl_codec_reader_finish(&reader);
    if (!ok) { memset(out, 0, sizeof(*out)); return VCS_ZCODE_DEV_ERR_WIRE_SIZE; }
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
    struct zcl_codec_writer w;
    zcl_codec_writer_init(&w, out, VCS_ZCODE_WORK_RECEIPT_BODY_BYTES);
    bool ok = zcl_codec_write_bytes(&w, receipt_magic,
                                    sizeof(receipt_magic)) &&
        zcl_codec_write_u16le(&w, r->schema_version) &&
        zcl_codec_write_bytes(&w, r->task_root, 32) &&
        zcl_codec_write_bytes(&w, r->candidate_root, 32) &&
        zcl_codec_write_bytes(&w, r->action_root, 32) &&
        zcl_codec_write_bytes(&w, r->input_root, 32) &&
        zcl_codec_write_bytes(&w, r->output_root, 32) &&
        zcl_codec_write_bytes(&w, r->proof_policy_root, 32) &&
        zcl_codec_write_bytes(&w, r->toolchain_capsule_root, 32) &&
        zcl_codec_write_bytes(&w, r->lease_id, 32) &&
        zcl_codec_write_bytes(&w, r->evidence_root, 32) &&
        zcl_codec_write_bytes(&w, r->confinement_root, 32) &&
        zcl_codec_write_u8(&w, r->work_kind) &&
        zcl_codec_write_u8(&w, r->status) &&
        zcl_codec_write_i32le(&w, r->exit_status) &&
        zcl_codec_write_i64le(&w, r->started_unix) &&
        zcl_codec_write_i64le(&w, r->finished_unix) &&
        zcl_codec_write_bytes(&w, r->signer_pubkey, 32);
    size_t written = 0;
    return ok && zcl_codec_writer_finish(&w, &written) &&
           written == VCS_ZCODE_WORK_RECEIPT_BODY_BYTES
        ? VCS_ZCODE_DEV_OK : VCS_ZCODE_DEV_ERR_WIRE_SIZE;
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
    struct zcl_codec_reader reader;
    zcl_codec_reader_init(&reader, wire + sizeof(receipt_magic),
                          len - sizeof(receipt_magic));
    bool ok = zcl_codec_read_u16le(&reader, &out->schema_version) &&
        zcl_codec_read_bytes(&reader, out->task_root, 32) &&
        zcl_codec_read_bytes(&reader, out->candidate_root, 32) &&
        zcl_codec_read_bytes(&reader, out->action_root, 32) &&
        zcl_codec_read_bytes(&reader, out->input_root, 32) &&
        zcl_codec_read_bytes(&reader, out->output_root, 32) &&
        zcl_codec_read_bytes(&reader, out->proof_policy_root, 32) &&
        zcl_codec_read_bytes(&reader, out->toolchain_capsule_root, 32) &&
        zcl_codec_read_bytes(&reader, out->lease_id, 32) &&
        zcl_codec_read_bytes(&reader, out->evidence_root, 32) &&
        zcl_codec_read_bytes(&reader, out->confinement_root, 32) &&
        zcl_codec_read_u8(&reader, &out->work_kind) &&
        zcl_codec_read_u8(&reader, &out->status) &&
        zcl_codec_read_i32le(&reader, &out->exit_status) &&
        zcl_codec_read_i64le(&reader, &out->started_unix) &&
        zcl_codec_read_i64le(&reader, &out->finished_unix) &&
        zcl_codec_read_bytes(&reader, out->signer_pubkey, 32) &&
        zcl_codec_read_bytes(&reader, out->signature, 64) &&
        zcl_codec_reader_finish(&reader);
    if (!ok) { memset(out, 0, sizeof(*out)); return VCS_ZCODE_DEV_ERR_WIRE_SIZE; }
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
    /* Build/test/fuzz/reproduce inputs are fixed-action artifacts (for V1,
     * the preprocessed TU), so their exact root is action-bound and cannot be
     * inferred from the candidate tree root here. Proposals and reviews have
     * canonical object inputs that can be checked without the action. */
    if (receipt->work_kind == VCS_ZCODE_WORK_PROPOSE &&
        memcmp(receipt->input_root, task->source_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_SOURCE_STALE;
    if (receipt->work_kind == VCS_ZCODE_WORK_REVIEW &&
        memcmp(receipt->input_root, candidate_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_SOURCE_STALE;
    if (receipt->work_kind == VCS_ZCODE_WORK_PROPOSE &&
        memcmp(receipt->output_root, candidate_root, 32) != 0)
        return VCS_ZCODE_DEV_ERR_OUTPUT_MISMATCH;
    return VCS_ZCODE_DEV_OK;
}
