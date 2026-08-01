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
#include "util/safe_alloc.h"
#include "vcs/build_action.h"
#include "vcs/build_artifact_manifest.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev.h"
#include "vcs/package_store.h"
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
    uint8_t *bytes = zcl_malloc(len, "zcode.improve.preprocessed");
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
    const char *preprocessed = zdev_str(request->input, "preprocessed_path");
    if (!workspace_arg || !datadir || !goal || !goal[0] ||
        strlen(goal) > 4096 || !policy_hex || !preprocessed) {
        zdev_fail(reply, "MISSING_INPUT",
                  "workspace, datadir, goal, proof policy, and preprocessed path are required");
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
        !(policy.required_proofs & VCS_ZCODE_PROOF_COMPILE)) {
        zdev_fail(reply, "BAD_PROOF_POLICY",
                  "proof_policy_hex must be canonical proof_policy.v1 requiring compile");
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
    uint8_t *input = zdev_read_file(preprocessed, &input_len);
    if (!input) {
        zdev_fail(reply, "BAD_PREPROCESSED_INPUT",
                  "preprocessed_path must be an absolute bounded regular file");
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
        .created_unix = now,
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
    bool remote_requested = remote_peer > 0;
    if (remote_requested &&
        !zdev_root(request->input, "context_root", context_root, reply))
        return;
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
                   VCS_BUILD_ACTION_KIND_V1);
    (void)snprintf(action.state, sizeof(action.state), "SNAPSHOTTED");
    zcl_hex_encode(input_root, 32, action.input_root_sha3);
    zcl_hex_encode(task_root, 32, action.task_root_sha3);
    zcl_hex_encode(candidate_root, 32, action.candidate_root_sha3);
    zcl_hex_encode(policy_root, 32, action.proof_policy_root_sha3);
    if (remote_requested)
        zcl_hex_encode(context_root, 32, action.context_root_sha3);
    (void)snprintf(action.target, sizeof(action.target), "%s",
                   VCS_BUILD_TARGET_V1);
    uint8_t fixed_flags[32], fixed_environment[32];
    vcs_build_action_v1_fixed_flags_root(fixed_flags);
    vcs_build_action_v1_fixed_environment_root(fixed_environment);
    zcl_hex_encode(fixed_flags, 32, action.flags_sha3);
    zcl_hex_encode(fixed_environment, 32, action.environment_sha3);
    (void)snprintf(action.virtual_workdir, sizeof(action.virtual_workdir),
                   "%s", VCS_BUILD_VIRTUAL_ROOT_V1);
    (void)snprintf(action.declared_outputs, sizeof(action.declared_outputs),
                   "%s", VCS_BUILD_OUTPUT_V1);
    (void)snprintf(action.resource_policy, sizeof(action.resource_policy),
                   "%s", VCS_BUILD_RESOURCE_POLICY_V1);
    action.created_at = action.updated_at = now;
    if (!build_fabric_action_id(&job, &action, action.action_id).ok ||
        !build_fabric_job_id(&job, action.action_id, job.job_id).ok) {
        zdev_fail(reply, "ACTION_ID_FAILED", "fixed build action identity refused");
        return;
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
    struct zcl_result submitted = planned.ok
        ? build_fabric_submit(&ndb, job.job_id, now) : planned;
    node_db_close(&ndb);
    if (!planned.ok || !submitted.ok) {
        zdev_fail(reply, "ZBUILD_PLAN_FAILED",
                  planned.ok ? submitted.message : planned.message);
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
    (void)json_push_kv_str(&reply->data, "state", "QUEUED");
    (void)json_push_kv_str(&reply->data, "lane", "FRONTIER");
    if (remote_requested) {
        const char *remote_outcome = "LOCAL_FALLBACK";
        struct vcs_zcode_work_node *work = vcs_zcode_work_node_global();
        struct vcs_package_store *store = vcs_package_store_global();
        struct vcs_package_store_status package_status;
        struct vcs_zcode_work_capability_v1 capability;
        if (work && store &&
            vcs_package_store_package_status(store, context_root,
                                              &package_status) &&
            package_status.complete &&
            vcs_zcode_work_node_peer_capability(
                work, (uint64_t)remote_peer, now, &capability)) {
            struct vcs_zcode_work_request_v1 remote = {
                .target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3,
                .work_kind = VCS_ZCODE_WORK_BUILD,
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
            (void)json_push_kv_str(
                &reply->data, "remote_reason",
                "peer/capability/context unavailable; local queued action remains authoritative");
    }
    (void)json_push_kv_str(
        &reply->data, "next",
        "an enabled local or P2P worker may produce the candidate-bound compile receipt; review, explicit acceptance, and publication remain required");
}
