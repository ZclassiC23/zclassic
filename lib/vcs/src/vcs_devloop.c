/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * vcs_devloop — implementation. See vcs/vcs_devloop.h. */

#include "vcs/vcs_devloop.h"

#include "vcs/vcs.h"
#include "vcs/vcs_index.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool vcs_devloop_hex32_decode(const char *hex, uint8_t out[32])
{
    if (!hex || !out)
        return false;
    if (strlen(hex) != 64)
        return false;
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void anchor_cycle_sync(const char *repo_root,
                              const struct vcs_devloop_verdict *v,
                              struct vcs_devloop_anchor_result *out)
{
    struct vcs_repo *r = vcs_open(repo_root);
    if (!r) {
        snprintf(out->error, sizeof(out->error),
                 "vcs_open failed for repo_root=%s", repo_root);
        LOG_WARN("vcs.devloop", "anchor_cycle: vcs_open failed root=%s",
                 repo_root);
        return;
    }

    /* First-run ergonomics: a repo with no HEAD ref yet is about to take its
     * first snapshot of the whole worktree, which is the one call in the
     * hot dev-loop path that is not O(changed files). Log the one-time
     * cost rather than staying silent about it. */
    struct vcs_index *idx = vcs_repo_index(r);
    uint8_t head_probe[32];
    bool head_found = false;
    bool first_snapshot =
        idx && vcs_index_ref_get(idx, "HEAD", head_probe, &head_found) &&
        !head_found;

    uint8_t generation[32];
    memset(generation, 0, sizeof(generation));
    bool have_generation = v->generation_hex && v->generation_hex[0] &&
                          vcs_devloop_hex32_decode(v->generation_hex, generation);
    if (v->generation_hex && v->generation_hex[0] && !have_generation)
        LOG_WARN("vcs.devloop",
                 "anchor_cycle: unparsable generation hex (binding zero): %s",
                 v->generation_hex);

    struct vcs_snapshot_meta meta = {0};
    meta.verdict_status = v->verdict_status;
    meta.phase = v->phase;
    meta.elapsed_ms = v->elapsed_ms < 0 ? 0 : (uint64_t)v->elapsed_ms;
    meta.generation_sha256 = have_generation ? generation : NULL;
    meta.agent_id = v->agent_id;
    meta.session_id = v->session_id;
    meta.task_ref = v->task_ref;

    int64_t t0 = platform_time_monotonic_us();
    uint8_t commit_id[32];
    int rc = vcs_snapshot(r, &meta, commit_id);
    int64_t t1 = platform_time_monotonic_us();

    if (first_snapshot)
        LOG_INFO("vcs.devloop",
                 "anchor_cycle: first snapshot of the working tree took %lld ms",
                 (long long)((t1 - t0) / 1000));

    vcs_close(r);

    if (rc == VCS_OK) {
        out->status = VCS_DEVLOOP_ANCHOR_OK;
        memcpy(out->commit_id, commit_id, 32);
        return;
    }
    if (rc == VCS_REFUSED) {
        out->status = VCS_DEVLOOP_ANCHOR_REFUSED;
        snprintf(out->error, sizeof(out->error),
                 "sealed-path change refused (advisory here; the dev-loop "
                 "publish already happened) — run the owner-gated unseal "
                 "ritual before the next anchor");
        LOG_WARN("vcs.devloop",
                 "anchor_cycle: sealed-path refusal (advisory; publish already happened)");
        return;
    }
    snprintf(out->error, sizeof(out->error), "vcs_snapshot failed (rc=%d)", rc);
    LOG_WARN("vcs.devloop", "anchor_cycle: vcs_snapshot failed rc=%d", rc);
}

/* Open (creating if absent) .zvcs/bootstrap.lock for the baseline
 * singleton. Returns -1 on any setup failure. Never spawns a process —
 * open()/mkdir() only (the ZVCS-sovereignty lint gate requires lib/vcs,
 * being release-linkable, to stay process-spawn free). */
static int open_bootstrap_lock(const char *repo_root, char *lock_path,
                               size_t lock_path_sz)
{
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/.zvcs", repo_root);
    if (n <= 0 || (size_t)n >= sizeof(dir) ||
        (mkdir(dir, 0700) != 0 && errno != EEXIST))
        return -1;
    n = snprintf(lock_path, lock_path_sz, "%s/bootstrap.lock", dir);
    if (n <= 0 || (size_t)n >= lock_path_sz)
        return -1;
    return open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
}

/* The first snapshot is generation-neutral bootstrap work: it may need to
 * durably store thousands of blobs. lib/vcs runs it synchronously in the
 * caller's own thread of control — it never forks a worker to detach it
 * (that mechanics lives in the dev-only tools/dev/devloop_baseline.c, which
 * calls THIS function from a double-forked grandchild). A singleton flock
 * still keeps two concurrent callers (in-process or across processes) from
 * racing the same baseline. */
void vcs_devloop_run_initial_baseline(const char *repo_root,
                                      struct vcs_devloop_anchor_result *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->status = VCS_DEVLOOP_ANCHOR_ERROR;

    if (!repo_root || !repo_root[0]) {
        snprintf(out->error, sizeof(out->error),
                 "vcs_devloop: invalid repo_root");
        LOG_WARN("vcs.devloop", "run_initial_baseline: invalid repo_root");
        return;
    }

    char lock_path[PATH_MAX];
    int lock_fd = open_bootstrap_lock(repo_root, lock_path, sizeof(lock_path));
    if (lock_fd < 0) {
        snprintf(out->error, sizeof(out->error),
                 "could not open .zvcs/bootstrap.lock under %s", repo_root);
        LOG_WARN("vcs.devloop", "run_initial_baseline: lock open failed root=%s",
                 repo_root);
        return;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(lock_fd);
        out->status = VCS_DEVLOOP_ANCHOR_DEFERRED;
        snprintf(out->error, sizeof(out->error),
                 "another caller already holds the initial ZVCS baseline lock");
        return;
    }

    struct vcs_devloop_verdict baseline = {
        .verdict_status = 0,
        .phase = "bootstrap_baseline",
        .elapsed_ms = 0,
    };
    anchor_cycle_sync(repo_root, &baseline, out);
    if (out->status == VCS_DEVLOOP_ANCHOR_OK)
        LOG_INFO("vcs.devloop", "run_initial_baseline: complete root=%s",
                 repo_root);
    else
        LOG_WARN("vcs.devloop", "run_initial_baseline: failed root=%s: %s",
                 repo_root, out->error[0] ? out->error : "unknown error");

    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
}

static bool durable_history_present(const char *repo_root)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.zvcs/commits.log", repo_root);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static bool initial_baseline_running(const char *repo_root)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.zvcs/bootstrap.lock",
                     repo_root);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        bool running = errno == EWOULDBLOCK || errno == EAGAIN;
        close(fd);
        return running;
    }
    (void)flock(fd, LOCK_UN);
    close(fd);
    return false;
}

void vcs_devloop_anchor_cycle(const char *repo_root,
                              const struct vcs_devloop_verdict *v,
                              struct vcs_devloop_anchor_result *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->status = VCS_DEVLOOP_ANCHOR_ERROR;

    if (!repo_root || !repo_root[0] || !v) {
        snprintf(out->error, sizeof(out->error),
                 "vcs_devloop: invalid arguments (repo_root or verdict is NULL)");
        LOG_WARN("vcs.devloop", "anchor_cycle: invalid arguments");
        return;
    }

    if (v->defer_initial_snapshot && initial_baseline_running(repo_root)) {
        out->status = VCS_DEVLOOP_ANCHOR_DEFERRED;
        out->baseline_needed = false;
        snprintf(out->error, sizeof(out->error),
                 "generation-neutral initial ZVCS baseline is still building; this cycle is unanchored");
        return;
    }

    if (v->defer_initial_snapshot && !durable_history_present(repo_root)) {
        /* lib/vcs never launches the baseline itself (the ZVCS-sovereignty
         * lint gate forbids fork/exec here). Report that one is REQUIRED
         * and leave this cycle unanchored; the caller runs it —
         * synchronously via vcs_devloop_run_initial_baseline(), or detached
         * via the dev-only launcher in tools/dev/devloop_baseline.c. */
        out->status = VCS_DEVLOOP_ANCHOR_DEFERRED;
        out->baseline_needed = true;
        snprintf(out->error, sizeof(out->error),
                 "generation-neutral initial ZVCS baseline required; this cycle is unanchored");
        return;
    }

    anchor_cycle_sync(repo_root, v, out);
}
