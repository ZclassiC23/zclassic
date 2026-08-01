/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Prove canonical ZCODE task/candidate/policy/review/receipt wires. */

#include "test/test_core.h"

#include "base/hex.h"
#include "crypto/ed25519.h"
#include "vcs/zcode_dev.h"

#include <stdio.h>
#include <string.h>

static void zd_root(uint8_t out[32], uint8_t value)
{
    memset(out, value, 32);
}

static void zd_policy(struct vcs_zcode_proof_policy_v1 *p)
{
    memset(p, 0, sizeof(*p));
    p->schema_version = VCS_ZCODE_DEV_VERSION;
    p->required_proofs = VCS_ZCODE_PROOF_COMPILE | VCS_ZCODE_PROOF_TEST |
                         VCS_ZCODE_PROOF_FUZZ | VCS_ZCODE_PROOF_REVIEW |
                         VCS_ZCODE_PROOF_LOCAL_REPRODUCTION;
    p->minimum_compile_receipts = 2;
    p->minimum_test_receipts = 2;
    p->minimum_fuzz_receipts = 1;
    p->minimum_reviews = 1;
    p->minimum_matching_receipts = 2;
    p->flags = VCS_ZCODE_POLICY_INDEPENDENT_SIGNERS |
               VCS_ZCODE_POLICY_RELEASE_BYTE_IDENTITY;
    p->deterministic_fuzz_seeds = 64;
    p->audit_basis_points = 100;
    p->maximum_proof_age_seconds = 3600;
}

static void zd_task(struct vcs_zcode_task_v1 *t,
                    const uint8_t policy_root[32])
{
    memset(t, 0, sizeof(*t));
    t->schema_version = VCS_ZCODE_DEV_VERSION;
    zd_root(t->source_root, 1);
    zd_root(t->dependency_lock_root, 2);
    zd_root(t->toolchain_capsule_root, 3);
    zd_root(t->write_scope_root, 4);
    zd_root(t->acceptance_tests_root, 5);
    memcpy(t->proof_policy_root, policy_root, 32);
    zd_root(t->model_policy_root, 7);
    zd_root(t->goal_root, 8);
    t->capabilities = VCS_ZCODE_TASK_CAP_V1_MASK;
    t->max_changed_files = 32;
    t->max_patch_bytes = 1024 * 1024;
    t->max_context_bytes = 2 * 1024 * 1024;
    t->max_cpu_seconds = 120;
    t->max_memory_bytes = UINT64_C(512) * 1024 * 1024;
    t->max_output_bytes = UINT64_C(64) * 1024 * 1024;
    t->expires_unix = 2000;
}

static void zd_candidate(struct vcs_zcode_candidate_v1 *c,
                         const struct vcs_zcode_task_v1 *task,
                         const uint8_t task_root[32])
{
    memset(c, 0, sizeof(*c));
    c->schema_version = VCS_ZCODE_DEV_VERSION;
    memcpy(c->task_root, task_root, 32);
    memcpy(c->base_source_root, task->source_root, 32);
    zd_root(c->patch_root, 9);
    zd_root(c->candidate_source_root, 10);
    zd_root(c->adapter_policy_root, 11);
    zd_root(c->author_pubkey, 12);
    c->sequence = 1;
    c->created_unix = 1000;
}

static int test_zd_policy_and_task(void)
{
    int failures = 0;
    TEST("zcode_dev: proof policy and task are canonical closed wires") {
        struct vcs_zcode_proof_policy_v1 policy, policy2;
        zd_policy(&policy);
        uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
        uint8_t policy_root[32], policy_root2[32];
        ASSERT_EQ(vcs_zcode_proof_policy_serialize(&policy, policy_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(policy_wire, "ZCPOLY\r\n", 8) == 0);
        ASSERT_EQ(vcs_zcode_proof_policy_parse(policy_wire,
                  sizeof(policy_wire), &policy2), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_policy_root(&policy, policy_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_policy_root(&policy2, policy_root2),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(policy_root, policy_root2, 32) == 0);

        struct vcs_zcode_task_v1 task, task2;
        zd_task(&task, policy_root);
        uint8_t task_wire[VCS_ZCODE_TASK_WIRE_BYTES];
        uint8_t task_root[32], task_root2[32];
        ASSERT_EQ(vcs_zcode_task_serialize(&task, task_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(task_wire, "ZCTASK\r\n", 8) == 0);
        ASSERT_EQ(vcs_zcode_task_parse(task_wire, sizeof(task_wire), &task2),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_task_root(&task, task_root), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_task_root(&task2, task_root2), VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(task_root, task_root2, 32) == 0);
        char policy_hex[65], task_hex[65];
        zcl_hex_encode(policy_root, 32, policy_hex);
        zcl_hex_encode(task_root, 32, task_hex);
        ASSERT_STR_EQ(policy_hex,
            "ab021a505c125b7aacc05499a4ec0ca153d7762a394d5bf612ce4ca83a2c9346");
        ASSERT_STR_EQ(task_hex,
            "973ef3c8d799329d22e6b802878ddfe20659f419fe8b5076259549a81d1282fd");
        ASSERT_EQ(vcs_zcode_task_validate_at(&task, 1999), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_task_validate_at(&task, 2000),
                  VCS_ZCODE_DEV_ERR_EXPIRY);

        task2.capabilities |= 1u << 31;
        ASSERT_EQ(vcs_zcode_task_validate(&task2),
                  VCS_ZCODE_DEV_ERR_CAPABILITY);
        policy2.required_proofs &= ~VCS_ZCODE_PROOF_FUZZ;
        ASSERT_EQ(vcs_zcode_proof_policy_validate(&policy2),
                  VCS_ZCODE_DEV_ERR_POLICY);
        policy_wire[0] ^= 1;
        ASSERT_EQ(vcs_zcode_proof_policy_parse(policy_wire,
                  sizeof(policy_wire), &policy2),
                  VCS_ZCODE_DEV_ERR_WIRE_MAGIC);
        ASSERT_EQ(vcs_zcode_task_parse(task_wire, sizeof(task_wire) - 1,
                                       &task2),
                  VCS_ZCODE_DEV_ERR_WIRE_SIZE);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zd_candidate_review(void)
{
    int failures = 0;
    TEST("zcode_dev: candidates and reviews bind the exact task and source") {
        struct vcs_zcode_proof_policy_v1 policy;
        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate, candidate2;
        struct vcs_zcode_review_v1 review, review2;
        uint8_t policy_root[32], task_root[32], candidate_root[32];
        zd_policy(&policy);
        ASSERT_EQ(vcs_zcode_proof_policy_root(&policy, policy_root),
                  VCS_ZCODE_DEV_OK);
        zd_task(&task, policy_root);
        ASSERT_EQ(vcs_zcode_task_root(&task, task_root), VCS_ZCODE_DEV_OK);
        zd_candidate(&candidate, &task, task_root);
        ASSERT_EQ(vcs_zcode_candidate_validate_for_task(
                      &task, &candidate, 1500), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_candidate_root(&candidate, candidate_root),
                  VCS_ZCODE_DEV_OK);

        uint8_t candidate_wire[VCS_ZCODE_CANDIDATE_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_candidate_serialize(&candidate, candidate_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(candidate_wire, "ZCCAND\r\n", 8) == 0);
        ASSERT_EQ(vcs_zcode_candidate_parse(candidate_wire,
                  sizeof(candidate_wire), &candidate2), VCS_ZCODE_DEV_OK);

        memset(&review, 0, sizeof(review));
        review.schema_version = VCS_ZCODE_DEV_VERSION;
        memcpy(review.task_root, task_root, 32);
        memcpy(review.candidate_root, candidate_root, 32);
        memcpy(review.proof_policy_root, policy_root, 32);
        zd_root(review.proof_set_root, 13);
        zd_root(review.findings_root, 14);
        zd_root(review.reviewer_pubkey, 15);
        review.verdict = VCS_ZCODE_REVIEW_APPROVE;
        review.sequence = 1;
        review.created_unix = 1300;
        ASSERT_EQ(vcs_zcode_review_validate_for_candidate(
                      &task, &candidate, &review, 1500), VCS_ZCODE_DEV_OK);
        uint8_t review_wire[VCS_ZCODE_REVIEW_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_review_serialize(&review, review_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(review_wire, "ZCREVW\r\n", 8) == 0);
        ASSERT_EQ(vcs_zcode_review_parse(review_wire, sizeof(review_wire),
                                         &review2), VCS_ZCODE_DEV_OK);
        uint8_t review_root[32];
        char candidate_hex[65], review_hex[65];
        ASSERT_EQ(vcs_zcode_review_root(&review, review_root),
                  VCS_ZCODE_DEV_OK);
        zcl_hex_encode(candidate_root, 32, candidate_hex);
        zcl_hex_encode(review_root, 32, review_hex);
        ASSERT_STR_EQ(candidate_hex,
            "fc65669e05d9a84cee90be08469cb00640fbe92eb1254b39cb24d40731235ca6");
        ASSERT_STR_EQ(review_hex,
            "a6f77d69c421c5cfee7b8f40578f53fc22abcaa88a8b789f4b59448fdac53713");

        candidate2.base_source_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_candidate_validate_for_task(
                      &task, &candidate2, 1500),
                  VCS_ZCODE_DEV_ERR_SOURCE_STALE);
        review2.candidate_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_review_validate_for_candidate(
                      &task, &candidate, &review2, 1500),
                  VCS_ZCODE_DEV_ERR_OUTPUT_MISMATCH);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zd_receipt(void)
{
    int failures = 0;
    TEST("zcode_dev: signed work receipts refuse stale task inputs") {
        struct vcs_zcode_proof_policy_v1 policy;
        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate;
        struct vcs_zcode_work_receipt_v1 receipt, parsed;
        uint8_t policy_root[32], task_root[32], candidate_root[32];
        zd_policy(&policy);
        ASSERT_EQ(vcs_zcode_proof_policy_root(&policy, policy_root),
                  VCS_ZCODE_DEV_OK);
        zd_task(&task, policy_root);
        ASSERT_EQ(vcs_zcode_task_root(&task, task_root), VCS_ZCODE_DEV_OK);
        zd_candidate(&candidate, &task, task_root);
        ASSERT_EQ(vcs_zcode_candidate_root(&candidate, candidate_root),
                  VCS_ZCODE_DEV_OK);

        memset(&receipt, 0, sizeof(receipt));
        receipt.schema_version = VCS_ZCODE_DEV_VERSION;
        memcpy(receipt.task_root, task_root, 32);
        memcpy(receipt.candidate_root, candidate_root, 32);
        zd_root(receipt.action_root, 16);
        memcpy(receipt.input_root, task.source_root, 32);
        memcpy(receipt.output_root, candidate_root, 32);
        memcpy(receipt.proof_policy_root, policy_root, 32);
        memcpy(receipt.toolchain_capsule_root,
               task.toolchain_capsule_root, 32);
        zd_root(receipt.lease_id, 17);
        zd_root(receipt.evidence_root, 18);
        zd_root(receipt.confinement_root, 19);
        receipt.work_kind = VCS_ZCODE_WORK_PROPOSE;
        receipt.status = VCS_ZCODE_WORK_PASS;
        receipt.exit_status = 0;
        receipt.started_unix = 1100;
        receipt.finished_unix = 1200;

        uint8_t seed[32], secret[32], pubkey[32];
        zd_root(seed, 20);
        ed25519_keypair(pubkey, secret, seed);
        ASSERT_EQ(vcs_zcode_work_receipt_seal(&receipt, secret, pubkey),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_work_receipt_verify(&receipt, pubkey),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_work_receipt_validate_for_candidate(
                      &task, &candidate, &receipt, 1500), VCS_ZCODE_DEV_OK);

        uint8_t wire[VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_work_receipt_serialize(&receipt, wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(wire, "ZCWRCP\r\n", 8) == 0);
        ASSERT_EQ(vcs_zcode_work_receipt_parse(wire, sizeof(wire), &parsed),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_work_receipt_verify(&parsed, pubkey),
                  VCS_ZCODE_DEV_OK);
        uint8_t receipt_id[32];
        char receipt_hex[65];
        ASSERT_EQ(vcs_zcode_work_receipt_id(&receipt, receipt_id),
                  VCS_ZCODE_DEV_OK);
        zcl_hex_encode(receipt_id, 32, receipt_hex);
        ASSERT_STR_EQ(receipt_hex,
            "20483010b65179890c2a7bc9d0f4a19444e68c9a8346723dd49be13676848822");

        parsed.toolchain_capsule_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_work_receipt_validate_for_candidate(
                      &task, &candidate, &parsed, 1500),
                  VCS_ZCODE_DEV_ERR_TOOLCHAIN_STALE);
        parsed = receipt;
        parsed.input_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_work_receipt_validate_for_candidate(
                      &task, &candidate, &parsed, 1500),
                  VCS_ZCODE_DEV_ERR_SOURCE_STALE);
        parsed = receipt;
        parsed.signature[0] ^= 1;
        ASSERT_EQ(vcs_zcode_work_receipt_verify(&parsed, pubkey),
                  VCS_ZCODE_DEV_ERR_SIGNATURE);
        ASSERT_EQ(vcs_zcode_work_receipt_validate_for_candidate(
                      &task, &candidate, &receipt, 2000),
                  VCS_ZCODE_DEV_ERR_EXPIRY);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_dev_objects(void)
{
    int failures = 0;
    failures += test_zd_policy_and_task();
    failures += test_zd_candidate_review();
    failures += test_zd_receipt();
    printf("=== zcode_dev_objects: %d failures ===\n", failures);
    return failures;
}
