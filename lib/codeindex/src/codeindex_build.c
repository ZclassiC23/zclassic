/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex_build — deterministic full rebuild + staleness.
 *
 * Enumerate the source set in a fixed sorted order, scan every file, fold in
 * include edges from build depfiles, write the group hierarchy, all into a
 * fresh <root>/.codeindex/index.kv.tmp, then atomically rename it over
 * index.kv and stamp meta.source_root_sha3. A partial build never corrupts the
 * live store: it only ever appears via the final rename. "Recompute, never
 * repair." */

#include "codeindex_priv.h"
#include "codeindex/codeindex_build.h"

#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── source enumeration ─────────────────────────────────────────────── */

struct strvec {
    char  **v;
    size_t  n;
    size_t  cap;
};

static bool sv_push(struct strvec *s, const char *str)
{
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 256;
        char **nv = zcl_realloc(s->v, ncap * sizeof(*nv), "ci_strvec");
        if (!nv) return false;
        s->v = nv; s->cap = ncap;
    }
    s->v[s->n] = zcl_strdup(str, "ci_relpath");
    if (!s->v[s->n]) return false;
    s->n++;
    return true;
}

static void sv_free(struct strvec *s)
{
    for (size_t i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL; s->n = s->cap = 0;
}

static int sv_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool is_source_name(const char *name)
{
    size_t n = strlen(name);
    if (n >= 2 && name[n - 2] == '.' && name[n - 1] == 'c') return true;
    if (n >= 2 && name[n - 2] == '.' && name[n - 1] == 'h') return true;
    return false;
}

/* pruned directory names — never descend these */
static bool prune_dir(const char *name)
{
    return strcmp(name, ".git") == 0 || strcmp(name, ".codeindex") == 0 ||
           strcmp(name, ".zvcs") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "bin") == 0 || strcmp(name, "obj") == 0 ||
           strcmp(name, ".cache") == 0 || strcmp(name, "vendor") == 0 ||
           strncmp(name, "test-tmp", 8) == 0;
}

/* Recursively collect .c/.h under <root>/<reldir> into vec. */
static void collect_dir(const char *root, const char *reldir, struct strvec *vec)
{
    char full[CI_PATH_MAX];
    if (reldir[0])
        snprintf(full, sizeof(full), "%s/%s", root, reldir);
    else
        snprintf(full, sizeof(full), "%s", root);
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char child[CI_PATH_MAX];
        int n;
        if (reldir[0])
            n = snprintf(child, sizeof(child), "%s/%s", reldir, e->d_name);
        else
            n = snprintf(child, sizeof(child), "%s", e->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;
        char cfull[CI_PATH_MAX];
        snprintf(cfull, sizeof(cfull), "%s/%s", root, child);
        struct stat st;
        if (lstat(cfull, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (prune_dir(e->d_name)) continue;
            collect_dir(root, child, vec);
        } else if (S_ISREG(st.st_mode) && is_source_name(e->d_name)) {
            sv_push(vec, child);
        }
    }
    closedir(d);
}

bool ci_enumerate_sources(const char *root, ci_enum_cb cb, void *user)
{
    if (!root || !cb) LOG_FAIL("codeindex", "null arg to enumerate");

    struct strvec vec = {0};

    /* lib/<mod>/{src,include} */
    size_t nmod = 0;
    const char *const *mods = ci_lib_modules(&nmod);
    for (size_t i = 0; i < nmod; i++) {
        char rel[CI_PATH_MAX];
        snprintf(rel, sizeof(rel), "lib/%s/src", mods[i]);
        collect_dir(root, rel, &vec);
        snprintf(rel, sizeof(rel), "lib/%s/include", mods[i]);
        collect_dir(root, rel, &vec);
    }
    /* app/<shape>/{src,include} */
    size_t nsh = 0;
    const char *const *shapes = ci_app_shapes(&nsh);
    for (size_t i = 0; i < nsh; i++) {
        char rel[CI_PATH_MAX];
        snprintf(rel, sizeof(rel), "app/%s/src", shapes[i]);
        collect_dir(root, rel, &vec);
        snprintf(rel, sizeof(rel), "app/%s/include", shapes[i]);
        collect_dir(root, rel, &vec);
    }
    /* the standalone roots */
    collect_dir(root, "core", &vec);
    collect_dir(root, "config/src", &vec);
    collect_dir(root, "config/include", &vec);
    collect_dir(root, "tools", &vec);
    collect_dir(root, "domain", &vec);
    collect_dir(root, "adapters", &vec);
    collect_dir(root, "ports", &vec);
    collect_dir(root, "src", &vec);

    /* Tests are not a production LIB_MODULES entry, but they are source a
     * developer must be able to navigate.  collect_dir() retains the same
     * generated-directory pruning used above; the sorted pass below de-dups
     * this root if it ever becomes a listed library module. */
    collect_dir(root, "lib/test", &vec);

    qsort(vec.v, vec.n, sizeof(vec.v[0]), sv_cmp);

    bool ok = true;
    for (size_t i = 0; i < vec.n && ok; i++) {
        /* de-dup exact repeats (a dir listed twice can't happen here, but be safe) */
        if (i > 0 && strcmp(vec.v[i], vec.v[i - 1]) == 0) continue;
        char full[CI_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", root, vec.v[i]);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        int64_t mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000ll +
                           st.st_mtim.tv_nsec;
        if (!cb(vec.v[i], mtime_ns, (int64_t)st.st_size, user))
            ok = false;
    }
    sv_free(&vec);
    return ok;
}

/* ── staleness stamp: cheap stat-based tree hash ────────────────────── */

struct stamp_ctx { struct sha3_256_ctx sha; };

static bool stamp_cb(const char *relpath, int64_t mtime_ns, int64_t size,
                     void *user)
{
    struct stamp_ctx *c = user;
    uint8_t buf[16];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((uint64_t)mtime_ns >> (8 * i));
    for (int i = 0; i < 8; i++) buf[8 + i] = (uint8_t)((uint64_t)size >> (8 * i));
    sha3_256_write(&c->sha, (const unsigned char *)relpath, strlen(relpath) + 1);
    sha3_256_write(&c->sha, buf, sizeof(buf));
    return true;
}

bool codeindex_source_root_sha3(const char *root, uint8_t out[32])
{
    if (!root || !out) LOG_FAIL("codeindex", "null arg to source_root_sha3");
    struct stamp_ctx c;
    sha3_256_init(&c.sha);
    static const uint8_t tag = 0x03;
    sha3_256_write(&c.sha, &tag, 1);
    if (!ci_enumerate_sources(root, stamp_cb, &c))
        LOG_FAIL("codeindex", "enumerate for stamp failed");
    sha3_256_finalize(&c.sha, out);
    return true;
}

bool codeindex_is_stale(struct codeindex *ci, bool *stale)
{
    if (stale) *stale = true;
    if (!ci || !ci->store) LOG_FAIL("codeindex", "null arg to is_stale");
    uint8_t cur[32];
    if (!codeindex_source_root_sha3(ci->root, cur))
        LOG_FAIL("codeindex", "compute source_root_sha3");
    uint8_t stored[32];
    size_t olen = 0; bool found = false;
    if (!ci_store_meta_get(ci->store, "source_root_sha3", stored,
                           sizeof(stored), &olen, &found))
        LOG_FAIL("codeindex", "meta_get source_root_sha3");
    if (stale)
        *stale = !(found && olen == 32 && memcmp(cur, stored, 32) == 0);
    return true;
}

/* ── rebuild ────────────────────────────────────────────────────────── */

/* path → file_id map (sorted, bsearch) */
struct idmap_ent { char path[256]; int64_t id; };
struct build_ctx {
    struct ci_store   *store;
    bool               err;
    struct idmap_ent  *ids;
    size_t             nids, cap_ids;
};

static void on_sym_cb(const struct ci_symbol *sym, void *user)
{
    struct build_ctx *b = user;
    if (b->err) return;
    if (!ci_store_put_symbol(b->store, sym)) b->err = true;
}

static void on_ref_cb(const char *callee, const char *ref_file, int ref_line,
                      void *user)
{
    struct build_ctx *b = user;
    if (b->err) return;
    if (!ci_store_put_ref(b->store, callee, ref_file, ref_line)) b->err = true;
}

static bool idmap_push(struct build_ctx *b, const char *path, int64_t id)
{
    if (b->nids == b->cap_ids) {
        size_t ncap = b->cap_ids ? b->cap_ids * 2 : 512;
        void *nb = zcl_realloc(b->ids, ncap * sizeof(*b->ids), "ci_idmap");
        if (!nb) return false;
        b->ids = nb; b->cap_ids = ncap;
    }
    snprintf(b->ids[b->nids].path, sizeof(b->ids[b->nids].path), "%s", path);
    b->ids[b->nids].id = id;
    b->nids++;
    return true;
}

static int idmap_cmp(const void *a, const void *b)
{
    return strcmp(((const struct idmap_ent *)a)->path,
                  ((const struct idmap_ent *)b)->path);
}

static int64_t idmap_find(const struct build_ctx *b, const char *path)
{
    size_t lo = 0, hi = b->nids;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(b->ids[mid].path, path);
        if (c == 0) return b->ids[mid].id;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* the per-file enumeration callback needs root; carry it alongside build_ctx. */
struct build_file_env { struct build_ctx *b; const char *root; };

static bool build_file_cb2(const char *relpath, int64_t mtime_ns, int64_t size,
                           void *user)
{
    (void)size;
    struct build_file_env *env = user;
    struct build_ctx *b = env->b;
    if (b->err) return false;

    uint8_t sha[32];
    char purpose[160] = "";
    if (!ci_scan_file(env->root, relpath, on_sym_cb, on_ref_cb, b, sha,
                      purpose)) {
        b->err = true;
        return false;
    }
    struct ci_file f;
    memset(&f, 0, sizeof(f));
    snprintf(f.path, sizeof(f.path), "%s", relpath);
    ci_group_for_path(relpath, f.group);
    /* self-description derived from the file's leading comment (§1.1). */
    snprintf(f.purpose, sizeof(f.purpose), "%s", purpose);
    int64_t id = -1;
    if (!ci_store_put_file(b->store, &f, sha, mtime_ns, &id)) {
        b->err = true;
        return false;
    }
    if (!idmap_push(b, relpath, id)) { b->err = true; return false; }
    return true;
}

static void on_dep_cb(const char *src_relpath, const char *dep_relpath,
                      void *user)
{
    struct build_ctx *b = user;
    if (b->err) return;
    int64_t id = idmap_find(b, src_relpath);
    if (id < 0) return;  /* source not in our indexed set — skip */
    if (!ci_store_put_include(b->store, id, dep_relpath)) b->err = true;
}

bool codeindex_rebuild(struct codeindex *ci)
{
    if (!ci) LOG_FAIL("codeindex", "null ci to rebuild");

    char dir[CI_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.codeindex", ci->root);
    mkdir(dir, 0755);

    char dbpath[CI_PATH_MAX], tmp[CI_PATH_MAX], wal[CI_PATH_MAX], shm[CI_PATH_MAX];
    snprintf(dbpath, sizeof(dbpath), "%s/index.kv", dir);
    snprintf(tmp, sizeof(tmp), "%s/index.kv.tmp", dir);
    snprintf(wal, sizeof(wal), "%s/index.kv.tmp-wal", dir);
    snprintf(shm, sizeof(shm), "%s/index.kv.tmp-shm", dir);
    unlink(tmp); unlink(wal); unlink(shm);

    struct ci_store *st = ci_store_open_path(tmp);
    if (!st) LOG_FAIL("codeindex", "open tmp store");

    struct build_ctx b;
    memset(&b, 0, sizeof(b));
    b.store = st;

    bool ok = true;
    if (!ci_store_begin(st)) { ci_store_close(st); LOG_FAIL("codeindex", "begin tmp"); }
    if (!ci_store_clear(st)) ok = false;
    if (ok && !ci_group_emit_all(st)) ok = false;

    struct build_file_env env = { &b, ci->root };
    if (ok && !ci_enumerate_sources(ci->root, build_file_cb2, &env)) ok = false;
    if (b.err) ok = false;

    /* include edges from build depfiles */
    if (ok) {
        qsort(b.ids, b.nids, sizeof(b.ids[0]), idmap_cmp);
        ci_deps_scan(ci->root, on_dep_cb, &b);
        if (b.err) ok = false;
    }

    /* staleness stamp */
    if (ok) {
        uint8_t stamp[32];
        if (codeindex_source_root_sha3(ci->root, stamp)) {
            if (!ci_store_meta_set(st, "source_root_sha3", stamp, 32)) ok = false;
        } else ok = false;
    }

    if (!ok) {
        ci_store_rollback(st);
        ci_store_close(st);
        free(b.ids);
        unlink(tmp); unlink(wal); unlink(shm);
        LOG_FAIL("codeindex", "rebuild failed; tmp discarded");
    }
    if (!ci_store_commit(st)) {
        ci_store_close(st);
        free(b.ids);
        unlink(tmp); unlink(wal); unlink(shm);
        LOG_FAIL("codeindex", "commit tmp");
    }
    ci_store_close(st);   /* checkpoint(TRUNCATE) + close: tmp is standalone */
    free(b.ids);

    /* swap into place */
    char dwal[CI_PATH_MAX], dshm[CI_PATH_MAX];
    snprintf(dwal, sizeof(dwal), "%s/index.kv-wal", dir);
    snprintf(dshm, sizeof(dshm), "%s/index.kv-shm", dir);
    if (ci->store) { ci_store_close(ci->store); ci->store = NULL; }
    unlink(dbpath); unlink(dwal); unlink(dshm);
    unlink(wal); unlink(shm);  /* orphan tmp sidecars, if any */
    if (rename(tmp, dbpath) != 0) {
        LOG_FAIL("codeindex", "rename tmp -> index.kv");
    }

    ci->store = ci_store_open(ci->root);
    if (!ci->store) LOG_FAIL("codeindex", "reopen after rebuild");
    return true;
}
