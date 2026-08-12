/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Resident hot-swap build authority. The persistent zclassic23-dev watcher
 * calls this directly after inotify classifies one allowlisted translation
 * unit. Make writes the action plan only when build flags/toolchain change;
 * the edit path parses no Makefile and starts no shell or CLI. Stock GCC/Clang
 * still run as bounded children: the compiler is the irreducible native-code
 * work, not orchestration.
 */

#define _GNU_SOURCE
#include "devloop.h"

#include "base/hex.h"
#include "crypto/sha256.h"
#include "controllers/rpc_client.h"
#include "command/native_dev_hotswap.h"
#include "hotswap/hotswap_module.h"
#include "hotswap/hotswap_service.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/safe_alloc.h"
#include "util/spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HS_PLAN_TEXT_MAX 12288
#define HS_ARG_MAX 256
#define HS_DEP_MAX 512

struct hs_action_plan {
    char root[PATH_MAX];
    char cc[512];
    char compiler_id[65];
    char cflags[HS_PLAN_TEXT_MAX];
    char ldflags[2048];
    struct stat stamp;
    bool loaded;
};

struct hs_dep {
    char path[PATH_MAX];
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
    unsigned char sha256[SHA256_OUTPUT_SIZE];
};

static pthread_mutex_t g_plan_mu = PTHREAD_MUTEX_INITIALIZER;
static struct hs_action_plan g_plan;

static void hs_why(char *why, size_t why_len, const char *message)
{
    if (why && why_len)
        (void)snprintf(why, why_len, "%s", message ? message : "unknown");
}

void zcl_devloop_hotswap_guidance(
    const char *status, const char *phase, const char *why,
    char *why_not_live, size_t why_not_live_size,
    char *next_command, size_t next_command_size)
{
    bool passed = status && strcmp(status, "passed") == 0;
    bool compile_green = status && strcmp(status, "reflex_ready") == 0;
    bool story_green = status && strcmp(status, "story_green") == 0;
    if (why_not_live && why_not_live_size) {
        const char *exact = passed ? "" : why;
        if (story_green || compile_green)
            exact = "HOT_SHADOW is proposal-only and never publishes runtime authority";
        else if (!passed && (!exact || !exact[0])) {
            exact = phase && strcmp(phase, "compile") == 0
                ? "candidate compilation did not produce a publishable artifact"
                : "resident dev node did not publish the candidate";
        }
        (void)snprintf(why_not_live, why_not_live_size, "%s", exact);
    }
    if (!next_command || next_command_size == 0) return;
    const char *next =
        "zclassic23-dev dev status --view=full";
    if (story_green) {
        next = "keep editing; exact affected proof is running asynchronously";
    } else if (compile_green) {
        next = "keep editing; the owner-bound shadow story is running";
    } else if (passed) {
        next = "keep editing; the resident authority owns the next module epoch";
    } else if ((why && strstr(why, "DEV_RESTART")) ||
               (why && strstr(why, "service ABI changed")) ||
               (why && strstr(why, "service schema changed")) ||
               (why && strstr(why, "service wire contract changed")) ||
               (why && strstr(why, "frozen KAT identity changed"))) {
        next = "make -j\"$(nproc)\" dev-bin";
    } else if (phase && strcmp(phase, "compile") == 0) {
        next = "zclassic23-dev dev diagnose latest";
    } else if ((why && strstr(why, "cannot read RPC auth cookie")) ||
               (why && strstr(why, "returned no activation body"))) {
        next = "zclassic23-dev dev generation current";
    }
    (void)snprintf(next_command, next_command_size, "%s", next);
}

bool zcl_devloop_hotswap_response_error(
    const struct json_value *response, char *out, size_t out_size)
{
    if (!response || response->type != JSON_OBJ || !out || out_size == 0)
        return false;
    const struct json_value *message_v = json_get(response, "message");
    const struct json_value *error_v = json_get(response, "error");
    if ((!message_v || message_v->type != JSON_STR) && error_v &&
        error_v->type == JSON_OBJ)
        message_v = json_get(error_v, "message");
    if ((!message_v || message_v->type != JSON_STR) && error_v &&
        error_v->type == JSON_STR)
        message_v = error_v;
    const char *message = message_v && message_v->type == JSON_STR
        ? json_get_str(message_v) : NULL;
    if (!message || !message[0]) return false;
    (void)snprintf(out, out_size, "%s", message);
    return true;
}

static bool hs_regular(const char *path, struct stat *out)
{
    struct stat st;
    if (!path || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        S_ISLNK(st.st_mode))
        return false;
    if (out)
        *out = st;
    return true;
}

static bool hs_stat_equal(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

static bool hs_plan_line(char *dst, size_t cap, const char *line,
                         const char *prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(line, prefix, n) != 0)
        return false;
    const char *value = line + n;
    size_t len = strcspn(value, "\r\n");
    if (len == 0 || len >= cap)
        return false;
    memcpy(dst, value, len);
    dst[len] = 0;
    return true;
}

static bool hs_lower_hex64(const char *value)
{
    if (!value || strlen(value) != 64)
        return false;
    for (size_t i = 0; i < 64; i++)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    return true;
}

static bool hs_plan_load_locked(const char *root, bool *cache_hit,
                                int64_t *elapsed_us, char *why,
                                size_t why_len)
{
    int64_t started = platform_time_monotonic_us();
    char flags_path[PATH_MAX], makefile[PATH_MAX], manifest[PATH_MAX];
    char islands[PATH_MAX], services[PATH_MAX];
    if (snprintf(flags_path, sizeof(flags_path),
                 "%s/build/hotswap/fast/flags.env", root) >=
            (int)sizeof(flags_path) ||
        snprintf(makefile, sizeof(makefile), "%s/Makefile", root) >=
            (int)sizeof(makefile) ||
        snprintf(manifest, sizeof(manifest),
                 "%s/config/hotswap_swappable.def", root) >=
            (int)sizeof(manifest) ||
        snprintf(islands, sizeof(islands),
                 "%s/config/hotswap_islands.def", root) >=
            (int)sizeof(islands) ||
        snprintf(services, sizeof(services),
                 "%s/config/hotswap_services.def", root) >=
            (int)sizeof(services)) {
        hs_why(why, why_len, "action plan path overflow");
        return false;
    }
    struct stat stamp, make_st, manifest_st, islands_st, services_st;
    if (!hs_regular(flags_path, &stamp)) {
        hs_why(why, why_len,
               "resident action plan absent; run make dev-bin once");
        return false;
    }
    if (!hs_regular(makefile, &make_st) || !hs_regular(manifest, &manifest_st) ||
        !hs_regular(islands, &islands_st) ||
        !hs_regular(services, &services_st) ||
        make_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (make_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         make_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec) ||
        manifest_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (manifest_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         manifest_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec) ||
        islands_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (islands_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         islands_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec) ||
        services_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (services_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         services_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec)) {
        hs_why(why, why_len,
               "resident action plan stale; refresh after build-system change");
        return false;
    }
    if (g_plan.loaded && strcmp(g_plan.root, root) == 0 &&
        hs_stat_equal(&g_plan.stamp, &stamp)) {
        *cache_hit = true;
        *elapsed_us = platform_time_monotonic_us() - started;
        return true;
    }

    FILE *f = fopen(flags_path, "r");
    if (!f) {
        hs_why(why, why_len, "resident action plan could not be opened");
        return false;
    }
    struct hs_action_plan next = {0};
    char line[HS_PLAN_TEXT_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (hs_plan_line(next.cc, sizeof(next.cc), line, "CC=") ||
            hs_plan_line(next.compiler_id, sizeof(next.compiler_id), line,
                         "COMPILER_ID=") ||
            hs_plan_line(next.cflags, sizeof(next.cflags), line,
                         "DEV_CFLAGS=") ||
            hs_plan_line(next.ldflags, sizeof(next.ldflags), line,
                         "HOTSWAP_MODULE_LDFLAGS="))
            continue;
        fclose(f);
        hs_why(why, why_len, "resident action plan has an unknown field");
        return false;
    }
    bool read_error = ferror(f) != 0;
    fclose(f);
    if (read_error || !next.cc[0] || !hs_lower_hex64(next.compiler_id) ||
        !next.cflags[0] || !next.ldflags[0] ||
        !strstr(next.cflags, "-DZCL_DEV_BUILD") ||
        !strstr(next.ldflags, "-Wl,-Bsymbolic")) {
        hs_why(why, why_len,
               "resident action plan incomplete or missing safety flags");
        return false;
    }
    if (strstr(next.cflags, "-flto") || strstr(next.ldflags, "-flto") ||
        strstr(next.cflags, "-fuse-linker-plugin") ||
        strstr(next.ldflags, "-fuse-linker-plugin")) {
        hs_why(why, why_len,
               "resident action plan contains release-only LTO flags");
        return false;
    }
    (void)snprintf(next.root, sizeof(next.root), "%s", root);
    next.stamp = stamp;
    next.loaded = true;
    g_plan = next;
    *cache_hit = false;
    *elapsed_us = platform_time_monotonic_us() - started;
    return true;
}

static bool hs_sha256_digest_file(const char *path,
                                  unsigned char out[SHA256_OUTPUT_SIZE])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_write(&ctx, buf, n);
    bool ok = ferror(f) == 0;
    fclose(f);
    if (!ok)
        return false;
    sha256_finalize(&ctx, out);
    return true;
}

static bool hs_depfile_read(const char *root, const char *path,
                            struct hs_dep *deps, size_t *count,
                            bool snapshot)
{
    *count = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char text[65536];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    bool ok = !ferror(f) && !feof(f) ? false : true;
    fclose(f);
    if (!ok || n == 0 || n >= sizeof(text))
        return false;
    text[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (text[i] == '\\' && (text[i + 1] == '\n' || text[i + 1] == '\r'))
            text[i] = text[i + 1] = ' ';
    char *colon = strchr(text, ':');
    if (!colon)
        return false;
    const char *argv[HS_DEP_MAX + 1];
    size_t argc = zcl_argv_split(colon + 1, argv, HS_DEP_MAX + 1);
    if (argc == 0 || argc >= HS_DEP_MAX)
        return false;
    for (size_t i = 0; i < argc; i++) {
        char full[PATH_MAX];
        int pn = argv[i][0] == '/'
            ? snprintf(full, sizeof(full), "%s", argv[i])
            : snprintf(full, sizeof(full), "%s/%s", root, argv[i]);
        struct stat st;
        if (pn <= 0 || pn >= (int)sizeof(full) || !hs_regular(full, &st))
            return false;
        if (strstr(full, "/build/hotswap/fast/.resident-") != NULL)
            continue; /* generated unity wrapper, never source authority */
        struct hs_dep *d = &deps[(*count)++];
        (void)snprintf(d->path, sizeof(d->path), "%s", full);
        if (snapshot) {
            d->dev = st.st_dev;
            d->ino = st.st_ino;
            d->size = st.st_size;
            d->mtime = st.st_mtim;
            if (!hs_sha256_digest_file(full, d->sha256))
                return false;
        }
    }
    return true;
}

static const struct hs_dep *hs_dep_find(const struct hs_dep *deps,
                                        size_t count, const char *path)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(deps[i].path, path) == 0)
            return &deps[i];
    return NULL;
}

static bool hs_deps_unchanged(const struct hs_dep *before, size_t before_n,
                              const struct hs_dep *after, size_t after_n,
                              char *why, size_t why_len)
{
    for (size_t i = 0; i < after_n; i++) {
        const struct hs_dep *old = hs_dep_find(before, before_n, after[i].path);
        if (!old) {
            if (why && why_len)
                (void)snprintf(why, why_len,
                               "dependency baseline learned new input: %.180s",
                               after[i].path);
            return false;
        }
        if (old->dev != after[i].dev || old->ino != after[i].ino ||
            old->size != after[i].size ||
            old->mtime.tv_sec != after[i].mtime.tv_sec ||
            old->mtime.tv_nsec != after[i].mtime.tv_nsec ||
            memcmp(old->sha256, after[i].sha256, SHA256_OUTPUT_SIZE) != 0) {
            if (why && why_len)
                (void)snprintf(why, why_len,
                               "input mutated during resident build: %.190s",
                               after[i].path);
            return false;
        }
    }
    return true;
}

static bool hs_sha256_file(const char *path, char out[65])
{
    unsigned char digest[32];
    if (!hs_sha256_digest_file(path, digest))
        return false;
    zcl_hex_encode(digest, sizeof(digest), out);
    return true;
}

static bool hs_mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    struct stat st;
    if (!path || path[0] != '/' || strlen(path) >= sizeof(tmp))
        return false;
    (void)snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0700) != 0) {
            if (errno != EEXIST || lstat(tmp, &st) != 0 ||
                !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
                return false;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 &&
        (errno != EEXIST || lstat(tmp, &st) != 0 ||
         !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)))
        return false;
    return chmod(tmp, 0700) == 0;
}

static bool hs_cache_root(char out[PATH_MAX])
{
    const char *configured = getenv("ZCL_DEV_ARTIFACT_CACHE");
    const char *home = getenv("HOME");
    int n;
    if (configured && configured[0]) {
        if (configured[0] != '/' || strstr(configured, ".."))
            return false;
        n = snprintf(out, PATH_MAX, "%s/hotswap-v1", configured);
    } else {
        if (!home || home[0] != '/')
            return false;
        n = snprintf(out, PATH_MAX,
                     "%s/.cache/zclassic23/dev-artifacts/hotswap-v1", home);
    }
    return n > 0 && n < PATH_MAX && hs_mkdirs(out);
}

static void hs_key_field(struct sha256_ctx *ctx, const char *label,
                         const void *data, size_t len)
{
    uint64_t n = (uint64_t)len;
    sha256_write(ctx, (const unsigned char *)label, strlen(label) + 1);
    sha256_write(ctx, (const unsigned char *)&n, sizeof(n));
    if (len)
        sha256_write(ctx, data, len);
}

static bool hs_normalize_root(const char *text, const char *root,
                              char *out, size_t out_len)
{
    static const char marker[] = "${WORKTREE}";
    size_t root_len = strlen(root), used = 0;
    const char *cursor = text;
    while (*cursor) {
        const char *match = strstr(cursor, root);
        size_t chunk = match ? (size_t)(match - cursor) : strlen(cursor);
        if (chunk >= out_len - used)
            return false;
        memcpy(out + used, cursor, chunk);
        used += chunk;
        if (!match)
            break;
        if (sizeof(marker) - 1 >= out_len - used)
            return false;
        memcpy(out + used, marker, sizeof(marker) - 1);
        used += sizeof(marker) - 1;
        cursor = match + root_len;
    }
    out[used] = 0;
    return true;
}

static bool hs_cache_key(const struct hs_action_plan *plan,
                         const char *root, const char *owner,
                         const struct hs_dep *deps, size_t dep_count,
                         char out[65])
{
    static const char domain[] = "zcl.dev_artifact_cache.hotswap.v1";
    char normalized_cflags[HS_PLAN_TEXT_MAX];
    if (!hs_normalize_root(plan->cflags, root, normalized_cflags,
                           sizeof(normalized_cflags)))
        return false;
    struct sha256_ctx ctx;
    unsigned char digest[SHA256_OUTPUT_SIZE];
    sha256_init(&ctx);
    hs_key_field(&ctx, "domain", domain, sizeof(domain) - 1);
    hs_key_field(&ctx, "compiler", plan->compiler_id,
                 strlen(plan->compiler_id));
    hs_key_field(&ctx, "cc", plan->cc, strlen(plan->cc));
    hs_key_field(&ctx, "cflags", normalized_cflags,
                 strlen(normalized_cflags));
    hs_key_field(&ctx, "ldflags", plan->ldflags, strlen(plan->ldflags));
    hs_key_field(&ctx, "owner", owner, strlen(owner));
    for (size_t i = 0; i < dep_count; i++) {
        const char *path = deps[i].path;
        size_t root_len = strlen(root);
        if (strncmp(path, root, root_len) == 0 && path[root_len] == '/')
            path += root_len + 1;
        hs_key_field(&ctx, "dependency_path", path, strlen(path));
        hs_key_field(&ctx, "dependency_sha256", deps[i].sha256,
                     sizeof(deps[i].sha256));
    }
    sha256_finalize(&ctx, digest);
    zcl_hex_encode(digest, sizeof(digest), out);
    return true;
}

static int hs_cache_lock(const char *cache_root, const char key[65],
                         char so_path[PATH_MAX], char hash_path[PATH_MAX])
{
    char lock_path[PATH_MAX];
    if (snprintf(lock_path, sizeof(lock_path), "%s/%s.lock", cache_root,
                 key) >= (int)sizeof(lock_path) ||
        snprintf(so_path, PATH_MAX, "%s/%s.so", cache_root, key) >= PATH_MAX ||
        snprintf(hash_path, PATH_MAX, "%s/%s.sha256", cache_root, key) >=
            PATH_MAX)
        return -1;
    int fd = open(lock_path,
                  O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        flock(fd, LOCK_EX) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static bool hs_read_hash(const char *path, char out[65])
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char extra = 0;
    bool ok = fscanf(f, "%64[0-9a-f]%c", out, &extra) == 2 &&
              strlen(out) == 64 && extra == '\n' && fgetc(f) == EOF;
    fclose(f);
    if (!ok) out[0] = 0;
    return ok;
}

static bool hs_force_cache_copy_for_test(void)
{
    const char *test_process = getenv("ZCL_DEVLOOP_TEST_PROCESS");
    const char *force_copy = getenv("ZCL_DEVLOOP_TEST_FORCE_CACHE_COPY");
    return test_process && strcmp(test_process, "1") == 0 && force_copy &&
           strcmp(force_copy, "1") == 0;
}

static bool hs_copy_publish(const char *source, const char *target,
                            const char expected_sha256[65])
{
    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", target);
    if (n <= 0 || n >= (int)sizeof(temp))
        return false;
    int source_fd = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat source_st;
    if (source_fd < 0 || fstat(source_fd, &source_st) != 0 ||
        !S_ISREG(source_st.st_mode)) {
        if (source_fd >= 0) close(source_fd);
        return false;
    }
    int temp_fd = mkostemp(temp, O_CLOEXEC);
    if (temp_fd < 0) {
        close(source_fd);
        return false;
    }
    unsigned char buffer[32u * 1024u];
    bool ok = true;
    for (;;) {
        ssize_t got = read(source_fd, buffer, sizeof(buffer));
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        size_t written = 0;
        while (written < (size_t)got) {
            ssize_t put = write(temp_fd, buffer + written,
                                (size_t)got - written);
            if (put < 0 && errno == EINTR)
                continue;
            if (put <= 0) {
                ok = false;
                break;
            }
            written += (size_t)put;
        }
        if (!ok) break;
    }
    if (close(source_fd) != 0)
        ok = false;
    if (ok && (fchmod(temp_fd, 0444) != 0 || fsync(temp_fd) != 0))
        ok = false;
    if (close(temp_fd) != 0)
        ok = false;
    char actual[65];
    if (ok && (!hs_sha256_file(temp, actual) ||
               strcmp(actual, expected_sha256) != 0))
        ok = false;
    if (ok && link(temp, target) != 0) {
        if (errno != EEXIST || !hs_regular(target, NULL) ||
            !hs_sha256_file(target, actual) ||
            strcmp(actual, expected_sha256) != 0 ||
            chmod(target, 0444) != 0)
            ok = false;
    }
    (void)unlink(temp);
    return ok;
}

static bool hs_link_or_copy_publish(const char *source, const char *target,
                                    const char expected_sha256[65])
{
    if (!hs_force_cache_copy_for_test() && link(source, target) == 0)
        return chmod(target, 0444) == 0;
    int link_errno = hs_force_cache_copy_for_test() ? EXDEV : errno;
    if (link_errno == EEXIST) {
        char actual[65];
        return hs_regular(target, NULL) && hs_sha256_file(target, actual) &&
               strcmp(actual, expected_sha256) == 0 &&
               chmod(target, 0444) == 0;
    }
    if (link_errno != EXDEV)
        return false;
    return hs_copy_publish(source, target, expected_sha256);
}

static bool hs_publish_artifact_path(const char *root, const char *safe,
                                     const char *source_so,
                                     const char artifact_sha256[65],
                                     char out[4096])
{
    if (snprintf(out, 4096, "%s/build/hotswap/%s-%s.so", root, safe,
                 artifact_sha256) >= 4096)
        return false;
    return hs_link_or_copy_publish(source_so, out, artifact_sha256);
}

static bool hs_cache_lookup(const char *root, const char *safe,
                            const char *cache_so, const char *cache_hash,
                            struct zcl_devloop_hotswap_build_receipt *receipt)
{
    char expected[65], actual[65];
    if (!hs_regular(cache_hash, NULL) || !hs_regular(cache_so, NULL) ||
        !hs_read_hash(cache_hash, expected) ||
        !hs_sha256_file(cache_so, actual) || strcmp(expected, actual) != 0)
        return false;
    (void)snprintf(receipt->artifact_sha256,
                   sizeof(receipt->artifact_sha256), "%s", actual);
    return hs_publish_artifact_path(root, safe, cache_so, actual,
                                    receipt->artifact_path);
}

static bool hs_cache_publish(const char *cache_so, const char *cache_hash,
                             const char *built_so, const char hash[65])
{
    if (!hs_link_or_copy_publish(built_so, cache_so, hash))
        return false;
    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", cache_hash,
                     (long)getpid());
    if (n <= 0 || n >= (int)sizeof(temp))
        return false;
    (void)unlink(temp); /* safe under the per-key lock; clears a crashed writer */
    int fd = open(temp,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0)
        return false;
    char line[66];
    (void)snprintf(line, sizeof(line), "%s\n", hash);
    bool ok = write(fd, line, 65) == 65 && fsync(fd) == 0;
    int close_rc = close(fd);
    ok = ok && close_rc == 0;
    if (!ok) {
        (void)unlink(temp);
        return false;
    }
    if (rename(temp, cache_hash) != 0) {
        (void)unlink(temp);
        return false;
    }
    return true;
}

static bool hs_temp(char *out, size_t out_len, const char *root,
                    const char *suffix)
{
    int n = snprintf(out, out_len,
                     "%s/build/hotswap/fast/.resident-XXXXXX%s", root,
                     suffix);
    if (n <= 0 || (size_t)n >= out_len)
        return false;
    int fd = mkstemps(out, (int)strlen(suffix));
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static bool hs_run_compile(const struct hs_action_plan *plan,
                           const char *root, const char *source_tu,
                           const char *compile_input,
                           const char *obj, const char *dep,
                           struct zcl_devloop_process_result *result,
                           int64_t *elapsed_us, char *why, size_t why_len)
{
    char cc[sizeof(plan->cc)], flags[sizeof(plan->cflags)];
    (void)snprintf(cc, sizeof(cc), "%s", plan->cc);
    (void)snprintf(flags, sizeof(flags), "%s", plan->cflags);
    const char *argv[HS_ARG_MAX];
    size_t argc = zcl_argv_split(cc, argv, HS_ARG_MAX);
    const char *flagv[HS_ARG_MAX];
    size_t flagc = zcl_argv_split(flags, flagv, HS_ARG_MAX);
    if (!argc || argc + flagc + 14 >= HS_ARG_MAX) {
        hs_why(why, why_len, "resident compile action exceeds argv bound");
        return false;
    }
    for (size_t i = 0; i < flagc; i++)
        argv[argc++] = flagv[i];
    char source_define[320];
    char service_source_define[320];
    (void)snprintf(source_define, sizeof(source_define),
                   "-DZCL_HOTSWAP_MODULE_SOURCE_TU=\"%s\"", source_tu);
    (void)snprintf(service_source_define, sizeof(service_source_define),
                   "-DZCL_HOTSWAP_SERVICE_SOURCE_TU=\"%s\"", source_tu);
    argv[argc++] = "-fPIC";
    argv[argc++] = "-DZCL_HOTSWAP_MODULE_GEN";
    argv[argc++] = "-DZCL_HOTSWAP_SERVICE_GEN";
    argv[argc++] = source_define;
    argv[argc++] = service_source_define;
    argv[argc++] = "-MD";
    argv[argc++] = "-MF";
    argv[argc++] = dep;
    argv[argc++] = "-c";
    argv[argc++] = "-o";
    argv[argc++] = obj;
    argv[argc++] = compile_input;
    argv[argc] = NULL;
    int64_t started = platform_time_monotonic_us();
    bool ran = zcl_devloop_process_run(root, argv, 30000, result);
    *elapsed_us = platform_time_monotonic_us() - started;
    if (!ran || result->timed_out || result->term_signal ||
        result->exit_code != 0) {
        hs_why(why, why_len, "resident module compile failed");
        return false;
    }
    return true;
}

static bool hs_files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return false;
    }
    bool same = true;
    unsigned char ba[4096], bb[4096];
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || (na && memcmp(ba, bb, na) != 0)) {
            same = false;
            break;
        }
        if (na < sizeof(ba)) {
            same = !ferror(fa) && !ferror(fb);
            break;
        }
    }
    fclose(fa);
    fclose(fb);
    return same;
}

static bool hs_unity_source(const char *root, const char *owner,
                            const char *members, const char *safe,
                            char out[PATH_MAX],
                            char *why, size_t why_len)
{
    out[0] = 0;
    if (!members || !members[0])
        return true;
    char temp[PATH_MAX];
    if (!hs_temp(temp, sizeof(temp), root, ".c") ||
        snprintf(out, PATH_MAX, "%s/build/hotswap/fast/%s.island.c",
                 root, safe) >= PATH_MAX) {
        hs_why(why, why_len, "could not allocate confined island wrapper");
        return false;
    }
    FILE *f = fopen(temp, "w");
    if (!f) {
        (void)unlink(temp);
        out[0] = 0;
        hs_why(why, why_len, "could not open confined island wrapper");
        return false;
    }
    char member_text[2048];
    (void)snprintf(member_text, sizeof(member_text), "%s", members);
    const char *memberv[64];
    size_t memberc = zcl_argv_split(member_text, memberv, 64);
    bool ok = memberc > 0;
    for (size_t i = 0; ok && i < memberc; i++) {
        char full[PATH_MAX];
        ok = memberv[i][0] != '/' && !strstr(memberv[i], "..") &&
             snprintf(full, sizeof(full), "%s/%s", root, memberv[i]) <
                 (int)sizeof(full) && hs_regular(full, NULL) &&
             fprintf(f, "#include \"%s\"\n", full) > 0;
    }
    char owner_full[PATH_MAX];
    ok = ok && snprintf(owner_full, sizeof(owner_full), "%s/%s", root,
                        owner) < (int)sizeof(owner_full) &&
         hs_regular(owner_full, NULL) &&
         fprintf(f, "#include \"%s\"\n", owner_full) > 0;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok) {
        (void)unlink(temp);
        out[0] = 0;
        hs_why(why, why_len, "island member list is invalid or unwritable");
        return false;
    }
    if (hs_regular(out, NULL) && hs_files_equal(temp, out)) {
        (void)unlink(temp);
    } else {
        (void)unlink(out);
        if (rename(temp, out) != 0) {
            (void)unlink(temp);
            out[0] = 0;
            hs_why(why, why_len, "could not publish stable island wrapper");
            return false;
        }
    }
    return true;
}

static bool hs_run_link(const struct hs_action_plan *plan,
                        const char *root, const char *obj, const char *so,
                        struct zcl_devloop_process_result *result,
                        int64_t *elapsed_us, char *why, size_t why_len)
{
    char cc[sizeof(plan->cc)], flags[sizeof(plan->ldflags)];
    (void)snprintf(cc, sizeof(cc), "%s", plan->cc);
    (void)snprintf(flags, sizeof(flags), "%s", plan->ldflags);
    const char *argv[HS_ARG_MAX], *flagv[HS_ARG_MAX];
    size_t argc = zcl_argv_split(cc, argv, HS_ARG_MAX);
    size_t flagc = zcl_argv_split(flags, flagv, HS_ARG_MAX);
    if (!argc || argc + flagc + 4 >= HS_ARG_MAX) {
        hs_why(why, why_len, "resident link action exceeds argv bound");
        return false;
    }
    for (size_t i = 0; i < flagc; i++)
        argv[argc++] = flagv[i];
    argv[argc++] = "-o";
    argv[argc++] = so;
    argv[argc++] = obj;
    argv[argc] = NULL;
    int64_t started = platform_time_monotonic_us();
    bool ran = zcl_devloop_process_run(root, argv, 30000, result);
    *elapsed_us = platform_time_monotonic_us() - started;
    if (!ran || result->timed_out || result->term_signal ||
        result->exit_code != 0) {
        hs_why(why, why_len, "resident module link failed");
        return false;
    }
    return true;
}

bool zcl_devloop_hotswap_build(
    const char *repo_root, const char *source_tu,
    struct zcl_devloop_hotswap_build_receipt *receipt,
    struct zcl_devloop_process_result *process,
    char *why, size_t why_len)
{
    int64_t started = platform_time_monotonic_us();
    if (why && why_len) why[0] = 0;
    const char *owner = hotswap_island_owner_for_path(source_tu);
    bool service_island = false;
    if (!owner) {
        owner = zcl_hotswap_service_source_for_path(source_tu);
        service_island = owner != NULL;
    }
    if (!repo_root || !source_tu || !receipt || !process ||
        source_tu[0] == '/' || strstr(source_tu, "..") || !owner) {
        hs_why(why, why_len, "source is outside the compiled swappable allowlist");
        return false;
    }
    memset(receipt, 0, sizeof(*receipt));
    memset(process, 0, sizeof(*process));
    char root[PATH_MAX], source_path[PATH_MAX];
    if (!realpath(repo_root, root) ||
        snprintf(source_path, sizeof(source_path), "%s/%s", root, owner) >=
            (int)sizeof(source_path) || !hs_regular(source_path, NULL)) {
        hs_why(why, why_len, "source path is not a regular checkout file");
        return false;
    }

    struct hs_action_plan plan = {0};
    pthread_mutex_lock(&g_plan_mu);
    bool plan_ok = hs_plan_load_locked(root, &receipt->plan_cache_hit,
                                       &receipt->plan_load_us, why, why_len);
    if (plan_ok)
        plan = g_plan;
    pthread_mutex_unlock(&g_plan_mu);
    if (!plan_ok)
        return false;

    char safe[256];
    size_t sn = strlen(owner);
    if (sn >= sizeof(safe)) {
        hs_why(why, why_len, "source path exceeds artifact-name bound");
        return false;
    }
    for (size_t i = 0; i <= sn; i++) {
        unsigned char c = (unsigned char)owner[i];
        safe[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-'
            ? (char)c : c ? '_' : 0;
    }
    char cached_dep[PATH_MAX], tmp_o[PATH_MAX] = {0}, tmp_d[PATH_MAX] = {0};
    char tmp_so[PATH_MAX] = {0}, unity[PATH_MAX] = {0};
    char cache_root[PATH_MAX] = {0}, cache_so[PATH_MAX] = {0};
    char cache_hash[PATH_MAX] = {0};
    int cache_fd = -1;
    if (snprintf(cached_dep, sizeof(cached_dep),
                 "%s/build/hotswap/fast/%s.d", root, safe) >=
            (int)sizeof(cached_dep) ||
        !hs_temp(tmp_o, sizeof(tmp_o), root, ".o") ||
        !hs_temp(tmp_d, sizeof(tmp_d), root, ".d") ||
        !hs_temp(tmp_so, sizeof(tmp_so), root, ".so")) {
        hs_why(why, why_len, "could not allocate confined build temporaries");
        goto fail;
    }
    const char *members = service_island ? NULL
        : hotswap_island_members_for_source(owner);
    if (!service_island &&
        (!members || !hs_unity_source(root, owner, members, safe, unity,
                                      why, why_len)))
        goto fail;
    const char *compile_input = service_island ? owner
                                               : (unity[0] ? unity : owner);

    struct hs_dep *before = zcl_malloc(sizeof(*before) * HS_DEP_MAX,
                                       "hotswap dependency baseline");
    struct hs_dep *after = zcl_malloc(sizeof(*after) * HS_DEP_MAX,
                                      "hotswap dependency result");
    if (!before || !after) {
        free(before);
        free(after);
        hs_why(why, why_len, "dependency snapshot allocation failed");
        goto fail;
    }
    size_t before_n = 0, after_n = 0;
    bool have_baseline = hs_depfile_read(root, cached_dep, before, &before_n,
                                         true);
    if (have_baseline &&
        hs_cache_key(&plan, root, owner, before, before_n,
                     receipt->artifact_cache_key) &&
        hs_cache_root(cache_root)) {
        cache_fd = hs_cache_lock(cache_root, receipt->artifact_cache_key,
                                 cache_so, cache_hash);
        if (cache_fd >= 0 &&
            hs_cache_lookup(root, safe, cache_so, cache_hash, receipt)) {
            receipt->artifact_cache_hit = true;
            receipt->dependency_count = (uint32_t)before_n;
            (void)snprintf(receipt->source_tu, sizeof(receipt->source_tu),
                           "%s", owner);
            receipt->publish_us = platform_time_monotonic_us() - started -
                                  receipt->plan_load_us;
            receipt->total_us = platform_time_monotonic_us() - started;
            free(before);
            free(after);
            (void)flock(cache_fd, LOCK_UN);
            (void)close(cache_fd);
            (void)unlink(tmp_o);
            (void)unlink(tmp_d);
            (void)unlink(tmp_so);
            return true;
        }
        if (cache_fd >= 0) {
            /* A .so without its final hash marker, or a marker whose content
             * does not verify, is a partial/corrupt entry. Under the per-key
             * lock it cannot be a publisher still in flight. */
            (void)unlink(cache_so);
            (void)unlink(cache_hash);
        }
    }
    receipt->compiler_processes = 1;
    if (!hs_run_compile(&plan, root, owner, compile_input, tmp_o, tmp_d,
                        process,
                        &receipt->compile_us, why, why_len)) {
        free(before);
        free(after);
        goto fail;
    }
    if (!hs_depfile_read(root, tmp_d, after, &after_n, true)) {
        free(before);
        free(after);
        hs_why(why, why_len, "compiler produced no valid bounded depfile");
        goto fail;
    }
    /* Refresh the next edit's dependency baseline even when this first/new
     * dependency observation is refused. rename is confined to build/. */
    (void)unlink(cached_dep);
    if (rename(tmp_d, cached_dep) != 0) {
        free(before);
        free(after);
        hs_why(why, why_len, "could not publish dependency baseline");
        goto fail;
    }
    tmp_d[0] = 0;
    receipt->dependency_count = (uint32_t)after_n;
    bool stable = have_baseline &&
        hs_deps_unchanged(before, before_n, after, after_n, why, why_len);
    char post_key[65] = {0};
    if (stable && receipt->artifact_cache_key[0] &&
        (!hs_cache_key(&plan, root, owner, after, after_n, post_key) ||
         strcmp(post_key, receipt->artifact_cache_key) != 0)) {
        hs_why(why, why_len,
               "artifact cache key changed across dependency verification");
        stable = false;
    }
    free(before);
    free(after);
    if (!stable) {
        if (!have_baseline)
            hs_why(why, why_len,
                   "dependency baseline initialized; save once more to activate");
        goto fail;
    }
    receipt->linker_processes = 1;
    if (!hs_run_link(&plan, root, tmp_o, tmp_so, process, &receipt->link_us,
                     why, why_len))
        goto fail;

    int64_t publish_started = platform_time_monotonic_us();
    if (!hs_sha256_file(tmp_so, receipt->artifact_sha256)) {
        hs_why(why, why_len, "could not hash resident module artifact");
        goto fail;
    }
    if (cache_fd >= 0 &&
        !hs_cache_publish(cache_so, cache_hash, tmp_so,
                          receipt->artifact_sha256)) {
        hs_why(why, why_len,
               "shared artifact cache publication or verification failed");
        goto fail;
    }
    const char *published_source = cache_fd >= 0 ? cache_so : tmp_so;
    if (!hs_publish_artifact_path(root, safe, published_source,
                                  receipt->artifact_sha256,
                                  receipt->artifact_path)) {
        hs_why(why, why_len,
               "content-addressed artifact collision or publish failure");
        goto fail;
    }
    receipt->publish_us = platform_time_monotonic_us() - publish_started;
    (void)snprintf(receipt->source_tu, sizeof(receipt->source_tu), "%s",
                   owner);
    receipt->total_us = platform_time_monotonic_us() - started;
    (void)unlink(tmp_o);
    (void)unlink(tmp_so);
    if (cache_fd >= 0) {
        (void)flock(cache_fd, LOCK_UN);
        (void)close(cache_fd);
    }
    return true;

fail:
    if (tmp_o[0]) (void)unlink(tmp_o);
    if (tmp_d[0]) (void)unlink(tmp_d);
    if (tmp_so[0]) (void)unlink(tmp_so);
    if (cache_fd >= 0) {
        (void)flock(cache_fd, LOCK_UN);
        (void)close(cache_fd);
    }
    receipt->total_us = platform_time_monotonic_us() - started;
    return false;
}

static void hs_json_text_preview(const char *input, char out[1025])
{
    size_t n = input ? strlen(input) : 0;
    if (n > 1024) n = 1024;
    if (n) memcpy(out, input, n);
    out[n] = 0;
}

static bool hs_resident_call(const char *artifact, bool activate,
                             struct json_value *response, int64_t *elapsed_us,
                             char *why, size_t why_len)
{
    const char *home = getenv("HOME");
    char datadir[PATH_MAX];
    if (!home || !home[0] ||
        snprintf(datadir, sizeof(datadir), "%s/.zclassic-c23-dev", home) >=
            (int)sizeof(datadir)) {
        hs_why(why, why_len, "HOME cannot resolve the isolated dev datadir");
        return false;
    }
    node_rpc_client_init(datadir, 18252);
    struct json_value params, path, flag;
    json_init(&params);
    json_set_array(&params);
    json_init(&path);
    json_set_str(&path, artifact);
    (void)json_push_back(&params, &path);
    json_free(&path);
    json_init(&flag);
    json_set_bool(&flag, activate);
    (void)json_push_back(&params, &flag);
    json_free(&flag);
    char params_json[PATH_MAX + 64];
    size_t params_n = json_write(&params, params_json, sizeof(params_json));
    json_free(&params);
    if (!params_n) {
        hs_why(why, why_len, "resident activation request exceeded its bound");
        return false;
    }
    int64_t started = platform_time_monotonic_us();
    char *raw = node_rpc_call("dev_hotswap_native", params_json);
    *elapsed_us = platform_time_monotonic_us() - started;
    if (!raw) {
        hs_why(why, why_len, "resident dev node returned no activation body");
        return false;
    }
    json_init(response);
    bool parsed = json_read(response, raw, strlen(raw)) &&
                  response->type == JSON_OBJ;
    free(raw);
    if (!parsed) {
        json_free(response);
        hs_why(why, why_len, "resident dev node returned malformed activation JSON");
        return false;
    }
    const struct json_value *ok_v = json_get(response, "ok");
    if (!ok_v || ok_v->type != JSON_BOOL || !json_get_bool(ok_v)) {
        char response_error[512];
        if (zcl_devloop_hotswap_response_error(
                response, response_error, sizeof(response_error)))
            hs_why(why, why_len, response_error);
        else
            hs_why(why, why_len, "resident refused the candidate");
        return false;
    }
    const struct json_value *activated_v = json_get(response, "activated");
    if (activate && (!activated_v || activated_v->type != JSON_BOOL ||
                     !json_get_bool(activated_v))) {
        hs_why(why, why_len,
               "resident verified but did not activate the candidate");
        return false;
    }
    return true;
}

struct hs_shadow_wire {
    uint32_t magic;
    struct zcl_hotswap_service_report report;
};

#define HS_SHADOW_WIRE_MAGIC UINT32_C(0x48535331)

/* The watcher itself is the persistent shadow parent: contracts, registry,
 * dependency state, and immutable fixtures are already resident. Each
 * candidate gets a disposable fork, maps only the tiny service .so, runs the
 * resident-frozen KAT, reports one fixed-size result, and exits. No exec, RPC,
 * node, wallet, SQLite, network, publication, or full-program link exists on
 * this path. */
static bool hs_shadow_probe(const char *artifact,
                            struct json_value *response,
                            int64_t *elapsed_us,
                            char *why, size_t why_len)
{
    int pipefd[2] = {-1, -1};
    int64_t started = platform_time_monotonic_us();
    if (!artifact || !response || !elapsed_us ||
        pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) {
        hs_why(why, why_len, "shadow runner pipe unavailable");
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]); close(pipefd[1]);
        hs_why(why, why_len, "shadow runner fork unavailable");
        return false;
    }
    if (child == 0) {
        close(pipefd[0]);
        struct hs_shadow_wire wire = {.magic = HS_SHADOW_WIRE_MAGIC};
        (void)zcl_native_hotswap_service_probe_local(
            artifact, &wire.report);
        const uint8_t *p = (const uint8_t *)&wire;
        size_t left = sizeof(wire);
        while (left > 0) {
            ssize_t wrote = write(pipefd[1], p, left);
            if (wrote > 0) {
                p += (size_t)wrote;
                left -= (size_t)wrote;
            } else if (wrote < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(pipefd[1]);
        _exit(left == 0 ? 0 : 125);
    }
    close(pipefd[1]);
    struct hs_shadow_wire wire;
    memset(&wire, 0, sizeof(wire));
    uint8_t *dst = (uint8_t *)&wire;
    size_t have = 0;
    bool timed_out = false, cancelled = false;
    const int64_t deadline = started + 1000000;
    while (have < sizeof(wire)) {
        if (zcl_devloop_process_cancel_requested()) {
            cancelled = true;
            break;
        }
        int64_t remaining = deadline - platform_time_monotonic_us();
        if (remaining <= 0) {
            timed_out = true;
            break;
        }
        int wait_ms = remaining > 10000 ? 10 : (int)((remaining + 999) / 1000);
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN | POLLHUP};
        int ready = poll(&pfd, 1, wait_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) continue;
        ssize_t got = read(pipefd[0], dst + have, sizeof(wire) - have);
        if (got > 0) have += (size_t)got;
        else if (got == 0) break;
        else if (errno != EAGAIN && errno != EINTR) break;
    }
    close(pipefd[0]);
    if (timed_out || cancelled || have != sizeof(wire))
        (void)kill(child, SIGKILL);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    *elapsed_us = platform_time_monotonic_us() - started;
    bool valid = waited == child && !timed_out && !cancelled &&
        have == sizeof(wire) &&
        wire.magic == HS_SHADOW_WIRE_MAGIC && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;
    bool ok = valid && wire.report.recognized && wire.report.ok &&
        wire.report.verify_only && wire.report.probed &&
        !wire.report.activated;
    json_init(response); json_set_object(response);
    (void)json_push_kv_str(response, "schema", "zcl.dev_shadow_story.v1");
    (void)json_push_kv_str(response, "mode", "HOT_SHADOW");
    (void)json_push_kv_str(response, "status", ok ? "green" : "red");
    (void)json_push_kv_bool(response, "forked", true);
    (void)json_push_kv_bool(response, "exec_process", false);
    (void)json_push_kv_bool(response, "activated", false);
    (void)json_push_kv_bool(response, "forbidden_effects_absent", true);
    (void)json_push_kv_int(response, "elapsed_us", *elapsed_us);
    if (valid) {
        (void)json_push_kv_str(response, "service_id",
                               wire.report.service_id);
        (void)json_push_kv_str(response, "probe_stage", wire.report.stage);
    }
    if (!ok) {
        const char *message = cancelled ? "shadow story superseded" :
            timed_out ? "shadow story exceeded 1000 ms" :
            valid && wire.report.error[0] ? wire.report.error :
            "shadow story worker returned no valid frozen-KAT receipt";
        hs_why(why, why_len, message);
        (void)json_push_kv_str(response, "error", message);
    }
    return ok;
}

static bool hs_emit_event(const char *root, const char *source,
                          size_t changed_path_count,
                          const char *status, const char *phase,
                          bool published, int64_t elapsed_us,
                          const struct zcl_devloop_hotswap_build_receipt *build,
                          int64_t activation_us,
                          const struct json_value *resident,
                          const struct zcl_devloop_process_result *process,
                          const char *why, bool flush_after)
{
    struct json_value doc, receipt;
    json_init(&doc);
    json_set_object(&doc);
    (void)json_push_kv_str(&doc, "schema", "zcl.dev_cycle.v1");
    (void)json_push_kv_str(&doc, "producer", "resident-build-authority");
    (void)json_push_kv_str(&doc, "status", status);
    (void)json_push_kv_str(&doc, "action", "hotswap");
    const bool service_island =
        zcl_hotswap_service_source_for_path(source) != NULL;
    (void)json_push_kv_str(
        &doc, "reason", service_island
            ? (changed_path_count > 1 ? "single_service_island_batch"
                                      : "single_service_island")
            : (changed_path_count > 1 ? "single_stateless_island_batch"
                                      : "single_stateless_provider"));
    (void)json_push_kv_str(&doc, "phase",
                           zcl_devloop_progress_phase(status, phase));
    (void)json_push_kv_str(&doc, "stage_detail", phase);
    if (zcl_devloop_event_edit_epoch()[0])
        (void)json_push_kv_str(&doc, "edit_epoch",
                               zcl_devloop_event_edit_epoch());
    (void)json_push_kv_bool(&doc, "runtime_published", published);
    (void)json_push_kv_int(&doc, "changed_path_count",
                           (int64_t)changed_path_count);
    (void)json_push_kv_bool(&doc, "atomic_batch_generation",
                            changed_path_count > 1 && published);
    (void)json_push_kv_int(&doc, "elapsed_us", elapsed_us);
    (void)json_push_kv_int(&doc, "elapsed_ms", elapsed_us / 1000);
    (void)json_push_kv_int(&doc, "make_processes", 0);
    (void)json_push_kv_int(&doc, "shell_processes", 0);
    (void)json_push_kv_int(&doc, "git_operations", 0);
    (void)json_push_kv_int(&doc, "publication_operations", 0);
    (void)json_push_kv_int(&doc, "remote_operations", 0);
    (void)json_push_kv_int(&doc, "network_operations", 0);
    (void)json_push_kv_int(&doc, "storage_ack_waits", 0);
    (void)json_push_kv_int(&doc, "full_program_links", 0);
    (void)json_push_kv_int(&doc, "sqlite_operations", 0);
    (void)json_push_kv_int(&doc, "full_tree_scans", 0);
    (void)json_push_kv_str(&doc, "source_tu", source);
    if (why && why[0])
        (void)json_push_kv_str(&doc, "failure_capsule", why);
    if (process && process->output_len) {
        char preview[1025];
        hs_json_text_preview(process->output, preview);
        (void)json_push_kv_str(&doc, "compiler_output", preview);
        (void)json_push_kv_bool(&doc, "compiler_output_truncated",
                                process->output_len > 1024 ||
                                process->output_truncated);
    }
    if (build) {
        json_init(&receipt);
        json_set_object(&receipt);
        (void)json_push_kv_str(&receipt, "schema",
                               "zcl.hotswap_build_receipt.v1");
        (void)json_push_kv_str(&receipt, "source_tu", build->source_tu);
        (void)json_push_kv_str(&receipt, "artifact_path",
                               build->artifact_path);
        (void)json_push_kv_str(&receipt, "artifact_sha256",
                               build->artifact_sha256);
        (void)json_push_kv_bool(&receipt, "plan_cache_hit",
                                build->plan_cache_hit);
        (void)json_push_kv_bool(&receipt, "artifact_cache_hit",
                                build->artifact_cache_hit);
        if (build->artifact_cache_key[0])
            (void)json_push_kv_str(&receipt, "artifact_cache_key",
                                   build->artifact_cache_key);
        (void)json_push_kv_int(&receipt, "dependencies",
                               build->dependency_count);
        (void)json_push_kv_int(&receipt, "compiler_processes",
                               build->compiler_processes);
        (void)json_push_kv_int(&receipt, "linker_processes",
                               build->linker_processes);
        (void)json_push_kv_int(&receipt, "full_program_linker_processes", 0);
        (void)json_push_kv_int(&receipt, "plan_load_us",
                               build->plan_load_us);
        (void)json_push_kv_int(&receipt, "compile_us", build->compile_us);
        (void)json_push_kv_int(&receipt, "link_us", build->link_us);
        (void)json_push_kv_int(&receipt, "publish_us", build->publish_us);
        (void)json_push_kv_int(&receipt, "build_total_us", build->total_us);
        (void)json_push_kv_int(&receipt, "activation_us", activation_us);
        (void)json_push_kv(&doc, "build_receipt", &receipt);
        json_free(&receipt);
    }
    if (resident && resident->type == JSON_OBJ)
        (void)json_push_kv(&doc, "resident", resident);
    char why_not_live[512], next_command[256];
    zcl_devloop_hotswap_guidance(
        status, phase, why, why_not_live, sizeof(why_not_live),
        next_command, sizeof(next_command));
    (void)json_push_kv_str(&doc, "why_not_live", why_not_live);
    (void)json_push_kv_str(&doc, "agent_next_action", next_command);

    char wire[16384];
    size_t n = json_write(&doc, wire, sizeof(wire) - 1);
    json_free(&doc);
    if (!n)
        return false;
    wire[n++] = '\n';
    wire[n] = 0;
    char state_why[160] = {0};
    int64_t epoch = 0;
    if (!zcl_devloop_cycle_stream_publish(root, wire, n, &epoch,
                                          state_why, sizeof(state_why))) {
        fprintf(stderr, "[devloop] resident event publication failed: %s\n",
                state_why[0] ? state_why : "unknown");
        return false;
    }
    (void)fwrite(wire, 1, n, stdout);
    (void)fflush(stdout);
    if (flush_after && !zcl_devloop_cycle_stream_flush_through(
                           root, epoch, state_why, sizeof(state_why))) {
        fprintf(stderr, "[devloop] async event journal flush failed: %s\n",
                state_why[0] ? state_why : "unknown");
        return false;
    }
    return true;
}

static const char *hs_owner_for_path(const char *path)
{
    const char *owner = hotswap_island_owner_for_path(path);
    return owner ? owner : zcl_hotswap_service_source_for_path(path);
}

int zcl_devloop_hotswap_batch_event(
    const char *repo_root, const char *const *paths, size_t path_count,
    enum zcl_devloop_publish_mode publish_mode)
{
    if (!repo_root || !paths || path_count == 0 ||
        path_count > ZCL_DEVLOOP_MAX_FILES)
        return 0;
    const char *owner = hs_owner_for_path(paths[0]);
    if (!owner) return 0;
    for (size_t i = 1; i < path_count; i++) {
        const char *next = hs_owner_for_path(paths[i]);
        if (!next || strcmp(next, owner) != 0) return 0;
    }
    int64_t started = platform_time_monotonic_us();
    struct zcl_devloop_hotswap_build_receipt build = {0};
    struct zcl_devloop_process_result process = {0};
    char why[512] = {0};
    if (!zcl_devloop_hotswap_build(repo_root, owner, &build, &process,
                                   why, sizeof(why))) {
        if (process.cancelled || zcl_devloop_process_cancel_requested())
            return 2;
        return hs_emit_event(repo_root, owner, path_count,
                             "rejected", "compile",
                             false, platform_time_monotonic_us() - started,
                             &build, 0, NULL, &process, why, true) ? 1 : -1;
    }
    if (!hs_emit_event(repo_root, owner, path_count,
                       "reflex_ready", "candidate_compile", false,
                       platform_time_monotonic_us() - started, &build, 0,
                       NULL, &process, "", false))
        return -1;
    bool activate = zcl_devloop_publish_mode_applies(publish_mode);
    struct json_value resident;
    json_init(&resident);
    int64_t activation_us = 0;
    const bool shadow_story = !activate &&
        strcmp(owner,
               "app/services/src/vault_intent_decision_service.c") == 0;
    bool ok = shadow_story
        ? hs_shadow_probe(build.artifact_path, &resident, &activation_us,
                          why, sizeof(why))
        : hs_resident_call(build.artifact_path, activate, &resident,
                           &activation_us, why, sizeof(why));
    if (zcl_devloop_process_cancel_requested()) {
        json_free(&resident);
        return 2;
    }
    const char *phase = shadow_story ? "vault_intent_story" :
        (ok ? (activate ? "resident_commit" : "resident_probe")
            : "resident_probe");
    const char *status = shadow_story ? (ok ? "story_green" : "story_red")
                                      : (ok ? "passed" : "rejected");
    bool emitted = hs_emit_event(
        repo_root, owner, path_count, status, phase,
        ok && activate, platform_time_monotonic_us() - started, &build,
        activation_us, resident.type == JSON_OBJ ? &resident : NULL,
        &process, why, true);
    json_free(&resident);
    if (!emitted)
        return -1;
    return shadow_story && ok
        ? ZCL_DEVLOOP_RESTART_EVENT_PROOF_PENDING
        : ZCL_DEVLOOP_RESTART_EVENT_FINAL;
}

int zcl_devloop_hotswap_event(const char *repo_root, const char *source_tu,
                              enum zcl_devloop_publish_mode publish_mode)
{
    const char *paths[] = {source_tu};
    return zcl_devloop_hotswap_batch_event(repo_root, paths, 1,
                                           publish_mode);
}
