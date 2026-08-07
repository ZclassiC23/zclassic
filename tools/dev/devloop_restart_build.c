/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Resident DEV_RESTART builder. Make freezes the compiler/link action once;
 * the watcher compiles only changed C translation units, substitutes their
 * objects into the complete static-dev link response, links directly, and
 * executes one bounded command-runtime probe. Every candidate is explicitly
 * incomplete until mapped proofs run. It never starts Make or a shell and
 * never publishes to a service, datadir, wallet, or release path.
 */

#define _GNU_SOURCE
#include "devloop.h"

#include "base/hex.h"
#include "crypto/sha256.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/safe_alloc.h"
#include "util/spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define RR_TEXT_MAX 16384
#define RR_ARG_MAX 512
#define RR_SOURCE_MAX 32

struct rr_plan {
    char root[PATH_MAX];
    char cc[512];
    char compiler_id[65];
    char base_generation[65];
    char cflags[RR_TEXT_MAX];
    char ldflags[4096];
    char libs[4096];
    char obj_dir[PATH_MAX];
    char link_rsp[PATH_MAX];
    struct stat stamp;
    bool loaded;
};

struct rr_overlay {
    char source[ZCL_DEVLOOP_PATH_MAX];
    char base_object[PATH_MAX];
    char overlay_object[PATH_MAX];
    char source_sha256[65];
};

static pthread_mutex_t g_rr_mu = PTHREAD_MUTEX_INITIALIZER;
static struct rr_plan g_rr_plan;

static void rr_why(char *why, size_t why_len, const char *message)
{
    if (why && why_len)
        (void)snprintf(why, why_len, "%s", message ? message : "unknown");
}

static bool rr_regular(const char *path, struct stat *out)
{
    struct stat st;
    if (!path || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        S_ISLNK(st.st_mode))
        return false;
    if (out) *out = st;
    return true;
}

static bool rr_directory(const char *path)
{
    struct stat st;
    return path && lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           !S_ISLNK(st.st_mode);
}

static bool rr_mkdirs(const char *path)
{
    char copy[PATH_MAX];
    struct stat st;
    if (!path || path[0] != '/' || strlen(path) >= sizeof(copy))
        return false;
    (void)snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(copy, 0700) != 0 &&
            (errno != EEXIST || lstat(copy, &st) != 0 ||
             !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)))
            return false;
        *p = '/';
    }
    return mkdir(copy, 0700) == 0 ||
           (errno == EEXIST && lstat(copy, &st) == 0 &&
            S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode));
}

static bool rr_safe_relative(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || strstr(path, "..") ||
        strchr(path, '\\'))
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        if (*p < 0x20)
            return false;
    return true;
}

static bool rr_hex64(const char *value)
{
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    return true;
}

static bool rr_line(char *dst, size_t cap, const char *line,
                    const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0)
        return false;
    const char *value = line + prefix_len;
    size_t len = strcspn(value, "\r\n");
    if (len == 0 || len >= cap)
        return false;
    memcpy(dst, value, len);
    dst[len] = 0;
    return true;
}

static bool rr_join_root(const char *root, const char *rel,
                         char out[PATH_MAX])
{
    return rr_safe_relative(rel) &&
           snprintf(out, PATH_MAX, "%s/%s", root, rel) < PATH_MAX;
}

static bool rr_stat_equal(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

static bool rr_plan_load_locked(const char *root, struct rr_plan *out,
                                bool *cache_hit, int64_t *elapsed_us,
                                char *why, size_t why_len)
{
    int64_t started = platform_time_monotonic_us();
    char path[PATH_MAX], makefile[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/build/dev-loop/restart.env", root) >=
            (int)sizeof(path) ||
        snprintf(makefile, sizeof(makefile), "%s/Makefile", root) >=
            (int)sizeof(makefile)) {
        rr_why(why, why_len, "restart action-plan path overflow");
        return false;
    }
    struct stat stamp, make_st;
    if (!rr_regular(path, &stamp) || !rr_regular(makefile, &make_st)) {
        rr_why(why, why_len,
               "restart action plan absent; run make dev-bin once");
        return false;
    }
    if (make_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (make_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         make_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec)) {
        rr_why(why, why_len,
               "restart action plan stale after build-system change");
        return false;
    }
    if (g_rr_plan.loaded && strcmp(g_rr_plan.root, root) == 0 &&
        rr_stat_equal(&g_rr_plan.stamp, &stamp)) {
        *out = g_rr_plan;
        *cache_hit = true;
        *elapsed_us = platform_time_monotonic_us() - started;
        return true;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        rr_why(why, why_len, "restart action plan could not be opened");
        return false;
    }
    struct rr_plan next = {0};
    char line[RR_TEXT_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (rr_line(next.cc, sizeof(next.cc), line, "CC=") ||
            rr_line(next.compiler_id, sizeof(next.compiler_id), line,
                    "COMPILER_ID=") ||
            rr_line(next.base_generation, sizeof(next.base_generation), line,
                    "BASE_GENERATION=") ||
            rr_line(next.cflags, sizeof(next.cflags), line, "DEV_CFLAGS=") ||
            rr_line(next.ldflags, sizeof(next.ldflags), line,
                    "DEV_LDFLAGS=") ||
            rr_line(next.libs, sizeof(next.libs), line, "DEV_LIBS=") ||
            rr_line(next.obj_dir, sizeof(next.obj_dir), line,
                    "DEV_OBJ_DIR=") ||
            rr_line(next.link_rsp, sizeof(next.link_rsp), line,
                    "DEV_LINK_RSP="))
            continue;
        fclose(f);
        rr_why(why, why_len, "restart action plan has an unknown field");
        return false;
    }
    bool read_error = ferror(f) != 0;
    fclose(f);
    char obj_full[PATH_MAX], rsp_full[PATH_MAX];
    if (read_error || !next.cc[0] || !rr_hex64(next.compiler_id) ||
        !rr_hex64(next.base_generation) ||
        !next.cflags[0] || !next.ldflags[0] || !next.libs[0] ||
        !rr_join_root(root, next.obj_dir, obj_full) ||
        !rr_join_root(root, next.link_rsp, rsp_full) ||
        !rr_directory(obj_full) || !rr_regular(rsp_full, NULL) ||
        !strstr(next.cflags, "-DZCL_DEV_BUILD")) {
        rr_why(why, why_len,
               "restart action plan incomplete or its object graph is absent");
        return false;
    }
    if (strstr(next.cflags, "-flto") || strstr(next.ldflags, "-flto") ||
        strstr(next.cflags, "-fuse-linker-plugin") ||
        strstr(next.ldflags, "-fuse-linker-plugin")) {
        rr_why(why, why_len,
               "restart action plan contains release-only LTO flags");
        return false;
    }
    (void)snprintf(next.root, sizeof(next.root), "%s", root);
    next.stamp = stamp;
    next.loaded = true;
    g_rr_plan = next;
    *out = next;
    *cache_hit = false;
    *elapsed_us = platform_time_monotonic_us() - started;
    return true;
}

static bool rr_sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct sha256_ctx sha;
    unsigned char digest[SHA256_OUTPUT_SIZE], buf[65536];
    sha256_init(&sha);
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_write(&sha, buf, n);
    bool ok = ferror(f) == 0;
    fclose(f);
    if (!ok) return false;
    sha256_finalize(&sha, digest);
    zcl_hex_encode(digest, sizeof(digest), out);
    return true;
}

static bool rr_temp(char out[PATH_MAX], const char *dir, const char *suffix)
{
    int n = snprintf(out, PATH_MAX, "%s/.restart-XXXXXX%s", dir, suffix);
    if (n <= 0 || n >= PATH_MAX) return false;
    int fd = mkstemps(out, (int)strlen(suffix));
    if (fd < 0) return false;
    close(fd);
    return true;
}

static bool rr_source_is_c(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return rr_safe_relative(path) && n > 2 && strcmp(path + n - 2, ".c") == 0;
}

static bool rr_compile_one(const struct rr_plan *plan, const char *root,
                           struct rr_overlay *overlay,
                           struct zcl_devloop_process_result *process,
                           int64_t *elapsed_us, char *why, size_t why_len)
{
    char source_full[PATH_MAX], before_hash[65], after_hash[65];
    if (!rr_join_root(root, overlay->source, source_full) ||
        !rr_regular(source_full, NULL) ||
        !rr_sha256_file(source_full, before_hash)) {
        rr_why(why, why_len, "restart source is absent or not a regular file");
        return false;
    }
    char overlay_dir[PATH_MAX];
    (void)snprintf(overlay_dir, sizeof(overlay_dir), "%s", overlay->overlay_object);
    char *slash = strrchr(overlay_dir, '/');
    if (!slash) return false;
    *slash = 0;
    if (!rr_mkdirs(overlay_dir)) {
        rr_why(why, why_len, "could not prepare confined restart object directory");
        return false;
    }
    char temp_o[PATH_MAX], temp_d[PATH_MAX];
    if (!rr_temp(temp_o, overlay_dir, ".o") ||
        !rr_temp(temp_d, overlay_dir, ".d")) {
        rr_why(why, why_len, "could not allocate restart compile temporaries");
        return false;
    }
    char cc[sizeof(plan->cc)], flags[sizeof(plan->cflags)];
    (void)snprintf(cc, sizeof(cc), "%s", plan->cc);
    (void)snprintf(flags, sizeof(flags), "%s", plan->cflags);
    const char *argv[RR_ARG_MAX], *flagv[RR_ARG_MAX];
    size_t argc = zcl_argv_split(cc, argv, RR_ARG_MAX);
    size_t flagc = zcl_argv_split(flags, flagv, RR_ARG_MAX);
    if (!argc || argc + flagc + 9 >= RR_ARG_MAX) {
        (void)unlink(temp_o); (void)unlink(temp_d);
        rr_why(why, why_len, "restart compile action exceeds argv bound");
        return false;
    }
    for (size_t i = 0; i < flagc; i++) argv[argc++] = flagv[i];
    argv[argc++] = "-MD"; argv[argc++] = "-MF"; argv[argc++] = temp_d;
    argv[argc++] = "-c"; argv[argc++] = "-o"; argv[argc++] = temp_o;
    argv[argc++] = overlay->source; argv[argc] = NULL;
    int64_t started = platform_time_monotonic_us();
    bool ran = zcl_devloop_process_run(root, argv, 30000, process);
    *elapsed_us += platform_time_monotonic_us() - started;
    bool ok = ran && !process->timed_out && !process->term_signal &&
              process->exit_code == 0 && rr_regular(temp_o, NULL) &&
              rr_regular(temp_d, NULL) && rr_sha256_file(source_full, after_hash) &&
              strcmp(before_hash, after_hash) == 0;
    if (!ok) {
        (void)unlink(temp_o); (void)unlink(temp_d);
        rr_why(why, why_len, ran ? "restart compile failed or source changed"
                                : "restart compiler could not be executed");
        return false;
    }
    char marker[PATH_MAX];
    if (snprintf(marker, sizeof(marker), "%s.source", overlay->overlay_object) >=
        (int)sizeof(marker)) {
        (void)unlink(temp_o); (void)unlink(temp_d);
        rr_why(why, why_len, "restart overlay marker path overflow");
        return false;
    }
    (void)unlink(marker);
    (void)unlink(overlay->overlay_object);
    if (rename(temp_o, overlay->overlay_object) != 0) {
        (void)unlink(temp_o); (void)unlink(temp_d);
        rr_why(why, why_len, "could not publish restart overlay object");
        return false;
    }
    (void)chmod(overlay->overlay_object, 0444);
    (void)snprintf(overlay->source_sha256,
                   sizeof(overlay->source_sha256), "%s", after_hash);
    char marker_temp[PATH_MAX];
    if (!rr_temp(marker_temp, overlay_dir, ".source") ) {
        (void)unlink(overlay->overlay_object); (void)unlink(temp_d);
        rr_why(why, why_len, "could not allocate restart overlay marker");
        return false;
    }
    FILE *marker_file = fopen(marker_temp, "w");
    bool marker_ok = marker_file &&
        fprintf(marker_file, "%s %s\n", plan->base_generation,
                after_hash) == 130 &&
        fflush(marker_file) == 0 && fsync(fileno(marker_file)) == 0;
    if (marker_file) fclose(marker_file);
    if (!marker_ok || rename(marker_temp, marker) != 0) {
        (void)unlink(marker_temp); (void)unlink(overlay->overlay_object);
        (void)unlink(temp_d);
        rr_why(why, why_len, "could not publish restart overlay marker");
        return false;
    }
    (void)chmod(marker, 0444);
    (void)unlink(temp_d);
    return true;
}

static bool rr_overlay_for_base(const struct rr_plan *plan, const char *root,
                                const char *base_object,
                                char overlay_relative[PATH_MAX])
{
    size_t prefix_len = strlen(plan->obj_dir);
    size_t token_len = strlen(base_object);
    if (strncmp(base_object, plan->obj_dir, prefix_len) != 0 ||
        base_object[prefix_len] != '/' || token_len < 3 ||
        strcmp(base_object + token_len - 2, ".o") != 0)
        return false;
    const char *stem = base_object + prefix_len + 1;
    char source[PATH_MAX], overlay[PATH_MAX], marker[PATH_MAX];
    if (snprintf(source, sizeof(source), "%s", stem) >= (int)sizeof(source))
        return false;
    size_t source_len = strlen(source);
    source[source_len - 1] = 'c';
    if (snprintf(overlay_relative, PATH_MAX,
                 "build/dev-loop/restart-objects/%s", stem) >= PATH_MAX ||
        snprintf(overlay, sizeof(overlay), "%s/%s", root,
                 overlay_relative) >= (int)sizeof(overlay) ||
        snprintf(marker, sizeof(marker), "%s.source", overlay) >=
            (int)sizeof(marker) || !rr_regular(overlay, NULL) ||
        !rr_regular(marker, NULL))
        return false;
    FILE *f = fopen(marker, "r");
    if (!f) return false;
    char generation[65] = {0}, expected[65] = {0}, extra = 0;
    bool parsed = fscanf(f, "%64[0-9a-f] %64[0-9a-f]%c", generation,
                         expected, &extra) == 3 && extra == '\n' &&
                  fgetc(f) == EOF;
    fclose(f);
    char source_full[PATH_MAX], actual[65];
    return parsed && strcmp(generation, plan->base_generation) == 0 &&
           rr_join_root(root, source, source_full) &&
           rr_regular(source_full, NULL) &&
           rr_sha256_file(source_full, actual) && strcmp(actual, expected) == 0;
}

static bool rr_write_response(const struct rr_plan *plan, const char *root,
                              const struct rr_overlay *overlays,
                              size_t overlay_count, char out[PATH_MAX],
                              char *why, size_t why_len)
{
    char rsp_full[PATH_MAX], dir[PATH_MAX];
    if (!rr_join_root(root, plan->link_rsp, rsp_full) ||
        snprintf(dir, sizeof(dir), "%s/build/dev-loop", root) >=
            (int)sizeof(dir) || !rr_mkdirs(dir) || !rr_temp(out, dir, ".rsp")) {
        rr_why(why, why_len, "could not prepare restart link response");
        return false;
    }
    FILE *in = fopen(rsp_full, "r"), *dst = fopen(out, "w");
    if (!in || !dst) {
        if (in) fclose(in);
        if (dst) fclose(dst);
        (void)unlink(out);
        rr_why(why, why_len, "could not open restart link response");
        return false;
    }
    bool seen[RR_SOURCE_MAX] = {0};
    char token[PATH_MAX];
    bool ok = true;
    while (ok && fscanf(in, "%4095s", token) == 1) {
        const char *write_path = token;
        char persistent_overlay[PATH_MAX];
        bool current = false;
        for (size_t i = 0; i < overlay_count; i++) {
            if (strcmp(token, overlays[i].base_object) == 0) {
                write_path = overlays[i].overlay_object;
                /* Linker cwd is root; retain a worktree-relative response. */
                size_t root_len = strlen(root);
                if (strncmp(write_path, root, root_len) == 0 &&
                    write_path[root_len] == '/')
                    write_path += root_len + 1;
                seen[i] = true;
                current = true;
                break;
            }
        }
        if (!current && rr_overlay_for_base(plan, root, token,
                                            persistent_overlay))
            write_path = persistent_overlay;
        ok = fprintf(dst, "%s\n", write_path) > 0;
    }
    ok = ok && !ferror(in) && fflush(dst) == 0 && fsync(fileno(dst)) == 0;
    fclose(in); fclose(dst);
    for (size_t i = 0; i < overlay_count; i++) ok = ok && seen[i];
    if (!ok) {
        (void)unlink(out);
        rr_why(why, why_len,
               "restart link response did not contain every changed object");
    }
    return ok;
}

static bool rr_link(const struct rr_plan *plan, const char *root,
                    const char *rsp, char binary[PATH_MAX],
                    struct zcl_devloop_process_result *process,
                    int64_t *elapsed_us, char *why, size_t why_len)
{
    char dir[PATH_MAX], temp[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/build/dev-loop", root) >=
            (int)sizeof(dir) || !rr_temp(temp, dir, ".bin")) {
        rr_why(why, why_len, "could not allocate restart binary temporary");
        return false;
    }
    char cc[sizeof(plan->cc)], flags[sizeof(plan->ldflags)];
    char libs[sizeof(plan->libs)], rsp_arg[PATH_MAX + 2];
    (void)snprintf(cc, sizeof(cc), "%s", plan->cc);
    (void)snprintf(flags, sizeof(flags), "%s", plan->ldflags);
    (void)snprintf(libs, sizeof(libs), "%s", plan->libs);
    (void)snprintf(rsp_arg, sizeof(rsp_arg), "@%s", rsp);
    const char *argv[RR_ARG_MAX], *flagv[RR_ARG_MAX], *libv[RR_ARG_MAX];
    size_t argc = zcl_argv_split(cc, argv, RR_ARG_MAX);
    size_t flagc = zcl_argv_split(flags, flagv, RR_ARG_MAX);
    size_t libc = zcl_argv_split(libs, libv, RR_ARG_MAX);
    if (!argc || argc + flagc + libc + 4 >= RR_ARG_MAX) {
        (void)unlink(temp);
        rr_why(why, why_len, "restart link action exceeds argv bound");
        return false;
    }
    for (size_t i = 0; i < flagc; i++) argv[argc++] = flagv[i];
    argv[argc++] = "-o"; argv[argc++] = temp; argv[argc++] = rsp_arg;
    for (size_t i = 0; i < libc; i++) argv[argc++] = libv[i];
    argv[argc] = NULL;
    int64_t started = platform_time_monotonic_us();
    bool ran = zcl_devloop_process_run(root, argv, 30000, process);
    *elapsed_us = platform_time_monotonic_us() - started;
    if (!ran || process->timed_out || process->term_signal ||
        process->exit_code != 0 || !rr_regular(temp, NULL)) {
        (void)unlink(temp);
        rr_why(why, why_len, "restart candidate link failed");
        return false;
    }
    char hash[65], candidates[PATH_MAX];
    if (!rr_sha256_file(temp, hash) ||
        snprintf(candidates, sizeof(candidates),
                 "%s/build/dev-loop/restart-candidates", root) >=
            (int)sizeof(candidates) || !rr_mkdirs(candidates) ||
        snprintf(binary, PATH_MAX, "%s/%s-zclassic23-dev", candidates,
                 hash) >= PATH_MAX) {
        (void)unlink(temp);
        rr_why(why, why_len, "could not address restart candidate");
        return false;
    }
    if (link(temp, binary) != 0) {
        char existing[65];
        if (errno != EEXIST || !rr_regular(binary, NULL) ||
            !rr_sha256_file(binary, existing) || strcmp(existing, hash) != 0) {
            (void)unlink(temp);
            rr_why(why, why_len, "restart candidate publication collision");
            return false;
        }
    }
    (void)chmod(binary, 0555);
    (void)unlink(temp);
    return true;
}

bool zcl_devloop_restart_build(
    const char *repo_root, const char *const *source_tus, size_t source_count,
    struct zcl_devloop_restart_build_receipt *receipt,
    struct zcl_devloop_process_result *process,
    char *why, size_t why_len)
{
    int64_t started = platform_time_monotonic_us();
    if (why && why_len) why[0] = 0;
    if (!repo_root || !source_tus || !receipt || !process ||
        source_count == 0 || source_count > RR_SOURCE_MAX) {
        rr_why(why, why_len, "restart source set is empty or exceeds its bound");
        return false;
    }
    memset(receipt, 0, sizeof(*receipt));
    memset(process, 0, sizeof(*process));
    struct zcl_devloop_plan classification;
    if (!zcl_devloop_plan_files(source_tus, source_count, &classification)) {
        rr_why(why, why_len, "restart source set is invalid");
        return false;
    }
    if (classification.consensus_risk) {
        rr_why(why, why_len,
               "consensus-risk input is excluded from fast restart");
        return false;
    }
    for (size_t i = 0; i < source_count; i++) {
        if (!rr_source_is_c(source_tus[i])) {
            rr_why(why, why_len,
                   "fast restart currently requires changed C translation units");
            return false;
        }
    }
    char root[PATH_MAX];
    if (!realpath(repo_root, root)) {
        rr_why(why, why_len, "restart checkout root could not be resolved");
        return false;
    }
    struct rr_plan plan = {0};
    pthread_mutex_lock(&g_rr_mu);
    bool plan_ok = rr_plan_load_locked(root, &plan, &receipt->plan_cache_hit,
                                       &receipt->plan_load_us, why, why_len);
    pthread_mutex_unlock(&g_rr_mu);
    if (!plan_ok) return false;

    struct dev_source_record source_before = {0}, source_after = {0};
    if (!zcl_dev_source_cas_capture(root, &source_before) ||
        !source_before.cas_present) {
        rr_why(why, why_len,
               "restart source snapshot could not be captured before compile");
        return false;
    }

    struct rr_overlay *overlays = zcl_calloc(source_count, sizeof(*overlays),
                                              "restart overlays");
    if (!overlays) {
        rr_why(why, why_len, "restart overlay allocation failed");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < source_count; i++) {
        size_t n = strlen(source_tus[i]);
        (void)snprintf(overlays[i].source, sizeof(overlays[i].source), "%s",
                       source_tus[i]);
        char stem[ZCL_DEVLOOP_PATH_MAX];
        (void)snprintf(stem, sizeof(stem), "%s", source_tus[i]);
        stem[n - 1] = 'o';
        if (snprintf(overlays[i].base_object,
                     sizeof(overlays[i].base_object), "%s/%s", plan.obj_dir,
                     stem) >= (int)sizeof(overlays[i].base_object) ||
            snprintf(overlays[i].overlay_object,
                     sizeof(overlays[i].overlay_object),
                     "%s/build/dev-loop/restart-objects/%s", root, stem) >=
                (int)sizeof(overlays[i].overlay_object)) {
            rr_why(why, why_len, "restart object path overflow");
            ok = false;
            break;
        }
        receipt->compiler_processes++;
        if (!rr_compile_one(&plan, root, &overlays[i], process,
                            &receipt->compile_us, why, why_len)) {
            ok = false;
            break;
        }
    }
    char rsp[PATH_MAX] = {0};
    if (ok && !rr_write_response(&plan, root, overlays, source_count, rsp,
                                 why, why_len))
        ok = false;
    if (ok) {
        receipt->linker_processes = 1;
        ok = rr_link(&plan, root, rsp, receipt->artifact_path, process,
                     &receipt->link_us, why, why_len);
    }
    if (rsp[0]) (void)unlink(rsp);
    free(overlays);
    if (!ok) {
        receipt->total_us = platform_time_monotonic_us() - started;
        return false;
    }
    if (!zcl_dev_source_cas_capture(root, &source_after) ||
        !source_after.cas_present ||
        strcmp(source_before.cas_root_sha3, source_after.cas_root_sha3) != 0) {
        rr_why(why, why_len,
               "restart source snapshot changed during candidate build");
        receipt->total_us = platform_time_monotonic_us() - started;
        return false;
    }
    if (!rr_sha256_file(receipt->artifact_path, receipt->artifact_sha256)) {
        rr_why(why, why_len, "restart candidate could not be rehashed");
        return false;
    }
    const char *probe_argv[] = {
        receipt->artifact_path, "discover", "help", NULL
    };
    receipt->probe_processes = 1;
    int64_t probe_started = platform_time_monotonic_us();
    bool probed = zcl_devloop_process_run(root, probe_argv, 5000, process);
    receipt->probe_us = platform_time_monotonic_us() - probe_started;
    receipt->candidate_probe_passed = probed && !process->timed_out &&
        !process->term_signal && process->exit_code == 0;
    (void)snprintf(receipt->probe, sizeof(receipt->probe), "%s",
                   "discover.help");
    receipt->changed_sources = (uint32_t)source_count;
    receipt->total_us = platform_time_monotonic_us() - started;
    if (!receipt->candidate_probe_passed) {
        rr_why(why, why_len, "restart candidate command-runtime probe failed");
        return false;
    }
    return true;
}

static void rr_output_preview(const struct zcl_devloop_process_result *process,
                              char out[1025])
{
    size_t n = process ? process->output_len : 0;
    if (n > 1024) n = 1024;
    if (n) memcpy(out, process->output + process->output_len - n, n);
    out[n] = 0;
}

static bool rr_emit_event(
    const char *root, const char *const *sources, size_t source_count,
    const char *status, const char *phase, int64_t elapsed_us,
    enum zcl_devloop_publish_mode requested_mode,
    const struct zcl_devloop_restart_build_receipt *build,
    const struct zcl_devloop_process_result *process, const char *why)
{
    struct json_value doc, files, receipt;
    json_init(&doc); json_set_object(&doc);
    (void)json_push_kv_str(&doc, "schema", "zcl.dev_cycle.v1");
    (void)json_push_kv_str(&doc, "producer", "resident-restart-authority");
    (void)json_push_kv_str(&doc, "status", status);
    (void)json_push_kv_str(&doc, "action", "restart");
    (void)json_push_kv_str(&doc, "reason", "process_restart_candidate");
    (void)json_push_kv_str(&doc, "phase", phase);
    (void)json_push_kv_bool(&doc, "runtime_published", false);
    (void)json_push_kv_bool(&doc, "publication_requested",
                            zcl_devloop_publish_mode_applies(requested_mode));
    (void)json_push_kv_bool(&doc, "proof_complete", false);
    (void)json_push_kv_int(&doc, "elapsed_us", elapsed_us);
    (void)json_push_kv_int(&doc, "elapsed_ms", elapsed_us / 1000);
    (void)json_push_kv_int(&doc, "file_count", (int64_t)source_count);
    json_init(&files); json_set_array(&files);
    for (size_t i = 0; i < source_count; i++) {
        struct json_value item;
        json_init(&item); json_set_str(&item, sources[i]);
        (void)json_push_back(&files, &item); json_free(&item);
    }
    (void)json_push_kv(&doc, "files", &files); json_free(&files);
    if (why && why[0])
        (void)json_push_kv_str(&doc, "failure_capsule", why);
    if (process && process->output_len) {
        char preview[1025];
        rr_output_preview(process, preview);
        (void)json_push_kv_str(&doc, "process_output", preview);
        (void)json_push_kv_bool(&doc, "process_output_truncated",
                                process->output_len > 1024 ||
                                process->output_truncated);
    }
    if (build) {
        json_init(&receipt); json_set_object(&receipt);
        (void)json_push_kv_str(&receipt, "schema",
                               "zcl.dev_restart_build_receipt.v1");
        if (build->artifact_path[0])
            (void)json_push_kv_str(&receipt, "artifact_path",
                                   build->artifact_path);
        if (build->artifact_sha256[0])
            (void)json_push_kv_str(&receipt, "artifact_sha256",
                                   build->artifact_sha256);
        (void)json_push_kv_str(&receipt, "probe", build->probe);
        (void)json_push_kv_bool(&receipt, "candidate_probe_passed",
                                build->candidate_probe_passed);
        (void)json_push_kv_bool(&receipt, "plan_cache_hit",
                                build->plan_cache_hit);
        (void)json_push_kv_int(&receipt, "changed_sources",
                               build->changed_sources);
        (void)json_push_kv_int(&receipt, "compiler_processes",
                               build->compiler_processes);
        (void)json_push_kv_int(&receipt, "linker_processes",
                               build->linker_processes);
        (void)json_push_kv_int(&receipt, "probe_processes",
                               build->probe_processes);
        (void)json_push_kv_int(&receipt, "plan_load_us",
                               build->plan_load_us);
        (void)json_push_kv_int(&receipt, "compile_us", build->compile_us);
        (void)json_push_kv_int(&receipt, "link_us", build->link_us);
        (void)json_push_kv_int(&receipt, "probe_us", build->probe_us);
        (void)json_push_kv_int(&receipt, "build_total_us", build->total_us);
        (void)json_push_kv(&doc, "build_receipt", &receipt);
        json_free(&receipt);
    }
    (void)json_push_kv_str(
        &doc, "agent_next_action",
        strcmp(status, "candidate_ready") == 0
            ? "candidate runtime is healthy; resident mapped proofs must pass before acceptance"
            : "repair the named restart refusal; no service or source was replaced");
    char wire[16384];
    size_t n = json_write(&doc, wire, sizeof(wire) - 1);
    json_free(&doc);
    if (!n) return false;
    wire[n++] = '\n'; wire[n] = 0;
    char state_why[160] = {0};
    if (!zcl_devloop_cycle_state_write(root, wire, n, state_why,
                                       sizeof(state_why))) {
        fprintf(stderr, "[devloop] restart receipt persistence failed: %s\n",
                state_why[0] ? state_why : "unknown");
        return false;
    }
    (void)fwrite(wire, 1, n, stdout); (void)fflush(stdout);
    return true;
}

int zcl_devloop_restart_event(const char *repo_root,
                              const char *const *source_tus,
                              size_t source_count,
                              enum zcl_devloop_publish_mode publish_mode)
{
    if (!repo_root || !source_tus || source_count == 0 ||
        source_count > RR_SOURCE_MAX)
        return 0;
    struct zcl_devloop_plan plan;
    if (!zcl_devloop_plan_files(source_tus, source_count, &plan) ||
        plan.docs_only || plan.consensus_risk)
        return 0;
    for (size_t i = 0; i < source_count; i++)
        if (!rr_source_is_c(source_tus[i]))
            return 0;
    int64_t started = platform_time_monotonic_us();
    struct zcl_devloop_restart_build_receipt build = {0};
    struct zcl_devloop_process_result process = {0};
    char why[512] = {0};
    bool ok = zcl_devloop_restart_build(repo_root, source_tus, source_count,
                                        &build, &process, why, sizeof(why));
    bool emitted = rr_emit_event(
        repo_root, source_tus, source_count,
        ok ? "candidate_ready" : "rejected",
        ok ? "command_runtime_probe" : "compile_link_probe",
        platform_time_monotonic_us() - started, publish_mode, &build,
        &process, why);
    return emitted ? 1 : -1;
}
