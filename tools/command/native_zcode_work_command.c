/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: human-first orchestration over existing ZCODE development owners.
 * HOT_FORK strictly exercises caller-owned input normalization and byte totals. */

#include "base/checked.h"
#include "json/json.h"
#include "vcs/package_prepare.h"

#ifndef ZCL_HOTFORK_ZWORK_INPUT_CORE
#include "command/native_command.h"

#include "base/cleanse.h"
#include "base/hex.h"
#include "crypto/ed25519.h"
#include "hotswap/hotswap_service.h"
#include "models/build_fabric.h"
#include "models/database.h"
#include "platform/time_compat.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_worker.h"
#include "services/zcode_goal_context_calc_service.h"
#include "services/zcode_goal_context_service.h"
#include "services/zcode_lane_service.h"
#include "sha3/sha3.h"
#include "util/safe_alloc.h"
#include "util/file_tree_ops.h"
#include "vcs/package_recipe.h"
#include "vcs/build_action.h"
#include "vcs/vcs.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev_product.h"
#include "vcs/zcode_patch.h"
#include "vcs/zcode_task_index.h"
#include "vcs/zcode_work_swarm.h"
#endif

#include <stdio.h>
#include <string.h>
#ifndef ZCL_HOTFORK_ZWORK_INPUT_CORE
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#ifndef ZCL_HOTFORK_ZWORK_INPUT_CORE
#define ZWORK_PATH_MAX 4400
#define ZWORK_LINE_COUNT_MAX 65536u

struct zwork_patch_summary {
    struct vcs_zcode_patch_v1 patch;
    uint64_t added_lines;
    uint64_t deleted_lines;
    size_t public_api_changes;
    bool line_counts_exact;
};

struct zwork_proof_snapshot {
    bool available;
    struct build_fabric_proof_evaluation facts;
};
#endif

static const char *zwork_str(const struct json_value *input, const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

#ifndef ZCL_HOTFORK_ZWORK_INPUT_CORE
static bool zwork_open_build_ledger(
    struct node_db *ndb, const char *path, const char *reason,
    bool allow_create)
{
    if (!ndb || !path || !path[0] || !reason || !reason[0]) return false;
    if (access(path, F_OK) == 0)
        return node_db_open_existing_runtime(ndb, path, reason);
    return allow_create && node_db_open(ndb, path);
}
#endif

static int64_t zwork_int(
    const struct json_value *input, const char *key, int64_t fallback)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_INT ? json_get_int(value) : fallback;
}

#ifndef ZCL_HOTFORK_ZWORK_INPUT_CORE
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
#endif

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

#ifndef ZCL_HOTFORK_ZWORK_INPUT_CORE
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
    int64_t expires_unix, int64_t max_cpu_seconds)
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
        json_push_kv_int(input, "max_cpu_seconds", max_cpu_seconds) &&
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
                         (int64_t)selection->generation_us) &&
        json_push_kv_int(out, "context_service_generation",
                         selection->service_generation);
}

void zcl_native_handle_zcode_work_context(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !request->input ||
        request->input->type != JSON_OBJ || request->input->num_children != 0) {
        if (reply) zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_WORK_CONTEXT_INPUT", "status", false, false,
            "zcode work context accepts no input keys", "zcode.work.context");
        return;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_goal_context_calc_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_GOAL_CONTEXT_CALC_SERVICE_ID, &lease);
    if (!service) service = zcode_goal_context_calc_service_builtin();
    struct zcode_goal_context_view_v1 view;
    bool rendered = service->render_status(&view) && view.valid &&
        view.capability[0] && view.next_action[0];
    zcl_hotswap_service_release(&lease);
    if (!rendered) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "WORK_CONTEXT_VIEW_FAILED", "render", false, false,
            "the pure goal-context view refused bounded status",
            "zcode.work.context");
        return;
    }
    (void)json_push_kv_bool(&reply->data, "ready", true);
    (void)json_push_kv_str(&reply->data, "capability", view.capability);
    (void)json_push_kv_str(&reply->data, "service_id",
                           ZCODE_GOAL_CONTEXT_CALC_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_bool(&reply->data, "codeindex_reads_static", true);
    (void)json_push_kv_bool(&reply->data, "clock_measurement_static", true);
    (void)json_push_kv_bool(&reply->data, "workspace_writes_swappable", false);
    (void)json_push_kv_bool(&reply->data, "task_creation_swappable", false);
    (void)json_push_kv_str(&reply->data, "agent_next_action",
                           view.next_action);
}

void zcl_native_handle_zcode_work_start(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace = zwork_str(request->input, "workspace");
    const char *goal = zwork_str(request->input, "goal");
    const char *profile_name = zwork_str(request->input, "profile");
    const char *exact_symbol = zwork_str(request->input, "context_symbol");
    int64_t max_cpu_seconds = zwork_int(
        request->input, "max_cpu_seconds", 600);
    if (!profile_name || !profile_name[0]) profile_name = "standard";
    if (!workspace || !workspace[0] || !goal || !goal[0] ||
        max_cpu_seconds <= 0 || max_cpu_seconds > 600) {
        zwork_fail(reply, "MISSING_INPUT", "validate",
                   "work start requires workspace/goal and max_cpu_seconds in 1..600",
                   false, false);
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
                          &selection, scopes, (int64_t)expires,
                          max_cpu_seconds)) {
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

static int zwork_line_hash_compare(const void *left, const void *right)
{
    return memcmp(left, right, 32);
}

static bool zwork_line_hashes(const uint8_t *bytes, size_t len,
                              uint8_t **out, size_t *count_out,
                              bool *text_out)
{
    *out = NULL; *count_out = 0; *text_out = false;
    if (len > 0 && (!bytes || memchr(bytes, '\0', len))) return true;
    size_t count = 0;
    for (size_t i = 0; i < len; i++)
        if (bytes[i] == '\n') count++;
    if (len > 0 && bytes[len - 1u] != '\n') count++;
    if (count > ZWORK_LINE_COUNT_MAX) return true;
    uint8_t *hashes = count > 0
        ? zcl_malloc(count * 32u, "zcode.work.line_hashes") : NULL;
    if (count > 0 && !hashes) return false;
    size_t start = 0, at = 0;
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != '\n') continue;
        sha3_256(bytes + start, i + 1u - start, hashes + at++ * 32u);
        start = i + 1u;
    }
    if (start < len)
        sha3_256(bytes + start, len - start, hashes + at++ * 32u);
    if (at != count) { free(hashes); return false; }
    if (count > 1)
        qsort(hashes, count, 32u, zwork_line_hash_compare);
    *out = hashes; *count_out = count; *text_out = true;
    return true;
}

static bool zwork_blob(const char *workspace, const uint8_t root[32],
                       uint8_t **bytes, size_t *len)
{
    return vcs_object_get(workspace, root, VCS_TAG_BLOB, bytes, len) == 0;
}

static bool zwork_line_delta(const char *workspace,
                             const struct vcs_zcode_patch_change_v1 *change,
                             uint64_t *added, uint64_t *deleted,
                             bool *exact)
{
    uint8_t *old_bytes = NULL, *new_bytes = NULL;
    size_t old_len = 0, new_len = 0;
    bool have_old = change->kind != VCS_DIFF_ADDED;
    bool have_new = change->kind != VCS_DIFF_REMOVED;
    if ((have_old && !zwork_blob(workspace, change->old_blob,
                                 &old_bytes, &old_len)) ||
        (have_new && !zwork_blob(workspace, change->new_blob,
                                 &new_bytes, &new_len))) {
        free(new_bytes); free(old_bytes); return false;
    }
    uint8_t *old_hashes = NULL, *new_hashes = NULL;
    size_t old_count = 0, new_count = 0;
    bool old_text = true, new_text = true;
    bool ok = (!have_old || zwork_line_hashes(
                   old_bytes, old_len, &old_hashes, &old_count, &old_text)) &&
        (!have_new || zwork_line_hashes(
                   new_bytes, new_len, &new_hashes, &new_count, &new_text));
    free(new_bytes); free(old_bytes);
    if (!ok) { free(new_hashes); free(old_hashes); return false; }
    if (!old_text || !new_text) {
        *exact = false; free(new_hashes); free(old_hashes); return true;
    }
    size_t oi = 0, ni = 0, common = 0;
    while (oi < old_count && ni < new_count) {
        int cmp = memcmp(old_hashes + oi * 32u, new_hashes + ni * 32u, 32u);
        if (cmp < 0) oi++;
        else if (cmp > 0) ni++;
        else { common++; oi++; ni++; }
    }
    *deleted += old_count - common;
    *added += new_count - common;
    free(new_hashes); free(old_hashes); return true;
}

static bool zwork_recipe_load(const char *workspace, const char *root_hex,
                              struct vcs_package_recipe *recipe)
{
    uint8_t root[32], checked[32], *wire = NULL;
    size_t len = 0;
    bool ok = zcl_hex_decode_lower(root_hex, root, sizeof(root)) &&
        vcs_object_load_raw_bounded(workspace, root,
                                    VCS_PACKAGE_RECIPE_MAX_WIRE_BYTES,
                                    &wire, &len) == 0 &&
        vcs_package_recipe_parse(wire, len, recipe) == VCS_PACKAGE_RECIPE_OK &&
        vcs_package_recipe_root(recipe, checked) == VCS_PACKAGE_RECIPE_OK &&
        memcmp(root, checked, sizeof(root)) == 0;
    free(wire); return ok;
}

static bool zwork_is_public_header(const struct vcs_package_recipe *recipe,
                                   const char *path)
{
    for (size_t i = 0; i < recipe->public_headers.count; i++)
        if (strcmp(recipe->public_headers.items[i], path) == 0) return true;
    return false;
}

static bool zwork_patch_summary_load(
    const char *workspace, const struct vcs_zcode_task_index_entry *entry,
    struct zwork_patch_summary *out)
{
    memset(out, 0, sizeof(*out));
    vcs_zcode_patch_init(&out->patch);
    out->line_counts_exact = true;
    if (!entry->latest_patch_root_hex[0]) return true;
    uint8_t root[32], checked[32], *wire = NULL;
    size_t len = 0;
    bool ok = zcl_hex_decode_lower(entry->latest_patch_root_hex, root,
                                   sizeof(root)) &&
        vcs_object_load_raw_bounded(workspace, root,
                                    VCS_ZCODE_TASK_MAX_PATCH_BYTES,
                                    &wire, &len) == 0 &&
        vcs_zcode_patch_parse(wire, len, &out->patch) == VCS_ZCODE_PATCH_OK &&
        vcs_zcode_patch_root(&out->patch, checked) == VCS_ZCODE_PATCH_OK &&
        memcmp(root, checked, sizeof(root)) == 0;
    free(wire);
    struct vcs_package_recipe recipe;
    vcs_package_recipe_init(&recipe);
    ok = ok && zwork_recipe_load(workspace, entry->acceptance_tests_root_hex,
                                 &recipe);
    if (!ok) {
        vcs_package_recipe_free(&recipe);
        vcs_zcode_patch_free(&out->patch);
        return false;
    }
    for (size_t i = 0; i < out->patch.count && ok; i++) {
        const struct vcs_zcode_patch_change_v1 *change =
            &out->patch.changes[i];
        ok = zwork_line_delta(workspace, change, &out->added_lines,
                              &out->deleted_lines,
                              &out->line_counts_exact);
        if (zwork_is_public_header(&recipe, change->path))
            out->public_api_changes++;
    }
    vcs_package_recipe_free(&recipe);
    if (!ok) vcs_zcode_patch_free(&out->patch);
    return ok;
}

static bool zwork_policy_load(
    const char *workspace, const char *root_hex,
    struct vcs_zcode_proof_policy_v1 *policy)
{
    uint8_t root[32], checked[32], *wire = NULL;
    size_t wire_len = 0;
    bool ok = policy && zcl_hex_decode_lower(root_hex, root, 32) &&
        vcs_object_load_raw_bounded(
            workspace, root, VCS_ZCODE_PROOF_POLICY_WIRE_BYTES,
            &wire, &wire_len) == 0 &&
        vcs_zcode_proof_policy_parse(wire, wire_len, policy) ==
            VCS_ZCODE_DEV_OK &&
        vcs_zcode_proof_policy_root(policy, checked) == VCS_ZCODE_DEV_OK &&
        memcmp(root, checked, sizeof(root)) == 0;
    free(wire);
    return ok;
}

/* Human status is a projection only. Re-evaluate the exact action and receipt
 * bytes without storing a proof set or promoting receipt trust. */
static void zwork_proof_snapshot_read(
    const char *workspace, const char *task_root, const char *action_id,
    int64_t now, struct zwork_proof_snapshot *out)
{
    memset(out, 0, sizeof(*out));
    if (!workspace || !task_root || strlen(task_root) != 64u ||
        !action_id || strlen(action_id) != 64u)
        return;
    char db_path[ZWORK_PATH_MAX];
    int n = snprintf(db_path, sizeof(db_path),
                     "/tmp/zclassic23-zcode-workspaces/%lu/%.64s/zbuild/node.db",
                     (unsigned long)getuid(), task_root);
    if (n <= 0 || (size_t)n >= sizeof(db_path) ||
        access(db_path, F_OK) != 0)
        return;
    struct node_db ndb = {0};
    if (!node_db_open_existing_runtime(
            &ndb, db_path, "zcode.work.status.proof"))
        return;
    struct zcl_result result = build_fabric_proof_evaluate_readonly(
        &ndb, workspace, action_id, now, &out->facts);
    node_db_close(&ndb);
    out->available = result.ok;
}

static bool zwork_proof_required(
    const struct vcs_zcode_proof_policy_v1 *policy, uint32_t kind,
    uint16_t minimum)
{
    return (policy->required_proofs & kind) != 0 || minimum > 0;
}

static bool zwork_confirmation_identity(
    const struct vcs_zcode_task_index_entry *entry,
    const struct zwork_proof_snapshot *proof, char identity[65])
{
    identity[0] = '\0';
    if (!entry || !proof || !proof->available ||
        !proof->facts.policy_satisfied ||
        !proof->facts.proof_set_root_sha3[0])
        return false;
    uint8_t task[32], candidate[32], policy[32], proof_set[32], plan[32];
    if (!zcl_hex_decode_lower(entry->task_root_hex, task, sizeof(task)) ||
        !zcl_hex_decode_lower(entry->latest_candidate_root_hex, candidate,
                              sizeof(candidate)) ||
        !zcl_hex_decode_lower(entry->proof_policy_root_hex, policy,
                              sizeof(policy)) ||
        !zcl_hex_decode_lower(proof->facts.proof_set_root_sha3, proof_set,
                              sizeof(proof_set)) ||
        vcs_zcode_acceptance_plan_root(task, candidate, policy, proof_set,
                                       plan) != VCS_ZCODE_DEV_OK)
        return false;
    zcl_hex_encode(plan, sizeof(plan), identity);
    return true;
}

void zcl_native_handle_zcode_work_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace = zwork_str(request->input, "workspace");
    const char *work = zwork_str(request->input, "work");
    if (!workspace || !workspace[0]) workspace = ".";
    int64_t now = platform_time_wall_unix();
    struct vcs_zcode_task_index *index = vcs_zcode_task_index_build(
        workspace, now);
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
    bool repair_needed = strcmp(state, VCS_ZCODE_TASK_STATE_REPAIR_NEEDED) == 0;
    bool accepted = strcmp(state, VCS_ZCODE_TASK_STATE_PROVEN) == 0;
    struct zwork_patch_summary summary;
    struct vcs_zcode_proof_policy_v1 policy;
    if (!zwork_patch_summary_load(workspace, entry, &summary) ||
        !zwork_policy_load(workspace, entry->proof_policy_root_hex,
                           &policy)) {
        zwork_fail(reply, "PATCH_SUMMARY_FAILED", "rebuild",
                   "latest canonical patch or its source blobs could not be reverified",
                   false, false);
        vcs_zcode_patch_free(&summary.patch);
        free(goal); vcs_zcode_task_index_free(index); return;
    }
    struct zwork_proof_snapshot proof;
    zwork_proof_snapshot_read(workspace, entry->task_root_hex,
                              entry->latest_action_root_hex, now, &proof);
    char confirmation_identity[65];
    bool confirmation_ready = zwork_confirmation_identity(
        entry, &proof, confirmation_identity);
    bool compile_required = zwork_proof_required(
        &policy, VCS_ZCODE_PROOF_COMPILE,
        policy.minimum_compile_receipts > policy.minimum_matching_receipts
            ? policy.minimum_compile_receipts
            : policy.minimum_matching_receipts);
    bool test_required = zwork_proof_required(
        &policy, VCS_ZCODE_PROOF_TEST, policy.minimum_test_receipts);
    bool fuzz_required = zwork_proof_required(
        &policy, VCS_ZCODE_PROOF_FUZZ, policy.minimum_fuzz_receipts);
    bool review_required = zwork_proof_required(
        &policy, VCS_ZCODE_PROOF_REVIEW, policy.minimum_reviews);
    bool clean_shadow_required =
        (policy.required_proofs & VCS_ZCODE_PROOF_LOCAL_REPRODUCTION) != 0;
    bool standard_evidence = policy.minimum_compile_receipts >= 2u ||
                             policy.minimum_test_receipts >= 2u;
    size_t sanitizer_receipts = standard_evidence && proof.available
        ? proof.facts.compile_receipts : 0;
    bool sanitizer_satisfied = standard_evidence && proof.available &&
        proof.facts.compile_satisfied;
    const char *build_summary = repair_needed ? "failed" :
        !entry->latest_action_root_hex[0] ? "not_started" :
        !proof.available ? "unknown" : proof.facts.compile_satisfied
            ? "passed" : proof.facts.compile_receipts > 0
                ? "pending_policy" : "not_started";
    const char *test_summary = repair_needed ? "failed_or_not_reached" :
        !test_required ? "not_required" : !proof.available ? "unknown" :
        proof.facts.test_satisfied ? "passed_declared_tests" :
        proof.facts.test_receipts > 0 ? "pending_policy" : "not_started";
    const char *fuzz_summary = !fuzz_required ? "not_required" :
        !proof.available ? "unknown" : proof.facts.fuzz_satisfied
            ? "passed_declared_fuzz" : proof.facts.fuzz_receipts > 0
                ? "pending_policy" : "not_started";
    const char *sanitizer_summary = !standard_evidence ? "not_required" :
        repair_needed ? "failed_or_unavailable" :
        !entry->latest_action_root_hex[0] ? "not_started" :
        !proof.available ? "unknown" : sanitizer_satisfied
            ? "passed_asan_ubsan" : sanitizer_receipts > 0
                ? "pending_policy" : "not_started";
    const char *reproduction_grade = !entry->latest_action_root_hex[0]
        ? "none" : !proof.available ? "unknown" :
        proof.facts.local_reproduced ? "clean_shadow_matched" :
        proof.facts.quorum_satisfied ? "approved_signer_threshold" :
        proof.facts.valid_receipts > 0 ? "pending" : "none";
    const char *next_safe_command = entry->expired ? "zcode work start" :
        accepted ? "apply or reject the accepted patch in source control" :
        repair_needed && entry->candidate_count >= 3u ? "zcode work start" :
        repair_needed || !entry->latest_action_root_hex[0]
            ? "zcode work run" :
        confirmation_ready ? "ask user to confirm exact candidate" :
        proof.available && review_required &&
            proof.facts.compile_satisfied && proof.facts.test_satisfied &&
            proof.facts.fuzz_satisfied && !proof.facts.review_satisfied
            ? "zcode work review" : "zcode work status";
    const char *next_action = entry->expired ? "Start a fresh bounded task." :
        accepted ? "Decide whether to apply or reject the accepted patch." :
        repair_needed ? "Repair the named candidate failure." :
        !entry->latest_action_root_hex[0] ? "Produce one bounded candidate." :
        confirmation_ready
            ? "Ask the user to confirm or cancel this exact candidate." :
        proof.available && review_required &&
            proof.facts.compile_satisfied && proof.facts.test_satisfied &&
            proof.facts.fuzz_satisfied && !proof.facts.review_satisfied
            ? "Review the exact candidate evidence." :
              "Keep thinking while the missing proof arrives.";
    char remaining_risks[256];
    if (entry->expired)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "task expired");
    else if (repair_needed)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "latest candidate failed confined package build or tests");
    else if (accepted)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "accepted candidate is not applied, release-qualified, or published");
    else if (!entry->latest_action_root_hex[0])
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "candidate not admitted");
    else if (!proof.available)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "canonical proof ledger is unavailable; no proof result inferred");
    else if (compile_required && !proof.facts.compile_satisfied)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "required compile or matching-build evidence is pending");
    else if (test_required && !proof.facts.test_satisfied)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "required candidate behavior evidence is pending");
    else if (fuzz_required && !proof.facts.fuzz_satisfied)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "required deterministic fuzz evidence is pending");
    else if (review_required && !proof.facts.review_satisfied)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "required independent review is pending");
    else if (clean_shadow_required && !proof.facts.local_reproduced)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "required clean-shadow observation is pending");
    else if (!proof.facts.release_identity_satisfied)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "release byte-identity evidence is not satisfied");
    else if (proof.facts.policy_satisfied)
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "human acceptance, secure release qualification, and publication remain separate");
    else
        (void)snprintf(remaining_risks, sizeof(remaining_risks),
                       "proof policy remains unsatisfied by canonical evidence");
    struct json_value expert, changed_paths, proof_json;
    json_init(&expert); json_set_object(&expert);
    json_init(&changed_paths); json_set_array(&changed_paths);
    json_init(&proof_json); json_set_object(&proof_json);
    bool paths_ok = true;
    for (size_t i = 0; paths_ok && i < summary.patch.count; i++) {
        struct json_value path;
        json_init(&path); json_set_str(&path, summary.patch.changes[i].path);
        paths_ok = json_push_back(&changed_paths, &path);
        json_free(&path);
    }
    char api_summary[96];
    if (summary.public_api_changes == 0)
        (void)snprintf(api_summary, sizeof(api_summary), "none");
    else
        (void)snprintf(api_summary, sizeof(api_summary), "%zu public header file%s changed",
                       summary.public_api_changes,
                       summary.public_api_changes == 1 ? "" : "s");
    const char *review_summary = entry->latest_review_verdict ==
            VCS_ZCODE_REVIEW_APPROVE ? "approve" :
        entry->latest_review_verdict == VCS_ZCODE_REVIEW_REQUEST_CHANGES
            ? "request_changes" :
        entry->latest_review_verdict == VCS_ZCODE_REVIEW_REJECT
            ? "reject" : "not_started";
    bool proof_ok =
        json_push_kv_bool(&proof_json, "facts_available", proof.available) &&
        json_push_kv_int(&proof_json, "valid_receipts",
                         (int64_t)proof.facts.valid_receipts) &&
        json_push_kv_int(&proof_json, "compile_receipts",
                         (int64_t)proof.facts.compile_receipts) &&
        json_push_kv_int(&proof_json, "test_receipts",
                         (int64_t)proof.facts.test_receipts) &&
        json_push_kv_int(&proof_json, "sanitizer_receipts",
                         (int64_t)sanitizer_receipts) &&
        json_push_kv_int(&proof_json, "fuzz_receipts",
                         (int64_t)proof.facts.fuzz_receipts) &&
        json_push_kv_int(&proof_json, "review_receipts",
                         (int64_t)proof.facts.review_receipts) &&
        json_push_kv_int(&proof_json, "approved_distinct_signers",
                         (int64_t)proof.facts.approved_distinct_signers) &&
        json_push_kv_bool(&proof_json, "clean_shadow_observed",
                          proof.facts.local_reproduced) &&
        json_push_kv_bool(&proof_json, "signer_threshold_satisfied",
                          proof.facts.quorum_satisfied) &&
        json_push_kv_bool(&proof_json, "compile_satisfied",
                          proof.facts.compile_satisfied) &&
        json_push_kv_bool(&proof_json, "test_satisfied",
                          proof.facts.test_satisfied) &&
        json_push_kv_bool(&proof_json, "sanitizer_satisfied",
                          sanitizer_satisfied) &&
        json_push_kv_bool(&proof_json, "fuzz_satisfied",
                          proof.facts.fuzz_satisfied) &&
        json_push_kv_bool(&proof_json, "review_satisfied",
                          proof.facts.review_satisfied) &&
        json_push_kv_bool(&proof_json, "policy_satisfied",
                          proof.facts.policy_satisfied) &&
        json_push_kv_str(&proof_json, "authority",
                         "canonical_readonly_receipt_evaluation");
    const char *proof_set_root = proof.available &&
        proof.facts.proof_set_root_sha3[0]
            ? proof.facts.proof_set_root_sha3
            : entry->latest_proof_set_root_hex;
    bool ok = paths_ok && proof_ok &&
        json_push_kv_str(&expert, "task_root", entry->task_root_hex) &&
        json_push_kv_str(&expert, "source_root", entry->source_root_hex) &&
        json_push_kv_str(&expert, "goal_root", entry->goal_root_hex) &&
        json_push_kv_str(&expert, "proof_policy_root",
                         entry->proof_policy_root_hex) &&
        json_push_kv_str(&expert, "toolchain_capsule_root",
                         entry->toolchain_capsule_root_hex) &&
        json_push_kv_str(&expert, "action_id",
                         entry->latest_action_root_hex) &&
        json_push_kv_str(&expert, "candidate_root",
                         entry->latest_candidate_root_hex) &&
        json_push_kv_str(&expert, "patch_root",
                         entry->latest_patch_root_hex) &&
        json_push_kv_str(&expert, "work_receipt_root",
                         entry->latest_work_receipt_hex) &&
        json_push_kv_str(&expert, "lane_receipt_root",
                         entry->latest_lane_receipt_hex) &&
        json_push_kv_str(&expert, "proof_set_root",
                         proof_set_root) &&
        json_push_kv_str(&expert, "review_root",
                         entry->latest_review_root_hex) &&
        json_push_kv_str(&reply->data, "work_id", work_id) &&
        json_push_kv_str(&reply->data, "goal", goal) &&
        json_push_kv_str(&reply->data, "state", state) &&
        json_push_kv_int(&reply->data, "changed_files",
                         (int64_t)summary.patch.count) &&
        json_push_kv(&reply->data, "changed_paths", &changed_paths) &&
        json_push_kv_int(&reply->data, "added_lines",
                         (int64_t)summary.added_lines) &&
        json_push_kv_int(&reply->data, "deleted_lines",
                         (int64_t)summary.deleted_lines) &&
        json_push_kv_bool(&reply->data, "line_counts_complete",
                          summary.line_counts_exact) &&
        json_push_kv_str(&reply->data, "public_api_changes", api_summary) &&
        json_push_kv_str(&reply->data, "build_result",
                         build_summary) &&
        json_push_kv_str(&reply->data, "test_result",
                         test_summary) &&
        json_push_kv_str(&reply->data, "sanitizer_result",
                         sanitizer_summary) &&
        json_push_kv_str(&reply->data, "fuzz_result", fuzz_summary) &&
        json_push_kv_str(&reply->data, "reproduction_grade",
                         reproduction_grade) &&
        json_push_kv_str(&reply->data, "review_verdict", review_summary) &&
        json_push_kv_str(&reply->data, "remaining_risks",
                         remaining_risks) &&
        json_push_kv_bool(&reply->data, "confirmation_ready",
                          confirmation_ready) &&
        json_push_kv_str(&reply->data, "confirmation_identity",
                         confirmation_identity) &&
        json_push_kv_str(&reply->data, "confirmation_effect",
                         confirmation_ready
                           ? "advance exact candidate to PROVEN; do not apply, sign, or publish"
                           : "none") &&
        json_push_kv_int(&reply->data, "scope_violations", 0) &&
        json_push_kv_str(&reply->data, "next_action", next_action) &&
        json_push_kv_str(&reply->data, "next_safe_command",
                         next_safe_command) &&
        json_push_kv(&reply->data, "proof", &proof_json) &&
        json_push_kv(&reply->data, "expert", &expert);
    json_free(&changed_paths); json_free(&proof_json); json_free(&expert);
    vcs_zcode_patch_free(&summary.patch);
    free(goal); vcs_zcode_task_index_free(index);
    if (!ok || !paths_ok)
        zwork_fail(reply, "WORK_STATUS_OUTPUT", "render",
                   "human work status could not be rendered", false, false);
}

static void zwork_accept_inner(const char *workspace, const char *datadir,
                               const char *action_id, const char *lane,
                               struct zcl_command_reply *reply)
{
    int target = lane && strcmp(lane, "CANDIDATE") == 0
        ? VCS_ZCODE_LANE_CANDIDATE
        : lane && strcmp(lane, "PROVEN") == 0
            ? VCS_ZCODE_LANE_PROVEN : 0;
    char db_path[ZWORK_PATH_MAX];
    int n = snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb = {0};
    struct db_build_worker signer;
    uint8_t secret[32] = {0}, pubkey[32] = {0};
    struct zcode_lane_status status;
    bool opened = target != 0 && n > 0 && (size_t)n < sizeof(db_path) &&
        zwork_open_build_ledger(
            &ndb, db_path, "zcode.work.accept", false);
    struct zcl_result result = opened
        ? build_fabric_worker_identity_load(
              datadir, &signer, secret, pubkey)
        : ZCL_ERR(-1, "human acceptance ledger could not be opened");
    if (result.ok)
        result = zcode_lane_advance(
            &ndb, workspace, action_id, target,
            (int64_t)platform_time_wall_unix(), secret, pubkey, &status);
    memory_cleanse(secret, sizeof(secret));
    if (opened) node_db_close(&ndb);
    if (!result.ok) {
        zwork_fail(reply, "LANE_PROMOTION_REFUSED", "accept",
                   result.message, false, false);
        return;
    }
    (void)json_push_kv_str(&reply->data, "lane", status.lane_name);
    (void)json_push_kv_str(&reply->data, "source_root",
                           status.source_root_sha3);
    (void)json_push_kv_str(&reply->data, "task_root",
                           status.task_root_sha3);
    (void)json_push_kv_str(&reply->data, "candidate_root",
                           status.candidate_root_sha3);
    (void)json_push_kv_str(&reply->data, "proof_policy_root",
                           status.proof_policy_root_sha3);
    (void)json_push_kv_str(&reply->data, "proof_set_root",
                           status.proof_set_root_sha3);
    (void)json_push_kv_str(&reply->data, "lane_receipt_root",
                           status.receipt_root_sha3);
    (void)json_push_kv_str(&reply->data, "prior_lane_receipt_root",
                           status.prior_receipt_root_sha3);
    (void)json_push_kv_str(&reply->data, "signer_pubkey",
                           status.signer_pubkey);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

static void zwork_lane_inner(const char *workspace, const char *datadir,
                             const char *source_root,
                             struct zcl_command_reply *reply)
{
    struct json_value input;
    json_init(&input); json_set_object(&input);
    bool ok = json_push_kv_str(&input, "workspace", workspace) &&
        json_push_kv_str(&input, "datadir", datadir) &&
        json_push_kv_str(&input, "source_root", source_root);
    if (ok) {
        struct zcl_command_request request = { .input = &input };
        zcl_native_handle_zcode_lane(&request, reply);
    }
    json_free(&input);
    if (!ok)
        zwork_fail(reply, "LANE_INPUT_FAILED", "compose",
                   "existing lane lookup input could not be composed",
                   false, false);
}

static void zwork_evidence_inner(const char *workspace, const char *datadir,
                                 const char *action_id,
                                 struct zcl_command_reply *reply)
{
    struct json_value input;
    json_init(&input); json_set_object(&input);
    bool ok = json_push_kv_str(&input, "workspace", workspace) &&
        json_push_kv_str(&input, "datadir", datadir) &&
        json_push_kv_str(&input, "action_id", action_id);
    if (ok) {
        struct zcl_command_request request = { .input = &input };
        zcl_native_handle_zcode_evidence(&request, reply);
    }
    json_free(&input);
    if (!ok)
        zwork_fail(reply, "EVIDENCE_INPUT_FAILED", "compose",
                   "existing evidence evaluation input could not be composed",
                   false, false);
}

static bool zwork_review_load_objects(
    const char *workspace, const struct vcs_zcode_task_index_entry *entry,
    struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_candidate_v1 *candidate)
{
    uint8_t root[32], *wire = NULL;
    size_t wire_len = 0;
    bool ok = zcl_hex_decode_lower(entry->task_root_hex, root, 32) &&
        vcs_object_load_raw(workspace, root, &wire, &wire_len) == 0 &&
        vcs_zcode_task_parse(wire, wire_len, task) == VCS_ZCODE_DEV_OK;
    free(wire); wire = NULL; wire_len = 0;
    if (!ok || !zcl_hex_decode_lower(entry->latest_candidate_root_hex,
                                     root, 32) ||
        vcs_object_load_raw(workspace, root, &wire, &wire_len) != 0 ||
        vcs_zcode_candidate_parse(wire, wire_len, candidate) !=
            VCS_ZCODE_DEV_OK) {
        free(wire);
        return false;
    }
    free(wire);
    return vcs_zcode_candidate_validate_for_task(
               task, candidate, platform_time_wall_unix()) ==
           VCS_ZCODE_DEV_OK;
}

static bool zwork_review_action(
    struct node_db *ndb, const struct db_build_action *base,
    const struct db_build_job *base_job, int64_t now,
    struct db_build_action *review)
{
    memset(review, 0, sizeof(*review));
    review->sequence = base->sequence + 1;
    (void)snprintf(review->kind, sizeof(review->kind), "%s",
                   VCS_BUILD_ACTION_KIND_REVIEW_V1);
    (void)snprintf(review->state, sizeof(review->state), "SNAPSHOTTED");
    (void)snprintf(review->input_root_sha3,
                   sizeof(review->input_root_sha3), "%s",
                   base->candidate_root_sha3);
    (void)snprintf(review->task_root_sha3,
                   sizeof(review->task_root_sha3), "%s",
                   base->task_root_sha3);
    (void)snprintf(review->candidate_root_sha3,
                   sizeof(review->candidate_root_sha3), "%s",
                   base->candidate_root_sha3);
    (void)snprintf(review->proof_policy_root_sha3,
                   sizeof(review->proof_policy_root_sha3), "%s",
                   base->proof_policy_root_sha3);
    (void)snprintf(review->target, sizeof(review->target), "%s",
                   VCS_BUILD_TARGET_V1);
    uint8_t flags[32], environment[32];
    if (!vcs_build_action_v1_fixed_flags_root_for_kind(
            review->kind, flags) ||
        !vcs_build_action_v1_fixed_environment_root_for_kind(
            review->kind, environment))
        return false;
    zcl_hex_encode(flags, 32, review->flags_sha3);
    zcl_hex_encode(environment, 32, review->environment_sha3);
    (void)snprintf(review->virtual_workdir,
                   sizeof(review->virtual_workdir), "%s",
                   VCS_BUILD_REVIEW_VIRTUAL_ROOT_V1);
    (void)snprintf(review->declared_outputs,
                   sizeof(review->declared_outputs), "%s",
                   VCS_BUILD_REVIEW_OUTPUT_V1);
    (void)snprintf(review->resource_policy,
                   sizeof(review->resource_policy), "%s",
                   VCS_BUILD_REVIEW_RESOURCE_POLICY_V1);
    review->created_at = review->updated_at = now;
    struct db_build_job job = *base_job;
    job.job_id[0] = '\0';
    (void)snprintf(job.state, sizeof(job.state), "PLANNED");
    job.outcome[0] = '\0';
    job.created_at = job.updated_at = now;
    if (!build_fabric_action_id(&job, review, review->action_id).ok ||
        !build_fabric_job_id(&job, review->action_id, job.job_id).ok)
        return false;
    (void)snprintf(review->job_id, sizeof(review->job_id), "%s", job.job_id);
    return db_build_job_save(ndb, &job) && db_build_action_save(ndb, review);
}

static bool zwork_review_receipt(
    struct node_db *ndb, const char *workspace,
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const struct db_build_action *action, const uint8_t proof_set_root[32],
    const uint8_t review_root[32], const uint8_t secret[32],
    const uint8_t pubkey[32], int64_t now, char receipt_root[65])
{
    struct vcs_zcode_work_request_v1 request = {
        .request_id = (uint64_t)now,
        .work_kind = VCS_ZCODE_WORK_REVIEW,
        .target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3,
        .max_cpu_seconds = task->max_cpu_seconds,
        .max_memory_bytes = task->max_memory_bytes,
        .max_output_bytes = task->max_output_bytes,
        .deadline_unix = task->expires_unix - 1,
    };
    uint8_t task_root[32], candidate_root[32], action_root[32];
    if (vcs_zcode_task_root(task, task_root) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_root(candidate, candidate_root) !=
            VCS_ZCODE_DEV_OK ||
        !zcl_hex_decode_lower(action->action_id, action_root, 32))
        return false;
    memcpy(request.task_root, task_root, 32);
    memcpy(request.candidate_root, candidate_root, 32);
    memcpy(request.action_root, action_root, 32);
    memcpy(request.input_root, candidate_root, 32);
    memcpy(request.context_root, proof_set_root, 32);
    memcpy(request.proof_policy_root, task->proof_policy_root, 32);
    memcpy(request.toolchain_capsule_root,
           task->toolchain_capsule_root, 32);
    if (!vcs_zcode_work_request_seal(&request, secret, pubkey))
        return false;
    struct vcs_zcode_work_result_v1 result = {0};
    result.request_id = request.request_id;
    memcpy(result.task_root, task_root, 32);
    memcpy(result.candidate_root, candidate_root, 32);
    memcpy(result.action_root, action_root, 32);
    memcpy(result.output_root, review_root, 32);
    struct vcs_zcode_work_receipt_v1 *receipt = &result.receipt;
    receipt->schema_version = VCS_ZCODE_DEV_VERSION;
    memcpy(receipt->task_root, task_root, 32);
    memcpy(receipt->candidate_root, candidate_root, 32);
    memcpy(receipt->action_root, action_root, 32);
    memcpy(receipt->input_root, candidate_root, 32);
    memcpy(receipt->output_root, review_root, 32);
    memcpy(receipt->proof_policy_root, task->proof_policy_root, 32);
    memcpy(receipt->toolchain_capsule_root,
           task->toolchain_capsule_root, 32);
    static const char lease_domain[] = "zcl.zcode.review.manual.lease.v1";
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)lease_domain,
                   sizeof(lease_domain));
    sha3_256_write(&sha, review_root, 32);
    sha3_256_finalize(&sha, receipt->lease_id);
    memcpy(receipt->evidence_root, proof_set_root, 32);
    static const char confinement[] =
        "zcode-review:manual;candidate=read-only;accept=0;publish=0";
    sha3_256((const uint8_t *)confinement, sizeof(confinement),
             receipt->confinement_root);
    receipt->work_kind = VCS_ZCODE_WORK_REVIEW;
    receipt->status = VCS_ZCODE_WORK_PASS;
    receipt->started_unix = now > 0 ? now - 1 : 0;
    receipt->finished_unix = now;
    if (vcs_zcode_work_receipt_seal(receipt, secret, pubkey) !=
        VCS_ZCODE_DEV_OK)
        return false;
    return build_fabric_receipt_observe_remote(
               ndb, workspace, &request, &result, now, receipt_root).ok;
}

// long-function-ok:review-authority-transaction — object, action, receipt and
// evidence evaluation must remain one fail-closed operation over one candidate.
void zcl_native_handle_zcode_work_review(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = zwork_str(request->input, "workspace");
    const char *work = zwork_str(request->input, "work");
    const char *adapter = zwork_str(request->input, "adapter");
    const char *verdict_text = zwork_str(request->input, "verdict");
    const char *findings = zwork_str(request->input, "findings");
    if (!workspace_arg || !workspace_arg[0]) workspace_arg = ".";
    if (!adapter || !adapter[0]) adapter = "manual";
    uint8_t verdict = verdict_text && strcmp(verdict_text, "approve") == 0
        ? VCS_ZCODE_REVIEW_APPROVE
        : verdict_text && strcmp(verdict_text, "request_changes") == 0
        ? VCS_ZCODE_REVIEW_REQUEST_CHANGES
        : verdict_text && strcmp(verdict_text, "reject") == 0
        ? VCS_ZCODE_REVIEW_REJECT : 0;
    if (strcmp(adapter, "manual") != 0 || verdict == 0 || !findings ||
        findings[0] == '\0' || strlen(findings) > 4096u) {
        zwork_fail(reply, strcmp(adapter, "manual") != 0
                         ? "REVIEW_ADAPTER_UNAVAILABLE" : "BAD_REVIEW_INPUT",
                   "review", "manual review requires a closed verdict and 1..4096 bytes of findings",
                   false, false);
        return;
    }
    char workspace[ZWORK_PATH_MAX];
    if (!realpath(workspace_arg, workspace)) {
        zwork_fail(reply, "BAD_WORKSPACE", "resolve",
                   "workspace must resolve to an existing directory",
                   false, false);
        return;
    }
    int64_t now = platform_time_wall_unix();
    struct vcs_zcode_task_index *index =
        vcs_zcode_task_index_build(workspace, now);
    bool ambiguous = false;
    const struct vcs_zcode_task_index_entry *entry = index
        ? zwork_resolve(index, work, &ambiguous) : NULL;
    struct vcs_zcode_task_v1 task;
    struct vcs_zcode_candidate_v1 candidate;
    if (!entry || entry->expired || !entry->latest_action_root_hex[0] ||
        !zwork_review_load_objects(workspace, entry, &task, &candidate)) {
        zwork_fail(reply, ambiguous ? "AMBIGUOUS_WORK" : "WORK_NOT_REVIEWABLE",
                   "review", "a current candidate with signed non-review evidence is required",
                   false, false);
        vcs_zcode_task_index_free(index);
        return;
    }
    char datadir[ZWORK_PATH_MAX], reviewer_dir[ZWORK_PATH_MAX], db_path[ZWORK_PATH_MAX];
    int dn = snprintf(datadir, sizeof(datadir),
                      "/tmp/zclassic23-zcode-workspaces/%lu/%.64s/zbuild",
                      (unsigned long)getuid(), entry->task_root_hex);
    int rn = snprintf(reviewer_dir, sizeof(reviewer_dir), "%s/reviewer", datadir);
    int bn = snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb = {0};
    struct db_build_action base_action, review_action;
    struct db_build_job base_job;
    struct build_fabric_proof_evaluation before = {0}, after = {0};
    struct db_build_worker reviewer;
    uint8_t secret[32] = {0}, pubkey[32] = {0};
    bool opened = dn > 0 && (size_t)dn < sizeof(datadir) && rn > 0 &&
        (size_t)rn < sizeof(reviewer_dir) && bn > 0 &&
        (size_t)bn < sizeof(db_path) && zwork_open_build_ledger(
            &ndb, db_path, "zcode.work.review", true);
    const char *failed_stage = opened ? "base_action" : "scratch_ledger";
    bool ready = opened && db_build_action_find(
        &ndb, entry->latest_action_root_hex, &base_action);
    if (ready) failed_stage = "base_job";
    ready = ready && db_build_job_find(&ndb, base_action.job_id, &base_job);
    if (ready) failed_stage = "non_review_proof_set";
    ready = ready && build_fabric_proof_evaluate(
        &ndb, workspace, entry->latest_action_root_hex, now, &before).ok &&
        before.valid_receipts > 0 && before.review_receipts == 0;
    if (ready) failed_stage = "reviewer_directory";
    ready = ready && zcl_mkdir_p(reviewer_dir, 0700).ok;
    if (ready) failed_stage = "reviewer_identity";
    ready = ready && build_fabric_worker_identity_load(
        reviewer_dir, &reviewer, secret, pubkey).ok;
    if (ready) failed_stage = "reviewer_independence";
    ready = ready && memcmp(pubkey, candidate.author_pubkey, 32) != 0;
    if (ready) failed_stage = "reviewer_approval";
    ready = ready && build_fabric_worker_approve(&ndb, &reviewer, now).ok;
    uint8_t proof_set_root[32], findings_root[32], review_root[32];
    struct vcs_zcode_review_v1 review = {
        .schema_version = VCS_ZCODE_DEV_VERSION,
        .verdict = verdict,
        .sequence = 1,
        .created_unix = now,
    };
    if (ready) failed_stage = "proof_set_root";
    ready = ready && zcl_hex_decode_lower(
        before.proof_set_root_sha3, proof_set_root, 32);
    if (ready) {
        (void)vcs_zcode_task_root(&task, review.task_root);
        (void)vcs_zcode_candidate_root(&candidate, review.candidate_root);
        memcpy(review.proof_policy_root, task.proof_policy_root, 32);
        memcpy(review.proof_set_root, proof_set_root, 32);
        sha3_256((const uint8_t *)findings, strlen(findings), findings_root);
        memcpy(review.findings_root, findings_root, 32);
        memcpy(review.reviewer_pubkey, pubkey, 32);
    }
    uint8_t review_wire[VCS_ZCODE_REVIEW_WIRE_BYTES];
    if (ready) failed_stage = "findings_store";
    ready = ready && vcs_object_put_addressed(
        workspace, findings_root, (const uint8_t *)findings, strlen(findings));
    if (ready) failed_stage = "review_wire";
    ready = ready && vcs_zcode_review_serialize(
        &review, review_wire) == VCS_ZCODE_DEV_OK &&
        vcs_zcode_review_root(&review, review_root) == VCS_ZCODE_DEV_OK;
    if (ready) failed_stage = "review_store";
    ready = ready && vcs_object_put_addressed(
        workspace, review_root, review_wire, sizeof(review_wire));
    if (ready) failed_stage = "review_action";
    ready = ready && zwork_review_action(
        &ndb, &base_action, &base_job, now, &review_action);
    char receipt_root[65] = {0};
    if (ready) failed_stage = "review_receipt";
    ready = ready && zwork_review_receipt(
        &ndb, workspace, &task, &candidate, &review_action,
        proof_set_root, review_root, secret, pubkey, now, receipt_root);
    if (ready) failed_stage = "proof_re_evaluation";
    ready = ready && build_fabric_proof_evaluate(
        &ndb, workspace, entry->latest_action_root_hex, now, &after).ok;
    memory_cleanse(secret, sizeof(secret));
    if (opened) node_db_close(&ndb);
    if (!ready) {
        char detail[256];
        (void)snprintf(detail, sizeof(detail),
                       "review stopped at %s; prior canonical evidence is preserved",
                       failed_stage);
        zwork_fail(reply, before.review_receipts > 0 ? "REVIEW_ALREADY_PRESENT" :
                   "REVIEW_EXECUTION_FAILED", failed_stage,
                   before.review_receipts > 0
                     ? "the candidate already has a trusted review; conflicting-review support is not yet complete"
                     : detail,
                   true, opened);
        vcs_zcode_task_index_free(index);
        return;
    }
    char review_hex[65], reviewer_hex[65];
    zcl_hex_encode(review_root, 32, review_hex);
    zcl_hex_encode(pubkey, 32, reviewer_hex);
    char work_id[32];
    (void)snprintf(work_id, sizeof(work_id), "work-%.12s",
                   entry->task_root_hex);
    bool rendered = json_push_kv_str(&reply->data, "work_id", work_id) &&
        json_push_kv_str(&reply->data, "adapter", "manual") &&
        json_push_kv_str(&reply->data, "verdict", verdict_text) &&
        json_push_kv_bool(&reply->data, "independent_reviewer", true) &&
        json_push_kv_int(&reply->data, "review_receipts",
                         (int64_t)after.review_receipts) &&
        json_push_kv_bool(&reply->data, "review_satisfied",
                          after.review_satisfied) &&
        json_push_kv_bool(&reply->data, "policy_satisfied",
                          after.policy_satisfied) &&
        json_push_kv_str(&reply->data, "review_root", review_hex) &&
        json_push_kv_str(&reply->data, "work_receipt_root", receipt_root) &&
        json_push_kv_str(&reply->data, "reviewer_pubkey", reviewer_hex) &&
        json_push_kv_str(&reply->data, "proof_set_reviewed",
                         before.proof_set_root_sha3) &&
        json_push_kv_str(&reply->data, "next_safe_command",
                         "zcode work status");
    vcs_zcode_task_index_free(index);
    if (!rendered)
        zwork_fail(reply, "REVIEW_OUTPUT_FAILED", "render",
                   "review result could not be rendered", false, true);
}

void zcl_native_handle_zcode_work_accept(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace_arg = zwork_str(request->input, "workspace");
    const char *work = zwork_str(request->input, "work");
    const char *confirmed_identity =
        zwork_str(request->input, "confirmation_identity");
    if (!workspace_arg || !workspace_arg[0]) workspace_arg = ".";
    char workspace[ZWORK_PATH_MAX];
    if (!realpath(workspace_arg, workspace)) {
        zwork_fail(reply, "BAD_WORKSPACE", "resolve",
                   "workspace must resolve to an existing directory",
                   false, false);
        return;
    }
    struct vcs_zcode_task_index *index = vcs_zcode_task_index_build(
        workspace, platform_time_wall_unix());
    bool ambiguous = false;
    const struct vcs_zcode_task_index_entry *entry = index
        ? zwork_resolve(index, work, &ambiguous) : NULL;
    bool ready = entry && !entry->expired &&
        (strcmp(entry->state, VCS_ZCODE_TASK_STATE_EVIDENCE_READY) == 0 ||
         strcmp(entry->state,
                VCS_ZCODE_TASK_STATE_CANDIDATE_PROOFS_READY) == 0 ||
         strcmp(entry->state, VCS_ZCODE_TASK_STATE_PROVEN) == 0);
    if (!ready || !entry->latest_action_root_hex[0] ||
        !entry->latest_candidate_source_root_hex[0]) {
        zwork_fail(reply, ambiguous ? "AMBIGUOUS_WORK" :
                     "WORK_NOT_READY_FOR_ACCEPTANCE", "accept",
                   entry && entry->expired ? "task expired" :
                   "the latest candidate lacks verified passing evidence",
                   false, false);
        vcs_zcode_task_index_free(index); return;
    }
    char datadir[ZWORK_PATH_MAX];
    int n = snprintf(datadir, sizeof(datadir),
                     "/tmp/zclassic23-zcode-workspaces/%lu/%.64s/zbuild",
                     (unsigned long)getuid(), entry->task_root_hex);
    if (n <= 0 || (size_t)n >= sizeof(datadir)) {
        zwork_fail(reply, "ACCEPT_PATH_FAILED", "resolve",
                   "task-local ZBuild path is too long", false, false);
        vcs_zcode_task_index_free(index); return;
    }
    char acceptance_hex[65] = {0};
    if (confirmed_identity) {
        char db_path[ZWORK_PATH_MAX];
        int dbn = snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
        struct node_db identity_db = {0};
        struct build_fabric_proof_evaluation identity_facts;
        bool identity_ready = dbn > 0 && (size_t)dbn < sizeof(db_path) &&
            node_db_open_existing_runtime(
                &identity_db, db_path, "zcode.work.accept.confirmation");
        if (identity_ready) {
            identity_ready = build_fabric_proof_evaluate_readonly(
                &identity_db, workspace, entry->latest_action_root_hex,
                (int64_t)platform_time_wall_unix(), &identity_facts).ok;
            node_db_close(&identity_db);
        }
        uint8_t task_root[32], candidate_root[32], policy_root[32];
        uint8_t proof_root[32], acceptance_root[32], supplied_root[32];
        identity_ready = identity_ready &&
            zcl_hex_decode_lower(entry->task_root_hex, task_root, 32) &&
            zcl_hex_decode_lower(entry->latest_candidate_root_hex,
                                 candidate_root, 32) &&
            zcl_hex_decode_lower(entry->proof_policy_root_hex,
                                 policy_root, 32) &&
            zcl_hex_decode_lower(identity_facts.proof_set_root_sha3,
                                 proof_root, 32) &&
            vcs_zcode_acceptance_plan_root(
                task_root, candidate_root, policy_root, proof_root,
                acceptance_root) == VCS_ZCODE_DEV_OK;
        if (identity_ready)
            zcl_hex_encode(acceptance_root, 32, acceptance_hex);
        if (!identity_ready ||
            !zcl_hex_decode_lower(confirmed_identity, supplied_root, 32) ||
            memcmp(supplied_root, acceptance_root, 32) != 0) {
            zwork_fail(reply, "CONFIRMATION_IDENTITY_STALE", "accept",
                       "the confirmed native decision no longer matches the exact candidate proof set",
                       false, false);
            vcs_zcode_task_index_free(index); return;
        }
    }
    struct zcl_command_reply lane_reply;
    zcl_command_reply_init(&lane_reply, "zcl.zcode_lane.v1");
    zwork_lane_inner(workspace, datadir,
                     entry->latest_candidate_source_root_hex, &lane_reply);
    const struct json_value *lane_value = lane_reply.status ==
            ZCL_COMMAND_STATUS_PASSED
        ? json_get(&lane_reply.data, "lane") : NULL;
    const char *lane = lane_value && lane_value->type == JSON_STR
        ? json_get_str(lane_value) : NULL;
    if (!lane) {
        zwork_fail(reply, "LANE_STATE_MISSING", "accept",
                   lane_reply.error.message[0] ? lane_reply.error.message :
                   "the signed FRONTIER lane could not be reloaded",
                   true, false);
        zcl_command_reply_free(&lane_reply);
        vcs_zcode_task_index_free(index); return;
    }
    bool already_proven = strcmp(lane, "PROVEN") == 0;
    struct zcl_command_reply final_reply;
    zcl_command_reply_init(&final_reply, "zcl.zcode_accept.v1");
    if (already_proven) {
        json_copy(&final_reply.data, &lane_reply.data);
        final_reply.status = ZCL_COMMAND_STATUS_PASSED;
    } else {
        struct zcl_command_reply evidence;
        zcl_command_reply_init(&evidence, "zcl.zcode_evidence.v1");
        zwork_evidence_inner(workspace, datadir,
                             entry->latest_action_root_hex, &evidence);
        const struct json_value *satisfied = evidence.status ==
                ZCL_COMMAND_STATUS_PASSED
            ? json_get(&evidence.data, "policy_satisfied") : NULL;
        if (!satisfied || !json_get_bool(satisfied)) {
            zwork_fail(reply, "PROOF_PROFILE_INCOMPLETE", "evidence",
                       evidence.status == ZCL_COMMAND_STATUS_PASSED
                         ? "the exact proof profile is not yet satisfied; preserved evidence remains inspectable"
                         : evidence.error.message,
                       true, false);
            zcl_command_reply_free(&evidence);
            zcl_command_reply_free(&final_reply);
            zcl_command_reply_free(&lane_reply);
            vcs_zcode_task_index_free(index); return;
        }
        zcl_command_reply_free(&evidence);
        if (strcmp(lane, "FRONTIER") == 0) {
            struct zcl_command_reply candidate;
            zcl_command_reply_init(&candidate, "zcl.zcode_accept.v1");
            zwork_accept_inner(workspace, datadir,
                               entry->latest_action_root_hex,
                               "CANDIDATE", &candidate);
            if (candidate.status != ZCL_COMMAND_STATUS_PASSED) {
                zwork_fail(reply, "CANDIDATE_ACCEPTANCE_REFUSED", "accept",
                           candidate.error.message, false, false);
                zcl_command_reply_free(&candidate);
                zcl_command_reply_free(&final_reply);
                zcl_command_reply_free(&lane_reply);
                vcs_zcode_task_index_free(index); return;
            }
            zcl_command_reply_free(&candidate);
        }
        zwork_accept_inner(workspace, datadir,
                           entry->latest_action_root_hex, "PROVEN",
                           &final_reply);
    }
    if (final_reply.status != ZCL_COMMAND_STATUS_PASSED) {
        zwork_fail(reply, "PROVEN_ACCEPTANCE_REFUSED", "accept",
                   final_reply.error.message, false, true);
    } else {
        struct json_value expert;
        json_init(&expert); json_copy(&expert, &final_reply.data);
        char work_id[32];
        (void)snprintf(work_id, sizeof(work_id), "work-%.12s",
                       entry->task_root_hex);
        bool ok = json_push_kv_str(&reply->data, "work_id", work_id) &&
            json_push_kv_str(&reply->data, "goal_decision", "accepted") &&
            json_push_kv_str(&reply->data, "state", "PROVEN") &&
            (!confirmed_identity ||
             json_push_kv_str(&reply->data, "confirmation_identity",
                              acceptance_hex)) &&
            json_push_kv_bool(&reply->data, "confirmation_identity_checked",
                              confirmed_identity != NULL) &&
            json_push_kv_bool(&reply->data, "idempotent", already_proven) &&
            json_push_kv_str(&reply->data, "authoritative_workspace",
                             "unchanged") &&
            json_push_kv_str(&reply->data, "next_safe_command",
                             "zcode work status") &&
            json_push_kv(&reply->data, "expert", &expert);
        json_free(&expert);
        if (!ok)
            zwork_fail(reply, "ACCEPT_OUTPUT_FAILED", "render",
                       "human acceptance summary could not be rendered",
                       false, !already_proven);
    }
    zcl_command_reply_free(&final_reply);
    zcl_command_reply_free(&lane_reply);
    vcs_zcode_task_index_free(index);
}
#endif
