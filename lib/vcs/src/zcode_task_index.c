/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcode_task_index — implementation of the rebuildable dev-task projection
 * declared in vcs/zcode_task_index.h. Every build re-walks the workspace CAS
 * and re-verifies each projected wire against its address; nothing is cached
 * across builds. */

#include "vcs/zcode_task_index.h"

#include "base/hex.h"
#include "base/log_macros.h"
#include "base/safe_alloc.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_agent_context.h"
#include "vcs/zcode_dev.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INDEX_LOG "vcs.task_index"

/* Wire magics from zcode_dev.c — the first 8 bytes decide whether an object
 * of the right size is even a candidate for projection. */
static const uint8_t task_magic[8] = {'Z','C','T','A','S','K','\r','\n'};
static const uint8_t candidate_magic[8] = {'Z','C','C','A','N','D','\r','\n'};
static const uint8_t context_magic[8] = {'Z','C','A','C','T','X','\r','\n'};

struct vcs_zcode_task_index {
    struct vcs_zcode_task_index_entry *tasks;
    size_t task_count;
    struct vcs_zcode_task_candidate_entry *candidates;
    size_t candidate_count;
    struct vcs_zcode_task_context_entry *contexts;
    size_t context_count;
};

static bool index_hex_lower(const char *s, size_t want)
{
    for (size_t i = 0; i < want; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return s[want] == '\0';
}

/* Project one CAS object when it is a verified task or candidate wire.
 * Wrong size or wrong magic means the object is another CAS citizen and is
 * skipped silently; right magic with a failed parse/validation/root check is
 * corruption and is logged. */
static void index_consider_object(const char *repo_root, const char *hex64,
                                  struct vcs_zcode_task_index *index,
                                  bool *cap_logged)
{
    uint8_t address[32];
    if (!zcl_hex_decode_lower(hex64, address, 32))
        return;
    uint8_t *wire = NULL;
    size_t len = 0;
    if (vcs_object_load_raw(repo_root, address, &wire, &len) != 0) {
        LOG_ERROR(INDEX_LOG, "unreadable CAS object %.8s", hex64);
        return;
    }
    if (len == VCS_ZCODE_TASK_WIRE_BYTES &&
        memcmp(wire, task_magic, sizeof(task_magic)) == 0) {
        struct vcs_zcode_task_v1 task;
        uint8_t root[32];
        bool ok = vcs_zcode_task_parse(wire, len, &task) == VCS_ZCODE_DEV_OK &&
            vcs_zcode_task_validate(&task) == VCS_ZCODE_DEV_OK &&
            vcs_zcode_task_root(&task, root) == VCS_ZCODE_DEV_OK &&
            memcmp(root, address, 32) == 0;
        if (!ok) {
            LOG_ERROR(INDEX_LOG, "skipping task-magic object %.8s: "
                      "parse, validation, or root agreement failed", hex64);
        } else if (index->task_count >= VCS_ZCODE_TASK_INDEX_MAX_TASKS) {
            if (!*cap_logged) {
                LOG_ERROR(INDEX_LOG, "task index cap %u reached",
                          VCS_ZCODE_TASK_INDEX_MAX_TASKS);
                *cap_logged = true;
            }
        } else {
            struct vcs_zcode_task_index_entry *e =
                &index->tasks[index->task_count++];
            memset(e, 0, sizeof(*e));
            zcl_hex_encode(address, 32, e->task_root_hex);
            zcl_hex_encode(task.source_root, 32, e->source_root_hex);
            zcl_hex_encode(task.goal_root, 32, e->goal_root_hex);
            zcl_hex_encode(task.proof_policy_root, 32,
                           e->proof_policy_root_hex);
            zcl_hex_encode(task.toolchain_capsule_root, 32,
                           e->toolchain_capsule_root_hex);
            e->expires_unix = task.expires_unix;
        }
    } else if (len == VCS_ZCODE_CANDIDATE_WIRE_BYTES &&
               memcmp(wire, candidate_magic, sizeof(candidate_magic)) == 0) {
        struct vcs_zcode_candidate_v1 candidate;
        uint8_t root[32];
        bool ok = vcs_zcode_candidate_parse(wire, len, &candidate) ==
                VCS_ZCODE_DEV_OK &&
            vcs_zcode_candidate_validate(&candidate) == VCS_ZCODE_DEV_OK &&
            vcs_zcode_candidate_root(&candidate, root) == VCS_ZCODE_DEV_OK &&
            memcmp(root, address, 32) == 0;
        if (!ok) {
            LOG_ERROR(INDEX_LOG, "skipping candidate-magic object %.8s: "
                      "parse, validation, or root agreement failed", hex64);
        } else if (index->candidate_count >=
                   VCS_ZCODE_TASK_INDEX_MAX_CANDIDATES) {
            if (!*cap_logged) {
                LOG_ERROR(INDEX_LOG, "candidate index cap %u reached",
                          VCS_ZCODE_TASK_INDEX_MAX_CANDIDATES);
                *cap_logged = true;
            }
        } else {
            struct vcs_zcode_task_candidate_entry *e =
                &index->candidates[index->candidate_count++];
            memset(e, 0, sizeof(*e));
            zcl_hex_encode(candidate.task_root, 32, e->task_root_hex);
            zcl_hex_encode(address, 32, e->candidate_root_hex);
            zcl_hex_encode(candidate.author_pubkey, 32, e->author_pubkey_hex);
            e->sequence = candidate.sequence;
            e->created_unix = candidate.created_unix;
        }
    } else if (len >= VCS_ZCODE_AGENT_CONTEXT_FIXED_BYTES &&
               memcmp(wire, context_magic, sizeof(context_magic)) == 0) {
        struct vcs_zcode_agent_context_v1 context;
        uint8_t root[32];
        bool parsed = vcs_zcode_agent_context_parse(
                wire, len, VCS_ZCODE_TASK_MAX_CONTEXT_BYTES, &context) ==
                VCS_ZCODE_AGENT_CONTEXT_OK;
        bool ok = parsed && vcs_zcode_agent_context_root(
                &context, VCS_ZCODE_TASK_MAX_CONTEXT_BYTES, root) ==
                VCS_ZCODE_AGENT_CONTEXT_OK && memcmp(root, address, 32) == 0;
        if (!ok) {
            LOG_ERROR(INDEX_LOG, "skipping context-magic object %.8s: "
                      "parse, validation, or root agreement failed", hex64);
        } else if (index->context_count >= VCS_ZCODE_TASK_INDEX_MAX_CONTEXTS) {
            if (!*cap_logged) {
                LOG_ERROR(INDEX_LOG, "context index cap %u reached",
                          VCS_ZCODE_TASK_INDEX_MAX_CONTEXTS);
                *cap_logged = true;
            }
        } else {
            struct vcs_zcode_task_context_entry *e =
                &index->contexts[index->context_count++];
            memset(e, 0, sizeof(*e));
            zcl_hex_encode(context.task_root, 32, e->task_root_hex);
            zcl_hex_encode(address, 32, e->context_root_hex);
            (void)snprintf(e->query, sizeof(e->query), "%s", context.query);
            e->wire_bytes = len;
            e->file_count = (uint32_t)context.file_count;
            for (size_t i = 0; i < context.file_count; i++)
                e->excerpt_bytes += context.files[i].content_len;
        }
        if (parsed) vcs_zcode_agent_context_free(&context);
    }
    free(wire);
}

static void index_scan_shard(const char *repo_root, const char *shard_path,
                             const char *shard,
                             struct vcs_zcode_task_index *index,
                             bool *cap_logged)
{
    DIR *d = opendir(shard_path);
    if (!d) {
        LOG_ERROR(INDEX_LOG, "cannot open CAS shard %s", shard_path);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!index_hex_lower(de->d_name, 62))
            continue;
        char hex64[65];
        int n = snprintf(hex64, sizeof(hex64), "%s%s", shard, de->d_name);
        if (n != 64)
            continue;
        index_consider_object(repo_root, hex64, index, cap_logged);
    }
    closedir(d);
}

static int index_task_cmp(const void *a, const void *b)
{
    return strcmp(((const struct vcs_zcode_task_index_entry *)a)->task_root_hex,
                  ((const struct vcs_zcode_task_index_entry *)b)->task_root_hex);
}

static int index_candidate_cmp(const void *a, const void *b)
{
    return strcmp(
        ((const struct vcs_zcode_task_candidate_entry *)a)->candidate_root_hex,
        ((const struct vcs_zcode_task_candidate_entry *)b)->candidate_root_hex);
}

static void index_derive_states(struct vcs_zcode_task_index *index,
                                int64_t now_unix)
{
    for (size_t i = 0; i < index->task_count; i++) {
        struct vcs_zcode_task_index_entry *e = &index->tasks[i];
        for (size_t c = 0; c < index->candidate_count; c++)
            if (strcmp(index->candidates[c].task_root_hex,
                       e->task_root_hex) == 0)
                e->candidate_count++;
        e->expired = now_unix > 0 && now_unix >= e->expires_unix;
        const char *state = e->expired ? VCS_ZCODE_TASK_STATE_EXPIRED
            : e->candidate_count > 0 ? VCS_ZCODE_TASK_STATE_CANDIDATE_ADMITTED
            : VCS_ZCODE_TASK_STATE_AWAITING_CANDIDATE;
        (void)snprintf(e->state, sizeof(e->state), "%s", state);
    }
}

struct vcs_zcode_task_index *vcs_zcode_task_index_build(
    const char *repo_root, int64_t now_unix)
{
    if (!repo_root)
        LOG_RETURN(NULL, INDEX_LOG, "null repo_root");
    struct vcs_zcode_task_index *index =
        zcl_malloc(sizeof(*index), "vcs_zcode_task_index");
    if (!index)
        LOG_RETURN(NULL, INDEX_LOG, "index alloc");
    memset(index, 0, sizeof(*index));
    index->tasks = zcl_malloc(sizeof(*index->tasks) *
                              VCS_ZCODE_TASK_INDEX_MAX_TASKS, "task_index_rows");
    index->candidates = zcl_malloc(sizeof(*index->candidates) *
                                   VCS_ZCODE_TASK_INDEX_MAX_CANDIDATES,
                                   "task_index_candidates");
    index->contexts = zcl_malloc(sizeof(*index->contexts) *
                                 VCS_ZCODE_TASK_INDEX_MAX_CONTEXTS,
                                 "task_index_contexts");
    if (!index->tasks || !index->candidates || !index->contexts) {
        free(index->contexts);
        free(index->candidates);
        free(index->tasks);
        free(index);
        LOG_RETURN(NULL, INDEX_LOG, "entry arrays");
    }
    char objects[4400];
    int n = snprintf(objects, sizeof(objects), "%s/.zvcs/objects", repo_root);
    if (n <= 0 || (size_t)n >= sizeof(objects)) {
        vcs_zcode_task_index_free(index);
        LOG_RETURN(NULL, INDEX_LOG, "objects path too long");
    }
    DIR *d = opendir(objects);
    if (!d)
        return index; /* no object store yet: an empty projection, not an error */
    bool cap_logged = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!index_hex_lower(de->d_name, 2))
            continue; /* skips "tmp" and any non-shard entry */
        char shard_path[4400];
        n = snprintf(shard_path, sizeof(shard_path), "%s/%s", objects,
                     de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(shard_path))
            continue;
        index_scan_shard(repo_root, shard_path, de->d_name, index, &cap_logged);
    }
    closedir(d);
    if (index->task_count > 1)
        qsort(index->tasks, index->task_count, sizeof(*index->tasks),
              index_task_cmp);
    if (index->candidate_count > 1)
        qsort(index->candidates, index->candidate_count,
              sizeof(*index->candidates), index_candidate_cmp);
    index_derive_states(index, now_unix);
    return index;
}

void vcs_zcode_task_index_free(struct vcs_zcode_task_index *index)
{
    if (!index)
        return;
    free(index->contexts);
    free(index->candidates);
    free(index->tasks);
    free(index);
}

size_t vcs_zcode_task_index_task_count(
    const struct vcs_zcode_task_index *index)
{
    return index ? index->task_count : 0;
}

const struct vcs_zcode_task_index_entry *vcs_zcode_task_index_task_at(
    const struct vcs_zcode_task_index *index, size_t i)
{
    if (!index || i >= index->task_count)
        return NULL;
    return &index->tasks[i];
}

size_t vcs_zcode_task_index_candidate_count(
    const struct vcs_zcode_task_index *index)
{
    return index ? index->candidate_count : 0;
}

const struct vcs_zcode_task_candidate_entry *
vcs_zcode_task_index_candidate_at(const struct vcs_zcode_task_index *index,
                                  size_t i)
{
    if (!index || i >= index->candidate_count)
        return NULL;
    return &index->candidates[i];
}

const struct vcs_zcode_task_context_entry *
vcs_zcode_task_index_context_for_task(
    const struct vcs_zcode_task_index *index, const char *task_root_hex,
    bool *ambiguous)
{
    if (ambiguous) *ambiguous = false;
    if (!index || !task_root_hex) return NULL;
    const struct vcs_zcode_task_context_entry *found = NULL;
    for (size_t i = 0; i < index->context_count; i++) {
        const struct vcs_zcode_task_context_entry *at = &index->contexts[i];
        if (strcmp(at->task_root_hex, task_root_hex) != 0) continue;
        if (found) {
            if (ambiguous) *ambiguous = true;
            return NULL;
        }
        found = at;
    }
    return found;
}

const struct vcs_zcode_task_index_entry *vcs_zcode_task_index_find(
    const struct vcs_zcode_task_index *index, const uint8_t task_root[32])
{
    if (!index || !task_root)
        return NULL;
    char root_hex[65];
    zcl_hex_encode(task_root, 32, root_hex);
    for (size_t i = 0; i < index->task_count; i++)
        if (strcmp(index->tasks[i].task_root_hex, root_hex) == 0)
            return &index->tasks[i];
    return NULL;
}

static bool index_task_has_author(const struct vcs_zcode_task_index *index,
                                  const char *task_root_hex,
                                  const char *author)
{
    for (size_t c = 0; c < index->candidate_count; c++) {
        const struct vcs_zcode_task_candidate_entry *e =
            &index->candidates[c];
        if (strcmp(e->task_root_hex, task_root_hex) == 0 &&
            strncmp(e->author_pubkey_hex, author, strlen(author)) == 0)
            return true;
    }
    return false;
}

static bool index_entry_matches(
    const struct vcs_zcode_task_index *index,
    const struct vcs_zcode_task_index_entry *e,
    const struct vcs_zcode_task_search *s)
{
    if (s->task_root && s->task_root[0] &&
        strncmp(e->task_root_hex, s->task_root, strlen(s->task_root)) != 0)
        return false;
    if (s->source_root && s->source_root[0] &&
        strncmp(e->source_root_hex, s->source_root, strlen(s->source_root)) != 0)
        return false;
    if (s->state && s->state[0] && strcmp(e->state, s->state) != 0)
        return false;
    if (s->author && s->author[0] &&
        !index_task_has_author(index, e->task_root_hex, s->author))
        return false;
    return true;
}

size_t vcs_zcode_task_index_search(
    const struct vcs_zcode_task_index *index,
    const struct vcs_zcode_task_search *search,
    const struct vcs_zcode_task_index_entry **out, size_t out_cap)
{
    if (!index || !search)
        return 0;
    size_t total = 0;
    for (size_t i = 0; i < index->task_count; i++) {
        if (!index_entry_matches(index, &index->tasks[i], search))
            continue;
        if (out && total < out_cap)
            out[total] = &index->tasks[i];
        total++;
    }
    return total;
}
