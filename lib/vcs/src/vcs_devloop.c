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
#include <sys/wait.h>
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

/* The first snapshot is generation-neutral bootstrap work. It may need to
 * durably store thousands of blobs, so detach it from the save-to-verdict
 * latency path. A singleton flock prevents two rapid cycles from launching
 * competing baselines. The double fork means a persistent watcher never
 * accumulates an unreaped child. */
static bool queue_initial_baseline(const char *repo_root)
{
    char dir[PATH_MAX], lock_path[PATH_MAX], log_path[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/.zvcs", repo_root);
    if (n <= 0 || (size_t)n >= sizeof(dir) ||
        (mkdir(dir, 0700) != 0 && errno != EEXIST))
        return false;
    n = snprintf(lock_path, sizeof(lock_path), "%s/bootstrap.lock", dir);
    if (n <= 0 || (size_t)n >= sizeof(lock_path))
        return false;
    n = snprintf(log_path, sizeof(log_path), "%s/bootstrap.log", dir);
    if (n <= 0 || (size_t)n >= sizeof(log_path))
        return false;

    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0)
        return false;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        bool already_running = errno == EWOULDBLOCK || errno == EAGAIN;
        close(lock_fd);
        return already_running;
    }

    pid_t launcher = fork();
    if (launcher < 0) {
        close(lock_fd);
        return false;
    }
    if (launcher == 0) {
        if (setsid() < 0)
            _exit(1);
        pid_t worker = fork();
        if (worker < 0)
            _exit(1);
        if (worker > 0)
            _exit(0);

        int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        int log_fd = open(log_path,
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        if (log_fd >= 0) {
            (void)dup2(log_fd, STDOUT_FILENO);
            (void)dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        struct vcs_devloop_verdict baseline = {
            .verdict_status = 0,
            .phase = "bootstrap_baseline",
            .elapsed_ms = 0,
        };
        struct vcs_devloop_anchor_result result = {
            .status = VCS_DEVLOOP_ANCHOR_ERROR,
        };
        anchor_cycle_sync(repo_root, &baseline, &result);
        if (result.status == VCS_DEVLOOP_ANCHOR_OK)
            fprintf(stderr, "[vcs.devloop] initial baseline complete\n");
        else
            fprintf(stderr, "[vcs.devloop] initial baseline failed: %s\n",
                    result.error[0] ? result.error : "unknown error");
        close(lock_fd);
        _exit(result.status == VCS_DEVLOOP_ANCHOR_OK ? 0 : 1);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(launcher, &status, 0);
    } while (waited < 0 && errno == EINTR);
    close(lock_fd);
    return waited == launcher && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
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
        snprintf(out->error, sizeof(out->error),
                 "generation-neutral initial ZVCS baseline is still building; this cycle is unanchored");
        return;
    }

    if (v->defer_initial_snapshot && !durable_history_present(repo_root)) {
        if (!queue_initial_baseline(repo_root)) {
            snprintf(out->error, sizeof(out->error),
                     "initial ZVCS baseline could not be queued");
            LOG_WARN("vcs.devloop", "anchor_cycle: baseline queue failed");
            return;
        }
        out->status = VCS_DEVLOOP_ANCHOR_DEFERRED;
        snprintf(out->error, sizeof(out->error),
                 "generation-neutral initial ZVCS baseline queued; this cycle is unanchored");
        return;
    }

    anchor_cycle_sync(repo_root, v, out);
}
