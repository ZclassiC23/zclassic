/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * vcs_devloop — the dev-loop <-> ZVCS glue: turns one green
 * `zcl.dev_cycle.v1` verdict (tools/dev/devloop_cycle.c:finish_cycle()) into
 * one auto-anchored ZVCS commit binding the source tree to the verdict and
 * the binary generation it produced.
 *
 * FAIL-OPEN BY DESIGN: this is called from the hot dev-loop path on every
 * green cycle. A ZVCS failure here must never fail the cycle or crash the
 * loop — every path returns a populated result instead of using the
 * process-terminating LOG_FAIL/LOG_ERR/LOG_NULL macros. The one exception
 * that gets a distinct, loud status is a sealed-path refusal
 * (VCS_DEVLOOP_ANCHOR_REFUSED): the dev-loop publish has ALREADY happened by
 * this point in the pipeline (this hook runs after finish_cycle's own
 * publish/hotswap step), so a refusal here is advisory only, not a block —
 * Wave 2.4 moves the seal check earlier, before publish. */

#ifndef ZCL_VCS_DEVLOOP_H
#define ZCL_VCS_DEVLOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The subset of a dev-cycle verdict this glue needs, deliberately narrow —
 * every field is something tools/dev/devloop_cycle.c:finish_cycle() already
 * has in scope or can read from state it already touches (the
 * zcl.agent_dev_deploy.v1 deploy-state file for a reload cycle, or the
 * hotswap load report's artifact_sha256 for a hotswap cycle).
 *
 * generation_hex, when non-empty, MUST be exactly 64 lowercase/uppercase hex
 * characters (a raw SHA-256 digest) — the binary identity this cycle
 * produced. An unparsable non-empty value is treated as absent (the commit
 * still lands, with an all-zero generation_sha256) rather than failing the
 * anchor. */
struct vcs_devloop_verdict {
    uint32_t    verdict_status;  /* 0 = passed; any other value = not passed */
    const char *phase;           /* e.g. "resident_commit", "transactional_reload" */
    int64_t     elapsed_ms;
    const char *generation_hex;  /* 64 hex chars, or NULL/empty if unknown */
    const char *agent_id;        /* from ZCL_AGENT_ID, or NULL/empty */
    const char *session_id;      /* from ZCL_SESSION_ID, or NULL/empty */
    const char *task_ref;        /* from ZCL_TASK_REF, or NULL/empty */
    /* A fresh repository has no stat cache or object baseline and can require
     * thousands of durable writes. When true, queue that generation-neutral
     * baseline out of band and return DEFERRED instead of blocking the edit
     * verdict. Existing repositories still anchor synchronously. */
    bool        defer_initial_snapshot;
};

enum vcs_devloop_anchor_status {
    VCS_DEVLOOP_ANCHOR_OK      = 0,  /* committed; out->commit_id is valid */
    VCS_DEVLOOP_ANCHOR_ERROR   = 1,  /* vcs failure; fail-open, see out->error */
    VCS_DEVLOOP_ANCHOR_REFUSED = 2,  /* sealed-path change refused (advisory) */
    VCS_DEVLOOP_ANCHOR_DEFERRED = 3, /* generation-neutral baseline queued */
};

struct vcs_devloop_anchor_result {
    enum vcs_devloop_anchor_status status;
    uint8_t commit_id[32];  /* valid iff status == VCS_DEVLOOP_ANCHOR_OK */
    char    error[256];     /* human-readable detail iff status != OK */
};

/* Anchor one green dev-loop cycle: open (creating if absent) the ZVCS repo
 * rooted at repo_root, and take a snapshot bound to *v. Never aborts the
 * calling process. `out` is always fully populated (memset first) — check
 * out->status rather than a boolean return. Safe to call from the hot
 * dev-loop path: the object store dedupes unchanged files, so steady-state
 * cost tracks the change set. Callers that set defer_initial_snapshot get a
 * detached, generation-neutral first baseline and a DEFERRED result; this
 * prevents checkout size or filesystem latency from delaying publication. */
void vcs_devloop_anchor_cycle(const char *repo_root,
                              const struct vcs_devloop_verdict *v,
                              struct vcs_devloop_anchor_result *out);

/* Decode exactly 64 hex characters (either case) into 32 bytes. Returns
 * false — and leaves *out unmodified — on a wrong length or any non-hex
 * character; never crashes on a malformed or NULL input. */
bool vcs_devloop_hex32_decode(const char *hex, uint8_t out[32]);

#endif /* ZCL_VCS_DEVLOOP_H */
