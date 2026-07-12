/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE
#include "devloop.h"

#include "platform/time_compat.h"
#include "vcs/vcs_devloop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static bool appendf(char *out, size_t cap, size_t *pos,
                    const char *fmt, ...)
{
    if (!out || !pos || *pos >= cap)
        return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos)
        return false;
    *pos += (size_t)n;
    return true;
}

static bool append_string(char *out, size_t cap, size_t *pos,
                          const char *value)
{
    if (!appendf(out, cap, pos, "\""))
        return false;
    for (const unsigned char *p = (const unsigned char *)(value ? value : "");
         *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (!appendf(out, cap, pos, "\\%c", *p))
                return false;
        } else if (*p == '\n') {
            if (!appendf(out, cap, pos, "\\n"))
                return false;
        } else if (*p == '\r') {
            if (!appendf(out, cap, pos, "\\r"))
                return false;
        } else if (*p < 0x20) {
            if (!appendf(out, cap, pos, "\\u%04x", *p))
                return false;
        } else if (!appendf(out, cap, pos, "%c", *p)) {
            return false;
        }
    }
    return appendf(out, cap, pos, "\"");
}

static bool mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    if (!path || !path[0] || strlen(path) >= sizeof(tmp))
        return false;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static bool native_state_path(char out[PATH_MAX])
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    int n = snprintf(out, PATH_MAX,
                     "%s/.local/state/zclassic23-dev/native-cycle.json",
                     home);
    return n > 0 && n < PATH_MAX;
}

static bool write_atomic(const char *path, const char *body, size_t len)
{
    char dir[PATH_MAX], tmp[PATH_MAX];
    if (!path || !body || strlen(path) >= sizeof(dir))
        return false;
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return false;
    *slash = 0;
    if (!mkdirs(dir))
        return false;
    int n = snprintf(tmp, sizeof(tmp), "%s/.native-cycle.%ld.tmp",
                     dir, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    size_t off = 0;
    while (off < len) {
        ssize_t wrote = write(fd, body + off, len - off);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            close(fd);
            unlink(tmp);
            return false;
        }
        off += (size_t)wrote;
    }
    bool ok = fsync(fd) == 0 && close(fd) == 0 && rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
    return ok;
}

/* Scan [out, out+len) FORWARD line-by-line for the first ACTIONABLE line —
 * a compiler diagnostic (a line containing ": error:") or a test failure (a
 * line containing "FAIL", "Assertion", or "EXPECT") — and copy it (newline
 * stripped, bounded by dstcap) into dst. Returns true iff one was found; dst
 * is always NUL-terminated. Pure: no I/O, no allocation. Static so the real
 * capsule builder can call it directly; a thin non-static wrapper below lets
 * the ZCL_TESTING harness exercise this pure function without ZCL_DEV_BUILD.
 * Guarded ZCL_DEV_BUILD||ZCL_TESTING so the test binary (which defines only
 * ZCL_TESTING) compiles and reaches it. */
#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)
static bool distill_first_error(const char *out, size_t len,
                                char *dst, size_t dstcap)
{
    if (!out || !dst || dstcap == 0)
        return false;
    dst[0] = 0;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && out[i] != '\n' && out[i] != '\r')
            i++;
        size_t line_len = i - start;
        while (i < len && (out[i] == '\n' || out[i] == '\r'))
            i++;
        if (line_len == 0)
            continue;
        const char *line = out + start;
        bool hit = memmem(line, line_len, ": error:", 8) != NULL ||
                   memmem(line, line_len, "FAIL", 4) != NULL ||
                   memmem(line, line_len, "Assertion", 9) != NULL ||
                   memmem(line, line_len, "EXPECT", 6) != NULL;
        if (hit) {
            size_t copy = line_len < dstcap - 1 ? line_len : dstcap - 1;
            memcpy(dst, line, copy);
            dst[copy] = 0;
            return true;
        }
    }
    return false;
}

/* Thin testable wrapper — distill_first_error is static; the ZCL_TESTING
 * harness drives it through this. Also keeps the static function "used" in a
 * ZCL_TESTING-only build (where output_capsule below is compiled out). */
bool zcl_devloop_distill_first_error(const char *out, size_t len,
                                     char *dst, size_t dstcap)
{
    return distill_first_error(out, len, dst, dstcap);
}
#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

#ifdef ZCL_DEV_BUILD
static void output_capsule(const struct zcl_devloop_process_result *result,
                           char out[1024])
{
    if (!result || result->output_len == 0) {
        out[0] = 0;
        return;
    }

    /* Dense capsule (A4): lead with the first actionable error line so the
     * agent sees the root cause without paging the tail. Fail-open — when no
     * pattern matches, the existing last-N-bytes tail is the whole body, so
     * nothing regresses. */
    size_t pos = 0;
    char first[512];
    if (distill_first_error(result->output, result->output_len,
                            first, sizeof(first))) {
        int n = snprintf(out, 1024, "first_error=%s\n", first);
        if (n > 0 && (size_t)n < 1024)
            pos = (size_t)n;
    }

    const char *start = result->output;
    size_t len = result->output_len;
    if (len > 900) {
        start += len - 900;
        len = 900;
    }
    while (len > 0 && (*start == '\n' || *start == '\r')) {
        start++;
        len--;
    }
    /* Bound the tail so first_error + tail never overflow out[1024]. */
    if (len > 1024 - 1 - pos)
        len = 1024 - 1 - pos;
    memcpy(out + pos, start, len);
    out[pos + len] = 0;
}

static bool result_ok(const struct zcl_devloop_process_result *result)
{
    return result && !result->timed_out && result->term_signal == 0 &&
           result->exit_code == 0;
}

static bool repo_root_resolve(const char *requested, char out[PATH_MAX])
{
    const char *root = requested && requested[0] ? requested : ".";
    if (!realpath(root, out))
        return false;
    char makefile[PATH_MAX], git[PATH_MAX];
    int mn = snprintf(makefile, sizeof(makefile), "%s/Makefile", out);
    int gn = snprintf(git, sizeof(git), "%s/.git", out);
    struct stat st;
    return mn > 0 && (size_t)mn < sizeof(makefile) &&
           gn > 0 && (size_t)gn < sizeof(git) &&
           stat(makefile, &st) == 0 && stat(git, &st) == 0;
}

static bool find_artifact(const char *root, const char *output,
                          char artifact[PATH_MAX])
{
    if (!root || !output)
        return false;
    const char *end = output + strlen(output);
    while (end > output) {
        while (end > output && (end[-1] == '\n' || end[-1] == '\r'))
            end--;
        const char *start = end;
        while (start > output && start[-1] != '\n' && start[-1] != '\r')
            start--;
        size_t len = (size_t)(end - start);
        if (len > 3 && len < PATH_MAX &&
            memchr(start, ' ', len) == NULL &&
            memchr(start, '\t', len) == NULL &&
            memcmp(end - 3, ".so", 3) == 0) {
            char candidate[PATH_MAX];
            if (start[0] == '/')
                snprintf(candidate, sizeof(candidate), "%.*s", (int)len, start);
            else
                snprintf(candidate, sizeof(candidate), "%s/%.*s",
                         root, (int)len, start);
            if (realpath(candidate, artifact)) {
                char expected[PATH_MAX];
                snprintf(expected, sizeof(expected), "%s/build/hotswap/", root);
                return strncmp(artifact, expected, strlen(expected)) == 0;
            }
        }
        end = start;
    }
    return false;
}

static bool build_hotswap_args(const char *artifact, const char *probe,
                               char out[PATH_MAX + 256])
{
    if (!artifact || strchr(artifact, '"') || strchr(artifact, '\\') ||
        !probe || strspn(probe, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != strlen(probe))
        return false;
    int n = snprintf(out, PATH_MAX + 256,
                     "{\"so_path\":\"%s\",\"probe_tool\":\"%s\"}",
                     artifact, probe);
    return n > 0 && n < PATH_MAX + 256;
}

/* Extract the value of a top-level `"field":"<64 hex chars>"` member from a
 * JSON blob without pulling in a full parser — matches the string-scan idiom
 * this file already uses (see the `strstr(result.output, ...)` checks
 * above). Returns false (leaving out[0]=0) if the field is absent or is not
 * exactly 64 hex characters. */
static bool extract_hex64_field(const char *json, const char *field,
                                char out[65])
{
    out[0] = 0;
    if (!json || !field)
        return false;
    char key[96];
    int kn = snprintf(key, sizeof(key), "\"%s\"", field);
    if (kn <= 0 || (size_t)kn >= sizeof(key))
        return false;
    const char *p = strstr(json, key);
    if (!p)
        return false;
    p = strchr(p + strlen(key), '"');
    if (!p)
        return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) != 64)
        return false;
    memcpy(out, p, 64);
    out[64] = 0;
    return true;
}

/* Best-effort generation-id lookup for a RELOAD (transactional_reload)
 * cycle: `make agent-deploy-fast` drives tools/dev/deploy-dev-lane.sh, which
 * writes the zcl.agent_dev_deploy.v1 state file at
 * $HOME/.zclassic-c23-dev/agent-deploy.json on every activation attempt.
 * candidate_sha256 is a bare 64-hex sha256 of the built binary;
 * running_generation is "gen-<64 hex>" or "legacy-<64 hex>" once activation
 * verifies the running process matches. Absence of HOME, the file, or
 * either field is not an error here — the cycle still anchors, just with an
 * all-zero generation binding (finish_cycle passes NULL onward). */
static bool read_reload_generation(char out[65])
{
    out[0] = 0;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path),
                     "%s/.zclassic-c23-dev/agent-deploy.json", home);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char buf[8192];
    size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rn] = 0;

    if (extract_hex64_field(buf, "candidate_sha256", out))
        return true;

    /* running_generation carries a "gen-"/"legacy-" prefix, so it never
     * matches extract_hex64_field's bare-64-hex expectation directly; strip
     * the prefix by hand before comparing lengths. */
    const char *key = "\"running_generation\"";
    const char *p = strstr(buf, key);
    if (!p)
        return false;
    p = strchr(p + strlen(key), '"');
    if (!p)
        return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end)
        return false;
    if ((size_t)(end - p) > 4 && memcmp(p, "gen-", 4) == 0)
        p += 4;
    else if ((size_t)(end - p) > 7 && memcmp(p, "legacy-", 7) == 0)
        p += 7;
    if ((size_t)(end - p) != 64)
        return false;
    memcpy(out, p, 64);
    out[64] = 0;
    return true;
}

/* Hex-encode a 32-byte commit id for the `"vcs_commit"` verdict field. */
static void hex_encode32(const uint8_t in[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[2 * i]     = digits[(in[i] >> 4) & 0xf];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[64] = 0;
}
#endif

/* ── Wave 3.2 native activation engine — dev-lane wiring (pure glue) ────
 * These three functions are declared in devloop.h; the guard there matches
 * this one (ZCL_DEV_BUILD || ZCL_TESTING) so the hermetic test harness can
 * exercise them directly without a fake ops vtable — none of them execs a
 * process or performs I/O beyond getenv(). The actual engine call
 * (dev_activation_run / dev_activation_default_ops, both real-process-exec)
 * stays confined to the ZCL_DEV_BUILD-only zcl_devloop_run_cycle() body
 * below. */
#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)
bool dev_activation_native_enabled(void)
{
    const char *v = getenv("ZCL_DEV_NATIVE_ACTIVATION");
    if (!v || !v[0])
        return false;
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
           strcmp(v, "yes") == 0;
}

bool dev_activation_request_from_cycle(const char *repo_root,
                                       const char *build_commit,
                                       struct dev_activation_cycle_request *out)
{
    if (!repo_root || !repo_root[0] || !build_commit || !out)
        return false;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;

    int n = snprintf(out->artifact_path, sizeof(out->artifact_path),
                     "%s/build/bin/zclassic23-dev", repo_root);
    if (n <= 0 || (size_t)n >= sizeof(out->artifact_path))
        return false;

    const char *gen_root_override = getenv("ZCL_DEV_GENERATION_ROOT");
    n = (gen_root_override && gen_root_override[0])
            ? snprintf(out->gen_root, sizeof(out->gen_root), "%s",
                      gen_root_override)
            : snprintf(out->gen_root, sizeof(out->gen_root),
                      "%s/.local/lib/zclassic23-dev", home);
    if (n <= 0 || (size_t)n >= sizeof(out->gen_root))
        return false;

    n = snprintf(out->datadir, sizeof(out->datadir), "%s/.zclassic-c23-dev",
                home);
    if (n <= 0 || (size_t)n >= sizeof(out->datadir))
        return false;

    memset(&out->req, 0, sizeof(out->req));
    out->req.repo_root = repo_root;
    out->req.artifact_path = out->artifact_path;
    out->req.build_commit = build_commit;
    out->req.build_type = "fast";
    out->req.gen_root = out->gen_root;
    out->req.datadir = out->datadir;
    out->req.unit = "zcl23-dev.service";
    out->req.rpcport = 18252;
    out->req.mode = DEV_ACTIVATION_MODE_ACTIVATE;
    return true;
}

void dev_activation_map_result(const struct dev_activation_result *r,
                               struct dev_activation_cycle_outcome *out)
{
    memset(out, 0, sizeof(*out));
    if (!r)
        return;
    out->ok = (r->status == DEV_ACTIVATION_OK);
    const char *capsule = r->failure_capsule[0] ? r->failure_capsule
                          : r->verify_detail[0] ? r->verify_detail : "";
    snprintf(out->capsule, sizeof(out->capsule), "%s", capsule);
    snprintf(out->generation_hex, sizeof(out->generation_hex), "%s",
            r->candidate_sha256);
}
#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

int zcl_devloop_run_sim(const char *repo_root)
{
#ifndef ZCL_DEV_BUILD
    (void)repo_root;
    fprintf(stderr, "[devloop] simulator execution requires ZCL_DEV_BUILD\n");
    return 2;
#else
    char root[PATH_MAX], test_bin[PATH_MAX];
    if (!repo_root_resolve(repo_root, root)) {
        fprintf(stderr, "[devloop] simulator: invalid repository root\n");
        return 2;
    }
    snprintf(test_bin, sizeof(test_bin), "%s/build/bin/test_parallel_fast", root);
    if (access(test_bin, X_OK) != 0) {
        fprintf(stderr, "[devloop] simulator: focused runner is not built\n");
        return 2;
    }
    const char *argv[] = { test_bin, "--only=hotswap_simnet", NULL };
    struct zcl_devloop_process_result result = {0};
    if (!zcl_devloop_process_run(root, argv, 10000, &result))
        return 1;
    if (!result_ok(&result)) {
        char capsule[1024];
        output_capsule(&result, capsule);
        fprintf(stderr, "[devloop] simulator failed: %s\n", capsule);
        return 1;
    }
    printf("{\"schema\":\"zcl.dev_sim.v1\",\"status\":\"passed\","
           "\"group\":\"hotswap_simnet\",\"elapsed_ms\":%lld}\n",
           (long long)result.elapsed_ms);
    return 0;
#endif
}

/* ZVCS auto-anchor outcome for this cycle's verdict JSON. Populated
 * unconditionally (zero-valued when the anchor was never attempted — e.g. a
 * non-"passed" verdict, or a release build), so cycle_json() needs no
 * ZCL_DEV_BUILD conditional of its own. */
struct vcs_anchor_fields {
    bool attempted;
    char commit_hex[65];  /* set iff the snapshot committed */
    char error[256];      /* set iff attempted and not committed */
    bool sealed_refusal;  /* true iff refused for touching a sealed path */
    bool deferred;        /* generation-neutral first baseline is out of band */
};

static size_t cycle_json(const struct zcl_devloop_plan *plan,
                         const char *const *files, size_t file_count,
                         const char *status, const char *phase,
                         int64_t elapsed_ms, const char *capsule,
                         const struct vcs_anchor_fields *vcs,
                         char *out, size_t out_sz)
{
    size_t pos = 0;
    if (!appendf(out, out_sz, &pos,
                 "{\"schema\":\"zcl.dev_cycle.v1\",\"producer\":\"native\",\"status\":") ||
        !append_string(out, out_sz, &pos, status) ||
        !appendf(out, out_sz, &pos, ",\"action\":") ||
        !append_string(out, out_sz, &pos, plan->action_name) ||
        !appendf(out, out_sz, &pos, ",\"reason\":") ||
        !append_string(out, out_sz, &pos, plan->reason) ||
        !appendf(out, out_sz, &pos, ",\"phase\":") ||
        !append_string(out, out_sz, &pos, phase) ||
        !appendf(out, out_sz, &pos,
                 ",\"elapsed_ms\":%lld,\"files\":[",
                 (long long)elapsed_ms))
        return 0;
    for (size_t i = 0; i < file_count; i++) {
        if ((i && !appendf(out, out_sz, &pos, ",")) ||
            !append_string(out, out_sz, &pos, files[i]))
            return 0;
    }
    if (!appendf(out, out_sz, &pos, "],\"failure_capsule\":") ||
        !append_string(out, out_sz, &pos, capsule ? capsule : "") ||
        !appendf(out, out_sz, &pos, ",\"agent_next_action\":") ||
        !append_string(out, out_sz, &pos,
                       strcmp(status, "passed") == 0
                            ? "keep editing; native watch owns the next cycle"
                            : "zclassic23-dev dev diagnose latest"))
        return 0;
    if (vcs && vcs->attempted) {
        if (vcs->commit_hex[0] &&
            (!appendf(out, out_sz, &pos, ",\"vcs_commit\":") ||
             !append_string(out, out_sz, &pos, vcs->commit_hex)))
            return 0;
        if (vcs->error[0] &&
            (!appendf(out, out_sz, &pos, ",\"vcs_error\":") ||
             !append_string(out, out_sz, &pos, vcs->error)))
            return 0;
        if (vcs->sealed_refusal &&
            !appendf(out, out_sz, &pos, ",\"vcs_sealed_refusal\":true"))
            return 0;
        if (vcs->deferred &&
            !appendf(out, out_sz, &pos, ",\"vcs_deferred\":true"))
            return 0;
    }
    if (!appendf(out, out_sz, &pos, "}"))
        return 0;
    return pos;
}

static int finish_cycle(const struct zcl_devloop_plan *plan,
                        const char *const *files, size_t file_count,
                        const char *status, const char *phase,
                        int64_t started_us, const char *capsule,
                        const char *repo_root, const char *generation_hex)
{
    char body[16384], path[PATH_MAX];
    int64_t elapsed_ms = (platform_time_monotonic_us() - started_us) / 1000;

    /* Auto-anchor on green (Wave 2.3): every "passed" verdict gets a ZVCS
     * snapshot binding the source tree to this verdict + the binary
     * generation it produced. Fail-open by construction: vcs_devloop never
     * uses a process-terminating LOG_* macro, so a ZVCS problem can only
     * ever change what lands in the "vcs_commit"/"vcs_error" verdict
     * fields below, never the cycle's own pass/fail outcome. */
    struct vcs_anchor_fields vcsf = {0};
#ifdef ZCL_DEV_BUILD
    if (strcmp(status, "passed") == 0 && repo_root && repo_root[0]) {
        struct vcs_devloop_verdict v = {0};
        v.verdict_status = 0;
        v.phase = phase;
        v.elapsed_ms = elapsed_ms;
        v.generation_hex = (generation_hex && generation_hex[0]) ? generation_hex : NULL;
        v.agent_id = getenv("ZCL_AGENT_ID");
        v.session_id = getenv("ZCL_SESSION_ID");
        v.task_ref = getenv("ZCL_TASK_REF");
        v.defer_initial_snapshot = true;

        struct vcs_devloop_anchor_result ar;
        vcs_devloop_anchor_cycle(repo_root, &v, &ar);
        vcsf.attempted = true;
        switch (ar.status) {
        case VCS_DEVLOOP_ANCHOR_OK:
            hex_encode32(ar.commit_id, vcsf.commit_hex);
            break;
        case VCS_DEVLOOP_ANCHOR_REFUSED:
            vcsf.sealed_refusal = true;
            snprintf(vcsf.error, sizeof(vcsf.error), "%s", ar.error);
            break;
        case VCS_DEVLOOP_ANCHOR_DEFERRED:
            vcsf.deferred = true;
            snprintf(vcsf.error, sizeof(vcsf.error), "%s", ar.error);
            /* lib/vcs never launches the baseline itself (ZVCS
             * sovereignty — check-vcs-no-git). When this cycle is the one
             * that discovered no baseline is running yet, the dev loop is
             * responsible for detaching it so the baseline's first-snapshot
             * cost doesn't block the edit->verdict latency path. */
            if (ar.baseline_needed &&
                !zcl_devloop_baseline_launch(repo_root))
                fprintf(stderr,
                        "[devloop] vcs baseline detach failed (fail-open): "
                        "will retry next cycle\n");
            break;
        case VCS_DEVLOOP_ANCHOR_ERROR:
        default:
            snprintf(vcsf.error, sizeof(vcsf.error), "%s", ar.error);
            fprintf(stderr, "[devloop] vcs auto-anchor failed (fail-open): %s\n",
                    ar.error);
            break;
        }
    }
#else
    (void)repo_root;
    (void)generation_hex;
#endif

    size_t len = cycle_json(plan, files, file_count, status, phase,
                            elapsed_ms, capsule, &vcsf, body, sizeof(body));
    if (len == 0) {
        fprintf(stderr, "[devloop] cycle verdict exceeded its bounded buffer\n");
        return 1;
    }
    body[len++] = '\n';
    body[len] = 0;
    if (native_state_path(path) && !write_atomic(path, body, len))
        fprintf(stderr, "[devloop] could not persist native cycle verdict\n");
    fwrite(body, 1, len, stdout);
    fflush(stdout);
    return strcmp(status, "passed") == 0 ? 0 : 1;
}

/* Wave 2.4 core refusal: emit the structured sealed-core refusal envelope
 * (stdout + the persisted zcl.dev_cycle.v1 verdict) and return CLI exit 3
 * (blocked-by-precondition). Reused by both the one-shot `dev change cycle`
 * path and the persistent watcher — both funnel through
 * zcl_devloop_run_cycle(). No subprocess is launched: the caller returns here
 * BEFORE any hotswap/reload publish step. */
#ifdef ZCL_DEV_BUILD
/* Recompute the build-commit stamp the exact way
 * tools/dev/deploy-dev-lane.sh:build_candidate() does: refresh the index,
 * take the short HEAD hash, append "-dirty" iff the tracked tree differs
 * from HEAD. ZCL_DEV_BUILD_COMMIT_OVERRIDE takes precedence when set (same
 * override name the shell path honors). Every step is a fixed-argv exec via
 * zcl_devloop_process_run — never a shell string. Returns false when git is
 * unavailable or its output cannot be parsed; the caller falls back to the
 * shell transactional-reload path in that case. */
static bool resolve_build_commit(const char *root, char out[128])
{
    const char *override = getenv("ZCL_DEV_BUILD_COMMIT_OVERRIDE");
    if (override && override[0]) {
        int n = snprintf(out, 128, "%s", override);
        return n > 0 && n < 128;
    }

    struct zcl_devloop_process_result refresh = {0};
    const char *refresh_argv[] = {
        "git", "update-index", "-q", "--refresh", NULL
    };
    (void)zcl_devloop_process_run(root, refresh_argv, 15000, &refresh);

    struct zcl_devloop_process_result rev = {0};
    const char *rev_argv[] = { "git", "rev-parse", "--short", "HEAD", NULL };
    if (!zcl_devloop_process_run(root, rev_argv, 15000, &rev) ||
        !result_ok(&rev) || rev.output_len == 0)
        return false;
    char hash[64];
    size_t hn = rev.output_len;
    while (hn > 0 && (rev.output[hn - 1] == '\n' || rev.output[hn - 1] == '\r'))
        hn--;
    if (hn == 0 || hn >= sizeof(hash))
        return false;
    memcpy(hash, rev.output, hn);
    hash[hn] = 0;

    struct zcl_devloop_process_result dirty = {0};
    const char *dirty_argv[] = {
        "git", "diff-index", "--quiet", "HEAD", "--", NULL
    };
    bool is_dirty = !zcl_devloop_process_run(root, dirty_argv, 15000, &dirty) ||
                    dirty.timed_out || dirty.term_signal != 0 ||
                    dirty.exit_code != 0;

    int n = snprintf(out, 128, "%s%s", hash, is_dirty ? "-dirty" : "");
    return n > 0 && n < 128;
}
#endif /* ZCL_DEV_BUILD */

static int emit_refusal(const char *const *files, size_t file_count)
{
    char body[16384], path[PATH_MAX];
    size_t len = zcl_devloop_refusal_json(files, file_count, body,
                                          sizeof(body) - 2);
    if (len == 0) {
        fprintf(stderr,
                "[devloop] sealed-core refusal envelope exceeded its buffer\n");
        return 3;  /* still refuse — never fall through to publish */
    }
    body[len++] = '\n';
    body[len] = 0;
    if (native_state_path(path) && !write_atomic(path, body, len))
        fprintf(stderr, "[devloop] could not persist refusal verdict\n");
    fwrite(body, 1, len, stdout);
    fflush(stdout);
    return 3;
}

int zcl_devloop_run_cycle(const char *repo_root,
                          const char *const *files,
                          size_t file_count)
{
    struct zcl_devloop_plan plan;
    int64_t started_us = platform_time_monotonic_us();
    if (!zcl_devloop_plan_files(files, file_count, &plan)) {
        fprintf(stderr, "[devloop] cycle: invalid changed-file set\n");
        return 2;
    }

    /* Core refusal, BEFORE any publish and BEFORE the dev-build gate (a
     * release binary refuses identically). A changed-file set touching the
     * sealed consensus core (core/ — what core/MANIFEST.sha3 pins) is
     * structurally refused from the autonomous fast path unless the owner
     * unseal ritual left a valid one-shot .core-unseal-token at the repo root.
     *
     * Token semantics (see zcl_devloop_unseal_token_present): the token is
     * checked read-only and NEVER consumed here. `make core-seal` consumes it
     * when the sealed edit lands, so one `make core-unseal` authorizes exactly
     * one landed commit — which may cover several iterative dev-cycles while
     * the author converges the fix — not one dev-cycle. With the token
     * present the cycle proceeds and (because zcl_devloop_plan_files marks any
     * sealed file consensus_risk) routes to the heaviest proof path, exactly
     * as a lib/validation edit does today. Sealed != frozen: the refusal
     * envelope always names the elevated procedure. */
    if (plan.sealed_core) {
        if (!zcl_devloop_unseal_token_present(repo_root))
            return emit_refusal(files, file_count);
        fprintf(stderr,
                "[devloop] sealed consensus core: unseal token present at "
                "%s/.core-unseal-token — seal lifted for THIS cycle; routing "
                "to the heaviest proof path. The token is consumed by "
                "'make core-seal' when the edit lands, so one unseal = one "
                "landed commit, not one dev-cycle.\n",
                (repo_root && repo_root[0]) ? repo_root : ".");
    }
#ifndef ZCL_DEV_BUILD
    (void)repo_root;
    return finish_cycle(&plan, files, file_count, "rejected",
                        "dev_build_required", started_us,
                        "native mutation is compiled out of release builds",
                        NULL, NULL);
#else
    char root[PATH_MAX];
    if (!repo_root_resolve(repo_root, root))
        return finish_cycle(&plan, files, file_count, "rejected",
                            "confinement", started_us,
                            "repository root is not a real zclassic23 checkout",
                            NULL, NULL);
    if (plan.action == ZCL_DEVLOOP_CHECK)
        return finish_cycle(&plan, files, file_count, "passed", "check",
                            started_us, "", root, NULL);

    struct zcl_devloop_process_result result = {0};
    char capsule[1024] = {0};
    if (plan.action == ZCL_DEVLOOP_HOTSWAP) {
        char test_bin[PATH_MAX];
        snprintf(test_bin, sizeof(test_bin), "%s/build/bin/test_parallel_fast", root);
        const char *sim_argv[] = {
            test_bin, "--only=hotswap_simnet", NULL
        };
        if (access(test_bin, X_OK) != 0 ||
            !zcl_devloop_process_run(root, sim_argv, 10000, &result) ||
            !result_ok(&result)) {
            output_capsule(&result, capsule);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "sim", started_us,
                                capsule[0] ? capsule : "fast sim runner unavailable",
                                root, NULL);
        }

        char files_arg[ZCL_DEVLOOP_PATH_MAX + 16];
        snprintf(files_arg, sizeof(files_arg), "FILES=%s", files[0]);
        const char *build_argv[] = {
            "make", "--no-print-directory", "hotswap-so", files_arg, NULL
        };
        if (!zcl_devloop_process_run(root, build_argv, 60000, &result) ||
            !result_ok(&result)) {
            output_capsule(&result, capsule);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "build", started_us, capsule, root, NULL);
        }
        char artifact[PATH_MAX];
        if (!find_artifact(root, result.output, artifact))
            return finish_cycle(&plan, files, file_count, "rejected",
                                "build", started_us,
                                "generation builder returned no confined artifact",
                                root, NULL);

        const char *home = getenv("HOME");
        char bin[PATH_MAX], datadir_flag[PATH_MAX], args_json[PATH_MAX + 256];
        if (!home || !home[0] ||
            snprintf(bin, sizeof(bin), "%s/build/bin/zclassic23-dev", root) <= 0 ||
            snprintf(datadir_flag, sizeof(datadir_flag),
                     "-datadir=%s/.zclassic-c23-dev", home) <= 0 ||
            !build_hotswap_args(artifact, plan.probe_tool, args_json))
            return finish_cycle(&plan, files, file_count, "rejected",
                                "confinement", started_us,
                                "could not construct exact dev-lane invocation",
                                root, NULL);

        const char *smoke_argv[] = {
            bin, datadir_flag, "-rpcport=18252", "mcpcall",
            "zcl_agent_hotswap", args_json, NULL
        };
        if (!zcl_devloop_process_run(root, smoke_argv, 15000, &result) ||
            !result_ok(&result) || !strstr(result.output, "\"ok\":true") ||
            strstr(result.output, "\"probe_error\"")) {
            output_capsule(&result, capsule);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "precommit_probe", started_us, capsule,
                                root, NULL);
        }

        const char *commit_argv[] = {
            bin, datadir_flag, "-rpcport=18252", "dev_hotswap",
            artifact, plan.probe_tool, NULL
        };
        if (!zcl_devloop_process_run(root, commit_argv, 15000, &result) ||
            !result_ok(&result) || !strstr(result.output, "\"ok\":true")) {
            if (strstr(result.output, "generation registry full") &&
                strstr(result.output, "\"rejection_stage\":\"registry\""))
                goto transactional_reload;
            output_capsule(&result, capsule);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "resident_commit", started_us, capsule,
                                root, NULL);
        }
        /* The route-generation id for a hotswap cycle: dev_hotswap's JSON
         * result (same shape as zcl_agent_hotswap's, see
         * tools/mcp/controllers/dev_hotswap_controller.c) already carries
         * artifact_sha256 — the sha256 of the exact .so this cycle dlopen'd.
         * Best-effort: an empty/unparsable value just means the anchor binds
         * a zero generation, never a cycle failure. */
        char generation_hex[65];
        extract_hex64_field(result.output, "artifact_sha256", generation_hex);
        return finish_cycle(&plan, files, file_count, "passed",
                            "resident_commit", started_us, "",
                            root, generation_hex[0] ? generation_hex : NULL);
    }

    /* MODE=verify / MODE=ff (A6): decouple "prove it compiles + tests +
     * lints" from "deploy it to the running dev lane". When the persistent
     * watcher runs in the opt-in verify mode, a plain RELOAD-classified edit
     * runs the fast feed-forward `make ff` ladder (compile -> focused tests
     * -> lint) and reports a verify verdict INSTEAD of paying the ~60s
     * transactional dev-lane reload below; the operator deploys explicitly
     * with `dev change apply`. The guard deliberately excludes consensus-risk
     * and sealed-core edits — those MUST still take the heavy reload path so
     * the proof surface is never weakened. Hotswap-eligible single-file edits
     * were already handled and returned above; a registry-full hotswap that
     * `goto`s the label below jumps PAST this guard and reloads heavily. */
    if (plan.action == ZCL_DEVLOOP_RELOAD &&
        !plan.consensus_risk && !plan.sealed_core) {
        const char *watch_mode = getenv("ZCL_DEV_WATCH_MODE");
        if (watch_mode && (strcmp(watch_mode, "verify") == 0 ||
                           strcmp(watch_mode, "ff") == 0)) {
            plan.action_name = "verify";
            const char *ff_argv[] = {
                "make", "--no-print-directory", "ff", NULL
            };
            if (!zcl_devloop_process_run(root, ff_argv, 900000, &result) ||
                !result_ok(&result)) {
                output_capsule(&result, capsule);
                fprintf(stderr,
                        "[devloop] verify (make ff) failed — fix, then run "
                        "`dev change apply` to deploy\n");
                return finish_cycle(&plan, files, file_count, "rejected",
                                    "verify", started_us,
                                    capsule[0] ? capsule : "make ff failed",
                                    root, NULL);
            }
            fprintf(stderr,
                    "[devloop] verified in %llds — run `dev change apply` "
                    "to deploy\n",
                    (long long)((platform_time_monotonic_us() - started_us) /
                                1000000));
            return finish_cycle(&plan, files, file_count, "passed", "verify",
                                started_us, "", root, NULL);
        }
    }

transactional_reload:
    /* Wave 3.2: ZCL_DEV_NATIVE_ACTIVATION=1 routes this cycle through the
     * native transactional activation engine (dev_activation_run()) instead
     * of shelling out to deploy-dev-lane.sh. The build step is common to
     * both paths (`make fast-rebuild`); only what happens to the freshly
     * built artifact afterward differs. Default OFF — see
     * dev_activation_native_enabled() in devloop.h for why this is a
     * runtime env check rather than a second compile-time gate. */
    if (dev_activation_native_enabled()) {
        const char *build_argv[] = {
            "make", "--no-print-directory", "fast-rebuild", NULL
        };
        if (!zcl_devloop_process_run(root, build_argv, 600000, &result) ||
            !result_ok(&result)) {
            output_capsule(&result, capsule);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "transactional_reload", started_us,
                                capsule[0] ? capsule : "fast-rebuild failed",
                                root, NULL);
        }

        char build_commit[128];
        struct dev_activation_cycle_request creq;
        if (resolve_build_commit(root, build_commit) &&
            dev_activation_request_from_cycle(root, build_commit, &creq)) {
            struct dev_activation_ops ops;
            dev_activation_default_ops(&creq.req, &ops);
            struct dev_activation_result ar = {0};
            dev_activation_run(&creq.req, &ops, &ar);

            struct dev_activation_cycle_outcome outcome;
            dev_activation_map_result(&ar, &outcome);
            if (outcome.ok)
                return finish_cycle(&plan, files, file_count, "passed",
                                    "transactional_reload", started_us, "",
                                    root, outcome.generation_hex[0]
                                          ? outcome.generation_hex : NULL);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "transactional_reload", started_us,
                                outcome.capsule[0] ? outcome.capsule
                                    : "native activation failed",
                                root, NULL);
        }
        /* A precondition the native engine cannot itself satisfy (HOME
         * unset, git unavailable/unparsable) — fall through to the proven
         * shell path below, which re-derives the same inputs its own way
         * and has its own fast-rebuild call (idempotent after the one just
         * run above). */
        fprintf(stderr,
                "[devloop] native activation preconditions unmet; falling "
                "back to the shell transactional-reload path\n");
    }

    /* Compatibility backend during the native activation extraction.  The
     * LLM-facing plane is already C-only; this fixed argv is never a shell
     * interpolation surface and preserves the proven transactional rollback
     * behavior until ZCL_DEV_NATIVE_ACTIVATION is the default (see
     * docs/work/HOTSWAP.md). */
    const char *reload_argv[] = {
        "make", "--no-print-directory", "agent-deploy-fast", NULL
    };
    if (!zcl_devloop_process_run(root, reload_argv, 900000, &result) ||
        !result_ok(&result)) {
        output_capsule(&result, capsule);
        return finish_cycle(&plan, files, file_count, "rejected",
                            "transactional_reload", started_us, capsule,
                            root, NULL);
    }
    /* The generation id for a RELOAD cycle: read it back from the
     * zcl.agent_dev_deploy.v1 state file the just-run deploy-dev-lane.sh
     * wrote (see read_reload_generation() above). Best-effort, same
     * fail-open contract as the hotswap path. */
    char generation_hex[65];
    read_reload_generation(generation_hex);
    return finish_cycle(&plan, files, file_count, "passed",
                        "transactional_reload", started_us, "",
                        root, generation_hex[0] ? generation_hex : NULL);
#endif
}

int zcl_devloop_print_status(void)
{
    char path[PATH_MAX];
    if (!native_state_path(path)) {
        fprintf(stderr, "[devloop] status: HOME is unavailable\n");
        return 1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("{\"schema\":\"zcl.dev_cycle.v1\",\"status\":\"unavailable\","
               "\"agent_next_action\":\"keep editing or run dev loop watch\"}\n");
        return 0;
    }
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    fwrite(buf, 1, n, stdout);
    if (n == 0 || buf[n - 1] != '\n')
        fputc('\n', stdout);
    return 0;
}
