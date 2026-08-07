/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: prove canonical evidence-derived ZC23 Score receipts. */

#include "test/test_core.h"

#include "base/hex.h"
#include "command/native_command.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "core/uint256.h"
#include "vcs/package_manifest.h"
#include "vcs/package_release.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_contributor_binding.h"
#include "vcs/zcode_creation_attribution.h"
#include "vcs/zcode_continuity_policy.h"
#include "vcs/zcode_epoch_creation.h"
#include "vcs/zcode_patronage.h"
#include "vcs/zcode_patronage_funding.h"
#include "vcs/zcode_patronage_projection.h"
#include "vcs/zcode_patronage_settlement.h"
#include "vcs/zcode_score_receipt.h"

#include <stdlib.h>
#include <string.h>

struct score_work_fixture {
    struct vcs_zcode_work_receipt_v1 receipt;
    uint8_t root[32];
};

struct score_package_fixture {
    const char *name;
    const char *content;
    const char *release;
    const char *recipe;
    const char *lock;
    const char *capsule;
};

#define ZCODE_PACKAGE(name, dir, sequence, content, release, recipe, lock, capsule, dependency, signature) \
    {name, content, release, recipe, lock, capsule},
static const struct score_package_fixture score_packages[] = {
#include "../../../config/zcode_package_registry.def"
};
#undef ZCODE_PACKAGE

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

static bool score_fixture_for_roots(
    struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_candidate_v1 *candidate,
    struct vcs_zcode_proof_policy_v1 *policy,
    struct vcs_zcode_lane_receipt_v1 *lane,
    struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS],
    uint8_t lane_secret[32], uint8_t lane_pubkey[32],
    const uint8_t source_root[32], const uint8_t lock_root[32],
    const uint8_t capsule_root[32], const uint8_t author_pubkey[32])
{
    score_policy(policy);
    uint8_t policy_root[32], task_root[32], candidate_root[32];
    if (vcs_zcode_proof_policy_root(policy, policy_root) != VCS_ZCODE_DEV_OK)
        return false;
    memset(task, 0, sizeof(*task));
    task->schema_version = VCS_ZCODE_DEV_VERSION;
    memcpy(task->source_root, source_root, 32);
    memcpy(task->dependency_lock_root, lock_root, 32);
    memcpy(task->toolchain_capsule_root, capsule_root, 32);
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
    if (author_pubkey)
        memcpy(candidate->author_pubkey, author_pubkey, 32);
    else
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
        struct sha3_256_ctx evidence_sha;
        static const char evidence_domain[] =
            "zcl.zcode.selfhost_vertical_evidence.v1";
        sha3_256_init(&evidence_sha);
        sha3_256_write(&evidence_sha, (const uint8_t *)evidence_domain,
                       sizeof(evidence_domain));
        sha3_256_write(&evidence_sha, source_root, 32);
        sha3_256_write(&evidence_sha, work->action_root, 32);
        sha3_256_finalize(&evidence_sha, work->evidence_root);
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

static bool score_fixture(
    struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_candidate_v1 *candidate,
    struct vcs_zcode_proof_policy_v1 *policy,
    struct vcs_zcode_lane_receipt_v1 *lane,
    struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS],
    uint8_t lane_secret[32], uint8_t lane_pubkey[32])
{
    uint8_t source[32], lock[32], capsule[32];
    return zcl_hex_decode_lower(
               "ea54d7038792764c059a697792d46ee92fe75e29aa302d3c8db3a208a580876e",
               source, sizeof(source)) &&
        zcl_hex_decode_lower(
               "a32339729bd0a4e0cf723238faa4c1ad378d93f7de4bad84591781fc782d92a3",
               lock, sizeof(lock)) &&
        zcl_hex_decode_lower(
               "c0c3ec6514fd2a7ea242e087aff75b33fdc208a219c61855788509efef37b15d",
               capsule, sizeof(capsule)) &&
        score_fixture_for_roots(task, candidate, policy, lane, works,
                                lane_secret, lane_pubkey, source, lock,
                                capsule, NULL);
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
            "ea54d7038792764c059a697792d46ee92fe75e29aa302d3c8db3a208a580876e",
            package, 32));
        ASSERT(zcl_hex_decode_lower(
            "17f33b8f5be818a1a396d7c9bf04de1c11926af9e6d1118b313a9ac0a6335af8",
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

static bool score_store_vertical(
    const char *workspace, const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct vcs_zcode_proof_policy_v1 *policy,
    const struct vcs_zcode_lane_receipt_v1 *lane,
    const struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS],
    const struct vcs_zcode_score_receipt_v1 *score,
    uint8_t task_root[32], uint8_t candidate_root[32],
    uint8_t proof_set_root[32], uint8_t lane_root[32],
    uint8_t score_root[32])
{
    uint8_t task_wire[VCS_ZCODE_TASK_WIRE_BYTES];
    uint8_t candidate_wire[VCS_ZCODE_CANDIDATE_WIRE_BYTES];
    uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
    uint8_t lane_wire[VCS_ZCODE_LANE_WIRE_BYTES];
    uint8_t proof_wire[VCS_ZCODE_PROOF_SET_WIRE_MAX];
    uint8_t score_wire[VCS_ZCODE_SCORE_WIRE_BYTES];
    uint8_t policy_root[32], proof_roots[VCS_ZCODE_SCORE_UNITS][32];
    size_t proof_len = 0;
    for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++)
        memcpy(proof_roots[i], works[i].root, 32);
    if (vcs_zcode_task_serialize(task, task_wire) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_serialize(candidate, candidate_wire) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_policy_serialize(policy, policy_wire) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_serialize(lane, lane_wire) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_set_serialize(
            proof_roots, VCS_ZCODE_SCORE_UNITS, proof_wire,
            sizeof(proof_wire), &proof_len) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_score_receipt_serialize(score, score_wire) !=
            VCS_ZCODE_SCORE_OK ||
        vcs_zcode_task_root(task, task_root) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_root(candidate, candidate_root) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_policy_root(policy, policy_root) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_set_root(proof_roots, VCS_ZCODE_SCORE_UNITS,
                                 proof_set_root) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_lane_receipt_id(lane, lane_root) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_score_receipt_id(score, score_root) !=
            VCS_ZCODE_SCORE_OK)
        return false;
    if (!vcs_object_store_init(workspace) ||
        !vcs_object_put_addressed(workspace, task_root, task_wire,
                                  sizeof(task_wire)) ||
        !vcs_object_put_addressed(workspace, candidate_root, candidate_wire,
                                  sizeof(candidate_wire)) ||
        !vcs_object_put_addressed(workspace, policy_root, policy_wire,
                                  sizeof(policy_wire)) ||
        !vcs_object_put_addressed(workspace, proof_set_root, proof_wire,
                                  proof_len) ||
        !vcs_object_put_addressed(workspace, lane_root, lane_wire,
                                  sizeof(lane_wire)) ||
        !vcs_object_put_addressed(workspace, score_root, score_wire,
                                  sizeof(score_wire)))
        return false;
    for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
        uint8_t work_wire[VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES];
        if (vcs_zcode_work_receipt_serialize(&works[i].receipt, work_wire) !=
                VCS_ZCODE_DEV_OK ||
            !vcs_object_put_addressed(workspace, works[i].root, work_wire,
                                      sizeof(work_wire)))
            return false;
    }
    return true;
}

static int test_score_package_verticals(void)
{
    int failures = 0;
    TEST("zcode score: SHA3, base, then codec complete hermetic PROVEN verticals") {
        static const size_t evidence_order[] = {1, 0, 2};
        static const char *const scratch_labels[] = {"sha3", "base", "codec"};
        ASSERT_EQ(sizeof(score_packages) / sizeof(score_packages[0]), 3);
        for (size_t order = 0;
             order < sizeof(evidence_order) / sizeof(evidence_order[0]);
             order++) {
            const struct score_package_fixture *package =
                &score_packages[evidence_order[order]];
            uint8_t content[32], release[32], recipe[32], lock[32], capsule[32];
            ASSERT(zcl_hex_decode_lower(package->content, content, 32));
            ASSERT(zcl_hex_decode_lower(package->release, release, 32));
            ASSERT(zcl_hex_decode_lower(package->recipe, recipe, 32));
            ASSERT(zcl_hex_decode_lower(package->lock, lock, 32));
            ASSERT(zcl_hex_decode_lower(package->capsule, capsule, 32));
            struct vcs_zcode_task_v1 task;
            struct vcs_zcode_candidate_v1 candidate;
            struct vcs_zcode_proof_policy_v1 policy;
            struct vcs_zcode_lane_receipt_v1 lane;
            struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS];
            uint8_t secret[32], pubkey[32];
            ASSERT(score_fixture_for_roots(
                &task, &candidate, &policy, &lane, works, secret, pubkey,
                content, lock, capsule, NULL));
            uint8_t proof_roots[VCS_ZCODE_SCORE_UNITS][32];
            struct vcs_zcode_work_receipt_v1
                receipts[VCS_ZCODE_SCORE_UNITS];
            for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
                memcpy(proof_roots[i], works[i].root, 32);
                receipts[i] = works[i].receipt;
            }
            struct vcs_zcode_score_plan_input input = {
                .task = &task, .candidate = &candidate,
                .proof_policy = &policy, .proven_lane = &lane,
                .proof_receipt_roots = proof_roots,
                .work_receipts = receipts,
                .work_receipt_count = VCS_ZCODE_SCORE_UNITS,
                .package_root = content, .release_root = release,
                .recipe_root = recipe, .dependency_lock_root = lock,
                .api_capsule_root = capsule,
            };
            struct vcs_zcode_score_receipt_v1 score;
            ASSERT_EQ(vcs_zcode_score_plan(&input, &score),
                      VCS_ZCODE_SCORE_OK);
            ASSERT_EQ(score.awarded_mask, 0x1b);
            ASSERT_EQ(score.score, 4);
            ASSERT_EQ(vcs_zcode_score_receipt_seal(&score, secret, pubkey),
                      VCS_ZCODE_SCORE_OK);
            ASSERT_EQ(vcs_zcode_score_receipt_verify(&score),
                      VCS_ZCODE_SCORE_OK);
            char workspace[256];
            test_make_tmpdir(workspace, sizeof(workspace),
                             "zcode_selfhost_vertical",
                             scratch_labels[order]);
            uint8_t task_root[32], candidate_root[32], proof_set_root[32];
            uint8_t policy_root[32], lane_root[32], score_root[32];
            ASSERT(score_store_vertical(
                workspace, &task, &candidate, &policy, &lane, works,
                &score, task_root, candidate_root, proof_set_root,
                lane_root, score_root));
            ASSERT_EQ(vcs_zcode_proof_policy_root(&policy, policy_root),
                      VCS_ZCODE_DEV_OK);
            char task_hex[65], candidate_hex[65], policy_hex[65];
            char proof_hex[65];
            char lane_hex[65], score_hex[65];
            zcl_hex_encode(task_root, 32, task_hex);
            zcl_hex_encode(candidate_root, 32, candidate_hex);
            zcl_hex_encode(policy_root, 32, policy_hex);
            zcl_hex_encode(proof_set_root, 32, proof_hex);
            zcl_hex_encode(lane_root, 32, lane_hex);
            zcl_hex_encode(score_root, 32, score_hex);
            printf("zcode selfhost vertical: %s task=%s candidate=%s policy=%s proof_set=%s proven_lane=%s score_receipt=%s\n",
                   package->name, task_hex, candidate_hex, policy_hex,
                   proof_hex, lane_hex, score_hex);
            for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
                char evidence_hex[65], work_hex[65];
                zcl_hex_encode(score.evidence_roots[i], 32, evidence_hex);
                zcl_hex_encode(works[i].root, 32, work_hex);
                printf("zcode selfhost evidence: %s %s=%s work_receipt=%s awarded=%s\n",
                       package->name,
                       vcs_zcode_score_unit_name(
                           (enum vcs_zcode_score_unit)i),
                       evidence_hex, work_hex,
                       (score.awarded_mask & (UINT8_C(1) << i))
                           ? "true" : "false");
            }
            test_rm_rf(workspace);
        }
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
        uint8_t accepted_action[32], born_red_action[32];
        vcs_zcode_score_action_root(VCS_ZCODE_SCORE_ACCEPTED_EXTRACTION,
                                    accepted_action);
        vcs_zcode_score_action_root(VCS_ZCODE_SCORE_BORN_RED_DEFECT_TEST,
                                    born_red_action);
        size_t accepted = VCS_ZCODE_SCORE_UNITS;
        size_t born_red = VCS_ZCODE_SCORE_UNITS;
        for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
            if (memcmp(works[i].receipt.action_root, accepted_action, 32) == 0)
                accepted = i;
            if (memcmp(works[i].receipt.action_root, born_red_action, 32) == 0)
                born_red = i;
        }
        ASSERT(accepted < VCS_ZCODE_SCORE_UNITS);
        ASSERT(born_red < VCS_ZCODE_SCORE_UNITS);
        memcpy(works[born_red].receipt.action_root, accepted_action, 32);
        works[born_red].receipt.work_kind = VCS_ZCODE_WORK_REVIEW;
        uint8_t seed[32], worker_secret[32], worker_pubkey[32];
        score_fill(seed, 110);
        ed25519_keypair(worker_pubkey, worker_secret, seed);
        ASSERT_EQ(vcs_zcode_work_receipt_seal(
                      &works[born_red].receipt, worker_secret, worker_pubkey),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_work_receipt_id(
                      &works[born_red].receipt, works[born_red].root),
                  VCS_ZCODE_DEV_OK);
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

struct creation_callback_fixture {
    bool anchor_active;
    bool duplicate;
    bool continuity_duplicate;
    uint64_t opening_height;
    uint8_t opening_hash[32];
    uint64_t maturity_height;
    uint8_t maturity_hash[32];
    uint64_t expected_award_atoms;
};

static bool creation_test_anchor(void *opaque, uint64_t height,
                                 const uint8_t hash[32])
{
    const struct creation_callback_fixture *fixture = opaque;
    return fixture && fixture->anchor_active &&
           ((height == fixture->opening_height &&
             memcmp(hash, fixture->opening_hash, 32) == 0) ||
            (height == fixture->maturity_height &&
             memcmp(hash, fixture->maturity_hash, 32) == 0));
}

static bool creation_test_continuity_duplicate(
    void *opaque, const uint8_t event_key[32],
    const uint8_t attribution_root[32])
{
    const struct creation_callback_fixture *fixture = opaque;
    (void)event_key;
    (void)attribution_root;
    return fixture && fixture->continuity_duplicate;
}

static bool creation_test_duplicate(void *opaque,
                                    const uint8_t candidate_root[32],
                                    const uint8_t attribution_root[32])
{
    const struct creation_callback_fixture *fixture = opaque;
    (void)candidate_root;
    (void)attribution_root;
    return fixture && fixture->duplicate;
}

static bool creation_test_award(
    void *opaque,
    const struct vcs_zcode_creation_attribution_v1 *attribution,
    uint64_t *expected_atoms)
{
    const struct creation_callback_fixture *fixture = opaque;
    if (expected_atoms) *expected_atoms = 0;
    if (!fixture || !attribution || !expected_atoms)
        return false;
    *expected_atoms = fixture->expected_award_atoms;
    return true;
}

static bool creation_test_keypair(uint8_t value, struct privkey *secret,
                                  struct pubkey *pubkey)
{
    memset(secret->vch, value, 32);
    secret->fValid = true;
    secret->fCompressed = true;
    return privkey_get_pubkey(secret, pubkey) &&
           pubkey->size == COMPRESSED_PUBLIC_KEY_SIZE;
}

static bool creation_test_release(struct vcs_package_release *release,
                                  const uint8_t package_root[32],
                                  const uint8_t recipe_root[32])
{
    struct privkey secret;
    struct pubkey pubkey;
    if (!creation_test_keypair(0x31, &secret, &pubkey))
        return false;
    memset(release, 0, sizeof(*release));
    release->schema_version = VCS_PACKAGE_RELEASE_VERSION;
    (void)snprintf(release->name, sizeof(release->name), "commons/work");
    (void)snprintf(release->semver, sizeof(release->semver), "0.1.0");
    memcpy(release->package_root, package_root, 32);
    memcpy(release->publisher_pubkey, pubkey.vch, 33);
    release->publisher_sequence = 1;
    (void)snprintf(release->license, sizeof(release->license), "MIT");
    memcpy(release->recipe_root, recipe_root, 32);
    (void)snprintf(release->chain_id, sizeof(release->chain_id),
                   "zclassic-main");
    uint8_t id[32];
    struct uint256 hash;
    unsigned char compact[COMPACT_SIGNATURE_SIZE];
    if (vcs_package_release_id(release, id) != VCS_PACKAGE_RELEASE_OK)
        return false;
    memcpy(hash.data, id, 32);
    if (!privkey_sign_compact(&secret, &hash, compact))
        return false;
    memcpy(release->signature, compact + 1, 64);
    return vcs_package_release_verify(release) == VCS_PACKAGE_RELEASE_OK;
}

static bool creation_test_binding(
    struct vcs_zcode_contributor_binding_v1 *binding,
    const uint8_t network[32], uint8_t zid_pubkey[32],
    uint8_t zid_secret[32])
{
    uint8_t zid_seed[32], zcl_secret[32];
    memset(zid_seed, 0x41, sizeof(zid_seed));
    memset(zcl_secret, 0x42, sizeof(zcl_secret));
    ed25519_keypair(zid_pubkey, zid_secret, zid_seed);
    struct privkey secret;
    struct pubkey pubkey;
    if (!creation_test_keypair(0x42, &secret, &pubkey))
        return false;
    memset(binding, 0, sizeof(*binding));
    binding->schema_version = VCS_ZCODE_CONTRIBUTOR_BINDING_VERSION;
    memcpy(binding->network_genesis_root, network, 32);
    memcpy(binding->zid_pubkey, zid_pubkey, 32);
    memcpy(binding->zcl_pubkey, pubkey.vch, 33);
    struct key_id key_id = pubkey_get_id(&pubkey);
    memcpy(binding->zcl_key_id, key_id.id.data, 20);
    binding->sequence = 1;
    binding->issued_unix = 100;
    binding->expires_unix = 1000000;
    binding->operation = VCS_ZCODE_BINDING_ACTIVE;
    return vcs_zcode_contributor_binding_seal(
               binding, zid_secret, zid_pubkey, zcl_secret) ==
           VCS_ZCODE_BINDING_OK;
}

static int test_patronage_intent_cross_validation(void)
{
    int failures = 0;
    TEST("ZC23 patronage intent: CAS authorities rederive or fail closed") {
        char workspace[256];
        test_make_tmpdir(workspace, sizeof(workspace),
                         "zcode_patronage", "cas");
        ASSERT(vcs_object_store_init(workspace));
        uint8_t network[32], zid_pubkey[32], zid_secret[32];
        score_fill(network, 0xc1);
        struct vcs_zcode_contributor_binding_v1 binding;
        ASSERT(creation_test_binding(&binding, network, zid_pubkey,
                                     zid_secret));
        uint8_t binding_root[32];
        uint8_t binding_wire[VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_contributor_binding_root(&binding, binding_root),
                  VCS_ZCODE_BINDING_OK);
        ASSERT_EQ(vcs_zcode_contributor_binding_serialize(
                      &binding, binding_wire), VCS_ZCODE_BINDING_OK);
        ASSERT(vcs_object_put_addressed(workspace, binding_root,
                                        binding_wire, sizeof(binding_wire)));

        struct vcs_zcode_proof_policy_v1 policy;
        score_policy(&policy);
        uint8_t policy_root[32];
        uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_proof_policy_root(&policy, policy_root),
                  VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_proof_policy_serialize(&policy, policy_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(vcs_object_put_addressed(workspace, policy_root, policy_wire,
                                        sizeof(policy_wire)));

        struct vcs_zcode_task_v1 task;
        memset(&task, 0, sizeof(task));
        task.schema_version = VCS_ZCODE_DEV_VERSION;
        score_fill(task.source_root, 1);
        score_fill(task.dependency_lock_root, 2);
        score_fill(task.toolchain_capsule_root, 3);
        score_fill(task.write_scope_root, 4);
        score_fill(task.acceptance_tests_root, 5);
        memcpy(task.proof_policy_root, policy_root, 32);
        score_fill(task.model_policy_root, 6);
        score_fill(task.goal_root, 7);
        task.capabilities = VCS_ZCODE_TASK_CAP_V1_MASK;
        task.max_changed_files = 4;
        task.max_patch_bytes = 4096;
        task.max_context_bytes = 4096;
        task.max_cpu_seconds = 60;
        task.max_memory_bytes = 1024 * 1024;
        task.max_output_bytes = 1024 * 1024;
        task.expires_unix = 5000;
        uint8_t task_root[32], task_wire[VCS_ZCODE_TASK_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_task_root(&task, task_root), VCS_ZCODE_DEV_OK);
        ASSERT_EQ(vcs_zcode_task_serialize(&task, task_wire),
                  VCS_ZCODE_DEV_OK);
        ASSERT(vcs_object_put_addressed(workspace, task_root, task_wire,
                                        sizeof(task_wire)));

        struct vcs_zcode_patronage_intent_v1 intent;
        memset(&intent, 0, sizeof(intent));
        intent.schema_version = VCS_ZCODE_PATRONAGE_INTENT_VERSION;
        intent.mode = VCS_ZCODE_PATRONAGE_EXACT_TASK_COMMISSION;
        intent.target_kind = VCS_ZCODE_PATRONAGE_TARGET_TASK;
        intent.settlement_trust_mode = VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER;
        intent.flags = VCS_ZCODE_PATRONAGE_NO_AUTHORITY |
                       VCS_ZCODE_PATRONAGE_SIMULATION_ONLY;
        memcpy(intent.network_genesis_root, network, 32);
        score_fill(intent.zc23_token_or_simulation_root, 8);
        memcpy(intent.patron_contributor_binding_root, binding_root, 32);
        memcpy(intent.patron_zid_pubkey, zid_pubkey, 32);
        memcpy(intent.target_root, task_root, 32);
        memcpy(intent.task_root, task_root, 32);
        memcpy(intent.proof_policy_root, policy_root, 32);
        memcpy(intent.intended_recipient_binding_root, binding_root, 32);
        intent.amount_atoms = 100000000;
        intent.created_unix = 1000;
        intent.expires_unix = 2000;
        intent.refund_height = 3000;
        intent.refund_unix = 2100;
        intent.sequence = 1;
        intent.maximum_zcl_fee_zat = 10000;
        ASSERT_EQ(vcs_zcode_patronage_intent_seal(
                      &intent, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_OK);
        struct vcs_zcode_patronage_validation_context context = {
            .workspace = workspace,
            .expected_network_genesis_root = network,
            .now_unix = 1500,
        };
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(&intent, &context),
                  VCS_ZCODE_PATRONAGE_OK);
        uint8_t offer_plan_wire[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES];
        char offer_plan_hex[sizeof(offer_plan_wire) * 2u + 1u];
        char network_hex[65];
        ASSERT_EQ(vcs_zcode_patronage_intent_serialize(
                      &intent, offer_plan_wire), VCS_ZCODE_PATRONAGE_OK);
        zcl_hex_encode(offer_plan_wire, sizeof(offer_plan_wire),
                       offer_plan_hex);
        zcl_hex_encode(network, sizeof(network), network_hex);
        struct json_value offer_input;
        json_init(&offer_input); json_set_object(&offer_input);
        ASSERT(json_push_kv_str(&offer_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&offer_input, "intent_hex",
                                offer_plan_hex));
        ASSERT(json_push_kv_str(&offer_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&offer_input, "now_unix", 1500));
        struct zcl_command_request offer_request = {.input = &offer_input};
        struct zcl_command_reply offer_reply;
        zcl_command_reply_init(&offer_reply, "zcl.test.patronage.v1");
        zcl_native_handle_zcode_patronage_offer_plan(
            &offer_request, &offer_reply);
        ASSERT_EQ(offer_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(!json_get_bool(json_get(&offer_reply.data, "persisted")));
        ASSERT(!json_get_bool(json_get(&offer_reply.data, "funded")));
        zcl_command_reply_free(&offer_reply);
        json_free(&offer_input);

        intent.settlement_trust_mode =
            VCS_ZCODE_PATRONAGE_SIMULATED_FUNDING;
        ASSERT_EQ(vcs_zcode_patronage_intent_seal(
                      &intent, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_OK);
        uint8_t intent_root[32];
        uint8_t intent_wire[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_patronage_intent_root(&intent, intent_root),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_serialize(&intent, intent_wire),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT(vcs_object_put_addressed(workspace, intent_root, intent_wire,
                                        sizeof(intent_wire)));
        char committed_intent_hex[sizeof(intent_wire) * 2u + 1u];
        char committed_intent_root_hex[65];
        zcl_hex_encode(intent_wire, sizeof(intent_wire), committed_intent_hex);
        zcl_hex_encode(intent_root, 32, committed_intent_root_hex);
        struct json_value commit_input;
        json_init(&commit_input); json_set_object(&commit_input);
        ASSERT(json_push_kv_str(&commit_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&commit_input, "intent_hex",
                                committed_intent_hex));
        ASSERT(json_push_kv_str(&commit_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&commit_input, "now_unix", 1500));
        struct zcl_command_request commit_request = {.input = &commit_input};
        struct zcl_command_reply commit_reply;
        zcl_command_reply_init(&commit_reply, "zcl.test.patronage.v1");
        zcl_native_handle_zcode_patronage_offer_commit(
            &commit_request, &commit_reply);
        ASSERT_EQ(commit_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(json_get_bool(json_get(&commit_reply.data, "persisted")));
        ASSERT(!json_get_bool(json_get(&commit_reply.data, "funded")));
        zcl_command_reply_free(&commit_reply);
        json_free(&commit_input);

        struct json_value show_input;
        json_init(&show_input); json_set_object(&show_input);
        ASSERT(json_push_kv_str(&show_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&show_input, "root",
                                committed_intent_root_hex));
        ASSERT(json_push_kv_str(&show_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&show_input, "now_unix", 1500));
        struct zcl_command_request show_request = {.input = &show_input};
        struct zcl_command_reply show_reply;
        zcl_command_reply_init(&show_reply, "zcl.test.patronage.v1");
        zcl_native_handle_zcode_patronage_show(&show_request, &show_reply);
        ASSERT_EQ(show_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&show_reply.data, "object")),
                      "patronage_intent");
        ASSERT(!json_get_bool(json_get(&show_reply.data, "implies_ownership")));
        zcl_command_reply_free(&show_reply);
        json_free(&show_input);
        struct vcs_zcode_patronage_funding_v1 funding;
        memset(&funding, 0, sizeof(funding));
        funding.schema_version = VCS_ZCODE_PATRONAGE_FUNDING_VERSION;
        funding.funding_kind =
            VCS_ZCODE_PATRONAGE_FUNDING_FULLY_SIMULATED;
        funding.flags = VCS_ZCODE_PATRONAGE_FUNDING_NO_LIVE_FUNDS |
                        VCS_ZCODE_PATRONAGE_FUNDING_NO_TRANSACTION_BYTES;
        memcpy(funding.network_genesis_root, network, 32);
        memcpy(funding.patronage_intent_root, intent_root, 32);
        memcpy(funding.funder_contributor_binding_root, binding_root, 32);
        memcpy(funding.funder_zid_pubkey, zid_pubkey, 32);
        funding.amount_atoms = intent.amount_atoms;
        funding.created_unix = 1600;
        funding.sequence = 1;
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, funding.amount_atoms,
                      funding.simulation_plan_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(
                      &funding, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        uint8_t funding_wire[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES];
        struct vcs_zcode_patronage_funding_v1 parsed_funding, zero_funding;
        memset(&zero_funding, 0, sizeof(zero_funding));
        ASSERT_EQ(vcs_zcode_patronage_funding_serialize(
                      &funding, funding_wire),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_parse(
                      funding_wire, sizeof(funding_wire), &parsed_funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_verify(&parsed_funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_parse(
                      funding_wire, sizeof(funding_wire) - 1,
                      &parsed_funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_WIRE_SIZE);
        ASSERT(memcmp(&parsed_funding, &zero_funding,
                      sizeof(parsed_funding)) == 0);
        context.now_unix = 1700;
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(
                      &funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        uint8_t committed_funding_root[32];
        char committed_funding_hex[sizeof(funding_wire) * 2u + 1u];
        char committed_funding_root_hex[65];
        ASSERT_EQ(vcs_zcode_patronage_funding_root(
                      &funding, committed_funding_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        zcl_hex_encode(funding_wire, sizeof(funding_wire),
                       committed_funding_hex);
        zcl_hex_encode(committed_funding_root, 32,
                       committed_funding_root_hex);
        struct json_value fund_input;
        json_init(&fund_input); json_set_object(&fund_input);
        ASSERT(json_push_kv_str(&fund_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&fund_input, "funding_hex",
                                committed_funding_hex));
        ASSERT(json_push_kv_str(&fund_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&fund_input, "now_unix", 1700));
        struct zcl_command_request fund_request = {.input = &fund_input};
        struct zcl_command_reply fund_plan_reply, fund_commit_reply;
        zcl_command_reply_init(&fund_plan_reply, "zcl.test.patronage.v1");
        zcl_native_handle_zcode_patronage_fund_plan(
            &fund_request, &fund_plan_reply);
        ASSERT_EQ(fund_plan_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(!json_get_bool(json_get(&fund_plan_reply.data, "persisted")));
        ASSERT(!json_get_bool(json_get(&fund_plan_reply.data, "funded")));
        ASSERT(json_get_bool(json_get(&fund_plan_reply.data,
                                      "simulation_funded")));
        zcl_command_reply_free(&fund_plan_reply);
        zcl_command_reply_init(&fund_commit_reply, "zcl.test.patronage.v1");
        zcl_native_handle_zcode_patronage_fund_commit(
            &fund_request, &fund_commit_reply);
        ASSERT_EQ(fund_commit_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(json_get_bool(json_get(&fund_commit_reply.data, "persisted")));
        ASSERT(!json_get_bool(json_get(&fund_commit_reply.data,
                                       "moves_live_funds")));
        zcl_command_reply_free(&fund_commit_reply);
        json_free(&fund_input);

        json_init(&show_input); json_set_object(&show_input);
        ASSERT(json_push_kv_str(&show_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&show_input, "root",
                                committed_funding_root_hex));
        ASSERT(json_push_kv_str(&show_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&show_input, "now_unix", 1700));
        show_request.input = &show_input;
        zcl_command_reply_init(&show_reply, "zcl.test.patronage.v1");
        zcl_native_handle_zcode_patronage_show(&show_request, &show_reply);
        ASSERT_EQ(show_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&show_reply.data, "object")),
                      "patronage_funding");
        ASSERT(!json_get_bool(json_get(&show_reply.data, "funded")));
        zcl_command_reply_free(&show_reply);
        json_free(&show_input);
        funding.flags &= (uint8_t)
            ~VCS_ZCODE_PATRONAGE_FUNDING_NO_LIVE_FUNDS;
        ASSERT_EQ(vcs_zcode_patronage_funding_validate(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_SHAPE);
        funding.flags |= VCS_ZCODE_PATRONAGE_FUNDING_NO_LIVE_FUNDS;
        funding.amount_atoms++;
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, funding.amount_atoms,
                      funding.simulation_plan_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(
                      &funding, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(
                      &funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_AMOUNT);
        funding.amount_atoms--;

        struct vcs_zcode_patronage_intent_v1 gift = intent;
        gift.mode = VCS_ZCODE_PATRONAGE_DIRECT_GIFT;
        gift.target_kind = VCS_ZCODE_PATRONAGE_TARGET_CONTRIBUTOR;
        memcpy(gift.target_root, binding_root, 32);
        memset(gift.task_root, 0, 32);
        memset(gift.proof_policy_root, 0, 32);
        gift.refund_height = 0;
        gift.refund_unix = 0;
        gift.sequence = 2;
        gift.amount_atoms = UINT64_C(50000000);
        ASSERT_EQ(vcs_zcode_patronage_intent_seal(
                      &gift, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_OK);
        uint8_t gift_root[32];
        ASSERT_EQ(vcs_zcode_patronage_intent_root(&gift, gift_root),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_serialize(&gift, intent_wire),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT(vcs_object_put_addressed(workspace, gift_root, intent_wire,
                                        sizeof(intent_wire)));

        struct vcs_zcode_patronage_funding_v1 gift_funding;
        memset(&gift_funding, 0, sizeof(gift_funding));
        gift_funding.schema_version = VCS_ZCODE_PATRONAGE_FUNDING_VERSION;
        gift_funding.funding_kind =
            VCS_ZCODE_PATRONAGE_FUNDING_FULLY_SIMULATED;
        gift_funding.flags = VCS_ZCODE_PATRONAGE_FUNDING_NO_LIVE_FUNDS |
                             VCS_ZCODE_PATRONAGE_FUNDING_NO_TRANSACTION_BYTES;
        memcpy(gift_funding.network_genesis_root, network, 32);
        memcpy(gift_funding.patronage_intent_root, gift_root, 32);
        memcpy(gift_funding.funder_contributor_binding_root,
               binding_root, 32);
        memcpy(gift_funding.funder_zid_pubkey, zid_pubkey, 32);
        gift_funding.amount_atoms = gift.amount_atoms;
        gift_funding.created_unix = 1600;
        gift_funding.sequence = 2;
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      gift_root, gift.amount_atoms,
                      gift_funding.simulation_plan_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(
                      &gift_funding, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        uint8_t gift_funding_root[32];
        ASSERT_EQ(vcs_zcode_patronage_funding_root(
                      &gift_funding, gift_funding_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_serialize(
                      &gift_funding, funding_wire),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT(vcs_object_put_addressed(workspace, gift_funding_root,
                                        funding_wire,
                                        sizeof(funding_wire)));

        struct vcs_zcode_patronage_settlement_v1 gift_settlement;
        memset(&gift_settlement, 0, sizeof(gift_settlement));
        gift_settlement.schema_version =
            VCS_ZCODE_PATRONAGE_SETTLEMENT_VERSION;
        gift_settlement.action = VCS_ZCODE_PATRONAGE_SIMULATED_SETTLED;
        gift_settlement.flags =
            VCS_ZCODE_PATRONAGE_SETTLEMENT_SIMULATION_ONLY |
            VCS_ZCODE_PATRONAGE_SETTLEMENT_NO_LIVE_FUNDS |
            VCS_ZCODE_PATRONAGE_SETTLEMENT_NO_TRANSACTION_BYTES;
        memcpy(gift_settlement.network_genesis_root, network, 32);
        memcpy(gift_settlement.patronage_intent_root, gift_root, 32);
        memcpy(gift_settlement.patronage_funding_root,
               gift_funding_root, 32);
        memcpy(gift_settlement.recipient_contributor_binding_root,
               binding_root, 32);
        memcpy(gift_settlement.settler_zid_pubkey, zid_pubkey, 32);
        gift_settlement.amount_atoms = gift.amount_atoms;
        gift_settlement.created_unix = 1700;
        gift_settlement.observed_height = 1;
        gift_settlement.observed_mtp = 1500;
        gift_settlement.sequence = 1;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &gift_settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        struct vcs_zcode_patronage_settlement_validation_context
            gift_context = {
                .patronage = &context,
                .active_height = 1,
                .active_mtp = 1700,
                .now_unix = 1700,
            };
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &gift_settlement, &gift_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        memcpy(gift_settlement.patronage_funding_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &gift_settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &gift_settlement, &gift_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_FUNDING);
        memcpy(gift_settlement.patronage_funding_root,
               gift_funding_root, 32);
        gift_settlement.action = VCS_ZCODE_PATRONAGE_SIMULATED_REFUNDED;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &gift_settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &gift_settlement, &gift_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_INTENT);

        uint8_t other_network[32]; score_fill(other_network, 0xc2);
        context.expected_network_genesis_root = other_network;
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(&intent, &context),
                  VCS_ZCODE_PATRONAGE_NETWORK);
        context.expected_network_genesis_root = network;
        memcpy(intent.task_root, policy_root, 32);
        memcpy(intent.target_root, policy_root, 32);
        ASSERT_EQ(vcs_zcode_patronage_intent_seal(
                      &intent, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(&intent, &context),
                  VCS_ZCODE_PATRONAGE_TASK);
        test_rm_rf(workspace);
        PASS();
    } _test_next:;
    return failures;
}

static int test_creation_attribution_cross_validation(void)
{
    int failures = 0;
    TEST("ZC23 creation attribution: CAS authorities rederive or fail closed") {
        static const uint8_t license_bytes[] =
            "MIT License\n\nPermission is hereby granted, free of charge.\n";
        uint8_t license_chunk[32], package_root[32], license_root[32];
        ASSERT(vcs_package_chunk_hash(license_bytes,
                                      sizeof(license_bytes) - 1,
                                      license_chunk));
        struct vcs_package_manifest manifest;
        vcs_package_manifest_init(&manifest);
        ASSERT(vcs_package_manifest_add(
            &manifest, "LICENSE", VCS_PACKAGE_MODE_FILE,
            sizeof(license_bytes) - 1, license_chunk, 1));
        ASSERT(vcs_package_manifest_root(&manifest, package_root));
        ASSERT(vcs_package_file_hash(&manifest.files[0], license_root));
        uint8_t *manifest_wire = NULL; size_t manifest_wire_len = 0;
        ASSERT(vcs_package_manifest_serialize(
            &manifest, &manifest_wire, &manifest_wire_len));

        uint8_t network[32], policy_authority[32], recipe[32], lock[32];
        char network_hex[65];
        uint8_t capsule[32], zid_pubkey[32], zid_secret[32];
        score_fill(network, 0xa1); score_fill(policy_authority, 0xa2);
        zcl_hex_encode(network, sizeof(network), network_hex);
        score_fill(recipe, 0xa3); score_fill(lock, 0xa4);
        score_fill(capsule, 0xa5);
        struct vcs_zcode_contributor_binding_v1 binding;
        ASSERT(creation_test_binding(&binding, network, zid_pubkey,
                                     zid_secret));
        uint8_t binding_wire[VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES];
        uint8_t binding_root[32];
        ASSERT_EQ(vcs_zcode_contributor_binding_serialize(
                      &binding, binding_wire), VCS_ZCODE_BINDING_OK);
        ASSERT_EQ(vcs_zcode_contributor_binding_root(
                      &binding, binding_root), VCS_ZCODE_BINDING_OK);

        struct vcs_package_release release;
        ASSERT(creation_test_release(&release, package_root, recipe));
        uint8_t *release_wire = NULL; size_t release_wire_len = 0;
        uint8_t release_root[32];
        ASSERT_EQ(vcs_package_release_serialize(
                      &release, &release_wire, &release_wire_len),
                  VCS_PACKAGE_RELEASE_OK);
        ASSERT_EQ(vcs_package_release_id(&release, release_root),
                  VCS_PACKAGE_RELEASE_OK);

        struct vcs_zcode_task_v1 task;
        struct vcs_zcode_candidate_v1 candidate;
        struct vcs_zcode_proof_policy_v1 proof_policy;
        struct vcs_zcode_lane_receipt_v1 lane;
        struct score_work_fixture works[VCS_ZCODE_SCORE_UNITS];
        uint8_t lane_secret[32], lane_pubkey[32];
        ASSERT(score_fixture_for_roots(
            &task, &candidate, &proof_policy, &lane, works,
            lane_secret, lane_pubkey, package_root, lock, capsule,
            zid_pubkey));
        uint8_t proof_roots[VCS_ZCODE_SCORE_UNITS][32];
        struct vcs_zcode_work_receipt_v1 receipts[VCS_ZCODE_SCORE_UNITS];
        for (size_t i = 0; i < VCS_ZCODE_SCORE_UNITS; i++) {
            memcpy(proof_roots[i], works[i].root, 32);
            receipts[i] = works[i].receipt;
        }
        struct vcs_zcode_score_plan_input score_input = {
            .task = &task, .candidate = &candidate,
            .proof_policy = &proof_policy, .proven_lane = &lane,
            .proof_receipt_roots = proof_roots,
            .work_receipts = receipts,
            .work_receipt_count = VCS_ZCODE_SCORE_UNITS,
            .package_root = package_root, .release_root = release_root,
            .recipe_root = recipe, .dependency_lock_root = lock,
            .api_capsule_root = capsule,
        };
        struct vcs_zcode_score_receipt_v1 score;
        ASSERT_EQ(vcs_zcode_score_plan(&score_input, &score),
                  VCS_ZCODE_SCORE_OK);
        ASSERT_EQ(vcs_zcode_score_receipt_seal(
                      &score, lane_secret, lane_pubkey), VCS_ZCODE_SCORE_OK);

        char workspace[256];
        test_make_tmpdir(workspace, sizeof(workspace),
                         "zcode_creation", "cross");
        uint8_t task_root[32], candidate_root[32], proof_set_root[32];
        uint8_t lane_root[32], score_root[32], proof_policy_root[32];
        ASSERT(score_store_vertical(
            workspace, &task, &candidate, &proof_policy, &lane, works,
            &score, task_root, candidate_root, proof_set_root,
            lane_root, score_root));
        ASSERT_EQ(vcs_zcode_proof_policy_root(
                      &proof_policy, proof_policy_root), VCS_ZCODE_DEV_OK);
        ASSERT(vcs_object_put_addressed(workspace, package_root,
                                         manifest_wire,
                                         manifest_wire_len));
        ASSERT(vcs_object_put_addressed(workspace, license_chunk,
                                         license_bytes,
                                         sizeof(license_bytes) - 1));
        ASSERT(vcs_object_put_addressed(workspace, release_root,
                                         release_wire, release_wire_len));
        ASSERT(vcs_object_put_addressed(workspace, binding_root,
                                         binding_wire,
                                         sizeof(binding_wire)));

        struct vcs_zcode_creation_attribution_v1 attribution;
        memset(&attribution, 0, sizeof(attribution));
        attribution.schema_version =
            VCS_ZCODE_CREATION_ATTRIBUTION_VERSION;
        attribution.category = VCS_ZCODE_CREATION_PUBLIC_SOURCE;
        attribution.epoch = 3;
        attribution.award_atoms = UINT64_C(250000000);
        attribution.challenge_opening_height = 100;
        score_fill(attribution.challenge_opening_hash, 0xb1);
        attribution.challenge_opening_mtp = 1000;
        attribution.challenge_maturity_height = 8164;
        attribution.challenge_maturity_mtp = 605800;
        attribution.created_unix = 605801;
        memcpy(attribution.network_genesis_root, network, 32);
        memcpy(attribution.zc23_policy_root, policy_authority, 32);
        memcpy(attribution.contributor_binding_root, binding_root, 32);
        memcpy(attribution.task_root, task_root, 32);
        memcpy(attribution.candidate_root, candidate_root, 32);
        memcpy(attribution.proof_policy_root, proof_policy_root, 32);
        memcpy(attribution.proof_set_root, proof_set_root, 32);
        memcpy(attribution.proven_lane_root, lane_root, 32);
        memcpy(attribution.score_receipt_root, score_root, 32);
        memcpy(attribution.package_root, package_root, 32);
        memcpy(attribution.release_root, release_root, 32);
        memcpy(attribution.license_evidence_root, license_root, 32);

        struct creation_callback_fixture callbacks = {
            .anchor_active = true, .duplicate = false,
            .opening_height = attribution.challenge_opening_height,
            .maturity_height = attribution.challenge_maturity_height,
            .expected_award_atoms = attribution.award_atoms,
        };
        memcpy(callbacks.opening_hash, attribution.challenge_opening_hash,
               32);
        memcpy(callbacks.maturity_hash, attribution.challenge_opening_hash,
               32);
        struct vcs_zcode_creation_validation_context context = {
            .workspace = workspace,
            .expected_network_genesis_root = network,
            .expected_zc23_policy_root = policy_authority,
            .expected_epoch = attribution.epoch,
            .expected_award_atoms = attribution.award_atoms,
            .active_height = 9000,
            .active_mtp = 700000,
            .now_unix = 700000,
            .anchor_is_active = creation_test_anchor,
            .contribution_is_duplicate = creation_test_duplicate,
            .callback_opaque = &callbacks,
        };
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &attribution, &context), VCS_ZCODE_CREATION_OK);

        uint8_t attribution_wire[
            VCS_ZCODE_CREATION_ATTRIBUTION_WIRE_BYTES];
        uint8_t attribution_root[32];
        ASSERT_EQ(vcs_zcode_creation_attribution_serialize(
                      &attribution, attribution_wire),
                  VCS_ZCODE_CREATION_OK);
        ASSERT_EQ(vcs_zcode_creation_attribution_root(
                      &attribution, attribution_root),
                  VCS_ZCODE_CREATION_OK);
        ASSERT(vcs_object_put_addressed(
            workspace, attribution_root, attribution_wire,
            sizeof(attribution_wire)));
        struct vcs_zcode_epoch_creation_set_v1 epoch_set;
        vcs_zcode_epoch_creation_init(&epoch_set);
        epoch_set.schema_version = VCS_ZCODE_EPOCH_CREATION_VERSION;
        epoch_set.epoch = attribution.epoch;
        ASSERT_EQ(vcs_zc23_policy_epoch_cap_atoms(
                      epoch_set.epoch, &epoch_set.emission_cap_atoms),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        epoch_set.actual_mint_atoms = attribution.award_atoms;
        epoch_set.unissued_atoms = epoch_set.emission_cap_atoms -
                                   epoch_set.actual_mint_atoms;
        memcpy(epoch_set.network_genesis_root, network, 32);
        memcpy(epoch_set.zc23_policy_root, policy_authority, 32);
        score_fill(epoch_set.previous_epoch_creation_root, 0xc1);
        score_fill(epoch_set.committee_evidence_snapshot_root, 0xc2);
        epoch_set.opening_height = attribution.challenge_opening_height;
        memcpy(epoch_set.opening_hash,
               attribution.challenge_opening_hash, 32);
        epoch_set.opening_mtp = attribution.challenge_opening_mtp;
        epoch_set.maturity_height = attribution.challenge_maturity_height;
        score_fill(epoch_set.maturity_hash, 0xc3);
        epoch_set.maturity_mtp = attribution.challenge_maturity_mtp;
        epoch_set.attribution_roots = &attribution_root;
        epoch_set.attribution_count = 1;
        callbacks.maturity_height = epoch_set.maturity_height;
        memcpy(callbacks.maturity_hash, epoch_set.maturity_hash, 32);
        struct vcs_zcode_epoch_creation_validation_context epoch_context = {
            .workspace = workspace,
            .expected_network_genesis_root = network,
            .expected_zc23_policy_root = policy_authority,
            .expected_previous_epoch_creation_root =
                epoch_set.previous_epoch_creation_root,
            .observed_actual_mint_atoms = epoch_set.actual_mint_atoms,
            .active_height = 9000,
            .active_mtp = 700000,
            .now_unix = 700000,
            .anchor_is_active = creation_test_anchor,
            .contribution_is_duplicate = creation_test_duplicate,
            .award_atoms_for_creation = creation_test_award,
            .callback_opaque = &callbacks,
        };
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(
                      &epoch_set, &epoch_context),
                  VCS_ZCODE_EPOCH_CREATION_OK);

        struct vcs_zcode_patronage_intent_v1 intent;
        memset(&intent, 0, sizeof(intent));
        intent.schema_version = VCS_ZCODE_PATRONAGE_INTENT_VERSION;
        intent.mode = VCS_ZCODE_PATRONAGE_EXACT_TASK_COMMISSION;
        intent.target_kind = VCS_ZCODE_PATRONAGE_TARGET_TASK;
        intent.settlement_trust_mode =
            VCS_ZCODE_PATRONAGE_SIMULATED_FUNDING;
        intent.flags = VCS_ZCODE_PATRONAGE_NO_AUTHORITY |
                       VCS_ZCODE_PATRONAGE_SIMULATION_ONLY;
        memcpy(intent.network_genesis_root, network, 32);
        score_fill(intent.zc23_token_or_simulation_root, 0xd1);
        memcpy(intent.patron_contributor_binding_root, binding_root, 32);
        memcpy(intent.patron_zid_pubkey, zid_pubkey, 32);
        memcpy(intent.target_root, task_root, 32);
        memcpy(intent.task_root, task_root, 32);
        memcpy(intent.proof_policy_root, proof_policy_root, 32);
        memcpy(intent.intended_recipient_binding_root, binding_root, 32);
        intent.amount_atoms = UINT64_C(100000000);
        intent.created_unix = 1000;
        intent.expires_unix = 800000;
        intent.refund_height = 10000;
        intent.refund_unix = 800100;
        intent.sequence = 1;
        intent.maximum_zcl_fee_zat = 10000;
        ASSERT_EQ(vcs_zcode_patronage_intent_seal(
                      &intent, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_OK);
        uint8_t intent_root[32];
        uint8_t intent_wire[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_patronage_intent_root(&intent, intent_root),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_serialize(
                      &intent, intent_wire), VCS_ZCODE_PATRONAGE_OK);
        ASSERT(vcs_object_put_addressed(workspace, intent_root, intent_wire,
                                        sizeof(intent_wire)));

        struct vcs_zcode_patronage_funding_v1 funding;
        memset(&funding, 0, sizeof(funding));
        funding.schema_version = VCS_ZCODE_PATRONAGE_FUNDING_VERSION;
        funding.funding_kind =
            VCS_ZCODE_PATRONAGE_FUNDING_FULLY_SIMULATED;
        funding.flags = VCS_ZCODE_PATRONAGE_FUNDING_NO_LIVE_FUNDS |
                        VCS_ZCODE_PATRONAGE_FUNDING_NO_TRANSACTION_BYTES;
        memcpy(funding.network_genesis_root, network, 32);
        memcpy(funding.patronage_intent_root, intent_root, 32);
        memcpy(funding.funder_contributor_binding_root, binding_root, 32);
        memcpy(funding.funder_zid_pubkey, zid_pubkey, 32);
        funding.amount_atoms = intent.amount_atoms;
        funding.created_unix = 1100;
        funding.sequence = 1;
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, funding.amount_atoms,
                      funding.simulation_plan_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(
                      &funding, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        uint8_t funding_root[32];
        uint8_t funding_wire[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_patronage_funding_root(&funding, funding_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_serialize(
                      &funding, funding_wire),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT(vcs_object_put_addressed(workspace, funding_root, funding_wire,
                                        sizeof(funding_wire)));

        struct vcs_zcode_patronage_settlement_v1 settlement;
        memset(&settlement, 0, sizeof(settlement));
        settlement.schema_version =
            VCS_ZCODE_PATRONAGE_SETTLEMENT_VERSION;
        settlement.action = VCS_ZCODE_PATRONAGE_SIMULATED_SETTLED;
        settlement.flags = VCS_ZCODE_PATRONAGE_SETTLEMENT_SIMULATION_ONLY |
            VCS_ZCODE_PATRONAGE_SETTLEMENT_NO_LIVE_FUNDS |
            VCS_ZCODE_PATRONAGE_SETTLEMENT_NO_TRANSACTION_BYTES;
        memcpy(settlement.network_genesis_root, network, 32);
        memcpy(settlement.patronage_intent_root, intent_root, 32);
        memcpy(settlement.patronage_funding_root, funding_root, 32);
        memcpy(settlement.creation_attribution_root, attribution_root, 32);
        memcpy(settlement.task_root, task_root, 32);
        memcpy(settlement.candidate_root, candidate_root, 32);
        memcpy(settlement.proof_policy_root, proof_policy_root, 32);
        memcpy(settlement.proof_set_root, proof_set_root, 32);
        memcpy(settlement.proven_lane_root, lane_root, 32);
        memcpy(settlement.score_receipt_root, score_root, 32);
        memcpy(settlement.recipient_contributor_binding_root,
               binding_root, 32);
        memcpy(settlement.settler_zid_pubkey, zid_pubkey, 32);
        settlement.amount_atoms = intent.amount_atoms;
        settlement.created_unix = 700000;
        settlement.observed_height = 9000;
        settlement.observed_mtp = 700000;
        settlement.sequence = 1;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        struct vcs_zcode_patronage_validation_context patronage_context = {
            .workspace = workspace,
            .expected_network_genesis_root = network,
            .now_unix = 700000,
        };
        struct vcs_zcode_continuity_policy_v1 continuity;
        memset(&continuity, 0, sizeof(continuity));
        continuity.schema_version = VCS_ZCODE_CONTINUITY_POLICY_VERSION;
        continuity.event_mask = VCS_ZCODE_CONTINUITY_BORN_RED_FIX |
            VCS_ZCODE_CONTINUITY_SECURITY_FIX |
            VCS_ZCODE_CONTINUITY_INDEPENDENT_REPRODUCTION |
            VCS_ZCODE_CONTINUITY_COMPATIBILITY |
            VCS_ZCODE_CONTINUITY_PRESERVATION;
        continuity.flags = VCS_ZCODE_CONTINUITY_NO_AUTHORITY |
                           VCS_ZCODE_CONTINUITY_SIMULATION_ONLY;
        memcpy(continuity.network_genesis_root, network, 32);
        score_fill(continuity.zc23_token_or_simulation_root, 0xe1);
        memcpy(continuity.patron_contributor_binding_root, binding_root, 32);
        memcpy(continuity.patron_zid_pubkey, zid_pubkey, 32);
        memcpy(continuity.package_root, package_root, 32);
        memcpy(continuity.current_release_root, release_root, 32);
        score_fill(continuity.from_capsule_root, 0xe2);
        memcpy(continuity.to_capsule_root, capsule, 32);
        memcpy(continuity.proof_policy_root, proof_policy_root, 32);
        continuity.maximum_cycles = 3;
        continuity.per_cycle_cap_atoms = UINT64_C(250000000);
        continuity.total_cap_atoms = UINT64_C(750000000);
        continuity.created_unix = 1000;
        continuity.expires_unix = 800000;
        continuity.sequence = 1;
        ASSERT_EQ(vcs_zcode_continuity_policy_seal(
                      &continuity, zid_secret, zid_pubkey),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_verify_cas(
                      &continuity, &patronage_context),
                  VCS_ZCODE_CONTINUITY_OK);
        struct vcs_zcode_creation_attribution_v1 continuity_attribution =
            attribution;
        continuity_attribution.category = VCS_ZCODE_CREATION_COMPATIBILITY;
        continuity_attribution.lineage_kind =
            VCS_ZCODE_CREATION_LINEAGE_CONTINUITY_POLICY;
        uint8_t continuity_policy_root[32], continuity_event_key[32];
        ASSERT_EQ(vcs_zcode_continuity_policy_root(
                      &continuity, continuity_policy_root),
                  VCS_ZCODE_CONTINUITY_OK);
        memcpy(continuity_attribution.lineage_root,
               continuity_policy_root, 32);
        ASSERT_EQ(vcs_zcode_continuity_event_key(
                      &continuity_attribution, &continuity, &task, &score,
                      continuity_event_key),
                  VCS_ZCODE_CONTINUITY_OK);
        char continuity_event_key_hex[65];
        zcl_hex_encode(continuity_event_key, 32, continuity_event_key_hex);
        ASSERT_STR_EQ(continuity_event_key_hex,
            "aa520e9943150bf70bdda88fe56f94bdc276eebc6ee90a0f14b93678add2e50c");
        uint8_t continuity_wire[VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES];
        uint8_t continuity_root[32];
        char continuity_hex[sizeof(continuity_wire) * 2u + 1u];
        char continuity_root_hex[65];
        ASSERT_EQ(vcs_zcode_continuity_policy_serialize(
                      &continuity, continuity_wire),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_root(
                      &continuity, continuity_root),
                  VCS_ZCODE_CONTINUITY_OK);
        zcl_hex_encode(continuity_wire, sizeof(continuity_wire),
                       continuity_hex);
        zcl_hex_encode(continuity_root, sizeof(continuity_root),
                       continuity_root_hex);
        struct json_value continuity_input;
        json_init(&continuity_input); json_set_object(&continuity_input);
        ASSERT(json_push_kv_str(&continuity_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&continuity_input, "policy_hex",
                                continuity_hex));
        ASSERT(json_push_kv_str(&continuity_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&continuity_input, "now_unix", 700000));
        struct zcl_command_request continuity_request = {
            .input = &continuity_input,
        };
        struct zcl_command_reply continuity_reply;
        zcl_command_reply_init(&continuity_reply,
                               "zcl.test.continuity.v1");
        zcl_native_handle_zcode_continuity_plan(
            &continuity_request, &continuity_reply);
        ASSERT_EQ(continuity_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(!json_get_bool(json_get(&continuity_reply.data, "persisted")));
        ASSERT(json_get_bool(json_get(&continuity_reply.data,
                                      "simulation_only")));
        ASSERT(json_get_bool(json_get(&continuity_reply.data,
                                      "no_authority")));
        ASSERT(!json_get_bool(json_get(&continuity_reply.data, "funded")));
        ASSERT(!json_get_bool(json_get(&continuity_reply.data,
                                       "guaranteed_income")));
        zcl_command_reply_free(&continuity_reply);

        ASSERT(json_push_kv_bool(&continuity_input, "unknown", true));
        zcl_command_reply_init(&continuity_reply,
                               "zcl.test.continuity.v1");
        zcl_native_handle_zcode_continuity_plan(
            &continuity_request, &continuity_reply);
        ASSERT_EQ(continuity_reply.exit_code, ZCL_COMMAND_EXIT_INVALID);
        zcl_command_reply_free(&continuity_reply);
        json_free(&continuity_input);

        json_init(&continuity_input); json_set_object(&continuity_input);
        ASSERT(json_push_kv_str(&continuity_input, "workspace", workspace));
        ASSERT(json_push_kv_str(&continuity_input, "policy_hex",
                                continuity_hex));
        ASSERT(json_push_kv_str(&continuity_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&continuity_input, "now_unix", 700000));
        continuity_request.input = &continuity_input;
        zcl_command_reply_init(&continuity_reply,
                               "zcl.test.continuity.v1");
        zcl_native_handle_zcode_continuity_commit(
            &continuity_request, &continuity_reply);
        ASSERT_EQ(continuity_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(json_get_bool(json_get(&continuity_reply.data, "persisted")));
        ASSERT(!json_get_bool(json_get(&continuity_reply.data,
                                       "moves_live_funds")));
        zcl_command_reply_free(&continuity_reply);
        json_free(&continuity_input);

        struct json_value continuity_status_input;
        json_init(&continuity_status_input);
        json_set_object(&continuity_status_input);
        ASSERT(json_push_kv_str(&continuity_status_input, "workspace",
                                workspace));
        ASSERT(json_push_kv_str(&continuity_status_input, "root",
                                continuity_root_hex));
        ASSERT(json_push_kv_str(&continuity_status_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&continuity_status_input, "now_unix",
                                700000));
        struct zcl_command_request continuity_status_request = {
            .input = &continuity_status_input,
        };
        zcl_command_reply_init(&continuity_reply,
                               "zcl.test.continuity.v1");
        zcl_native_handle_zcode_continuity_status(
            &continuity_status_request, &continuity_reply);
        ASSERT_EQ(continuity_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&continuity_reply.data, "object")),
                      "continuity_policy");
        ASSERT_STR_EQ(json_get_str(json_get(&continuity_reply.data,
                                            "continuity_policy_root")),
                      continuity_root_hex);
        ASSERT_EQ(json_get_int(json_get(&continuity_reply.data,
                                        "maximum_cycles")), 3);
        ASSERT(!json_get_bool(json_get(&continuity_reply.data, "funded")));
        zcl_command_reply_free(&continuity_reply);
        json_free(&continuity_status_input);

        struct json_value patronage_list_input;
        json_init(&patronage_list_input);
        json_set_object(&patronage_list_input);
        ASSERT(json_push_kv_str(&patronage_list_input, "workspace",
                                workspace));
        ASSERT(json_push_kv_str(&patronage_list_input,
                                "expected_network_genesis_root",
                                network_hex));
        ASSERT(json_push_kv_int(&patronage_list_input, "now_unix", 700000));
        struct zcl_command_request patronage_list_request = {
            .input = &patronage_list_input,
        };
        zcl_command_reply_init(&continuity_reply,
                               "zcl.test.patronage_list.v1");
        zcl_native_handle_zcode_patronage_list(
            &patronage_list_request, &continuity_reply);
        ASSERT_EQ(continuity_reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(json_get_int(json_get(&continuity_reply.data, "count")), 3);
        ASSERT_EQ(json_get_int(json_get(&continuity_reply.data,
                                        "offer_count")), 1);
        ASSERT_EQ(json_get_int(json_get(&continuity_reply.data,
                                        "simulated_funding_count")), 1);
        ASSERT_EQ(json_get_int(json_get(&continuity_reply.data,
                                        "continuity_policy_count")), 1);
        ASSERT(!json_get_bool(json_get(&continuity_reply.data, "funded")));
        ASSERT(!json_get_bool(json_get(&continuity_reply.data, "persisted")));
        zcl_command_reply_free(&continuity_reply);
        json_free(&patronage_list_input);
        struct vcs_zcode_patronage_projection *first_patronage =
            vcs_zcode_patronage_projection_build(&patronage_context);
        struct vcs_zcode_patronage_projection *second_patronage =
            vcs_zcode_patronage_projection_build(&patronage_context);
        uint8_t first_patronage_root[32], second_patronage_root[32];
        ASSERT(first_patronage && second_patronage);
        ASSERT(vcs_zcode_patronage_projection_root(
            first_patronage, first_patronage_root));
        ASSERT(vcs_zcode_patronage_projection_root(
            second_patronage, second_patronage_root));
        ASSERT(memcmp(first_patronage_root, second_patronage_root, 32) == 0);
        vcs_zcode_patronage_projection_free(second_patronage);
        vcs_zcode_patronage_projection_free(first_patronage);

        context.continuity_is_duplicate =
            creation_test_continuity_duplicate;
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &continuity_attribution, &context),
                  VCS_ZCODE_CREATION_OK);
        context.continuity_is_duplicate = NULL;
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &continuity_attribution, &context),
                  VCS_ZCODE_CREATION_CONTINUITY);
        context.continuity_is_duplicate =
            creation_test_continuity_duplicate;
        callbacks.continuity_duplicate = true;
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &continuity_attribution, &context),
                  VCS_ZCODE_CREATION_DUPLICATE);
        callbacks.continuity_duplicate = false;

        struct vcs_zcode_creation_attribution_v1 born_red_attribution =
            continuity_attribution;
        struct vcs_zcode_creation_attribution_v1 security_attribution =
            continuity_attribution;
        born_red_attribution.category = VCS_ZCODE_CREATION_BORN_RED_FIX;
        security_attribution.category = VCS_ZCODE_CREATION_SECURITY_FIX;
        uint8_t born_red_event_key[32], security_event_key[32];
        ASSERT_EQ(vcs_zcode_continuity_event_key(
                      &born_red_attribution, &continuity, &task, &score,
                      born_red_event_key),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_event_key(
                      &security_attribution, &continuity, &task, &score,
                      security_event_key),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT(memcmp(born_red_event_key, security_event_key, 32) == 0);

        struct vcs_zcode_continuity_policy_v1 mismatched_continuity =
            continuity;
        score_fill(mismatched_continuity.to_capsule_root, 0xe3);
        ASSERT_EQ(vcs_zcode_continuity_policy_seal(
                      &mismatched_continuity, zid_secret, zid_pubkey),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_root(
                      &mismatched_continuity,
                      continuity_attribution.lineage_root),
                  VCS_ZCODE_CONTINUITY_OK);
        uint8_t zero_event_key[32];
        memset(zero_event_key, 0, sizeof(zero_event_key));
        memset(continuity_event_key, 0xa5, sizeof(continuity_event_key));
        ASSERT_EQ(vcs_zcode_continuity_event_key(
                      &continuity_attribution, &mismatched_continuity,
                      &task, &score, continuity_event_key),
                  VCS_ZCODE_CONTINUITY_TRANSITION);
        ASSERT(memcmp(continuity_event_key, zero_event_key,
                      sizeof(zero_event_key)) == 0);

        memcpy(continuity.patron_contributor_binding_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_continuity_policy_seal(
                      &continuity, zid_secret, zid_pubkey),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_verify_cas(
                      &continuity, &patronage_context),
                  VCS_ZCODE_CONTINUITY_CONTRIBUTOR);
        memcpy(continuity.patron_contributor_binding_root, binding_root, 32);
        memcpy(continuity.package_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_continuity_policy_seal(
                      &continuity, zid_secret, zid_pubkey),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_verify_cas(
                      &continuity, &patronage_context),
                  VCS_ZCODE_CONTINUITY_PACKAGE);
        memcpy(continuity.package_root, package_root, 32);
        memcpy(continuity.current_release_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_continuity_policy_seal(
                      &continuity, zid_secret, zid_pubkey),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_verify_cas(
                      &continuity, &patronage_context),
                  VCS_ZCODE_CONTINUITY_RELEASE);
        memcpy(continuity.current_release_root, release_root, 32);
        memcpy(continuity.proof_policy_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_continuity_policy_seal(
                      &continuity, zid_secret, zid_pubkey),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_verify_cas(
                      &continuity, &patronage_context),
                  VCS_ZCODE_CONTINUITY_PROOF_POLICY);
        memcpy(continuity.proof_policy_root, proof_policy_root, 32);
        score_fill(continuity.network_genesis_root, 0xe4);
        ASSERT_EQ(vcs_zcode_continuity_policy_seal(
                      &continuity, zid_secret, zid_pubkey),
                  VCS_ZCODE_CONTINUITY_OK);
        ASSERT_EQ(vcs_zcode_continuity_policy_verify_cas(
                      &continuity, &patronage_context),
                  VCS_ZCODE_CONTINUITY_NETWORK);
        memcpy(continuity.network_genesis_root, network, 32);
        struct vcs_zcode_patronage_settlement_validation_context
            settlement_context = {
                .patronage = &patronage_context,
                .creation = &context,
                .active_height = 9000,
                .active_mtp = 700000,
                .now_unix = 700000,
            };
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);

        memcpy(settlement.task_root, candidate_root, 32);
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_EVIDENCE);
        memcpy(settlement.task_root, task_root, 32);
        callbacks.anchor_active = false;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_EVIDENCE);
        callbacks.anchor_active = true;
        settlement.observed_height =
            attribution.challenge_maturity_height - 1;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_EVIDENCE);

        settlement.action = VCS_ZCODE_PATRONAGE_SIMULATED_REFUNDED;
        memset(settlement.creation_attribution_root, 0, 32);
        memset(settlement.task_root, 0, 32);
        memset(settlement.candidate_root, 0, 32);
        memset(settlement.proof_policy_root, 0, 32);
        memset(settlement.proof_set_root, 0, 32);
        memset(settlement.proven_lane_root, 0, 32);
        memset(settlement.score_receipt_root, 0, 32);
        settlement.created_unix = intent.expires_unix - 1;
        settlement.observed_height = intent.refund_height - 1;
        settlement.observed_mtp = intent.expires_unix - 1;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_TIME);
        settlement.created_unix = intent.refund_unix;
        settlement.observed_height = intent.refund_height;
        settlement.observed_mtp = intent.refund_unix;
        settlement_context.active_height = intent.refund_height;
        settlement_context.active_mtp = intent.refund_unix;
        settlement_context.now_unix = intent.refund_unix;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        memcpy(settlement.recipient_contributor_binding_root,
               task_root, 32);
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, zid_secret, zid_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_INTENT);

        epoch_set.actual_mint_atoms--;
        epoch_set.unissued_atoms++;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(
                      &epoch_set, &epoch_context),
                  VCS_ZCODE_EPOCH_CREATION_MINT);
        epoch_set.actual_mint_atoms += 2;
        epoch_set.unissued_atoms -= 2;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(
                      &epoch_set, &epoch_context),
                  VCS_ZCODE_EPOCH_CREATION_MINT);
        epoch_context.observed_actual_mint_atoms =
            epoch_set.actual_mint_atoms;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(
                      &epoch_set, &epoch_context),
                  VCS_ZCODE_EPOCH_CREATION_SUM);
        epoch_set.actual_mint_atoms = attribution.award_atoms;
        epoch_set.unissued_atoms = epoch_set.emission_cap_atoms -
                                   epoch_set.actual_mint_atoms;
        epoch_context.observed_actual_mint_atoms =
            epoch_set.actual_mint_atoms;

        context.active_height = attribution.challenge_maturity_height - 1;
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &attribution, &context), VCS_ZCODE_CREATION_IMMATURE);
        context.active_height = 9000;
        callbacks.anchor_active = false;
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &attribution, &context), VCS_ZCODE_CREATION_REORG);
        callbacks.anchor_active = true; callbacks.duplicate = true;
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &attribution, &context), VCS_ZCODE_CREATION_DUPLICATE);
        callbacks.duplicate = false;

        struct vcs_zcode_creation_attribution_v1 substituted = attribution;
        score_fill(substituted.license_evidence_root, 0xb2);
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &substituted, &context), VCS_ZCODE_CREATION_LICENSE);
        substituted = attribution;
        memcpy(substituted.proven_lane_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &substituted, &context), VCS_ZCODE_CREATION_LANE);
        substituted = attribution;
        memcpy(substituted.release_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &substituted, &context), VCS_ZCODE_CREATION_RELEASE);
        substituted = attribution;
        memcpy(substituted.contributor_binding_root, task_root, 32);
        ASSERT_EQ(vcs_zcode_creation_attribution_verify_cas(
                      &substituted, &context),
                  VCS_ZCODE_CREATION_CONTRIBUTOR);

        free(release_wire); free(manifest_wire);
        vcs_package_manifest_free(&manifest);
        test_rm_rf(workspace);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_score_receipt(void)
{
    int failures = test_score_happy_path() + test_score_package_verticals() +
                   test_score_rejections() +
                   test_creation_attribution_cross_validation() +
                   test_patronage_intent_cross_validation();
    printf("=== zcode_score_receipt: %d failures ===\n", failures);
    return failures;
}
