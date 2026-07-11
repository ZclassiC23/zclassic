/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the registry-owned `code` tree — the in-binary,
 * hierarchical, token-bounded source-code navigator. Each leaf opens the
 * lib/codeindex store (which self-rebuilds on open if the source tree is
 * stale), runs one query, and renders exactly one bounded JSON document within
 * ZCL_COMMAND_RESULT_BUDGET: a structured array plus compact human one-liners.
 *
 * Local, read-only, deterministic. Never bound to RPC/REST/MCP (native
 * transport only). The source of truth is IN-TREE SOURCE SCANNING, so these
 * answer "where is X / what calls X / what's in this file" without spending
 * tokens reading whole files.
 */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "kernel/command_registry.h"
#include "json/json.h"
#include "codeindex/codeindex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Conservative per-list caps. The whole reply envelope must fit in
 * ZCL_COMMAND_RESULT_BUDGET (4096 bytes), so lists stay small and each rendered
 * string is truncated. Overflow beyond a cap is reported, never silently cut. */
enum {
    CODE_SUBGROUP_CAP = 40,
    CODE_FILE_CAP     = 16,
    CODE_SYM_CAP      = 20,
    CODE_INC_CAP      = 16,
    CODE_REFS_DEFAULT = 5,
    CODE_REFS_MAX     = 20,
    CODE_FIND_DEFAULT = 5,
    CODE_FIND_MAX     = 12,
    CODE_OTHER_DEF_CAP = 5,
};

/* Bounded copy of at most `max` visible chars of `src` into dst[cap]; appends
 * "…"-as-"..." when truncated. Always NUL-terminates. */
static void code_trunc(char *dst, size_t cap, const char *src, size_t max)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t lim = max;
    if (lim > cap - 1) lim = cap - 1;
    size_t i = 0;
    for (; i < lim && src[i]; i++) dst[i] = src[i];
    if (src[i] != '\0' && i + 3 < cap) {
        dst[i++] = '.'; dst[i++] = '.'; dst[i++] = '.';
    }
    dst[i] = '\0';
}

/* The checkout root the index scans: an explicit context source_root wins, then
 * ZCL_DEV_SOURCE_ROOT, else the current directory. */
static const char *code_source_root(const struct zcl_command_request *request)
{
    if (request && request->context && request->context->source_root &&
        request->context->source_root[0])
        return request->context->source_root;
    const char *env = getenv("ZCL_DEV_SOURCE_ROOT");
    return env && env[0] ? env : ".";
}

/* Open the index or fail the reply with a bounded internal error. */
static struct codeindex *code_open(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    struct codeindex *ci = codeindex_open(code_source_root(request));
    if (!ci) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "CODEINDEX_OPEN",
                               "dispatch", true, false,
                               "could not open or rebuild the code index",
                               code_source_root(request));
    }
    return ci;
}

/* Positional/typed string input for `key` (NULL when absent/empty). */
static const char *code_str(const struct zcl_command_request *request,
                            const char *key)
{
    const char *v = json_get_str(json_get(request->input, key));
    return (v && v[0]) ? v : NULL;
}

/* Optional bounded "limit" input: clamp to [1, max], default `def`. */
static int code_limit(const struct zcl_command_request *request, int def, int max)
{
    const struct json_value *v = json_get(request->input, "limit");
    if (!v) return def;
    long n = (long)json_get_int(v);
    if (n < 1) n = 1;
    if (n > max) n = max;
    return (int)n;
}

/* Push one string onto a JSON array. */
static void code_push_line(struct json_value *arr, const char *s)
{
    struct json_value item;
    json_init(&item);
    json_set_str(&item, s);
    (void)json_push_back(arr, &item);
    json_free(&item);
}

/* Push a completed object onto an array (copies, then frees the local). */
static void code_push_obj(struct json_value *arr, struct json_value *obj)
{
    (void)json_push_back(arr, obj);
    json_free(obj);
}

/* ── code.group ─────────────────────────────────────────────────────────── */
void zcl_native_handle_code_group(const struct zcl_command_request *request,
                                  struct zcl_command_reply *reply)
{
    struct codeindex *ci = code_open(request, reply);
    if (!ci) return;

    const char *arg = code_str(request, "group");

    static struct ci_group groups[512];
    int ng = codeindex_groups(ci, groups, (int)(sizeof(groups) / sizeof(groups[0])));
    if (ng < 0) ng = 0;

    struct json_value list, lines;
    json_init(&list);  json_set_array(&list);
    json_init(&lines); json_set_array(&lines);

    if (!arg) {
        /* No arg: the top buckets (direct children of "root", plus root). */
        int shown = 0;
        for (int i = 0; i < ng; i++) {
            const char *p = groups[i].parent;
            bool top = (p[0] == '\0') || strcmp(p, "root") == 0;
            if (!top) continue;
            if (shown >= CODE_SUBGROUP_CAP) break;
            char purpose[80];
            code_trunc(purpose, sizeof(purpose), groups[i].purpose, 64);
            struct json_value o;
            json_init(&o); json_set_object(&o);
            (void)json_push_kv_str(&o, "path", groups[i].path);
            (void)json_push_kv_str(&o, "kind", groups[i].kind);
            (void)json_push_kv_str(&o, "purpose", purpose);
            code_push_obj(&list, &o);
            char line[160];
            (void)snprintf(line, sizeof(line), "%s%s%s", groups[i].path,
                           purpose[0] ? " — " : "", purpose);
            code_push_line(&lines, line);
            shown++;
        }
        (void)json_push_kv_str(&reply->data, "scope", "top");
        (void)json_push_kv(&reply->data, "groups", &list);
        (void)json_push_kv(&reply->data, "lines", &lines);
        (void)json_push_kv_int(&reply->data, "count", shown);
        char summary[128];
        (void)snprintf(summary, sizeof(summary),
                       "%d top source groups; run `code group <path>` to descend",
                       shown);
        (void)json_push_kv_str(&reply->data, "summary", summary);
        json_free(&list); json_free(&lines);
        codeindex_close(ci);
        return;
    }

    /* Arg given: that group's immediate subgroups, then its files. */
    int nsub = 0;
    for (int i = 0; i < ng && nsub < CODE_SUBGROUP_CAP; i++) {
        if (strcmp(groups[i].parent, arg) != 0) continue;
        struct json_value o;
        json_init(&o); json_set_object(&o);
        (void)json_push_kv_str(&o, "path", groups[i].path);
        (void)json_push_kv_str(&o, "kind", groups[i].kind);
        code_push_obj(&list, &o);
        code_push_line(&lines, groups[i].path);
        nsub++;
    }

    static struct ci_file files[CODE_FILE_CAP + 1];
    int nf = codeindex_files_in_group(ci, arg, files, CODE_FILE_CAP + 1);
    if (nf < 0) nf = 0;
    bool files_trunc = nf > CODE_FILE_CAP;
    if (files_trunc) nf = CODE_FILE_CAP;

    struct json_value farr;
    json_init(&farr); json_set_array(&farr);
    for (int i = 0; i < nf; i++) {
        char purpose[72];
        code_trunc(purpose, sizeof(purpose), files[i].purpose, 55);
        struct json_value o;
        json_init(&o); json_set_object(&o);
        (void)json_push_kv_str(&o, "path", files[i].path);
        (void)json_push_kv_str(&o, "purpose", purpose);
        code_push_obj(&farr, &o);
        char line[200];
        (void)snprintf(line, sizeof(line), "%s%s%s", files[i].path,
                       purpose[0] ? " — " : "", purpose);
        code_push_line(&lines, line);
    }

    (void)json_push_kv_str(&reply->data, "scope", "group");
    (void)json_push_kv_str(&reply->data, "group", arg);
    (void)json_push_kv(&reply->data, "subgroups", &list);
    (void)json_push_kv(&reply->data, "files", &farr);
    (void)json_push_kv(&reply->data, "lines", &lines);
    (void)json_push_kv_int(&reply->data, "subgroup_count", nsub);
    (void)json_push_kv_int(&reply->data, "file_count", nf);
    (void)json_push_kv_bool(&reply->data, "files_truncated", files_trunc);
    if (nsub == 0 && nf == 0)
        (void)json_push_kv_bool(&reply->data, "found", false);
    char summary[160];
    (void)snprintf(summary, sizeof(summary),
                   "group %s: %d subgroup(s), %d file(s)%s", arg, nsub, nf,
                   files_trunc ? " (more not shown)" : "");
    (void)json_push_kv_str(&reply->data, "summary", summary);

    json_free(&list); json_free(&farr); json_free(&lines);
    codeindex_close(ci);
}

/* ── code.file ──────────────────────────────────────────────────────────── */
void zcl_native_handle_code_file(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply)
{
    const char *path = code_str(request, "path");
    if (!path) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_PATH",
                               "normalize", false, false,
                               "code file requires a repo-relative path", "");
        return;
    }
    struct codeindex *ci = code_open(request, reply);
    if (!ci) return;

    struct ci_file finfo;
    bool ffound = false;
    (void)codeindex_file(ci, path, &finfo, &ffound);

    static struct ci_symbol syms[CODE_SYM_CAP + 1];
    int ns = codeindex_symbols_in_file(ci, path, syms, CODE_SYM_CAP + 1);
    if (ns < 0) ns = 0;
    bool syms_trunc = ns > CODE_SYM_CAP;
    if (syms_trunc) ns = CODE_SYM_CAP;

    static char incs[CODE_INC_CAP + 1][256];
    int ni = codeindex_includes_of_file(ci, path, incs, CODE_INC_CAP + 1);
    if (ni < 0) ni = 0;
    bool inc_trunc = ni > CODE_INC_CAP;
    if (inc_trunc) ni = CODE_INC_CAP;

    (void)json_push_kv_str(&reply->data, "path", path);
    (void)json_push_kv_str(&reply->data, "group", ffound ? finfo.group : "");
    if (ffound)
        (void)json_push_kv_str(&reply->data, "purpose", finfo.purpose);
    (void)json_push_kv_bool(&reply->data, "found",
                            ffound || ns > 0 || ni > 0);

    /* code.file emits ONLY the structured `symbols` array (the machine-readable
     * form); the redundant per-symbol human `lines` string is dropped so a large
     * file's reply fits the 4096-byte result budget. `signature` carries the
     * human-readable content; the CLI can render lines from the structured rows. */
    struct json_value sarr, iarr;
    json_init(&sarr);  json_set_array(&sarr);
    json_init(&iarr);  json_set_array(&iarr);

    for (int i = 0; i < ns; i++) {
        char sig[72];
        code_trunc(sig, sizeof(sig), syms[i].signature, 60);
        int line = syms[i].def_path[0] && strcmp(syms[i].def_path, path) == 0
                       ? syms[i].def_line : syms[i].decl_line;
        struct json_value o;
        json_init(&o); json_set_object(&o);
        (void)json_push_kv_str(&o, "name", syms[i].name);
        char kind[2] = { syms[i].kind, '\0' };
        (void)json_push_kv_str(&o, "kind", kind);
        (void)json_push_kv_int(&o, "line", line);
        (void)json_push_kv_str(&o, "signature", sig);
        if (syms[i].partial)
            (void)json_push_kv_bool(&o, "partial", true);
        code_push_obj(&sarr, &o);
    }
    for (int i = 0; i < ni; i++)
        code_push_line(&iarr, incs[i]);

    (void)json_push_kv(&reply->data, "symbols", &sarr);
    (void)json_push_kv(&reply->data, "includes", &iarr);
    (void)json_push_kv_int(&reply->data, "symbol_count", ns);
    (void)json_push_kv_int(&reply->data, "include_count", ni);
    (void)json_push_kv_bool(&reply->data, "symbols_truncated", syms_trunc);
    (void)json_push_kv_bool(&reply->data, "includes_truncated", inc_trunc);
    char summary[176];
    (void)snprintf(summary, sizeof(summary),
                   "%s: %d symbol(s)%s, %d include(s)%s", path, ns,
                   syms_trunc ? "+" : "", ni, inc_trunc ? "+" : "");
    (void)json_push_kv_str(&reply->data, "summary", summary);

    json_free(&sarr); json_free(&iarr);
    codeindex_close(ci);
}

/* ── code.sym ───────────────────────────────────────────────────────────── */
void zcl_native_handle_code_sym(const struct zcl_command_request *request,
                                struct zcl_command_reply *reply)
{
    const char *name = code_str(request, "name");
    if (!name) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_NAME",
                               "normalize", false, false,
                               "code sym requires a symbol name", "");
        return;
    }
    struct codeindex *ci = code_open(request, reply);
    if (!ci) return;

    struct ci_symbol s;
    bool found = false;
    (void)codeindex_symbol(ci, name, &s, &found);
    if (!found) {
        (void)json_push_kv_str(&reply->data, "name", name);
        (void)json_push_kv_bool(&reply->data, "found", false);
        char summary[160];
        (void)snprintf(summary, sizeof(summary),
                       "no indexed symbol named '%s'; try `code find %s`",
                       name, name);
        (void)json_push_kv_str(&reply->data, "summary", summary);
        codeindex_close(ci);
        return;
    }

    char sig[320], doc[224];
    code_trunc(sig, sizeof(sig), s.signature, 300);
    code_trunc(doc, sizeof(doc), s.doc, 200);
    char kind[2] = { s.kind, '\0' };

    (void)json_push_kv_bool(&reply->data, "found", true);
    (void)json_push_kv_str(&reply->data, "name", s.name);
    (void)json_push_kv_str(&reply->data, "kind", kind);
    (void)json_push_kv_str(&reply->data, "def_path", s.def_path);
    (void)json_push_kv_int(&reply->data, "def_line", s.def_line);
    (void)json_push_kv_str(&reply->data, "decl_path", s.decl_path);
    (void)json_push_kv_int(&reply->data, "decl_line", s.decl_line);
    (void)json_push_kv_str(&reply->data, "signature", sig);
    (void)json_push_kv_str(&reply->data, "doc", doc);
    (void)json_push_kv_str(&reply->data, "guard", s.guard);
    (void)json_push_kv_str(&reply->data, "group", s.group);
    if (s.partial)
        (void)json_push_kv_bool(&reply->data, "partial", true);

    /* The ~150-token rendered card as the human one-liner block. */
    char card[600];
    int cn = codeindex_render_card(ci, name, card, sizeof(card));
    if (cn > 0)
        (void)json_push_kv_str(&reply->data, "card", card);

    /* Other same-named definitions (overloads/statics in multiple files). */
    static struct ci_symbol hits[CODE_OTHER_DEF_CAP + 4];
    int nh = codeindex_find(ci, name, hits, (int)(sizeof(hits) / sizeof(hits[0])));
    if (nh < 0) nh = 0;
    struct json_value others;
    json_init(&others); json_set_array(&others);
    int shown = 0;
    for (int i = 0; i < nh && shown < CODE_OTHER_DEF_CAP; i++) {
        if (strcmp(hits[i].name, name) != 0) continue;              /* exact only */
        if (hits[i].def_path[0] == '\0') continue;                  /* bare decl */
        if (hits[i].def_line == s.def_line &&
            strcmp(hits[i].def_path, s.def_path) == 0) continue;    /* the primary */
        struct json_value o;
        json_init(&o); json_set_object(&o);
        (void)json_push_kv_str(&o, "def_path", hits[i].def_path);
        (void)json_push_kv_int(&o, "def_line", hits[i].def_line);
        code_push_obj(&others, &o);
        shown++;
    }
    if (shown > 0)
        (void)json_push_kv(&reply->data, "other_defs", &others);
    json_free(&others);

    char summary[224];
    (void)snprintf(summary, sizeof(summary), "%s [%s] %s:%d", s.name, kind,
                   s.def_path[0] ? s.def_path : s.decl_path,
                   s.def_path[0] ? s.def_line : s.decl_line);
    (void)json_push_kv_str(&reply->data, "summary", summary);

    codeindex_close(ci);
}

/* ── code.refs ──────────────────────────────────────────────────────────── */
void zcl_native_handle_code_refs(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply)
{
    const char *name = code_str(request, "name");
    if (!name) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_NAME",
                               "normalize", false, false,
                               "code refs requires a symbol name", "");
        return;
    }
    int limit = code_limit(request, CODE_REFS_DEFAULT, CODE_REFS_MAX);
    struct codeindex *ci = code_open(request, reply);
    if (!ci) return;

    /* Fetch one extra to detect (and report) overflow past the cap. */
    static struct ci_ref refs[CODE_REFS_MAX + 1];
    int want = limit + 1;
    if (want > CODE_REFS_MAX + 1) want = CODE_REFS_MAX + 1;
    int nr = codeindex_refs(ci, name, refs, want);
    if (nr < 0) nr = 0;
    bool truncated = nr > limit;
    if (truncated) nr = limit;

    struct json_value arr, lines;
    json_init(&arr);   json_set_array(&arr);
    json_init(&lines); json_set_array(&lines);
    for (int i = 0; i < nr; i++) {
        struct json_value o;
        json_init(&o); json_set_object(&o);
        (void)json_push_kv_str(&o, "file", refs[i].ref_file);
        (void)json_push_kv_int(&o, "line", refs[i].ref_line);
        code_push_obj(&arr, &o);
        char l[300];
        (void)snprintf(l, sizeof(l), "%s:%d", refs[i].ref_file, refs[i].ref_line);
        code_push_line(&lines, l);
    }

    (void)json_push_kv_str(&reply->data, "name", name);
    (void)json_push_kv(&reply->data, "refs", &arr);
    (void)json_push_kv(&reply->data, "lines", &lines);
    (void)json_push_kv_int(&reply->data, "count", nr);
    (void)json_push_kv_int(&reply->data, "limit", limit);
    (void)json_push_kv_bool(&reply->data, "truncated", truncated);
    char summary[160];
    (void)snprintf(summary, sizeof(summary), "%d reference(s) to %s%s", nr, name,
                   truncated ? " (more not shown; raise --limit)" : "");
    (void)json_push_kv_str(&reply->data, "summary", summary);

    json_free(&arr); json_free(&lines);
    codeindex_close(ci);
}

/* ── code.find ──────────────────────────────────────────────────────────── */
void zcl_native_handle_code_find(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply)
{
    const char *text = code_str(request, "text");
    if (!text) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_TEXT",
                               "normalize", false, false,
                               "code find requires search text", "");
        return;
    }
    int limit = code_limit(request, CODE_FIND_DEFAULT, CODE_FIND_MAX);
    struct codeindex *ci = code_open(request, reply);
    if (!ci) return;

    static struct ci_symbol hits[CODE_FIND_MAX + 1];
    int want = limit + 1;
    if (want > CODE_FIND_MAX + 1) want = CODE_FIND_MAX + 1;
    int nh = codeindex_find(ci, text, hits, want);
    if (nh < 0) nh = 0;
    bool truncated = nh > limit;
    if (truncated) nh = limit;

    struct json_value arr, lines;
    json_init(&arr);   json_set_array(&arr);
    json_init(&lines); json_set_array(&lines);
    for (int i = 0; i < nh; i++) {
        char sig[64];
        code_trunc(sig, sizeof(sig), hits[i].signature, 52);
        const char *p = hits[i].def_path[0] ? hits[i].def_path : hits[i].decl_path;
        int line = hits[i].def_path[0] ? hits[i].def_line : hits[i].decl_line;
        char kind[2] = { hits[i].kind, '\0' };
        struct json_value o;
        json_init(&o); json_set_object(&o);
        (void)json_push_kv_str(&o, "name", hits[i].name);
        (void)json_push_kv_str(&o, "kind", kind);
        (void)json_push_kv_str(&o, "def_path", p);
        (void)json_push_kv_int(&o, "def_line", line);
        (void)json_push_kv_str(&o, "signature", sig);
        code_push_obj(&arr, &o);
        char l[200];
        (void)snprintf(l, sizeof(l), "%s  %s:%d", hits[i].name, p, line);
        code_push_line(&lines, l);
    }

    (void)json_push_kv_str(&reply->data, "query", text);
    (void)json_push_kv(&reply->data, "matches", &arr);
    (void)json_push_kv(&reply->data, "lines", &lines);
    (void)json_push_kv_int(&reply->data, "count", nh);
    (void)json_push_kv_int(&reply->data, "limit", limit);
    (void)json_push_kv_bool(&reply->data, "truncated", truncated);
    char summary[160];
    (void)snprintf(summary, sizeof(summary), "%d match(es) for '%s'%s", nh, text,
                   truncated ? " (more not shown; raise --limit)" : "");
    (void)json_push_kv_str(&reply->data, "summary", summary);

    json_free(&arr); json_free(&lines);
    codeindex_close(ci);
}
