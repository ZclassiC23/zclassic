/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex — an in-binary, hierarchical, token-bounded source-code
 * navigator index for the zclassic23 tree.
 *
 * ── What it is ──
 * A DERIVED store (like lib/vcs's index.kv) built by scanning the in-tree C
 * source: it records, per symbol, where it is DEFINED and DECLARED, a cleaned
 * one-line signature, its leading doc comment, its enclosing `#ifdef` guard,
 * the group (lib/<mod>, app/<shape>, core, tools, config, …) it belongs to,
 * plus include edges (from build depfiles) and a bounded call-site ref index.
 *
 * ── Ground truth ──
 * The PRIMARY source of truth is IN-TREE SOURCE SCANNING — the release build
 * ships without `-g`, so `nm` yields no line info. Everything here is
 * recomputed from source; nothing is repaired in place ("recompute, never
 * repair"). The store lives at <root>/.codeindex/index.kv (a dedicated
 * single-writer SQLite WAL below the AR layer, beside .git).
 *
 * This header is the QUERY surface. The rebuild / staleness surface is in
 * codeindex_build.h. A LATER lane wires the `code` command branch on top of
 * these calls — do not add commands here.
 */

#ifndef ZCL_CODEINDEX_H
#define ZCL_CODEINDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque handle. Open lazily rebuilds the store if the source tree changed. */
struct codeindex;

/* ── Result records — flat POD, fixed buffers (mirrors vcs records) ── */

/* A symbol "card": everything needed to render a one-screen answer about a
 * name without opening the file. `kind` is a single char:
 *   'T' func         't' static func
 *   'S' struct/union 'Y' typedef      'E' enum
 *   'M' macro (#define)               'D' data / other top-level decl
 * `partial` is set when the scanner could not confidently extract a clean
 * one-line signature (multiline prototypes, X-macro-wrapped decls, …) and
 * fell back to the raw declaration line — the symbol is still emitted. */
struct ci_symbol {
    char name[128];
    char kind;
    char def_path[256];
    int  def_line;
    char decl_path[256];
    int  decl_line;
    char signature[512];
    char doc[256];
    char guard[128];
    char group[64];
    bool partial;
};

/* A source file and the group it maps to. */
struct ci_file {
    char path[256];
    char group[64];
    char purpose[160];
};

/* A node in the group hierarchy (lib/<mod>, app/<shape>, core, …). */
struct ci_group {
    char path[64];
    char kind[16];
    char parent[64];
    char purpose[160];
};

/* A recorded call site. */
struct ci_ref {
    char callee[128];
    char ref_file[256];
    int  ref_line;
};

/* ── Lifecycle ── */

/* Open (creating if needed) <root>/.codeindex/index.kv. If the store is
 * missing or the source tree's staleness stamp no longer matches, this
 * rebuilds it before returning. NULL on hard failure. */
struct codeindex *codeindex_open(const char *root);
void codeindex_close(struct codeindex *ci);

/* ── Queries ── */

/* Exact-name lookup. On a hit fills *out and sets *found=true; verify-on-read
 * rejects a corrupted row (returns found=false). Returns false only on a hard
 * error (never for "not found"). */
bool codeindex_symbol(struct codeindex *ci, const char *name,
                      struct ci_symbol *out, bool *found);

/* Ranked substring search over symbol names: exact match ranks first, then
 * prefix, then substring; ties broken by name then def_path for determinism.
 * Fills up to `cap` rows in `out`, returns the count (>=0), -1 on error. */
int codeindex_find(struct codeindex *ci, const char *query,
                   struct ci_symbol *out, int cap);

/* Call sites referencing `callee`, ordered by (ref_file, ref_line). Fills up
 * to `cap` rows, returns count (>=0), -1 on error. */
int codeindex_refs(struct codeindex *ci, const char *callee,
                   struct ci_ref *out, int cap);

/* File → its group/purpose. */
bool codeindex_file(struct codeindex *ci, const char *path,
                    struct ci_file *out, bool *found);

/* The full group hierarchy, ordered by path. */
int codeindex_groups(struct codeindex *ci, struct ci_group *out, int cap);

/* Files that belong to `group` (e.g. "app/services", "lib/net"), ordered by
 * path. Fills up to `cap` rows, returns the count (>=0), -1 on error. */
int codeindex_files_in_group(struct codeindex *ci, const char *group,
                             struct ci_file *out, int cap);

/* Count files in `group`. When `recursive` is false, only files stamped with
 * EXACTLY this group; when true, also every descendant group (so "lib" or "app"
 * aggregates its child modules/shapes). Returns the count (>=0), -1 on error. */
int codeindex_count_files_in_group(struct codeindex *ci, const char *group,
                                   bool recursive);

/* The symbol table of one file: symbols DEFINED in it (for a .c) or DECLARED in
 * it (for a header), definitions first then source order. Fills up to `cap`
 * rows, returns count (>=0), -1 on error. */
int codeindex_symbols_in_file(struct codeindex *ci, const char *path,
                              struct ci_symbol *out, int cap);

/* In-tree include dependencies of `path`, ordered by dep path. Each out[i] is a
 * NUL-terminated repo-relative path (up to 255 bytes). Fills up to `cap` rows,
 * returns count (>=0), -1 on error. */
int codeindex_includes_of_file(struct codeindex *ci, const char *path,
                               char (*out)[256], int cap);

/* Render a bounded, human-readable card for `name` into `buf` (NUL-terminated,
 * never exceeds `cap`). Returns the number of bytes written (excluding NUL),
 * or -1 on error / not found. */
int codeindex_render_card(struct codeindex *ci, const char *name,
                          char *buf, size_t cap);

#endif /* ZCL_CODEINDEX_H */
