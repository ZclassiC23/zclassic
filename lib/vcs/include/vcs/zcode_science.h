/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical ZCODE scientific-study and evidence object wires. */

#ifndef ZCL_VCS_ZCODE_SCIENCE_H
#define ZCL_VCS_ZCODE_SCIENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vcs/build_action.h"
#include "vcs/zcode_dev.h"

#define VCS_ZCODE_SCIENCE_VERSION 1u
#define VCS_ZCODE_STUDY_SPEC_DOMAIN "zcl.zcode.study_spec.v1"
#define VCS_ZCODE_BENCHMARK_RESULT_DOMAIN "zcl.zcode.benchmark_result.v1"
#define VCS_ZCODE_REPRODUCTION_DOMAIN "zcl.zcode.reproduction.v1"
#define VCS_ZCODE_SCIENCE_FINDINGS_DOMAIN "zcl.zcode.science_findings.v1"
#define VCS_ZCODE_CURATION_VOTE_DOMAIN "zcl.zcode.curation_vote.v1"

#define VCS_ZCODE_STUDY_SPEC_WIRE_BYTES 422u
#define VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES 299u
#define VCS_ZCODE_REPRODUCTION_WIRE_BYTES 251u
#define VCS_ZCODE_SCIENCE_FINDINGS_WIRE_BYTES 317u
#define VCS_ZCODE_CURATION_VOTE_BODY_BYTES 155u
#define VCS_ZCODE_CURATION_VOTE_WIRE_BYTES 219u

#define VCS_ZCODE_STUDY_REQUIRED_MAX 64u

enum vcs_zcode_benchmark_status {
    VCS_ZCODE_BENCHMARK_OBSERVED = 1,
    VCS_ZCODE_BENCHMARK_NULL_RESULT = 2,
    VCS_ZCODE_BENCHMARK_NEGATIVE_RESULT = 3,
    VCS_ZCODE_BENCHMARK_EXECUTION_FAILED = 4,
};

enum vcs_zcode_reproduction_verdict {
    VCS_ZCODE_REPRODUCTION_REPLICATED = 1,
    VCS_ZCODE_REPRODUCTION_CONTRADICTED = 2,
    VCS_ZCODE_REPRODUCTION_INCONCLUSIVE = 3,
};

enum vcs_zcode_science_finding_flag {
    VCS_ZCODE_FINDING_NEGATIVE = 1u << 0,
    VCS_ZCODE_FINDING_NULL = 1u << 1,
    VCS_ZCODE_FINDING_ENVIRONMENT_INCOMPATIBLE = 1u << 2,
    VCS_ZCODE_FINDING_STALE = 1u << 3,
    VCS_ZCODE_FINDING_RETRACTION = 1u << 4,
};

#define VCS_ZCODE_FINDING_V1_FLAG_MASK \
    (VCS_ZCODE_FINDING_NEGATIVE | VCS_ZCODE_FINDING_NULL | \
     VCS_ZCODE_FINDING_ENVIRONMENT_INCOMPATIBLE | \
     VCS_ZCODE_FINDING_STALE | VCS_ZCODE_FINDING_RETRACTION)

enum vcs_zcode_science_finding_severity {
    VCS_ZCODE_FINDING_INFORMATIONAL = 1,
    VCS_ZCODE_FINDING_MATERIAL = 2,
    VCS_ZCODE_FINDING_CRITICAL = 3,
};

enum vcs_zcode_curation_signal {
    VCS_ZCODE_CURATION_USEFUL = 1,
    VCS_ZCODE_CURATION_INTERESTING = 2,
    VCS_ZCODE_CURATION_FLAG = 3,
};

enum vcs_zcode_science_error {
    VCS_ZCODE_SCIENCE_OK = 0,
    VCS_ZCODE_SCIENCE_ERR_NULL,
    VCS_ZCODE_SCIENCE_ERR_VERSION,
    VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE,
    VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC,
    VCS_ZCODE_SCIENCE_ERR_ROOT_ZERO,
    VCS_ZCODE_SCIENCE_ERR_PUBKEY_ZERO,
    VCS_ZCODE_SCIENCE_ERR_SIGNATURE,
    VCS_ZCODE_SCIENCE_ERR_LIMIT,
    VCS_ZCODE_SCIENCE_ERR_TIME_ORDER,
    VCS_ZCODE_SCIENCE_ERR_STATUS,
    VCS_ZCODE_SCIENCE_ERR_VERDICT,
    VCS_ZCODE_SCIENCE_ERR_FLAGS,
    VCS_ZCODE_SCIENCE_ERR_ROOT_REUSED,
    VCS_ZCODE_SCIENCE_ERR_STUDY_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_TASK_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_CANDIDATE_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_RESULT_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_REVIEW_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_ENVIRONMENT_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_NETWORK_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_IDENTITY_MISMATCH,
    VCS_ZCODE_SCIENCE_ERR_EXPIRED,
    /* New codes append at the END only; the codes above are frozen. */
    VCS_ZCODE_SCIENCE_ERR_EVIDENCE_FUTURE,
    VCS_ZCODE_SCIENCE_ERR_ACTION_MISMATCH,
};

const char *vcs_zcode_science_error_string(
    enum vcs_zcode_science_error error);

struct vcs_zcode_study_spec_v1 {
    uint16_t schema_version;
    uint8_t hypothesis_root[32];
    uint8_t null_hypothesis_root[32];
    uint8_t source_root[32];
    uint8_t dependency_lock_root[32];
    uint8_t toolchain_capsule_root[32];
    uint8_t protocol_root[32];
    uint8_t workloads_root[32];
    uint8_t metrics_root[32];
    uint8_t estimator_tolerance_root[32];
    uint8_t environment_policy_root[32];
    uint8_t citations_root[32];
    uint8_t preregistration_policy_root[32];
    uint16_t required_reproductions;
    uint16_t required_reviews;
    uint64_t sequence;
    int64_t created_unix;
    int64_t expires_unix;
};

struct vcs_zcode_benchmark_result_v1 {
    uint16_t schema_version;
    uint8_t study_root[32];
    uint8_t task_root[32];
    uint8_t candidate_root[32];
    uint8_t action_root[32];
    uint8_t achieved_environment_root[32];
    uint8_t raw_sample_root[32];
    uint8_t evidence_root[32];
    uint8_t status;
    uint64_t challenge_block_height;
    uint8_t challenge_block_hash[32];
    uint64_t sequence;
    int64_t started_unix;
    int64_t finished_unix;
};

struct vcs_zcode_reproduction_v1 {
    uint16_t schema_version;
    uint8_t study_root[32];
    uint8_t original_result_root[32];
    uint8_t reproduced_result_root[32];
    uint8_t comparison_policy_root[32];
    uint8_t original_environment_root[32];
    uint8_t reproduced_environment_root[32];
    uint8_t reproducer_pubkey[32];
    uint8_t verdict;
    uint64_t sequence;
    int64_t created_unix;
};

struct vcs_zcode_science_findings_v1 {
    uint16_t schema_version;
    uint8_t study_root[32];
    uint8_t task_root[32];
    uint8_t candidate_root[32];
    uint8_t result_root[32];
    uint8_t proof_set_root[32];
    uint8_t methods_root[32];
    uint8_t limitations_root[32];
    uint8_t conflicts_root[32];
    uint8_t retraction_target_root[32];
    uint16_t flags;
    uint8_t severity;
    uint64_t sequence;
    int64_t created_unix;
};

struct vcs_zcode_curation_vote_v1 {
    uint16_t schema_version;
    uint8_t network_genesis_root[32];
    uint8_t voter_zid_root[32];
    uint8_t property_root[32];
    uint8_t signal;
    uint64_t sequence;
    int64_t expires_unix;
    uint8_t signer_pubkey[32];
    uint8_t signature[64];
};

enum vcs_zcode_science_error vcs_zcode_study_spec_validate(
    const struct vcs_zcode_study_spec_v1 *study);
enum vcs_zcode_science_error vcs_zcode_study_spec_validate_at(
    const struct vcs_zcode_study_spec_v1 *study, int64_t now_unix);
/* Submit-vs-verify split: expiry gates NEW submissions only. A study accepts
 * a submission at now_unix iff the spec is structurally valid and now_unix is
 * inside [created_unix, expires_unix). Historical evidence created inside the
 * window must keep re-verifying after the window closes, so the cross-object
 * validators below never call this gate (or validate_at) on the study — they
 * check the evidence object's own timestamps against the window instead. */
bool vcs_zcode_study_spec_accepts_submission_at(
    const struct vcs_zcode_study_spec_v1 *study, int64_t now_unix);
enum vcs_zcode_science_error vcs_zcode_study_spec_serialize(
    const struct vcs_zcode_study_spec_v1 *study,
    uint8_t out[VCS_ZCODE_STUDY_SPEC_WIRE_BYTES]);
enum vcs_zcode_science_error vcs_zcode_study_spec_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_study_spec_v1 *out);
enum vcs_zcode_science_error vcs_zcode_study_spec_root(
    const struct vcs_zcode_study_spec_v1 *study, uint8_t out[32]);

enum vcs_zcode_science_error vcs_zcode_benchmark_result_validate(
    const struct vcs_zcode_benchmark_result_v1 *result);
enum vcs_zcode_science_error vcs_zcode_benchmark_result_serialize(
    const struct vcs_zcode_benchmark_result_v1 *result,
    uint8_t out[VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES]);
enum vcs_zcode_science_error vcs_zcode_benchmark_result_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_benchmark_result_v1 *out);
enum vcs_zcode_science_error vcs_zcode_benchmark_result_root(
    const struct vcs_zcode_benchmark_result_v1 *result, uint8_t out[32]);

enum vcs_zcode_science_error vcs_zcode_reproduction_validate(
    const struct vcs_zcode_reproduction_v1 *reproduction);
enum vcs_zcode_science_error vcs_zcode_reproduction_serialize(
    const struct vcs_zcode_reproduction_v1 *reproduction,
    uint8_t out[VCS_ZCODE_REPRODUCTION_WIRE_BYTES]);
enum vcs_zcode_science_error vcs_zcode_reproduction_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_reproduction_v1 *out);
enum vcs_zcode_science_error vcs_zcode_reproduction_root(
    const struct vcs_zcode_reproduction_v1 *reproduction, uint8_t out[32]);

enum vcs_zcode_science_error vcs_zcode_science_findings_validate(
    const struct vcs_zcode_science_findings_v1 *findings);
enum vcs_zcode_science_error vcs_zcode_science_findings_serialize(
    const struct vcs_zcode_science_findings_v1 *findings,
    uint8_t out[VCS_ZCODE_SCIENCE_FINDINGS_WIRE_BYTES]);
enum vcs_zcode_science_error vcs_zcode_science_findings_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_science_findings_v1 *out);
enum vcs_zcode_science_error vcs_zcode_science_findings_root(
    const struct vcs_zcode_science_findings_v1 *findings, uint8_t out[32]);

enum vcs_zcode_science_error vcs_zcode_curation_vote_validate(
    const struct vcs_zcode_curation_vote_v1 *vote);
/* Curation votes are LIVE signals, not historical evidence: unlike the
 * evidence objects above, a vote's expiry keeps gating it at verify time and
 * an expired vote is simply no longer counted. */
enum vcs_zcode_science_error vcs_zcode_curation_vote_validate_at(
    const struct vcs_zcode_curation_vote_v1 *vote, int64_t now_unix);
enum vcs_zcode_science_error vcs_zcode_curation_vote_serialize(
    const struct vcs_zcode_curation_vote_v1 *vote,
    uint8_t out[VCS_ZCODE_CURATION_VOTE_WIRE_BYTES]);
enum vcs_zcode_science_error vcs_zcode_curation_vote_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_curation_vote_v1 *out);
enum vcs_zcode_science_error vcs_zcode_curation_vote_id(
    const struct vcs_zcode_curation_vote_v1 *vote, uint8_t out[32]);
enum vcs_zcode_science_error vcs_zcode_curation_vote_seal(
    struct vcs_zcode_curation_vote_v1 *vote, const uint8_t secret[32],
    const uint8_t pubkey[32]);
enum vcs_zcode_science_error vcs_zcode_curation_vote_verify(
    const struct vcs_zcode_curation_vote_v1 *vote,
    const uint8_t expected_network_genesis[32],
    const uint8_t expected_voter_zid[32],
    const uint8_t expected_signer[32], int64_t now_unix);

/* Cross-object evidence validators. These VERIFY evidence whenever it is
 * read, including long after the study window closed; they reject evidence
 * whose own timestamps fall outside [study.created_unix, study.expires_unix),
 * and report evidence timestamped after now_unix as
 * VCS_ZCODE_SCIENCE_ERR_EVIDENCE_FUTURE so callers can distinguish a clock
 * problem from a window violation. The benchmark result must also bind the
 * canonical root of a registered fixed action (build_action.h): `action` is
 * the executed action instance and result->action_root must equal its
 * canonical root under one of the fixed kinds. */
enum vcs_zcode_science_error vcs_zcode_benchmark_result_validate_for_study(
    const struct vcs_zcode_study_spec_v1 *study,
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct vcs_build_action_v1 *action,
    const struct vcs_zcode_benchmark_result_v1 *result, int64_t now_unix);
enum vcs_zcode_science_error vcs_zcode_reproduction_validate_for_results(
    const struct vcs_zcode_study_spec_v1 *study,
    const struct vcs_zcode_benchmark_result_v1 *original,
    const struct vcs_zcode_benchmark_result_v1 *reproduced,
    const struct vcs_zcode_reproduction_v1 *reproduction, int64_t now_unix);
enum vcs_zcode_science_error vcs_zcode_science_findings_validate_for_review(
    const struct vcs_zcode_study_spec_v1 *study,
    const struct vcs_zcode_review_v1 *review,
    const struct vcs_zcode_benchmark_result_v1 *result,
    const struct vcs_zcode_science_findings_v1 *findings, int64_t now_unix);

#endif /* ZCL_VCS_ZCODE_SCIENCE_H */
