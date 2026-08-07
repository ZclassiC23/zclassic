/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: human-first orchestration over existing ZCODE development owners. */

#include "command/native_command.h"

#include "base/checked.h"
#include "base/hex.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "services/zcode_goal_context_service.h"
#include "sha3/sha3.h"
#include "util/safe_alloc.h"
#include "vcs/package_prepare.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev_product.h"
#include "vcs/zcode_task_index.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZWORK_PATH_MAX 4400

static const char *zwork_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static void zwork_fail(struct zcl_command_reply *reply, const char *code,
                       const char *phase, const char *detail, bool retryable,
                       bool mutated)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, phase, retryable,
                           mutated, detail, "zcode.work");
}

static char *zwork_hex_alloc(const uint8_t *bytes, size_t len,
                             const char *label)
{
    if (!bytes || len > (SIZE_MAX - 1u) / 2u) return NULL;
    char *hex = zcl_malloc(len * 2u + 1u, label);
    if (hex) zcl_hex_encode(bytes, len, hex);
    return hex;
}

static bool zwork_scope_add(char out[1024], const char *path)
{
    if (!path || !path[0]) return false;
    const char *slash = strchr(path, '/');
    size_t len = slash ? (size_t)(slash - path) : strlen(path);
    if (len == 0 || len > 255) return false;
    char part[256];
    memcpy(part, path, len); part[len] = '\0';
    const char *at = out;
    while (*at) {
        const char *end = strchr(at, ',');
        size_t have = end ? (size_t)(end - at) : strlen(at);
        if (have == len && memcmp(at, part, len) == 0) return true;
        if (!end) break;
        at = end + 1;
    }
    size_t used = strlen(out);
    size_t need = len + (used ? 1u : 0u);
    if (need >= sizeof(char[1024]) - used) return false;
    if (used) out[used++] = ',';
    memcpy(out + used, part, len + 1u);
    return true;
}

static bool zwork_scopes(const struct vcs_package_recipe *recipe,
                         char out[1024])
{
    out[0] = '\0';
    const struct vcs_package_recipe_strings *lists[] = {
        &recipe->public_headers, &recipe->sources, &recipe->test_sources,
    };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++)
        for (size_t j = 0; j < lists[i]->count; j++)
            if (!zwork_scope_add(out, lists[i]->items[j])) return false;
    return out[0] != '\0';
}

static uint64_t zwork_source_bytes(
    const struct vcs_package_prepared *prepared)
{
    uint64_t total = 0;
    const struct vcs_package_recipe_strings *lists[] = {
        &prepared->recipe.public_headers, &prepared->recipe.sources,
        &prepared->recipe.test_sources,
    };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        for (size_t j = 0; j < lists[i]->count; j++) {
            for (size_t f = 0; f < prepared->manifest.count; f++) {
                if (strcmp(lists[i]->items[j],
                           prepared->manifest.files[f].path) != 0)
                    continue;
                if (!zcl_u64_add(total, prepared->manifest.files[f].size,
                                 &total))
                    return 0;
                break;
            }
        }
    }
    return total;
}

static bool zwork_prepare(const char *workspace,
                          struct vcs_package_prepared *prepared,
                          char *detail, size_t detail_cap)
{
    static const uint8_t pubkey[33] = {
        0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb,
        0xac, 0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b,
        0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28,
        0xd9, 0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
    };
    struct vcs_package_prepare_options options = {
        .dir = workspace, .publisher_sequence = 1,
        .reward_address = "", .chain_id = "zclassic-main",
    };
    memcpy(options.publisher_pubkey, pubkey, sizeof(pubkey));
    return vcs_package_prepare(&options, prepared, detail, detail_cap) ==
           VCS_PACKAGE_PREPARE_OK;
}

static bool zwork_plan_input(
    struct json_value *input, const char *workspace, const char *goal,
    const struct vcs_package_prepared *prepared,
    const struct vcs_zcode_dev_profile *profile,
    const struct zcode_goal_selection *selection, const char *scopes,
    int64_t expires_unix)
{
    uint8_t policy_wire[VCS_ZCODE_PROOF_POLICY_WIRE_BYTES];
    static const char model_policy[] =
        "zcode-product-v0.1:bounded-context;candidate-only-write;human-accept";
    uint8_t model_root[32];
    if (vcs_zcode_proof_policy_serialize(&profile->policy, policy_wire) !=
        VCS_ZCODE_DEV_OK)
        return false;
    sha3_256((const uint8_t *)model_policy, sizeof(model_policy), model_root);
    char *lock_hex = zwork_hex_alloc(prepared->lock_wire,
                                     prepared->lock_wire_len,
                                     "zcode.work.lock_hex");
    char *recipe_hex = zwork_hex_alloc(prepared->recipe_wire,
                                       prepared->recipe_wire_len,
                                       "zcode.work.recipe_hex");
    char *policy_hex = zwork_hex_alloc(policy_wire, sizeof(policy_wire),
                                       "zcode.work.policy_hex");
    char model_hex[65]; zcl_hex_encode(model_root, 32, model_hex);
    json_init(input); json_set_object(input);
    bool ok = lock_hex && recipe_hex && policy_hex &&
        json_push_kv_str(input, "mode", "plan") &&
        json_push_kv_str(input, "workspace", workspace) &&
        json_push_kv_str(input, "dependency_lock_hex", lock_hex) &&
        json_push_kv_str(input, "write_scope_csv", scopes) &&
        json_push_kv_str(input, "acceptance_recipe_hex", recipe_hex) &&
        json_push_kv_str(input, "model_policy_root", model_hex) &&
        json_push_kv_str(input, "goal", goal) &&
        json_push_kv_str(input, "proof_policy_hex", policy_hex) &&
        json_push_kv_str(input, "context_symbol",
                         selection->selected_symbol_id) &&
        json_push_kv_int(input, "expires_unix", expires_unix) &&
        json_push_kv_int(input, "max_context_bytes", 256 * 1024);
    free(policy_hex); free(recipe_hex); free(lock_hex);
    return ok;
}

static bool zwork_render_selection(
    struct json_value *out, const struct zcode_goal_selection *selection,
    const struct json_value *plan_data, uint64_t total_source_bytes)
{
    json_init(out); json_set_object(out);
    const struct json_value *bytes =
        json_get(plan_data, "agent_context_excerpt_bytes");
    const struct json_value *files =
        json_get(plan_data, "agent_context_files");
    int64_t selected_bytes = bytes && bytes->type == JSON_INT
        ? json_get_int(bytes) : 0;
    return json_push_kv_str(out, "symbol", selection->selected.name) &&
        json_push_kv_str(out, "symbol_id", selection->selected_symbol_id) &&
        json_push_kv_str(out, "why", selection->why) &&
        json_push_kv_int(out, "selected_context_bytes", selected_bytes) &&
        json_push_kv_int(out, "total_source_bytes",
                         (int64_t)total_source_bytes) &&
        json_push_kv_int(out, "selected_file_count",
                         files && files->type == JSON_INT
                             ? json_get_int(files) : 0) &&
        json_push_kv_int(out, "ranked_candidate_count",
                         (int64_t)selection->candidate_count) &&
        json_push_kv_int(out, "dropped_candidate_count",
                         (int64_t)selection->dropped_candidates) &&
        json_push_kv_bool(out, "budget_exhausted",
                          selection->budget_exhausted) &&
        json_push_kv_int(out, "generation_us",
                         (int64_t)selection->generation_us);
}

void zcl_native_handle_zcode_work_start(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace = zwork_str(request->input, "workspace");
    const char *goal = zwork_str(request->input, "goal");
    const char *profile_name = zwork_str(request->input, "profile");
    const char *exact_symbol = zwork_str(request->input, "context_symbol");
    if (!profile_name || !profile_name[0]) profile_name = "standard";
    if (!workspace || !workspace[0] || !goal || !goal[0]) {
        zwork_fail(reply, "MISSING_INPUT", "validate",
                   "work start requires workspace and goal", false, false);
        return;
    }
    struct vcs_zcode_dev_profile profile;
    if (!vcs_zcode_dev_profile_expand(profile_name, &profile)) {
        zwork_fail(reply, "BAD_PROFILE", "validate",
                   "profile must be quick, standard, strong, or release",
                   false, false);
        return;
    }
    struct vcs_package_prepared prepared;
    char detail[256] = {0};
    if (!zwork_prepare(workspace, &prepared, detail, sizeof(detail))) {
        zwork_fail(reply, "PROJECT_INSPECT_FAILED", "inspect", detail,
                   false, false);
        return;
    }
    struct zcode_goal_selection selection;
    struct zcl_result selected = zcode_goal_context_select(
        workspace, goal, exact_symbol, &selection);
    char scopes[1024];
    uint64_t expires = 0;
    int64_t now = platform_time_wall_unix();
    bool composed = selected.ok && zwork_scopes(&prepared.recipe, scopes) &&
        now > 0 && zcl_u64_add((uint64_t)now, 86400u, &expires) &&
        expires <= INT64_MAX;
    if (!composed) {
        zwork_fail(reply, "CONTEXT_SELECTION_FAILED", "context",
                   selected.ok ? "project scopes or expiry could not be derived"
                               : selected.message,
                   false, false);
        vcs_package_prepared_free(&prepared);
        return;
    }
    struct json_value plan_input;
    if (!zwork_plan_input(&plan_input, workspace, goal, &prepared, &profile,
                          &selection, scopes, (int64_t)expires)) {
        zwork_fail(reply, "WORK_COMPOSE_FAILED", "compose",
                   "existing task inputs could not be composed", false, false);
        vcs_package_prepared_free(&prepared);
        return;
    }
    struct zcl_command_request inner_request = { .input = &plan_input };
    struct zcl_command_reply inner;
    zcl_command_reply_init(&inner, "zcl.zcode_improve.v1");
    zcl_native_handle_zcode_improve(&inner_request, &inner);
    json_free(&plan_input);
    if (inner.status != ZCL_COMMAND_STATUS_PASSED) {
        zwork_fail(reply, inner.error.code[0] ? inner.error.code : "WORK_PLAN_FAILED",
                   inner.error.phase[0] ? inner.error.phase : "plan",
                   inner.error.message[0] ? inner.error.message
                                          : "existing task planner refused",
                   inner.error.retryable, inner.error.mutated);
        zcl_command_reply_free(&inner);
        vcs_package_prepared_free(&prepared);
        return;
    }
    const struct json_value *task_root = json_get(&inner.data, "task_root");
    const char *task_hex = task_root ? json_get_str(task_root) : NULL;
    struct json_value context, expert;
    uint64_t source_bytes = zwork_source_bytes(&prepared);
    bool ok = task_hex && strlen(task_hex) == 64 && source_bytes != 0 &&
        zwork_render_selection(&context, &selection, &inner.data,
                               source_bytes);
    json_init(&expert);
    if (ok) json_copy(&expert, &inner.data);
    char work_id[32] = {0};
    if (ok) (void)snprintf(work_id, sizeof(work_id), "work-%.12s", task_hex);
    if (ok) {
        ok = json_push_kv_str(&reply->data, "work_id", work_id) &&
             json_push_kv_str(&reply->data, "goal", goal) &&
             json_push_kv_str(&reply->data, "state", "AWAITING_CANDIDATE") &&
             json_push_kv_str(&reply->data, "profile", profile.name) &&
             json_push_kv(&reply->data, "selected_context", &context) &&
             json_push_kv_str(&reply->data, "authoritative_workspace",
                              "unchanged") &&
             json_push_kv_str(&reply->data, "next_safe_command",
                              "zcode work run") &&
             json_push_kv(&reply->data, "expert", &expert);
    }
    json_free(&expert); json_free(&context);
    zcl_command_reply_free(&inner);
    vcs_package_prepared_free(&prepared);
    if (!ok)
        zwork_fail(reply, "WORK_OUTPUT_FAILED", "render",
                   "bounded human work summary could not be rendered",
                   false, true);
}

static const struct vcs_zcode_task_index_entry *zwork_resolve(
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

static char *zwork_load_goal(const char *workspace, const char *root_hex)
{
    uint8_t root[32], check[32], *bytes = NULL;
    size_t len = 0;
    if (!zcl_hex_decode_lower(root_hex, root, 32) ||
        vcs_object_load_raw(workspace, root, &bytes, &len) != 0 ||
        len == 0 || len > 4096 || memchr(bytes, '\0', len)) {
        free(bytes); return NULL;
    }
    sha3_256(bytes, len, check);
    if (memcmp(root, check, 32) != 0) { free(bytes); return NULL; }
    char *goal = zcl_malloc(len + 1u, "zcode.work.goal");
    if (!goal) { free(bytes); return NULL; }
    memcpy(goal, bytes, len); goal[len] = '\0'; free(bytes);
    return goal;
}

void zcl_native_handle_zcode_work_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace = zwork_str(request->input, "workspace");
    const char *work = zwork_str(request->input, "work");
    if (!workspace || !workspace[0]) workspace = ".";
    struct vcs_zcode_task_index *index = vcs_zcode_task_index_build(
        workspace, platform_time_wall_unix());
    if (!index) {
        zwork_fail(reply, "WORK_INDEX_FAILED", "rebuild",
                   "canonical task projection could not be rebuilt", true,
                   false);
        return;
    }
    bool ambiguous = false;
    const struct vcs_zcode_task_index_entry *entry =
        zwork_resolve(index, work, &ambiguous);
    char *goal = entry ? zwork_load_goal(workspace, entry->goal_root_hex) : NULL;
    if (!entry || !goal) {
        zwork_fail(reply, ambiguous ? "AMBIGUOUS_WORK" : "WORK_NOT_FOUND",
                   "resolve", ambiguous
                     ? "work prefix resolves to more than one canonical task"
                     : "no verified canonical task matches this work alias",
                   false, false);
        free(goal); vcs_zcode_task_index_free(index); return;
    }
    char work_id[32];
    (void)snprintf(work_id, sizeof(work_id), "work-%.12s",
                   entry->task_root_hex);
    const char *state = entry->expired ? "BLOCKED" : entry->state;
    bool evidence_ready = strcmp(state, VCS_ZCODE_TASK_STATE_EVIDENCE_READY) == 0;
    struct json_value expert;
    json_init(&expert); json_set_object(&expert);
    bool ok = json_push_kv_str(&expert, "task_root", entry->task_root_hex) &&
        json_push_kv_str(&expert, "source_root", entry->source_root_hex) &&
        json_push_kv_str(&expert, "goal_root", entry->goal_root_hex) &&
        json_push_kv_str(&expert, "proof_policy_root",
                         entry->proof_policy_root_hex) &&
        json_push_kv_str(&expert, "toolchain_capsule_root",
                         entry->toolchain_capsule_root_hex) &&
        json_push_kv_str(&expert, "work_receipt_root",
                         entry->latest_work_receipt_hex) &&
        json_push_kv_str(&reply->data, "work_id", work_id) &&
        json_push_kv_str(&reply->data, "goal", goal) &&
        json_push_kv_str(&reply->data, "state", state) &&
        json_push_kv_int(&reply->data, "changed_files", 0) &&
        json_push_kv_str(&reply->data, "public_api_changes", "not_yet_available") &&
        json_push_kv_str(&reply->data, "build_result",
                         evidence_ready ? "passed" : "not_started") &&
        json_push_kv_str(&reply->data, "test_result",
                         evidence_ready ? "passed_declared_tests" :
                                          "not_started") &&
        json_push_kv_str(&reply->data, "sanitizer_result", "not_started") &&
        json_push_kv_str(&reply->data, "fuzz_result", "not_started") &&
        json_push_kv_str(&reply->data, "reproduction_grade", "none") &&
        json_push_kv_str(&reply->data, "review_verdict", "not_started") &&
        json_push_kv_str(&reply->data, "remaining_risks",
                         entry->expired ? "task expired" : evidence_ready
                            ? "proof profile and independent review not yet evaluated"
                            : "candidate not admitted") &&
        json_push_kv_int(&reply->data, "scope_violations", 0) &&
        json_push_kv_str(&reply->data, "next_safe_command",
                         entry->expired ? "zcode work start" : "zcode work run") &&
        json_push_kv(&reply->data, "expert", &expert);
    json_free(&expert); free(goal); vcs_zcode_task_index_free(index);
    if (!ok)
        zwork_fail(reply, "WORK_STATUS_OUTPUT", "render",
                   "human work status could not be rendered", false, false);
}
