/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical SHA3-addressed ZCODE scientific evidence wires. */

#include "vcs/zcode_science.h"

#include "base/serialize_le.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"

#include <string.h>

static const uint8_t study_magic[8] = {'Z','C','S','T','U','D','\r','\n'};
static const uint8_t result_magic[8] = {'Z','C','B','E','N','C','\r','\n'};
static const uint8_t reproduction_magic[8] =
    {'Z','C','R','E','P','R','\r','\n'};
static const uint8_t findings_magic[8] = {'Z','C','F','I','N','D','\r','\n'};
static const uint8_t vote_magic[8] = {'Z','C','V','O','T','E','\r','\n'};

static bool bytes_nonzero(const uint8_t *bytes, size_t len)
{
    uint8_t any = 0;
    if (!bytes) return false;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

static bool root_nonzero(const uint8_t root[32])
{
    return bytes_nonzero(root, 32);
}

static void put_bytes(uint8_t *wire, size_t *off, const void *src, size_t len)
{
    memcpy(wire + *off, src, len);
    *off += len;
}

static void put_u16(uint8_t *wire, size_t *off, uint16_t value)
{
    zcl_write_u16_le(wire + *off, value);
    *off += 2;
}

static void put_u64(uint8_t *wire, size_t *off, uint64_t value)
{
    zcl_write_u64_le(wire + *off, value);
    *off += 8;
}

static void get_bytes(const uint8_t *wire, size_t *off, void *out, size_t len)
{
    memcpy(out, wire + *off, len);
    *off += len;
}

static uint16_t get_u16(const uint8_t *wire, size_t *off)
{
    uint16_t value = zcl_read_u16_le(wire + *off);
    *off += 2;
    return value;
}

static uint64_t get_u64(const uint8_t *wire, size_t *off)
{
    uint64_t value = zcl_read_u64_le(wire + *off);
    *off += 8;
    return value;
}

static void science_root(const char *domain, size_t domain_len,
                         const uint8_t *wire, size_t wire_len,
                         uint8_t out[32])
{
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, domain_len);
    sha3_256_write(&sha, wire, wire_len);
    sha3_256_finalize(&sha, out);
}

const char *vcs_zcode_science_error_string(enum vcs_zcode_science_error error)
{
    switch (error) {
    case VCS_ZCODE_SCIENCE_OK: return "ok";
    case VCS_ZCODE_SCIENCE_ERR_NULL: return "null-argument";
    case VCS_ZCODE_SCIENCE_ERR_VERSION: return "schema-version";
    case VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE: return "wire-size";
    case VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC: return "wire-magic";
    case VCS_ZCODE_SCIENCE_ERR_ROOT_ZERO: return "root-zero";
    case VCS_ZCODE_SCIENCE_ERR_PUBKEY_ZERO: return "pubkey-zero";
    case VCS_ZCODE_SCIENCE_ERR_SIGNATURE: return "signature-invalid";
    case VCS_ZCODE_SCIENCE_ERR_LIMIT: return "limit-invalid";
    case VCS_ZCODE_SCIENCE_ERR_TIME_ORDER: return "time-order-invalid";
    case VCS_ZCODE_SCIENCE_ERR_STATUS: return "benchmark-status-invalid";
    case VCS_ZCODE_SCIENCE_ERR_VERDICT: return "reproduction-verdict-invalid";
    case VCS_ZCODE_SCIENCE_ERR_FLAGS: return "finding-flags-invalid";
    case VCS_ZCODE_SCIENCE_ERR_ROOT_REUSED: return "distinct-root-required";
    case VCS_ZCODE_SCIENCE_ERR_STUDY_MISMATCH: return "study-root-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_TASK_MISMATCH: return "task-root-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_CANDIDATE_MISMATCH:
        return "candidate-root-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_RESULT_MISMATCH: return "result-root-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_REVIEW_MISMATCH: return "review-root-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_ENVIRONMENT_MISMATCH:
        return "environment-root-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_NETWORK_MISMATCH:
        return "network-genesis-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_IDENTITY_MISMATCH:
        return "identity-root-mismatch";
    case VCS_ZCODE_SCIENCE_ERR_EXPIRED: return "object-expired";
    case VCS_ZCODE_SCIENCE_ERR_EVIDENCE_FUTURE: return "evidence-from-future";
    case VCS_ZCODE_SCIENCE_ERR_ACTION_MISMATCH: return "action-root-mismatch";
    }
    return "unknown";
}

enum vcs_zcode_science_error vcs_zcode_study_spec_validate(
    const struct vcs_zcode_study_spec_v1 *study)
{
    if (!study) return VCS_ZCODE_SCIENCE_ERR_NULL;
    if (study->schema_version != VCS_ZCODE_SCIENCE_VERSION)
        return VCS_ZCODE_SCIENCE_ERR_VERSION;
    const uint8_t *roots[] = {
        study->hypothesis_root, study->null_hypothesis_root,
        study->source_root, study->dependency_lock_root,
        study->toolchain_capsule_root, study->protocol_root,
        study->workloads_root, study->metrics_root,
        study->estimator_tolerance_root, study->environment_policy_root,
        study->citations_root, study->preregistration_policy_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!root_nonzero(roots[i])) return VCS_ZCODE_SCIENCE_ERR_ROOT_ZERO;
    if (memcmp(study->hypothesis_root, study->null_hypothesis_root, 32) == 0)
        return VCS_ZCODE_SCIENCE_ERR_ROOT_REUSED;
    if (study->required_reproductions == 0 ||
        study->required_reproductions > VCS_ZCODE_STUDY_REQUIRED_MAX ||
        study->required_reviews == 0 ||
        study->required_reviews > VCS_ZCODE_STUDY_REQUIRED_MAX ||
        study->sequence == 0)
        return VCS_ZCODE_SCIENCE_ERR_LIMIT;
    if (study->created_unix <= 0 ||
        study->expires_unix <= study->created_unix)
        return VCS_ZCODE_SCIENCE_ERR_TIME_ORDER;
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_study_spec_validate_at(
    const struct vcs_zcode_study_spec_v1 *study, int64_t now_unix)
{
    enum vcs_zcode_science_error error = vcs_zcode_study_spec_validate(study);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    if (now_unix <= 0 || now_unix < study->created_unix ||
        now_unix >= study->expires_unix)
        return VCS_ZCODE_SCIENCE_ERR_EXPIRED;
    return VCS_ZCODE_SCIENCE_OK;
}

bool vcs_zcode_study_spec_accepts_submission_at(
    const struct vcs_zcode_study_spec_v1 *study, int64_t now_unix)
{
    return vcs_zcode_study_spec_validate(study) == VCS_ZCODE_SCIENCE_OK &&
           now_unix >= study->created_unix && now_unix < study->expires_unix;
}

enum vcs_zcode_science_error vcs_zcode_study_spec_serialize(
    const struct vcs_zcode_study_spec_v1 *study,
    uint8_t out[VCS_ZCODE_STUDY_SPEC_WIRE_BYTES])
{
    enum vcs_zcode_science_error error = vcs_zcode_study_spec_validate(study);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    size_t off = 0;
    put_bytes(out, &off, study_magic, sizeof(study_magic));
    put_u16(out, &off, study->schema_version);
    const uint8_t *roots[] = {
        study->hypothesis_root, study->null_hypothesis_root,
        study->source_root, study->dependency_lock_root,
        study->toolchain_capsule_root, study->protocol_root,
        study->workloads_root, study->metrics_root,
        study->estimator_tolerance_root, study->environment_policy_root,
        study->citations_root, study->preregistration_policy_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        put_bytes(out, &off, roots[i], 32);
    put_u16(out, &off, study->required_reproductions);
    put_u16(out, &off, study->required_reviews);
    put_u64(out, &off, study->sequence);
    put_u64(out, &off, (uint64_t)study->created_unix);
    put_u64(out, &off, (uint64_t)study->expires_unix);
    return off == VCS_ZCODE_STUDY_SPEC_WIRE_BYTES
               ? VCS_ZCODE_SCIENCE_OK : VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
}

enum vcs_zcode_science_error vcs_zcode_study_spec_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_study_spec_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_SCIENCE_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_STUDY_SPEC_WIRE_BYTES)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
    if (memcmp(wire, study_magic, sizeof(study_magic)) != 0)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC;
    size_t off = sizeof(study_magic);
    out->schema_version = get_u16(wire, &off);
    uint8_t *roots[] = {
        out->hypothesis_root, out->null_hypothesis_root, out->source_root,
        out->dependency_lock_root, out->toolchain_capsule_root,
        out->protocol_root, out->workloads_root, out->metrics_root,
        out->estimator_tolerance_root, out->environment_policy_root,
        out->citations_root, out->preregistration_policy_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        get_bytes(wire, &off, roots[i], 32);
    out->required_reproductions = get_u16(wire, &off);
    out->required_reviews = get_u16(wire, &off);
    out->sequence = get_u64(wire, &off);
    out->created_unix = (int64_t)get_u64(wire, &off);
    out->expires_unix = (int64_t)get_u64(wire, &off);
    enum vcs_zcode_science_error error = vcs_zcode_study_spec_validate(out);
    if (error != VCS_ZCODE_SCIENCE_OK) memset(out, 0, sizeof(*out));
    return error;
}

enum vcs_zcode_science_error vcs_zcode_study_spec_root(
    const struct vcs_zcode_study_spec_v1 *study, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_STUDY_SPEC_WIRE_BYTES];
    enum vcs_zcode_science_error error =
        vcs_zcode_study_spec_serialize(study, wire);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    static const char domain[] = VCS_ZCODE_STUDY_SPEC_DOMAIN;
    science_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_benchmark_result_validate(
    const struct vcs_zcode_benchmark_result_v1 *result)
{
    if (!result) return VCS_ZCODE_SCIENCE_ERR_NULL;
    if (result->schema_version != VCS_ZCODE_SCIENCE_VERSION)
        return VCS_ZCODE_SCIENCE_ERR_VERSION;
    const uint8_t *roots[] = {
        result->study_root, result->task_root, result->candidate_root,
        result->action_root, result->achieved_environment_root,
        result->raw_sample_root, result->evidence_root,
        result->challenge_block_hash,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!root_nonzero(roots[i])) return VCS_ZCODE_SCIENCE_ERR_ROOT_ZERO;
    if (result->status < VCS_ZCODE_BENCHMARK_OBSERVED ||
        result->status > VCS_ZCODE_BENCHMARK_EXECUTION_FAILED)
        return VCS_ZCODE_SCIENCE_ERR_STATUS;
    if (result->challenge_block_height == 0 || result->sequence == 0)
        return VCS_ZCODE_SCIENCE_ERR_LIMIT;
    if (result->started_unix <= 0 ||
        result->finished_unix < result->started_unix)
        return VCS_ZCODE_SCIENCE_ERR_TIME_ORDER;
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_benchmark_result_serialize(
    const struct vcs_zcode_benchmark_result_v1 *result,
    uint8_t out[VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES])
{
    enum vcs_zcode_science_error error =
        vcs_zcode_benchmark_result_validate(result);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    size_t off = 0;
    put_bytes(out, &off, result_magic, sizeof(result_magic));
    put_u16(out, &off, result->schema_version);
    const uint8_t *roots[] = {
        result->study_root, result->task_root, result->candidate_root,
        result->action_root, result->achieved_environment_root,
        result->raw_sample_root, result->evidence_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        put_bytes(out, &off, roots[i], 32);
    out[off++] = result->status;
    put_u64(out, &off, result->challenge_block_height);
    put_bytes(out, &off, result->challenge_block_hash, 32);
    put_u64(out, &off, result->sequence);
    put_u64(out, &off, (uint64_t)result->started_unix);
    put_u64(out, &off, (uint64_t)result->finished_unix);
    return off == VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES
               ? VCS_ZCODE_SCIENCE_OK : VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
}

enum vcs_zcode_science_error vcs_zcode_benchmark_result_parse(
    const uint8_t *wire, size_t len,
    struct vcs_zcode_benchmark_result_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_SCIENCE_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
    if (memcmp(wire, result_magic, sizeof(result_magic)) != 0)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC;
    size_t off = sizeof(result_magic);
    out->schema_version = get_u16(wire, &off);
    uint8_t *roots[] = {
        out->study_root, out->task_root, out->candidate_root,
        out->action_root, out->achieved_environment_root,
        out->raw_sample_root, out->evidence_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        get_bytes(wire, &off, roots[i], 32);
    out->status = wire[off++];
    out->challenge_block_height = get_u64(wire, &off);
    get_bytes(wire, &off, out->challenge_block_hash, 32);
    out->sequence = get_u64(wire, &off);
    out->started_unix = (int64_t)get_u64(wire, &off);
    out->finished_unix = (int64_t)get_u64(wire, &off);
    enum vcs_zcode_science_error error =
        vcs_zcode_benchmark_result_validate(out);
    if (error != VCS_ZCODE_SCIENCE_OK) memset(out, 0, sizeof(*out));
    return error;
}

enum vcs_zcode_science_error vcs_zcode_benchmark_result_root(
    const struct vcs_zcode_benchmark_result_v1 *result, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES];
    enum vcs_zcode_science_error error =
        vcs_zcode_benchmark_result_serialize(result, wire);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    static const char domain[] = VCS_ZCODE_BENCHMARK_RESULT_DOMAIN;
    science_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_reproduction_validate(
    const struct vcs_zcode_reproduction_v1 *reproduction)
{
    if (!reproduction) return VCS_ZCODE_SCIENCE_ERR_NULL;
    if (reproduction->schema_version != VCS_ZCODE_SCIENCE_VERSION)
        return VCS_ZCODE_SCIENCE_ERR_VERSION;
    const uint8_t *roots[] = {
        reproduction->study_root, reproduction->original_result_root,
        reproduction->reproduced_result_root,
        reproduction->comparison_policy_root,
        reproduction->original_environment_root,
        reproduction->reproduced_environment_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!root_nonzero(roots[i])) return VCS_ZCODE_SCIENCE_ERR_ROOT_ZERO;
    if (!root_nonzero(reproduction->reproducer_pubkey))
        return VCS_ZCODE_SCIENCE_ERR_PUBKEY_ZERO;
    if (memcmp(reproduction->original_result_root,
               reproduction->reproduced_result_root, 32) == 0)
        return VCS_ZCODE_SCIENCE_ERR_ROOT_REUSED;
    if (reproduction->verdict < VCS_ZCODE_REPRODUCTION_REPLICATED ||
        reproduction->verdict > VCS_ZCODE_REPRODUCTION_INCONCLUSIVE)
        return VCS_ZCODE_SCIENCE_ERR_VERDICT;
    if (reproduction->sequence == 0) return VCS_ZCODE_SCIENCE_ERR_LIMIT;
    if (reproduction->created_unix <= 0)
        return VCS_ZCODE_SCIENCE_ERR_TIME_ORDER;
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_reproduction_serialize(
    const struct vcs_zcode_reproduction_v1 *reproduction,
    uint8_t out[VCS_ZCODE_REPRODUCTION_WIRE_BYTES])
{
    enum vcs_zcode_science_error error =
        vcs_zcode_reproduction_validate(reproduction);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    size_t off = 0;
    put_bytes(out, &off, reproduction_magic, sizeof(reproduction_magic));
    put_u16(out, &off, reproduction->schema_version);
    const uint8_t *roots[] = {
        reproduction->study_root, reproduction->original_result_root,
        reproduction->reproduced_result_root,
        reproduction->comparison_policy_root,
        reproduction->original_environment_root,
        reproduction->reproduced_environment_root,
        reproduction->reproducer_pubkey,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        put_bytes(out, &off, roots[i], 32);
    out[off++] = reproduction->verdict;
    put_u64(out, &off, reproduction->sequence);
    put_u64(out, &off, (uint64_t)reproduction->created_unix);
    return off == VCS_ZCODE_REPRODUCTION_WIRE_BYTES
               ? VCS_ZCODE_SCIENCE_OK : VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
}

enum vcs_zcode_science_error vcs_zcode_reproduction_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_reproduction_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_SCIENCE_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_REPRODUCTION_WIRE_BYTES)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
    if (memcmp(wire, reproduction_magic, sizeof(reproduction_magic)) != 0)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC;
    size_t off = sizeof(reproduction_magic);
    out->schema_version = get_u16(wire, &off);
    uint8_t *roots[] = {
        out->study_root, out->original_result_root,
        out->reproduced_result_root, out->comparison_policy_root,
        out->original_environment_root, out->reproduced_environment_root,
        out->reproducer_pubkey,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        get_bytes(wire, &off, roots[i], 32);
    out->verdict = wire[off++];
    out->sequence = get_u64(wire, &off);
    out->created_unix = (int64_t)get_u64(wire, &off);
    enum vcs_zcode_science_error error = vcs_zcode_reproduction_validate(out);
    if (error != VCS_ZCODE_SCIENCE_OK) memset(out, 0, sizeof(*out));
    return error;
}

enum vcs_zcode_science_error vcs_zcode_reproduction_root(
    const struct vcs_zcode_reproduction_v1 *reproduction, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_REPRODUCTION_WIRE_BYTES];
    enum vcs_zcode_science_error error =
        vcs_zcode_reproduction_serialize(reproduction, wire);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    static const char domain[] = VCS_ZCODE_REPRODUCTION_DOMAIN;
    science_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_science_findings_validate(
    const struct vcs_zcode_science_findings_v1 *findings)
{
    if (!findings) return VCS_ZCODE_SCIENCE_ERR_NULL;
    if (findings->schema_version != VCS_ZCODE_SCIENCE_VERSION)
        return VCS_ZCODE_SCIENCE_ERR_VERSION;
    const uint8_t *roots[] = {
        findings->study_root, findings->task_root, findings->candidate_root,
        findings->result_root, findings->proof_set_root,
        findings->methods_root, findings->limitations_root,
        findings->conflicts_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!root_nonzero(roots[i])) return VCS_ZCODE_SCIENCE_ERR_ROOT_ZERO;
    if ((findings->flags & ~VCS_ZCODE_FINDING_V1_FLAG_MASK) != 0 ||
        (((findings->flags & VCS_ZCODE_FINDING_RETRACTION) != 0) !=
         root_nonzero(findings->retraction_target_root)))
        return VCS_ZCODE_SCIENCE_ERR_FLAGS;
    if (findings->severity < VCS_ZCODE_FINDING_INFORMATIONAL ||
        findings->severity > VCS_ZCODE_FINDING_CRITICAL)
        return VCS_ZCODE_SCIENCE_ERR_FLAGS;
    if (findings->sequence == 0) return VCS_ZCODE_SCIENCE_ERR_LIMIT;
    if (findings->created_unix <= 0)
        return VCS_ZCODE_SCIENCE_ERR_TIME_ORDER;
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_science_findings_serialize(
    const struct vcs_zcode_science_findings_v1 *findings,
    uint8_t out[VCS_ZCODE_SCIENCE_FINDINGS_WIRE_BYTES])
{
    enum vcs_zcode_science_error error =
        vcs_zcode_science_findings_validate(findings);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    size_t off = 0;
    put_bytes(out, &off, findings_magic, sizeof(findings_magic));
    put_u16(out, &off, findings->schema_version);
    const uint8_t *roots[] = {
        findings->study_root, findings->task_root, findings->candidate_root,
        findings->result_root, findings->proof_set_root,
        findings->methods_root, findings->limitations_root,
        findings->conflicts_root,
        findings->retraction_target_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        put_bytes(out, &off, roots[i], 32);
    put_u16(out, &off, findings->flags);
    out[off++] = findings->severity;
    put_u64(out, &off, findings->sequence);
    put_u64(out, &off, (uint64_t)findings->created_unix);
    return off == VCS_ZCODE_SCIENCE_FINDINGS_WIRE_BYTES
               ? VCS_ZCODE_SCIENCE_OK : VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
}

enum vcs_zcode_science_error vcs_zcode_science_findings_parse(
    const uint8_t *wire, size_t len,
    struct vcs_zcode_science_findings_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_SCIENCE_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_SCIENCE_FINDINGS_WIRE_BYTES)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
    if (memcmp(wire, findings_magic, sizeof(findings_magic)) != 0)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC;
    size_t off = sizeof(findings_magic);
    out->schema_version = get_u16(wire, &off);
    uint8_t *roots[] = {
        out->study_root, out->task_root, out->candidate_root,
        out->result_root, out->proof_set_root, out->methods_root,
        out->limitations_root, out->conflicts_root,
        out->retraction_target_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        get_bytes(wire, &off, roots[i], 32);
    out->flags = get_u16(wire, &off);
    out->severity = wire[off++];
    out->sequence = get_u64(wire, &off);
    out->created_unix = (int64_t)get_u64(wire, &off);
    enum vcs_zcode_science_error error =
        vcs_zcode_science_findings_validate(out);
    if (error != VCS_ZCODE_SCIENCE_OK) memset(out, 0, sizeof(*out));
    return error;
}

enum vcs_zcode_science_error vcs_zcode_science_findings_root(
    const struct vcs_zcode_science_findings_v1 *findings, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_SCIENCE_FINDINGS_WIRE_BYTES];
    enum vcs_zcode_science_error error =
        vcs_zcode_science_findings_serialize(findings, wire);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    static const char domain[] = VCS_ZCODE_SCIENCE_FINDINGS_DOMAIN;
    science_root(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_SCIENCE_OK;
}

static enum vcs_zcode_science_error curation_vote_fields(
    const struct vcs_zcode_curation_vote_v1 *vote, bool require_signature)
{
    if (!vote) return VCS_ZCODE_SCIENCE_ERR_NULL;
    if (vote->schema_version != VCS_ZCODE_SCIENCE_VERSION)
        return VCS_ZCODE_SCIENCE_ERR_VERSION;
    const uint8_t *roots[] = {
        vote->network_genesis_root, vote->voter_zid_root,
        vote->property_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!root_nonzero(roots[i])) return VCS_ZCODE_SCIENCE_ERR_ROOT_ZERO;
    if (!root_nonzero(vote->signer_pubkey))
        return VCS_ZCODE_SCIENCE_ERR_PUBKEY_ZERO;
    if (vote->signal < VCS_ZCODE_CURATION_USEFUL ||
        vote->signal > VCS_ZCODE_CURATION_FLAG)
        return VCS_ZCODE_SCIENCE_ERR_VERDICT;
    if (vote->sequence == 0) return VCS_ZCODE_SCIENCE_ERR_LIMIT;
    if (vote->expires_unix <= 0) return VCS_ZCODE_SCIENCE_ERR_TIME_ORDER;
    if (require_signature &&
        !bytes_nonzero(vote->signature, sizeof(vote->signature)))
        return VCS_ZCODE_SCIENCE_ERR_SIGNATURE;
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_curation_vote_validate(
    const struct vcs_zcode_curation_vote_v1 *vote)
{
    return curation_vote_fields(vote, true);
}

enum vcs_zcode_science_error vcs_zcode_curation_vote_validate_at(
    const struct vcs_zcode_curation_vote_v1 *vote, int64_t now_unix)
{
    enum vcs_zcode_science_error error = vcs_zcode_curation_vote_validate(vote);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    if (now_unix <= 0 || now_unix >= vote->expires_unix)
        return VCS_ZCODE_SCIENCE_ERR_EXPIRED;
    return VCS_ZCODE_SCIENCE_OK;
}

static enum vcs_zcode_science_error curation_vote_body(
    const struct vcs_zcode_curation_vote_v1 *vote,
    uint8_t out[VCS_ZCODE_CURATION_VOTE_BODY_BYTES])
{
    enum vcs_zcode_science_error error = curation_vote_fields(vote, false);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    size_t off = 0;
    put_bytes(out, &off, vote_magic, sizeof(vote_magic));
    put_u16(out, &off, vote->schema_version);
    put_bytes(out, &off, vote->network_genesis_root, 32);
    put_bytes(out, &off, vote->voter_zid_root, 32);
    put_bytes(out, &off, vote->property_root, 32);
    out[off++] = vote->signal;
    put_u64(out, &off, vote->sequence);
    put_u64(out, &off, (uint64_t)vote->expires_unix);
    put_bytes(out, &off, vote->signer_pubkey, 32);
    return off == VCS_ZCODE_CURATION_VOTE_BODY_BYTES
               ? VCS_ZCODE_SCIENCE_OK : VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
}

enum vcs_zcode_science_error vcs_zcode_curation_vote_serialize(
    const struct vcs_zcode_curation_vote_v1 *vote,
    uint8_t out[VCS_ZCODE_CURATION_VOTE_WIRE_BYTES])
{
    enum vcs_zcode_science_error error = vcs_zcode_curation_vote_validate(vote);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    error = curation_vote_body(vote, out);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    memcpy(out + VCS_ZCODE_CURATION_VOTE_BODY_BYTES, vote->signature, 64);
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_curation_vote_parse(
    const uint8_t *wire, size_t len, struct vcs_zcode_curation_vote_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_SCIENCE_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_CURATION_VOTE_WIRE_BYTES)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE;
    if (memcmp(wire, vote_magic, sizeof(vote_magic)) != 0)
        return VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC;
    size_t off = sizeof(vote_magic);
    out->schema_version = get_u16(wire, &off);
    get_bytes(wire, &off, out->network_genesis_root, 32);
    get_bytes(wire, &off, out->voter_zid_root, 32);
    get_bytes(wire, &off, out->property_root, 32);
    out->signal = wire[off++];
    out->sequence = get_u64(wire, &off);
    out->expires_unix = (int64_t)get_u64(wire, &off);
    get_bytes(wire, &off, out->signer_pubkey, 32);
    get_bytes(wire, &off, out->signature, 64);
    enum vcs_zcode_science_error error = vcs_zcode_curation_vote_validate(out);
    if (error != VCS_ZCODE_SCIENCE_OK) memset(out, 0, sizeof(*out));
    return error;
}

enum vcs_zcode_science_error vcs_zcode_curation_vote_id(
    const struct vcs_zcode_curation_vote_v1 *vote, uint8_t out[32])
{
    uint8_t body[VCS_ZCODE_CURATION_VOTE_BODY_BYTES];
    enum vcs_zcode_science_error error = curation_vote_body(vote, body);
    if (error != VCS_ZCODE_SCIENCE_OK || !out)
        return out ? error : VCS_ZCODE_SCIENCE_ERR_NULL;
    static const char domain[] = VCS_ZCODE_CURATION_VOTE_DOMAIN;
    science_root(domain, sizeof(domain), body, sizeof(body), out);
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_curation_vote_seal(
    struct vcs_zcode_curation_vote_v1 *vote, const uint8_t secret[32],
    const uint8_t pubkey[32])
{
    if (!vote || !secret || !pubkey) return VCS_ZCODE_SCIENCE_ERR_NULL;
    if (!root_nonzero(pubkey)) return VCS_ZCODE_SCIENCE_ERR_PUBKEY_ZERO;
    memcpy(vote->signer_pubkey, pubkey, 32);
    uint8_t id[32];
    enum vcs_zcode_science_error error = vcs_zcode_curation_vote_id(vote, id);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    ed25519_sign(vote->signature, id, sizeof(id), secret, pubkey);
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_curation_vote_verify(
    const struct vcs_zcode_curation_vote_v1 *vote,
    const uint8_t expected_network[32], const uint8_t expected_zid[32],
    const uint8_t expected_signer[32], int64_t now_unix)
{
    enum vcs_zcode_science_error error =
        vcs_zcode_curation_vote_validate_at(vote, now_unix);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    if (!expected_network ||
        memcmp(vote->network_genesis_root, expected_network, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_NETWORK_MISMATCH;
    if (!expected_zid || memcmp(vote->voter_zid_root, expected_zid, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_IDENTITY_MISMATCH;
    if (!expected_signer ||
        memcmp(vote->signer_pubkey, expected_signer, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_SIGNATURE;
    uint8_t id[32];
    error = vcs_zcode_curation_vote_id(vote, id);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    return ed25519_verify(vote->signature, id, sizeof(id),
                          vote->signer_pubkey)
               ? VCS_ZCODE_SCIENCE_OK : VCS_ZCODE_SCIENCE_ERR_SIGNATURE;
}

static enum vcs_zcode_science_error map_task_error(
    enum vcs_zcode_dev_error error)
{
    if (error == VCS_ZCODE_DEV_OK) return VCS_ZCODE_SCIENCE_OK;
    if (error == VCS_ZCODE_DEV_ERR_EXPIRY)
        return VCS_ZCODE_SCIENCE_ERR_EXPIRED;
    if (error == VCS_ZCODE_DEV_ERR_TASK_MISMATCH)
        return VCS_ZCODE_SCIENCE_ERR_TASK_MISMATCH;
    if (error == VCS_ZCODE_DEV_ERR_OUTPUT_MISMATCH)
        return VCS_ZCODE_SCIENCE_ERR_CANDIDATE_MISMATCH;
    return VCS_ZCODE_SCIENCE_ERR_CANDIDATE_MISMATCH;
}

/* The benchmark result carries only action_root (no action-kind field), so
 * the action instance is canonicalized under every registered fixed kind and
 * must match under exactly the kind its descriptor fields select. */
static bool action_root_is_canonical_fixed(
    const struct vcs_build_action_v1 *action, const uint8_t action_root[32])
{
    static const char *const fixed_kinds[] = {
        VCS_BUILD_ACTION_KIND_V1,
        VCS_BUILD_ACTION_KIND_TEST_V1,
        VCS_BUILD_ACTION_KIND_FUZZ_V1,
        VCS_BUILD_ACTION_KIND_BENCHMARK_V1,
        VCS_BUILD_ACTION_KIND_BENCHMARK_REPRODUCE_V1,
        VCS_BUILD_ACTION_KIND_REVIEW_V1,
    };
    if (!action || !action_root) return false;
    for (size_t i = 0; i < sizeof(fixed_kinds) / sizeof(fixed_kinds[0]); i++) {
        uint8_t canonical[32];
        if (vcs_build_action_v1_root_for_kind(fixed_kinds[i], action,
                                              canonical) &&
            memcmp(canonical, action_root, 32) == 0)
            return true;
    }
    return false;
}

enum vcs_zcode_science_error vcs_zcode_benchmark_result_validate_for_study(
    const struct vcs_zcode_study_spec_v1 *study,
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct vcs_build_action_v1 *action,
    const struct vcs_zcode_benchmark_result_v1 *result, int64_t now_unix)
{
    /* Verify, not submit: the study is structural-validated only; the window
     * checks below bind the EVIDENCE timestamps, never now_unix. See
     * vcs_zcode_study_spec_accepts_submission_at for the submission gate. */
    enum vcs_zcode_science_error error = vcs_zcode_study_spec_validate(study);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    error = map_task_error(vcs_zcode_task_validate(task));
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    error = map_task_error(vcs_zcode_candidate_validate(candidate));
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    error = vcs_zcode_benchmark_result_validate(result);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    if (!action) return VCS_ZCODE_SCIENCE_ERR_NULL;
    uint8_t study_root[32], task_root[32], candidate_root[32];
    if (vcs_zcode_study_spec_root(study, study_root) !=
            VCS_ZCODE_SCIENCE_OK ||
        memcmp(result->study_root, study_root, 32) != 0 ||
        memcmp(task->goal_root, study_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_STUDY_MISMATCH;
    if (vcs_zcode_task_root(task, task_root) != VCS_ZCODE_DEV_OK ||
        memcmp(result->task_root, task_root, 32) != 0 ||
        memcmp(candidate->task_root, task_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_TASK_MISMATCH;
    if (vcs_zcode_candidate_root(candidate, candidate_root) !=
            VCS_ZCODE_DEV_OK ||
        memcmp(result->candidate_root, candidate_root, 32) != 0 ||
        memcmp(candidate->base_source_root, task->source_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_CANDIDATE_MISMATCH;
    if (memcmp(study->source_root, task->source_root, 32) != 0 ||
        memcmp(study->dependency_lock_root, task->dependency_lock_root, 32) != 0 ||
        memcmp(study->toolchain_capsule_root,
               task->toolchain_capsule_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_TASK_MISMATCH;
    if (!action_root_is_canonical_fixed(action, result->action_root))
        return VCS_ZCODE_SCIENCE_ERR_ACTION_MISMATCH;
    if (candidate->created_unix >= task->expires_unix ||
        result->started_unix < study->created_unix ||
        result->finished_unix >= study->expires_unix)
        return VCS_ZCODE_SCIENCE_ERR_EXPIRED;
    if (candidate->created_unix > now_unix ||
        result->finished_unix > now_unix)
        return VCS_ZCODE_SCIENCE_ERR_EVIDENCE_FUTURE;
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_reproduction_validate_for_results(
    const struct vcs_zcode_study_spec_v1 *study,
    const struct vcs_zcode_benchmark_result_v1 *original,
    const struct vcs_zcode_benchmark_result_v1 *reproduced,
    const struct vcs_zcode_reproduction_v1 *reproduction, int64_t now_unix)
{
    /* Verify, not submit: the study is structural-validated only, so
     * historical reproductions keep re-verifying after the window closes. */
    enum vcs_zcode_science_error error = vcs_zcode_study_spec_validate(study);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    error = vcs_zcode_benchmark_result_validate(original);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    error = vcs_zcode_benchmark_result_validate(reproduced);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    error = vcs_zcode_reproduction_validate(reproduction);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    uint8_t study_root[32], original_root[32], reproduced_root[32];
    (void)vcs_zcode_study_spec_root(study, study_root);
    if (memcmp(original->study_root, study_root, 32) != 0 ||
        memcmp(reproduced->study_root, study_root, 32) != 0 ||
        memcmp(reproduction->study_root, study_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_STUDY_MISMATCH;
    /* A reproduction reruns the SAME task, candidate, and method (fixed
     * action) under the same study; only the environment, samples, and
     * verdict may differ. */
    if (memcmp(original->task_root, reproduced->task_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_TASK_MISMATCH;
    if (memcmp(original->candidate_root, reproduced->candidate_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_CANDIDATE_MISMATCH;
    if (memcmp(original->action_root, reproduced->action_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_ACTION_MISMATCH;
    (void)vcs_zcode_benchmark_result_root(original, original_root);
    (void)vcs_zcode_benchmark_result_root(reproduced, reproduced_root);
    if (memcmp(reproduction->original_result_root, original_root, 32) != 0 ||
        memcmp(reproduction->reproduced_result_root, reproduced_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_RESULT_MISMATCH;
    if (memcmp(reproduction->original_environment_root,
               original->achieved_environment_root, 32) != 0 ||
        memcmp(reproduction->reproduced_environment_root,
               reproduced->achieved_environment_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_ENVIRONMENT_MISMATCH;
    if (reproduction->created_unix < original->finished_unix ||
        reproduction->created_unix < reproduced->finished_unix ||
        reproduction->created_unix >= study->expires_unix)
        return VCS_ZCODE_SCIENCE_ERR_EXPIRED;
    if (reproduction->created_unix > now_unix)
        return VCS_ZCODE_SCIENCE_ERR_EVIDENCE_FUTURE;
    return VCS_ZCODE_SCIENCE_OK;
}

enum vcs_zcode_science_error vcs_zcode_science_findings_validate_for_review(
    const struct vcs_zcode_study_spec_v1 *study,
    const struct vcs_zcode_review_v1 *review,
    const struct vcs_zcode_benchmark_result_v1 *result,
    const struct vcs_zcode_science_findings_v1 *findings, int64_t now_unix)
{
    /* Verify, not submit: the study is structural-validated only, so
     * historical findings keep re-verifying after the window closes. */
    enum vcs_zcode_science_error error = vcs_zcode_study_spec_validate(study);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    if (vcs_zcode_review_validate(review) != VCS_ZCODE_DEV_OK)
        return VCS_ZCODE_SCIENCE_ERR_REVIEW_MISMATCH;
    error = vcs_zcode_benchmark_result_validate(result);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    error = vcs_zcode_science_findings_validate(findings);
    if (error != VCS_ZCODE_SCIENCE_OK) return error;
    uint8_t study_root[32], findings_root[32], result_root[32];
    (void)vcs_zcode_study_spec_root(study, study_root);
    if (memcmp(result->study_root, study_root, 32) != 0 ||
        memcmp(findings->study_root, study_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_STUDY_MISMATCH;
    (void)vcs_zcode_benchmark_result_root(result, result_root);
    if (memcmp(findings->result_root, result_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_RESULT_MISMATCH;
    /* The findings must discuss the exact result they pin: same task and
     * candidate roots as the evaluated result, not just the same study. */
    if (memcmp(findings->task_root, result->task_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_TASK_MISMATCH;
    if (memcmp(findings->candidate_root, result->candidate_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_CANDIDATE_MISMATCH;
    if (memcmp(findings->task_root, review->task_root, 32) != 0 ||
        memcmp(findings->candidate_root, review->candidate_root, 32) != 0 ||
        memcmp(findings->proof_set_root, review->proof_set_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_REVIEW_MISMATCH;
    (void)vcs_zcode_science_findings_root(findings, findings_root);
    if (memcmp(review->findings_root, findings_root, 32) != 0)
        return VCS_ZCODE_SCIENCE_ERR_REVIEW_MISMATCH;
    /* The findings are formed first and the review binds their root
     * afterward, so the review timestamp may be LATER than the findings',
     * never earlier. */
    if (findings->created_unix < result->finished_unix ||
        review->created_unix < findings->created_unix ||
        findings->created_unix >= study->expires_unix)
        return VCS_ZCODE_SCIENCE_ERR_EXPIRED;
    if (findings->created_unix > now_unix)
        return VCS_ZCODE_SCIENCE_ERR_EVIDENCE_FUTURE;
    return VCS_ZCODE_SCIENCE_OK;
}
