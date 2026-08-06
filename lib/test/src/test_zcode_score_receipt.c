/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: prove canonical evidence-derived ZC23 Score receipts. */

#include "test/test_core.h"

#include "base/hex.h"
#include "command/native_command.h"
#include "crypto/ed25519.h"
#include "json/json.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_score_receipt.h"

#include <stdlib.h>
#include <string.h>

struct score_work_fixture {
    struct vcs_zcode_work_receipt_v1 receipt;
    uint8_t root[32];
};

static void score_fill(uint8_t out[32], uint8_t value)
{
    memset(out, value, 32);
}

static int score_work_compare(const void *left, const void *right)
{
    const struct score_work_fixture *a = left, *b = right;
    return memcmp(a->root, b->root, 32);
}

static void score_policy(struct vcs_zcode_proof_policy_v1 *policy)
{
    memset(policy, 0, sizeof(*policy));
    policy->schema_version = VCS_ZCODE_DEV_VERSION;
    policy->required_proofs = VCS_ZCODE_PROOF_COMPILE |
        VCS_ZCODE_PROOF_TEST | VCS_ZCODE_PROOF_FUZZ |
        VCS_ZCODE_PROOF_REVIEW | VCS_ZCODE_PROOF_LOCAL_REPRODUCTION;
    policy->minimum_compile_receipts = 1;
    policy->minimum_test_receipts = 1;
    policy->minimum_fuzz_receipts = 1;
    policy->minimum_reviews = 1;
    policy->minimum_matching_receipts = 1;
    policy->flags = VCS_ZCODE_POLICY_INDEPENDENT_SIGNERS |
        VCS_ZCODE_POLICY_RELEASE_BYTE_IDENTITY;
    policy->deterministic_fuzz_seeds = 1;
    policy->audit_basis_points = 100;
    policy->maximum_proof_age_seconds = 3600;
}

static bool score_fixture(
    struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_candidate_v1 *candidate,
    struct vcs_zcode_proof_policy_v1 *policy,
    struct vcs_zcode_lane_receipt_v1 *lane,
    struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS],
    uint8_t lane_secret[32], uint8_t lane_pubkey[32])
{
    score_policy(policy);
    uint8_t policy_root[32], task_root[32], candidate_root[32];
    if (vcs_zcode_proof_policy_root(policy, policy_root) != VCS_ZCODE_DEV_OK)
        return false;
    memset(task, 0, sizeof(*task));
    task->schema_version = VCS_ZCODE_DEV_VERSION;
    if (!zcl_hex_decode_lower(
            "6e03d74b8edab650e790424f4dc8274e8ca262cccfc4f94d5b068ad66f60f48e",
            task->source_root, 32) ||
        !zcl_hex_decode_lower(
            "149b3e4e10eaad9fb93626419bae842b1dfceddf64f8a8b1f065c69bb30dc21a",
            task->dependency_lock_root, 32) ||
        !zcl_hex_decode_lower(
            "c0c3ec6514fd2a7ea242e087aff75b33fdc208a219c61855788509efef37b15d",
            task->toolchain_capsule_root, 32))
        return false;
    score_fill(task->write_scope_root, 4);
    score_fill(task->acceptance_tests_root, 5);
    memcpy(task->proof_policy_root, policy_root, 32);
    score_fill(task->model_policy_root, 7);
    score_fill(task->goal_root, 8);
    task->capabilities = VCS_ZCODE_TASK_CAP_V1_MASK;
    task->max_changed_files = 8;
    task->max_patch_bytes = 4096;
    task->max_context_bytes = 4096;
    task->max_cpu_seconds = 60;
    task->max_memory_bytes = 1024 * 1024;
    task->max_output_bytes = 1024 * 1024;
    task->expires_unix = 5000;
    if (vcs_zcode_task_root(task, task_root) != VCS_ZCODE_DEV_OK)
        return false;
    memset(candidate, 0, sizeof(*candidate));
    candidate->schema_version = VCS_ZCODE_DEV_VERSION;
    memcpy(candidate->task_root, task_root, 32);
    memcpy(candidate->base_source_root, task->source_root, 32);
    score_fill(candidate->patch_root, 9);
    memcpy(candidate->candidate_source_root, task->source_root, 32);
    score_fill(candidate->adapter_policy_root, 11);
    score_fill(candidate->author_pubkey, 12);
    candidate->sequence = 1;
    candidate->created_unix = 1000;
    if (vcs_zcode_candidate_root(candidate, candidate_root) !=
            VCS_ZCODE_DEV_OK)
        return false;
    for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
        struct vcs_zcode_work_receipt_v1 *work = &works[i].receipt;
        memset(work, 0, sizeof(*work));
        work->schema_version = VCS_ZCODE_DEV_VERSION;
        memcpy(work->task_root, task_root, 32);
        memcpy(work->candidate_root, candidate_root, 32);
        vcs_zcode_score_action_root((enum vcs_zcode_score_unit)i,
                                    work->action_root);
        score_fill(work->input_root, (uint8_t)(20 + i));
        score_fill(work->output_root, (uint8_t)(30 + i));
        memcpy(work->proof_policy_root, policy_root, 32);
        memcpy(work->toolchain_capsule_root,
               task->toolchain_capsule_root, 32);
        score_fill(work->lease_id, (uint8_t)(40 + i));
        score_fill(work->evidence_root, (uint8_t)(50 + i));
        score_fill(work->confinement_root, (uint8_t)(60 + i));
        static const uint8_t kinds[VCS_ZCODE_SCORE_UNITS] = {
            VCS_ZCODE_WORK_REVIEW, VCS_ZCODE_WORK_TEST,
            VCS_ZCODE_WORK_REPRODUCE, VCS_ZCODE_WORK_BUILD,
            VCS_ZCODE_WORK_BUILD,
        };
        work->work_kind = kinds[i];
        work->status = VCS_ZCODE_WORK_PASS;
        work->started_unix = 1100 + (int64_t)i;
        work->finished_unix = 1200 + (int64_t)i;
        uint8_t seed[32], secret[32], pubkey[32];
        score_fill(seed, (uint8_t)(70 + i));
        ed25519_keypair(pubkey, secret, seed);
        if (vcs_zcode_work_receipt_seal(work, secret, pubkey) !=
                VCS_ZCODE_DEV_OK ||
            vcs_zcode_work_receipt_id(work, works[i].root) !=
                VCS_ZCODE_DEV_OK)
            return false;
    }
    qsort(works, VCS_ZCODE_SCORE_UNITS, sizeof(works[0]),
          score_work_compare);
    uint8_t proof_roots[VCS_ZCODE_SCORE_UNITS][32], proof_set_root[32];
    for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++)
        memcpy(proof_roots[i], works[i].root, 32);
    if (vcs_zcode_proof_set_root(proof_roots, VCS_ZCODE_SCORE_UNITS,
                                 proof_set_root) != VCS_ZCODE_DEV_OK)
        return false;
    memset(lane, 0, sizeof(*lane));
    lane->schema_version = VCS_ZCODE_DEV_VERSION;
    lane->lane = VCS_ZCODE_LANE_PROVEN;
    memcpy(lane->source_root, candidate->candidate_source_root, 32);
    memcpy(lane->task_root, task_root, 32);
    memcpy(lane->candidate_root, candidate_root, 32);
    memcpy(lane->proof_policy_root, policy_root, 32);
    memcpy(lane->proof_set_root, proof_set_root, 32);
    score_fill(lane->prior_receipt_root, 90);
    lane->created_unix = 1400;
    uint8_t lane_seed[32]; score_fill(lane_seed, 91);
    ed25519_keypair(lane_pubkey, lane_secret, lane_seed);
    return vcs_zcode_lane_receipt_seal(lane, lane_secret, lane_pubkey) ==
           VCS_ZCODE_DEV_OK;
}

static int test_score_happy_path(void)
{
    int failures = 0;
    TEST("zcode score: fixed actions derive signed score with off-host credit withheld") {
        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate;
        struct vcs_zcode_proof_policy_v1 policy;
        struct vcs_zcode_lane_receipt_v1 lane;
        struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS];
        uint8_t secret[32], pubkey[32];
        ASSERT(score_fixture(&task, &candidate, &policy, &lane, works,
                             secret, pubkey));
        uint8_t proof_roots[VCS_ZCODE_SCORE_UNITS][32];
        struct vcs_zcode_work_receipt_v1 receipts[VCS_ZCODE_SCORE_UNITS];
        for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
            memcpy(proof_roots[i], works[i].root, 32);
            receipts[i] = works[i].receipt;
        }
        uint8_t package[32], release[32], recipe[32];
        ASSERT(zcl_hex_decode_lower(
            "6e03d74b8edab650e790424f4dc8274e8ca262cccfc4f94d5b068ad66f60f48e",
            package, 32));
        ASSERT(zcl_hex_decode_lower(
            "e13fe883c1166c9a9587a864716804b8d30e0246e8802ae477385952c268ed37",
            release, 32));
        ASSERT(zcl_hex_decode_lower(
            "71280e02ba1ec0c8006b28a8c325657cc2d2f5547b70a19442d91411199f7b49",
            recipe, 32));
        struct vcs_zcode_score_plan_input input = {
            .task = &task, .candidate = &candidate,
            .proof_policy = &policy, .proven_lane = &lane,
            .proof_receipt_roots = proof_roots,
            .work_receipts = receipts,
            .work_receipt_count = VCS_ZCODE_SCORE_UNITS,
            .package_root = package, .release_root = release,
            .recipe_root = recipe,
            .dependency_lock_root = task.dependency_lock_root,
            .api_capsule_root = task.toolchain_capsule_root,
        };
        struct vcs_zcode_score_receipt_v1 score, parsed;
        ASSERT_EQ(vcs_zcode_score_plan(&input, &score), VCS_ZCODE_SCORE_OK);
        ASSERT_EQ(score.score, 4);
        ASSERT_EQ(score.awarded_mask, 0x1b);
        ASSERT(score.evidence_roots[2][0] != 0);
        ASSERT(!vcs_zcode_score_offhost_reproducer_approved(
            receipts[0].signer_pubkey));
        ASSERT_EQ(vcs_zcode_score_receipt_seal(&score, secret, pubkey),
                  VCS_ZCODE_SCORE_OK);
        ASSERT_EQ(vcs_zcode_score_receipt_verify(&score),
                  VCS_ZCODE_SCORE_OK);
        uint8_t wire[VCS_ZCODE_SCORE_WIRE_BYTES], id[32], parsed_id[32];
        ASSERT_EQ(vcs_zcode_score_receipt_serialize(&score, wire),
                  VCS_ZCODE_SCORE_OK);
        ASSERT_EQ(vcs_zcode_score_receipt_parse(wire, sizeof(wire), &parsed),
                  VCS_ZCODE_SCORE_OK);
        ASSERT_EQ(vcs_zcode_score_receipt_id(&score, id),
                  VCS_ZCODE_SCORE_OK);
        ASSERT_EQ(vcs_zcode_score_receipt_id(&parsed, parsed_id),
                  VCS_ZCODE_SCORE_OK);
        ASSERT(memcmp(id, parsed_id, 32) == 0);
        ASSERT_EQ(vcs_zcode_score_receipt_parse(wire, sizeof(wire) - 1,
                                                &parsed),
                  VCS_ZCODE_SCORE_SHAPE);
        wire[12] = 1;
        ASSERT_EQ(vcs_zcode_score_receipt_parse(wire, sizeof(wire), &parsed),
                  VCS_ZCODE_SCORE_SHAPE);
        wire[12] = 0;

        char workspace[256];
        test_make_tmpdir(workspace, sizeof(workspace), "zcode_score", "cas");
        ASSERT(vcs_object_store_init(workspace));
        uint8_t task_wire[VCS_ZCODE_TASK_WIRE_BYTES];
        uint8_t candidate_wire[VCS_ZCODE_CANDIDATE_WIRE_BYTES];
        uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
        uint8_t lane_wire[VCS_ZCODE_LANE_WIRE_BYTES];
        uint8_t proof_wire[VCS_ZCODE_PROOF_SET_WIRE_MAX]; size_t proof_len = 0;
        ASSERT_EQ(vcs_zcode_task_serialize(&task, task_wire), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_candidate_serialize(&candidate, candidate_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_policy_serialize(&policy, policy_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_lane_receipt_serialize(&lane, lane_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_set_serialize(
                      proof_roots, VCS_ZCODE_SCORE_UNITS, proof_wire,
                      sizeof(proof_wire), &proof_len), VCS_ZCODE_DEV_OK);
        uint8_t task_root[32], candidate_root[32], policy_root[32];
        uint8_t proof_set_root[32], lane_root[32];
        ASSERT_EQ(vcs_zcode_task_root(&task, task_root), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_candidate_root(&candidate, candidate_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_policy_root(&policy, policy_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_set_root(
                      proof_roots, VCS_ZCODE_SCORE_UNITS, proof_set_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_lane_receipt_id(&lane, lane_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT(vcs_object_put_addressed(workspace, task_root, task_wire,
                                         sizeof(task_wire)));
        ASSERT(vcs_object_put_addressed(workspace, candidate_root,
                                         candidate_wire,
                                         sizeof(candidate_wire)));
        ASSERT(vcs_object_put_addressed(workspace, policy_root, policy_wire,
                                         sizeof(policy_wire)));
        ASSERT(vcs_object_put_addressed(workspace, proof_set_root, proof_wire,
                                         proof_len));
        ASSERT(vcs_object_put_addressed(workspace, lane_root, lane_wire,
                                         sizeof(lane_wire)));
        for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
            uint8_t work_wire[VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES];
            ASSERT_EQ(vcs_zcode_work_receipt_serialize(
                          &receipts[i], work_wire), VCS_ZCODE_DEV_OK);
            ASSERT(vcs_object_put_addressed(workspace, proof_roots[i],
                                             work_wire, sizeof(work_wire)));
        }
        char task_hex[sizeof(task_wire) * 2u + 1u];
        char candidate_hex[sizeof(candidate_wire) * 2u + 1u];
        char policy_hex[sizeof(policy_wire) * 2u + 1u];
        char lane_hex[sizeof(lane_wire) * 2u + 1u];
        char proof_hex[VCS_ZCODE_PROOF_SET_WIRE_MAX * 2u + 1u];
        char package_hex[65], release_hex[65], recipe_hex[65];
        char lock_hex[65], capsule_hex[65], score_wire_hex[1185];
        zcl_hex_encode(task_wire, sizeof(task_wire), task_hex);
        zcl_hex_encode(candidate_wire, sizeof(candidate_wire), candidate_hex);
        zcl_hex_encode(policy_wire, sizeof(policy_wire), policy_hex);
        zcl_hex_encode(lane_wire, sizeof(lane_wire), lane_hex);
        zcl_hex_encode(proof_wire, proof_len, proof_hex);
        zcl_hex_encode(package, 32, package_hex);
        zcl_hex_encode(release, 32, release_hex);
        zcl_hex_encode(recipe, 32, recipe_hex);
        zcl_hex_encode(task.dependency_lock_root, 32, lock_hex);
        zcl_hex_encode(task.toolchain_capsule_root, 32, capsule_hex);
        zcl_hex_encode(wire, sizeof(wire), score_wire_hex);
        struct json_value plan_input;
        json_init(&plan_input); json_set_object(&plan_input);
#define SCORE_PLAN_STR(key, value) ASSERT(json_push_kv_str(&plan_input, key, value))
        SCORE_PLAN_STR("workspace", workspace);
        SCORE_PLAN_STR("task_hex", task_hex);
        SCORE_PLAN_STR("candidate_hex", candidate_hex);
        SCORE_PLAN_STR("proof_policy_hex", policy_hex);
        SCORE_PLAN_STR("proof_set_hex", proof_hex);
        SCORE_PLAN_STR("proven_lane_hex", lane_hex);
        SCORE_PLAN_STR("package_root", package_hex);
        SCORE_PLAN_STR("release_root", release_hex);
        SCORE_PLAN_STR("recipe_root", recipe_hex);
        SCORE_PLAN_STR("dependency_lock_root", lock_hex);
        SCORE_PLAN_STR("api_capsule_root", capsule_hex);
#undef SCORE_PLAN_STR
        struct zcl_command_request plan_request = { .input = &plan_input };
        struct zcl_command_reply plan_reply;
        zcl_command_reply_init(&plan_reply, "zcl.zcode_score_test.v1");
        zcl_native_handle_zcode_package_dev_score_plan(&plan_request,
                                                       &plan_reply);
        ASSERT_EQ(plan_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(json_get_int(json_get(&plan_reply.data, "score")), 4);
        ASSERT(!json_get_bool(json_get(&plan_reply.data, "persisted")));
        zcl_command_reply_free(&plan_reply); json_free(&plan_input);

        struct json_value commit_input;
        json_init(&commit_input); json_set_object(&commit_input);
        ASSERT(json_push_kv_str(&commit_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&commit_input, "receipt_hex", score_wire_hex));
        struct zcl_command_request commit_request = { .input = &commit_input };
        struct zcl_command_reply commit_reply;
        zcl_command_reply_init(&commit_reply, "zcl.zcode_score_test.v1");
        zcl_native_handle_zcode_package_dev_score_commit(&commit_request,
                                                         &commit_reply);
        ASSERT_EQ(commit_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        const char *stored_root = json_get_str(json_get(
            &commit_reply.data, "score_receipt_root"));
        ASSERT(stored_root != NULL);
        char stored_root_copy[65];
        (void)snprintf(stored_root_copy, sizeof(stored_root_copy), "%s",
                       stored_root);
        zcl_command_reply_free(&commit_reply); json_free(&commit_input);

        struct json_value show_input;
        json_init(&show_input); json_set_object(&show_input);
        ASSERT(json_push_kv_str(&show_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&show_input, "root", stored_root_copy));
        struct zcl_command_request show_request = { .input = &show_input };
        struct zcl_command_reply show_reply;
        zcl_command_reply_init(&show_reply, "zcl.zcode_score_test.v1");
        zcl_native_handle_zcode_package_dev_score_show(&show_request,
                                                       &show_reply);
        ASSERT_EQ(show_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(json_get_int(json_get(&show_reply.data, "score")), 4);
        zcl_command_reply_free(&show_reply); json_free(&show_input);
        test_rm_rf(workspace);

        score.awarded_mask |= 1u << 2;
        score.score = 5;
        ASSERT_EQ(vcs_zcode_score_receipt_verify(&score),
                  VCS_ZCODE_SCORE_SHAPE);
        PASS();
    } _test_next:;
    return failures;
}

static int test_score_rejections(void)
{
    int failures = 0;
    TEST("zcode score: stale bindings and duplicate registered actions fail closed") {
        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate;
        struct vcs_zcode_proof_policy_v1 policy;
        struct vcs_zcode_lane_receipt_v1 lane;
        struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS];
        uint8_t secret[32], pubkey[32];
        ASSERT(score_fixture(&task, &candidate, &policy, &lane, works,
                             secret, pubkey));
        memcpy(works[1].receipt.action_root, works[0].receipt.action_root, 32);
        uint8_t seed[32], worker_secret[32], worker_pubkey[32];
        score_fill(seed, 110);
        ed25519_keypair(worker_pubkey, worker_secret, seed);
        ASSERT_EQ(vcs_zcode_work_receipt_seal(
                      &works[1].receipt, worker_secret, worker_pubkey),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_work_receipt_id(
                      &works[1].receipt, works[1].root), VCS_ZCODE_DEV_OK);
        qsort(works, VCS_ZCODE_SCORE_UNITS, sizeof(works[0]),
              score_work_compare);
        uint8_t roots[VCS_ZCODE_SCORE_UNITS][32];
        struct vcs_zcode_work_receipt_v1 receipts[VCS_ZCODE_SCORE_UNITS];
        for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
            memcpy(roots[i], works[i].root, 32);
            receipts[i] = works[i].receipt;
        }
        ASSERT_EQ(vcs_zcode_proof_set_root(
                      roots, VCS_ZCODE_SCORE_UNITS, lane.proof_set_root),
                  VCS_ZCODE_DEV_OK);
        memset(lane.signature, 0, sizeof(lane.signature));
        ASSERT_EQ(vcs_zcode_lane_receipt_seal(
                      &lane, secret, pubkey), VCS_ZCODE_DEV_OK);
        uint8_t package[32], release[32], recipe[32];
        score_fill(package, 101); score_fill(release, 102);
        score_fill(recipe, 103);
        struct vcs_zcode_score_plan_input input = {
            .task = &task, .candidate = &candidate,
            .proof_policy = &policy, .proven_lane = &lane,
            .proof_receipt_roots = roots, .work_receipts = receipts,
            .work_receipt_count = VCS_ZCODE_SCORE_UNITS,
            .package_root = package, .release_root = release,
            .recipe_root = recipe,
            .dependency_lock_root = task.dependency_lock_root,
            .api_capsule_root = task.toolchain_capsule_root,
        };
        struct vcs_zcode_score_receipt_v1 score;
        uint8_t stale[32]; score_fill(stale, 104);
        input.api_capsule_root = stale;
        ASSERT_EQ(vcs_zcode_score_plan(&input, &score),
                  VCS_ZCODE_SCORE_BINDING);
        input.api_capsule_root = task.toolchain_capsule_root;
        ASSERT_EQ(vcs_zcode_score_plan(&input, &score),
                  VCS_ZCODE_SCORE_DUPLICATE);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_score_receipt(void)
{
    int failures = test_score_happy_path() + test_score_rejections();
    printf("=== zcode_score_receipt: %d failures ===\n", failures);
    return failures;
}
