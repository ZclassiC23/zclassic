/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Prove canonical ZCODE scientific object and action identities. */

#include "test/test_core.h"

#include "base/hex.h"
#include "crypto/ed25519.h"
#include "vcs/build_action.h"
#include "vcs/zcode_science.h"

#include <stdio.h>
#include <string.h>

static void zs_root(uint8_t out[32], uint8_t value)
{
    memset(out, value, 32);
}

static void zs_study(struct vcs_zcode_study_spec_v1 *study)
{
    memset(study, 0, sizeof(*study));
    study->schema_version = VCS_ZCODE_SCIENCE_VERSION;
    zs_root(study->hypothesis_root, 1);
    zs_root(study->null_hypothesis_root, 2);
    zs_root(study->source_root, 3);
    zs_root(study->dependency_lock_root, 4);
    zs_root(study->toolchain_capsule_root, 5);
    zs_root(study->protocol_root, 6);
    zs_root(study->workloads_root, 7);
    zs_root(study->metrics_root, 8);
    zs_root(study->estimator_tolerance_root, 9);
    zs_root(study->environment_policy_root, 10);
    zs_root(study->citations_root, 11);
    zs_root(study->preregistration_policy_root, 12);
    study->required_reproductions = 2;
    study->required_reviews = 3;
    study->sequence = 17;
    study->created_unix = 1000;
    study->expires_unix = 5000;
}

static void zs_task_candidate(
    const struct vcs_zcode_study_spec_v1 *study,
    struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_candidate_v1 *candidate,
    uint8_t task_root[32], uint8_t candidate_root[32])
{
    uint8_t study_root[32];
    (void)vcs_zcode_study_spec_root(study, study_root);
    memset(task, 0, sizeof(*task));
    task->schema_version = VCS_ZCODE_DEV_VERSION;
    memcpy(task->source_root, study->source_root, 32);
    memcpy(task->dependency_lock_root, study->dependency_lock_root, 32);
    memcpy(task->toolchain_capsule_root, study->toolchain_capsule_root, 32);
    zs_root(task->write_scope_root, 20);
    zs_root(task->acceptance_tests_root, 21);
    zs_root(task->proof_policy_root, 22);
    zs_root(task->model_policy_root, 23);
    memcpy(task->goal_root, study_root, 32);
    task->capabilities = VCS_ZCODE_TASK_CAP_V1_MASK;
    task->max_changed_files = 32;
    task->max_patch_bytes = 1024 * 1024;
    task->max_context_bytes = 2 * 1024 * 1024;
    task->max_cpu_seconds = 120;
    task->max_memory_bytes = UINT64_C(512) * 1024 * 1024;
    task->max_output_bytes = UINT64_C(64) * 1024 * 1024;
    task->expires_unix = study->expires_unix;
    (void)vcs_zcode_task_root(task, task_root);

    memset(candidate, 0, sizeof(*candidate));
    candidate->schema_version = VCS_ZCODE_DEV_VERSION;
    memcpy(candidate->task_root, task_root, 32);
    memcpy(candidate->base_source_root, task->source_root, 32);
    zs_root(candidate->patch_root, 24);
    zs_root(candidate->candidate_source_root, 25);
    zs_root(candidate->adapter_policy_root, 26);
    zs_root(candidate->author_pubkey, 27);
    candidate->sequence = 1;
    candidate->created_unix = 1100;
    (void)vcs_zcode_candidate_root(candidate, candidate_root);
}

static void zs_result(
    const struct vcs_zcode_study_spec_v1 *study,
    const uint8_t task_root[32], const uint8_t candidate_root[32],
    struct vcs_zcode_benchmark_result_v1 *result)
{
    memset(result, 0, sizeof(*result));
    result->schema_version = VCS_ZCODE_SCIENCE_VERSION;
    (void)vcs_zcode_study_spec_root(study, result->study_root);
    memcpy(result->task_root, task_root, 32);
    memcpy(result->candidate_root, candidate_root, 32);
    zs_root(result->action_root, 30);
    zs_root(result->achieved_environment_root, 31);
    zs_root(result->raw_sample_root, 32);
    zs_root(result->evidence_root, 33);
    result->status = VCS_ZCODE_BENCHMARK_NULL_RESULT;
    result->challenge_block_height = 3200000;
    zs_root(result->challenge_block_hash, 34);
    result->sequence = 1;
    result->started_unix = 1200;
    result->finished_unix = 1300;
}

static int test_zs_study_codec(void)
{
    int failures = 0;
    TEST("zcode_science: preregistered study wire is exact and canonical") {
        struct vcs_zcode_study_spec_v1 study, parsed;
        zs_study(&study);
        uint8_t wire[VCS_ZCODE_STUDY_SPEC_WIRE_BYTES + 1], root[32];
        char root_hex[65];
        ASSERT_EQ(vcs_zcode_study_spec_validate_at(&study, 1500),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_study_spec_serialize(&study, wire),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT(memcmp(wire, "ZCSTUD\r\n", 8) == 0);
        ASSERT_EQ(vcs_zcode_study_spec_parse(
                      wire, VCS_ZCODE_STUDY_SPEC_WIRE_BYTES, &parsed),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT(memcmp(&study, &parsed, sizeof(study)) == 0);
        ASSERT_EQ(vcs_zcode_study_spec_root(&study, root),
                  VCS_ZCODE_SCIENCE_OK);
        zcl_hex_encode(root, 32, root_hex);
        ASSERT_STR_EQ(root_hex,
            "36c02aa95792e0fb1698a2da0e51badb6cb7b715f8774e419681a1bfb56d2098");

        ASSERT_EQ(vcs_zcode_study_spec_parse(
                      wire, VCS_ZCODE_STUDY_SPEC_WIRE_BYTES + 1, &parsed),
                  VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE);
        wire[0] ^= 1;
        ASSERT_EQ(vcs_zcode_study_spec_parse(
                      wire, VCS_ZCODE_STUDY_SPEC_WIRE_BYTES, &parsed),
                  VCS_ZCODE_SCIENCE_ERR_WIRE_MAGIC);
        ASSERT(parsed.schema_version == 0);
        study.null_hypothesis_root[0] = study.hypothesis_root[0];
        memset(study.null_hypothesis_root, 1, 32);
        ASSERT_EQ(vcs_zcode_study_spec_validate(&study),
                  VCS_ZCODE_SCIENCE_ERR_ROOT_REUSED);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zs_result_and_reproduction(void)
{
    int failures = 0;
    TEST("zcode_science: results are observations and reproductions are local verdicts") {
        struct vcs_zcode_study_spec_v1 study;
        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate;
        struct vcs_zcode_benchmark_result_v1 original, reproduced, parsed;
        struct vcs_zcode_reproduction_v1 reproduction, reproduction_parsed;
        uint8_t task_root[32], candidate_root[32];
        zs_study(&study);
        zs_task_candidate(&study, &task, &candidate,
                          task_root, candidate_root);
        zs_result(&study, task_root, candidate_root, &original);
        ASSERT_EQ(vcs_zcode_benchmark_result_validate_for_study(
                      &study, &task, &candidate, &original, 2000),
                  VCS_ZCODE_SCIENCE_OK);
        uint8_t result_wire[VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES + 1];
        uint8_t original_root[32], reproduced_root[32];
        char result_hex[65];
        ASSERT_EQ(vcs_zcode_benchmark_result_serialize(&original, result_wire),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_benchmark_result_parse(
                      result_wire, VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES,
                      &parsed), VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(parsed.status, VCS_ZCODE_BENCHMARK_NULL_RESULT);
        ASSERT_EQ(vcs_zcode_benchmark_result_root(&original, original_root),
                  VCS_ZCODE_SCIENCE_OK);
        zcl_hex_encode(original_root, 32, result_hex);
        ASSERT_STR_EQ(result_hex,
            "6edccd045383dadffa1ce094c54ab542c6dc7e703385b70e89f893610d6c0d97");
        ASSERT_EQ(vcs_zcode_benchmark_result_parse(
                      result_wire,
                      VCS_ZCODE_BENCHMARK_RESULT_WIRE_BYTES + 1, &parsed),
                  VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE);

        reproduced = original;
        zs_root(reproduced.action_root, 35);
        zs_root(reproduced.achieved_environment_root, 36);
        zs_root(reproduced.raw_sample_root, 37);
        zs_root(reproduced.evidence_root, 38);
        reproduced.status = VCS_ZCODE_BENCHMARK_NEGATIVE_RESULT;
        reproduced.sequence = 2;
        reproduced.started_unix = 1400;
        reproduced.finished_unix = 1500;
        ASSERT_EQ(vcs_zcode_benchmark_result_root(&reproduced,
                                                   reproduced_root),
                  VCS_ZCODE_SCIENCE_OK);
        memset(&reproduction, 0, sizeof(reproduction));
        reproduction.schema_version = VCS_ZCODE_SCIENCE_VERSION;
        memcpy(reproduction.study_root, original.study_root, 32);
        memcpy(reproduction.original_result_root, original_root, 32);
        memcpy(reproduction.reproduced_result_root, reproduced_root, 32);
        zs_root(reproduction.comparison_policy_root, 39);
        memcpy(reproduction.original_environment_root,
               original.achieved_environment_root, 32);
        memcpy(reproduction.reproduced_environment_root,
               reproduced.achieved_environment_root, 32);
        zs_root(reproduction.reproducer_pubkey, 40);
        reproduction.verdict = VCS_ZCODE_REPRODUCTION_CONTRADICTED;
        reproduction.sequence = 1;
        reproduction.created_unix = 1600;
        ASSERT_EQ(vcs_zcode_reproduction_validate_for_results(
                      &study, &original, &reproduced, &reproduction, 2000),
                  VCS_ZCODE_SCIENCE_OK);
        uint8_t reproduction_wire[VCS_ZCODE_REPRODUCTION_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_reproduction_serialize(
                      &reproduction, reproduction_wire),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_reproduction_parse(
                      reproduction_wire, sizeof(reproduction_wire),
                      &reproduction_parsed), VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(reproduction_parsed.verdict,
                  VCS_ZCODE_REPRODUCTION_CONTRADICTED);
        uint8_t reproduction_root[32];
        char reproduction_hex[65];
        ASSERT_EQ(vcs_zcode_reproduction_root(
                      &reproduction, reproduction_root),
                  VCS_ZCODE_SCIENCE_OK);
        zcl_hex_encode(reproduction_root, 32, reproduction_hex);
        ASSERT_STR_EQ(reproduction_hex,
            "b516b52e7e4e576d9b240c6ce5716dc98eafbae7d62945ca9b16f9ac11523d26");

        reproduction_parsed.reproduced_environment_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_reproduction_validate_for_results(
                      &study, &original, &reproduced,
                      &reproduction_parsed, 2000),
                  VCS_ZCODE_SCIENCE_ERR_ENVIRONMENT_MISMATCH);
        original.status = 0;
        ASSERT_EQ(vcs_zcode_benchmark_result_validate(&original),
                  VCS_ZCODE_SCIENCE_ERR_STATUS);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zs_findings(void)
{
    int failures = 0;
    TEST("zcode_science: structured findings bind the existing review without a hash cycle") {
        struct vcs_zcode_study_spec_v1 study;
        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate;
        struct vcs_zcode_benchmark_result_v1 result;
        struct vcs_zcode_science_findings_v1 findings, parsed;
        struct vcs_zcode_review_v1 review;
        uint8_t task_root[32], candidate_root[32], result_root[32];
        zs_study(&study);
        zs_task_candidate(&study, &task, &candidate,
                          task_root, candidate_root);
        zs_result(&study, task_root, candidate_root, &result);
        (void)vcs_zcode_benchmark_result_root(&result, result_root);
        memset(&findings, 0, sizeof(findings));
        findings.schema_version = VCS_ZCODE_SCIENCE_VERSION;
        memcpy(findings.study_root, result.study_root, 32);
        memcpy(findings.task_root, task_root, 32);
        memcpy(findings.candidate_root, candidate_root, 32);
        memcpy(findings.result_root, result_root, 32);
        zs_root(findings.proof_set_root, 41);
        zs_root(findings.methods_root, 42);
        zs_root(findings.limitations_root, 43);
        zs_root(findings.conflicts_root, 44);
        findings.flags = VCS_ZCODE_FINDING_NULL;
        findings.severity = VCS_ZCODE_FINDING_MATERIAL;
        findings.sequence = 1;
        findings.created_unix = 1500;
        uint8_t findings_root[32];
        char findings_hex[65];
        ASSERT_EQ(vcs_zcode_science_findings_root(&findings, findings_root),
                  VCS_ZCODE_SCIENCE_OK);
        zcl_hex_encode(findings_root, 32, findings_hex);
        ASSERT_STR_EQ(findings_hex,
            "1875514a5aa0326e2f5758315a756ff6d0220bcee60b9e3bfef03f0a88cf7129");

        memset(&review, 0, sizeof(review));
        review.schema_version = VCS_ZCODE_DEV_VERSION;
        memcpy(review.task_root, task_root, 32);
        memcpy(review.candidate_root, candidate_root, 32);
        memcpy(review.proof_policy_root, task.proof_policy_root, 32);
        memcpy(review.proof_set_root, findings.proof_set_root, 32);
        memcpy(review.findings_root, findings_root, 32);
        zs_root(review.reviewer_pubkey, 45);
        review.verdict = VCS_ZCODE_REVIEW_APPROVE;
        review.sequence = 1;
        review.created_unix = 1500;
        ASSERT_EQ(vcs_zcode_science_findings_validate_for_review(
                      &study, &review, &result, &findings, 2000),
                  VCS_ZCODE_SCIENCE_OK);
        uint8_t wire[VCS_ZCODE_SCIENCE_FINDINGS_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_science_findings_serialize(&findings, wire),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_science_findings_parse(
                      wire, sizeof(wire), &parsed), VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(parsed.flags, VCS_ZCODE_FINDING_NULL);

        review.findings_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_science_findings_validate_for_review(
                      &study, &review, &result, &findings, 2000),
                  VCS_ZCODE_SCIENCE_ERR_REVIEW_MISMATCH);
        findings.flags |= VCS_ZCODE_FINDING_RETRACTION;
        ASSERT_EQ(vcs_zcode_science_findings_validate(&findings),
                  VCS_ZCODE_SCIENCE_ERR_FLAGS);
        zs_root(findings.retraction_target_root, 46);
        ASSERT_EQ(vcs_zcode_science_findings_validate(&findings),
                  VCS_ZCODE_SCIENCE_OK);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zs_curation_vote(void)
{
    int failures = 0;
    TEST("zcode_science: curation is signed network-bound discovery input only") {
        struct vcs_zcode_curation_vote_v1 vote, parsed;
        memset(&vote, 0, sizeof(vote));
        vote.schema_version = VCS_ZCODE_SCIENCE_VERSION;
        zs_root(vote.network_genesis_root, 50);
        zs_root(vote.voter_zid_root, 51);
        zs_root(vote.property_root, 52);
        vote.signal = VCS_ZCODE_CURATION_INTERESTING;
        vote.sequence = 9;
        vote.expires_unix = 5000;
        uint8_t seed[32], secret[32], pubkey[32], id[32], wrong[32];
        char id_hex[65];
        zs_root(seed, 53);
        ed25519_keypair(pubkey, secret, seed);
        ASSERT_EQ(vcs_zcode_curation_vote_seal(&vote, secret, pubkey),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_curation_vote_verify(
                      &vote, vote.network_genesis_root, vote.voter_zid_root,
                      pubkey, 2000), VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_curation_vote_id(&vote, id),
                  VCS_ZCODE_SCIENCE_OK);
        zcl_hex_encode(id, 32, id_hex);
        ASSERT_STR_EQ(id_hex,
            "442ac3dc808c8fd6ecebb3091c08cdbaef218fabe79ab9b8df5630fb2f4306c1");
        uint8_t wire[VCS_ZCODE_CURATION_VOTE_WIRE_BYTES + 1];
        ASSERT_EQ(vcs_zcode_curation_vote_serialize(&vote, wire),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_curation_vote_parse(
                      wire, VCS_ZCODE_CURATION_VOTE_WIRE_BYTES, &parsed),
                  VCS_ZCODE_SCIENCE_OK);
        ASSERT_EQ(vcs_zcode_curation_vote_parse(
                      wire, VCS_ZCODE_CURATION_VOTE_WIRE_BYTES + 1, &parsed),
                  VCS_ZCODE_SCIENCE_ERR_WIRE_SIZE);
        zs_root(wrong, 54);
        ASSERT_EQ(vcs_zcode_curation_vote_verify(
                      &vote, wrong, vote.voter_zid_root, pubkey, 2000),
                  VCS_ZCODE_SCIENCE_ERR_NETWORK_MISMATCH);
        ASSERT_EQ(vcs_zcode_curation_vote_verify(
                      &vote, vote.network_genesis_root, wrong, pubkey, 2000),
                  VCS_ZCODE_SCIENCE_ERR_IDENTITY_MISMATCH);
        vote.property_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_curation_vote_verify(
                      &vote, vote.network_genesis_root, vote.voter_zid_root,
                      pubkey, 2000), VCS_ZCODE_SCIENCE_ERR_SIGNATURE);
        PASS();
    } _test_next:;
    return failures;
}

static void zs_action(struct vcs_build_action_v1 *action, const char *kind,
                      uint64_t sequence)
{
    const char *workdir = NULL, *output = NULL, *resource = NULL;
    memset(action, 0, sizeof(*action));
    zs_root(action->source_sha256, 60);
    zs_root(action->source_cas_sha3, 61);
    zs_root(action->input_root_sha3, 62);
    zs_root(action->toolchain_capsule_sha3, 63);
    (void)vcs_build_action_v1_fixed_flags_root_for_kind(
        kind, action->flags_sha3);
    (void)vcs_build_action_v1_fixed_environment_root_for_kind(
        kind, action->environment_sha3);
    (void)snprintf(action->target, sizeof(action->target), "%s",
                   VCS_BUILD_TARGET_V1);
    (void)snprintf(action->profile, sizeof(action->profile), "science");
    (void)vcs_build_action_v1_descriptors(
        kind, &workdir, &output, &resource);
    (void)snprintf(action->virtual_workdir,
                   sizeof(action->virtual_workdir), "%s", workdir);
    (void)snprintf(action->declared_outputs,
                   sizeof(action->declared_outputs), "%s", output);
    (void)snprintf(action->resource_policy,
                   sizeof(action->resource_policy), "%s", resource);
    action->sequence = sequence;
}

static int test_zs_fixed_actions(void)
{
    int failures = 0;
    TEST("zcode_science: benchmark and reproduction actions are closed identities") {
        ASSERT_EQ(vcs_build_action_v1_work_kind(
                      VCS_BUILD_ACTION_KIND_BENCHMARK_V1),
                  VCS_ZCODE_WORK_TEST);
        ASSERT_EQ(vcs_build_action_v1_work_kind(
                      VCS_BUILD_ACTION_KIND_BENCHMARK_REPRODUCE_V1),
                  VCS_ZCODE_WORK_REPRODUCE);
        ASSERT_EQ(vcs_build_action_v1_work_kind("c23.benchmark.shell.v1"), 0);
        struct vcs_build_action_v1 benchmark, reproduction;
        uint8_t benchmark_root[32], reproduction_root[32];
        zs_action(&benchmark, VCS_BUILD_ACTION_KIND_BENCHMARK_V1, 1);
        zs_action(&reproduction,
                  VCS_BUILD_ACTION_KIND_BENCHMARK_REPRODUCE_V1, 1);
        ASSERT(vcs_build_action_v1_root_for_kind(
            VCS_BUILD_ACTION_KIND_BENCHMARK_V1,
            &benchmark, benchmark_root));
        ASSERT(vcs_build_action_v1_root_for_kind(
            VCS_BUILD_ACTION_KIND_BENCHMARK_REPRODUCE_V1,
            &reproduction, reproduction_root));
        ASSERT(memcmp(benchmark_root, reproduction_root, 32) != 0);
        ASSERT(!vcs_build_action_v1_root_for_kind(
            VCS_BUILD_ACTION_KIND_BENCHMARK_REPRODUCE_V1,
            &benchmark, reproduction_root));
        ASSERT_STR_EQ(benchmark.resource_policy,
                      VCS_BUILD_BENCHMARK_RESOURCE_POLICY_V1);
        ASSERT(strstr(benchmark.resource_policy, "network=0") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_science(void)
{
    int failures = 0;
    failures += test_zs_study_codec();
    failures += test_zs_result_and_reproduction();
    failures += test_zs_findings();
    failures += test_zs_curation_vote();
    failures += test_zs_fixed_actions();
    printf("=== zcode_science: %d failures ===\n", failures);
    return failures;
}
