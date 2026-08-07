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
#include "hotswap/hotswap_module.h"
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

#define HS_PLAN_TEXT_MAX 12288
#define HS_ARG_MAX 256
#define HS_DEP_MAX 512

struct hs_action_plan {
    char root[PATH_MAX];
    char cc[512];
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

static bool hs_plan_load_locked(const char *root, bool *cache_hit,
                                int64_t *elapsed_us, char *why,
                                size_t why_len)
{
    int64_t started = platform_time_monotonic_us();
    char flags_path[PATH_MAX], makefile[PATH_MAX], manifest[PATH_MAX];
    char islands[PATH_MAX];
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
            (int)sizeof(islands)) {
        hs_why(why, why_len, "action plan path overflow");
        return false;
    }
    struct stat stamp, make_st, manifest_st, islands_st;
    if (!hs_regular(flags_path, &stamp)) {
        hs_why(why, why_len,
               "resident action plan absent; run make hotswap-module-so once");
        return false;
    }
    if (!hs_regular(makefile, &make_st) || !hs_regular(manifest, &manifest_st) ||
        !hs_regular(islands, &islands_st) ||
        make_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (make_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         make_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec) ||
        manifest_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (manifest_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         manifest_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec) ||
        islands_st.st_mtim.tv_sec > stamp.st_mtim.tv_sec ||
        (islands_st.st_mtim.tv_sec == stamp.st_mtim.tv_sec &&
         islands_st.st_mtim.tv_nsec > stamp.st_mtim.tv_nsec)) {
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
    if (read_error || !next.cc[0] || !next.cflags[0] || !next.ldflags[0] ||
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

static bool hs_run_compile(const char *root, const char *source_tu,
                           const char *compile_input,
                           const char *obj, const char *dep,
                           struct zcl_devloop_process_result *result,
                           int64_t *elapsed_us, char *why, size_t why_len)
{
    char cc[sizeof(g_plan.cc)], flags[sizeof(g_plan.cflags)];
    (void)snprintf(cc, sizeof(cc), "%s", g_plan.cc);
    (void)snprintf(flags, sizeof(flags), "%s", g_plan.cflags);
    const char *argv[HS_ARG_MAX];
    size_t argc = zcl_argv_split(cc, argv, HS_ARG_MAX);
    const char *flagv[HS_ARG_MAX];
    size_t flagc = zcl_argv_split(flags, flagv, HS_ARG_MAX);
    if (!argc || argc + flagc + 12 >= HS_ARG_MAX) {
        hs_why(why, why_len, "resident compile action exceeds argv bound");
        return false;
    }
    for (size_t i = 0; i < flagc; i++)
        argv[argc++] = flagv[i];
    char source_define[320];
    (void)snprintf(source_define, sizeof(source_define),
                   "-DZCL_HOTSWAP_MODULE_SOURCE_TU=\"%s\"", source_tu);
    argv[argc++] = "-fPIC";
    argv[argc++] = "-DZCL_HOTSWAP_MODULE_GEN";
    argv[argc++] = source_define;
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

static bool hs_run_link(const char *root, const char *obj, const char *so,
                        struct zcl_devloop_process_result *result,
                        int64_t *elapsed_us, char *why, size_t why_len)
{
    char cc[sizeof(g_plan.cc)], flags[sizeof(g_plan.ldflags)];
    (void)snprintf(cc, sizeof(cc), "%s", g_plan.cc);
    (void)snprintf(flags, sizeof(flags), "%s", g_plan.ldflags);
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

    pthread_mutex_lock(&g_plan_mu);
    bool plan_ok = hs_plan_load_locked(root, &receipt->plan_cache_hit,
                                       &receipt->plan_load_us, why, why_len);
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
    if (snprintf(cached_dep, sizeof(cached_dep),
                 "%s/build/hotswap/fast/%s.d", root, safe) >=
            (int)sizeof(cached_dep) ||
        !hs_temp(tmp_o, sizeof(tmp_o), root, ".o") ||
        !hs_temp(tmp_d, sizeof(tmp_d), root, ".d") ||
        !hs_temp(tmp_so, sizeof(tmp_so), root, ".so")) {
        hs_why(why, why_len, "could not allocate confined build temporaries");
        goto fail;
    }
    const char *members = hotswap_island_members_for_source(owner);
    if (!members || !hs_unity_source(root, owner, members, safe, unity,
                                     why, why_len))
        goto fail;
    const char *compile_input = unity[0] ? unity : owner;

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
    if (!hs_run_compile(root, owner, compile_input, tmp_o, tmp_d, process,
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
    free(before);
    free(after);
    if (!stable) {
        if (!have_baseline)
            hs_why(why, why_len,
                   "dependency baseline initialized; save once more to activate");
        goto fail;
    }
    if (!hs_run_link(root, tmp_o, tmp_so, process, &receipt->link_us,
                     why, why_len))
        goto fail;

    int64_t publish_started = platform_time_monotonic_us();
    if (!hs_sha256_file(tmp_so, receipt->artifact_sha256)) {
        hs_why(why, why_len, "could not hash resident module artifact");
        goto fail;
    }
    if (snprintf(receipt->artifact_path, sizeof(receipt->artifact_path),
                 "%s/build/hotswap/%s-%s.so", root, safe,
                 receipt->artifact_sha256) >=
        (int)sizeof(receipt->artifact_path)) {
        hs_why(why, why_len, "content-addressed artifact path overflow");
        goto fail;
    }
    if (link(tmp_so, receipt->artifact_path) != 0) {
        char existing[65];
        if (errno != EEXIST ||
            !hs_sha256_file(receipt->artifact_path, existing) ||
            strcmp(existing, receipt->artifact_sha256) != 0) {
            hs_why(why, why_len,
                   "content-addressed artifact collision or publish failure");
            goto fail;
        }
    }
    (void)chmod(receipt->artifact_path, 0444);
    receipt->publish_us = platform_time_monotonic_us() - publish_started;
    (void)snprintf(receipt->source_tu, sizeof(receipt->source_tu), "%s",
                   owner);
    receipt->total_us = platform_time_monotonic_us() - started;
    (void)unlink(tmp_o);
    (void)unlink(tmp_so);
    return true;

fail:
    if (tmp_o[0]) (void)unlink(tmp_o);
    if (tmp_d[0]) (void)unlink(tmp_d);
    if (tmp_so[0]) (void)unlink(tmp_so);
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
        const struct json_value *message_v = json_get(response, "message");
        if (!message_v) message_v = json_get(response, "error");
        const char *message = message_v && message_v->type == JSON_STR
            ? json_get_str(message_v) : "resident refused the candidate";
        hs_why(why, why_len, message);
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

static bool hs_emit_event(const char *root, const char *source,
                          const char *status, const char *phase,
                          bool published, int64_t elapsed_us,
                          const struct zcl_devloop_hotswap_build_receipt *build,
                          int64_t activation_us,
                          const struct json_value *resident,
                          const struct zcl_devloop_process_result *process,
                          const char *why)
{
    struct json_value doc, receipt;
    json_init(&doc);
    json_set_object(&doc);
    (void)json_push_kv_str(&doc, "schema", "zcl.dev_cycle.v1");
    (void)json_push_kv_str(&doc, "producer", "resident-build-authority");
    (void)json_push_kv_str(&doc, "status", status);
    (void)json_push_kv_str(&doc, "action", "hotswap");
    (void)json_push_kv_str(&doc, "reason", "single_stateless_provider");
    (void)json_push_kv_str(&doc, "phase", phase);
    (void)json_push_kv_bool(&doc, "runtime_published", published);
    (void)json_push_kv_int(&doc, "elapsed_us", elapsed_us);
    (void)json_push_kv_int(&doc, "elapsed_ms", elapsed_us / 1000);
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
        (void)json_push_kv_int(&receipt, "dependencies",
                               build->dependency_count);
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
    (void)json_push_kv_str(
        &doc, "agent_next_action",
        strcmp(status, "passed") == 0
            ? "keep editing; the resident authority owns the next module epoch"
            : "repair the named fast-lane refusal; no old handler was replaced");

    char wire[16384];
    size_t n = json_write(&doc, wire, sizeof(wire) - 1);
    json_free(&doc);
    if (!n)
        return false;
    wire[n++] = '\n';
    wire[n] = 0;
    char state_why[160] = {0};
    if (!zcl_devloop_cycle_state_write(root, wire, n, state_why,
                                       sizeof(state_why))) {
        fprintf(stderr, "[devloop] resident receipt persistence failed: %s\n",
                state_why[0] ? state_why : "unknown");
        return false;
    }
    (void)fwrite(wire, 1, n, stdout);
    (void)fflush(stdout);
    return true;
}

int zcl_devloop_hotswap_event(const char *repo_root, const char *source_tu,
                              enum zcl_devloop_publish_mode publish_mode)
{
    if (!repo_root || !source_tu || !hotswap_island_owner_for_path(source_tu))
        return 0;
    int64_t started = platform_time_monotonic_us();
    struct zcl_devloop_hotswap_build_receipt build = {0};
    struct zcl_devloop_process_result process = {0};
    char why[512] = {0};
    if (!zcl_devloop_hotswap_build(repo_root, source_tu, &build, &process,
                                   why, sizeof(why))) {
        return hs_emit_event(repo_root, source_tu, "rejected", "compile",
                             false, platform_time_monotonic_us() - started,
                             &build, 0, NULL, &process, why) ? 1 : -1;
    }
    bool activate = zcl_devloop_publish_mode_applies(publish_mode);
    struct json_value resident;
    json_init(&resident);
    int64_t activation_us = 0;
    bool ok = hs_resident_call(build.artifact_path, activate, &resident,
                               &activation_us, why, sizeof(why));
    const char *phase = ok ? (activate ? "resident_commit" : "resident_probe")
                           : "resident_probe";
    bool emitted = hs_emit_event(
        repo_root, source_tu, ok ? "passed" : "rejected", phase,
        ok && activate, platform_time_monotonic_us() - started, &build,
        activation_us, resident.type == JSON_OBJ ? &resident : NULL,
        &process, why);
    json_free(&resident);
    return emitted ? 1 : -1;
}
