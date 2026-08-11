/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: contained model-neutral handoff for one verified ZCODE task. */

#include "command/native_command.h"

#include "base/hex.h"
#include "base/cleanse.h"
#include "json/json.h"
#include "platform/os_proc.h"
#include "platform/time_compat.h"
#include "models/database.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_worker.h"
#include "sha3/sha3.h"
#include "util/file_tree_ops.h"
#include "util/safe_alloc.h"
#include "util/spawn.h"
#include "vcs/vcs.h"
#include "vcs/vcs_object.h"
#include "vcs/build_action.h"
#include "vcs/package_deps.h"
#include "vcs/package_recipe.h"
#include "vcs/zcode_agent_context.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_task_index.h"
#include "vcs/zcode_write_scope.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZWORK_RUN_PATH_MAX 4400
#define ZWORK_ADAPTER_OUTPUT_MAX (32u * 1024u)
#define ZWORK_ADAPTER_PACKET_MAX (512u * 1024u)

static const char *run_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static void run_fail(struct zcl_command_reply *reply, const char *code,
                     const char *phase, const char *detail, bool retryable,
                     bool mutated)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, phase, retryable,
                           mutated, detail, "zcode.work.run");
}

static bool run_codex_runner_path(char out[ZWORK_RUN_PATH_MAX])
{
    char executable[ZWORK_RUN_PATH_MAX];
    const char *api_key = getenv("CODEX_API_KEY");
    const char *access_token = getenv("CODEX_ACCESS_TOKEN");
    if ((!api_key || !api_key[0]) &&
        (!access_token || !access_token[0]))
        return false;
    if ((api_key && api_key[0]) && (access_token && access_token[0]))
        return false;
    if (!os_proc_exe_path(executable, sizeof(executable)))
        return false;
    char *slash = strrchr(executable, '/');
    if (!slash)
        return false;
    *slash = '\0';
    int n = snprintf(out, ZWORK_RUN_PATH_MAX,
                     "%s/zclassic23-zcode-adapter-runner", executable);
    return n > 0 && (size_t)n < ZWORK_RUN_PATH_MAX &&
           access(out, X_OK) == 0;
}

static bool run_packet_path(const char *candidate_workspace,
                            char path[ZWORK_RUN_PATH_MAX])
{
    int n = snprintf(path, ZWORK_RUN_PATH_MAX,
                     "%s/.zcode-adapter-packet.json", candidate_workspace);
    return n > 0 && (size_t)n < ZWORK_RUN_PATH_MAX;
}

static bool run_write_packet(const char *candidate_workspace,
                             const struct json_value *packet,
                             char path[ZWORK_RUN_PATH_MAX])
{
    size_t len = json_write(packet, NULL, 0);
    if (len == 0 || len > ZWORK_ADAPTER_PACKET_MAX)
        return false;
    char *wire = zcl_malloc(len + 1u, "zcode.work.adapter.packet");
    if (!wire || json_write(packet, wire, len + 1u) != len) {
        free(wire);
        return false;
    }
    if (!run_packet_path(candidate_workspace, path)) {
        free(wire);
        return false;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    bool ok = fd >= 0;
    size_t off = 0;
    while (ok && off < len) {
        ssize_t wrote = write(fd, wire + off, len - off);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            ok = false;
        else
            off += (size_t)wrote;
    }
    if (ok)
        ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0)
        ok = false;
    if (!ok)
        (void)unlink(path);
    free(wire);
    return ok;
}

static void run_adapter_cleanup(const char *candidate_workspace,
                                const char *packet_path)
{
    if (packet_path && packet_path[0])
        (void)unlink(packet_path);
    char path[ZWORK_RUN_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.zcode-adapter-home",
                     candidate_workspace);
    if (n > 0 && (size_t)n < sizeof(path))
        ZCL_IGNORE_RESULT(zcl_tree_remove(path),
                          "remove private ephemeral adapter home");
    n = snprintf(path, sizeof(path), "%s/.zcode-adapter-tmp",
                 candidate_workspace);
    if (n > 0 && (size_t)n < sizeof(path))
        ZCL_IGNORE_RESULT(zcl_tree_remove(path),
                          "remove private ephemeral adapter temp");
}

static const struct vcs_zcode_task_index_entry *run_resolve(
    const struct vcs_zcode_task_index *index, const char *work, bool *ambiguous)
{
    *ambiguous = false;
    size_t count = vcs_zcode_task_index_task_count(index);
    if (count == 0) return NULL;
    if (!work || !work[0] || strcmp(work, "latest") == 0) {
        const struct vcs_zcode_task_index_entry *best =
            vcs_zcode_task_index_task_at(index, 0);
        for (size_t i = 1; i < count; i++) {
            const struct vcs_zcode_task_index_entry *at =
                vcs_zcode_task_index_task_at(index, i);
            if (at->expires_unix > best->expires_unix ||
                (at->expires_unix == best->expires_unix &&
                 strcmp(at->task_root_hex, best->task_root_hex) > 0))
                best = at;
        }
        return best;
    }
    const char *prefix = strncmp(work, "work-", 5) == 0 ? work + 5 : work;
    size_t prefix_len = strlen(prefix);
    if (prefix_len < 8 || prefix_len > 64) return NULL;
    const struct vcs_zcode_task_index_entry *match = NULL;
    for (size_t i = 0; i < count; i++) {
        const struct vcs_zcode_task_index_entry *at =
            vcs_zcode_task_index_task_at(index, i);
        if (strncmp(at->task_root_hex, prefix, prefix_len) != 0) continue;
        if (match) { *ambiguous = true; return NULL; }
        match = at;
    }
    return match;
}

static bool run_load_task(const char *workspace, const char *root_hex,
                          struct vcs_zcode_task_v1 *task)
{
    uint8_t root[32], check[32], *wire = NULL;
    size_t len = 0;
    bool ok = zcl_hex_decode_lower(root_hex, root, 32) &&
        vcs_object_load_raw_bounded(workspace, root, VCS_ZCODE_TASK_WIRE_BYTES,
                                    &wire, &len) == 0 &&
        len == VCS_ZCODE_TASK_WIRE_BYTES &&
        vcs_zcode_task_parse(wire, len, task) == VCS_ZCODE_DEV_OK &&
        vcs_zcode_task_root(task, check) == VCS_ZCODE_DEV_OK &&
        memcmp(check, root, 32) == 0;
    free(wire);
    return ok;
}

static char *run_load_goal(const char *workspace,
                           const struct vcs_zcode_task_v1 *task)
{
    uint8_t *bytes = NULL, check[32];
    size_t len = 0;
    if (vcs_object_load_raw_bounded(workspace, task->goal_root, 4096,
                                    &bytes, &len) != 0 || len == 0 ||
        memchr(bytes, '\0', len)) {
        free(bytes);
        return NULL;
    }
    sha3_256(bytes, len, check);
    if (memcmp(check, task->goal_root, 32) != 0) {
        free(bytes);
        return NULL;
    }
    char *goal = zcl_malloc(len + 1u, "zcode.work.run.goal");
    if (!goal) { free(bytes); return NULL; }
    memcpy(goal, bytes, len); goal[len] = '\0'; free(bytes);
    return goal;
}

static bool run_load_context(
    const char *workspace, const struct vcs_zcode_task_context_entry *entry,
    const struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_agent_context_v1 *context)
{
    uint8_t root[32], check[32], *wire = NULL;
    size_t len = 0;
    bool ok = zcl_hex_decode_lower(entry->context_root_hex, root, 32) &&
        vcs_object_load_raw_bounded(workspace, root, task->max_context_bytes,
                                    &wire, &len) == 0 &&
        vcs_zcode_agent_context_parse(wire, len, task->max_context_bytes,
                                      context) ==
            VCS_ZCODE_AGENT_CONTEXT_OK &&
        vcs_zcode_agent_context_root(context, task->max_context_bytes, check) ==
            VCS_ZCODE_AGENT_CONTEXT_OK && memcmp(check, root, 32) == 0;
    free(wire);
    return ok;
}

static bool run_load_scope(const char *workspace,
                           const struct vcs_zcode_task_v1 *task,
                           struct vcs_zcode_write_scope_v1 *scope)
{
    uint8_t *wire = NULL, check[32];
    size_t len = 0;
    bool ok = vcs_object_load_raw_bounded(
            workspace, task->write_scope_root,
            VCS_ZCODE_WRITE_SCOPE_WIRE_MAX, &wire, &len) == 0 &&
        vcs_zcode_write_scope_parse(wire, len, scope) ==
            VCS_ZCODE_WRITE_SCOPE_OK &&
        vcs_zcode_write_scope_root(scope, check) ==
            VCS_ZCODE_WRITE_SCOPE_OK &&
        memcmp(check, task->write_scope_root, 32) == 0;
    free(wire);
    return ok;
}

static bool run_excerpts_json(
    struct json_value *out, const struct vcs_zcode_agent_context_v1 *context)
{
    json_init(out); json_set_array(out);
    for (size_t i = 0; i < context->file_count; i++) {
        const struct vcs_zcode_agent_context_entry_v1 *entry =
            &context->files[i];
        if (memchr(entry->content, '\0', entry->content_len)) return false;
        char *content = zcl_malloc(entry->content_len + 1u,
                                   "zcode.work.run.excerpt");
        if (!content) return false;
        memcpy(content, entry->content, entry->content_len);
        content[entry->content_len] = '\0';
        struct json_value row;
        json_init(&row); json_set_object(&row);
        bool ok = json_push_kv_str(&row, "path", entry->path) &&
            json_push_kv_int(&row, "start_line", entry->start_line) &&
            json_push_kv_int(&row, "full_file_bytes",
                             (int64_t)entry->full_file_bytes) &&
            json_push_kv_str(&row, "content", content) &&
            json_push_back(out, &row);
        json_free(&row); free(content);
        if (!ok) return false;
    }
    return true;
}

static bool run_candidate_workspace(const char *store,
                                    const struct vcs_zcode_task_v1 *task,
                                    const char *task_hex, uint32_t attempt,
                                    const uint8_t source_root[32], char out[4400],
                                    bool *created)
{
    char parent[ZWORK_RUN_PATH_MAX];
    int n = snprintf(parent, sizeof(parent),
                     "/tmp/zclassic23-zcode-workspaces/%lu/%.64s",
                     (unsigned long)getuid(), task_hex);
    if (n <= 0 || (size_t)n >= sizeof(parent)) return false;
    struct zcl_result made = zcl_mkdir_p(parent, 0700);
    n = snprintf(out, ZWORK_RUN_PATH_MAX, "%s/attempt-%u", parent, attempt);
    if (!made.ok || n <= 0 || (size_t)n >= ZWORK_RUN_PATH_MAX) return false;
    if (mkdir(out, 0700) == 0) {
        *created = true;
        if (vcs_tree_materialize(store, source_root, out,
                                 task->max_output_bytes, 0u) != VCS_OK) {
            ZCL_IGNORE_RESULT(
                zcl_tree_remove(out),
                "best-effort rollback of a failed candidate materialization");
            return false;
        }
        return true;
    }
    struct stat st;
    *created = false;
    return errno == EEXIST && lstat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool run_packet(struct json_value *packet, const char *goal,
                       const char *candidate_workspace,
                       const struct vcs_zcode_task_index_entry *entry,
                       const struct vcs_zcode_task_context_entry *context_entry,
                       const struct vcs_zcode_task_v1 *task,
                       const struct vcs_zcode_agent_context_v1 *context,
                       const struct vcs_zcode_write_scope_v1 *scope)
{
    struct json_value excerpts, limits, scopes;
    if (!run_excerpts_json(&excerpts, context)) return false;
    char scope_hex[65], recipe_hex[65], lock_hex[65], toolchain_hex[65];
    zcl_hex_encode(task->write_scope_root, 32, scope_hex);
    zcl_hex_encode(task->acceptance_tests_root, 32, recipe_hex);
    zcl_hex_encode(task->dependency_lock_root, 32, lock_hex);
    zcl_hex_encode(task->toolchain_capsule_root, 32, toolchain_hex);
    json_init(&limits); json_set_object(&limits);
    json_init(&scopes); json_set_array(&scopes);
    bool ok = json_push_kv_int(&limits, "max_changed_files",
                               task->max_changed_files) &&
        json_push_kv_int(&limits, "max_patch_bytes",
                         (int64_t)task->max_patch_bytes) &&
        json_push_kv_int(&limits, "max_context_bytes",
                         (int64_t)task->max_context_bytes) &&
        json_push_kv_int(&limits, "max_cpu_seconds", task->max_cpu_seconds) &&
        json_push_kv_int(&limits, "max_memory_bytes",
                         (int64_t)task->max_memory_bytes) &&
        json_push_kv_int(&limits, "max_output_bytes",
                         (int64_t)task->max_output_bytes);
    for (size_t i = 0; ok && i < scope->count; i++) {
        struct json_value path;
        json_init(&path); json_set_str(&path, scope->paths[i]);
        ok = json_push_back(&scopes, &path);
        json_free(&path);
    }
    json_init(packet); json_set_object(packet);
    ok = ok && json_push_kv_str(packet, "goal", goal) &&
        json_push_kv_str(packet, "candidate_workspace", candidate_workspace) &&
        json_push_kv_str(packet, "task_root", entry->task_root_hex) &&
        json_push_kv_str(packet, "source_root", entry->source_root_hex) &&
        json_push_kv_str(packet, "context_root",
                         context_entry->context_root_hex) &&
        json_push_kv_str(packet, "context_query", context->query) &&
        json_push_kv(packet, "selected_excerpts", &excerpts) &&
        json_push_kv(packet, "allowed_write_scopes", &scopes) &&
        json_push_kv_str(packet, "write_scope_root", scope_hex) &&
        json_push_kv_str(packet, "package_recipe_root", recipe_hex) &&
        json_push_kv_str(packet, "dependency_lock_root", lock_hex) &&
        json_push_kv_str(packet, "proof_policy_root",
                         entry->proof_policy_root_hex) &&
        json_push_kv_str(packet, "toolchain_capsule_root", toolchain_hex) &&
        json_push_kv(packet, "limits", &limits) &&
        json_push_kv_str(packet, "instruction",
                         "Edit only the candidate workspace. Do not accept, publish, or claim proof.");
    json_free(&scopes); json_free(&limits); json_free(&excerpts);
    return ok;
}

static char *run_wire_hex(const char *workspace, const uint8_t root[32],
                          size_t maximum_bytes)
{
    uint8_t *wire = NULL;
    size_t len = 0;
    if (vcs_object_load_raw_bounded(workspace, root, maximum_bytes,
                                    &wire, &len) != 0 || len == 0 ||
        len > (SIZE_MAX - 1u) / 2u) {
        free(wire);
        return NULL;
    }
    char *hex = zcl_malloc(len * 2u + 1u, "zcode.work.run.wire_hex");
    if (hex) zcl_hex_encode(wire, len, hex);
    free(wire);
    return hex;
}

static bool run_scope_csv(const struct vcs_zcode_write_scope_v1 *scope,
                          char out[4097])
{
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < scope->count; i++) {
        size_t len = strlen(scope->paths[i]);
        size_t extra = len + (i ? 1u : 0u);
        if (extra > 4096u - used) return false;
        if (i) out[used++] = ',';
        memcpy(out + used, scope->paths[i], len);
        used += len;
        out[used] = '\0';
    }
    return used > 0;
}

static bool run_admit_input(
    struct json_value *input, const char *workspace, const char *datadir,
    const char *candidate_workspace, const char *goal,
    const struct vcs_zcode_task_index_entry *entry,
    const struct vcs_zcode_task_context_entry *context_entry,
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_agent_context_v1 *context,
    const struct vcs_zcode_write_scope_v1 *scope, const char *author_hex,
    const char *adapter_hex, uint64_t candidate_sequence,
    const char *execution_profile)
{
    char *policy = run_wire_hex(workspace, task->proof_policy_root,
                                VCS_ZCODE_PROOF_POLICY_WIRE_BYTES);
    char *lock = run_wire_hex(workspace, task->dependency_lock_root,
                              VCS_PACKAGE_LOCK_MAX_WIRE_BYTES);
    char *recipe = run_wire_hex(workspace, task->acceptance_tests_root,
                                VCS_PACKAGE_RECIPE_MAX_WIRE_BYTES);
    char scopes[4097], model_hex[65];
    zcl_hex_encode(task->model_policy_root, 32, model_hex);
    json_init(input); json_set_object(input);
    bool ok = policy && lock && recipe && run_scope_csv(scope, scopes) &&
        json_push_kv_str(input, "mode", "admit") &&
        json_push_kv_str(input, "workspace", workspace) &&
        json_push_kv_str(input, "datadir", datadir) &&
        json_push_kv_str(input, "goal", goal) &&
        json_push_kv_str(input, "proof_policy_hex", policy) &&
        json_push_kv_str(input, "dependency_lock_hex", lock) &&
        json_push_kv_str(input, "acceptance_recipe_hex", recipe) &&
        json_push_kv_str(input, "write_scope_csv", scopes) &&
        json_push_kv_str(input, "model_policy_root", model_hex) &&
        json_push_kv_str(input, "context_symbol", context->query) &&
        json_push_kv_str(input, "planned_task_root", entry->task_root_hex) &&
        json_push_kv_str(input, "planned_context_root",
                         context_entry->context_root_hex) &&
        json_push_kv_str(input, "candidate_workspace", candidate_workspace) &&
        json_push_kv_str(input, "adapter_policy_root", adapter_hex) &&
        json_push_kv_str(input, "author_pubkey", author_hex) &&
        json_push_kv_int(input, "candidate_sequence",
                         (int64_t)candidate_sequence) &&
        json_push_kv_str(input, "action_kind",
                         VCS_BUILD_ACTION_KIND_PACKAGE_V1) &&
        json_push_kv_str(input, "profile", execution_profile) &&
        json_push_kv_int(input, "expires_unix", task->expires_unix) &&
        json_push_kv_int(input, "max_changed_files",
                         task->max_changed_files) &&
        json_push_kv_int(input, "max_patch_bytes",
                         (int64_t)task->max_patch_bytes) &&
        json_push_kv_int(input, "max_context_bytes",
                         (int64_t)task->max_context_bytes) &&
        json_push_kv_int(input, "max_cpu_seconds", task->max_cpu_seconds) &&
        json_push_kv_int(input, "max_memory_bytes",
                         (int64_t)task->max_memory_bytes) &&
        json_push_kv_int(input, "max_output_bytes",
                         (int64_t)task->max_output_bytes);
    free(recipe); free(lock); free(policy);
    return ok;
}

static bool run_standard_policy(
    const char *workspace, const struct vcs_zcode_task_v1 *task,
    bool *standard)
{
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    struct vcs_zcode_proof_policy_v1 policy;
    if (!standard || vcs_object_load_raw_bounded(
            workspace, task->proof_policy_root,
            VCS_ZCODE_PROOF_POLICY_WIRE_BYTES, &wire, &wire_len) != 0)
        return false;
    bool ok = vcs_zcode_proof_policy_parse(wire, wire_len, &policy) ==
              VCS_ZCODE_DEV_OK;
    free(wire);
    if (!ok) return false;
    *standard = policy.minimum_compile_receipts >= 2u ||
                policy.minimum_test_receipts >= 2u;
    return true;
}

static struct zcl_result run_plan_standard_peer(
    const char *datadir, const char *primary_action_id,
    char peer_action_id[BUILD_FABRIC_ID_HEX + 1])
{
    char db_path[ZWORK_RUN_PATH_MAX];
    int n = snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb = {0};
    if (n <= 0 || (size_t)n >= sizeof(db_path) ||
        !node_db_open(&ndb, db_path))
        return ZCL_ERR(-1, "scratch ZBuild ledger could not be reopened");
    struct db_build_action action;
    struct db_build_job job;
    bool found = db_build_action_find(&ndb, primary_action_id, &action) &&
                 db_build_job_find(&ndb, action.job_id, &job);
    if (!found) {
        node_db_close(&ndb);
        return ZCL_ERR(-1, "primary package action is absent");
    }
    int64_t now = platform_time_wall_unix();
    (void)snprintf(job.profile, sizeof(job.profile), "%s",
                   VCS_BUILD_PACKAGE_PROFILE_STANDARD_B_V1);
    job.job_id[0] = '\0';
    (void)snprintf(job.state, sizeof(job.state), "PLANNED");
    job.outcome[0] = '\0';
    job.cancel_requested = 0;
    job.created_at = job.updated_at = now;
    action.action_id[0] = '\0';
    action.job_id[0] = '\0';
    (void)snprintf(action.state, sizeof(action.state), "SNAPSHOTTED");
    action.outcome[0] = '\0';
    action.output_root_sha3[0] = '\0';
    action.worker_id[0] = '\0';
    action.lease_id[0] = '\0';
    action.last_error[0] = '\0';
    action.lease_expires_at = action.lease_heartbeat_at = 0;
    action.attempt_count = action.claimed_at = action.started_at = 0;
    action.finished_at = 0;
    action.created_at = action.updated_at = now;
    struct zcl_result result = build_fabric_action_id(
        &job, &action, action.action_id);
    if (result.ok)
        result = build_fabric_job_id(&job, action.action_id, job.job_id);
    if (result.ok)
        (void)snprintf(action.job_id, sizeof(action.job_id), "%s",
                       job.job_id);
    if (result.ok) result = build_fabric_plan(&ndb, &job, &action);
    if (result.ok) result = build_fabric_submit(&ndb, job.job_id, now);
    if (result.ok)
        (void)snprintf(peer_action_id, BUILD_FABRIC_ID_HEX + 1u, "%s",
                       action.action_id);
    node_db_close(&ndb);
    return result;
}

static struct zcl_result run_execute_action(
    const char *workspace, const char *datadir, const char *action_id,
    struct db_build_worker *worker, const uint8_t secret[32],
    const uint8_t pubkey[32], struct db_build_receipt *receipt)
{
    char db_path[ZWORK_RUN_PATH_MAX];
    int n = snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb = {0};
    if (n <= 0 || (size_t)n >= sizeof(db_path) ||
        !node_db_open(&ndb, db_path))
        return ZCL_ERR(-1, "scratch ZBuild ledger could not be reopened");
    int64_t now = platform_time_wall_unix();
    worker->last_seen_at = now;
    struct zcl_result result = build_fabric_worker_approve(
        &ndb, worker, now);
    uint8_t lease_root[32];
    struct sha3_256_ctx sha;
    static const char domain[] = "zcl.zcode.work.local_lease.v1";
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, (const uint8_t *)action_id, strlen(action_id));
    sha3_256_finalize(&sha, lease_root);
    char lease_id[65];
    zcl_hex_encode(lease_root, 32, lease_id);
    struct db_build_action claimed_action;
    bool claimed = false;
    if (result.ok)
        result = build_fabric_claim(
            &ndb, worker->worker_id, lease_id, now,
            BUILD_FABRIC_LEASE_SECONDS_MAX, &claimed_action, &claimed);
    if (result.ok && (!claimed ||
        strcmp(claimed_action.action_id, action_id) != 0))
        result = ZCL_ERR(-1, "queued action was not claimable by exact id");
    if (result.ok)
        result = build_fabric_worker_execute(
            &ndb, workspace, datadir, action_id, lease_id,
            secret, pubkey, receipt);
    node_db_close(&ndb);
    return result;
}

static bool run_admit(
    const char *workspace, const char *candidate_workspace,
    const char *proof_datadir, const char *goal,
    const struct vcs_zcode_task_index_entry *entry,
    const struct vcs_zcode_task_context_entry *context_entry,
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_agent_context_v1 *context,
    const struct vcs_zcode_write_scope_v1 *scope,
    uint64_t candidate_sequence, const char *adapter_name,
    struct zcl_command_reply *reply)
{
    char datadir[ZWORK_RUN_PATH_MAX];
    if (proof_datadir && proof_datadir[0]) {
        int n = snprintf(datadir, sizeof(datadir), "%s", proof_datadir);
        if (n <= 0 || (size_t)n >= sizeof(datadir))
            return false;
    } else {
        (void)snprintf(datadir, sizeof(datadir), "%s", candidate_workspace);
        char *slash = strrchr(datadir, '/');
        if (!slash) return false;
        (void)snprintf(slash, (size_t)(datadir + sizeof(datadir) - slash),
                       "/zbuild");
    }
    struct zcl_result made = zcl_mkdir_p(datadir, 0700);
    struct db_build_worker worker;
    uint8_t secret[32] = {0}, pubkey[32] = {0};
    struct zcl_result identity = made.ok
        ? build_fabric_worker_identity_load(
              datadir, &worker, secret, pubkey)
        : made;
    char author_hex[65], adapter_hex[65];
    zcl_hex_encode(pubkey, 32, author_hex);
    uint8_t context_root[32], adapter_root[32];
    bool rooted = zcl_hex_decode_lower(context_entry->context_root_hex,
                                       context_root, 32);
    struct sha3_256_ctx sha;
    static const char manual_domain[] = "zcl.zcode.adapter.manual.v1";
    static const char codex_domain[] = "zcl.zcode.adapter.codex.v1";
    const char *domain = strcmp(adapter_name, "codex") == 0
        ? codex_domain : manual_domain;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, strlen(domain) + 1u);
    if (rooted) sha3_256_write(&sha, context_root, sizeof(context_root));
    if (rooted && candidate_sequence > 1u) {
        uint8_t parent_root[32];
        if (!zcl_hex_decode_lower(entry->latest_candidate_root_hex,
                                  parent_root, sizeof(parent_root)))
            rooted = false;
        else
            sha3_256_write(&sha, parent_root, sizeof(parent_root));
    }
    sha3_256_finalize(&sha, adapter_root);
    zcl_hex_encode(adapter_root, 32, adapter_hex);
    if (!identity.ok || !rooted) {
        memory_cleanse(secret, sizeof(secret));
        return false;
    }
    bool standard = false;
    if (!run_standard_policy(workspace, task, &standard)) {
        memory_cleanse(secret, sizeof(secret));
        return false;
    }
    const char *execution_profile = standard
        ? VCS_BUILD_PACKAGE_PROFILE_STANDARD_A_V1
        : VCS_BUILD_PACKAGE_PROFILE_QUICK_V1;
    struct json_value input;
    if (!run_admit_input(&input, workspace, datadir, candidate_workspace,
                         goal, entry, context_entry, task, context, scope,
                         author_hex, adapter_hex, candidate_sequence,
                         execution_profile)) {
        memory_cleanse(secret, sizeof(secret));
        return false;
    }
    struct zcl_command_request inner_request = { .input = &input };
    struct zcl_command_reply inner;
    zcl_command_reply_init(&inner, "zcl.zcode_improve.v1");
    zcl_native_handle_zcode_improve(&inner_request, &inner);
    json_free(&input);
    if (inner.status != ZCL_COMMAND_STATUS_PASSED) {
        run_fail(reply, inner.error.code[0] ? inner.error.code :
                     "CANDIDATE_ADMISSION_FAILED",
                 inner.error.phase[0] ? inner.error.phase : "admit",
                 inner.error.message[0] ? inner.error.message :
                     "existing candidate admission refused",
                 inner.error.retryable, inner.error.mutated);
        memory_cleanse(secret, sizeof(secret));
        zcl_command_reply_free(&inner);
        return true;
    }
    const struct json_value *changed = json_get(&inner.data, "changed_files");
    const struct json_value *candidate = json_get(&inner.data, "candidate_root");
    const struct json_value *candidate_source =
        json_get(&inner.data, "candidate_source_root");
    const struct json_value *patch = json_get(&inner.data, "patch_root");
    const struct json_value *action = json_get(&inner.data, "action_id");
    const struct json_value *proof_state =
        json_get(&inner.data, "async_proof_state");
    const struct json_value *proof_event =
        json_get(&inner.data, "async_proof_event_root");
    const struct json_value *proof_request =
        json_get(&inner.data, "remote_request_id");
    const struct json_value *submit_us =
        json_get(&inner.data, "local_submit_us");
    struct db_build_receipt receipt;
    struct zcl_result executed = action && json_get_str(action)
        ? run_execute_action(workspace, datadir, json_get_str(action), &worker,
                             secret, pubkey, &receipt)
        : ZCL_ERR(-1, "admission did not return an action id");
    char peer_action_id[BUILD_FABRIC_ID_HEX + 1] = {0};
    struct db_build_receipt peer_receipt;
    memset(&peer_receipt, 0, sizeof(peer_receipt));
    if (executed.ok && receipt.exit_status == 0 && standard) {
        executed = run_plan_standard_peer(
            datadir, json_get_str(action), peer_action_id);
        if (executed.ok)
            executed = run_execute_action(
                workspace, datadir, peer_action_id, &worker, secret, pubkey,
                &peer_receipt);
    }
    memory_cleanse(secret, sizeof(secret));
    if (!executed.ok) {
        run_fail(reply, "PACKAGE_BUILD_FAILED", "build", executed.message,
                 true, true);
        zcl_command_reply_free(&inner);
        return true;
    }
    struct json_value expert;
    json_init(&expert); json_set_object(&expert);
    bool expert_ok = action && candidate && candidate_source && patch &&
        json_push_kv_str(&expert, "task_root", entry->task_root_hex) &&
        json_push_kv_str(&expert, "candidate_root",
                         json_get_str(candidate)) &&
        json_push_kv_str(&expert, "candidate_source_root",
                         json_get_str(candidate_source)) &&
        json_push_kv_str(&expert, "patch_root", json_get_str(patch)) &&
        json_push_kv_str(&expert, "action_id", json_get_str(action)) &&
        json_push_kv_str(&expert, "receipt_id", receipt.receipt_id) &&
        json_push_kv_str(&expert, "output_root", receipt.output_sha3) &&
        json_push_kv_str(&expert, "work_receipt_root",
                         receipt.work_receipt_sha3) &&
        (!standard ||
         (json_push_kv_str(&expert, "standard_peer_action_id",
                           peer_action_id) &&
          json_push_kv_str(&expert, "standard_peer_work_receipt_root",
                           peer_receipt.work_receipt_sha3)));
    char work_id[32];
    (void)snprintf(work_id, sizeof(work_id), "work-%.12s",
                   entry->task_root_hex);
    bool passed = receipt.exit_status == 0 &&
                  (!standard || peer_receipt.exit_status == 0);
    char next_workspace[ZWORK_RUN_PATH_MAX] = {0};
    bool next_created = false;
    uint8_t next_source_root[32];
    bool retry_ready = !passed && candidate_sequence < 3u &&
        candidate_source && json_get_str(candidate_source) &&
        zcl_hex_decode_lower(json_get_str(candidate_source), next_source_root,
                             sizeof(next_source_root)) &&
        run_candidate_workspace(workspace, task, entry->task_root_hex,
                                (uint32_t)candidate_sequence + 1u,
                                next_source_root, next_workspace,
                                &next_created);
    (void)next_created;
    struct json_value diagnostic;
    json_init(&diagnostic); json_set_object(&diagnostic);
    bool diagnostic_ok = json_push_kv_str(&diagnostic, "stage",
                                           "package_build_and_tests") &&
        json_push_kv_int(&diagnostic, "attempt",
                         (int64_t)candidate_sequence) &&
        json_push_kv_int(&diagnostic, "exit_status", receipt.exit_status) &&
        json_push_kv_str(&diagnostic, "evidence_root", receipt.output_sha3) &&
        json_push_kv_str(&diagnostic, "work_receipt_root",
                         receipt.work_receipt_sha3) &&
        json_push_kv_bool(&diagnostic, "retry_safe", retry_ready);
    struct json_value repair_packet;
    json_init(&repair_packet); json_set_object(&repair_packet);
    bool repair_packet_ok = !retry_ready ||
        (candidate && patch && run_packet(&repair_packet, goal, next_workspace, entry,
                    context_entry, task, context, scope) &&
         json_push_kv_str(&repair_packet, "parent_candidate_root",
                          json_get_str(candidate)) &&
         json_push_kv_str(&repair_packet, "prior_patch_root",
                          json_get_str(patch)) &&
         json_push_kv(&repair_packet, "diagnostic", &diagnostic));
    bool ok = changed && candidate && patch && proof_state && proof_event &&
        proof_request && submit_us && diagnostic_ok && expert_ok &&
        repair_packet_ok && receipt.work_receipt_sha3[0] &&
        json_push_kv_str(&reply->data, "work_id", work_id) &&
        json_push_kv_str(&reply->data, "state", passed ? "EVIDENCE_READY" :
                         retry_ready ? "REPAIR_NEEDED" : "BLOCKED") &&
        json_push_kv_int(&reply->data, "changed_files",
                         json_get_int(changed)) &&
        json_push_kv_str(&reply->data, "candidate_root",
                         json_get_str(candidate)) &&
        json_push_kv_str(&reply->data, "patch_root", json_get_str(patch)) &&
        json_push_kv_str(&reply->data, "work_receipt_root",
                         receipt.work_receipt_sha3) &&
        json_push_kv_str(&reply->data, "async_proof_state",
                         json_get_str(proof_state)) &&
        json_push_kv_str(&reply->data, "async_proof_event_root",
                         json_get_str(proof_event)) &&
        json_push_kv_int(&reply->data, "remote_request_id",
                         json_get_int(proof_request)) &&
        json_push_kv_int(&reply->data, "local_submit_us",
                         json_get_int(submit_us)) &&
        json_push_kv_str(&reply->data, "build_result",
                         passed ? "passed" : "failed") &&
        json_push_kv_int(&reply->data, "compile_receipts",
                         passed ? (standard ? 2 : 1) : 0) &&
        json_push_kv_int(&reply->data, "test_receipts",
                         passed ? (standard ? 2 : 1) : 0) &&
        json_push_kv_str(&reply->data, "sanitizer_result",
                         passed && standard ? "passed_asan_ubsan" :
                         standard ? "failed_or_unavailable" :
                                    "not_required") &&
        json_push_kv_int(&reply->data, "attempt",
                         (int64_t)candidate_sequence) &&
        json_push_kv(&reply->data, "diagnostic", &diagnostic) &&
        (!retry_ready ||
         json_push_kv_str(&reply->data, "candidate_workspace",
                          next_workspace)) &&
        (!retry_ready ||
         json_push_kv(&reply->data, "repair_packet", &repair_packet)) &&
        json_push_kv_str(&reply->data, "adapter", adapter_name) &&
        json_push_kv_str(&reply->data, "next_safe_command",
                         passed ? "zcode work status" :
                         retry_ready ? "edit candidate_workspace, then rerun zcode work run" :
                                       "zcode work status") &&
        json_push_kv(&reply->data, "expert", &expert);
    json_free(&repair_packet); json_free(&diagnostic); json_free(&expert);
    zcl_command_reply_free(&inner);
    if (!ok)
        run_fail(reply, "ADMISSION_OUTPUT_FAILED", "render",
                 "candidate admission summary could not be rendered",
                 false, true);
    return true;
}

void zcl_native_handle_zcode_work_run(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = run_str(request->input, "workspace");
    const char *work = run_str(request->input, "work");
    const char *adapter = run_str(request->input, "adapter");
    const char *proof_datadir_arg = run_str(request->input, "datadir");
    if (!workspace_arg || !workspace_arg[0]) workspace_arg = ".";
    if (!adapter || !adapter[0]) adapter = "manual";
    bool codex_adapter = strcmp(adapter, "codex") == 0;
    if (strcmp(adapter, "manual") != 0 && !codex_adapter) {
        run_fail(reply, "ADAPTER_REFUSED", "adapter",
                 "adapter must name one fixed adapter: manual or codex",
                 false, false);
        return;
    }
    char codex_runner[ZWORK_RUN_PATH_MAX] = {0};
    if (codex_adapter && !run_codex_runner_path(codex_runner)) {
        run_fail(reply, "ADAPTER_UNAVAILABLE", "adapter",
                 "the fixed confined Codex runner or one supported single-run CODEX credential is unavailable; manual remains safe",
                 true, false);
        return;
    }
    char workspace[ZWORK_RUN_PATH_MAX];
    if (!realpath(workspace_arg, workspace)) {
        run_fail(reply, "BAD_WORKSPACE", "resolve",
                 "workspace must resolve to an existing directory", false,
                 false);
        return;
    }
    char proof_datadir[ZWORK_RUN_PATH_MAX] = {0};
    if (proof_datadir_arg && proof_datadir_arg[0] &&
        !realpath(proof_datadir_arg, proof_datadir)) {
        run_fail(reply, "BAD_DATADIR", "resolve",
                 "datadir must resolve to an existing full-node data directory",
                 false, false);
        return;
    }
    struct vcs_zcode_task_index *index = vcs_zcode_task_index_build(
        workspace, platform_time_wall_unix());
    bool ambiguous = false;
    const struct vcs_zcode_task_index_entry *entry = index
        ? run_resolve(index, work, &ambiguous) : NULL;
    bool context_ambiguous = false;
    const struct vcs_zcode_task_context_entry *context_entry = entry
        ? vcs_zcode_task_index_context_for_task(
              index, entry->task_root_hex, &context_ambiguous) : NULL;
    struct vcs_zcode_task_v1 task;
    struct vcs_zcode_agent_context_v1 context;
    vcs_zcode_agent_context_init(&context);
    struct vcs_zcode_write_scope_v1 scope;
    vcs_zcode_write_scope_init(&scope);
    char *goal = NULL;
    bool loaded = entry && context_entry && !context_ambiguous &&
        run_load_task(workspace, entry->task_root_hex, &task) &&
        (goal = run_load_goal(workspace, &task)) != NULL &&
        run_load_context(workspace, context_entry, &task, &context) &&
        run_load_scope(workspace, &task, &scope) && !entry->expired;
    if (!loaded) {
        run_fail(reply, context_ambiguous ? "AMBIGUOUS_CONTEXT" :
                     ambiguous ? "AMBIGUOUS_WORK" : "WORK_HANDOFF_MISSING",
                 "resolve",
                 entry && entry->expired
                    ? "task expired; start a new bounded work item"
                    : context_ambiguous
                    ? "task has multiple contexts; select an exact expert context"
                    : "verified task, goal, and unique context could not be reloaded",
                 false, false);
        free(goal); vcs_zcode_agent_context_free(&context);
        vcs_zcode_task_index_free(index); return;
    }
    uint8_t task_root[32];
    bool bindings = zcl_hex_decode_lower(entry->task_root_hex, task_root, 32) &&
        memcmp(context.task_root, task_root, 32) == 0 &&
        memcmp(context.source_root, task.source_root, 32) == 0 &&
        memcmp(context.goal_root, task.goal_root, 32) == 0;
    if (strcmp(entry->state, VCS_ZCODE_TASK_STATE_EVIDENCE_READY) == 0) {
        char work_id[32];
        (void)snprintf(work_id, sizeof(work_id), "work-%.12s",
                       entry->task_root_hex);
        bool ok = json_push_kv_str(&reply->data, "work_id", work_id) &&
            json_push_kv_str(&reply->data, "state", "EVIDENCE_READY") &&
            json_push_kv_str(&reply->data, "build_result", "passed") &&
            json_push_kv_str(&reply->data, "next_safe_command",
                             "zcode work status");
        if (!ok)
            run_fail(reply, "HANDOFF_OUTPUT_FAILED", "render",
                     "evidence-ready summary could not be rendered",
                     false, false);
        free(goal); vcs_zcode_agent_context_free(&context);
        vcs_zcode_task_index_free(index); return;
    }
    if (strcmp(entry->state, VCS_ZCODE_TASK_STATE_CANDIDATE_ADMITTED) == 0) {
        run_fail(reply, "CANDIDATE_EXECUTION_INCOMPLETE", "build",
                 "the candidate is captured but its prior package execution produced no signed work receipt; preserve it and diagnose the package prerequisite before starting another attempt",
                 true, true);
        free(goal); vcs_zcode_agent_context_free(&context);
        vcs_zcode_task_index_free(index); return;
    }
    bool repairing = strcmp(entry->state,
                            VCS_ZCODE_TASK_STATE_REPAIR_NEEDED) == 0;
    if (entry->candidate_count >= 3u) {
        run_fail(reply, "REPAIR_LIMIT_REACHED", "repair",
                 "three candidate attempts are preserved; start a new bounded work item",
                 false, false);
        free(goal); vcs_zcode_agent_context_free(&context);
        vcs_zcode_task_index_free(index); return;
    }
    uint64_t candidate_sequence = entry->candidate_count + 1u;
    uint8_t materialize_root[32];
    bool materialize_root_ok = true;
    if (repairing)
        materialize_root_ok = zcl_hex_decode_lower(
            entry->latest_candidate_source_root_hex, materialize_root,
            sizeof(materialize_root));
    else
        memcpy(materialize_root, task.source_root, sizeof(materialize_root));
    char candidate_workspace[ZWORK_RUN_PATH_MAX];
    bool created = false;
    if (!bindings || !materialize_root_ok || !run_candidate_workspace(
            workspace, &task, entry->task_root_hex,
            (uint32_t)candidate_sequence, materialize_root,
            candidate_workspace, &created)) {
        run_fail(reply, "HANDOFF_REFUSED", "materialize",
                 bindings ? "isolated candidate workspace could not be created"
                          : "task and context bindings disagree",
                 true, false);
        free(goal); vcs_zcode_agent_context_free(&context);
        vcs_zcode_task_index_free(index); return;
    }
    char prior_packet_path[ZWORK_RUN_PATH_MAX] = {0};
    if (run_packet_path(candidate_workspace, prior_packet_path))
        run_adapter_cleanup(candidate_workspace, prior_packet_path);
    if (!created) {
        uint8_t candidate_root[32];
        if (vcs_tree_capture_into(candidate_workspace, workspace,
                                  candidate_root) != VCS_OK) {
            run_fail(reply, "CANDIDATE_CAPTURE_FAILED", "capture",
                     "candidate workspace changed or contains a refused file",
                     true, false);
            free(goal); vcs_zcode_agent_context_free(&context);
            vcs_zcode_task_index_free(index); return;
        }
        if (memcmp(candidate_root, materialize_root, 32) != 0) {
            bool handled = run_admit(
                workspace, candidate_workspace, proof_datadir, goal,
                entry, context_entry,
                &task, &context, &scope, candidate_sequence, "manual", reply);
            if (!handled)
                run_fail(reply, "CANDIDATE_ADMISSION_FAILED", "admit",
                         "scratch identity or existing task composition failed",
                         true, false);
            free(goal); vcs_zcode_agent_context_free(&context);
            vcs_zcode_task_index_free(index); return;
        }
    }
    struct json_value packet;
    json_init(&packet);
    char work_id[32];
    (void)snprintf(work_id, sizeof(work_id), "work-%.12s",
                   entry->task_root_hex);
    bool packet_ok = run_packet(&packet, goal, candidate_workspace, entry,
                                context_entry, &task, &context, &scope);
    if (packet_ok && codex_adapter) {
        char packet_path[ZWORK_RUN_PATH_MAX] = {0};
        char *adapter_output = zcl_malloc(ZWORK_ADAPTER_OUTPUT_MAX,
                                          "zcode.work.adapter.output");
        bool staged = adapter_output && run_write_packet(
            candidate_workspace, &packet, packet_path);
        const char *const argv[] = {
            codex_runner, candidate_workspace, packet_path, NULL,
        };
        int rc = staged ? zcl_spawn_capture(
            argv, adapter_output, ZWORK_ADAPTER_OUTPUT_MAX, 300000) : -1;
        run_adapter_cleanup(candidate_workspace, packet_path);
        if (!staged || rc != 0) {
            char detail[384];
            const char *kind = rc == 137 ? "timed out" :
                               rc == 69 || rc == 127 ? "is unavailable" :
                               "refused or failed";
            const char *output_tail = adapter_output ? adapter_output : "";
            size_t output_len = strlen(output_tail);
            if (output_len > 220u) output_tail += output_len - 220u;
            (void)snprintf(detail, sizeof(detail),
                           "confined Codex adapter %s (exit=%d)%s%.220s",
                           kind, rc,
                           adapter_output && adapter_output[0] ? ": " : "",
                           output_tail);
            run_fail(reply, rc == 137 ? "ADAPTER_TIMEOUT" :
                       rc == 69 || rc == 127 ? "ADAPTER_UNAVAILABLE" :
                                              "ADAPTER_REFUSAL",
                     "adapter", detail, rc != 70, staged);
            free(adapter_output); json_free(&packet); free(goal);
            vcs_zcode_agent_context_free(&context);
            vcs_zcode_task_index_free(index); return;
        }
        uint8_t candidate_root[32];
        bool captured = vcs_tree_capture_into(candidate_workspace, workspace,
                                              candidate_root) == VCS_OK;
        if (!captured || memcmp(candidate_root, materialize_root, 32) == 0) {
            run_fail(reply, captured ? "ADAPTER_REFUSAL" :
                                      "CANDIDATE_CAPTURE_FAILED",
                     captured ? "adapter" : "capture",
                     captured ? "Codex completed without an admissible source change"
                              : "Codex output could not be captured safely",
                     true, true);
            free(adapter_output); json_free(&packet); free(goal);
            vcs_zcode_agent_context_free(&context);
            vcs_zcode_task_index_free(index); return;
        }
        bool handled = run_admit(
            workspace, candidate_workspace, proof_datadir, goal,
            entry, context_entry,
            &task, &context, &scope, candidate_sequence, "codex", reply);
        if (handled && reply->status == ZCL_COMMAND_STATUS_PASSED)
            (void)json_push_kv_str(&reply->data, "adapter_output",
                                   adapter_output);
        if (!handled)
            run_fail(reply, "CANDIDATE_ADMISSION_FAILED", "admit",
                     "confined Codex result could not enter existing candidate authority",
                     true, true);
        free(adapter_output); json_free(&packet); free(goal);
        vcs_zcode_agent_context_free(&context);
        vcs_zcode_task_index_free(index); return;
    }
    char manual_packet_path[ZWORK_RUN_PATH_MAX] = {0};
    size_t manual_packet_bytes = packet_ok ? json_write(&packet, NULL, 0) : 0;
    bool manual_staged = packet_ok && manual_packet_bytes > 0 &&
        run_write_packet(candidate_workspace, &packet, manual_packet_path);
    bool ok = manual_staged &&
        json_push_kv_str(&reply->data, "work_id", work_id) &&
        json_push_kv_str(&reply->data, "adapter", "manual") &&
        json_push_kv_str(&reply->data, "state", repairing
                         ? "REPAIR_NEEDED" : "AWAITING_CANDIDATE") &&
        json_push_kv_str(&reply->data, "candidate_workspace",
                         candidate_workspace) &&
        json_push_kv_bool(&reply->data, "workspace_created", created) &&
        json_push_kv_str(&reply->data, "adapter_packet_path",
                         manual_packet_path) &&
        json_push_kv_int(&reply->data, "adapter_packet_bytes",
                         (int64_t)manual_packet_bytes) &&
        json_push_kv_str(&reply->data, "authority", "NONE_MANUAL_HANDOFF") &&
        json_push_kv_str(&reply->data, "next_safe_command", repairing
                         ? "edit candidate_workspace, then rerun zcode work run"
                         : "edit only candidate_workspace, then inspect status");
    json_free(&packet); free(goal); vcs_zcode_agent_context_free(&context);
    vcs_zcode_task_index_free(index);
    if (!ok)
        run_fail(reply, "HANDOFF_OUTPUT_FAILED", "render",
                 "bounded manual adapter packet could not be rendered",
                 false, created);
}
