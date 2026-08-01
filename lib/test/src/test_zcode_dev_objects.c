/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Prove canonical ZCODE task/candidate/policy/review/receipt wires. */

#include "test/test_core.h"

#include "base/hex.h"
#include "command/native_command.h"
#include "crypto/ed25519.h"
#include "json/json.h"
#include "models/build_fabric.h"
#include "models/database.h"
#include "platform/time_compat.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_worker.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_work_node.h"
#include "vcs/zcode_work_swarm.h"

#include <stdio.h>
#include <stdlib.h>
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

static void zd_swarm_result(
    struct vcs_zcode_work_result_v1 *result,
    const struct vcs_zcode_work_request_v1 *request,
    uint8_t output_value, uint8_t signer_value)
{
    memset(result, 0, sizeof(*result));
    result->request_id = request->request_id;
    memcpy(result->task_root, request->task_root, 32);
    memcpy(result->candidate_root, request->candidate_root, 32);
    memcpy(result->action_root, request->action_root, 32);
    zd_root(result->output_root, output_value);
    struct vcs_zcode_work_receipt_v1 *receipt = &result->receipt;
    receipt->schema_version = VCS_ZCODE_DEV_VERSION;
    memcpy(receipt->task_root, request->task_root, 32);
    memcpy(receipt->candidate_root, request->candidate_root, 32);
    memcpy(receipt->action_root, request->action_root, 32);
    memcpy(receipt->input_root, request->input_root, 32);
    memcpy(receipt->output_root, result->output_root, 32);
    memcpy(receipt->proof_policy_root, request->proof_policy_root, 32);
    memcpy(receipt->toolchain_capsule_root,
           request->toolchain_capsule_root, 32);
    zd_root(receipt->lease_id, 40);
    zd_root(receipt->evidence_root, 41);
    zd_root(receipt->confinement_root, 42);
    receipt->work_kind = request->work_kind;
    receipt->status = VCS_ZCODE_WORK_PASS;
    receipt->started_unix = 1000;
    receipt->finished_unix = 1001;
    uint8_t seed[32], secret[32], public_key[32];
    zd_root(seed, signer_value);
    ed25519_keypair(public_key, secret, seed);
    (void)vcs_zcode_work_receipt_seal(receipt, secret, public_key);
}

static int test_zd_work_swarm(void)
{
    int failures = 0;
    TEST("zcode_dev: work swarm binds requests and counts signer quorum") {
        struct vcs_zcode_work_swarm_message message = {0}, parsed = {0};
        message.type = VCS_ZCODE_WORK_SWARM_CAPABILITY;
        zd_root(message.body.capability.toolchain_capsule_root, 31);
        message.body.capability.work_kinds =
            (UINT32_C(1) << VCS_ZCODE_WORK_BUILD) |
            (UINT32_C(1) << VCS_ZCODE_WORK_TEST);
        message.body.capability.target =
            VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3;
        message.body.capability.confinement =
            VCS_ZCODE_WORK_CONFINEMENT_V1_MASK;
        message.body.capability.max_cpu_seconds = 60;
        message.body.capability.max_memory_bytes = UINT64_C(512) * 1024 * 1024;
        message.body.capability.max_output_bytes = UINT64_C(64) * 1024 * 1024;
        message.body.capability.max_lease_seconds = 120;
        message.body.capability.slots = 2;
        message.body.capability.queue_headroom = 2;
        message.body.capability.expires_unix = 2000;
        uint8_t requester_seed[32], requester_secret[32], requester_key[32];
        zd_root(requester_seed, 30);
        ed25519_keypair(requester_key, requester_secret, requester_seed);
        ASSERT(vcs_zcode_work_capability_seal(&message.body.capability,
                                              requester_secret,
                                              requester_key));
        uint8_t wire[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_work_swarm_wire_size(&message), 184);
        ASSERT(vcs_zcode_work_swarm_serialize(&message, wire, sizeof(wire),
                                              &wire_len));
        ASSERT_EQ(wire_len, 184);
        ASSERT(vcs_zcode_work_swarm_parse(wire, wire_len, &parsed));
        ASSERT_EQ(parsed.body.capability.slots, 2);
        wire[110] ^= 1;
        ASSERT(!vcs_zcode_work_swarm_parse(wire, wire_len, &parsed));

        memset(&message, 0, sizeof(message));
        message.type = VCS_ZCODE_WORK_SWARM_REQUEST;
        struct vcs_zcode_work_request_v1 *request = &message.body.request;
        request->request_id = 77;
        zd_root(request->task_root, 33);
        zd_root(request->candidate_root, 34);
        zd_root(request->action_root, 35);
        zd_root(request->input_root, 36);
        zd_root(request->context_root, 39);
        zd_root(request->proof_policy_root, 37);
        zd_root(request->toolchain_capsule_root, 38);
        request->work_kind = VCS_ZCODE_WORK_BUILD;
        request->target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3;
        request->max_cpu_seconds = 60;
        request->max_memory_bytes = UINT64_C(512) * 1024 * 1024;
        request->max_output_bytes = UINT64_C(64) * 1024 * 1024;
        request->deadline_unix = 2000;
        ASSERT(vcs_zcode_work_request_seal(request, requester_secret,
                                           requester_key));
        ASSERT(vcs_zcode_work_swarm_serialize(&message, wire, sizeof(wire),
                                              &wire_len));
        ASSERT_EQ(wire_len, 372);
        ASSERT(vcs_zcode_work_swarm_parse(wire, wire_len, &parsed));
        ASSERT_EQ(parsed.body.request.request_id, 77);
        struct vcs_zcode_work_request_v1 pinned_request = parsed.body.request;
        request = &pinned_request;

        struct vcs_zcode_work_result_v1 results[2], mismatched[2];
        zd_swarm_result(&results[0], request, 43, 44);
        zd_swarm_result(&results[1], request, 43, 45);
        uint8_t approved[2][32];
        memcpy(approved[0], results[0].receipt.signer_pubkey, 32);
        memcpy(approved[1], results[1].receipt.signer_pubkey, 32);
        ASSERT(vcs_zcode_work_result_verify(request, &results[0],
                                            approved[0]));
        ASSERT(!vcs_zcode_work_result_verify(request, &results[0],
                                             approved[1]));
        memset(&message, 0, sizeof(message));
        message.type = VCS_ZCODE_WORK_SWARM_RESULT;
        message.body.result = results[0];
        ASSERT(vcs_zcode_work_swarm_serialize(&message, wire, sizeof(wire),
                                              &wire_len));
        ASSERT_EQ(wire_len, 592);
        ASSERT(vcs_zcode_work_swarm_parse(wire, wire_len, &parsed));
        ASSERT(vcs_zcode_work_result_verify(request, &parsed.body.result,
                                            approved[0]));
        uint8_t agreed_root[32];
        ASSERT_EQ(vcs_zcode_work_result_quorum(request, results, 2, approved,
                  2, 2, agreed_root), 2);
        ASSERT(memcmp(agreed_root, results[0].output_root, 32) == 0);

        mismatched[0] = results[0];
        zd_swarm_result(&mismatched[1], request, 46, 45);
        ASSERT_EQ(vcs_zcode_work_result_quorum(request, mismatched, 2,
                  approved, 2, 2, agreed_root), 1);
        uint8_t zero[32] = {0};
        ASSERT(memcmp(agreed_root, zero, 32) == 0);
        results[1] = results[0];
        ASSERT_EQ(vcs_zcode_work_result_quorum(request, results, 2, approved,
                  2, 2, agreed_root), 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zd_work_node(void)
{
    int failures = 0;
    TEST("zcode_dev: existing package peers carry requester-owned work") {
        struct vcs_zcode_work_node *requester = vcs_zcode_work_node_create();
        struct vcs_zcode_work_node *worker = vcs_zcode_work_node_create();
        ASSERT(requester && worker);
        ASSERT(vcs_zcode_work_node_peer_add(requester, 11));
        ASSERT(vcs_zcode_work_node_peer_add(worker, 22));
        uint8_t worker_seed[32], worker_secret[32], worker_key[32];
        zd_root(worker_seed, 71);
        ed25519_keypair(worker_key, worker_secret, worker_seed);
        struct vcs_zcode_work_capability_v1 capability = {0};
        memcpy(capability.signer_pubkey, worker_key, 32);
        zd_root(capability.toolchain_capsule_root, 72);
        capability.work_kinds = UINT32_C(1) << VCS_ZCODE_WORK_BUILD;
        capability.target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3;
        capability.confinement = VCS_ZCODE_WORK_CONFINEMENT_V1_MASK;
        capability.max_cpu_seconds = 60;
        capability.max_memory_bytes = UINT64_C(512) * 1024 * 1024;
        capability.max_output_bytes = UINT64_C(64) * 1024 * 1024;
        capability.max_lease_seconds = 120;
        capability.slots = 2;
        capability.queue_headroom = 2;
        capability.expires_unix = 2000;
        ASSERT(vcs_zcode_work_capability_seal(
            &capability, worker_secret, worker_key));
        ASSERT(vcs_zcode_work_node_set_local_capability(worker, &capability));
        uint8_t frame[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES];
        uint64_t peer_out = 0; size_t frame_len = 0;
        ASSERT(vcs_zcode_work_node_next_outbound(
            worker, 22, &peer_out, frame, &frame_len));
        ASSERT_EQ(peer_out, 22);
        ASSERT_EQ(vcs_zcode_work_node_handle_frame(
            requester, 11, frame, frame_len, 1000), VCS_ZCODE_WORK_NODE_OK);
        struct vcs_zcode_work_capability_v1 observed;
        ASSERT(vcs_zcode_work_node_peer_capability(
            requester, 11, 1000, &observed));
        ASSERT(memcmp(observed.signer_pubkey, worker_key, 32) == 0);

        uint8_t requester_seed[32], requester_secret[32], requester_key[32];
        zd_root(requester_seed, 73);
        ed25519_keypair(requester_key, requester_secret, requester_seed);
        struct vcs_zcode_work_request_v1 request = {0};
        request.request_id = 700;
        zd_root(request.task_root, 74); zd_root(request.candidate_root, 75);
        zd_root(request.action_root, 76); zd_root(request.input_root, 77);
        zd_root(request.context_root, 78); zd_root(request.proof_policy_root, 79);
        memcpy(request.toolchain_capsule_root,
               capability.toolchain_capsule_root, 32);
        request.work_kind = VCS_ZCODE_WORK_BUILD;
        request.target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3;
        request.max_cpu_seconds = 60;
        request.max_memory_bytes = UINT64_C(512) * 1024 * 1024;
        request.max_output_bytes = UINT64_C(64) * 1024 * 1024;
        request.deadline_unix = 1100;
        ASSERT(vcs_zcode_work_request_seal(
            &request, requester_secret, requester_key));
        ASSERT_EQ(vcs_zcode_work_node_submit(requester, 11, &request, 1000),
                  VCS_ZCODE_WORK_NODE_OK);
        ASSERT(vcs_zcode_work_node_next_outbound(
            requester, 11, &peer_out, frame, &frame_len));
        ASSERT_EQ(vcs_zcode_work_node_handle_frame(
            worker, 22, frame, frame_len, 1000), VCS_ZCODE_WORK_NODE_OK);
        struct vcs_zcode_work_request_v1 received;
        ASSERT(vcs_zcode_work_node_next_request(worker, &peer_out, &received));
        ASSERT_EQ(received.request_id, 700);
        struct vcs_zcode_work_result_v1 result;
        zd_swarm_result(&result, &received, 80, 71);
        ASSERT_EQ(vcs_zcode_work_node_publish_result(worker, 22, &result),
                  VCS_ZCODE_WORK_NODE_OK);
        ASSERT(vcs_zcode_work_node_next_outbound(
            worker, 22, &peer_out, frame, &frame_len));
        ASSERT_EQ(vcs_zcode_work_node_handle_frame(
            requester, 11, frame, frame_len, 1001), VCS_ZCODE_WORK_NODE_OK);
        ASSERT_EQ(vcs_zcode_work_node_handle_frame(
            requester, 11, frame, frame_len, 1001),
                  VCS_ZCODE_WORK_NODE_REPLAY);
        struct vcs_zcode_work_result_v1 accepted;
        ASSERT(vcs_zcode_work_node_next_result(
            requester, &peer_out, &accepted));
        ASSERT(vcs_zcode_work_result_verify(&request, &accepted, worker_key));

        request.request_id = 701;
        ASSERT(vcs_zcode_work_request_seal(
            &request, requester_secret, requester_key));
        ASSERT_EQ(vcs_zcode_work_node_submit(requester, 11, &request, 1000),
                  VCS_ZCODE_WORK_NODE_OK);
        ASSERT(vcs_zcode_work_node_next_outbound(
            requester, 11, &peer_out, frame, &frame_len));
        ASSERT_EQ(vcs_zcode_work_node_handle_frame(
            worker, 22, frame, frame_len, 1000), VCS_ZCODE_WORK_NODE_OK);
        struct vcs_zcode_work_cancel_v1 cancel = { .request_id = 701 };
        memcpy(cancel.task_root, request.task_root, 32);
        ASSERT(vcs_zcode_work_cancel_seal(
            &cancel, requester_secret, requester_key));
        ASSERT_EQ(vcs_zcode_work_node_cancel(requester, 11, &cancel),
                  VCS_ZCODE_WORK_NODE_OK);
        ASSERT(vcs_zcode_work_node_next_outbound(
            requester, 11, &peer_out, frame, &frame_len));
        ASSERT_EQ(vcs_zcode_work_node_handle_frame(
            worker, 22, frame, frame_len, 1001), VCS_ZCODE_WORK_NODE_OK);
        struct vcs_zcode_work_cancel_v1 received_cancel;
        ASSERT(vcs_zcode_work_node_next_cancel(
            worker, &peer_out, &received_cancel));
        ASSERT_EQ(received_cancel.request_id, 701);
        vcs_zcode_work_node_free(requester);
        vcs_zcode_work_node_free(worker);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zd_improve_command(void)
{
    int failures = 0;
    TEST("zcode_dev: improve stores canonical task and queues existing ZBuild") {
        char dir[256], preprocessed[320], workspace[4096];
        test_make_tmpdir(dir, sizeof(dir), "zcode_dev", "improve");
        ASSERT(realpath(dir, workspace) != NULL);
        (void)snprintf(preprocessed, sizeof(preprocessed), "%s/unit.i",
                       workspace);
        FILE *f = fopen(preprocessed, "wb");
        ASSERT(f != NULL);
        static const char source[] = "int zcode_improve_fixture(void){return 1;}\n";
        ASSERT(fwrite(source, 1, sizeof(source) - 1u, f) ==
               sizeof(source) - 1u);
        ASSERT(fclose(f) == 0);

        struct vcs_zcode_proof_policy_v1 policy;
        zd_policy(&policy);
        uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
        char policy_hex[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES * 2u + 1u];
        ASSERT_EQ(vcs_zcode_proof_policy_serialize(&policy, policy_wire),
                  VCS_ZCODE_DEV_OK);
        zcl_hex_encode(policy_wire, sizeof(policy_wire), policy_hex);
        char roots[10][65];
        for (size_t i = 0; i < 10; i++) {
            uint8_t root[32];
            zd_root(root, (uint8_t)(i + 1));
            zcl_hex_encode(root, 32, roots[i]);
        }
        struct json_value input;
        json_init(&input); json_set_object(&input);
        (void)json_push_kv_str(&input, "workspace", workspace);
        (void)json_push_kv_str(&input, "datadir", workspace);
        (void)json_push_kv_str(&input, "candidate_source_sha256", roots[9]);
        (void)json_push_kv_str(&input, "source_root", roots[0]);
        (void)json_push_kv_str(&input, "dependency_lock_root", roots[1]);
        (void)json_push_kv_str(&input, "write_scope_root", roots[2]);
        (void)json_push_kv_str(&input, "acceptance_tests_root", roots[3]);
        (void)json_push_kv_str(&input, "model_policy_root", roots[4]);
        (void)json_push_kv_str(&input, "patch_root", roots[5]);
        (void)json_push_kv_str(&input, "candidate_source_root", roots[6]);
        (void)json_push_kv_str(&input, "adapter_policy_root", roots[7]);
        (void)json_push_kv_str(&input, "author_pubkey", roots[8]);
        (void)json_push_kv_int(&input, "remote_peer", 99);
        (void)json_push_kv_str(&input, "context_root", roots[5]);
        (void)json_push_kv_str(&input, "goal", "fix deterministic fixture");
        (void)json_push_kv_str(&input, "proof_policy_hex", policy_hex);
        (void)json_push_kv_str(&input, "preprocessed_path", preprocessed);
        int64_t expires = (int64_t)platform_time_wall_unix() + 3600;
        (void)json_push_kv_int(&input, "expires_unix", expires);
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_improve.v1");
        zcl_native_handle_zcode_improve(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        const char *task_hex = json_get_str(json_get(&reply.data, "task_root"));
        const char *candidate_hex =
            json_get_str(json_get(&reply.data, "candidate_root"));
        const char *action_id = json_get_str(json_get(&reply.data, "action_id"));
        ASSERT(task_hex && candidate_hex && action_id);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "state")), "QUEUED");
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "lane")), "FRONTIER");
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "remote_outcome")),
                      "LOCAL_FALLBACK");
        uint8_t task_root[32], *task_wire = NULL;
        size_t task_wire_len = 0;
        ASSERT(zcl_hex_decode_lower(task_hex, task_root, 32));
        ASSERT_EQ(vcs_object_load_raw(workspace, task_root, &task_wire,
                                      &task_wire_len), 0);
        struct vcs_zcode_task_v1 task;
        ASSERT_EQ(vcs_zcode_task_parse(task_wire, task_wire_len, &task),
                  VCS_ZCODE_DEV_OK);
        free(task_wire);
        ASSERT_EQ(task.expires_unix, expires);
        char db_path[320];
        (void)snprintf(db_path, sizeof(db_path), "%s/node.db", workspace);
        struct node_db ndb = {0};
        ASSERT(node_db_open(&ndb, db_path));
        struct db_build_action action;
        ASSERT(db_build_action_find(&ndb, action_id, &action));
        ASSERT_STR_EQ(action.state, "QUEUED");
        ASSERT_STR_EQ(action.task_root_sha3, task_hex);
        ASSERT_STR_EQ(action.candidate_root_sha3, candidate_hex);
        ASSERT_STR_EQ(action.context_root_sha3, roots[5]);
        uint8_t candidate_root[32], *candidate_wire = NULL;
        size_t candidate_wire_len = 0;
        ASSERT(zcl_hex_decode_lower(candidate_hex, candidate_root, 32));
        ASSERT_EQ(vcs_object_load_raw(workspace, candidate_root,
                                      &candidate_wire,
                                      &candidate_wire_len), 0);
        struct vcs_zcode_candidate_v1 candidate;
        ASSERT_EQ(vcs_zcode_candidate_parse(candidate_wire,
                  candidate_wire_len, &candidate), VCS_ZCODE_DEV_OK);
        free(candidate_wire);
        struct db_build_worker worker;
        uint8_t worker_secret[32], worker_key[32];
        ASSERT(build_fabric_worker_identity_load(
            workspace, &worker, worker_secret, worker_key).ok);
        int64_t now = (int64_t)platform_time_wall_unix();
        ASSERT(build_fabric_worker_approve(&ndb, &worker, now).ok);
        uint8_t lease_root[32]; char lease_hex[65];
        zd_root(lease_root, 60); zcl_hex_encode(lease_root, 32, lease_hex);
        bool claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, lease_hex, now,
                                  300, &action, &claimed).ok);
        ASSERT(claimed);
        struct db_build_receipt receipt;
        ASSERT(build_fabric_worker_execute(
            &ndb, workspace, action_id, lease_hex, worker_secret, worker_key,
            &receipt).ok);
        ASSERT(strlen(receipt.work_receipt_sha3) == 64);
        uint8_t receipt_root[32], *receipt_wire = NULL;
        size_t receipt_wire_len = 0;
        ASSERT(zcl_hex_decode_lower(receipt.work_receipt_sha3,
                                    receipt_root, 32));
        ASSERT_EQ(vcs_object_load_raw(workspace, receipt_root, &receipt_wire,
                                      &receipt_wire_len), 0);
        struct vcs_zcode_work_receipt_v1 work_receipt;
        ASSERT_EQ(vcs_zcode_work_receipt_parse(
            receipt_wire, receipt_wire_len, &work_receipt), VCS_ZCODE_DEV_OK);
        free(receipt_wire);
        ASSERT_EQ(vcs_zcode_work_receipt_verify(&work_receipt, worker_key),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_work_receipt_validate_for_candidate(
            &task, &candidate, &work_receipt,
            (int64_t)platform_time_wall_unix()), VCS_ZCODE_DEV_OK);
        node_db_close(&ndb);
        zcl_command_reply_free(&reply);
        json_free(&input);
        test_rm_rf(dir);
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
    failures += test_zd_work_swarm();
    failures += test_zd_work_node();
    failures += test_zd_improve_command();
    printf("=== zcode_dev_objects: %d failures ===\n", failures);
    return failures;
}
