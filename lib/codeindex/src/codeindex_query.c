/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex_query — the public query surface: exact/ranked symbol lookup,
 * reverse refs, file→group, the group list, and a bounded human-readable card
 * render. Thin dispatch over the store; verify-on-read lives in the store. */

#include "codeindex_priv.h"

#include "util/log_macros.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool codeindex_symbol(struct codeindex *ci, const char *name,
                      struct ci_symbol *out, bool *found)
{
    if (found) *found = false;
    if (!ci || !ci->store || !name || !out)
        LOG_FAIL("codeindex", "null arg to codeindex_symbol");
    return ci_store_symbol_by_name(ci->store, name, out, found);
}

int codeindex_find(struct codeindex *ci, const char *query,
                   struct ci_symbol *out, int cap)
{
    if (!ci || !ci->store || !query || !out || cap <= 0)
        LOG_ERR("codeindex", "bad arg to codeindex_find");
    return ci_store_find_symbols(ci->store, query, out, cap);
}

int codeindex_refs(struct codeindex *ci, const char *callee,
                   struct ci_ref *out, int cap)
{
    if (!ci || !ci->store || !callee || !out || cap <= 0)
        LOG_ERR("codeindex", "bad arg to codeindex_refs");
    return ci_store_refs_by_callee(ci->store, callee, out, cap);
}

bool codeindex_file(struct codeindex *ci, const char *path,
                    struct ci_file *out, bool *found)
{
    if (found) *found = false;
    if (!ci || !ci->store || !path || !out)
        LOG_FAIL("codeindex", "null arg to codeindex_file");
    return ci_store_file_by_path(ci->store, path, out, found);
}

int codeindex_groups(struct codeindex *ci, struct ci_group *out, int cap)
{
    if (!ci || !ci->store || !out || cap <= 0)
        LOG_ERR("codeindex", "bad arg to codeindex_groups");
    return ci_store_list_groups(ci->store, out, cap);
}

int codeindex_files_in_group(struct codeindex *ci, const char *group,
                             struct ci_file *out, int cap)
{
    if (!ci || !ci->store || !group || !out || cap <= 0)
        LOG_ERR("codeindex", "bad arg to codeindex_files_in_group");
    return ci_store_files_in_group(ci->store, group, out, cap);
}

int codeindex_symbols_in_file(struct codeindex *ci, const char *path,
                              struct ci_symbol *out, int cap)
{
    if (!ci || !ci->store || !path || !out || cap <= 0)
        LOG_ERR("codeindex", "bad arg to codeindex_symbols_in_file");
    return ci_store_symbols_in_file(ci->store, path, out, cap);
}

int codeindex_includes_of_file(struct codeindex *ci, const char *path,
                               char (*out)[256], int cap)
{
    if (!ci || !ci->store || !path || !out || cap <= 0)
        LOG_ERR("codeindex", "bad arg to codeindex_includes_of_file");
    return ci_store_includes_of_file(ci->store, path, out, cap);
}

static const char *kind_name(char k)
{
    switch (k) {
    case 'T': return "func";
    case 't': return "static func";
    case 'S': return "struct/union";
    case 'Y': return "typedef";
    case 'E': return "enum";
    case 'M': return "macro";
    case 'D': return "data";
    default:  return "symbol";
    }
}

/* Append into buf[*len..cap) if room; always keep NUL-terminated. */
static void app(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *len += (size_t)n;
    if (*len >= cap) *len = cap - 1;  /* clamp on truncation */
}

int codeindex_render_card(struct codeindex *ci, const char *name,
                          char *buf, size_t cap)
{
    if (!ci || !ci->store || !name || !buf || cap == 0)
        LOG_ERR("codeindex", "bad arg to render_card");
    buf[0] = '\0';

    struct ci_symbol s;
    bool found = false;
    if (!ci_store_symbol_by_name(ci->store, name, &s, &found))
        LOG_ERR("codeindex", "symbol lookup failed");
    if (!found)
        return -1;

    size_t len = 0;
    app(buf, cap, &len, "%s  [%s]%s\n", s.name, kind_name(s.kind),
        s.partial ? " (partial)" : "");
    if (s.def_path[0])
        app(buf, cap, &len, "  def:   %s:%d\n", s.def_path, s.def_line);
    if (s.decl_path[0])
        app(buf, cap, &len, "  decl:  %s:%d\n", s.decl_path, s.decl_line);
    if (s.signature[0])
        app(buf, cap, &len, "  sig:   %s\n", s.signature);
    if (s.doc[0])
        app(buf, cap, &len, "  doc:   %s\n", s.doc);
    if (s.guard[0])
        app(buf, cap, &len, "  guard: #ifdef %s\n", s.guard);
    if (s.group[0])
        app(buf, cap, &len, "  group: %s\n", s.group);
    return (int)len;
}
