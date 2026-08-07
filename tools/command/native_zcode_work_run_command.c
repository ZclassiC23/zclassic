/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: contained model-neutral handoff for one verified ZCODE task. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "sha3/sha3.h"
#include "util/file_tree_ops.h"
#include "util/safe_alloc.h"
#include "vcs/vcs.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_agent_context.h"
#include "vcs/zcode_dev.h"
#include "vcs/zcode_task_index.h"
#include "vcs/zcode_write_scope.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZWORK_RUN_PATH_MAX 4400

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
                                    const char *task_hex, char out[4400],
                                    bool *created)
{
    char parent[ZWORK_RUN_PATH_MAX];
    int n = snprintf(parent, sizeof(parent),
                     "/tmp/zclassic23-zcode-workspaces/%lu/%.64s",
                     (unsigned long)getuid(), task_hex);
    if (n <= 0 || (size_t)n >= sizeof(parent)) return false;
    struct zcl_result made = zcl_mkdir_p(parent, 0700);
    n = snprintf(out, ZWORK_RUN_PATH_MAX, "%s/attempt-1", parent);
    if (!made.ok || n <= 0 || (size_t)n >= ZWORK_RUN_PATH_MAX) return false;
    if (mkdir(out, 0700) == 0) {
        *created = true;
        if (vcs_tree_materialize(store, task->source_root, out,
                                 task->max_output_bytes, 0600u) != VCS_OK) {
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

void zcl_native_handle_zcode_work_run(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = run_str(request->input, "workspace");
    const char *work = run_str(request->input, "work");
    const char *adapter = run_str(request->input, "adapter");
    if (!workspace_arg || !workspace_arg[0]) workspace_arg = ".";
    if (!adapter || !adapter[0]) adapter = "manual";
    if (strcmp(adapter, "manual") != 0) {
        run_fail(reply, "ADAPTER_REFUSED", "adapter",
                 "adapter must name the fixed available adapter: manual",
                 false, false);
        return;
    }
    char workspace[ZWORK_RUN_PATH_MAX];
    if (!realpath(workspace_arg, workspace)) {
        run_fail(reply, "BAD_WORKSPACE", "resolve",
                 "workspace must resolve to an existing directory", false,
                 false);
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
    char candidate_workspace[ZWORK_RUN_PATH_MAX];
    bool created = false;
    if (!bindings || !run_candidate_workspace(
            workspace, &task, entry->task_root_hex, candidate_workspace,
            &created)) {
        run_fail(reply, "HANDOFF_REFUSED", "materialize",
                 bindings ? "isolated candidate workspace could not be created"
                          : "task and context bindings disagree",
                 true, false);
        free(goal); vcs_zcode_agent_context_free(&context);
        vcs_zcode_task_index_free(index); return;
    }
    struct json_value packet;
    json_init(&packet);
    char work_id[32];
    (void)snprintf(work_id, sizeof(work_id), "work-%.12s",
                   entry->task_root_hex);
    bool ok = run_packet(&packet, goal, candidate_workspace, entry,
                         context_entry, &task, &context, &scope) &&
        json_push_kv_str(&reply->data, "work_id", work_id) &&
        json_push_kv_str(&reply->data, "adapter", "manual") &&
        json_push_kv_str(&reply->data, "state", "AWAITING_CANDIDATE") &&
        json_push_kv_str(&reply->data, "candidate_workspace",
                         candidate_workspace) &&
        json_push_kv_bool(&reply->data, "workspace_created", created) &&
        json_push_kv(&reply->data, "adapter_packet", &packet) &&
        json_push_kv_str(&reply->data, "authority", "NONE_MANUAL_HANDOFF") &&
        json_push_kv_str(&reply->data, "next_safe_command",
                         "edit only candidate_workspace, then inspect status");
    json_free(&packet); free(goal); vcs_zcode_agent_context_free(&context);
    vcs_zcode_task_index_free(index);
    if (!ok)
        run_fail(reply, "HANDOFF_OUTPUT_FAILED", "render",
                 "bounded manual adapter packet could not be rendered",
                 false, created);
}
