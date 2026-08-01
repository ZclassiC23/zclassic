/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Typed adapters for ZCODE create, use, and immutable improve tasks. */

#include "command/native_command.h"

#include "base/hex.h"
#include "base/serialize_le.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include "models/database.h"
#include "platform/time_compat.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_worker.h"
#include "services/zcode_lane_service.h"
#include "util/safe_alloc.h"
#include "vcs/build_action.h"
#include "vcs/build_artifact_manifest.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_lane.h"
#include "vcs/package_store.h"
#include "vcs/zcode_work_context.h"
#include "vcs/zcode_work_node.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZDEV_PATH_MAX 4096

static const char *zdev_str(const struct json_value *input, const char *key)
{
    const struct json_value *v = input ? json_get(input, key) : NULL;
    return v ? json_get_str(v) : NULL;
}

static int64_t zdev_int(const struct json_value *input, const char *key,
                        int64_t fallback)
{
    const struct json_value *v = input ? json_get(input, key) : NULL;
    return v ? json_get_int(v) : fallback;
}

static void zdev_fail(struct zcl_command_reply *reply, const char *code,
                      const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.improve");
}

static bool zdev_root(const struct json_value *input, const char *key,
                      uint8_t out[32], struct zcl_command_reply *reply)
{
    const char *value = zdev_str(input, key);
    if (value && zcl_hex_decode_lower(value, out, 32)) return true;
    char detail[128];
    (void)snprintf(detail, sizeof(detail), "%s must be 64 lowercase hex",
                   key);
    zdev_fail(reply, "BAD_ROOT", detail);
    return false;
}

static void zdev_push_root(struct json_value *out, const char *key,
                           const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(out, key, hex);
}

static uint8_t *zdev_read_file(const char *path, size_t *len_out)
{
    *len_out = 0;
    struct stat st;
    if (!path || path[0] != '/' || stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > VCS_BUILD_ARTIFACT_MAX_BYTES)
        return NULL;
    size_t len = (size_t)st.st_size;
    uint8_t *bytes = zcl_malloc(len, "zcode.improve.fixed_input");
    if (!bytes) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { free(bytes); return NULL; }
    size_t off = 0;
    while (off < len) {
        ssize_t got = read(fd, bytes + off, len - off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        off += (size_t)got;
    }
    close(fd);
    if (off != len) { free(bytes); return NULL; }
    *len_out = len;
    return bytes;
}

static bool zdev_open_db(const char *datadir, struct node_db *ndb)
{
    char db_path[ZDEV_PATH_MAX];
    int n = datadir
        ? snprintf(db_path, sizeof(db_path), "%s/node.db", datadir) : -1;
    return n > 0 && (size_t)n < sizeof(db_path) &&
           node_db_open(ndb, db_path);
}

static void zdev_push_lane(struct json_value *out,
                           const struct zcode_lane_status *status)
{
    (void)json_push_kv_str(out, "lane", status->lane_name);
    (void)json_push_kv_str(out, "source_root", status->source_root_sha3);
    (void)json_push_kv_str(out, "task_root", status->task_root_sha3);
    (void)json_push_kv_str(out, "candidate_root", status->candidate_root_sha3);
    (void)json_push_kv_str(out, "proof_policy_root",
                           status->proof_policy_root_sha3);
    (void)json_push_kv_str(out, "proof_set_root",
                           status->proof_set_root_sha3);
    (void)json_push_kv_str(out, "lane_receipt_root",
                           status->receipt_root_sha3);
    (void)json_push_kv_str(out, "prior_lane_receipt_root",
                           status->prior_receipt_root_sha3);
    (void)json_push_kv_str(out, "signer_pubkey", status->signer_pubkey);
    (void)json_push_kv_int(out, "created_unix", status->created_at);
}

void zcl_native_handle_zcode_create(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *mode = zdev_str(request->input, "mode");
    if (mode && strcmp(mode, "plan") == 0) {
        zcl_native_handle_zcode_package_publish_plan(request, reply);
        return;
    }
    if (mode && strcmp(mode, "commit") == 0) {
        zcl_native_handle_zcode_package_publish_commit(request, reply);
        return;
    }
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, "BAD_MODE", "normalize",
                           false, false, "mode must be plan or commit",
                           "zcode.create");
}

void zcl_native_handle_zcode_use(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    if (zdev_str(request->input, "plan_id"))
        zcl_native_handle_zcode_package_add_commit(request, reply);
    else
        zcl_native_handle_zcode_package_add_plan(request, reply);
}

void zcl_native_handle_zcode_evidence(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = zdev_str(request->input, "workspace");
    const char *action_id = zdev_str(request->input, "action_id");
    const char *datadir = zdev_str(request->input, "datadir");
    if (!datadir || !datadir[0]) datadir = zcl_native_command_datadir();
    char workspace[ZDEV_PATH_MAX];
    uint8_t action_check[32];
    if (!workspace_arg || !realpath(workspace_arg, workspace) || !datadir ||
        !action_id || !zcl_hex_decode_lower(action_id, action_check, 32)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_EVIDENCE_INPUT", "validate", false, false,
            "workspace must resolve and action_id must be 64 lowercase hex",
            "zcode.evidence");
        return;
    }
    char db_path[ZDEV_PATH_MAX];
    int n = snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb = {0};
    if (n <= 0 || (size_t)n >= sizeof(db_path) ||
        !node_db_open(&ndb, db_path)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
            "DATABASE_OPEN_FAILED", "evaluate", true, false,
            "the ZBuild ledger could not be opened", "zcode.evidence");
        return;
    }
    struct build_fabric_proof_evaluation evaluation;
    struct zcl_result result = build_fabric_proof_evaluate(
        &ndb, workspace, action_id,
        (int64_t)platform_time_wall_unix(), &evaluation);
    node_db_close(&ndb);
    if (!result.ok) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
            ZCL_COMMAND_EXIT_FAILED, "EVIDENCE_EVALUATION_FAILED",
            "evaluate", true, false, result.message, "zcode.evidence");
        return;
    }
    (void)json_push_kv_str(&reply->data, "action_id", action_id);
    (void)json_push_kv_int(&reply->data, "valid_receipts",
                           (int64_t)evaluation.valid_receipts);
    (void)json_push_kv_int(&reply->data, "approved_distinct_signers",
                           (int64_t)evaluation.approved_distinct_signers);
    (void)json_push_kv_int(&reply->data, "matching_receipts",
                           (int64_t)evaluation.matching_receipts);
    (void)json_push_kv_int(&reply->data, "compile_receipts",
                           (int64_t)evaluation.compile_receipts);
    (void)json_push_kv_int(&reply->data, "test_receipts",
                           (int64_t)evaluation.test_receipts);
    (void)json_push_kv_int(&reply->data, "fuzz_receipts",
                           (int64_t)evaluation.fuzz_receipts);
    (void)json_push_kv_int(&reply->data, "review_receipts",
                           (int64_t)evaluation.review_receipts);
    (void)json_push_kv_bool(&reply->data, "local_reproduced",
                            evaluation.local_reproduced);
    (void)json_push_kv_bool(&reply->data, "quorum_satisfied",
                            evaluation.quorum_satisfied);
    (void)json_push_kv_bool(&reply->data, "compile_satisfied",
                            evaluation.compile_satisfied);
    (void)json_push_kv_bool(&reply->data, "test_satisfied",
                            evaluation.test_satisfied);
    (void)json_push_kv_bool(&reply->data, "fuzz_satisfied",
                            evaluation.fuzz_satisfied);
    (void)json_push_kv_bool(&reply->data, "review_satisfied",
                            evaluation.review_satisfied);
    (void)json_push_kv_bool(&reply->data, "release_identity_satisfied",
                            evaluation.release_identity_satisfied);
    (void)json_push_kv_bool(&reply->data, "policy_satisfied",
                            evaluation.policy_satisfied);
    (void)json_push_kv_str(&reply->data, "output_root",
                           evaluation.output_root_sha3);
    (void)json_push_kv_str(&reply->data, "proof_set_root",
                           evaluation.proof_set_root_sha3);
    (void)json_push_kv_str(&reply->data, "authority",
        evaluation.local_reproduced ? "LOCAL_REPRODUCTION" :
        evaluation.quorum_satisfied ? "APPROVED_SIGNER_QUORUM" :
        "UNTRUSTED");
}

void zcl_native_handle_zcode_accept(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = zdev_str(request->input, "workspace");
    const char *action_id = zdev_str(request->input, "action_id");
    const char *lane = zdev_str(request->input, "lane");
    const char *datadir = zdev_str(request->input, "datadir");
    if (!datadir || !datadir[0]) datadir = zcl_native_command_datadir();
    int target = lane && strcmp(lane, "CANDIDATE") == 0
        ? VCS_ZCODE_LANE_CANDIDATE
        : lane && strcmp(lane, "PROVEN") == 0
            ? VCS_ZCODE_LANE_PROVEN : 0;
    char workspace[ZDEV_PATH_MAX];
    uint8_t action_root[32];
    if (!workspace_arg || !realpath(workspace_arg, workspace) ||
        !action_id || !zcl_hex_decode_lower(action_id, action_root, 32) ||
        !target) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_ACCEPT_INPUT", "validate", false, false,
            "workspace and action_id are required; lane must be CANDIDATE or PROVEN",
            "zcode.accept");
        return;
    }
    struct node_db ndb = {0};
    if (!zdev_open_db(datadir, &ndb)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
            "DATABASE_OPEN_FAILED", "accept", true, false,
            "the ZBuild ledger could not be opened", "zcode.accept");
        return;
    }
    struct db_build_worker signer;
    uint8_t secret[32], pubkey[32];
    struct zcl_result identity = build_fabric_worker_identity_load(
        datadir, &signer, secret, pubkey);
    struct zcode_lane_status status;
    struct zcl_result accepted = identity.ok
        ? zcode_lane_advance(&ndb, workspace, action_id, target,
              (int64_t)platform_time_wall_unix(), secret, pubkey, &status)
        : identity;
    memset(secret, 0, sizeof(secret));
    node_db_close(&ndb);
    if (!accepted.ok) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
            "LANE_PROMOTION_REFUSED", "accept", false, false,
            accepted.message, "zcode.accept");
        return;
    }
    zdev_push_lane(&reply->data, &status);
    (void)json_push_kv_str(&reply->data, "authority",
                           "OPERATOR_SIGNED_PROOF_POLICY");
}

void zcl_native_handle_zcode_lane(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = zdev_str(request->input, "workspace");
    const char *source_root = zdev_str(request->input, "source_root");
    const char *datadir = zdev_str(request->input, "datadir");
    if (!datadir || !datadir[0]) datadir = zcl_native_command_datadir();
    char workspace[ZDEV_PATH_MAX];
    uint8_t root[32];
    if (!workspace_arg || !realpath(workspace_arg, workspace) ||
        !source_root || !zcl_hex_decode_lower(source_root, root, 32)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_LANE_INPUT", "validate", false, false,
            "workspace and a 64-hex source_root are required", "zcode.lane");
        return;
    }
    sqlite3 *db = NULL;
    struct node_db ndb = {0};
    if (!zcl_native_node_db_require_readonly(
            datadir, reply, "the ZCODE lane ledger", &db, &ndb))
        return;
    struct zcode_lane_status status;
    struct zcl_result found = zcode_lane_find(
        &ndb, workspace, source_root, &status);
    zcl_native_node_db_close_readonly(&db, &ndb);
    if (!found.ok) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
            "LANE_NOT_FOUND", "lookup", false, false,
            found.message, "zcode.lane");
        return;
    }
    zdev_push_lane(&reply->data, &status);
    (void)json_push_kv_str(&reply->data, "authority",
                           "SIGNED_CAS_RECEIPT");
}

// long-function-ok:one-task-admission — canonical objects, CAS writes, and
// the ZBuild ledger commit form one fail-closed local admission transaction;
// no candidate or proof is claimed by this planning command.
void zcl_native_handle_zcode_improve(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = zdev_str(request->input, "workspace");
    const char *datadir = zdev_str(request->input, "datadir");
    if (!datadir || !datadir[0]) datadir = zcl_native_command_datadir();
    const char *goal = zdev_str(request->input, "goal");
    const char *policy_hex = zdev_str(request->input, "proof_policy_hex");
    const char *action_kind = zdev_str(request->input, "action_kind");
    if (!action_kind || !action_kind[0])
        action_kind = VCS_BUILD_ACTION_KIND_V1;
    uint8_t work_kind = vcs_build_action_v1_work_kind(action_kind);
    if (work_kind != VCS_ZCODE_WORK_BUILD &&
        work_kind != VCS_ZCODE_WORK_TEST) {
        zdev_fail(reply, "BAD_ACTION_KIND",
                  "action_kind must name the fixed compile or test executor");
        return;
    }
    const char *fixed_input = zdev_str(request->input, "fixed_input_path");
    if (!fixed_input)
        fixed_input = zdev_str(request->input, "preprocessed_path");
    if (!workspace_arg || !datadir || !goal || !goal[0] ||
        strlen(goal) > 4096 || !policy_hex || !fixed_input) {
        zdev_fail(reply, "MISSING_INPUT",
                  "workspace, datadir, goal, proof policy, and fixed input path are required");
        return;
    }
    char workspace[ZDEV_PATH_MAX];
    if (!realpath(workspace_arg, workspace)) {
        zdev_fail(reply, "BAD_WORKSPACE", "workspace must resolve to an existing directory");
        return;
    }
    struct vcs_zcode_proof_policy_v1 policy;
    uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
    if (strlen(policy_hex) != sizeof(policy_wire) * 2u ||
        !zcl_hex_decode_lower(policy_hex, policy_wire, sizeof(policy_wire)) ||
        vcs_zcode_proof_policy_parse(policy_wire, sizeof(policy_wire),
                                     &policy) != VCS_ZCODE_DEV_OK ||
        !(policy.required_proofs & VCS_ZCODE_PROOF_COMPILE) ||
        (work_kind == VCS_ZCODE_WORK_TEST &&
         !(policy.required_proofs & VCS_ZCODE_PROOF_TEST))) {
        zdev_fail(reply, "BAD_PROOF_POLICY",
                  "proof_policy_hex must require compile and the requested proof kind");
        return;
    }
    uint8_t policy_root[32];
    if (vcs_zcode_proof_policy_root(&policy, policy_root) !=
        VCS_ZCODE_DEV_OK) {
        zdev_fail(reply, "BAD_PROOF_POLICY", "proof policy root refused");
        return;
    }
    struct vcs_zcode_task_v1 task = { .schema_version = VCS_ZCODE_DEV_VERSION };
    if (!zdev_root(request->input, "source_root", task.source_root, reply) ||
        !zdev_root(request->input, "dependency_lock_root",
                   task.dependency_lock_root, reply) ||
        !zdev_root(request->input, "write_scope_root", task.write_scope_root,
                   reply) ||
        !zdev_root(request->input, "acceptance_tests_root",
                   task.acceptance_tests_root, reply) ||
        !zdev_root(request->input, "model_policy_root",
                   task.model_policy_root, reply))
        return;
    memcpy(task.proof_policy_root, policy_root, 32);
    sha3_256((const uint8_t *)goal, strlen(goal), task.goal_root);
    struct vcs_toolchain_capsule_v1 capsule;
    if (!vcs_toolchain_capsule_v1_capture_gcc(&capsule) ||
        !vcs_toolchain_capsule_v1_root(&capsule,
                                       task.toolchain_capsule_root)) {
        zdev_fail(reply, "TOOLCHAIN_CAPTURE_FAILED",
                  "the fixed GCC toolchain capsule could not be captured");
        return;
    }
    task.capabilities = VCS_ZCODE_TASK_CAP_V1_MASK;
    task.max_changed_files = (uint32_t)zdev_int(
        request->input, "max_changed_files", 64);
    task.max_patch_bytes = (uint64_t)zdev_int(
        request->input, "max_patch_bytes", 16 * 1024 * 1024);
    task.max_context_bytes = (uint64_t)zdev_int(
        request->input, "max_context_bytes", 16 * 1024 * 1024);
    task.max_cpu_seconds = (uint32_t)zdev_int(
        request->input, "max_cpu_seconds", 600);
    task.max_memory_bytes = (uint64_t)zdev_int(
        request->input, "max_memory_bytes", UINT64_C(2048) * 1024u * 1024u);
    task.max_output_bytes = (uint64_t)zdev_int(
        request->input, "max_output_bytes", VCS_BUILD_ARTIFACT_MAX_BYTES);
    task.expires_unix = zdev_int(request->input, "expires_unix", 0);
    int64_t now = (int64_t)platform_time_wall_unix();
    if (vcs_zcode_task_validate_at(&task, now) != VCS_ZCODE_DEV_OK) {
        zdev_fail(reply, "TASK_INVALID", "task limits, capabilities, roots, or expiry are invalid");
        return;
    }
    size_t input_len = 0;
    uint8_t *input = zdev_read_file(fixed_input, &input_len);
    if (!input) {
        zdev_fail(reply, "BAD_FIXED_INPUT",
                  "fixed_input_path must be an absolute bounded regular file");
        return;
    }
    uint8_t input_root[32];
    sha3_256(input, input_len, input_root);
    uint8_t task_wire[VCS_ZCODE_TASK_WIRE_BYTES], task_root[32];
    if (vcs_zcode_task_serialize(&task, task_wire) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_task_root(&task, task_root) != VCS_ZCODE_DEV_OK) {
        free(input);
        zdev_fail(reply, "TASK_INVALID", "canonical task serialization failed");
        return;
    }
    struct vcs_zcode_candidate_v1 candidate = {
        .schema_version = VCS_ZCODE_DEV_VERSION,
        .sequence = (uint64_t)zdev_int(request->input, "candidate_sequence", 1),
        .created_unix = zdev_int(
            request->input, "candidate_created_unix", now),
    };
    memcpy(candidate.task_root, task_root, 32);
    memcpy(candidate.base_source_root, task.source_root, 32);
    if (!zdev_root(request->input, "patch_root", candidate.patch_root, reply) ||
        !zdev_root(request->input, "candidate_source_root",
                   candidate.candidate_source_root, reply) ||
        !zdev_root(request->input, "adapter_policy_root",
                   candidate.adapter_policy_root, reply) ||
        !zdev_root(request->input, "author_pubkey", candidate.author_pubkey,
                   reply)) {
        free(input);
        return;
    }
    uint8_t candidate_wire[VCS_ZCODE_CANDIDATE_WIRE_BYTES];
    uint8_t candidate_root[32];
    if (vcs_zcode_candidate_validate_for_task(&task, &candidate, now) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_serialize(&candidate, candidate_wire) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_root(&candidate, candidate_root) !=
            VCS_ZCODE_DEV_OK ||
        !vcs_object_store_init(workspace) ||
        !vcs_object_put_addressed(workspace, policy_root, policy_wire,
                                  sizeof(policy_wire)) ||
        !vcs_object_put_addressed(workspace, task.goal_root,
                                  (const uint8_t *)goal, strlen(goal)) ||
        !vcs_object_put_addressed(workspace, input_root, input, input_len) ||
        !vcs_object_put_addressed(workspace, task_root, task_wire,
                                  sizeof(task_wire)) ||
        !vcs_object_put_addressed(workspace, candidate_root, candidate_wire,
                                  sizeof(candidate_wire))) {
        free(input);
        zdev_fail(reply, "CAS_WRITE_FAILED", "canonical task inputs could not be stored atomically");
        return;
    }
    free(input);

    const char *source_sha256 =
        zdev_str(request->input, "candidate_source_sha256");
    uint8_t source_sha_check[32];
    if (!source_sha256 ||
        !zcl_hex_decode_lower(source_sha256, source_sha_check, 32)) {
        zdev_fail(reply, "BAD_SOURCE_SHA256",
                  "candidate_source_sha256 must be 64 lowercase hex");
        return;
    }
    struct db_build_job job = {0};
    struct db_build_action action = {0};
    int64_t remote_peer = zdev_int(request->input, "remote_peer", 0);
    uint8_t context_root[32] = {0};
    uint8_t context_action_root[32] = {0};
    bool remote_requested = remote_peer > 0;
    bool context_ready = false;
    const char *context_reason = "package store unavailable";
    (void)snprintf(job.source_sha256, sizeof(job.source_sha256), "%s",
                   source_sha256);
    zcl_hex_encode(candidate.candidate_source_root, 32, job.source_cas_sha3);
    zcl_hex_encode(task.toolchain_capsule_root, 32, job.toolchain_sha3);
    const char *profile = zdev_str(request->input, "profile");
    (void)snprintf(job.profile, sizeof(job.profile), "%s",
                   profile && profile[0] ? profile : "dev");
    (void)snprintf(job.state, sizeof(job.state), "PLANNED");
    job.created_at = job.updated_at = now;
    action.sequence = 0;
    (void)snprintf(action.kind, sizeof(action.kind), "%s",
                   action_kind);
    (void)snprintf(action.state, sizeof(action.state), "SNAPSHOTTED");
    zcl_hex_encode(input_root, 32, action.input_root_sha3);
    zcl_hex_encode(task_root, 32, action.task_root_sha3);
    zcl_hex_encode(candidate_root, 32, action.candidate_root_sha3);
    zcl_hex_encode(policy_root, 32, action.proof_policy_root_sha3);
    (void)snprintf(action.target, sizeof(action.target), "%s",
                   VCS_BUILD_TARGET_V1);
    uint8_t fixed_flags[32], fixed_environment[32];
    const char *workdir = NULL, *output = NULL, *resource = NULL;
    if (!vcs_build_action_v1_descriptors(
            action_kind, &workdir, &output, &resource) ||
        !vcs_build_action_v1_fixed_flags_root_for_kind(
            action_kind, fixed_flags) ||
        !vcs_build_action_v1_fixed_environment_root_for_kind(
            action_kind, fixed_environment)) {
        zdev_fail(reply, "ACTION_DESCRIPTOR_FAILED",
                  "fixed action descriptor is unavailable");
        return;
    }
    zcl_hex_encode(fixed_flags, 32, action.flags_sha3);
    zcl_hex_encode(fixed_environment, 32, action.environment_sha3);
    (void)snprintf(action.virtual_workdir, sizeof(action.virtual_workdir),
                   "%s", workdir);
    (void)snprintf(action.declared_outputs, sizeof(action.declared_outputs),
                   "%s", output);
    (void)snprintf(action.resource_policy, sizeof(action.resource_policy),
                   "%s", resource);
    action.created_at = action.updated_at = now;
    if (remote_requested) {
        struct vcs_package_store *context_store = vcs_package_store_global();
        uint8_t *context_input = NULL;
        size_t context_input_len = 0;
        if (context_store &&
            vcs_object_load_raw(workspace, input_root, &context_input,
                                &context_input_len) == 0) {
            struct vcs_zcode_work_context_v1 context;
            vcs_zcode_work_context_init(&context);
            memcpy(context.source_sha256, source_sha_check, 32);
            (void)snprintf(context.profile, sizeof(context.profile), "%s",
                           job.profile);
            context.task = task;
            context.candidate = candidate;
            context.proof_policy = policy;
            context.fixed_input = context_input;
            context.fixed_input_len = context_input_len;
            enum vcs_zcode_work_context_result packed =
                vcs_zcode_work_context_put_for_kind(
                    context_store, &context, action_kind, now,
                    context_root, context_action_root);
            context.fixed_input = NULL;
            vcs_zcode_work_context_free(&context);
            free(context_input);
            if (packed == VCS_ZCODE_WORK_CONTEXT_OK) {
                context_ready = true;
                zcl_hex_encode(context_root, 32,
                               action.context_root_sha3);
            } else {
                context_reason = vcs_zcode_work_context_result_string(packed);
            }
        } else if (context_store) {
            free(context_input);
            context_reason = "local input CAS could not be read";
        }
    }
    if (!build_fabric_action_id(&job, &action, action.action_id).ok ||
        !build_fabric_job_id(&job, action.action_id, job.job_id).ok) {
        zdev_fail(reply, "ACTION_ID_FAILED", "fixed build action identity refused");
        return;
    }
    uint8_t planned_action_root[32];
    if (context_ready &&
        (!zcl_hex_decode_lower(action.action_id, planned_action_root, 32) ||
         memcmp(planned_action_root, context_action_root, 32) != 0)) {
        context_ready = false;
        context_reason = "context action identity mismatch";
        action.context_root_sha3[0] = '\0';
    }
    (void)snprintf(action.job_id, sizeof(action.job_id), "%s", job.job_id);
    char db_path[ZDEV_PATH_MAX];
    int dbn = snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb = {0};
    if (dbn <= 0 || (size_t)dbn >= sizeof(db_path) ||
        !node_db_open(&ndb, db_path)) {
        zdev_fail(reply, "DATABASE_OPEN_FAILED", "cannot open the task's ZBuild ledger");
        return;
    }
    struct zcl_result planned = build_fabric_plan(&ndb, &job, &action);
    struct zcode_lane_status frontier_status = {0};
    struct db_build_worker frontier_signer;
    uint8_t frontier_secret[32] = {0}, frontier_pubkey[32] = {0};
    struct zcl_result admitted = planned;
    if (planned.ok) {
        admitted = build_fabric_worker_identity_load(
            datadir, &frontier_signer, frontier_secret, frontier_pubkey);
        if (admitted.ok)
            admitted = zcode_lane_advance(
                &ndb, workspace, action.action_id, VCS_ZCODE_LANE_FRONTIER,
                now, frontier_secret, frontier_pubkey, &frontier_status);
    }
    memset(frontier_secret, 0, sizeof(frontier_secret));
    struct zcl_result submitted = admitted.ok
        ? build_fabric_submit(&ndb, job.job_id, now) : admitted;
    node_db_close(&ndb);
    if (!planned.ok || !admitted.ok || !submitted.ok) {
        const char *code = !planned.ok ? "ZBUILD_PLAN_FAILED" :
            !admitted.ok ? "FRONTIER_ADMISSION_FAILED" :
            "ZBUILD_SUBMIT_FAILED";
        zdev_fail(reply, code,
                  !planned.ok ? planned.message :
                  !admitted.ok ? admitted.message : submitted.message);
        return;
    }
    zdev_push_root(&reply->data, "task_root", task_root);
    zdev_push_root(&reply->data, "candidate_root", candidate_root);
    zdev_push_root(&reply->data, "proof_policy_root", policy_root);
    zdev_push_root(&reply->data, "toolchain_capsule_root",
                   task.toolchain_capsule_root);
    zdev_push_root(&reply->data, "input_root", input_root);
    (void)json_push_kv_str(&reply->data, "job_id", job.job_id);
    (void)json_push_kv_str(&reply->data, "action_id", action.action_id);
    (void)json_push_kv_str(&reply->data, "action_kind", action_kind);
    (void)json_push_kv_int(&reply->data, "candidate_created_unix",
                           candidate.created_unix);
    (void)json_push_kv_str(&reply->data, "state", "QUEUED");
    (void)json_push_kv_str(&reply->data, "lane", frontier_status.lane_name);
    (void)json_push_kv_str(&reply->data, "lane_receipt_root",
                           frontier_status.receipt_root_sha3);
    if (remote_requested) {
        const char *remote_outcome = "LOCAL_FALLBACK";
        struct vcs_zcode_work_node *work = vcs_zcode_work_node_global();
        struct vcs_package_store *store = vcs_package_store_global();
        struct vcs_package_store_status package_status;
        struct vcs_zcode_work_capability_v1 capability;
        if (context_ready && work && store &&
            vcs_package_store_package_status(store, context_root,
                                              &package_status) &&
            package_status.complete &&
            vcs_zcode_work_node_peer_capability(
                work, (uint64_t)remote_peer, now, &capability)) {
            struct vcs_zcode_work_request_v1 remote = {
                .target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3,
                .work_kind = work_kind,
            };
            memcpy(remote.task_root, task_root, 32);
            memcpy(remote.candidate_root, candidate_root, 32);
            (void)zcl_hex_decode_lower(action.action_id,
                                       remote.action_root, 32);
            memcpy(remote.input_root, input_root, 32);
            memcpy(remote.context_root, context_root, 32);
            memcpy(remote.proof_policy_root, policy_root, 32);
            memcpy(remote.toolchain_capsule_root,
                   task.toolchain_capsule_root, 32);
            remote.max_cpu_seconds = task.max_cpu_seconds <
                    capability.max_cpu_seconds
                ? task.max_cpu_seconds : capability.max_cpu_seconds;
            remote.max_memory_bytes = task.max_memory_bytes <
                    capability.max_memory_bytes
                ? task.max_memory_bytes : capability.max_memory_bytes;
            remote.max_output_bytes = task.max_output_bytes <
                    capability.max_output_bytes
                ? task.max_output_bytes : capability.max_output_bytes;
            int64_t lease_end = now + capability.max_lease_seconds;
            remote.deadline_unix = lease_end < task.expires_unix
                ? lease_end : task.expires_unix - 1;
            struct sha3_256_ctx request_sha;
            uint8_t request_digest[32];
            sha3_256_init(&request_sha);
            static const char request_domain[] =
                "zcl.zcode.local_request_id.v1";
            sha3_256_write(&request_sha, (const uint8_t *)request_domain,
                           sizeof(request_domain));
            sha3_256_write(&request_sha, remote.action_root, 32);
            sha3_256_write(&request_sha, remote.task_root, 32);
            uint8_t now_le[8];
            zcl_write_i64_le(now_le, now);
            sha3_256_write(&request_sha, now_le, sizeof(now_le));
            sha3_256_finalize(&request_sha, request_digest);
            remote.request_id = zcl_read_u64_le(request_digest);
            if (remote.request_id == 0) remote.request_id = 1;
            struct db_build_worker requester_identity;
            uint8_t requester_secret[32], requester_key[32];
            if (build_fabric_worker_identity_load(
                    datadir, &requester_identity, requester_secret,
                    requester_key).ok &&
                vcs_zcode_work_request_seal(
                    &remote, requester_secret, requester_key) &&
                vcs_zcode_work_node_submit(
                    work, (uint64_t)remote_peer, &remote, now) ==
                    VCS_ZCODE_WORK_NODE_OK) {
                remote_outcome = "QUEUED_REMOTE";
                (void)json_push_kv_int(&reply->data, "remote_request_id",
                                       (int64_t)remote.request_id);
            }
            memset(requester_secret, 0, sizeof(requester_secret));
        }
        (void)json_push_kv_str(&reply->data, "remote_outcome", remote_outcome);
        if (strcmp(remote_outcome, "LOCAL_FALLBACK") == 0)
            (void)json_push_kv_str(&reply->data, "remote_reason",
                context_ready
                    ? "peer/capability unavailable; local queued action remains authoritative"
                    : context_reason);
    }
    (void)json_push_kv_str(
        &reply->data, "next",
        "an enabled local or P2P worker may produce the candidate-bound fixed-action receipt; evidence evaluation, explicit acceptance, and publication remain required");
}
