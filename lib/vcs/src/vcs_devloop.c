/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * vcs_devloop — implementation. See vcs/vcs_devloop.h. */

#include "vcs/vcs_devloop.h"

#include "vcs/vcs.h"
#include "vcs/vcs_commit.h"
#include "vcs/vcs_index.h"
#include "vcs/vcs_object.h"

#include "base/hex.h"
#include "base/serialize_le.h"
#include "platform/time_compat.h"
#include "storage/event_log.h"
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

bool vcs_devloop_hex32_decode(const char *hex, uint8_t out[32])
{
    return zcl_hex_decode(hex, out, 32);
}

#define VCS_DEV_PROOF_WIRE_BYTES 268u
#define VCS_DEV_PUBLICATION_JOB_WIRE_BYTES 236u

static const uint8_t dev_proof_magic[8] = {'Z','D','P','F','1',0,0,0};
static const uint8_t publication_job_magic[8] =
    {'Z','P','J','B','1',0,0,0};

static bool publication_root_nonzero(const uint8_t root[32])
{
    uint8_t any = 0;
    if (!root) return false;
    for (size_t i = 0; i < 32; i++) any |= root[i];
    return any != 0;
}

static void publication_fixed(char *out, size_t cap, const char *value)
{
    memset(out, 0, cap);
    if (value)
        (void)snprintf(out, cap, "%s", value);
}

static bool publication_job_serialize(
    const struct vcs_devloop_publication_job *job,
    uint8_t wire[VCS_DEV_PUBLICATION_JOB_WIRE_BYTES])
{
    if (!job || !wire ||
        job->version != VCS_DEVLOOP_PUBLICATION_JOB_VERSION ||
        !publication_root_nonzero(job->vcs_commit_root) ||
        !publication_root_nonzero(job->source_tree_root) ||
        !publication_root_nonzero(job->proof_receipt_root) ||
        !publication_root_nonzero(job->source_identity_sha256) ||
        !publication_root_nonzero(job->source_cas_sha3)) {
        LOG_WARN("vcs.devloop", "publication job serialize: invalid roots");
        return false;
    }
    size_t off = 0;
    memcpy(wire + off, publication_job_magic, 8); off += 8;
    zcl_write_u32_le(wire + off, job->version); off += 4;
    memcpy(wire + off, job->vcs_commit_root, 32); off += 32;
    memcpy(wire + off, job->source_tree_root, 32); off += 32;
    memcpy(wire + off, job->proof_receipt_root, 32); off += 32;
    memcpy(wire + off, job->source_identity_sha256, 32); off += 32;
    memcpy(wire + off, job->source_cas_sha3, 32); off += 32;
    memcpy(wire + off, job->generation_sha256, 32); off += 32;
    memcpy(wire + off, job->parent_workspace_root, 32); off += 32;
    return off == VCS_DEV_PUBLICATION_JOB_WIRE_BYTES;
}

static bool publication_job_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_devloop_publication_job *out)
{
    if (!wire || !out || wire_len != VCS_DEV_PUBLICATION_JOB_WIRE_BYTES ||
        memcmp(wire, publication_job_magic, 8) != 0) {
        LOG_WARN("vcs.devloop", "publication job parse: invalid wire");
        return false;
    }
    struct vcs_devloop_publication_job parsed = {0};
    size_t off = 8;
    parsed.version = zcl_read_u32_le(wire + off); off += 4;
    memcpy(parsed.vcs_commit_root, wire + off, 32); off += 32;
    memcpy(parsed.source_tree_root, wire + off, 32); off += 32;
    memcpy(parsed.proof_receipt_root, wire + off, 32); off += 32;
    memcpy(parsed.source_identity_sha256, wire + off, 32); off += 32;
    memcpy(parsed.source_cas_sha3, wire + off, 32); off += 32;
    memcpy(parsed.generation_sha256, wire + off, 32); off += 32;
    memcpy(parsed.parent_workspace_root, wire + off, 32); off += 32;
    uint8_t checked[VCS_DEV_PUBLICATION_JOB_WIRE_BYTES];
    if (off != wire_len || !publication_job_serialize(&parsed, checked) ||
        memcmp(checked, wire, wire_len) != 0) {
        LOG_WARN("vcs.devloop", "publication job parse: noncanonical wire");
        return false;
    }
    *out = parsed;
    return true;
}

bool vcs_devloop_publication_job_load(
    const char *repo_root, const uint8_t job_root[32],
    struct vcs_devloop_publication_job *out)
{
    if (!repo_root || !repo_root[0] || !job_root || !out) {
        LOG_WARN("vcs.devloop", "publication job load: invalid arguments");
        return false;
    }
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (vcs_object_get(repo_root, job_root, VCS_TAG_PUBLICATION_JOB,
                       &wire, &wire_len) != 0) {
        LOG_WARN("vcs.devloop", "publication job load: object unavailable");
        return false;
    }
    bool ok = publication_job_parse(wire, wire_len, out);
    free(wire);
    return ok;
}

struct publication_queue_scan {
    const uint8_t *job_root;
    bool found;
};

static bool publication_queue_scan_cb(uint64_t offset,
                                      enum event_log_type type,
                                      const void *payload, size_t len,
                                      void *user)
{
    (void)offset;
    struct publication_queue_scan *scan = user;
    if (type == EV_VCS_PUBLICATION_JOB && len == 32 && payload &&
        memcmp(payload, scan->job_root, 32) == 0) {
        scan->found = true;
        return false;
    }
    return true;
}

static bool publication_queue_path(const char *repo_root, const char *name,
                                   char *out, size_t out_size)
{
    int n = repo_root && name
        ? snprintf(out, out_size, "%s/.zvcs/%s", repo_root, name) : -1;
    if (n <= 0 || (size_t)n >= out_size) {
        LOG_WARN("vcs.devloop", "publication queue path exceeds bound");
        return false;
    }
    return true;
}

bool vcs_devloop_publication_job_is_queued(
    const char *repo_root, const uint8_t job_root[32])
{
    if (!repo_root || !repo_root[0] || !job_root) {
        LOG_WARN("vcs.devloop", "publication queue status: invalid arguments");
        return false;
    }
    char path[PATH_MAX];
    if (!publication_queue_path(repo_root, "publication.log", path,
                                sizeof(path)))
        return false;
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    event_log_t *log = event_log_open(path);
    if (!log) {
        LOG_WARN("vcs.devloop", "publication queue status: open failed");
        return false;
    }
    struct publication_queue_scan scan = {.job_root = job_root};
    bool ok = event_log_stream(log, 0, publication_queue_scan_cb, &scan) == 0;
    event_log_close(log);
    if (!ok)
        LOG_WARN("vcs.devloop", "publication queue status: stream failed");
    return ok && scan.found;
}

bool vcs_devloop_publication_job_requeue(
    const char *repo_root, const uint8_t job_root[32], bool *reused_out)
{
    if (reused_out) *reused_out = false;
    struct vcs_devloop_publication_job checked;
    if (!repo_root || !repo_root[0] || !job_root ||
        !vcs_devloop_publication_job_load(repo_root, job_root, &checked)) {
        LOG_WARN("vcs.devloop", "publication requeue: invalid job");
        return false;
    }
    char lock_path[PATH_MAX], log_path[PATH_MAX];
    if (!publication_queue_path(repo_root, "publication.lock", lock_path,
                                sizeof(lock_path)) ||
        !publication_queue_path(repo_root, "publication.log", log_path,
                                sizeof(log_path)))
        return false;
    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0) {
        if (lock_fd >= 0) close(lock_fd);
        LOG_WARN("vcs.devloop", "publication requeue: lock failed");
        return false;
    }
    event_log_t *log = event_log_open(log_path);
    struct publication_queue_scan scan = {.job_root = job_root};
    bool ok = log &&
        event_log_stream(log, 0, publication_queue_scan_cb, &scan) == 0;
    if (ok && !scan.found)
        ok = event_log_append(log, EV_VCS_PUBLICATION_JOB, job_root, 32) !=
             UINT64_MAX;
    if (log) event_log_close(log);
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
    if (!ok) {
        LOG_WARN("vcs.devloop", "publication requeue: durable append failed");
        return false;
    }
    if (reused_out) *reused_out = scan.found;
    return true;
}

static bool publication_enqueue_from_commit(
    const char *repo_root, const struct vcs_devloop_verdict *verdict,
    const uint8_t commit_root[32], struct vcs_devloop_anchor_result *out)
{
    uint8_t source_identity[32], source_cas[32];
    size_t phase_len = verdict->phase ? strlen(verdict->phase) : 0;
    size_t scope_len = verdict->proof_scope ? strlen(verdict->proof_scope) : 0;
    if (!verdict->proof_complete || phase_len == 0 ||
        strcmp(verdict->phase, "verify") != 0 || phase_len >= 24 ||
        scope_len == 0 || scope_len >= 64 ||
        !vcs_devloop_hex32_decode(verdict->source_identity_hex,
                                  source_identity) ||
        !vcs_devloop_hex32_decode(verdict->source_cas_hex, source_cas)) {
        (void)snprintf(out->publication_error,
                       sizeof(out->publication_error), "%s",
                       "complete verify proof lacks exact bounded source identity, source CAS, or proof scope");
        LOG_WARN("vcs.devloop", "publication enqueue: incomplete proof basis");
        return false;
    }
    uint8_t *commit_preimage = NULL;
    size_t commit_len = 0;
    struct vcs_commit commit;
    if (vcs_object_get(repo_root, commit_root, VCS_TAG_COMMIT,
                       &commit_preimage, &commit_len) != 0 ||
        !vcs_commit_parse_preimage(commit_preimage, commit_len, &commit)) {
        free(commit_preimage);
        (void)snprintf(out->publication_error,
                       sizeof(out->publication_error), "%s",
                       "the committed ZVCS source object could not be reloaded");
        LOG_WARN("vcs.devloop", "publication enqueue: commit reload failed");
        return false;
    }
    free(commit_preimage);

    uint8_t proof_wire[VCS_DEV_PROOF_WIRE_BYTES] = {0};
    size_t off = 0;
    memcpy(proof_wire + off, dev_proof_magic, 8); off += 8;
    zcl_write_u32_le(proof_wire + off, 1); off += 4;
    memcpy(proof_wire + off, commit_root, 32); off += 32;
    memcpy(proof_wire + off, commit.tree_hash, 32); off += 32;
    memcpy(proof_wire + off, source_identity, 32); off += 32;
    memcpy(proof_wire + off, source_cas, 32); off += 32;
    memcpy(proof_wire + off, commit.generation_sha256, 32); off += 32;
    zcl_write_u64_le(proof_wire + off,
                     verdict->elapsed_ms < 0 ? 0 :
                     (uint64_t)verdict->elapsed_ms); off += 8;
    publication_fixed((char *)proof_wire + off, 24, verdict->phase); off += 24;
    publication_fixed((char *)proof_wire + off, 64,
                      verdict->proof_scope); off += 64;
    if (off != sizeof(proof_wire) ||
        !vcs_object_put(repo_root, proof_wire, sizeof(proof_wire),
                        VCS_TAG_DEV_PROOF, out->proof_receipt_root)) {
        (void)snprintf(out->publication_error,
                       sizeof(out->publication_error), "%s",
                       "the immutable proof receipt could not be stored");
        LOG_WARN("vcs.devloop", "publication enqueue: proof store failed");
        return false;
    }

    struct vcs_devloop_publication_job job = {
        .version = VCS_DEVLOOP_PUBLICATION_JOB_VERSION,
    };
    memcpy(job.vcs_commit_root, commit_root, 32);
    memcpy(job.source_tree_root, commit.tree_hash, 32);
    memcpy(job.proof_receipt_root, out->proof_receipt_root, 32);
    memcpy(job.source_identity_sha256, source_identity, 32);
    memcpy(job.source_cas_sha3, source_cas, 32);
    memcpy(job.generation_sha256, commit.generation_sha256, 32);
    uint8_t job_wire[VCS_DEV_PUBLICATION_JOB_WIRE_BYTES];
    if (!publication_job_serialize(&job, job_wire) ||
        !vcs_object_put(repo_root, job_wire, sizeof(job_wire),
                        VCS_TAG_PUBLICATION_JOB,
                        out->publication_job_root) ||
        !vcs_devloop_publication_job_requeue(
            repo_root, out->publication_job_root,
            &out->publication_reused)) {
        (void)snprintf(out->publication_error,
                       sizeof(out->publication_error), "%s",
                       "the immutable publication job could not be durably queued");
        LOG_WARN("vcs.devloop", "publication enqueue: job queue failed");
        return false;
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
        if (v->proof_complete) {
            int64_t enqueue_started = platform_time_monotonic_us();
            bool queued = publication_enqueue_from_commit(
                repo_root, v, commit_id, out);
            out->publication_enqueue_us =
                platform_time_monotonic_us() - enqueue_started;
            out->publication_status = queued
                ? VCS_DEVLOOP_PUBLICATION_QUEUED
                : VCS_DEVLOOP_PUBLICATION_ERROR;
        }
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
