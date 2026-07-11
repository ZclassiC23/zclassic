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

#ifdef ZCL_DEV_BUILD
static void output_capsule(const struct zcl_devloop_process_result *result,
                           char out[1024])
{
    if (!result || result->output_len == 0) {
        out[0] = 0;
        return;
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
    memcpy(out, start, len);
    out[len] = 0;
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

    /* Compatibility backend during the native activation extraction.  The
     * LLM-facing plane is already C-only; this fixed argv is never a shell
     * interpolation surface and preserves the proven transactional rollback
     * behavior until dev_activation.c owns it directly. */
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
