/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "kernel/command_registry.h"

#include "crypto/sha256.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool command_is_branch(const struct zcl_command_spec *spec);

static _Atomic uint64_t g_request_sequence = 1;

/* ── Hot-swap leaf-handler override layer ─────────────────────────────
 *
 * Mirrors the proven mcp_router_replace_batch snapshot design one layer down
 * (tools/mcp/router.c:263-334): a heap-cloned, immutable snapshot of
 * {path,handler} overrides published with ONE release-store on a static
 * _Atomic pointer. Readers acquire-load; a NULL active pointer is the zero-cost
 * fast path (no override ever installed). Published snapshots are never freed —
 * a dispatch that acquired an older snapshot must finish without a UAF race.
 * Writes are rare (hot swaps) and serialized by a tiny spin lock; readers stay
 * lock-free. */
struct zcl_command_handler_snapshot {
    uint32_t generation;
    size_t count;
    struct zcl_command_handler_override slots[ZCL_COMMAND_HANDLER_OVERRIDE_MAX];
};

static struct zcl_command_handler_snapshot *_Atomic g_active_handlers = NULL;
static atomic_flag g_handler_write_lock = ATOMIC_FLAG_INIT;
static const struct zcl_command_registry *_Atomic g_active_registry = NULL;

static inline void handler_write_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_handler_write_lock,
                                             memory_order_acquire))
        ; /* spin: writes are rare and short */
}

static inline void handler_write_unlock(void)
{
    atomic_flag_clear_explicit(&g_handler_write_lock, memory_order_release);
}

static zcl_command_handler_fn handler_override_lookup(const char *path)
{
    const struct zcl_command_handler_snapshot *snap =
        atomic_load_explicit(&g_active_handlers, memory_order_acquire);
    if (!snap || !path)
        return NULL; /* zero-overhead fast path when no snapshot exists */
    for (size_t i = 0; i < snap->count; i++) {
        if (snap->slots[i].path && strcmp(snap->slots[i].path, path) == 0)
            return snap->slots[i].handler;
    }
    return NULL;
}

void zcl_command_registry_set_active(const struct zcl_command_registry *registry)
{
    atomic_store_explicit(&g_active_registry, registry, memory_order_release);
}

uint32_t zcl_command_registry_active_generation(void)
{
    const struct zcl_command_handler_snapshot *snap =
        atomic_load_explicit(&g_active_handlers, memory_order_acquire);
    return snap ? snap->generation : 0u;
}

zcl_command_handler_fn zcl_command_registry_effective_handler(
    const struct zcl_command_spec *spec)
{
    if (!spec)
        return NULL;
    zcl_command_handler_fn override = handler_override_lookup(spec->path);
    return override ? override : spec->handler;
}

void zcl_command_registry_reset_overrides(void)
{
    handler_write_lock();
    /* Retire the active snapshot per the never-free discipline (an in-flight
     * reader may still hold it); just re-point at NULL under the write lock. */
    atomic_store_explicit(&g_active_handlers, NULL, memory_order_release);
    handler_write_unlock();
}

bool zcl_command_registry_replace_batch(
    uint32_t generation,
    const struct zcl_command_handler_override *overrides,
    size_t count, char *why, size_t why_sz)
{
    if (why && why_sz)
        why[0] = '\0';

    if (!overrides || count == 0 || count > ZCL_COMMAND_HANDLER_OVERRIDE_MAX) {
        if (why && why_sz)
            snprintf(why, why_sz, "invalid override count: %zu", count);
        LOG_FAIL("kernel.command", "invalid override count: %zu", count);
    }

    const struct zcl_command_registry *registry =
        atomic_load_explicit(&g_active_registry, memory_order_acquire);
    if (!registry) {
        if (why && why_sz)
            snprintf(why, why_sz, "no active registry bound");
        LOG_FAIL("kernel.command", "no active registry bound for override batch");
    }

    /* ── Validate the ENTIRE batch against the immutable registry before
     * touching the active snapshot (no lock needed — the registry is
     * immutable). Any rejection leaves the active snapshot untouched. */
    for (size_t i = 0; i < count; i++) {
        const struct zcl_command_handler_override *ovr = &overrides[i];
        if (!ovr->path || !ovr->path[0] || !ovr->handler) {
            if (why && why_sz)
                snprintf(why, why_sz, "override %zu: null/empty path or handler",
                         i);
            LOG_FAIL("kernel.command",
                     "override %zu: null/empty path or handler", i);
        }
        bool was_alias = false;
        const struct zcl_command_spec *spec =
            zcl_command_registry_find(registry, ovr->path, &was_alias);
        if (!spec || was_alias || strcmp(spec->path, ovr->path) != 0) {
            if (why && why_sz)
                snprintf(why, why_sz, "no canonical leaf named '%s'", ovr->path);
            LOG_FAIL("kernel.command", "no canonical leaf named '%s'",
                     ovr->path);
        }
        if (command_is_branch(spec)) {
            if (why && why_sz)
                snprintf(why, why_sz, "leaf '%s' is a branch, not swappable",
                         ovr->path);
            LOG_FAIL("kernel.command", "leaf '%s' is a branch, not swappable",
                     ovr->path);
        }
        if (spec->availability != ZCL_COMMAND_READY) {
            if (why && why_sz)
                snprintf(why, why_sz, "leaf '%s' is not READY", ovr->path);
            LOG_FAIL("kernel.command", "leaf '%s' is not READY", ovr->path);
        }
        if (spec->effect != ZCL_COMMAND_EFFECT_READ) {
            if (why && why_sz)
                snprintf(why, why_sz,
                         "leaf '%s' is mutating/destructive (effect=%s)",
                         ovr->path, zcl_command_effect_name(spec->effect));
            LOG_FAIL("kernel.command",
                     "refusing mutating/destructive leaf '%s' (effect=%s)",
                     ovr->path, zcl_command_effect_name(spec->effect));
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(overrides[j].path, ovr->path) == 0) {
                if (why && why_sz)
                    snprintf(why, why_sz, "duplicate override '%s'", ovr->path);
                LOG_FAIL("kernel.command", "duplicate override '%s'",
                         ovr->path);
            }
        }
    }

    handler_write_lock();
    const struct zcl_command_handler_snapshot *old =
        atomic_load_explicit(&g_active_handlers, memory_order_acquire);
    uint32_t old_gen = old ? old->generation : 0u;

    uint32_t next_generation = generation ? generation : old_gen + 1u;
    if (next_generation <= old_gen) {
        handler_write_unlock();
        if (why && why_sz)
            snprintf(why, why_sz,
                     "generation %u is not newer than active generation %u",
                     next_generation, old_gen);
        LOG_FAIL("kernel.command",
                 "generation %u not newer than active generation %u",
                 next_generation, old_gen);
    }

    struct zcl_command_handler_snapshot *next =
        zcl_malloc(sizeof(*next), "command handler override snapshot");
    if (!next) {
        handler_write_unlock();
        if (why && why_sz)
            snprintf(why, why_sz, "snapshot allocation failed");
        LOG_FAIL("kernel.command", "override snapshot allocation failed");
    }
    if (old)
        memcpy(next, old, sizeof(*next));
    else
        memset(next, 0, sizeof(*next));
    next->generation = next_generation;

    /* Merge: overwrite an existing override slot with the same path, else
     * append. Capacity is bounded by ZCL_COMMAND_HANDLER_OVERRIDE_MAX. */
    for (size_t i = 0; i < count; i++) {
        const struct zcl_command_handler_override *ovr = &overrides[i];
        size_t idx = next->count;
        for (size_t k = 0; k < next->count; k++) {
            if (next->slots[k].path && strcmp(next->slots[k].path, ovr->path) == 0) {
                idx = k;
                break;
            }
        }
        if (idx == next->count) {
            if (next->count >= ZCL_COMMAND_HANDLER_OVERRIDE_MAX) {
                handler_write_unlock();
                /* next is a private, unpublished clone — safe to free. */
                free(next);
                if (why && why_sz)
                    snprintf(why, why_sz, "override capacity exceeded");
                LOG_FAIL("kernel.command", "override capacity exceeded (max %u)",
                         (unsigned)ZCL_COMMAND_HANDLER_OVERRIDE_MAX);
            }
            next->count++;
        }
        next->slots[idx] = *ovr;
    }

    atomic_store_explicit(&g_active_handlers, next, memory_order_release);
    handler_write_unlock();
    return true;
}

static const char *const g_layer_names[] = {
    "root", "core", "app", "dev", "ops", "discover", "code"
};
static const char *const g_effect_names[] = {
    "read", "mutate", "destructive"
};
static const char *const g_risk_names[] = {
    "read", "app-write", "wallet", "core-recovery", "destructive",
    "dev-mutation"
};
static const char *const g_scope_names[] = {
    "local", "node", "dev-lane", "offline-copy"
};
static const char *const g_authority_names[] = {
    "public", "operator", "owner"
};
static const char *const g_availability_names[] = {
    "ready", "compat", "planned"
};
static const char *const g_mode_names[] = {
    "branch", "sync", "job", "stream"
};
static const char *const g_latency_names[] = {
    "instant", "fast", "foreground", "background", "persistent"
};
static const char *const g_cost_names[] = {
    "tiny", "low", "moderate", "high", "stream"
};
static const char *const g_confirmation_names[] = {
    "none", "idempotency", "plan-commit"
};
static const char *const g_status_names[] = {
    "passed", "accepted", "blocked", "failed"
};

#define NAME_FN(name, values, max_value)                                    \
    const char *name(max_value value)                                       \
    {                                                                       \
        size_t index = (size_t)value;                                       \
        return index < sizeof(values) / sizeof(values[0])                   \
            ? values[index] : "invalid";                                  \
    }

NAME_FN(zcl_command_layer_name, g_layer_names, enum zcl_command_layer)
NAME_FN(zcl_command_effect_name, g_effect_names, enum zcl_command_effect)
NAME_FN(zcl_command_risk_name, g_risk_names, enum zcl_command_risk)
NAME_FN(zcl_command_scope_name, g_scope_names, enum zcl_command_scope)
NAME_FN(zcl_command_authority_name, g_authority_names,
        enum zcl_command_authority)
NAME_FN(zcl_command_availability_name, g_availability_names,
        enum zcl_command_availability)
NAME_FN(zcl_command_mode_name, g_mode_names, enum zcl_command_mode)
NAME_FN(zcl_command_latency_name, g_latency_names, enum zcl_command_latency)
NAME_FN(zcl_command_cost_name, g_cost_names, enum zcl_command_cost)
NAME_FN(zcl_command_confirmation_name, g_confirmation_names,
        enum zcl_command_confirmation)
NAME_FN(zcl_command_status_name, g_status_names, enum zcl_command_status)

#undef NAME_FN

static bool copy_string(char *out, size_t out_size, const char *value)
{
    if (!out || out_size == 0)
        return false;
    int n = snprintf(out, out_size, "%s", value ? value : "");
    return n >= 0 && (size_t)n < out_size;
}

void zcl_command_reply_init(struct zcl_command_reply *reply,
                            const char *data_schema)
{
    if (!reply)
        return;
    memset(reply, 0, sizeof(*reply));
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
    reply->data_schema = data_schema ? data_schema : "zcl.command.empty.v1";
    json_init(&reply->data);
    json_set_object(&reply->data);
}

void zcl_command_reply_free(struct zcl_command_reply *reply)
{
    if (!reply)
        return;
    json_free(&reply->data);
    memset(reply, 0, sizeof(*reply));
}

void zcl_command_reply_fail(struct zcl_command_reply *reply,
                            enum zcl_command_status status,
                            enum zcl_command_exit exit_code,
                            const char *code, const char *phase,
                            bool retryable, bool mutated,
                            const char *message, const char *evidence)
{
    if (!reply)
        return;
    reply->status = status;
    reply->exit_code = exit_code;
    reply->error.retryable = retryable;
    reply->error.mutated = mutated;
    (void)copy_string(reply->error.code, sizeof(reply->error.code), code);
    (void)copy_string(reply->error.phase, sizeof(reply->error.phase), phase);
    (void)copy_string(reply->error.message, sizeof(reply->error.message),
                      message);
    (void)copy_string(reply->error.evidence, sizeof(reply->error.evidence),
                      evidence);
}

bool zcl_command_reply_add_next(struct zcl_command_reply *reply,
                                const char *command, const char *input_json,
                                const char *reason)
{
    if (!reply || !command || !command[0] ||
        reply->next_count >= ZCL_COMMAND_MAX_NEXT)
        return false;
    struct zcl_command_next *next = &reply->next[reply->next_count];
    if (!copy_string(next->command, sizeof(next->command), command) ||
        !copy_string(next->input_json, sizeof(next->input_json),
                     input_json && input_json[0] ? input_json : "{}") ||
        !copy_string(next->reason, sizeof(next->reason), reason))
        return false;
    reply->next_count++;
    return true;
}

static bool path_valid(const char *path)
{
    if (!path || !path[0] || strlen(path) >= ZCL_COMMAND_MAX_PATH)
        return false;
    bool token_start = true;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p == '.') {
            if (token_start)
                return false;
            token_start = true;
            continue;
        }
        if (token_start) {
            if (!(*p >= 'a' && *p <= 'z'))
                return false;
            token_start = false;
        } else if (!((*p >= 'a' && *p <= 'z') ||
                     (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return !token_start;
}

static bool csv_token_equal(const char *csv, const char *value)
{
    if (!csv || !csv[0] || !value)
        return false;
    size_t value_len = strlen(value);
    const char *at = csv;
    while (*at) {
        const char *end = strchr(at, ',');
        size_t len = end ? (size_t)(end - at) : strlen(at);
        if (len == value_len && memcmp(at, value, len) == 0)
            return true;
        if (!end)
            break;
        at = end + 1;
    }
    return false;
}

static bool csv_valid_paths(const char *csv)
{
    if (!csv || !csv[0])
        return true;
    const char *at = csv;
    char token[ZCL_COMMAND_MAX_PATH];
    while (*at) {
        const char *end = strchr(at, ',');
        size_t len = end ? (size_t)(end - at) : strlen(at);
        if (len == 0 || len >= sizeof(token))
            return false;
        memcpy(token, at, len);
        token[len] = 0;
        if (!path_valid(token))
            return false;
        if (!end)
            break;
        at = end + 1;
    }
    return true;
}

static bool enum_values_valid(const struct zcl_command_spec *spec)
{
    return spec->layer <= ZCL_COMMAND_LAYER_CODE &&
           spec->effect <= ZCL_COMMAND_EFFECT_DESTRUCTIVE &&
           spec->risk <= ZCL_COMMAND_RISK_DEV_MUTATION &&
           spec->scope <= ZCL_COMMAND_SCOPE_OFFLINE_COPY &&
           spec->authority <= ZCL_COMMAND_AUTH_OWNER &&
           spec->availability <= ZCL_COMMAND_PLANNED &&
           spec->mode <= ZCL_COMMAND_MODE_STREAM &&
           spec->latency <= ZCL_COMMAND_LATENCY_PERSISTENT &&
           spec->cost <= ZCL_COMMAND_COST_STREAM &&
           spec->confirmation <= ZCL_COMMAND_CONFIRM_PLAN_COMMIT;
}

static bool command_is_branch(const struct zcl_command_spec *spec)
{
    return spec && spec->mode == ZCL_COMMAND_MODE_BRANCH;
}

bool zcl_command_registry_validate(const struct zcl_command_registry *registry,
                                   char *why, size_t why_size)
{
    if (why && why_size)
        why[0] = 0;
    if (!registry || !registry->commands || registry->count == 0) {
        if (why) snprintf(why, why_size, "empty registry");
        return false;
    }
    for (size_t i = 0; i < registry->count; i++) {
        const struct zcl_command_spec *spec = &registry->commands[i];
        if (!path_valid(spec->path) || !spec->summary || !spec->summary[0] ||
            !enum_values_valid(spec) || !csv_valid_paths(spec->aliases)) {
            if (why) snprintf(why, why_size, "malformed command at index %zu", i);
            return false;
        }
        if (spec->parent && spec->parent[0] && !path_valid(spec->parent)) {
            if (why) snprintf(why, why_size, "invalid parent for %s", spec->path);
            return false;
        }
        if (command_is_branch(spec)) {
            if (spec->handler || spec->availability != ZCL_COMMAND_READY) {
                if (why) snprintf(why, why_size,
                                  "branch %s must be ready without handler",
                                  spec->path);
                return false;
            }
        } else {
            if (!spec->input_schema || !spec->input_schema[0] ||
                !spec->output_schema || !spec->output_schema[0] ||
                !spec->example || !spec->example[0]) {
                if (why) snprintf(why, why_size,
                                  "leaf %s lacks schema/example", spec->path);
                return false;
            }
            if (spec->availability == ZCL_COMMAND_READY && !spec->handler) {
                if (why) snprintf(why, why_size,
                                  "ready leaf %s lacks handler", spec->path);
                return false;
            }
            if (spec->availability == ZCL_COMMAND_PLANNED && spec->handler) {
                if (why) snprintf(why, why_size,
                                  "planned leaf %s has handler", spec->path);
                return false;
            }
        }
        if (spec->availability != ZCL_COMMAND_READY &&
            (!spec->availability_reason || !spec->availability_reason[0])) {
            if (why) snprintf(why, why_size,
                              "non-ready %s lacks reason", spec->path);
            return false;
        }
        if (spec->effect == ZCL_COMMAND_EFFECT_READ &&
            spec->risk != ZCL_COMMAND_RISK_READ) {
            if (why) snprintf(why, why_size,
                              "read effect/risk conflict for %s", spec->path);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            const struct zcl_command_spec *other = &registry->commands[j];
            if (strcmp(spec->path, other->path) == 0 ||
                csv_token_equal(spec->aliases, other->path) ||
                csv_token_equal(other->aliases, spec->path)) {
                if (why) snprintf(why, why_size,
                                  "path/alias collision for %s", spec->path);
                return false;
            }
            const char *at = spec->aliases;
            while (at && *at) {
                const char *end = strchr(at, ',');
                size_t len = end ? (size_t)(end - at) : strlen(at);
                char token[ZCL_COMMAND_MAX_PATH];
                memcpy(token, at, len);
                token[len] = 0;
                if (csv_token_equal(other->aliases, token)) {
                    if (why) snprintf(why, why_size,
                                      "duplicate alias %s", token);
                    return false;
                }
                if (!end)
                    break;
                at = end + 1;
            }
        }
        if (spec->parent && spec->parent[0]) {
            bool found_parent = false;
            for (size_t j = 0; j < registry->count; j++) {
                if (strcmp(registry->commands[j].path, spec->parent) == 0 &&
                    command_is_branch(&registry->commands[j])) {
                    found_parent = true;
                    break;
                }
            }
            if (!found_parent) {
                if (why) snprintf(why, why_size,
                                  "missing branch parent %s for %s",
                                  spec->parent, spec->path);
                return false;
            }
        }
    }
    return true;
}

const struct zcl_command_spec *zcl_command_registry_find(
    const struct zcl_command_registry *registry, const char *path_or_alias,
    bool *was_alias)
{
    if (was_alias)
        *was_alias = false;
    if (!registry || !path_or_alias || !path_or_alias[0])
        return NULL;
    for (size_t i = 0; i < registry->count; i++) {
        if (strcmp(registry->commands[i].path, path_or_alias) == 0)
            return &registry->commands[i];
    }
    for (size_t i = 0; i < registry->count; i++) {
        if (csv_token_equal(registry->commands[i].aliases, path_or_alias)) {
            if (was_alias)
                *was_alias = true;
            return &registry->commands[i];
        }
    }
    return NULL;
}

const struct zcl_command_spec *zcl_command_registry_resolve_words(
    const struct zcl_command_registry *registry,
    const char *const *words, size_t word_count, size_t *consumed,
    bool *was_alias, char *invoked, size_t invoked_size)
{
    if (consumed)
        *consumed = 0;
    if (was_alias)
        *was_alias = false;
    if (invoked && invoked_size)
        invoked[0] = 0;
    if (!registry || !words || word_count == 0)
        return NULL;

    char candidate[ZCL_COMMAND_MAX_PATH] = {0};
    size_t pos = 0;
    const struct zcl_command_spec *best = NULL;
    size_t best_count = 0;
    bool best_alias = false;
    for (size_t i = 0; i < word_count; i++) {
        const char *word = words[i];
        if (!word || !word[0] || word[0] == '-' || strchr(word, '.') ||
            strchr(word, '/') || strchr(word, '\\'))
            break;
        int n = snprintf(candidate + pos, sizeof(candidate) - pos,
                         "%s%s", pos ? "." : "", word);
        if (n <= 0 || (size_t)n >= sizeof(candidate) - pos)
            break;
        pos += (size_t)n;
        bool alias = false;
        const struct zcl_command_spec *found =
            zcl_command_registry_find(registry, candidate, &alias);
        if (found) {
            best = found;
            best_count = i + 1;
            best_alias = alias;
            if (invoked && invoked_size)
                (void)copy_string(invoked, invoked_size, candidate);
        }
    }
    if (consumed)
        *consumed = best_count;
    if (was_alias)
        *was_alias = best_alias;
    return best;
}

bool zcl_command_registry_input_validate(const struct zcl_command_spec *spec,
                                         const struct json_value *input,
                                         char *why, size_t why_size)
{
    if (why && why_size)
        why[0] = 0;
    if (!spec || !input || input->type != JSON_OBJ) {
        if (why) snprintf(why, why_size, "input must be one JSON object");
        return false;
    }
    if (strcmp(spec->input_schema, "zcl.command.empty_input.v1") == 0 &&
        input->num_children != 0) {
        if (why) snprintf(why, why_size, "command accepts no input keys");
        return false;
    }
    for (size_t i = 0; i < input->num_children; i++) {
        const char *key = input->keys[i];
        if (!key || !key[0] || !csv_token_equal(spec->input_keys, key)) {
            if (why) snprintf(why, why_size, "unknown input key '%s'",
                              key ? key : "");
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (input->keys[j] && strcmp(input->keys[j], key) == 0) {
                if (why) snprintf(why, why_size, "duplicate input key '%s'", key);
                return false;
            }
        }
        const struct json_value *value = &input->children[i];
        bool type_ok = false;
        if (strcmp(key, "files") == 0) {
            type_ok = value->type == JSON_ARR && value->num_children <= 256;
            for (size_t j = 0; type_ok && j < value->num_children; j++) {
                const struct json_value *item = &value->children[j];
                const char *text = json_get_str(item);
                type_ok = item->type == JSON_STR && text && text[0] &&
                          strlen(text) <= 1024;
            }
        } else if (strcmp(key, "verbose") == 0 || strcmp(key, "confirm") == 0) {
            type_ok = value->type == JSON_BOOL;
        } else if (strcmp(key, "seed") == 0) {
            type_ok = (value->type == JSON_INT && json_get_int(value) > 0) ||
                      (value->type == JSON_STR && json_get_str(value) &&
                       json_get_str(value)[0] && strlen(json_get_str(value)) <= 32);
        } else if (strcmp(key, "amount") == 0) {
            type_ok = value->type == JSON_INT || value->type == JSON_REAL ||
                      (value->type == JSON_STR && json_get_str(value) &&
                       json_get_str(value)[0] && strlen(json_get_str(value)) <= 64);
        } else if (strcmp(key, "cursor") == 0) {
            type_ok = (value->type == JSON_INT && json_get_int(value) >= 0) ||
                      (value->type == JSON_STR && json_get_str(value) &&
                       json_get_str(value)[0] && strlen(json_get_str(value)) <= 256);
        } else if (strcmp(key, "height") == 0 ||
                   strcmp(key, "start_height") == 0 ||
                   strcmp(key, "after") == 0 ||
                   strcmp(key, "after_epoch") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 0;
        } else if (strcmp(key, "timeout_ms") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 1 &&
                      json_get_int(value) <= 300000;
        } else if (strcmp(key, "heartbeat_ms") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 100 &&
                      json_get_int(value) <= 60000;
        } else if (strcmp(key, "verbosity") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 0 &&
                      json_get_int(value) <= 2;
        } else if (strcmp(key, "max_items") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 1 &&
                      json_get_int(value) <= 100;
        } else if (strcmp(key, "max_lines") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 1 &&
                      json_get_int(value) <= 1000;
        } else if (strcmp(key, "since_secs") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 0 &&
                      json_get_int(value) <= 31536000;
        } else if (strcmp(key, "limit") == 0 || strcmp(key, "depth") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) >= 1 &&
                      json_get_int(value) <= 1000000;
        } else if (strcmp(key, "watcher_id") == 0) {
            type_ok = value->type == JSON_INT && json_get_int(value) > 1;
        } else {
            const char *text = json_get_str(value);
            type_ok = value->type == JSON_STR && text && text[0] &&
                      strlen(text) <= 4096;
            if (type_ok && strcmp(key, "side") == 0)
                type_ok = strcmp(text, "input") == 0 ||
                          strcmp(text, "output") == 0;
            if (type_ok && strcmp(key, "view") == 0)
                type_ok = strcmp(text, "summary") == 0 ||
                          strcmp(text, "normal") == 0 ||
                          strcmp(text, "full") == 0;
        }
        if (!type_ok) {
            if (why) snprintf(why, why_size,
                              "invalid type or range for input key '%s'", key);
            return false;
        }
    }
    return true;
}

static void digest_text(struct sha256_ctx *sha, const char *value)
{
    static const unsigned char separator = 0;
    const char *text = value ? value : "";
    sha256_write(sha, (const unsigned char *)text, strlen(text));
    sha256_write(sha, &separator, 1);
}

void zcl_command_registry_digest(const struct zcl_command_registry *registry,
                                 char out[72])
{
    if (!out)
        return;
    struct sha256_ctx sha;
    unsigned char hash[SHA256_OUTPUT_SIZE];
    sha256_init(&sha);
    if (registry) {
        for (size_t i = 0; i < registry->count; i++) {
            const struct zcl_command_spec *spec = &registry->commands[i];
            digest_text(&sha, spec->path);
            digest_text(&sha, spec->parent);
            digest_text(&sha, spec->aliases);
            digest_text(&sha, spec->summary);
            digest_text(&sha, spec->tags);
            digest_text(&sha, spec->input_schema);
            digest_text(&sha, spec->output_schema);
            digest_text(&sha, spec->input_keys);
            digest_text(&sha, spec->positional_keys);
            unsigned char typed[] = {
                (unsigned char)spec->layer,
                (unsigned char)spec->effect,
                (unsigned char)spec->risk,
                (unsigned char)spec->scope,
                (unsigned char)spec->authority,
                (unsigned char)spec->availability,
                (unsigned char)spec->mode,
                (unsigned char)spec->latency,
                (unsigned char)spec->cost,
                (unsigned char)spec->confirmation,
            };
            sha256_write(&sha, typed, sizeof(typed));
        }
    }
    sha256_finalize(&sha, hash);
    memcpy(out, "sha256:", 7);
    for (size_t i = 0; i < sizeof(hash); i++)
        (void)snprintf(out + 7 + i * 2, 3, "%02x", hash[i]);
    out[71] = 0;
}

static bool push_string_array_csv(struct json_value *object, const char *key,
                                  const char *csv)
{
    struct json_value array;
    json_init(&array);
    json_set_array(&array);
    const char *at = csv;
    while (at && *at) {
        const char *end = strchr(at, ',');
        size_t len = end ? (size_t)(end - at) : strlen(at);
        char token[ZCL_COMMAND_MAX_PATH];
        if (len == 0 || len >= sizeof(token)) {
            json_free(&array);
            return false;
        }
        memcpy(token, at, len);
        token[len] = 0;
        struct json_value item;
        json_init(&item);
        json_set_str(&item, token);
        bool ok = json_push_back(&array, &item);
        json_free(&item);
        if (!ok) {
            json_free(&array);
            return false;
        }
        if (!end)
            break;
        at = end + 1;
    }
    bool ok = json_push_kv(object, key, &array);
    json_free(&array);
    return ok;
}

static bool push_child_summary(struct json_value *children,
                               const struct zcl_command_spec *spec)
{
    struct json_value child;
    json_init(&child);
    json_set_object(&child);
    bool ok = json_push_kv_str(&child, "path", spec->path) &&
              json_push_kv_str(&child, "summary", spec->summary) &&
              json_push_kv_str(&child, "risk",
                               zcl_command_risk_name(spec->risk)) &&
              json_push_kv_str(&child, "latency",
                               zcl_command_latency_name(spec->latency)) &&
              json_push_kv_str(&child, "availability",
                               zcl_command_availability_name(
                                   spec->availability)) &&
              json_push_back(children, &child);
    json_free(&child);
    return ok;
}

static size_t write_bounded_json(struct json_value *root, char *out,
                                 size_t out_size, size_t contract_budget)
{
    if (!root || !out || out_size == 0)
        return 0;
    size_t need = json_write(root, out, out_size);
    if (need >= out_size || need > contract_budget) {
        if (out_size)
            out[0] = 0;
        return 0;
    }
    return need;
}

size_t zcl_command_registry_menu_json(const struct zcl_command_registry *registry,
                                      const char *path, char *out,
                                      size_t out_size)
{
    const char *wanted = path && path[0] && strcmp(path, "root") != 0
        ? path : "";
    const struct zcl_command_spec *node = NULL;
    if (wanted[0]) {
        node = zcl_command_registry_find(registry, wanted, NULL);
        if (!node)
            return 0;
        if (!command_is_branch(node))
            return zcl_command_registry_describe_json(registry, wanted,
                                                       out, out_size);
    }

    char digest[72];
    zcl_command_registry_digest(registry, digest);
    struct json_value root, children;
    json_init(&root);
    json_init(&children);
    json_set_object(&root);
    json_set_array(&children);
    bool ok = json_push_kv_str(&root, "schema", "zcl.command_menu.v1") &&
              json_push_kv_str(&root, "path", wanted[0] ? wanted : "root") &&
              json_push_kv_str(&root, "summary",
                               node ? node->summary :
                               "ZClassic23 sovereign command interface") &&
              json_push_kv_str(&root, "registry_digest", digest);
    for (size_t i = 0; ok && registry && i < registry->count; i++) {
        const char *parent = registry->commands[i].parent;
        if (strcmp(parent ? parent : "", wanted) == 0)
            ok = push_child_summary(&children, &registry->commands[i]);
    }
    ok = ok && json_push_kv(&root, "children", &children);
    if (children.num_children > 0) {
        const struct json_value *first = json_at(&children, 0);
        const char *next_path = json_get_str(json_get(first, "path"));
        struct json_value next, empty;
        json_init(&next);
        json_init(&empty);
        json_set_object(&next);
        json_set_object(&empty);
        ok = ok && json_push_kv_str(&next, "command", "discover.help") &&
             json_push_kv_str(&empty, "path", next_path) &&
             json_push_kv(&next, "input", &empty) &&
             json_push_kv(&root, "next", &next);
        json_free(&empty);
        json_free(&next);
    }
    size_t result = ok
        ? write_bounded_json(&root, out, out_size,
                             wanted[0] ? ZCL_COMMAND_BRANCH_BUDGET
                                       : ZCL_COMMAND_ROOT_BUDGET)
        : 0;
    json_free(&children);
    json_free(&root);
    return result;
}

size_t zcl_command_registry_describe_json(
    const struct zcl_command_registry *registry, const char *path,
    char *out, size_t out_size)
{
    bool alias = false;
    const struct zcl_command_spec *spec =
        zcl_command_registry_find(registry, path, &alias);
    if (!spec)
        return 0;
    if (command_is_branch(spec))
        return zcl_command_registry_menu_json(registry, spec->path,
                                              out, out_size);

    char digest[72];
    zcl_command_registry_digest(registry, digest);
    struct json_value root, input, policy;
    json_init(&root);
    json_init(&input);
    json_init(&policy);
    json_set_object(&root);
    json_set_object(&input);
    json_set_object(&policy);
    bool ok = json_push_kv_str(&root, "schema", "zcl.command_spec.v1") &&
              json_push_kv_str(&root, "path", spec->path) &&
              json_push_kv_str(&root, "summary", spec->summary) &&
              json_push_kv_str(&root, "availability",
                               zcl_command_availability_name(
                                   spec->availability));
    if (spec->availability_reason && spec->availability_reason[0])
        ok = ok && json_push_kv_str(&root, "availability_reason",
                                    spec->availability_reason);
    ok = ok && json_push_kv_str(&root, "registry_digest", digest) &&
         json_push_kv_str(&input, "id", spec->input_schema) &&
         push_string_array_csv(&input, "allowed_keys", spec->input_keys) &&
         push_string_array_csv(&input, "positional_keys",
                               spec->positional_keys) &&
         json_push_kv(&root, "input_schema", &input) &&
         json_push_kv_str(&root, "output_schema", spec->output_schema) &&
         json_push_kv_str(&policy, "layer",
                          zcl_command_layer_name(spec->layer)) &&
         json_push_kv_str(&policy, "effect",
                          zcl_command_effect_name(spec->effect)) &&
         json_push_kv_str(&policy, "risk",
                          zcl_command_risk_name(spec->risk)) &&
         json_push_kv_str(&policy, "scope",
                          zcl_command_scope_name(spec->scope)) &&
         json_push_kv_str(&policy, "authority",
                          zcl_command_authority_name(spec->authority)) &&
         json_push_kv_str(&policy, "mode",
                          zcl_command_mode_name(spec->mode)) &&
         json_push_kv_str(&policy, "latency",
                          zcl_command_latency_name(spec->latency)) &&
         json_push_kv_str(&policy, "cost",
                          zcl_command_cost_name(spec->cost)) &&
         json_push_kv_str(&policy, "confirmation",
                          zcl_command_confirmation_name(spec->confirmation)) &&
         json_push_kv_bool(&policy, "deterministic",
                           (spec->traits & ZCL_COMMAND_TRAIT_DETERMINISTIC) != 0) &&
         json_push_kv_bool(&policy, "idempotent",
                           (spec->traits & ZCL_COMMAND_TRAIT_IDEMPOTENT) != 0) &&
         json_push_kv_int(&policy, "allowed_lanes", spec->allowed_lanes) &&
         json_push_kv_int(&policy, "required_capabilities",
                          (int64_t)spec->required_capabilities) &&
         json_push_kv(&root, "policy", &policy) &&
         json_push_kv_str(&root, "example", spec->example);
    if (spec->aliases && spec->aliases[0])
        ok = ok && push_string_array_csv(&root, "aliases", spec->aliases);
    if (alias)
        ok = ok && json_push_kv_str(&root, "canonical_path", spec->path);
    size_t result = ok
        ? write_bounded_json(&root, out, out_size, ZCL_COMMAND_SPEC_BUDGET)
        : 0;
    json_free(&policy);
    json_free(&input);
    json_free(&root);
    return result;
}

static bool contains_folded(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0])
        return false;
    size_t needle_len = strlen(needle);
    for (const unsigned char *h = (const unsigned char *)haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] &&
               tolower(h[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == needle_len)
            return true;
    }
    return false;
}

static bool normalize_query(const char *query, char out[129])
{
    if (!query)
        return false;
    size_t pos = 0;
    bool pending_space = false;
    for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
        if (*p < 0x20 && !isspace(*p))
            return false;
        if (isspace(*p)) {
            if (pos)
                pending_space = true;
            continue;
        }
        if (*p >= 0x80 || pos + (pending_space ? 1U : 0U) + 1U >= 129)
            return false;
        if (pending_space)
            out[pos++] = ' ';
        pending_space = false;
        out[pos++] = (char)tolower(*p);
    }
    out[pos] = 0;
    return pos > 0;
}

static int command_match_score(const struct zcl_command_spec *spec,
                               const char *query, const char **reason)
{
    if (strcmp(spec->path, query) == 0) {
        *reason = "exact_path";
        return 1000;
    }
    if (csv_token_equal(spec->aliases, query)) {
        *reason = "exact_alias";
        return 900;
    }
    if (csv_token_equal(spec->tags, query)) {
        *reason = "exact_tag";
        return 700;
    }
    if (strncmp(spec->path, query, strlen(query)) == 0) {
        *reason = "path_prefix";
        return 650;
    }
    if (contains_folded(spec->path, query)) {
        *reason = "path";
        return 550;
    }
    if (contains_folded(spec->tags, query)) {
        *reason = "tag";
        return 450;
    }
    if (contains_folded(spec->summary, query)) {
        *reason = "summary";
        return 300;
    }
    /* Multi-word fallback: a space-separated query ("dev loop") that matched
     * nothing as a single string still matches a command when EVERY word
     * appears (folded) in its path, tags, or summary. This lets natural
     * multi-word queries reach dotted command paths ("dev.loop.status") —
     * where the literal "dev loop" is never a substring. Single-word queries
     * never enter this block (no space), so all scoring above is unchanged. */
    if (strchr(query, ' ')) {
        size_t words = 0, matched = 0;
        for (const char *p = query; *p;) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            const char *start = p;
            while (*p && *p != ' ')
                p++;
            size_t wl = (size_t)(p - start);
            char word[129];
            if (wl == 0 || wl >= sizeof(word))
                return 0;
            memcpy(word, start, wl);
            word[wl] = 0;
            words++;
            if (contains_folded(spec->path, word) ||
                contains_folded(spec->tags, word) ||
                contains_folded(spec->summary, word))
                matched++;
        }
        if (words >= 2 && matched == words) {
            *reason = "terms";
            return 250;
        }
    }
    return 0;
}

struct search_hit {
    const struct zcl_command_spec *spec;
    const char *reason;
    int score;
};

static bool hit_before(const struct search_hit *a, const struct search_hit *b)
{
    return a->score > b->score ||
           (a->score == b->score && strcmp(a->spec->path, b->spec->path) < 0);
}

size_t zcl_command_registry_search_json(
    const struct zcl_command_registry *registry, const char *query,
    char *out, size_t out_size)
{
    char normalized[129];
    if (!registry || !normalize_query(query, normalized))
        return 0;
    struct search_hit hits[ZCL_COMMAND_SEARCH_LIMIT] = {0};
    size_t hit_count = 0, total = 0;
    for (size_t i = 0; i < registry->count; i++) {
        const char *reason = NULL;
        int score = command_match_score(&registry->commands[i], normalized,
                                        &reason);
        if (score == 0)
            continue;
        total++;
        struct search_hit candidate = {
            .spec = &registry->commands[i], .reason = reason, .score = score
        };
        size_t insert = hit_count;
        while (insert > 0 && hit_before(&candidate, &hits[insert - 1]))
            insert--;
        if (insert >= ZCL_COMMAND_SEARCH_LIMIT)
            continue;
        size_t end = hit_count < ZCL_COMMAND_SEARCH_LIMIT
            ? hit_count : ZCL_COMMAND_SEARCH_LIMIT - 1;
        while (end > insert) {
            hits[end] = hits[end - 1];
            end--;
        }
        hits[insert] = candidate;
        if (hit_count < ZCL_COMMAND_SEARCH_LIMIT)
            hit_count++;
    }

    char digest[72];
    zcl_command_registry_digest(registry, digest);
    struct json_value root, matches;
    json_init(&root);
    json_init(&matches);
    json_set_object(&root);
    json_set_array(&matches);
    bool ok = json_push_kv_str(&root, "schema", "zcl.command_search.v1") &&
              json_push_kv_str(&root, "query", normalized) &&
              json_push_kv_str(&root, "registry_digest", digest);
    for (size_t i = 0; ok && i < hit_count; i++) {
        struct json_value match;
        json_init(&match);
        json_set_object(&match);
        ok = json_push_kv_str(&match, "path", hits[i].spec->path) &&
             json_push_kv_str(&match, "reason", hits[i].reason) &&
             json_push_kv_str(&match, "risk",
                              zcl_command_risk_name(hits[i].spec->risk)) &&
             json_push_kv_str(&match, "latency",
                              zcl_command_latency_name(
                                  hits[i].spec->latency)) &&
             json_push_kv_str(&match, "availability",
                              zcl_command_availability_name(
                                  hits[i].spec->availability)) &&
             json_push_back(&matches, &match);
        json_free(&match);
    }
    ok = ok && json_push_kv(&root, "matches", &matches) &&
         json_push_kv_int(&root, "count", (int64_t)hit_count) &&
         json_push_kv_int(&root, "total_matches", (int64_t)total) &&
         json_push_kv_bool(&root, "truncated", total > hit_count);
    if (hit_count > 0) {
        struct json_value next, input;
        json_init(&next);
        json_init(&input);
        json_set_object(&next);
        json_set_object(&input);
        ok = ok && json_push_kv_str(&next, "command", "discover.describe") &&
             json_push_kv_str(&input, "path", hits[0].spec->path) &&
             json_push_kv(&next, "input", &input) &&
             json_push_kv(&root, "next", &next);
        json_free(&input);
        json_free(&next);
    }
    size_t result = ok
        ? write_bounded_json(&root, out, out_size, ZCL_COMMAND_LIST_BUDGET)
        : 0;
    json_free(&matches);
    json_free(&root);
    return result;
}

static bool lane_allowed(const struct zcl_command_spec *spec,
                         const struct zcl_command_context *context)
{
    if (!spec || spec->allowed_lanes == 0)
        return false;
    if (spec->allowed_lanes & ZCL_COMMAND_LANE_LOCAL)
        return true;
    const char *lane = context ? context->operator_lane : NULL;
    if (!lane || !lane[0])
        return false;
    if (strcmp(lane, "dev") == 0)
        return (spec->allowed_lanes & ZCL_COMMAND_LANE_DEV) != 0;
    if (strcmp(lane, "canonical") == 0 || strcmp(lane, "live") == 0)
        return (spec->allowed_lanes & ZCL_COMMAND_LANE_CANONICAL) != 0;
    if (strcmp(lane, "soak") == 0)
        return (spec->allowed_lanes & ZCL_COMMAND_LANE_SOAK) != 0;
    if (strcmp(lane, "offline-copy") == 0)
        return (spec->allowed_lanes & ZCL_COMMAND_LANE_OFFLINE_COPY) != 0;
    return false;
}

static bool push_next_array(struct json_value *root,
                            const struct zcl_command_reply *reply)
{
    struct json_value array;
    json_init(&array);
    json_set_array(&array);
    bool ok = true;
    for (size_t i = 0; ok && i < reply->next_count; i++) {
        struct json_value item, input;
        json_init(&item);
        json_init(&input);
        json_set_object(&item);
        if (!json_read(&input, reply->next[i].input_json,
                       strlen(reply->next[i].input_json)) ||
            input.type != JSON_OBJ) {
            json_free(&input);
            json_init(&input);
            json_set_object(&input);
        }
        ok = json_push_kv_str(&item, "command", reply->next[i].command) &&
             json_push_kv(&item, "input", &input) &&
             json_push_kv_str(&item, "reason", reply->next[i].reason) &&
             json_push_back(&array, &item);
        json_free(&input);
        json_free(&item);
    }
    ok = ok && json_push_kv(root, "next", &array);
    json_free(&array);
    return ok;
}

static bool push_error(struct json_value *root,
                       const struct zcl_command_error *error)
{
    struct json_value object, blockers;
    json_init(&object);
    json_init(&blockers);
    json_set_object(&object);
    json_set_array(&blockers);
    bool ok = json_push_kv_str(&object, "code", error->code) &&
              json_push_kv_str(&object, "message", error->message) &&
              json_push_kv_str(&object, "phase", error->phase) &&
              json_push_kv_bool(&object, "retryable", error->retryable) &&
              json_push_kv_bool(&object, "mutated", error->mutated);
    if (error->evidence[0])
        ok = ok && json_push_kv_str(&object, "evidence", error->evidence);
    if (error->failure_id[0])
        ok = ok && json_push_kv_str(&object, "failure_id",
                                    error->failure_id);
    ok = ok && json_push_kv(&object, "blockers", &blockers) &&
         json_push_kv(root, "error", &object);
    json_free(&blockers);
    json_free(&object);
    return ok;
}

static size_t serialize_reply(const struct zcl_command_spec *spec,
                              struct zcl_command_reply *reply,
                              bool invoked_by_alias,
                              uint64_t request_sequence,
                              int64_t elapsed_us,
                              size_t budget_bytes,
                              char *out, size_t out_size)
{
    char request_id[48];
    (void)snprintf(request_id, sizeof(request_id), "local-%016llx",
                   (unsigned long long)request_sequence);
    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    bool successful = reply->status == ZCL_COMMAND_STATUS_PASSED ||
                      reply->status == ZCL_COMMAND_STATUS_ACCEPTED;
    bool ok = json_push_kv_str(&root, "schema", "zcl.result.v1") &&
              json_push_kv_str(&root, "command", spec->path) &&
              json_push_kv_bool(&root, "ok", successful) &&
              json_push_kv_str(&root, "status",
                               zcl_command_status_name(reply->status)) &&
              json_push_kv_str(&root, "request_id", request_id) &&
              json_push_kv_int(&root, "elapsed_us",
                               elapsed_us < 0 ? 0 : elapsed_us);
    if (invoked_by_alias)
        ok = ok && json_push_kv_str(&root, "canonical_path", spec->path);
    if (successful) {
        ok = ok && json_push_kv_str(&root, "data_schema",
                                    reply->data_schema ? reply->data_schema :
                                    spec->output_schema) &&
             json_push_kv(&root, "data", &reply->data);
    } else {
        ok = ok && push_error(&root, &reply->error);
    }
    ok = ok && push_next_array(&root, reply);
    size_t contract = successful ? ZCL_COMMAND_RESULT_BUDGET
                                 : ZCL_COMMAND_ERROR_BUDGET;
    if (budget_bytes > 0 && budget_bytes < contract)
        contract = budget_bytes;
    size_t result = ok
        ? write_bounded_json(&root, out, out_size, contract)
        : 0;
    json_free(&root);
    return result;
}

size_t zcl_command_registry_execute_json(
    const struct zcl_command_registry *registry,
    const struct zcl_command_spec *spec,
    const struct zcl_command_context *context,
    const struct json_value *input,
    bool invoked_by_alias, const char *invoked_name,
    const char *view, size_t budget_bytes,
    size_t max_items, const char *cursor,
    char *out, size_t out_size, enum zcl_command_exit *exit_code)
{
    (void)registry;
    if (exit_code)
        *exit_code = ZCL_COMMAND_EXIT_INTERNAL;
    if (!spec || command_is_branch(spec) || !input || input->type != JSON_OBJ)
        return 0;

    /* Consult the hot-swap override snapshot for this resolved leaf before
     * falling back to the immutable catalog handler column. When no snapshot
     * is published this is a single atomic load + NULL check (zero overhead). */
    zcl_command_handler_fn handler = handler_override_lookup(spec->path);
    if (!handler)
        handler = spec->handler;

    struct zcl_command_reply reply;
    zcl_command_reply_init(&reply, spec->output_schema);
    int64_t started_us = platform_time_monotonic_us();
    if (spec->availability == ZCL_COMMAND_PLANNED) {
        zcl_command_reply_fail(&reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "COMMAND_PLANNED",
                               "dispatch", false, false,
                               "command is declared but not implemented",
                               spec->availability_reason);
        (void)zcl_command_reply_add_next(&reply, "discover.describe", "{}",
                                         "inspect availability and replacement");
    } else if (!handler) {
        zcl_command_reply_fail(&reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "COMMAND_COMPAT_ONLY", "dispatch", false,
                               false,
                               "canonical adapter is not executable yet",
                               spec->compat_target);
        (void)zcl_command_reply_add_next(&reply, "discover.describe", "{}",
                                         "inspect the compatibility target");
    } else if (!lane_allowed(spec, context)) {
        zcl_command_reply_fail(&reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_DENIED, "LANE_DENIED",
                               "authorize", false, false,
                               "command is not allowed in this lane",
                               context && context->operator_lane
                                   ? context->operator_lane : "unknown");
        (void)zcl_command_reply_add_next(&reply, "discover.describe", "{}",
                                         "inspect the declared lane scope");
    } else if (context &&
               (spec->required_capabilities &
                ~context->granted_capabilities) != 0) {
        zcl_command_reply_fail(&reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_DENIED,
                               "CAPABILITY_DENIED", "authorize", false,
                               false, "required capability was not granted",
                               spec->path);
        (void)zcl_command_reply_add_next(&reply, "discover.describe", "{}",
                                         "inspect required capabilities");
    } else {
        struct zcl_command_request request = {
            .spec = spec,
            .context = context,
            .input = input,
            .view = view && view[0] ? view : "normal",
            .budget_bytes = budget_bytes,
            .max_items = max_items,
            .cursor = cursor,
            .invoked_by_alias = invoked_by_alias,
            .invoked_name = invoked_name,
        };
        handler(&request, &reply);
    }

    bool status_ok = reply.status == ZCL_COMMAND_STATUS_PASSED ||
                     reply.status == ZCL_COMMAND_STATUS_ACCEPTED;
    if ((status_ok && reply.exit_code != ZCL_COMMAND_EXIT_OK) ||
        (!status_ok && reply.exit_code == ZCL_COMMAND_EXIT_OK) ||
        (status_ok && reply.error.code[0])) {
        zcl_command_reply_fail(&reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "INTERNAL_CONTRACT", "serialize", false,
                               reply.error.mutated,
                               "handler returned an inconsistent result",
                               spec->path);
    }
    int64_t elapsed_us = platform_time_monotonic_us() - started_us;
    uint64_t sequence = atomic_fetch_add_explicit(&g_request_sequence, 1,
                                                   memory_order_relaxed);
    size_t result = serialize_reply(spec, &reply, invoked_by_alias, sequence,
                                    elapsed_us, budget_bytes,
                                    out, out_size);
    if (result == 0) {
        static const char fallback[] =
            "{\"schema\":\"zcl.result.v1\",\"command\":\"internal\","
            "\"ok\":false,\"status\":\"failed\","
            "\"request_id\":\"local-overflow\",\"elapsed_us\":0,"
            "\"error\":{\"code\":\"RESPONSE_BUDGET_EXCEEDED\","
            "\"message\":\"bounded response could not be serialized\","
            "\"phase\":\"serialize\",\"retryable\":false,"
            "\"mutated\":false,\"blockers\":[]},\"next\":[]}";
        size_t len = sizeof(fallback) - 1;
        if (len < out_size && (budget_bytes == 0 || len <= budget_bytes)) {
            memcpy(out, fallback, len + 1);
            result = len;
            reply.exit_code = ZCL_COMMAND_EXIT_INTERNAL;
        }
    }
    if (exit_code)
        *exit_code = reply.exit_code;
    zcl_command_reply_free(&reply);
    return result;
}
