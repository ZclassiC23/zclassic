/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE
#include "devloop.h"

#include "platform/time_compat.h"

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

static size_t cycle_json(const struct zcl_devloop_plan *plan,
                         const char *const *files, size_t file_count,
                         const char *status, const char *phase,
                         int64_t elapsed_ms, const char *capsule,
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
                            : "zclassic23-dev dev diagnose latest") ||
        !appendf(out, out_sz, &pos, "}"))
        return 0;
    return pos;
}

static int finish_cycle(const struct zcl_devloop_plan *plan,
                        const char *const *files, size_t file_count,
                        const char *status, const char *phase,
                        int64_t started_us, const char *capsule)
{
    char body[16384], path[PATH_MAX];
    int64_t elapsed_ms = (platform_time_monotonic_us() - started_us) / 1000;
    size_t len = cycle_json(plan, files, file_count, status, phase,
                            elapsed_ms, capsule, body, sizeof(body));
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
#ifndef ZCL_DEV_BUILD
    (void)repo_root;
    return finish_cycle(&plan, files, file_count, "rejected",
                        "dev_build_required", started_us,
                        "native mutation is compiled out of release builds");
#else
    char root[PATH_MAX];
    if (!repo_root_resolve(repo_root, root))
        return finish_cycle(&plan, files, file_count, "rejected",
                            "confinement", started_us,
                            "repository root is not a real zclassic23 checkout");
    if (plan.action == ZCL_DEVLOOP_CHECK)
        return finish_cycle(&plan, files, file_count, "passed", "check",
                            started_us, "");

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
                                capsule[0] ? capsule : "fast sim runner unavailable");
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
                                "build", started_us, capsule);
        }
        char artifact[PATH_MAX];
        if (!find_artifact(root, result.output, artifact))
            return finish_cycle(&plan, files, file_count, "rejected",
                                "build", started_us,
                                "generation builder returned no confined artifact");

        const char *home = getenv("HOME");
        char bin[PATH_MAX], datadir_flag[PATH_MAX], args_json[PATH_MAX + 256];
        if (!home || !home[0] ||
            snprintf(bin, sizeof(bin), "%s/build/bin/zclassic23-dev", root) <= 0 ||
            snprintf(datadir_flag, sizeof(datadir_flag),
                     "-datadir=%s/.zclassic-c23-dev", home) <= 0 ||
            !build_hotswap_args(artifact, plan.probe_tool, args_json))
            return finish_cycle(&plan, files, file_count, "rejected",
                                "confinement", started_us,
                                "could not construct exact dev-lane invocation");

        const char *smoke_argv[] = {
            bin, datadir_flag, "-rpcport=18252", "mcpcall",
            "zcl_agent_hotswap", args_json, NULL
        };
        if (!zcl_devloop_process_run(root, smoke_argv, 15000, &result) ||
            !result_ok(&result) || !strstr(result.output, "\"ok\":true") ||
            strstr(result.output, "\"probe_error\"")) {
            output_capsule(&result, capsule);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "precommit_probe", started_us, capsule);
        }

        const char *commit_argv[] = {
            bin, datadir_flag, "-rpcport=18252", "dev_hotswap",
            artifact, plan.probe_tool, NULL
        };
        if (!zcl_devloop_process_run(root, commit_argv, 15000, &result) ||
            !result_ok(&result) || !strstr(result.output, "\"ok\":true")) {
            output_capsule(&result, capsule);
            return finish_cycle(&plan, files, file_count, "rejected",
                                "resident_commit", started_us, capsule);
        }
        return finish_cycle(&plan, files, file_count, "passed",
                            "resident_commit", started_us, "");
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
                            "transactional_reload", started_us, capsule);
    }
    return finish_cycle(&plan, files, file_count, "passed",
                        "transactional_reload", started_us, "");
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
