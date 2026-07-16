/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex_deps — turn the compiler's own dependency files (the depfiles
 * under build/, extension .d) into include edges. Each depfile records
 * "<obj>: <src.c> <hdr.h> ..."; we
 * emit (source, header) pairs for in-tree prerequisites, which is the exact
 * include graph the build already computed — no re-parsing of #include lines,
 * no guessing search paths. Retained `epochs/` and `history/` generations are
 * excluded; their duplicate immutable receipts are not the active graph. If
 * build/ is absent (a fresh tree), no edges are produced. Other I/O failures
 * fail closed. */

#include "codeindex_priv.h"

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

/* Rewrite an absolute or ./-relative depfile token to repo-relative, or return
 * false if the token is outside the tree (a system header). */
static bool to_relpath(const char *root, const char *tok, char out[CI_PATH_MAX])
{
    size_t rl = strlen(root);
    if (strncmp(tok, root, rl) == 0 && tok[rl] == '/') {
        snprintf(out, CI_PATH_MAX, "%s", tok + rl + 1);
        return true;
    }
    if (tok[0] == '/')
        return false;  /* absolute, outside root */
    /* already relative (build usually emits repo-relative prereqs) */
    if (strncmp(tok, "./", 2) == 0) tok += 2;
    if (tok[0] == '/') return false;
    /* reject paths that escape upward or reference vendored system trees */
    if (strncmp(tok, "../", 3) == 0) return false;
    snprintf(out, CI_PATH_MAX, "%s", tok);
    return true;
}

static bool has_ext(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && strcmp(s + a - b, ext) == 0;
}

/* Parse one depfile's text; emit (src, dep) edges. */
static void parse_depfile(const char *root, char *text, size_t len,
                          ci_dep_cb cb, void *user)
{
    /* fold line continuations: "\\\n" → "  " */
    for (size_t i = 0; i + 1 < len; i++) {
        if (text[i] == '\\' && text[i + 1] == '\n') {
            text[i] = ' ';
            text[i + 1] = ' ';
        }
    }
    /* process one logical rule per physical line */
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *rhs = colon + 1;
        /* tokenize prerequisites */
        char src_rel[CI_PATH_MAX];
        bool have_src = false;
        char *tsave = NULL;
        for (char *tok = strtok_r(rhs, " \t", &tsave); tok;
             tok = strtok_r(NULL, " \t", &tsave)) {
            char rel[CI_PATH_MAX];
            if (!to_relpath(root, tok, rel)) continue;
            if (!have_src && (has_ext(rel, ".c") || has_ext(rel, ".cc") ||
                              has_ext(rel, ".c23"))) {
                snprintf(src_rel, sizeof(src_rel), "%s", rel);
                have_src = true;
                continue;
            }
            if (have_src && (has_ext(rel, ".h") || has_ext(rel, ".hpp") ||
                             has_ext(rel, ".hh")))
                cb(src_rel, rel, user);
        }
    }
}

struct dep_paths {
    char **items;
    size_t count;
    size_t capacity;
};

static void dep_paths_free(struct dep_paths *paths)
{
    if (!paths) return;
    for (size_t i = 0; i < paths->count; i++) free(paths->items[i]);
    free(paths->items);
    memset(paths, 0, sizeof(*paths));
}

static bool dep_paths_push(struct dep_paths *paths, const char *path)
{
    if (paths->count == paths->capacity) {
        size_t next = paths->capacity ? paths->capacity * 2 : 128;
        char **items = zcl_realloc(paths->items, next * sizeof(*items),
                                   "codeindex dep paths");
        if (!items) return false;
        paths->items = items;
        paths->capacity = next;
    }
    paths->items[paths->count] = zcl_strdup(path, "codeindex dep path");
    if (!paths->items[paths->count]) return false;
    paths->count++;
    return true;
}

static int dep_path_cmp(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static bool collect_dep_paths(const char *root, const char *reldir,
                              struct dep_paths *paths)
{
    char full[CI_PATH_MAX];
    int fn = snprintf(full, sizeof(full), "%s/%s", root, reldir);
    if (fn <= 0 || (size_t)fn >= sizeof(full))
        return false;
    DIR *dir = opendir(full);
    if (!dir) return false;

    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char child[CI_PATH_MAX];
        int cn = snprintf(child, sizeof(child), "%s/%s", reldir,
                          entry->d_name);
        if (cn <= 0 || (size_t)cn >= sizeof(child)) {
            ok = false;
            break;
        }
        char child_full[CI_PATH_MAX];
        int cfn = snprintf(child_full, sizeof(child_full), "%s/%s", root,
                           child);
        if (cfn <= 0 || (size_t)cfn >= sizeof(child_full)) {
            ok = false;
            break;
        }
        struct stat st;
        if (lstat(child_full, &st) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            /* Compile epochs are immutable build-history receipts, not the
             * current include graph. Scanning all retained epochs inflated a
             * warm lookup to tens of thousands of files and duplicated every
             * edge. The familiar object-root aliases are the current inputs. */
            if (strcmp(entry->d_name, "epochs") == 0 ||
                strcmp(entry->d_name, "history") == 0)
                continue;
            ok = collect_dep_paths(root, child, paths);
        } else if (has_ext(entry->d_name, ".d")) {
            ok = S_ISREG(st.st_mode) && dep_paths_push(paths, child);
        }
    }
    int saved = errno;
    if (closedir(dir) != 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (!ok) errno = saved ? saved : EIO;
    return ok;
}

static void dep_root_init(struct sha3_256_ctx *sha, bool build_present)
{
    static const char domain[] = "zcl.codeindex.dep_root.v1";
    sha3_256_init(sha);
    sha3_256_write(sha, (const unsigned char *)domain, sizeof(domain));
    const unsigned char marker = build_present ? 1U : 0U;
    sha3_256_write(sha, &marker, 1);
}

static void dep_stat_root_init(struct sha3_256_ctx *sha, bool build_present)
{
    static const char domain[] = "zcl.codeindex.dep_stat_root.v1";
    sha3_256_init(sha);
    sha3_256_write(sha, (const unsigned char *)domain, sizeof(domain));
    const unsigned char marker = build_present ? 1U : 0U;
    sha3_256_write(sha, &marker, 1);
}

static void dep_sha_write_u64le(struct sha3_256_ctx *sha, uint64_t value)
{
    unsigned char encoded[8];
    for (unsigned int i = 0; i < sizeof(encoded); i++)
        encoded[i] = (unsigned char)((value >> (i * 8U)) & 0xffU);
    sha3_256_write(sha, encoded, sizeof(encoded));
}

static void dep_stat_root_add(struct sha3_256_ctx *sha, const char *relpath,
                              const struct stat *st)
{
    sha3_256_write(sha, (const unsigned char *)relpath, strlen(relpath) + 1);
    dep_sha_write_u64le(sha, (uint64_t)st->st_dev);
    dep_sha_write_u64le(sha, (uint64_t)st->st_ino);
    dep_sha_write_u64le(sha, (uint64_t)st->st_size);
    dep_sha_write_u64le(sha, (uint64_t)st->st_mtim.tv_sec);
    dep_sha_write_u64le(sha, (uint64_t)st->st_mtim.tv_nsec);
    dep_sha_write_u64le(sha, (uint64_t)st->st_ctim.tv_sec);
    dep_sha_write_u64le(sha, (uint64_t)st->st_ctim.tv_nsec);
}

static bool scan_one_depfile(const char *root, const char *relpath,
                             ci_dep_cb cb, void *user,
                             struct sha3_256_ctx *sha,
                             struct sha3_256_ctx *stat_sha)
{
    char full[CI_PATH_MAX];
    int fn = snprintf(full, sizeof(full), "%s/%s", root, relpath);
    if (fn <= 0 || (size_t)fn >= sizeof(full))
        LOG_FAIL("codeindex", "depfile path too long: %s", relpath);
    int fd = open(full, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        LOG_FAIL("codeindex", "open depfile failed path=%s: %s", relpath,
                 strerror(errno));
    struct stat before;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || before.st_size > INT64_C(67108864)) {
        int saved = errno ? errno : EFBIG;
        close(fd);
        LOG_FAIL("codeindex", "invalid depfile path=%s: %s", relpath,
                 strerror(saved));
    }
    size_t len = (size_t)before.st_size;
    char *buf = zcl_malloc(len + 1, "codeindex depfile bytes");
    if (!buf) {
        close(fd);
        LOG_FAIL("codeindex", "allocate depfile path=%s", relpath);
    }
    size_t used = 0;
    bool ok = true;
    int saved = 0;
    while (used < len) {
        ssize_t got = read(fd, buf + used, len - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ok = false;
            saved = got < 0 ? errno : EIO;
            break;
        }
        used += (size_t)got;
    }
    char extra;
    if (ok) {
        ssize_t got;
        do {
            got = read(fd, &extra, 1);
        } while (got < 0 && errno == EINTR);
        if (got != 0) {
            ok = false;
            saved = got < 0 ? errno : EBUSY;
        }
    }
    struct stat after;
    if (ok && (fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
               before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
               before.st_size != after.st_size ||
               before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
               before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
               before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
               before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)) {
        ok = false;
        saved = errno ? errno : EBUSY;
    }
    if (close(fd) != 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (!ok) {
        free(buf);
        LOG_FAIL("codeindex", "read depfile failed path=%s: %s", relpath,
                 strerror(saved ? saved : EIO));
    }
    buf[len] = '\0';
    sha3_256_write(sha, (const unsigned char *)relpath, strlen(relpath) + 1);
    unsigned char encoded_len[8];
    for (unsigned int i = 0; i < 8; i++)
        encoded_len[i] = (unsigned char)(((uint64_t)len >> (i * 8)) & 0xffU);
    sha3_256_write(sha, encoded_len, sizeof(encoded_len));
    sha3_256_write(sha, (const unsigned char *)buf, len);
    ci_test_note_exact_bytes((uint64_t)len);
    if (stat_sha) dep_stat_root_add(stat_sha, relpath, &after);
    if (cb) parse_depfile(root, buf, len, cb, user);
    free(buf);
    return true;
}

static bool deps_scan_exact(const char *root, ci_dep_cb cb, void *user,
                            uint8_t exact_out[32], uint8_t stat_out[32])
{
    if (!root || !exact_out)
        LOG_FAIL("codeindex", "null arg to deps_scan");
    char build[CI_PATH_MAX];
    int bn = snprintf(build, sizeof(build), "%s/build", root);
    if (bn <= 0 || (size_t)bn >= sizeof(build))
        LOG_FAIL("codeindex", "build path too long");
    struct stat build_st;
    bool present = lstat(build, &build_st) == 0;
    if (!present && errno != ENOENT)
        LOG_FAIL("codeindex", "inspect build directory failed: %s",
                 strerror(errno));
    if (present && !S_ISDIR(build_st.st_mode))
        LOG_FAIL("codeindex", "build path is not a directory");

    struct sha3_256_ctx sha;
    dep_root_init(&sha, present);
    struct sha3_256_ctx stat_sha;
    if (stat_out) dep_stat_root_init(&stat_sha, present);
    if (!present) {
        sha3_256_finalize(&sha, exact_out);
        if (stat_out) sha3_256_finalize(&stat_sha, stat_out);
        return true;
    }

    struct dep_paths paths = {0};
    if (!collect_dep_paths(root, "build", &paths)) {
        dep_paths_free(&paths);
        LOG_FAIL("codeindex", "collect depfiles failed: %s", strerror(errno));
    }
    qsort(paths.items, paths.count, sizeof(paths.items[0]), dep_path_cmp);
    bool ok = true;
    for (size_t i = 0; i < paths.count && ok; i++)
        ok = scan_one_depfile(root, paths.items[i], cb, user, &sha,
                              stat_out ? &stat_sha : NULL);
    dep_paths_free(&paths);
    if (!ok)
        LOG_FAIL("codeindex", "scan depfiles failed");
    sha3_256_finalize(&sha, exact_out);
    if (stat_out) sha3_256_finalize(&stat_sha, stat_out);
    return true;
}

bool ci_deps_scan(const char *root, ci_dep_cb cb, void *user,
                  uint8_t out_root[32])
{
    return deps_scan_exact(root, cb, user, out_root, NULL);
}

bool ci_deps_scan_roots(const char *root, ci_dep_cb cb, void *user,
                        uint8_t exact_out[32], uint8_t stat_out[32])
{
    if (!stat_out)
        LOG_FAIL("codeindex", "null dep stat root output");
    return deps_scan_exact(root, cb, user, exact_out, stat_out);
}

bool ci_deps_stat_root_sha3(const char *root, uint8_t out_root[32])
{
    if (!root || !out_root)
        LOG_FAIL("codeindex", "null arg to deps_stat_root");
    char build[CI_PATH_MAX];
    int bn = snprintf(build, sizeof(build), "%s/build", root);
    if (bn <= 0 || (size_t)bn >= sizeof(build))
        LOG_FAIL("codeindex", "build path too long");
    struct stat build_st;
    bool present = lstat(build, &build_st) == 0;
    if (!present && errno != ENOENT)
        LOG_FAIL("codeindex", "inspect build directory failed: %s",
                 strerror(errno));
    if (present && !S_ISDIR(build_st.st_mode))
        LOG_FAIL("codeindex", "build path is not a directory");

    struct sha3_256_ctx sha;
    dep_stat_root_init(&sha, present);
    if (!present) {
        sha3_256_finalize(&sha, out_root);
        return true;
    }

    struct dep_paths paths = {0};
    if (!collect_dep_paths(root, "build", &paths)) {
        dep_paths_free(&paths);
        LOG_FAIL("codeindex", "collect depfile metadata failed: %s",
                 strerror(errno));
    }
    qsort(paths.items, paths.count, sizeof(paths.items[0]), dep_path_cmp);
    bool ok = true;
    for (size_t i = 0; i < paths.count; i++) {
        char full[CI_PATH_MAX];
        int fn = snprintf(full, sizeof(full), "%s/%s", root, paths.items[i]);
        struct stat st;
        if (fn <= 0 || (size_t)fn >= sizeof(full) ||
            lstat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            ok = false;
            break;
        }
        dep_stat_root_add(&sha, paths.items[i], &st);
    }
    dep_paths_free(&paths);
    if (!ok)
        LOG_FAIL("codeindex", "inspect depfile metadata failed: %s",
                 strerror(errno ? errno : EIO));
    sha3_256_finalize(&sha, out_root);
    return true;
}
