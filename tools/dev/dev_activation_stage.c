/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * dev_activation_stage.c — candidate staging (immutable content-addressed
 * generations, rollback-generation import) and the byte-compatible
 * zcl.agent_dev_deploy.v1 deploy-state writer for the native dev-lane
 * activation engine. Split from dev_activation.c along the staging /
 * deploy-state seam (file-size ceiling). No process exec.
 */

#define _GNU_SOURCE

#include "dev_activation.h"
#include "dev_activation_internal.h"

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)

#include "storage/boot_auto_reindex.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── staging: immutable content-addressed generation ─────────────────── */

static bool dev_write_manifest(const char *path, const char *generation,
                               const char *sha, const char *build_commit,
                               const char *build_type, const char *source)
{
    FILE *f = fopen(path, "w");
    if (!f)
        LOG_FAIL("dev-activation", "manifest open %s: %s", path,
                 strerror(errno));
    char e_commit[256], e_type[64], e_src[PATH_MAX];
    dev_activation_json_escape(build_commit, e_commit, sizeof(e_commit));
    dev_activation_json_escape(build_type, e_type, sizeof(e_type));
    dev_activation_json_escape(source, e_src, sizeof(e_src));
    char now[32];
    dev_activation_iso_utc_now(now);
    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": \"zcl.dev_binary_generation.v1\",\n");
    fprintf(f, "  \"generation\": \"%s\",\n", generation);
    fprintf(f, "  \"sha256\": \"%s\",\n", sha);
    fprintf(f, "  \"build_commit\": \"%s\",\n", e_commit);
    fprintf(f, "  \"build_type\": \"%s\",\n", e_type);
    fprintf(f, "  \"source_artifact\": \"%s\",\n", e_src);
    fprintf(f, "  \"created_at_utc\": \"%s\"\n", now);
    fprintf(f, "}\n");
    if (fclose(f) != 0)
        LOG_FAIL("dev-activation", "manifest fclose: %s", strerror(errno));
    return true;
}

/* Copy `src` into `dst` with mode `mode`. */
bool dev_activation_install_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        LOG_FAIL("dev-activation", "install open %s: %s", src, strerror(errno));
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        close(in);
        LOG_FAIL("dev-activation", "install create %s: %s", dst,
                 strerror(errno));
    }
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok)
            break;
    }
    if (n < 0)
        ok = false;
    close(in);
    if (fchmod(out, mode) != 0)
        ok = false;
    if (close(out) != 0)
        ok = false;
    if (!ok)
        LOG_FAIL("dev-activation", "install copy %s -> %s failed", src, dst);
    return true;
}

/* Stage the candidate: sha the artifact, build gen-<sha> immutably. Sets
 * candidate_* fields. Idempotent on an existing matching generation; refuses a
 * quarantined generation. */
int dev_activation_stage_candidate(struct dev_activation_txn *txn)
{
    struct dev_activation_result *r = txn->result;
    if (access(txn->req->artifact_path, X_OK) != 0) {
        fprintf(stderr, "[dev-activation] artifact missing/not executable: %s\n",
                txn->req->artifact_path);
        return DEV_ACTIVATION_E_STAGE;
    }
    if (!dev_activation_sha256_file(txn->req->artifact_path,
                                    txn->candidate_sha_hex))
        return DEV_ACTIVATION_E_STAGE;
    snprintf(r->candidate_sha256, sizeof(r->candidate_sha256), "%s",
             txn->candidate_sha_hex);
    int n = snprintf(txn->candidate_generation,
                     sizeof(txn->candidate_generation), "gen-%s",
                     txn->candidate_sha_hex);
    if (n <= 0 || (size_t)n >= sizeof(txn->candidate_generation))
        return DEV_ACTIVATION_E_STAGE;
    snprintf(r->candidate_generation, sizeof(r->candidate_generation), "%s",
             txn->candidate_generation);
    if (!dev_activation_join(txn->candidate_dir, sizeof(txn->candidate_dir),
                  txn->gen_root, txn->candidate_generation))
        return DEV_ACTIVATION_E_STAGE;
    n = snprintf(txn->candidate_bin, sizeof(txn->candidate_bin),
                 "%s/zclassic23-dev", txn->candidate_dir);
    if (n <= 0 || (size_t)n >= sizeof(txn->candidate_bin))
        return DEV_ACTIVATION_E_STAGE;

    char reject_marker[PATH_MAX];
    n = snprintf(reject_marker, sizeof(reject_marker), "%s/%s.json",
                 txn->rejected_dir, txn->candidate_generation);
    if (n > 0 && (size_t)n < sizeof(reject_marker) &&
        access(reject_marker, F_OK) == 0) {
        fprintf(stderr, "[dev-activation] candidate %s is quarantined\n",
                txn->candidate_generation);
        return DEV_ACTIVATION_E_STAGE;
    }

    struct stat st;
    if (stat(txn->candidate_dir, &st) == 0) {
        /* Immutable collision: an existing gen dir must carry the same sha. */
        char have[65];
        if (access(txn->candidate_bin, X_OK) != 0 ||
            !dev_activation_sha256_file(txn->candidate_bin, have) ||
            strcmp(have, txn->candidate_sha_hex) != 0) {
            fprintf(stderr, "[dev-activation] immutable generation collision: %s\n",
                    txn->candidate_dir);
            return DEV_ACTIVATION_E_STAGE;
        }
        return DEV_ACTIVATION_OK; /* already staged, byte-identical */
    }

    char tmpl[PATH_MAX];
    n = snprintf(tmpl, sizeof(tmpl), "%s/.candidate.XXXXXX", txn->gen_root);
    if (n <= 0 || (size_t)n >= sizeof(tmpl))
        return DEV_ACTIVATION_E_STAGE;
    char *tmpdir = mkdtemp(tmpl);
    if (!tmpdir) {
        LOG_ERR("dev-activation", "candidate mkdtemp: %s", strerror(errno));
        return DEV_ACTIVATION_E_STAGE;
    }
    char tmp_bin[PATH_MAX], tmp_manifest[PATH_MAX];
    snprintf(tmp_bin, sizeof(tmp_bin), "%s/zclassic23-dev", tmpdir);
    snprintf(tmp_manifest, sizeof(tmp_manifest), "%s/manifest.json", tmpdir);
    if (!dev_activation_install_file(txn->req->artifact_path, tmp_bin, 0555) ||
        !dev_write_manifest(tmp_manifest, txn->candidate_generation,
                            txn->candidate_sha_hex, txn->req->build_commit,
                            txn->req->build_type, txn->req->artifact_path)) {
        (void)unlink(tmp_bin);
        (void)unlink(tmp_manifest);
        (void)rmdir(tmpdir);
        return DEV_ACTIVATION_E_STAGE;
    }
    (void)chmod(tmp_manifest, 0444);
    (void)chmod(tmpdir, 0555);
    if (rename(tmpdir, txn->candidate_dir) != 0) {
        (void)chmod(tmpdir, 0755);
        (void)unlink(tmp_bin);
        (void)unlink(tmp_manifest);
        (void)rmdir(tmpdir);
        if (access(txn->candidate_bin, X_OK) != 0) {
            LOG_ERR("dev-activation", "candidate rename lost the race: %s",
                    strerror(errno));
            return DEV_ACTIVATION_E_STAGE;
        }
    }
    return DEV_ACTIVATION_OK;
}

/* Import a pre-existing plain binary as a legacy-<sha> rollback generation and
 * point current+last-good at it. */
static bool dev_import_existing(struct dev_activation_txn *txn,
                                const char *existing)
{
    if (access(existing, X_OK) != 0)
        return false;
    char sha[65];
    if (!dev_activation_sha256_file(existing, sha))
        return false;
    char generation[DEV_GEN_ID_MAX];
    int n = snprintf(generation, sizeof(generation), "legacy-%s", sha);
    if (n <= 0 || (size_t)n >= sizeof(generation))
        return false;
    char dir[PATH_MAX];
    if (!dev_activation_join(dir, sizeof(dir), txn->gen_root, generation))
        return false;
    struct stat st;
    if (stat(dir, &st) != 0) {
        char tmpl[PATH_MAX];
        snprintf(tmpl, sizeof(tmpl), "%s/.legacy.XXXXXX", txn->gen_root);
        char *tmpdir = mkdtemp(tmpl);
        if (!tmpdir)
            LOG_FAIL("dev-activation", "legacy mkdtemp: %s", strerror(errno));
        char tmp_bin[PATH_MAX], tmp_manifest[PATH_MAX];
        snprintf(tmp_bin, sizeof(tmp_bin), "%s/zclassic23-dev", tmpdir);
        snprintf(tmp_manifest, sizeof(tmp_manifest), "%s/manifest.json", tmpdir);
        if (!dev_activation_install_file(existing, tmp_bin, 0555) ||
            !dev_write_manifest(tmp_manifest, generation, sha, "unknown",
                                "legacy-import", existing)) {
            (void)unlink(tmp_bin);
            (void)unlink(tmp_manifest);
            (void)rmdir(tmpdir);
            return false;
        }
        (void)chmod(tmp_manifest, 0444);
        (void)chmod(tmpdir, 0555);
        if (rename(tmpdir, dir) != 0 && access(tmp_bin, X_OK) != 0) {
            (void)chmod(tmpdir, 0755);
            (void)rmdir(tmpdir);
            LOG_FAIL("dev-activation", "legacy rename: %s", strerror(errno));
        }
    }
    if (!dev_activation_link_generation(txn, "current", generation) ||
        !dev_activation_link_generation(txn, "last-good", generation))
        return false;
    return dev_activation_refresh_compat_link(txn);
}

void dev_activation_ensure_rollback(struct dev_activation_txn *txn)
{
    dev_activation_refresh_gen_state(txn);
    if (txn->current_generation[0] != 0) {
        (void)dev_activation_refresh_compat_link(txn);
        return;
    }
    struct stat st;
    if (lstat(txn->compat_bin, &st) == 0 && !S_ISLNK(st.st_mode) &&
        access(txn->compat_bin, X_OK) == 0)
        (void)dev_import_existing(txn, txn->compat_bin);
    dev_activation_refresh_gen_state(txn);
}


/* ── deploy-state (zcl.agent_dev_deploy.v1) ──────────────────────────── */

static int dev_gen_name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void dev_emit_rejected(const struct dev_activation_txn *txn, FILE *f)
{
    fputc('[', f);
    DIR *d = opendir(txn->rejected_dir);
    if (!d) {
        fputc(']', f);
        return;
    }
    char *names[256];
    size_t count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 256) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || strncmp(e->d_name, "gen-", 4) != 0)
            continue;
        if (strcmp(e->d_name + len - 5, ".json") != 0)
            continue;
        char *nm = zcl_malloc(len - 4, "dev-activation rejected name");
        if (!nm)
            continue;
        memcpy(nm, e->d_name, len - 5);
        nm[len - 5] = 0;
        names[count++] = nm;
    }
    closedir(d);
    qsort(names, count, sizeof(names[0]), dev_gen_name_cmp);
    for (size_t i = 0; i < count; i++) {
        char esc[96];
        dev_activation_json_escape(names[i], esc, sizeof(esc));
        fprintf(f, "%s\"%s\"", i ? "," : "", esc);
        free(names[i]);
    }
    fputc(']', f);
}

bool dev_activation_write_deploy_state(struct dev_activation_txn *txn)
{
    const struct dev_activation_request *req = txn->req;
    struct dev_activation_result *r = txn->result;
    if (!dev_activation_mkdir_p(req->datadir))
        return false;
    dev_activation_refresh_gen_state(txn);
    bool rollback_available = txn->last_good_generation[0] != 0;

    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.XXXXXX", txn->deploy_state);
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        LOG_FAIL("dev-activation", "deploy-state tmp overflow");
    int fd = mkstemp(tmp);
    if (fd < 0)
        LOG_FAIL("dev-activation", "deploy-state mkstemp: %s", strerror(errno));
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        (void)unlink(tmp);
        LOG_FAIL("dev-activation", "deploy-state fdopen: %s", strerror(errno));
    }

    char now[32];
    dev_activation_iso_utc_now(now);
    char e_commit[256], e_type[64], e_artifact[PATH_MAX], e_bin[PATH_MAX];
    char e_root[PATH_MAX], e_cand[96], e_cur[96], e_run[96], e_lg[96], e_prev[96];
    char e_act[64], e_rb[64], e_lock[PATH_MAX], e_ddir[PATH_MAX];
    char e_vstat[64], e_vdetail[512], e_capsule[512];
    dev_activation_json_escape(req->build_commit, e_commit, sizeof(e_commit));
    dev_activation_json_escape(req->build_type, e_type, sizeof(e_type));
    dev_activation_json_escape(req->artifact_path, e_artifact, sizeof(e_artifact));
    dev_activation_json_escape(txn->compat_bin, e_bin, sizeof(e_bin));
    dev_activation_json_escape(txn->gen_root, e_root, sizeof(e_root));
    dev_activation_json_escape(r->candidate_generation, e_cand, sizeof(e_cand));
    dev_activation_json_escape(txn->current_generation, e_cur, sizeof(e_cur));
    dev_activation_json_escape(r->running_generation, e_run, sizeof(e_run));
    dev_activation_json_escape(txn->last_good_generation, e_lg, sizeof(e_lg));
    dev_activation_json_escape(txn->previous_generation, e_prev, sizeof(e_prev));
    dev_activation_json_escape(r->activation_status, e_act, sizeof(e_act));
    dev_activation_json_escape(r->rollback_status, e_rb, sizeof(e_rb));
    dev_activation_json_escape(txn->lock_path, e_lock, sizeof(e_lock));
    dev_activation_json_escape(req->datadir, e_ddir, sizeof(e_ddir));
    dev_activation_json_escape(r->verify_status, e_vstat, sizeof(e_vstat));
    dev_activation_json_escape(r->verify_detail, e_vdetail, sizeof(e_vdetail));
    dev_activation_json_escape(r->failure_capsule, e_capsule, sizeof(e_capsule));

    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": \"zcl.agent_dev_deploy.v1\",\n");
    fprintf(f, "  \"deployed_at_utc\": \"%s\",\n", now);
    fprintf(f, "  \"build_commit\": \"%s\",\n", e_commit);
    fprintf(f, "  \"build_type\": \"%s\",\n", e_type);
    fprintf(f, "  \"build_artifact\": \"%s\",\n", e_artifact);
    fprintf(f, "  \"installed_binary\": \"%s\",\n", e_bin);
    fprintf(f, "  \"generation_root\": \"%s\",\n", e_root);
    fprintf(f, "  \"candidate_generation\": \"%s\",\n", e_cand);
    fprintf(f, "  \"candidate_sha256\": \"%s\",\n", r->candidate_sha256);
    fprintf(f, "  \"current_generation\": \"%s\",\n", e_cur);
    fprintf(f, "  \"running_generation\": \"%s\",\n", e_run);
    fprintf(f, "  \"last_good_generation\": \"%s\",\n", e_lg);
    fprintf(f, "  \"previous_generation\": \"%s\",\n", e_prev);
    fprintf(f, "  \"rollback_available\": %s,\n",
            rollback_available ? "true" : "false");
    fprintf(f, "  \"activation_status\": \"%s\",\n", e_act);
    fprintf(f, "  \"rollback_status\": \"%s\",\n", e_rb);
    fprintf(f, "  \"activation_lock\": \"%s\",\n", e_lock);
    fprintf(f, "  \"activation_lock_held\": %s,\n",
            txn->lock_held ? "true" : "false");
    fprintf(f, "  \"rejected_generations\": ");
    dev_emit_rejected(txn, f);
    fprintf(f, ",\n");
    fprintf(f, "  \"service\": \"%s\",\n", req->unit);
    fprintf(f, "  \"datadir\": \"%s\",\n", e_ddir);
    fprintf(f, "  \"rpcport\": %d,\n", req->rpcport);
    fprintf(f, "  \"verify_status\": \"%s\",\n", e_vstat);
    fprintf(f, "  \"verify_detail\": \"%s\",\n", e_vdetail);
    fprintf(f, "  \"failure_capsule\": \"%s\",\n", e_capsule);
    /* Record the REAL crash-only auto-reindex sentinel state, matching
     * deploy-dev-lane.sh:write_deploy_state (a pending marker is a JSON bool
     * true; a TERMINAL/absent/malformed marker is not pending). The anchor and
     * count are emitted as bare-integer strings when a well-formed marker
     * exists, empty strings otherwise. */
    int32_t ar_anchor = 0;
    int ar_count = 0;
    bool ar_have = boot_auto_reindex_status(req->datadir, &ar_anchor, &ar_count);
    bool ar_pending = ar_have && ar_count > 0;
    fprintf(f, "  \"auto_reindex_pending\": %s,\n",
            ar_pending ? "true" : "false");
    if (ar_have) {
        fprintf(f, "  \"auto_reindex_anchor\": \"%d\",\n", (int)ar_anchor);
        fprintf(f, "  \"auto_reindex_count\": \"%d\"\n", ar_count);
    } else {
        fprintf(f, "  \"auto_reindex_anchor\": \"\",\n");
        fprintf(f, "  \"auto_reindex_count\": \"\"\n");
    }
    fprintf(f, "}\n");
    if (fclose(f) != 0) {
        (void)unlink(tmp);
        LOG_FAIL("dev-activation", "deploy-state fclose: %s", strerror(errno));
    }
    if (rename(tmp, txn->deploy_state) != 0) {
        (void)unlink(tmp);
        LOG_FAIL("dev-activation", "deploy-state rename: %s", strerror(errno));
    }
    return true;
}


#endif /* ZCL_DEV_BUILD || ZCL_TESTING */
