/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Prove canonical ZCODE task/candidate/policy/review/receipt wires. */

#include "test/test_core.h"

#include "base/hex.h"
#include "codeindex/codeindex_merkle.h"
#include "command/native_command.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include "models/build_fabric.h"
#include "models/database.h"
#include "models/zcode_lane.h"
#include "platform/time_compat.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_worker.h"
#include "services/zcode_lane_service.h"
#include "util/safe_alloc.h"
#include "vcs/package_manifest.h"
#include "vcs/package_store.h"
#include "vcs/build_action.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_agent_context.h"
#include "vcs/zcode_lane.h"
#include "vcs/zcode_work_context.h"
#include "vcs/zcode_work_node.h"
#include "vcs/zcode_work_swarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int test_zd_agent_context(void)
{
    int failures = 0;
    TEST("zcode_dev: agent context is canonical bounded source evidence") {
        static const char header[] = "int widget(int);\n";
        static const char source[] =
            "int widget(int x) { return x + 1; }\n";
        struct vcs_zcode_agent_context_v1 context;
        vcs_zcode_agent_context_init(&context);
        zd_root(context.task_root, 1);
        zd_root(context.source_root, 2);
        zd_root(context.goal_root, 3);
        zd_root(context.source_tree_root, 4);
        (void)snprintf(context.query, sizeof(context.query), "widget");
        context.file_count = 2;
        (void)snprintf(context.files[0].path,
                       sizeof(context.files[0].path), "include/widget.h");
        context.files[0].start_line = 1;
        context.files[0].full_file_bytes = sizeof(header) - 1u;
        context.files[0].content_len = sizeof(header) - 1u;
        context.files[0].content = zcl_malloc(
            context.files[0].content_len, "test.agent_context.header");
        ASSERT(context.files[0].content != NULL);
        memcpy(context.files[0].content, header,
               context.files[0].content_len);
        sha3_256(context.files[0].content, context.files[0].content_len,
                 context.files[0].content_root);
        (void)snprintf(context.files[1].path,
                       sizeof(context.files[1].path), "src/widget.c");
        context.files[1].start_line = 7;
        context.files[1].full_file_bytes = sizeof(source) - 1u;
        context.files[1].content_len = sizeof(source) - 1u;
        context.files[1].content = zcl_malloc(
            context.files[1].content_len, "test.agent_context.source");
        ASSERT(context.files[1].content != NULL);
        memcpy(context.files[1].content, source,
               context.files[1].content_len);
        sha3_256(context.files[1].content, context.files[1].content_len,
                 context.files[1].content_root);

        uint8_t root[32]; char root_hex[65];
        ASSERT_EQ(vcs_zcode_agent_context_validate(&context, 4096),
                  VCS_ZCODE_AGENT_CONTEXT_OK);
        ASSERT_EQ(vcs_zcode_agent_context_validate(&context, 374),
                  VCS_ZCODE_AGENT_CONTEXT_LIMIT);
        ASSERT_EQ(vcs_zcode_agent_context_root(&context, 4096, root),
                  VCS_ZCODE_AGENT_CONTEXT_OK);
        zcl_hex_encode(root, 32, root_hex);
        ASSERT_STR_EQ(root_hex,
            "261759f65d9bc99924d7f45375f73b598d8f695a740e28f7b7b9135623e0c1ed");
        uint8_t *wire = NULL; size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_agent_context_serialize(
                      &context, 4096, &wire, &wire_len),
                  VCS_ZCODE_AGENT_CONTEXT_OK);
        ASSERT_EQ(wire_len, 375);
        struct vcs_zcode_agent_context_v1 parsed;
        ASSERT_EQ(vcs_zcode_agent_context_parse(
                      wire, wire_len, 4096, &parsed),
                  VCS_ZCODE_AGENT_CONTEXT_OK);
        uint8_t parsed_root[32];
        ASSERT_EQ(vcs_zcode_agent_context_root(&parsed, 4096, parsed_root),
                  VCS_ZCODE_AGENT_CONTEXT_OK);
        ASSERT(memcmp(root, parsed_root, 32) == 0);
        ASSERT_STR_EQ(parsed.files[0].path, "include/widget.h");
        ASSERT_STR_EQ(parsed.files[1].path, "src/widget.c");
        vcs_zcode_agent_context_free(&parsed);

        wire[152] ^= 1u;
        ASSERT_EQ(vcs_zcode_agent_context_parse(
                      wire, wire_len, 4096, &parsed),
                  VCS_ZCODE_AGENT_CONTEXT_ROOT);
        wire[152] ^= 1u;
        wire[wire_len - 1u] ^= 1u;
        ASSERT_EQ(vcs_zcode_agent_context_parse(
                      wire, wire_len, 4096, &parsed),
                  VCS_ZCODE_AGENT_CONTEXT_ROOT);
        wire[wire_len - 1u] ^= 1u;
        uint8_t *trailed = zcl_malloc(wire_len + 1u,
                                      "test.agent_context.trailing");
        ASSERT(trailed != NULL);
        memcpy(trailed, wire, wire_len); trailed[wire_len] = 0;
        ASSERT_EQ(vcs_zcode_agent_context_parse(
                      trailed, wire_len + 1u, 4096, &parsed),
                  VCS_ZCODE_AGENT_CONTEXT_SHAPE);
        free(trailed); free(wire);
        (void)snprintf(context.files[0].path,
                       sizeof(context.files[0].path), "z/widget.h");
        ASSERT_EQ(vcs_zcode_agent_context_validate(&context, 4096),
                  VCS_ZCODE_AGENT_CONTEXT_SHAPE);
        vcs_zcode_agent_context_free(&context);
        PASS();
    } _test_next:;
    return failures;
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
        policy2.deterministic_fuzz_seeds = VCS_ZCODE_FUZZ_SEEDS_MAX + 1u;
        ASSERT_EQ(vcs_zcode_proof_policy_validate(&policy2),
                  VCS_ZCODE_DEV_ERR_POLICY);
        policy2 = policy;
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
        uint8_t proof_roots[2][32], parsed_roots[2][32], proof_set_root[32];
        zd_root(proof_roots[0], 21); zd_root(proof_roots[1], 22);
        uint8_t proof_set_wire[VCS_ZCODE_PROOF_SET_WIRE_MAX];
        size_t proof_set_len = 0, parsed_count = 0;
        ASSERT_EQ(vcs_zcode_proof_set_serialize(
            (const uint8_t (*)[32])proof_roots, 2, proof_set_wire,
            sizeof(proof_set_wire), &proof_set_len), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_set_parse(
            proof_set_wire, proof_set_len, parsed_roots, 2, &parsed_count),
            VCS_ZCODE_DEV_OK);
        ASSERT_EQ(parsed_count, 2);
        ASSERT_EQ(vcs_zcode_proof_set_root(
            (const uint8_t (*)[32])proof_roots, 2, proof_set_root),
            VCS_ZCODE_DEV_OK);
        uint8_t swap[32]; memcpy(swap, proof_roots[0], 32);
        memcpy(proof_roots[0], proof_roots[1], 32);
        memcpy(proof_roots[1], swap, 32);
        ASSERT_EQ(vcs_zcode_proof_set_serialize(
            (const uint8_t (*)[32])proof_roots, 2, proof_set_wire,
            sizeof(proof_set_wire), &proof_set_len),
            VCS_ZCODE_DEV_ERR_POLICY);
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

static int test_zd_lane_receipt(void)
{
    int failures = 0;
    TEST("zcode_dev: signed lane receipts are canonical chained CAS objects") {
        struct vcs_zcode_proof_policy_v1 policy;
        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate;
        uint8_t policy_root[32], task_root[32], candidate_root[32];
        zd_policy(&policy);
        ASSERT_EQ(vcs_zcode_proof_policy_root(&policy, policy_root),
                  VCS_ZCODE_DEV_OK);
        zd_task(&task, policy_root);
        ASSERT_EQ(vcs_zcode_task_root(&task, task_root), VCS_ZCODE_DEV_OK);
        zd_candidate(&candidate, &task, task_root);
        ASSERT_EQ(vcs_zcode_candidate_root(&candidate, candidate_root),
                  VCS_ZCODE_DEV_OK);
        uint8_t seed[32], signer_secret[32], signer_pubkey[32];
        zd_root(seed, 0x42);
        ed25519_keypair(signer_pubkey, signer_secret, seed);
        struct vcs_zcode_lane_receipt_v1 frontier = {
            .schema_version = VCS_ZCODE_DEV_VERSION,
            .lane = VCS_ZCODE_LANE_FRONTIER,
            .created_unix = 1500,
        };
        memcpy(frontier.source_root, candidate.candidate_source_root, 32);
        memcpy(frontier.task_root, task_root, 32);
        memcpy(frontier.candidate_root, candidate_root, 32);
        memcpy(frontier.proof_policy_root, policy_root, 32);
        ASSERT_EQ(vcs_zcode_lane_receipt_seal(
                      &frontier, signer_secret, signer_pubkey),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_lane_receipt_verify(&frontier, signer_pubkey),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_lane_receipt_validate_for_candidate(
                      &frontier, &task, &candidate, &policy),
                  VCS_ZCODE_DEV_OK);
        uint8_t wire[VCS_ZCODE_LANE_WIRE_BYTES], frontier_root[32];
        struct vcs_zcode_lane_receipt_v1 parsed;
        ASSERT_EQ(vcs_zcode_lane_receipt_serialize(&frontier, wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(wire, "ZCLANE\r\n", 8) == 0);
        ASSERT_EQ(vcs_zcode_lane_receipt_parse(wire, sizeof(wire), &parsed),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_lane_receipt_id(&frontier, frontier_root),
                  VCS_ZCODE_DEV_OK);
        char frontier_hex[65];
        zcl_hex_encode(frontier_root, 32, frontier_hex);
        ASSERT_STR_EQ(frontier_hex,
            "f2b5a2d11457039dbda6a82c756b38ae80b60e84ba82aaf4e35bbe6fa181e4c2");
        uint8_t parsed_root[32];
        ASSERT_EQ(vcs_zcode_lane_receipt_id(&parsed, parsed_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT(memcmp(frontier_root, parsed_root, 32) == 0);

        struct vcs_zcode_lane_receipt_v1 promoted = frontier;
        promoted.lane = VCS_ZCODE_LANE_CANDIDATE;
        promoted.created_unix = 1600;
        zd_root(promoted.proof_set_root, 0x16);
        memcpy(promoted.prior_receipt_root, frontier_root, 32);
        memset(promoted.signature, 0, sizeof(promoted.signature));
        ASSERT_EQ(vcs_zcode_lane_receipt_seal(
                      &promoted, signer_secret, signer_pubkey),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_lane_receipt_validate_for_candidate(
                      &promoted, &task, &candidate, &policy),
                  VCS_ZCODE_DEV_OK);
        promoted.prior_receipt_root[0] ^= 1;
        ASSERT_EQ(vcs_zcode_lane_receipt_verify(&promoted, signer_pubkey),
                  VCS_ZCODE_DEV_ERR_SIGNATURE);
        promoted.prior_receipt_root[0] ^= 1;
        wire[11] = 1;
        ASSERT_EQ(vcs_zcode_lane_receipt_parse(wire, sizeof(wire), &parsed),
                  VCS_ZCODE_DEV_ERR_WIRE_MAGIC);
        ASSERT_EQ(vcs_zcode_lane_receipt_parse(wire, sizeof(wire) - 1,
                                               &parsed),
                  VCS_ZCODE_DEV_ERR_WIRE_SIZE);
        memset(signer_secret, 0, sizeof(signer_secret));
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

static int test_zd_work_context(void)
{
    int failures = 0;
    TEST("zcode_dev: content.v2 context reconstructs the exact action") {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "zcode_dev", "context");
        struct vcs_package_store *store = vcs_package_store_open(
            dir, UINT64_C(256) * 1024u * 1024u);
        ASSERT(store != NULL);
        struct vcs_zcode_work_context_v1 context;
        vcs_zcode_work_context_init(&context);
        zd_policy(&context.proof_policy);
        uint8_t policy_root[32], task_root[32];
        ASSERT_EQ(vcs_zcode_proof_policy_root(&context.proof_policy,
                                              policy_root),
                  VCS_ZCODE_DEV_OK);
        zd_task(&context.task, policy_root);
        ASSERT_EQ(vcs_zcode_task_root(&context.task, task_root),
                  VCS_ZCODE_DEV_OK);
        zd_candidate(&context.candidate, &context.task, task_root);
        zd_root(context.source_sha256, 90);
        (void)snprintf(context.profile, sizeof(context.profile), "dev");
        context.fixed_input_len = VCS_PACKAGE_CHUNK_BYTES + 73u;
        context.fixed_input = malloc(context.fixed_input_len);
        ASSERT(context.fixed_input != NULL);
        for (size_t i = 0; i < context.fixed_input_len; i++)
            context.fixed_input[i] = (uint8_t)(i * 31u + 7u);
        uint8_t package_root[32], action_root[32], direct_action[32];
        uint8_t input_root[32];
        ASSERT_EQ(vcs_zcode_work_context_action_root(
                      &context, 1500, direct_action, input_root),
                  VCS_ZCODE_WORK_CONTEXT_OK);
        ASSERT_EQ(vcs_zcode_work_context_put(
                      store, &context, 1500, package_root, action_root),
                  VCS_ZCODE_WORK_CONTEXT_OK);
        ASSERT(memcmp(action_root, direct_action, 32) == 0);
        struct vcs_package_store_status status;
        ASSERT(vcs_package_store_package_status(store, package_root,
                                                 &status));
        ASSERT(status.complete);
        ASSERT_EQ(status.total_chunks, 2);
        struct vcs_zcode_work_context_v1 loaded;
        ASSERT_EQ(vcs_zcode_work_context_get(store, package_root, 1500,
                                              &loaded),
                  VCS_ZCODE_WORK_CONTEXT_OK);
        uint8_t loaded_action[32], loaded_input[32];
        ASSERT_EQ(vcs_zcode_work_context_action_root(
                      &loaded, 1500, loaded_action, loaded_input),
                  VCS_ZCODE_WORK_CONTEXT_OK);
        ASSERT(memcmp(loaded_action, action_root, 32) == 0);
        ASSERT(memcmp(loaded_input, input_root, 32) == 0);
        ASSERT_EQ(loaded.fixed_input_len, context.fixed_input_len);
        ASSERT(memcmp(loaded.fixed_input, context.fixed_input,
                      context.fixed_input_len) == 0);
        uint8_t test_action[32], test_input[32];
        ASSERT_EQ(vcs_zcode_work_context_action_root_for_kind(
                      &loaded, VCS_BUILD_ACTION_KIND_TEST_V1, 1500,
                      test_action, test_input),
                  VCS_ZCODE_WORK_CONTEXT_OK);
        ASSERT(memcmp(test_action, loaded_action, 32) != 0);
        ASSERT(memcmp(test_input, loaded_input, 32) == 0);
        ASSERT_EQ(vcs_zcode_work_context_action_root_for_kind(
                      &loaded, "c23.shell.v1", 1500,
                      test_action, test_input),
                  VCS_ZCODE_WORK_CONTEXT_ACTION);
        uint8_t *wire = NULL; size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_work_context_serialize(
                      &context, 1500, &wire, &wire_len),
                  VCS_ZCODE_WORK_CONTEXT_OK);
        wire[12] = 1;
        struct vcs_zcode_work_context_v1 rejected;
        ASSERT_EQ(vcs_zcode_work_context_parse(wire, wire_len, 1500,
                                                &rejected),
                  VCS_ZCODE_WORK_CONTEXT_SHAPE);
        free(wire);
        vcs_zcode_work_context_free(&loaded);
        vcs_zcode_work_context_free(&context);
        vcs_package_store_close(store);
        test_rm_rf(dir);
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

static bool zd_kind_action(
    struct node_db *ndb, const struct db_build_job *job,
    const struct db_build_action *base, const char *kind, int64_t sequence,
    const uint8_t input_root[32], struct db_build_action *out)
{
    memset(out, 0, sizeof(*out));
    out->sequence = sequence;
    (void)snprintf(out->kind, sizeof(out->kind), "%s", kind);
    (void)snprintf(out->state, sizeof(out->state), "SNAPSHOTTED");
    zcl_hex_encode(input_root, 32, out->input_root_sha3);
    (void)snprintf(out->task_root_sha3, sizeof(out->task_root_sha3), "%s",
                   base->task_root_sha3);
    (void)snprintf(out->candidate_root_sha3,
                   sizeof(out->candidate_root_sha3), "%s",
                   base->candidate_root_sha3);
    (void)snprintf(out->proof_policy_root_sha3,
                   sizeof(out->proof_policy_root_sha3), "%s",
                   base->proof_policy_root_sha3);
    (void)snprintf(out->target, sizeof(out->target), "%s",
                   VCS_BUILD_TARGET_V1);
    uint8_t flags[32], environment[32];
    if (!vcs_build_action_v1_fixed_flags_root_for_kind(kind, flags) ||
        !vcs_build_action_v1_fixed_environment_root_for_kind(
            kind, environment))
        return false;
    zcl_hex_encode(flags, 32, out->flags_sha3);
    zcl_hex_encode(environment, 32, out->environment_sha3);
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_TEST_V1) == 0) {
        (void)snprintf(out->virtual_workdir, sizeof(out->virtual_workdir),
                       "%s", VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1);
        (void)snprintf(out->declared_outputs,
                       sizeof(out->declared_outputs), "%s",
                       VCS_BUILD_TEST_OUTPUT_V1);
        (void)snprintf(out->resource_policy,
                       sizeof(out->resource_policy), "%s",
                       VCS_BUILD_TEST_RESOURCE_POLICY_V1);
    } else if (strcmp(kind, VCS_BUILD_ACTION_KIND_FUZZ_V1) == 0) {
        (void)snprintf(out->virtual_workdir, sizeof(out->virtual_workdir),
                       "%s", VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1);
        (void)snprintf(out->declared_outputs,
                       sizeof(out->declared_outputs), "%s",
                       VCS_BUILD_FUZZ_OUTPUT_V1);
        (void)snprintf(out->resource_policy,
                       sizeof(out->resource_policy), "%s",
                       VCS_BUILD_FUZZ_RESOURCE_POLICY_V1);
    } else if (strcmp(kind, VCS_BUILD_ACTION_KIND_REVIEW_V1) == 0) {
        (void)snprintf(out->virtual_workdir, sizeof(out->virtual_workdir),
                       "%s", VCS_BUILD_REVIEW_VIRTUAL_ROOT_V1);
        (void)snprintf(out->declared_outputs,
                       sizeof(out->declared_outputs), "%s",
                       VCS_BUILD_REVIEW_OUTPUT_V1);
        (void)snprintf(out->resource_policy,
                       sizeof(out->resource_policy), "%s",
                       VCS_BUILD_REVIEW_RESOURCE_POLICY_V1);
    } else {
        return false;
    }
    out->created_at = out->updated_at = base->created_at;
    struct db_build_job kind_job = *job;
    kind_job.job_id[0] = '\0';
    (void)snprintf(kind_job.state, sizeof(kind_job.state), "PLANNED");
    kind_job.outcome[0] = '\0';
    if (!build_fabric_action_id(&kind_job, out, out->action_id).ok ||
        !build_fabric_job_id(
            &kind_job, out->action_id, kind_job.job_id).ok)
        return false;
    (void)snprintf(out->job_id, sizeof(out->job_id), "%s", kind_job.job_id);
    return db_build_job_save(ndb, &kind_job) && db_build_action_save(ndb, out);
}

static bool zd_observe_kind(
    struct node_db *ndb, const char *workspace,
    const struct vcs_zcode_task_v1 *task,
    const struct db_build_action *action, uint8_t work_kind,
    const uint8_t output_root[32], uint8_t signer_value, int64_t now,
    char observed_id[65])
{
    struct vcs_zcode_work_request_v1 request = {
        .request_id = UINT64_C(10000) + signer_value,
        .work_kind = work_kind,
        .target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3,
        .max_cpu_seconds = 60,
        .max_memory_bytes = UINT64_C(512) * 1024u * 1024u,
        .max_output_bytes = UINT64_C(64) * 1024u * 1024u,
        .deadline_unix = task->expires_unix - 1,
    };
    if (!zcl_hex_decode_lower(action->task_root_sha3,
                              request.task_root, 32) ||
        !zcl_hex_decode_lower(action->candidate_root_sha3,
                              request.candidate_root, 32) ||
        !zcl_hex_decode_lower(action->action_id,
                              request.action_root, 32) ||
        !zcl_hex_decode_lower(action->input_root_sha3,
                              request.input_root, 32) ||
        !zcl_hex_decode_lower(action->proof_policy_root_sha3,
                              request.proof_policy_root, 32))
        return false;
    zd_root(request.context_root, (uint8_t)(signer_value + 1u));
    memcpy(request.toolchain_capsule_root,
           task->toolchain_capsule_root, 32);
    uint8_t requester_seed[32], requester_secret[32], requester_key[32];
    zd_root(requester_seed, (uint8_t)(signer_value + 2u));
    ed25519_keypair(requester_key, requester_secret, requester_seed);
    if (!vcs_zcode_work_request_seal(
            &request, requester_secret, requester_key))
        return false;
    struct vcs_zcode_work_result_v1 result;
    zd_swarm_result(&result, &request, 1, signer_value);
    memcpy(result.output_root, output_root, 32);
    memcpy(result.receipt.output_root, output_root, 32);
    result.receipt.started_unix = now > 0 ? now - 1 : 0;
    result.receipt.finished_unix = now;
    uint8_t seed[32], secret[32], public_key[32];
    zd_root(seed, signer_value);
    ed25519_keypair(public_key, secret, seed);
    if (vcs_zcode_work_receipt_seal(
            &result.receipt, secret, public_key) != VCS_ZCODE_DEV_OK ||
        !build_fabric_receipt_observe_remote(
            ndb, workspace, &request, &result, now, observed_id).ok)
        return false;
    struct db_build_receipt observed;
    struct db_build_worker worker;
    return db_build_receipt_find(ndb, observed_id, &observed) &&
           db_build_worker_find(ndb, observed.worker_id, &worker) &&
           build_fabric_worker_approve(ndb, &worker, now).ok;
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
        ASSERT(vcs_zcode_work_node_peek_request(
            worker, &peer_out, &received));
        ASSERT_EQ(received.request_id, 700);
        uint64_t inbound_peers[2];
        struct vcs_zcode_work_request_v1 inbound_requests[2];
        ASSERT_EQ(vcs_zcode_work_node_inbound_requests(
            worker, inbound_peers, inbound_requests, 2), 1);
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
        bool was_cancelled = false;
        ASSERT(vcs_zcode_work_node_inbound_request(
            worker, 22, 701, &received, &was_cancelled));
        ASSERT(was_cancelled);
        request.request_id = 702;
        ASSERT(vcs_zcode_work_request_seal(
            &request, requester_secret, requester_key));
        ASSERT_EQ(vcs_zcode_work_node_submit(requester, 11, &request, 1000),
                  VCS_ZCODE_WORK_NODE_OK);
        vcs_zcode_work_node_tick(requester, 1100);
        ASSERT(!vcs_zcode_work_node_next_outbound(
            requester, 11, &peer_out, frame, &frame_len));
        request.deadline_unix = 1150;
        ASSERT(vcs_zcode_work_request_seal(
            &request, requester_secret, requester_key));
        ASSERT_EQ(vcs_zcode_work_node_submit(requester, 11, &request, 1100),
                  VCS_ZCODE_WORK_NODE_OK);
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
        char dir[256], preprocessed[320], source_dir[320], source_path[384];
        char workspace[4096];
        test_make_tmpdir(dir, sizeof(dir), "zcode_dev", "improve");
        ASSERT(realpath(dir, workspace) != NULL);
        (void)snprintf(source_dir, sizeof(source_dir), "%s/src", workspace);
        ASSERT(mkdir(source_dir, 0700) == 0);
        (void)snprintf(source_path, sizeof(source_path), "%s/widget.c",
                       source_dir);
        FILE *source_file = fopen(source_path, "wb");
        ASSERT(source_file != NULL);
        static const char indexed_source[] =
            "int context_widget(int x) { return x + 1; }\n";
        ASSERT(fwrite(indexed_source, 1, sizeof(indexed_source) - 1u,
                      source_file) == sizeof(indexed_source) - 1u);
        ASSERT(fclose(source_file) == 0);
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
        struct ci_merkle *source_tree = ci_merkle_build_cold(workspace, NULL);
        struct ci_merkle_node source_tree_root;
        ASSERT(source_tree != NULL);
        ASSERT(ci_merkle_root(source_tree, &source_tree_root));
        ci_merkle_hex(&source_tree_root.digest, roots[0]);
        ci_merkle_free(source_tree);
        int64_t expires = (int64_t)platform_time_wall_unix() + 3600;

        /* Planning is a model-neutral handoff. It needs no candidate, patch,
         * fixed executable, agent process, or ZBuild database mutation. */
        struct json_value plan_input;
        json_init(&plan_input); json_set_object(&plan_input);
        (void)json_push_kv_str(&plan_input, "mode", "plan");
        (void)json_push_kv_str(&plan_input, "workspace", workspace);
        (void)json_push_kv_str(&plan_input, "datadir", workspace);
        (void)json_push_kv_str(&plan_input, "source_root", roots[0]);
        (void)json_push_kv_str(&plan_input, "dependency_lock_root", roots[1]);
        (void)json_push_kv_str(&plan_input, "write_scope_root", roots[2]);
        (void)json_push_kv_str(&plan_input, "acceptance_tests_root", roots[3]);
        (void)json_push_kv_str(&plan_input, "model_policy_root", roots[4]);
        (void)json_push_kv_str(&plan_input, "goal",
                               "fix deterministic fixture");
        (void)json_push_kv_str(&plan_input, "proof_policy_hex", policy_hex);
        (void)json_push_kv_str(&plan_input, "context_symbol",
                               "context_widget");
        (void)json_push_kv_int(&plan_input, "expires_unix", expires);
        struct zcl_command_request plan_request = { .input = &plan_input };
        struct zcl_command_reply plan_reply;
        zcl_command_reply_init(&plan_reply, "zcl.zcode_improve.v1");
        zcl_native_handle_zcode_improve(&plan_request, &plan_reply);
        ASSERT_EQ(plan_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&plan_reply.data, "mode")),
                      "plan");
        ASSERT_STR_EQ(json_get_str(json_get(&plan_reply.data, "state")),
                      "AWAITING_CANDIDATE");
        ASSERT_STR_EQ(json_get_str(json_get(&plan_reply.data, "authority")),
                      "TASK_AND_CONTEXT_ROOTS");
        const char *planned_task = json_get_str(json_get(
            &plan_reply.data, "task_root"));
        const char *planned_context = json_get_str(json_get(
            &plan_reply.data, "agent_context_root"));
        ASSERT(planned_task && strlen(planned_task) == 64);
        ASSERT(planned_context && strlen(planned_context) == 64);
        ASSERT(json_get(&plan_reply.data, "candidate_root") == NULL);
        ASSERT(json_get(&plan_reply.data, "action_id") == NULL);
        char plan_db[320];
        (void)snprintf(plan_db, sizeof(plan_db), "%s/node.db", workspace);
        ASSERT(access(plan_db, F_OK) != 0);

        struct json_value input;
        json_init(&input); json_set_object(&input);
        (void)json_push_kv_str(&input, "mode", "admit");
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
        (void)json_push_kv_str(&input, "goal", "fix deterministic fixture");
        (void)json_push_kv_str(&input, "proof_policy_hex", policy_hex);
        (void)json_push_kv_str(&input, "fixed_input_path", preprocessed);
        (void)json_push_kv_str(&input, "context_symbol", "context_widget");
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
        const char *agent_context_hex = json_get_str(json_get(
            &reply.data, "agent_context_root"));
        ASSERT(task_hex && candidate_hex && action_id && agent_context_hex);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "mode")), "admit");
        ASSERT_STR_EQ(task_hex, planned_task);
        ASSERT_STR_EQ(agent_context_hex, planned_context);
        zcl_command_reply_free(&plan_reply);
        json_free(&plan_input);
        ASSERT(strlen(agent_context_hex) == 64);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &reply.data, "agent_context_source_tree_root")),
                      roots[0]);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &reply.data, "agent_context_symbol")),
                      "context_widget");
        ASSERT(json_get_int(json_get(
                   &reply.data, "agent_context_files")) >= 1);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "state")), "QUEUED");
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "lane")), "FRONTIER");
        const char *frontier_receipt = json_get_str(json_get(
            &reply.data, "lane_receipt_root"));
        ASSERT(frontier_receipt && strlen(frontier_receipt) == 64);
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
        uint8_t agent_context_root[32], *agent_context_wire = NULL;
        size_t agent_context_wire_len = 0;
        ASSERT(zcl_hex_decode_lower(agent_context_hex,
                                    agent_context_root, 32));
        ASSERT_EQ(vcs_object_load_raw(
                      workspace, agent_context_root, &agent_context_wire,
                      &agent_context_wire_len), 0);
        struct vcs_zcode_agent_context_v1 agent_context;
        ASSERT_EQ(vcs_zcode_agent_context_parse(
                      agent_context_wire, agent_context_wire_len,
                      (size_t)task.max_context_bytes, &agent_context),
                  VCS_ZCODE_AGENT_CONTEXT_OK);
        free(agent_context_wire);
        ASSERT(memcmp(agent_context.task_root, task_root, 32) == 0);
        ASSERT(memcmp(agent_context.source_root, task.source_root, 32) == 0);
        ASSERT_STR_EQ(agent_context.query, "context_widget");
        ASSERT_STR_EQ(agent_context.files[0].path, "src/widget.c");
        ASSERT_EQ(agent_context.files[0].content_len,
                  sizeof(indexed_source) - 1u);
        ASSERT(memcmp(agent_context.files[0].content, indexed_source,
                      sizeof(indexed_source) - 1u) == 0);
        uint8_t agent_context_check[32];
        ASSERT_EQ(vcs_zcode_agent_context_root(
                      &agent_context, (size_t)task.max_context_bytes,
                      agent_context_check), VCS_ZCODE_AGENT_CONTEXT_OK);
        ASSERT(memcmp(agent_context_root, agent_context_check, 32) == 0);
        vcs_zcode_agent_context_free(&agent_context);

        /* Context capture is source-authoritative and refuses a task whose
         * claimed source identity is stale before creating a ZBuild job. */
        json_set_str((struct json_value *)json_get(&input, "source_root"),
                     roots[1]);
        struct zcl_command_reply stale_reply;
        zcl_command_reply_init(&stale_reply, "zcl.zcode_improve.v1");
        zcl_native_handle_zcode_improve(&request, &stale_reply);
        ASSERT_EQ(stale_reply.exit_code, ZCL_COMMAND_EXIT_INVALID);
        ASSERT_STR_EQ(stale_reply.error.code, "AGENT_CONTEXT_FAILED");
        zcl_command_reply_free(&stale_reply);
        json_set_str((struct json_value *)json_get(&input, "source_root"),
                     roots[0]);
        json_set_str((struct json_value *)json_get(&input, "mode"), "invalid");
        struct zcl_command_reply mode_reply;
        zcl_command_reply_init(&mode_reply, "zcl.zcode_improve.v1");
        zcl_native_handle_zcode_improve(&request, &mode_reply);
        ASSERT_EQ(mode_reply.exit_code, ZCL_COMMAND_EXIT_INVALID);
        ASSERT_STR_EQ(mode_reply.error.code, "BAD_MODE");
        zcl_command_reply_free(&mode_reply);
        json_set_str((struct json_value *)json_get(&input, "mode"), "admit");
        char db_path[320];
        (void)snprintf(db_path, sizeof(db_path), "%s/node.db", workspace);
        struct node_db ndb = {0};
        ASSERT(node_db_open(&ndb, db_path));
        struct zcode_lane_status frontier_status;
        ASSERT(zcode_lane_find(&ndb, workspace, roots[6],
                               &frontier_status).ok);
        ASSERT_EQ(frontier_status.lane, VCS_ZCODE_LANE_FRONTIER);
        ASSERT_STR_EQ(frontier_status.receipt_root_sha3, frontier_receipt);
        struct db_build_action action;
        ASSERT(db_build_action_find(&ndb, action_id, &action));
        ASSERT_STR_EQ(action.state, "QUEUED");
        ASSERT_STR_EQ(action.task_root_sha3, task_hex);
        ASSERT_STR_EQ(action.candidate_root_sha3, candidate_hex);
        ASSERT_STR_EQ(action.context_root_sha3, "");
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
        uint8_t compile_output_root[32];
        memcpy(compile_output_root, work_receipt.output_root, 32);

        /* Reuse the exact candidate timestamp to queue a second fixed proof
         * action in a distinct job. Evidence aggregation is candidate-bound,
         * not accidentally job-bound. */
        int64_t candidate_created = json_get_int(json_get(
            &reply.data, "candidate_created_unix"));
        json_set_str((struct json_value *)json_get(
                         &input, "fixed_input_path"), "/usr/bin/true");
        (void)json_push_kv_str(&input, "action_kind",
                               VCS_BUILD_ACTION_KIND_TEST_V1);
        (void)json_push_kv_int(&input, "candidate_created_unix",
                               candidate_created);
        struct zcl_command_reply test_reply;
        zcl_command_reply_init(&test_reply, "zcl.zcode_improve.v1");
        zcl_native_handle_zcode_improve(&request, &test_reply);
        ASSERT_EQ(test_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&test_reply.data, "task_root")),
                      task_hex);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &test_reply.data, "agent_context_root")),
                      agent_context_hex);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &test_reply.data, "candidate_root")), candidate_hex);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &test_reply.data, "action_kind")),
                      VCS_BUILD_ACTION_KIND_TEST_V1);
        const char *test_action_id = json_get_str(json_get(
            &test_reply.data, "action_id"));
        ASSERT(test_action_id && strcmp(test_action_id, action_id) != 0);
        char test_action_id_saved[65];
        (void)snprintf(test_action_id_saved, sizeof(test_action_id_saved),
                       "%s", test_action_id);
        struct db_build_action local_test_action;
        ASSERT(db_build_action_find(&ndb, test_action_id,
                                    &local_test_action));
        uint8_t test_lease_root[32]; char test_lease_hex[65];
        zd_root(test_lease_root, 61);
        zcl_hex_encode(test_lease_root, 32, test_lease_hex);
        claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, test_lease_hex,
                                  now, 300, &local_test_action, &claimed).ok);
        ASSERT(claimed);
        struct db_build_receipt local_test_receipt;
        ASSERT(build_fabric_worker_execute(
            &ndb, workspace, test_action_id, test_lease_hex, worker_secret,
            worker_key, &local_test_receipt).ok);
        uint8_t local_test_receipt_root[32];
        ASSERT(zcl_hex_decode_lower(local_test_receipt.work_receipt_sha3,
                                    local_test_receipt_root, 32));
        ASSERT_EQ(vcs_object_load_raw(
            workspace, local_test_receipt_root, &receipt_wire,
            &receipt_wire_len), 0);
        ASSERT_EQ(vcs_zcode_work_receipt_parse(
            receipt_wire, receipt_wire_len, &work_receipt),
            VCS_ZCODE_DEV_OK);
        free(receipt_wire); receipt_wire = NULL;
        ASSERT_EQ(work_receipt.work_kind, VCS_ZCODE_WORK_TEST);
        ASSERT_EQ(work_receipt.status, VCS_ZCODE_WORK_PASS);
        zcl_command_reply_free(&test_reply);

        /* The same immutable candidate also executes the policy's exact
         * deterministic seed range in the fixed fuzz sandbox. */
        json_set_str((struct json_value *)json_get(&input, "action_kind"),
                     VCS_BUILD_ACTION_KIND_FUZZ_V1);
        struct zcl_command_reply fuzz_reply;
        zcl_command_reply_init(&fuzz_reply, "zcl.zcode_improve.v1");
        zcl_native_handle_zcode_improve(&request, &fuzz_reply);
        ASSERT_EQ(fuzz_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&fuzz_reply.data, "task_root")),
                      task_hex);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &fuzz_reply.data, "candidate_root")), candidate_hex);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &fuzz_reply.data, "action_kind")),
                      VCS_BUILD_ACTION_KIND_FUZZ_V1);
        const char *local_fuzz_action_id = json_get_str(json_get(
            &fuzz_reply.data, "action_id"));
        ASSERT(local_fuzz_action_id &&
               strcmp(local_fuzz_action_id, action_id) != 0 &&
               strcmp(local_fuzz_action_id, test_action_id_saved) != 0);
        char local_fuzz_action_id_saved[65];
        (void)snprintf(local_fuzz_action_id_saved,
                       sizeof(local_fuzz_action_id_saved), "%s",
                       local_fuzz_action_id);
        struct db_build_action local_fuzz_action;
        ASSERT(db_build_action_find(&ndb, local_fuzz_action_id,
                                    &local_fuzz_action));
        uint8_t fuzz_lease_root[32]; char fuzz_lease_hex[65];
        zd_root(fuzz_lease_root, 62);
        zcl_hex_encode(fuzz_lease_root, 32, fuzz_lease_hex);
        claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, fuzz_lease_hex,
                                  now, 300, &local_fuzz_action, &claimed).ok);
        ASSERT(claimed);
        struct db_build_receipt local_fuzz_receipt;
        ASSERT(build_fabric_worker_execute(
            &ndb, workspace, local_fuzz_action_id, fuzz_lease_hex,
            worker_secret, worker_key, &local_fuzz_receipt).ok);
        uint8_t local_fuzz_receipt_root[32];
        ASSERT(zcl_hex_decode_lower(local_fuzz_receipt.work_receipt_sha3,
                                    local_fuzz_receipt_root, 32));
        ASSERT_EQ(vcs_object_load_raw(
            workspace, local_fuzz_receipt_root, &receipt_wire,
            &receipt_wire_len), 0);
        ASSERT_EQ(vcs_zcode_work_receipt_parse(
            receipt_wire, receipt_wire_len, &work_receipt),
            VCS_ZCODE_DEV_OK);
        free(receipt_wire); receipt_wire = NULL;
        ASSERT_EQ(work_receipt.work_kind, VCS_ZCODE_WORK_FUZZ);
        ASSERT_EQ(work_receipt.status, VCS_ZCODE_WORK_PASS);
        zcl_command_reply_free(&fuzz_reply);

        /* A target-found defect is durable FAIL evidence, not a sandbox
         * transport error and never a false passing proof. */
        json_set_str((struct json_value *)json_get(
                         &input, "fixed_input_path"), "/usr/bin/false");
        struct zcl_command_reply fuzz_fail_reply;
        zcl_command_reply_init(&fuzz_fail_reply, "zcl.zcode_improve.v1");
        zcl_native_handle_zcode_improve(&request, &fuzz_fail_reply);
        ASSERT_EQ(fuzz_fail_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        const char *fuzz_fail_action_id = json_get_str(json_get(
            &fuzz_fail_reply.data, "action_id"));
        ASSERT(fuzz_fail_action_id &&
               strcmp(fuzz_fail_action_id,
                      local_fuzz_action_id_saved) != 0);
        struct db_build_action fuzz_fail_action;
        ASSERT(db_build_action_find(&ndb, fuzz_fail_action_id,
                                    &fuzz_fail_action));
        uint8_t fail_lease_root[32]; char fail_lease_hex[65];
        zd_root(fail_lease_root, 63);
        zcl_hex_encode(fail_lease_root, 32, fail_lease_hex);
        claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, fail_lease_hex,
                                  now, 300, &fuzz_fail_action, &claimed).ok);
        ASSERT(claimed);
        struct db_build_receipt fuzz_fail_receipt;
        ASSERT(build_fabric_worker_execute(
            &ndb, workspace, fuzz_fail_action_id, fail_lease_hex,
            worker_secret, worker_key, &fuzz_fail_receipt).ok);
        ASSERT_EQ(fuzz_fail_receipt.exit_status, 1);
        ASSERT(db_build_action_find(&ndb, fuzz_fail_action_id,
                                    &fuzz_fail_action));
        ASSERT_STR_EQ(fuzz_fail_action.state, "FAILED");
        ASSERT(zcl_hex_decode_lower(fuzz_fail_receipt.work_receipt_sha3,
                                    local_fuzz_receipt_root, 32));
        ASSERT_EQ(vcs_object_load_raw(
            workspace, local_fuzz_receipt_root, &receipt_wire,
            &receipt_wire_len), 0);
        ASSERT_EQ(vcs_zcode_work_receipt_parse(
            receipt_wire, receipt_wire_len, &work_receipt),
            VCS_ZCODE_DEV_OK);
        free(receipt_wire); receipt_wire = NULL;
        ASSERT_EQ(work_receipt.work_kind, VCS_ZCODE_WORK_FUZZ);
        ASSERT_EQ(work_receipt.status, VCS_ZCODE_WORK_FAIL);
        zcl_command_reply_free(&fuzz_fail_reply);

        struct vcs_zcode_work_request_v1 remote_request = {
            .request_id = 991,
            .work_kind = VCS_ZCODE_WORK_BUILD,
            .target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3,
            .max_cpu_seconds = 60,
            .max_memory_bytes = UINT64_C(512) * 1024u * 1024u,
            .max_output_bytes = UINT64_C(64) * 1024u * 1024u,
            .deadline_unix = expires - 1,
        };
        ASSERT(zcl_hex_decode_lower(task_hex, remote_request.task_root, 32));
        ASSERT(zcl_hex_decode_lower(candidate_hex,
                                    remote_request.candidate_root, 32));
        ASSERT(zcl_hex_decode_lower(action_id,
                                    remote_request.action_root, 32));
        ASSERT(zcl_hex_decode_lower(action.input_root_sha3,
                                    remote_request.input_root, 32));
        zd_root(remote_request.context_root, 92);
        memcpy(remote_request.proof_policy_root, task.proof_policy_root, 32);
        memcpy(remote_request.toolchain_capsule_root,
               task.toolchain_capsule_root, 32);
        uint8_t requester_seed[32], requester_secret[32], requester_key[32];
        zd_root(requester_seed, 93);
        ed25519_keypair(requester_key, requester_secret, requester_seed);
        ASSERT(vcs_zcode_work_request_seal(
            &remote_request, requester_secret, requester_key));
        struct vcs_zcode_work_result_v1 remote_result;
        zd_swarm_result(&remote_result, &remote_request, 94, 95);
        memcpy(remote_result.output_root, compile_output_root, 32);
        memcpy(remote_result.receipt.output_root, compile_output_root, 32);
        remote_result.receipt.started_unix = now > 0 ? now - 1 : 0;
        remote_result.receipt.finished_unix = now;
        uint8_t remote_seed[32], remote_secret[32], remote_key[32];
        zd_root(remote_seed, 95);
        ed25519_keypair(remote_key, remote_secret, remote_seed);
        ASSERT_EQ(vcs_zcode_work_receipt_seal(
            &remote_result.receipt, remote_secret, remote_key),
            VCS_ZCODE_DEV_OK);
        struct vcs_zcode_work_request_v1 wrong_kind_request = remote_request;
        wrong_kind_request.work_kind = VCS_ZCODE_WORK_TEST;
        ASSERT(vcs_zcode_work_request_seal(
            &wrong_kind_request, requester_secret, requester_key));
        struct vcs_zcode_work_result_v1 wrong_kind_result;
        zd_swarm_result(&wrong_kind_result, &wrong_kind_request, 97, 98);
        char refused_id[65];
        ASSERT(!build_fabric_receipt_observe_remote(
            &ndb, workspace, &wrong_kind_request, &wrong_kind_result, now,
            refused_id).ok);
        char observed_id[65];
        ASSERT(build_fabric_receipt_observe_remote(
            &ndb, workspace, &remote_request, &remote_result, now,
            observed_id).ok);
        struct db_build_receipt observed;
        ASSERT(db_build_receipt_find(&ndb, observed_id, &observed));
        ASSERT_STR_EQ(observed.trust_state, "REMOTE_OBSERVED");
        ASSERT_STR_EQ(observed.work_receipt_sha3, observed_id);
        ASSERT(!build_fabric_receipt_accept(&ndb, &observed, now).ok);
        struct db_build_worker remote_worker;
        ASSERT(db_build_worker_find(&ndb, observed.worker_id,
                                    &remote_worker));
        ASSERT(!remote_worker.approved);
        ASSERT(build_fabric_worker_approve(&ndb, &remote_worker, now).ok);
        struct vcs_zcode_work_result_v1 second_remote = remote_result;
        uint8_t second_seed[32], second_secret[32], second_key[32];
        zd_root(second_seed, 96);
        ed25519_keypair(second_key, second_secret, second_seed);
        ASSERT_EQ(vcs_zcode_work_receipt_seal(
            &second_remote.receipt, second_secret, second_key),
            VCS_ZCODE_DEV_OK);
        char second_observed_id[65];
        ASSERT(build_fabric_receipt_observe_remote(
            &ndb, workspace, &remote_request, &second_remote, now,
            second_observed_id).ok);
        struct db_build_receipt second_observed;
        ASSERT(db_build_receipt_find(&ndb, second_observed_id,
                                     &second_observed));
        struct db_build_worker second_worker;
        ASSERT(db_build_worker_find(&ndb, second_observed.worker_id,
                                    &second_worker));
        ASSERT(build_fabric_worker_approve(&ndb, &second_worker, now).ok);
        struct build_fabric_proof_evaluation evaluation;
        int64_t evaluation_now = (int64_t)platform_time_wall_unix();
        ASSERT(build_fabric_proof_evaluate(
            &ndb, workspace, action_id, evaluation_now, &evaluation).ok);
        ASSERT(evaluation.local_reproduced);
        ASSERT(evaluation.quorum_satisfied);
        ASSERT(evaluation.approved_distinct_signers >= 2);
        ASSERT(evaluation.compile_satisfied);
        ASSERT(!evaluation.policy_satisfied);
        ASSERT(strlen(evaluation.proof_set_root_sha3) == 64);
        ASSERT(db_build_receipt_find(&ndb, observed_id, &observed));
        ASSERT_STR_EQ(observed.trust_state, "LOCAL_REPRODUCED");

        struct db_build_job job;
        ASSERT(db_build_job_find(&ndb, action.job_id, &job));
        struct db_build_action test_action, fuzz_action, review_action;
        ASSERT(zd_kind_action(
            &ndb, &job, &action, VCS_BUILD_ACTION_KIND_TEST_V1, 1,
            task.acceptance_tests_root, &test_action));
        uint8_t test_output[32]; zd_root(test_output, 120);
        char test_receipt_a[65], test_receipt_b[65];
        ASSERT(zd_observe_kind(
            &ndb, workspace, &task, &test_action, VCS_ZCODE_WORK_TEST,
            test_output, 121, now, test_receipt_a));
        ASSERT(zd_observe_kind(
            &ndb, workspace, &task, &test_action, VCS_ZCODE_WORK_TEST,
            test_output, 122, now, test_receipt_b));
        ASSERT(zd_kind_action(
            &ndb, &job, &action, VCS_BUILD_ACTION_KIND_FUZZ_V1, 2,
            task.acceptance_tests_root, &fuzz_action));
        uint8_t fuzz_output[32]; zd_root(fuzz_output, 123);
        char fuzz_receipt[65];
        ASSERT(zd_observe_kind(
            &ndb, workspace, &task, &fuzz_action, VCS_ZCODE_WORK_FUZZ,
            fuzz_output, 124, now, fuzz_receipt));
        struct build_fabric_proof_evaluation before_review;
        ASSERT(build_fabric_proof_evaluate(
            &ndb, workspace, action_id, evaluation_now,
            &before_review).ok);
        ASSERT(before_review.test_satisfied);
        ASSERT(before_review.fuzz_satisfied);
        ASSERT(!before_review.review_satisfied);
        ASSERT(!before_review.policy_satisfied);
        uint8_t review_seed[32], review_secret[32], review_key[32];
        zd_root(review_seed, 125);
        ed25519_keypair(review_key, review_secret, review_seed);
        struct vcs_zcode_review_v1 review = {
            .schema_version = VCS_ZCODE_DEV_VERSION,
            .verdict = VCS_ZCODE_REVIEW_APPROVE,
            .sequence = 1,
            .created_unix = now,
        };
        ASSERT(zcl_hex_decode_lower(task_hex, review.task_root, 32));
        ASSERT(zcl_hex_decode_lower(candidate_hex,
                                    review.candidate_root, 32));
        memcpy(review.proof_policy_root, task.proof_policy_root, 32);
        ASSERT(zcl_hex_decode_lower(before_review.proof_set_root_sha3,
                                    review.proof_set_root, 32));
        static const uint8_t findings[] = "reviewed: no findings";
        sha3_256(findings, sizeof(findings) - 1u, review.findings_root);
        ASSERT(vcs_object_put_addressed(
            workspace, review.findings_root, findings,
            sizeof(findings) - 1u));
        memcpy(review.reviewer_pubkey, review_key, 32);
        uint8_t review_wire[VCS_ZCODE_REVIEW_WIRE_BYTES], review_root[32];
        ASSERT_EQ(vcs_zcode_review_serialize(&review, review_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_review_root(&review, review_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT(vcs_object_put_addressed(
            workspace, review_root, review_wire, sizeof(review_wire)));
        ASSERT(zd_kind_action(
            &ndb, &job, &action, VCS_BUILD_ACTION_KIND_REVIEW_V1, 3,
            candidate_root, &review_action));
        char review_receipt[65];
        ASSERT(zd_observe_kind(
            &ndb, workspace, &task, &review_action, VCS_ZCODE_WORK_REVIEW,
            review_root, 125, now, review_receipt));
        struct build_fabric_proof_evaluation complete;
        ASSERT(build_fabric_proof_evaluate(
            &ndb, workspace, action_id, evaluation_now, &complete).ok);
        ASSERT_EQ(complete.compile_receipts, 3);
        ASSERT_EQ(complete.test_receipts, 3);
        ASSERT_EQ(complete.fuzz_receipts, 2);
        ASSERT_EQ(complete.review_receipts, 1);
        ASSERT(complete.compile_satisfied);
        ASSERT(complete.test_satisfied);
        ASSERT(complete.fuzz_satisfied);
        ASSERT(complete.review_satisfied);
        ASSERT(!complete.release_identity_satisfied);
        ASSERT(!complete.policy_satisfied);
        node_db_close(&ndb);

        struct json_value accept_input;
        json_init(&accept_input); json_set_object(&accept_input);
        (void)json_push_kv_str(&accept_input, "workspace", workspace);
        (void)json_push_kv_str(&accept_input, "datadir", workspace);
        (void)json_push_kv_str(&accept_input, "action_id", action_id);
        (void)json_push_kv_str(&accept_input, "lane", "CANDIDATE");
        struct zcl_command_request accept_request = { .input = &accept_input };
        struct zcl_command_reply accept_reply;
        zcl_command_reply_init(&accept_reply, "zcl.zcode_accept.v1");
        zcl_native_handle_zcode_accept(&accept_request, &accept_reply);
        ASSERT_EQ(accept_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&accept_reply.data, "lane")),
                      "CANDIDATE");
        ASSERT(strlen(json_get_str(json_get(
            &accept_reply.data, "lane_receipt_root"))) == 64);
        ASSERT(strlen(json_get_str(json_get(
            &accept_reply.data, "proof_set_root"))) == 64);

        struct json_value lane_input;
        json_init(&lane_input); json_set_object(&lane_input);
        (void)json_push_kv_str(&lane_input, "workspace", workspace);
        (void)json_push_kv_str(&lane_input, "datadir", workspace);
        (void)json_push_kv_str(&lane_input, "source_root", roots[6]);
        struct zcl_command_request lane_request = { .input = &lane_input };
        struct zcl_command_reply lane_reply;
        zcl_command_reply_init(&lane_reply, "zcl.zcode_lane.v1");
        zcl_native_handle_zcode_lane(&lane_request, &lane_reply);
        ASSERT_EQ(lane_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&lane_reply.data, "lane")),
                      "CANDIDATE");
        ASSERT_STR_EQ(json_get_str(json_get(&lane_reply.data, "authority")),
                      "SIGNED_CAS_RECEIPT");

        json_set_str((struct json_value *)json_get(&accept_input, "lane"),
                     "PROVEN");
        struct zcl_command_reply proven_reply;
        zcl_command_reply_init(&proven_reply, "zcl.zcode_accept.v1");
        zcl_native_handle_zcode_accept(&accept_request, &proven_reply);
        ASSERT_EQ(proven_reply.exit_code, ZCL_COMMAND_EXIT_FAILED);
        ASSERT_STR_EQ(proven_reply.error.code, "LANE_PROMOTION_REFUSED");
        zcl_command_reply_free(&proven_reply);
        zcl_command_reply_free(&lane_reply);
        json_free(&lane_input);
        zcl_command_reply_free(&accept_reply);
        json_free(&accept_input);

        struct json_value evidence_input;
        json_init(&evidence_input); json_set_object(&evidence_input);
        (void)json_push_kv_str(&evidence_input, "workspace", workspace);
        (void)json_push_kv_str(&evidence_input, "datadir", workspace);
        (void)json_push_kv_str(&evidence_input, "action_id", action_id);
        struct zcl_command_request evidence_request = {
            .input = &evidence_input,
        };
        struct zcl_command_reply evidence_reply;
        zcl_command_reply_init(&evidence_reply, "zcl.zcode_evidence.v1");
        zcl_native_handle_zcode_evidence(&evidence_request, &evidence_reply);
        ASSERT_EQ(evidence_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&evidence_reply.data, "authority")),
                      "LOCAL_REPRODUCTION");
        ASSERT_EQ(json_get_int(json_get(&evidence_reply.data,
                                        "test_receipts")), 3);
        ASSERT_EQ(json_get_int(json_get(&evidence_reply.data,
                                        "fuzz_receipts")), 2);
        ASSERT_EQ(json_get_int(json_get(&evidence_reply.data,
                                        "review_receipts")), 1);
        ASSERT(json_get_bool(json_get(&evidence_reply.data,
                                      "test_satisfied")));
        ASSERT(json_get_bool(json_get(&evidence_reply.data,
                                      "fuzz_satisfied")));
        ASSERT(json_get_bool(json_get(&evidence_reply.data,
                                      "review_satisfied")));
        ASSERT(!json_get_bool(json_get(&evidence_reply.data,
                                       "release_identity_satisfied")));
        ASSERT(!json_get_bool(json_get(&evidence_reply.data,
                                       "policy_satisfied")));
        zcl_command_reply_free(&evidence_reply);
        json_free(&evidence_input);
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
    failures += test_zd_agent_context();
    failures += test_zd_policy_and_task();
    failures += test_zd_candidate_review();
    failures += test_zd_lane_receipt();
    failures += test_zd_receipt();
    failures += test_zd_work_context();
    failures += test_zd_work_swarm();
    failures += test_zd_work_node();
    failures += test_zd_improve_command();
    printf("=== zcode_dev_objects: %d failures ===\n", failures);
    return failures;
}
