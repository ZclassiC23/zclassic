/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native transactional dev-lane activation engine — the C port of the proven
 * shell transaction in tools/dev/deploy-dev-lane.sh. See dev_activation.h.
 *
 * The whole engine is compiled only under a dev OR test build so the release
 * binary carries none of these symbols (proven by
 * tools/lint/check_release_no_dev_symbols.sh). Process exec (the real
 * systemctl/proc ops) is further confined to ZCL_DEV_BUILD.
 */

#define _GNU_SOURCE

#include "dev_activation.h"
#include "dev_activation_internal.h"

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)

#include "crypto/sha256.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── small utilities ─────────────────────────────────────────────────── */

bool dev_activation_join(char *out, size_t out_sz, const char *a, const char *b)
{
    int n = snprintf(out, out_sz, "%s/%s", a, b);
    if (n <= 0 || (size_t)n >= out_sz)
        LOG_FAIL("dev-activation", "path join overflow: %s/%s", a, b);
    return true;
}

bool dev_activation_mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        LOG_FAIL("dev-activation", "mkdir_p bad path length");
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            LOG_FAIL("dev-activation", "mkdir %s: %s", tmp, strerror(errno));
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        LOG_FAIL("dev-activation", "mkdir %s: %s", tmp, strerror(errno));
    return true;
}

void dev_activation_iso_utc_now(char out[32])
{
    time_t t = platform_time_wall_time_t();
    struct tm tmv;
    if (gmtime_r(&t, &tmv))
        strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    else
        snprintf(out, 32, "1970-01-01T00:00:00Z");
}

/* Escape a string for embedding in a JSON double-quoted value. */
void dev_activation_json_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    if (out_sz == 0)
        return;
    for (const char *p = in ? in : ""; *p && o + 2 < out_sz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (o + 2 >= out_sz)
                break;
            out[o++] = '\\';
            out[o++] = (c == '\n') ? 'n' : (c == '\r') ? 'r' : 't';
        } else if (c < 0x20) {
            /* drop other control bytes */
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

void dev_activation_hex32(const uint8_t in[32], char out[65])
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[2 * i] = d[(in[i] >> 4) & 0xf];
        out[2 * i + 1] = d[in[i] & 0xf];
    }
    out[64] = 0;
}

bool dev_activation_sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        LOG_FAIL("dev-activation", "sha256 open %s: %s", path, strerror(errno));
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_write(&ctx, buf, n);
    bool io_err = ferror(f) != 0;
    fclose(f);
    if (io_err)
        LOG_FAIL("dev-activation", "sha256 read %s failed", path);
    unsigned char digest[32];
    sha256_finalize(&ctx, digest);
    dev_activation_hex32(digest, out);
    return true;
}

bool dev_activation_canon(const char *in, char *out, size_t out_sz)
{
    if (!in || in[0] != '/')
        LOG_FAIL("dev-activation", "canon requires an absolute path");
    char *stack[256];
    size_t depth = 0;
    char work[PATH_MAX];
    size_t len = strlen(in);
    if (len >= sizeof(work))
        LOG_FAIL("dev-activation", "canon path too long");
    memcpy(work, in, len + 1);
    for (char *tok = strtok(work, "/"); tok; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (depth > 0)
                depth--;
            continue;
        }
        if (depth >= 256)
            LOG_FAIL("dev-activation", "canon depth overflow");
        stack[depth++] = tok;
    }
    size_t o = 0;
    if (depth == 0) {
        if (out_sz < 2)
            LOG_FAIL("dev-activation", "canon out overflow");
        out[0] = '/';
        out[1] = 0;
        return true;
    }
    for (size_t i = 0; i < depth; i++) {
        int n = snprintf(out + o, out_sz - o, "/%s", stack[i]);
        if (n <= 0 || (size_t)n >= out_sz - o)
            LOG_FAIL("dev-activation", "canon out overflow");
        o += (size_t)n;
    }
    return true;
}

/* ── generation link helpers ─────────────────────────────────────────── */

static bool dev_valid_generation_id(const char *g)
{
    size_t n = strlen(g);
    const char *hex;
    if (strncmp(g, "gen-", 4) == 0)
        hex = g + 4;
    else if (strncmp(g, "legacy-", 7) == 0)
        hex = g + 7;
    else
        return false;
    if (strchr(g, '/'))
        return false;
    if (n == 0)
        return false;
    for (const char *p = hex; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return false;
    return hex[0] != 0;
}

bool dev_activation_read_gen_link(const struct dev_activation_txn *txn,
                                  const char *link, char *out, size_t out_sz)
{
    char target[PATH_MAX];
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0)
        return false;
    target[n] = 0;
    if (!dev_valid_generation_id(target))
        return false;
    char full[PATH_MAX];
    int m = snprintf(full, sizeof(full), "%s/%s/zclassic23-dev",
                     txn->gen_root, target);
    if (m <= 0 || (size_t)m >= sizeof(full))
        return false;
    if (access(full, X_OK) != 0)
        return false;
    n = snprintf(out, out_sz, "%s", target);
    if (n <= 0 || (size_t)n >= out_sz)
        return false;
    return true;
}

void dev_activation_refresh_gen_state(struct dev_activation_txn *txn)
{
    if (!dev_activation_read_gen_link(txn, txn->current_link,
                                      txn->current_generation,
                                      sizeof(txn->current_generation)))
        txn->current_generation[0] = 0;
    if (!dev_activation_read_gen_link(txn, txn->last_good_link,
                                      txn->last_good_generation,
                                      sizeof(txn->last_good_generation)))
        txn->last_good_generation[0] = 0;
}

bool dev_activation_link_generation(const struct dev_activation_txn *txn,
                                    const char *name, const char *generation)
{
    if (!dev_valid_generation_id(generation))
        LOG_FAIL("dev-activation", "invalid generation id: %s", generation);
    char bin[PATH_MAX];
    int m = snprintf(bin, sizeof(bin), "%s/%s/zclassic23-dev",
                     txn->gen_root, generation);
    if (m <= 0 || (size_t)m >= sizeof(bin))
        LOG_FAIL("dev-activation", "generation bin path overflow");
    if (access(bin, X_OK) != 0)
        LOG_FAIL("dev-activation", "generation binary not executable: %s", bin);
    char link[PATH_MAX], tmp[PATH_MAX];
    if (!dev_activation_join(link, sizeof(link), txn->gen_root, name))
        return false;
    int n = snprintf(tmp, sizeof(tmp), "%s/.%s.%ld",
                     txn->gen_root, name, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        LOG_FAIL("dev-activation", "link tmp path overflow");
    (void)unlink(tmp);
    if (symlink(generation, tmp) != 0)
        LOG_FAIL("dev-activation", "symlink %s: %s", tmp, strerror(errno));
    if (rename(tmp, link) != 0) {
        (void)unlink(tmp);
        LOG_FAIL("dev-activation", "rename %s -> %s: %s", tmp, link,
                 strerror(errno));
    }
    return true;
}

bool dev_activation_refresh_compat_link(const struct dev_activation_txn *txn)
{
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s", txn->compat_bin);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        LOG_FAIL("dev-activation", "compat path overflow");
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (!dev_activation_mkdir_p(dir))
            return false;
    }
    char target[PATH_MAX], tmp[PATH_MAX];
    n = snprintf(target, sizeof(target), "%s/zclassic23-dev", txn->current_link);
    if (n <= 0 || (size_t)n >= sizeof(target))
        LOG_FAIL("dev-activation", "compat target overflow");
    n = snprintf(tmp, sizeof(tmp), "%s.next.%ld", txn->compat_bin,
                 (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        LOG_FAIL("dev-activation", "compat tmp overflow");
    (void)unlink(tmp);
    if (symlink(target, tmp) != 0)
        LOG_FAIL("dev-activation", "compat symlink: %s", strerror(errno));
    if (rename(tmp, txn->compat_bin) != 0) {
        (void)unlink(tmp);
        LOG_FAIL("dev-activation", "compat rename: %s", strerror(errno));
    }
    return true;
}

/* Extract the first "key":"value" string field from a JSON blob into out. */
bool dev_activation_json_first_string(const char *blob, const char *key,
                                  char *out, size_t out_sz)
{
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle))
        return false;
    const char *p = strstr(blob, needle);
    if (!p)
        return false;
    p = strchr(p + strlen(needle), ':');
    if (!p)
        return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"')
        return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end)
        return false;
    size_t vlen = (size_t)(end - p);
    if (vlen >= out_sz)
        return false;
    memcpy(out, p, vlen);
    out[vlen] = 0;
    return true;
}

void dev_activation_generation_commit(const struct dev_activation_txn *txn,
                                      const char *generation,
                                      char *out, size_t out_sz)
{
    out[0] = 0;
    char manifest[PATH_MAX];
    int n = snprintf(manifest, sizeof(manifest), "%s/%s/manifest.json",
                     txn->gen_root, generation);
    if (n <= 0 || (size_t)n >= sizeof(manifest))
        return;
    FILE *f = fopen(manifest, "rb");
    if (!f)
        return;
    char buf[4096];
    size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rn] = 0;
    (void)dev_activation_json_first_string(buf, "build_commit", out, out_sz);
}

void dev_activation_write_build_identity(const struct dev_activation_txn *txn,
                                         const char *generation)
{
    char commit[128];
    dev_activation_generation_commit(txn, generation, commit, sizeof(commit));
    if (commit[0] == 0)
        snprintf(commit, sizeof(commit), "unknown");
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", txn->build_id_dropin);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (!dev_activation_mkdir_p(dir))
            return;
    }
    FILE *f = fopen(txn->build_id_dropin, "w");
    if (!f) {
        fprintf(stderr, "[dev-activation] build-identity drop-in open: %s\n",
                strerror(errno));
        return;
    }
    char esc[256];
    dev_activation_json_escape(commit, esc, sizeof(esc));
    fprintf(f, "[Service]\n");
    fprintf(f, "Environment=\"ZCL_AGENT_EXPECT_BUILD_COMMIT=%s\"\n", esc);
    fprintf(f, "Environment=\"ZCL_AGENT_EXPECT_BUILD_SOURCE=deploy-dev\"\n");
    fclose(f);
}

/* ── confinement + path derivation ───────────────────────────────────── */

static void dev_set_status(struct dev_activation_result *r, const char *act,
                           const char *verify, const char *detail)
{
    if (act)
        snprintf(r->activation_status, sizeof(r->activation_status), "%s", act);
    if (verify)
        snprintf(r->verify_status, sizeof(r->verify_status), "%s", verify);
    if (detail)
        snprintf(r->verify_detail, sizeof(r->verify_detail), "%s", detail);
}

/* True iff canon(path) is at or under canon(root). */
static bool dev_path_under(const char *path, const char *root)
{
    char cp[PATH_MAX], cr[PATH_MAX];
    if (!dev_activation_canon(path, cp, sizeof(cp)) ||
        !dev_activation_canon(root, cr, sizeof(cr)))
        return false;
    size_t rl = strlen(cr);
    if (strncmp(cp, cr, rl) != 0)
        return false;
    return cp[rl] == 0 || cp[rl] == '/';
}

static bool dev_paths_equal(const char *a, const char *b)
{
    char ca[PATH_MAX], cb[PATH_MAX];
    if (!dev_activation_canon(a, ca, sizeof(ca)) ||
        !dev_activation_canon(b, cb, sizeof(cb)))
        return false;
    return strcmp(ca, cb) == 0;
}

/* TOP SEVERITY: refuse to touch anything but the isolated dev lane. */
static bool dev_validate_confinement(struct dev_activation_txn *txn)
{
    const struct dev_activation_request *req = txn->req;
    if (!req->gen_root || req->gen_root[0] != '/')
        LOG_FAIL("dev-activation",
                 "FATAL: generation root must be absolute: %s",
                 req->gen_root ? req->gen_root : "(null)");
    if (strstr(req->gen_root, "/../") || strstr(req->gen_root, "\n") ||
        (strlen(req->gen_root) >= 3 &&
         strcmp(req->gen_root + strlen(req->gen_root) - 3, "/..") == 0))
        LOG_FAIL("dev-activation", "FATAL: unsafe generation root: %s",
                 req->gen_root);

    char dev_dd[PATH_MAX], canonical[PATH_MAX], soak[PATH_MAX], legacy[PATH_MAX];
    if (!dev_activation_join(dev_dd, sizeof(dev_dd), txn->home, ".zclassic-c23-dev") ||
        !dev_activation_join(canonical, sizeof(canonical), txn->home, ".zclassic-c23") ||
        !dev_activation_join(soak, sizeof(soak), txn->home, ".zclassic-c23-soak") ||
        !dev_activation_join(legacy, sizeof(legacy), txn->home, ".zclassic"))
        return false;

    if (dev_path_under(req->gen_root, canonical) ||
        dev_path_under(req->gen_root, soak) ||
        dev_path_under(req->gen_root, legacy))
        LOG_FAIL("dev-activation",
                 "FATAL: generation root enters a canonical/soak/legacy datadir: %s",
                 req->gen_root);
    if (!req->unit || strcmp(req->unit, "zcl23-dev.service") != 0)
        LOG_FAIL("dev-activation", "FATAL: refusing non-dev unit: %s",
                 req->unit ? req->unit : "(null)");
    if (!req->datadir || !dev_paths_equal(req->datadir, dev_dd))
        LOG_FAIL("dev-activation", "FATAL: refusing non-dev datadir: %s",
                 req->datadir ? req->datadir : "(null)");
    if (req->rpcport != 18252)
        LOG_FAIL("dev-activation", "FATAL: refusing non-dev RPC port: %d",
                 req->rpcport);
    return true;
}

static bool dev_derive_paths(struct dev_activation_txn *txn)
{
    const struct dev_activation_request *req = txn->req;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        LOG_FAIL("dev-activation", "HOME is unset");
    int n = snprintf(txn->home, sizeof(txn->home), "%s", home);
    if (n <= 0 || (size_t)n >= sizeof(txn->home))
        LOG_FAIL("dev-activation", "HOME too long");
    n = snprintf(txn->gen_root, sizeof(txn->gen_root), "%s", req->gen_root);
    if (n <= 0 || (size_t)n >= sizeof(txn->gen_root))
        LOG_FAIL("dev-activation", "gen_root too long");
    if (!dev_activation_join(txn->current_link, sizeof(txn->current_link),
                  txn->gen_root, "current") ||
        !dev_activation_join(txn->last_good_link, sizeof(txn->last_good_link),
                  txn->gen_root, "last-good") ||
        !dev_activation_join(txn->staged_link, sizeof(txn->staged_link),
                  txn->gen_root, "staged") ||
        !dev_activation_join(txn->rejected_dir, sizeof(txn->rejected_dir),
                  txn->gen_root, "rejected") ||
        !dev_activation_join(txn->lock_path, sizeof(txn->lock_path),
                  txn->gen_root, "activation.lock"))
        return false;
    n = snprintf(txn->compat_bin, sizeof(txn->compat_bin),
                 "%s/.local/bin/zclassic23-dev", txn->home);
    if (n <= 0 || (size_t)n >= sizeof(txn->compat_bin))
        LOG_FAIL("dev-activation", "compat path too long");
    n = snprintf(txn->build_id_dropin, sizeof(txn->build_id_dropin),
                 "%s/.config/systemd/user/zcl23-dev.service.d/90-build-identity.conf",
                 txn->home);
    if (n <= 0 || (size_t)n >= sizeof(txn->build_id_dropin))
        LOG_FAIL("dev-activation", "drop-in path too long");
    n = snprintf(txn->deploy_state, sizeof(txn->deploy_state),
                 "%s/agent-deploy.json", req->datadir);
    if (n <= 0 || (size_t)n >= sizeof(txn->deploy_state))
        LOG_FAIL("dev-activation", "deploy-state path too long");
    return true;
}

/* ── activation lock ─────────────────────────────────────────────────── */

static int dev_acquire_lock(struct dev_activation_txn *txn)
{
    if (!dev_activation_mkdir_p(txn->gen_root) || !dev_activation_mkdir_p(txn->rejected_dir))
        return DEV_ACTIVATION_E_INTERNAL;
    int fd = open(txn->lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG_ERR("dev-activation", "lock open %s: %s", txn->lock_path,
                strerror(errno));
        return DEV_ACTIVATION_E_INTERNAL;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        close(fd);
        if (e == EWOULDBLOCK) {
            fprintf(stderr,
                    "[dev-activation] BUSY: another activation owns %s\n",
                    txn->lock_path);
            return DEV_ACTIVATION_E_LOCK_BUSY;
        }
        LOG_ERR("dev-activation", "flock %s: %s", txn->lock_path, strerror(e));
        return DEV_ACTIVATION_E_INTERNAL;
    }
    txn->lock_fd = fd;
    txn->lock_held = true;
    if (ftruncate(fd, 0) != 0) { /* best-effort payload */ }
    char now[32];
    dev_activation_iso_utc_now(now);
    char line[256];
    int n = snprintf(line, sizeof(line),
                     "{\"schema\":\"zcl.dev_activation_lock.v1\",\"pid\":%ld,"
                     "\"acquired_at_utc\":\"%s\"}\n",
                     (long)getpid(), now);
    if (n > 0)
        (void)!write(fd, line, (size_t)n);
    return DEV_ACTIVATION_OK;
}

static void dev_release_lock(struct dev_activation_txn *txn)
{
    if (txn->lock_held && txn->lock_fd >= 0) {
        flock(txn->lock_fd, LOCK_UN);
        close(txn->lock_fd);
    }
    txn->lock_fd = -1;
    txn->lock_held = false;
}

/* ── the activation transaction ──────────────────────────────────────── */

static void dev_mark_failed(struct dev_activation_result *r)
{
    if (strcmp(r->rollback_status, "verified") == 0)
        snprintf(r->activation_status, sizeof(r->activation_status),
                 "rolled_back");
    else
        snprintf(r->activation_status, sizeof(r->activation_status), "failed");
}

/* Perform the stop/flip/start/verify transaction for the already-staged
 * txn->candidate_generation. Assumes lock held, preflight passed. */
static int dev_activate_candidate(struct dev_activation_txn *txn)
{
    struct dev_activation_result *r = txn->result;
    const struct dev_activation_ops *ops = txn->ops;
    char reason[256];

    dev_activation_refresh_gen_state(txn);
    snprintf(txn->previous_generation, sizeof(txn->previous_generation), "%s",
             txn->current_generation);
    if (txn->previous_generation[0] != 0)
        (void)dev_activation_link_generation(txn, "last-good",
                                             txn->previous_generation);

    if (ops->service_stop(ops->ctx) != 0) {
        dev_set_status(r, "stop_failed", "activation_failed",
                       "could not stop the unit within the bounded stop window");
        snprintf(r->failure_capsule, sizeof(r->failure_capsule),
                 "candidate stop failed");
        (void)ops->service_start(ops->ctx);
        return DEV_ACTIVATION_E_ACTIVATE;
    }

    if (!dev_activation_link_generation(txn, "current",
                                        txn->candidate_generation)) {
        dev_set_status(r, "failed", "activation_failed",
                       "could not flip the current generation symlink");
        return DEV_ACTIVATION_E_ACTIVATE;
    }
    (void)dev_activation_refresh_compat_link(txn);
    dev_activation_write_build_identity(txn, txn->candidate_generation);
    txn->activation_in_progress = true;

    if (ops->service_daemon_reload(ops->ctx) != 0) {
        snprintf(reason, sizeof(reason), "daemon-reload failed mid-activation");
        dev_activation_quarantine(txn, reason);
        (void)dev_activation_rollback_previous(txn, reason);
        dev_mark_failed(r);
        dev_set_status(r, NULL, "activation_failed", reason);
        return DEV_ACTIVATION_E_ACTIVATE;
    }

    if (ops->service_start(ops->ctx) != 0) {
        snprintf(reason, sizeof(reason), "candidate service start failed");
        dev_activation_quarantine(txn, reason);
        (void)dev_activation_rollback_previous(txn, reason);
        dev_mark_failed(r);
        dev_set_status(r, NULL, "activation_failed", reason);
        return DEV_ACTIVATION_E_ACTIVATE;
    }

    if (!dev_activation_verify_running(txn, txn->candidate_generation)) {
        snprintf(reason, sizeof(reason),
                 "candidate failed bounded readiness or /proc identity");
        dev_activation_quarantine(txn, reason);
        (void)dev_activation_rollback_previous(txn, reason);
        dev_mark_failed(r);
        dev_set_status(r, NULL, "activation_failed", reason);
        return DEV_ACTIVATION_E_ACTIVATE;
    }

    (void)dev_activation_link_generation(txn, "last-good",
                                         txn->candidate_generation);
    (void)unlink(txn->staged_link);
    txn->activation_in_progress = false;
    dev_set_status(r, "active", "ready",
                   "exact candidate generation is active and verified");
    snprintf(r->rollback_status, sizeof(r->rollback_status), "not_needed");
    return DEV_ACTIVATION_OK;
}

/* Shared body: with the lock held and paths derived, run stage/preflight/
 * activate. `already_staged` skips staging (activate_generation revert hook). */
static int dev_run_locked(struct dev_activation_txn *txn, bool already_staged)
{
    struct dev_activation_result *r = txn->result;
    const struct dev_activation_ops *ops = txn->ops;

    if (!already_staged) {
        int st = dev_activation_stage_candidate(txn);
        if (st != DEV_ACTIVATION_OK) {
            dev_set_status(r, "stage_failed", "stage_failed",
                           "candidate staging failed");
            snprintf(r->failure_capsule, sizeof(r->failure_capsule),
                     "candidate staging failed");
            (void)dev_activation_write_deploy_state(txn);
            return st;
        }
    }
    dev_activation_ensure_rollback(txn);

    dev_set_status(r, "preflighting", "preflighting",
                   "candidate staged; running process untouched");
    (void)dev_activation_write_deploy_state(txn);

    char commit[128];
    dev_activation_generation_commit(txn, txn->candidate_generation, commit,
                                     sizeof(commit));
    if (commit[0] == 0)
        snprintf(commit, sizeof(commit), "%s", txn->req->build_commit);
    if (ops->preflight(ops->ctx, txn->candidate_bin, commit) != 0) {
        dev_set_status(r, "preflight_failed", "preflight_failed",
                       "candidate preflight failed");
        snprintf(r->failure_capsule, sizeof(r->failure_capsule),
                 "candidate preflight failed");
        dev_activation_quarantine(txn, "candidate preflight failed");
        (void)dev_activation_write_deploy_state(txn);
        fprintf(stderr, "[dev-activation] REJECTED: preflight failed; "
                        "running process and current generation untouched\n");
        return DEV_ACTIVATION_E_PREFLIGHT;
    }

    if (txn->req->mode == DEV_ACTIVATION_MODE_STAGE_ONLY) {
        (void)dev_activation_link_generation(txn, "staged",
                                             txn->candidate_generation);
        dev_set_status(r, "staged", "staged",
                       "candidate preflight passed; no service stop/restart");
        (void)dev_activation_write_deploy_state(txn);
        return DEV_ACTIVATION_OK;
    }

    int st = dev_activate_candidate(txn);
    (void)dev_activation_write_deploy_state(txn);
    return st;
}

/* ── public entry points ─────────────────────────────────────────────── */

static void dev_result_init(struct dev_activation_result *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->activation_status, sizeof(r->activation_status), "preparing");
    snprintf(r->verify_status, sizeof(r->verify_status), "started");
    snprintf(r->rollback_status, sizeof(r->rollback_status), "not_needed");
}

static int dev_prepare(struct dev_activation_txn *txn,
                       const struct dev_activation_request *req,
                       const struct dev_activation_ops *ops,
                       struct dev_activation_result *result,
                       struct dev_activation_ops *default_slot)
{
    memset(txn, 0, sizeof(*txn));
    txn->lock_fd = -1;
    txn->req = req;
    txn->result = result;
    dev_result_init(result);
    if (!req || !result)
        return DEV_ACTIVATION_E_INTERNAL;
    if (!ops) {
#ifdef ZCL_DEV_BUILD
        dev_activation_default_ops(req, default_slot);
        txn->ops = default_slot;
#else
        /* The real (process-exec) default ops live in a ZCL_DEV_BUILD-only TU;
         * a test build must always supply an ops vtable. */
        (void)default_slot;
        return DEV_ACTIVATION_E_INTERNAL;
#endif
    } else {
        txn->ops = ops;
    }
    if (!dev_derive_paths(txn))
        return DEV_ACTIVATION_E_INTERNAL;
    if (!dev_validate_confinement(txn)) {
        dev_set_status(result, "refused", "refused",
                       "confinement check failed: not the isolated dev lane");
        snprintf(result->failure_capsule, sizeof(result->failure_capsule),
                 "confinement refusal");
        return DEV_ACTIVATION_E_CONFINEMENT;
    }
    if (!dev_activation_mkdir_p(req->datadir) || !dev_activation_mkdir_p(txn->gen_root) ||
        !dev_activation_mkdir_p(txn->rejected_dir))
        return DEV_ACTIVATION_E_INTERNAL;
    return DEV_ACTIVATION_OK;
}

int dev_activation_run(const struct dev_activation_request *req,
                       const struct dev_activation_ops *ops,
                       struct dev_activation_result *result)
{
    struct dev_activation_txn txn;
    struct dev_activation_ops default_ops;
    int st = dev_prepare(&txn, req, ops, result, &default_ops);
    if (st != DEV_ACTIVATION_OK) {
        result->status = st;
        return st;
    }
    st = dev_acquire_lock(&txn);
    if (st != DEV_ACTIVATION_OK) {
        dev_set_status(result, "lock_busy", "lock_busy",
                       "another activation holds the lock");
        result->status = st;
        return st;
    }
    st = dev_run_locked(&txn, false);
    dev_release_lock(&txn);
    result->status = st;
    return st;
}

int dev_activation_activate_generation(const uint8_t gen_sha256[32],
                                       const struct dev_activation_request *req,
                                       const struct dev_activation_ops *ops,
                                       struct dev_activation_result *result)
{
    struct dev_activation_txn txn;
    struct dev_activation_ops default_ops;
    int st = dev_prepare(&txn, req, ops, result, &default_ops);
    if (st != DEV_ACTIVATION_OK) {
        result->status = st;
        return st;
    }
    /* Resolve the already-staged generation by sha — no build, no restage. */
    dev_activation_hex32(gen_sha256, txn.candidate_sha_hex);
    snprintf(result->candidate_sha256, sizeof(result->candidate_sha256), "%s",
             txn.candidate_sha_hex);
    snprintf(txn.candidate_generation, sizeof(txn.candidate_generation),
             "gen-%s", txn.candidate_sha_hex);
    snprintf(result->candidate_generation, sizeof(result->candidate_generation),
             "%s", txn.candidate_generation);
    dev_activation_join(txn.candidate_dir, sizeof(txn.candidate_dir), txn.gen_root,
             txn.candidate_generation);
    snprintf(txn.candidate_bin, sizeof(txn.candidate_bin),
             "%s/zclassic23-dev", txn.candidate_dir);

    st = dev_acquire_lock(&txn);
    if (st != DEV_ACTIVATION_OK) {
        dev_set_status(result, "lock_busy", "lock_busy",
                       "another activation holds the lock");
        result->status = st;
        return st;
    }
    char have[65];
    if (access(txn.candidate_bin, X_OK) != 0 ||
        !dev_activation_sha256_file(txn.candidate_bin, have) ||
        strcmp(have, txn.candidate_sha_hex) != 0) {
        dev_release_lock(&txn);
        dev_set_status(result, "stage_failed", "stage_failed",
                       "requested generation is not staged");
        snprintf(result->failure_capsule, sizeof(result->failure_capsule),
                 "generation %s not staged", txn.candidate_generation);
        (void)dev_activation_write_deploy_state(&txn);
        result->status = DEV_ACTIVATION_E_STAGE;
        return DEV_ACTIVATION_E_STAGE;
    }
    st = dev_run_locked(&txn, true);
    dev_release_lock(&txn);
    result->status = st;
    return st;
}

#endif /* ZCL_DEV_BUILD || ZCL_TESTING */
