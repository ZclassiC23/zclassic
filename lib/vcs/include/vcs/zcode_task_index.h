/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcode_task_index — the local ZCODE dev-task search index. This is a
 * REBUILDABLE PROJECTION over the workspace CAS (<repo_root>/.zvcs/objects):
 * the persisted task.v1 and candidate.v1 wires stay authoritative and this
 * index holds no truth of its own — like package_index, it is rebuilt from
 * the canonical objects on every build and may be discarded at any time. No
 * task table is created.
 *
 * A task entry projects one persisted task wire whose parse, structural
 * validation, and rederived root all succeed and whose root equals its CAS
 * address (the object file name). A candidate entry projects one persisted
 * candidate wire under the same discipline. Objects of any other size or
 * magic are other CAS citizens and are skipped unread; a file carrying task
 * or candidate magic that fails parse/validation/root agreement is logged
 * and skipped — a forged or misplaced file cannot enter the projection.
 * Entries are sorted by root hex for deterministic output. Bounds: at most
 * VCS_ZCODE_TASK_INDEX_MAX_TASKS tasks and MAX_CANDIDATES candidates.
 *
 * Read-only: the index never writes to the CAS and never verifies receipt
 * signatures (evidence evaluation owns those). */

#ifndef ZCL_VCS_ZCODE_TASK_INDEX_H
#define ZCL_VCS_ZCODE_TASK_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_TASK_INDEX_MAX_TASKS 1024u
#define VCS_ZCODE_TASK_INDEX_MAX_CANDIDATES 4096u

/* Derived per-task states. EXPIRED takes precedence: an expired task is
 * refused by task_validate_at no matter what its candidates look like. */
#define VCS_ZCODE_TASK_STATE_EXPIRED "EXPIRED"
#define VCS_ZCODE_TASK_STATE_AWAITING_CANDIDATE "AWAITING_CANDIDATE"
#define VCS_ZCODE_TASK_STATE_CANDIDATE_ADMITTED "CANDIDATE_ADMITTED"

struct vcs_zcode_task_index_entry {
    char task_root_hex[65];
    char source_root_hex[65];
    char goal_root_hex[65];
    char proof_policy_root_hex[65];
    char toolchain_capsule_root_hex[65];
    int64_t expires_unix;
    bool expired;             /* at the build's now_unix */
    uint32_t candidate_count; /* projected candidates binding this task */
    char state[24];
};

struct vcs_zcode_task_candidate_entry {
    char task_root_hex[65];
    char candidate_root_hex[65];
    char author_pubkey_hex[65];
    uint64_t sequence;
    int64_t created_unix;
};

struct vcs_zcode_task_index; /* opaque */

/* Build the projection from repo_root's workspace CAS. A missing/empty
 * object store yields an empty index. NULL on hard allocation failure
 * (logged). now_unix drives the expired flag and derived state. */
struct vcs_zcode_task_index *vcs_zcode_task_index_build(
    const char *repo_root, int64_t now_unix);
void vcs_zcode_task_index_free(struct vcs_zcode_task_index *index);

size_t vcs_zcode_task_index_task_count(
    const struct vcs_zcode_task_index *index);
const struct vcs_zcode_task_index_entry *vcs_zcode_task_index_task_at(
    const struct vcs_zcode_task_index *index, size_t i);

size_t vcs_zcode_task_index_candidate_count(
    const struct vcs_zcode_task_index *index);
const struct vcs_zcode_task_candidate_entry *
vcs_zcode_task_index_candidate_at(const struct vcs_zcode_task_index *index,
                                  size_t i);

/* Look up one task entry by task root (32 bytes). NULL when absent. */
const struct vcs_zcode_task_index_entry *vcs_zcode_task_index_find(
    const struct vcs_zcode_task_index *index, const uint8_t task_root[32]);

struct vcs_zcode_task_search {
    const char *task_root;   /* hex prefix of the task root, or NULL */
    const char *source_root; /* hex prefix of the source root, or NULL */
    const char *author;      /* hex prefix of a candidate author, or NULL */
    const char *state;       /* exact VCS_ZCODE_TASK_STATE_* string, or NULL */
};

/* Bounded search: fills out[] (entry pointers, sorted order) with up to
 * out_cap matches of ALL given filters; returns the TOTAL number of
 * matches (>= the count written), so callers can flag truncation. */
size_t vcs_zcode_task_index_search(
    const struct vcs_zcode_task_index *index,
    const struct vcs_zcode_task_search *search,
    const struct vcs_zcode_task_index_entry **out, size_t out_cap);

#endif /* ZCL_VCS_ZCODE_TASK_INDEX_H */
