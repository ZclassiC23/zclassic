/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex_group — the group hierarchy: how a repo-relative path maps to a
 * navigator group (lib/<mod>, app/<shape>, core, tools, config, domain,
 * adapters) and the fixed set of group nodes written into the store.
 *
 * The LIB_MODULES and shape lists here are the in-code MIRROR of the
 * Makefile's LIB_MODULES variable and the eight app/ shape folders. A parity
 * test (test_codeindex, case 6) asserts they stay in sync. Keep them sorted
 * the way the Makefile lists them is NOT required — the parity test compares
 * as sets. */

#include "codeindex_priv.h"
#include "codeindex/codeindex_build.h"

#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>

/* ── canonical mirrors (SINGLE source in code; parity-tested vs Makefile) ── */

static const char *const k_lib_modules[] = {
    "bloom", "chain", "coins", "core", "crypto", "crypto_registry", "encoding",
    "event", "framework", "health", "hotswap", "kernel", "json", "keys",
    "metrics", "mining", "net", "platform", "policy", "primitives", "rpc",
    "script", "sim", "storage", "support", "sync", "util", "validation",
    "vcs", "wallet", "sapling", "zslp", "znam", "codeindex",
};

static const char *const k_app_shapes[] = {
    "conditions", "controllers", "events", "jobs",
    "models", "services", "supervisors", "views",
};

const char *const *ci_lib_modules(size_t *count)
{
    if (count) *count = sizeof(k_lib_modules) / sizeof(k_lib_modules[0]);
    return k_lib_modules;
}

const char *const *ci_app_shapes(size_t *count)
{
    if (count) *count = sizeof(k_app_shapes) / sizeof(k_app_shapes[0]);
    return k_app_shapes;
}

/* ── path → group ────────────────────────────────────────────────────── */

/* Does relpath begin with "<seg>/"? */
static bool starts_seg(const char *relpath, const char *seg)
{
    size_t n = strlen(seg);
    return strncmp(relpath, seg, n) == 0 && relpath[n] == '/';
}

/* Copy the path component that follows "<top>/" into out (bounded). */
static void second_component(const char *relpath, const char *top, char out[64])
{
    out[0] = '\0';
    size_t n = strlen(top);
    if (strncmp(relpath, top, n) != 0 || relpath[n] != '/')
        return;
    const char *p = relpath + n + 1;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 0) return;
    if (len > 62) len = 62;
    snprintf(out, 64, "%s/%.*s", top, (int)len, p);
}

void ci_group_for_path(const char *relpath, char out[64])
{
    out[0] = '\0';
    if (!relpath || !relpath[0]) return;

    if (starts_seg(relpath, "lib")) { second_component(relpath, "lib", out); return; }
    if (starts_seg(relpath, "app")) { second_component(relpath, "app", out); return; }
    if (starts_seg(relpath, "domain")) { second_component(relpath, "domain", out); return; }
    if (starts_seg(relpath, "core")) { snprintf(out, 64, "core"); return; }
    if (starts_seg(relpath, "config")) { snprintf(out, 64, "config"); return; }
    if (starts_seg(relpath, "tools")) { snprintf(out, 64, "tools"); return; }
    if (starts_seg(relpath, "adapters")) { snprintf(out, 64, "adapters"); return; }
    if (starts_seg(relpath, "ports")) { snprintf(out, 64, "ports"); return; }
    /* top-level file (src/main.c etc.) */
    snprintf(out, 64, "root");
}

/* ── canned purposes for the well-known top groups ───────────────────── */

const char *ci_group_purpose(const char *group)
{
    if (!group || !group[0]) return "";
    if (strcmp(group, "lib") == 0) return "reusable node libraries (one whole-program TU each)";
    if (strcmp(group, "app") == 0) return "the eight application shapes (Rails-like feature slices)";
    if (strcmp(group, "core") == 0) return "sealed consensus core (params, chainparams, math, consensus)";
    if (strcmp(group, "config") == 0) return "boot configuration + argv wiring";
    if (strcmp(group, "tools") == 0) return "dev/ops tooling and the MCP surface";
    if (strcmp(group, "domain") == 0) return "pure framework-free bounded contexts";
    if (strcmp(group, "adapters") == 0) return "hexagonal adapters implementing ports/";
    if (strcmp(group, "ports") == 0) return "hexagonal interface headers";
    if (strcmp(group, "root") == 0) return "top-level entry (src/main.c) and repo root files";
    if (strcmp(group, "app/conditions") == 0) return "shape: liveness/blocker conditions";
    if (strcmp(group, "app/controllers") == 0) return "shape: REST + MCP + RPC request handlers";
    if (strcmp(group, "app/events") == 0) return "shape: domain events";
    if (strcmp(group, "app/jobs") == 0) return "shape: background jobs";
    if (strcmp(group, "app/models") == 0) return "shape: ActiveRecord models";
    if (strcmp(group, "app/services") == 0) return "shape: service objects (business logic)";
    if (strcmp(group, "app/supervisors") == 0) return "shape: liveness supervisors";
    if (strcmp(group, "app/views") == 0) return "shape: explorer/HTML/JSON views";
    return "";
}

/* ── emit the fixed hierarchy into an open txn ───────────────────────── */

static bool emit(struct ci_store *s, const char *path, const char *kind,
                 const char *parent)
{
    struct ci_group g;
    memset(&g, 0, sizeof(g));
    snprintf(g.path, sizeof(g.path), "%s", path);
    snprintf(g.kind, sizeof(g.kind), "%s", kind);
    snprintf(g.parent, sizeof(g.parent), "%s", parent);
    snprintf(g.purpose, sizeof(g.purpose), "%s", ci_group_purpose(path));
    return ci_store_put_group(s, &g);
}

bool ci_group_emit_all(struct ci_store *s)
{
    if (!s) LOG_FAIL("codeindex", "null store to emit_all");

    /* top-level roots */
    if (!emit(s, "root", "root", "")) return false;
    if (!emit(s, "lib", "root", "root")) return false;
    if (!emit(s, "app", "root", "root")) return false;
    if (!emit(s, "core", "core", "root")) return false;
    if (!emit(s, "config", "config", "root")) return false;
    if (!emit(s, "tools", "tools", "root")) return false;
    if (!emit(s, "domain", "root", "root")) return false;
    if (!emit(s, "adapters", "adapters", "root")) return false;
    if (!emit(s, "ports", "ports", "root")) return false;

    /* lib/<mod> */
    size_t nmod = 0;
    const char *const *mods = ci_lib_modules(&nmod);
    for (size_t i = 0; i < nmod; i++) {
        char path[64];
        snprintf(path, sizeof(path), "lib/%s", mods[i]);
        if (!emit(s, path, "lib", "lib")) return false;
    }

    /* app/<shape> */
    size_t nsh = 0;
    const char *const *shapes = ci_app_shapes(&nsh);
    for (size_t i = 0; i < nsh; i++) {
        char path[64];
        snprintf(path, sizeof(path), "app/%s", shapes[i]);
        if (!emit(s, path, "app-shape", "app")) return false;
    }
    return true;
}
