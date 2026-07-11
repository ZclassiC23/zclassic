/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * vcs — the ZVCS façade: open/close a repo and the high-level verbs
 * (snapshot, status, log, revert) that compose the object store, manifest,
 * index, commit log, and seal guard.
 *
 * A ZVCS repo lives in the working copy beside .git/ as <repo_root>/.zvcs/:
 *   objects/     content-addressed blob + manifest store (vcs_object)
 *   commits.log  append-only self-verifying commit log (event_log)
 *   index.kv     derived SQLite WAL: stat-cache, refs, seal_pin, anchors
 *
 * "code fearlessly, not recklessly": a snapshot that would change a sealed
 * path (the consensus core + neighbours) is REFUSED (returns VCS_REFUSED,
 * exit 3) unless a one-shot unseal token authorises it. */

#ifndef ZCL_VCS_H
#define ZCL_VCS_H

#include "vcs/vcs_commit.h"
#include "vcs/vcs_index.h"
#include "vcs/vcs_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return codes shared by the façade verbs. */
enum vcs_result {
    VCS_OK        = 0,
    VCS_ERR       = -1,
    VCS_ENOTIMPL  = -2,
    VCS_REFUSED   = 3,   /* sealed-path change without a valid unseal token */
};

struct vcs_repo;

/* Metadata bound into a snapshot's commit record. String fields may be NULL
 * (=> empty); the 32-byte hash pointers may be NULL (=> all-zero). */
struct vcs_snapshot_meta {
    uint32_t       verdict_status;
    const char    *phase;
    uint64_t       elapsed_ms;
    const uint8_t *generation_sha256;  /* 32 bytes or NULL */
    const uint8_t *failure_hash;       /* 32 bytes or NULL */
    const char    *agent_id;
    const char    *session_id;
    const char    *task_ref;
};

/* Open (creating .zvcs/ if needed) the repo rooted at repo_root. NULL on
 * failure. */
struct vcs_repo *vcs_open(const char *repo_root);
void vcs_close(struct vcs_repo *r);

/* Accessors (handy for tests / seal-token grants). */
struct vcs_index *vcs_repo_index(struct vcs_repo *r);
const char       *vcs_repo_root(struct vcs_repo *r);

/* Take a snapshot: build the worktree manifest, store dirty blobs + the
 * manifest, check the seal, append a commit, and advance HEAD/anchor/seal_pin.
 * On success writes the 32-byte commit id to out_commit_id. Returns VCS_OK,
 * VCS_REFUSED (sealed-path change), or VCS_ERR. */
int vcs_snapshot(struct vcs_repo *r, const struct vcs_snapshot_meta *meta,
                 uint8_t out_commit_id[32]);

/* Report working-tree changes vs HEAD (stat-cache warm path). Fires cb per
 * differing path (may be NULL to just count) and writes the change count to
 * *out_nchanges (may be NULL). Returns VCS_OK / VCS_ERR. */
int vcs_status(struct vcs_repo *r, vcs_diff_cb cb, void *user,
               size_t *out_nchanges);

/* Walk the commit log newest-first, invoking cb per commit until it returns
 * false or `limit` commits are emitted (0 = no limit). Returns VCS_OK/ERR. */
typedef bool (*vcs_log_cb)(const struct vcs_commit *c,
                           const uint8_t commit_id[32], void *user);
int vcs_log(struct vcs_repo *r, size_t limit, vcs_log_cb cb, void *user);

/* Restore the worktree to the manifest of target_commit (overwrite differing
 * tracked files atomically, delete files absent from the target), then record
 * the restoration as a forward commit (history stays append-only) whose id is
 * written to out_new_commit. If relink_generation is true the binary-generation
 * relink is NOT wired in v1 (Wave 3.3): the source revert + forward commit are
 * still performed, and the call returns VCS_ENOTIMPL to signal the relink half
 * did not run. Returns VCS_OK, VCS_ENOTIMPL, VCS_REFUSED, or VCS_ERR. */
int vcs_revert(struct vcs_repo *r, const uint8_t target_commit[32],
               bool relink_generation, uint8_t out_new_commit[32]);

#endif /* ZCL_VCS_H */
