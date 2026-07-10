/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tier 1 in-process hot-swap loader (DEV-ONLY dlopen path). See
 * hotswap/hotswap.h for the contract and docs/work/HOTSWAP.md for the
 * end-to-end recipe.
 *
 * Structure:
 *   - Pure helpers (path/datadir predicates) + the generation registry +
 *     the zcl_state dumper compile in ALL builds. They contain no dlopen /
 *     dlsym, so they are safe in release and unit-testable without loading
 *     a real .so.
 *   - hotswap_load()'s ACTUAL dlopen/dlsym/gen_init machinery lives inside
 *     `#ifdef ZCL_DEV_BUILD`. In release the function is a stub that refuses
 *     with "hotswap unavailable in release" and performs no dynamic load.
 *
 * The check-hotswap-dev-only lint gate enforces that dlopen/dlsym never
 * escape the ZCL_DEV_BUILD region below.
 */

#include "hotswap/hotswap.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Generation registry (always compiled) ───────────────────────── */

#define HOTSWAP_MAX_GENERATIONS 128

struct hotswap_generation {
    uint32_t gen;
    char     so_path[512];
    void    *handle;           /* dlopen handle (NULL in release); never closed */
    time_t   loaded_at;
    size_t   replaced_count;
    bool     ok;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hotswap_generation g_gens[HOTSWAP_MAX_GENERATIONS];
static size_t   g_gen_count = 0;
static uint32_t g_next_gen  = 1;

size_t hotswap_generation_count(void)
{
    pthread_mutex_lock(&g_lock);
    size_t n = g_gen_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ── Pure predicates (always compiled) ───────────────────────────── */

static bool has_suffix(const char *s, const char *suf)
{
    if (!s || !suf) return false;
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && memcmp(s + ls - lf, suf, lf) == 0;
}

bool hotswap_path_is_acceptable(const char *so_path, char *why, size_t why_sz)
{
    if (why && why_sz) why[0] = '\0';
    if (!so_path || !so_path[0]) {
        if (why) snprintf(why, why_sz, "empty path");
        return false;
    }
    if (so_path[0] != '/') {
        if (why) snprintf(why, why_sz, "path must be absolute");
        return false;
    }
    if (strstr(so_path, "..")) {
        if (why) snprintf(why, why_sz, "path must not contain '..'");
        return false;
    }
    if (!has_suffix(so_path, ".so")) {
        if (why) snprintf(why, why_sz, "path must end in .so");
        return false;
    }
    if (access(so_path, R_OK) != 0) {
        if (why) snprintf(why, why_sz, "file does not exist / unreadable");
        return false;
    }
    /* Resolve and confine to /tmp or a repo build/hotswap dir so a stray
     * MCP call cannot dlopen an arbitrary attacker-planted library. */
    char real[PATH_MAX];
    if (!realpath(so_path, real)) {
        if (why) snprintf(why, why_sz, "realpath failed");
        return false;
    }
    bool under_tmp = strncmp(real, "/tmp/", 5) == 0;
    bool under_build = strstr(real, "/build/hotswap") != NULL;
    if (!under_tmp && !under_build) {
        if (why)
            snprintf(why, why_sz,
                     "resolved path %s not under /tmp or a build/hotswap dir",
                     real);
        return false;
    }
    return true;
}

/* Expand "~/<tail>" against $HOME into buf. */
static void home_path(const char *tail, char *buf, size_t buf_sz)
{
    const char *home = getenv("HOME");
    snprintf(buf, buf_sz, "%s/%s", home ? home : "", tail);
}

/* True if two directory paths denote the same location (realpath when both
 * exist, else a byte compare with any single trailing slash trimmed). */
static bool same_dir(const char *a, const char *b)
{
    if (!a || !b) return false;
    char ra[PATH_MAX], rb[PATH_MAX];
    const char *pa = realpath(a, ra) ? ra : a;
    const char *pb = realpath(b, rb) ? rb : b;
    size_t la = strlen(pa), lb = strlen(pb);
    if (la && pa[la - 1] == '/') la--;
    if (lb && pb[lb - 1] == '/') lb--;
    return la == lb && strncmp(pa, pb, la) == 0;
}

bool hotswap_datadir_is_dev(const char *resolved_datadir)
{
    /* Refuse the canonical live datadir and the legacy zclassicd datadir;
     * everything else (empty, dev/test/soak copies) is treated as dev. */
    if (!resolved_datadir || !resolved_datadir[0])
        return true;
    char canonical[PATH_MAX], legacy[PATH_MAX];
    home_path(".zclassic-c23", canonical, sizeof(canonical));
    home_path(".zclassic", legacy, sizeof(legacy));
    if (same_dir(resolved_datadir, canonical))
        return false;
    if (same_dir(resolved_datadir, legacy))
        return false;
    return true;
}

/* ── zcl_state subsystem=hotswap (always compiled) ───────────────── */

bool hotswap_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

#ifdef ZCL_DEV_BUILD
    json_push_kv_bool(out, "available", true);
#else
    json_push_kv_bool(out, "available", false);
    json_push_kv_str(out, "note", "hotswap unavailable in release build");
#endif

    pthread_mutex_lock(&g_lock);
    json_push_kv_int(out, "generation_count", (int64_t)g_gen_count);
    json_push_kv_int(out, "next_gen", (int64_t)g_next_gen);

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < g_gen_count; i++) {
        const struct hotswap_generation *g = &g_gens[i];
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_int(&obj, "gen", (int64_t)g->gen);
        json_push_kv_str(&obj, "so_path", g->so_path);
        json_push_kv_int(&obj, "loaded_at", (int64_t)g->loaded_at);
        json_push_kv_int(&obj, "replaced_count", (int64_t)g->replaced_count);
        json_push_kv_bool(&obj, "ok", g->ok);
        json_push_back(&arr, &obj);
        json_free(&obj);
    }
    pthread_mutex_unlock(&g_lock);

    json_push_kv(out, "generations", &arr);
    json_free(&arr);
    return true;
}

/* ── Load path ───────────────────────────────────────────────────── */

#ifdef ZCL_DEV_BUILD

#include <dlfcn.h>

/* Register a generation slot (caller holds g_lock). Returns the slot or
 * NULL if the registry is full. */
static struct hotswap_generation *hotswap_registry_add(uint32_t gen,
                                                       const char *so_path,
                                                       void *handle)
{
    if (g_gen_count >= HOTSWAP_MAX_GENERATIONS)
        return NULL;
    struct hotswap_generation *g = &g_gens[g_gen_count++];
    memset(g, 0, sizeof(*g));
    g->gen = gen;
    snprintf(g->so_path, sizeof(g->so_path), "%s", so_path ? so_path : "");
    g->handle = handle;
    g->loaded_at = platform_time_wall_time_t();
    g->ok = false;
    return g;
}

/* Shared prologue: validate inputs before touching any dynamic loader.
 * Fills report->error and returns false on rejection. */
static bool hotswap_load_precheck(const char *so_path,
                                  const char *resolved_datadir,
                                  zcl_hotswap_replace_cb replace_cb,
                                  struct hotswap_load_report *report)
{
    if (!report)
        return false;
    memset(report, 0, sizeof(*report));

    if (!replace_cb) {
        snprintf(report->error, sizeof(report->error),
                 "no route-replacement callback supplied");
        return false;
    }
    if (!hotswap_datadir_is_dev(resolved_datadir)) {
        snprintf(report->error, sizeof(report->error),
                 "refusing hot-swap on canonical/live datadir '%s'",
                 resolved_datadir ? resolved_datadir : "");
        LOG_WARN("hotswap", "%s", report->error);
        return false;
    }
    char why[192] = {0};
    if (!hotswap_path_is_acceptable(so_path, why, sizeof(why))) {
        snprintf(report->error, sizeof(report->error),
                 "rejected so_path: %s", why);
        LOG_WARN("hotswap", "%s", report->error);
        return false;
    }
    return true;
}

/* Loads are serialized under g_lock, so a single file-static context lets
 * the C-callable host thunk (no closures in C) reach the in-progress
 * report + the caller's replace callback. */
static struct hotswap_load_report *g_active_report = NULL;
static zcl_hotswap_replace_cb      g_active_cb     = NULL;

/* host->mcp_replace thunk: publish via the caller's callback and record the
 * published tool name into the active report. */
static bool hotswap_replace_thunk(const char *name,
                                  const struct mcp_tool_route *route)
{
    if (!g_active_cb || !g_active_report)
        return false;
    bool ok = g_active_cb(name, route);
    if (ok && name) {
        struct hotswap_load_report *r = g_active_report;
        if (r->replaced_count < ZCL_HOTSWAP_GEN_MAX_REPLACED) {
            snprintf(r->replaced[r->replaced_count],
                     sizeof(r->replaced[0]), "%s", name);
            r->replaced_count++;
        } else {
            r->replaced_overflow = true;
        }
    }
    return ok;
}

bool hotswap_load(const char *so_path,
                  const char *resolved_datadir,
                  zcl_hotswap_replace_cb replace_cb,
                  struct hotswap_load_report *report)
{
    if (!hotswap_load_precheck(so_path, resolved_datadir, replace_cb, report))
        return false;

    pthread_mutex_lock(&g_lock);

    /* 1. dlopen. RTLD_LOCAL keeps the .so's symbols out of global scope
     *    (only the executable's -rdynamic kernel symbols are shared into
     *    the .so). RTLD_NOW surfaces unresolved symbols now, not later. */
    void *handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *e = dlerror();
        snprintf(report->error, sizeof(report->error),
                 "dlopen failed: %s", e ? e : "(unknown)");
        LOG_WARN("hotswap", "%s", report->error);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    /* 2. Resolve the well-known generation entrypoint. Nothing is published
     *    yet, so on failure we may safely dlclose this handle. */
    dlerror();
    /* POSIX object->function pointer round-trip via a void* lvalue avoids the
     * ISO C -Wpedantic diagnostic on a direct cast of dlsym's result. */
    zcl_hotswap_gen_init_fn gen_init = NULL;
    *(void **)(&gen_init) = dlsym(handle, "zcl_hotswap_gen_init");
    const char *sym_err = dlerror();
    if (!gen_init || sym_err) {
        snprintf(report->error, sizeof(report->error),
                 "missing zcl_hotswap_gen_init: %s",
                 sym_err ? sym_err : "symbol not found");
        LOG_WARN("hotswap", "%s", report->error);
        dlclose(handle);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    /* 3. Register the generation BEFORE calling gen_init: once gen_init runs
     *    it may publish routes pointing into this .so, so the handle must be
     *    tracked (and never closed) from here on. */
    uint32_t gen = g_next_gen++;
    struct hotswap_generation *slot =
        hotswap_registry_add(gen, so_path, handle);
    if (!slot) {
        snprintf(report->error, sizeof(report->error),
                 "generation registry full (%d)", HOTSWAP_MAX_GENERATIONS);
        LOG_WARN("hotswap", "%s", report->error);
        /* Nothing published yet; safe to close. */
        dlclose(handle);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    report->gen = gen;

    /* 4. Run gen_init with the host vtable. Its host->mcp_replace stages
     *    each replacement and records the name. */
    struct zcl_hotswap_host host = {
        .gen = gen,
        .mcp_replace = hotswap_replace_thunk,
    };
    g_active_report = report;
    g_active_cb = replace_cb;
    bool init_ok = gen_init(&host);
    g_active_report = NULL;
    g_active_cb = NULL;

    slot->replaced_count = report->replaced_count;
    slot->ok = init_ok;
    report->ok = init_ok;

    if (!init_ok) {
        /* v1 semantics: a partial failure leaves already-published entries
         * in place (they point at valid code in this kept-alive .so). Full
         * transactionality (stage-then-commit-all) is a later refinement. */
        snprintf(report->error, sizeof(report->error),
                 "gen_init reported failure after %zu replacement(s); "
                 "already-published routes were kept (v1 non-transactional)",
                 report->replaced_count);
        LOG_WARN("hotswap", "%s", report->error);
    }

    pthread_mutex_unlock(&g_lock);
    return init_ok;
}

#else /* !ZCL_DEV_BUILD — release: no dynamic loading whatsoever */

bool hotswap_load(const char *so_path,
                  const char *resolved_datadir,
                  zcl_hotswap_replace_cb replace_cb,
                  struct hotswap_load_report *report)
{
    (void)so_path;
    (void)resolved_datadir;
    (void)replace_cb;
    if (!report)
        return false;
    memset(report, 0, sizeof(*report));
    snprintf(report->error, sizeof(report->error),
             "hotswap unavailable in release build");
    return false;
}

#endif /* ZCL_DEV_BUILD */
