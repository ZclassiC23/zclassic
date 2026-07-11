/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * vcs_walk — implementation. See vcs_walk.h. */

#include "vcs_walk.h"

#include "vcs/vcs_object.h"
#include "vcs_priv.h"

#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Return the final path component of relpath. */
static const char *base_of(const char *relpath)
{
    const char *slash = strrchr(relpath, '/');
    return slash ? slash + 1 : relpath;
}

/* Does relpath (or any ancestor component) equal one of the ignored dir
 * names, or start with an ignored dir prefix? */
static bool has_dir_component(const char *relpath, const char *comp)
{
    size_t clen = strlen(comp);
    const char *p = relpath;
    while (*p) {
        /* p points at the start of a component. */
        if (strncmp(p, comp, clen) == 0 && (p[clen] == '/' || p[clen] == '\0'))
            return true;
        const char *slash = strchr(p, '/');
        if (!slash)
            break;
        p = slash + 1;
    }
    return false;
}

bool vcs_path_ignored(const char *relpath)
{
    if (!relpath || !relpath[0])
        return true;

    /* dir prefixes / components */
    if (has_dir_component(relpath, ".git"))
        return true;
    if (has_dir_component(relpath, ".zvcs"))
        return true;
    if (has_dir_component(relpath, "build"))
        return true;
    if (strncmp(relpath, "vendor/lib/", 11) == 0 ||
        strcmp(relpath, "vendor/lib") == 0)
        return true;

    const char *b = base_of(relpath);
    size_t blen = strlen(b);
    /* *.db */
    if (blen >= 3 && strcmp(b + blen - 3, ".db") == 0)
        return true;
    /* node.db* (covers node.db, node.db-wal, node.db-shm) */
    if (strncmp(b, "node.db", 7) == 0)
        return true;
    /* test-tmp* */
    if (strncmp(b, "test-tmp", 8) == 0)
        return true;

    return false;
}

struct walk_ctx {
    const char  *repo_root;
    vcs_walk_cb  cb;
    void        *user;
    bool         aborted;
};

/* Recurse into <repo_root>/<rel> (rel="" for the root). */
static bool walk_dir(struct walk_ctx *w, const char *rel)
{
    char full[4096];
    int n;
    if (rel[0])
        n = snprintf(full, sizeof(full), "%s/%s", w->repo_root, rel);
    else
        n = snprintf(full, sizeof(full), "%s", w->repo_root);
    if (n <= 0 || (size_t)n >= sizeof(full))
        LOG_FAIL("vcs", "walk path too long");

    DIR *d = opendir(full);
    if (!d)
        LOG_FAIL("vcs", "opendir %s: %s", full, strerror(errno));

    /* Collect names first (sorted) so the walk order is deterministic. */
    char **names = NULL;
    size_t ncount = 0, ncap = 0;
    struct dirent *de;
    bool ok = true;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (ncount == ncap) {
            size_t ncap2 = ncap ? ncap * 2 : 64;
            char **nn = zcl_realloc(names, ncap2 * sizeof(*nn), "vcs_walk_names");
            if (!nn) { ok = false; break; }
            names = nn;
            ncap = ncap2;
        }
        names[ncount] = zcl_strdup(de->d_name, "vcs_walk_name");
        if (!names[ncount]) { ok = false; break; }
        ncount++;
    }
    closedir(d);
    if (!ok) {
        for (size_t i = 0; i < ncount; i++) free(names[i]);
        free(names);
        LOG_FAIL("vcs", "readdir alloc failed");
    }

    /* deterministic order */
    for (size_t i = 0; i + 1 < ncount; i++)
        for (size_t j = i + 1; j < ncount; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char *t = names[i]; names[i] = names[j]; names[j] = t;
            }

    for (size_t i = 0; i < ncount && ok && !w->aborted; i++) {
        char childrel[4096];
        int cn;
        if (rel[0])
            cn = snprintf(childrel, sizeof(childrel), "%s/%s", rel, names[i]);
        else
            cn = snprintf(childrel, sizeof(childrel), "%s", names[i]);
        if (cn <= 0 || (size_t)cn >= sizeof(childrel))
            continue;  /* skip pathological over-long names */

        if (vcs_path_ignored(childrel))
            continue;

        char childfull[4096];
        int cfn = snprintf(childfull, sizeof(childfull), "%s/%s",
                           w->repo_root, childrel);
        if (cfn <= 0 || (size_t)cfn >= sizeof(childfull))
            continue;

        struct stat st;
        if (lstat(childfull, &st) != 0)
            continue;  /* vanished mid-walk; skip */

        if (S_ISDIR(st.st_mode)) {
            if (!walk_dir(w, childrel))
                ok = false;
        } else if (S_ISREG(st.st_mode)) {
            int64_t mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 +
                               st.st_mtim.tv_nsec;
            int64_t ctime_ns = (int64_t)st.st_ctim.tv_sec * 1000000000 +
                               st.st_ctim.tv_nsec;
            if (!w->cb(childrel, (uint32_t)st.st_mode, (uint64_t)st.st_size,
                       mtime_ns, ctime_ns, w->user)) {
                w->aborted = true;
            }
        }
        /* symlinks / fifos / sockets: not tracked in v1 */
    }

    for (size_t i = 0; i < ncount; i++) free(names[i]);
    free(names);
    return ok;
}

bool vcs_walk_tracked(const char *repo_root, vcs_walk_cb cb, void *user)
{
    if (!repo_root || !cb)
        LOG_FAIL("vcs", "null arg to walk_tracked");
    struct walk_ctx w = { repo_root, cb, user, false };
    if (!walk_dir(&w, ""))
        return false;
    return !w.aborted;
}

bool vcs_blob_hash_file(const char *repo_root, const char *relpath,
                        uint8_t out[32])
{
    if (!repo_root || !relpath || !out)
        LOG_FAIL("vcs", "null arg to blob_hash_file");
    char full[4096];
    int n = snprintf(full, sizeof(full), "%s/%s", repo_root, relpath);
    if (n <= 0 || (size_t)n >= sizeof(full))
        LOG_FAIL("vcs", "blob path too long");

    int fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        LOG_FAIL("vcs", "open %s: %s", full, strerror(errno));

    uint8_t tag = VCS_TAG_BLOB;
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, &tag, 1);

    unsigned char buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            LOG_FAIL("vcs", "read %s: %s", full, strerror(errno));
        }
        if (r == 0)
            break;
        sha3_256_write(&ctx, buf, (size_t)r);
    }
    close(fd);
    sha3_256_finalize(&ctx, out);
    return true;
}
