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

/* A recorded call site. `enclosing` is the name of the function the call site
 * sits inside — the greatest function whose def_line <= ref_line in the same
 * file (C does not nest functions; documented best-effort). Empty string when
 * unattributed (e.g. a reference at file scope). This is the column that turns
 * the flat refs table into a call graph: callees of X are the refs WHERE
 * enclosing == X. Populated by the scan pass (codeindex_scan.c). */
struct ci_ref {
    char callee[128];
    char ref_file[256];
    int  ref_line;
    char enclosing[128];
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

/* Stable-id lookup. Unlike the legacy name lookup, a static-function id
 * (`fn:static:<repo-path>:<name>`) resolves that exact definition, so equal
 * static names in different translation units cannot collide. */
bool codeindex_symbol_by_id(struct codeindex *ci, const char *id,
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

/* ── Call-graph queries (WF4 code-capsule) ──────────────────────────────
 *
 * Built on the `enclosing` column above (populated by the scan pass). */

/* Callers of `name`: the call sites referencing it, each with `enclosing`
 * filled (the function the call sits in). Ordered by (ref_file, ref_line).
 * Fills up to `cap` rows, returns count (>=0), -1 on error. Equivalent to
 * codeindex_refs but with the enclosing attribution guaranteed populated. */
int codeindex_callers(struct codeindex *ci, const char *name,
                      struct ci_ref *out, int cap);

/* Callees of `enclosing_name`: the distinct symbols referenced from inside it
 * (refs WHERE enclosing == enclosing_name). Ordered by (ref_file, ref_line).
 * Fills up to `cap` rows, returns count (>=0), -1 on error. */
int codeindex_callees(struct codeindex *ci, const char *enclosing_name,
                      struct ci_ref *out, int cap);

/* Identity-aware variants. Static functions are restricted to their
 * definition file; externally linked symbols retain name-wide behavior. */
int codeindex_callers_for_symbol(struct codeindex *ci,
                                 const struct ci_symbol *symbol,
                                 struct ci_ref *out, int cap);
int codeindex_callees_for_symbol(struct codeindex *ci,
                                 const struct ci_symbol *symbol,
                                 struct ci_ref *out, int cap);

/* Linkage-aware stable identity for `name`, computed from existing fields:
 * "fn:static:<path>:<name>" for a static function, "fn:external:<name>" for an
 * external one (name-based lookup is untouched). Writes a NUL-terminated id
 * into `buf` (capacity `cap`). Returns the length written (excluding NUL), or
 * -1 on error / not found. */
int codeindex_symbol_id(struct codeindex *ci, const char *name,
                        char *buf, size_t cap);
int codeindex_symbol_record_id(const struct ci_symbol *symbol,
                               char *buf, size_t cap);

/* ── Impact-closure query (proof-DAG from symbol closure, F3) ───────────
 *
 * Given a set of changed FILES, compute the changed symbols (every symbol the
 * store attributes to one of those files — a .c's definitions, a header's
 * declarations), then walk the bounded REVERSE-caller closure (callers of
 * callers, via refs.enclosing) up to `max_depth` levels, and return the set of
 * impacted FILES: every file that transitively references a changed symbol
 * PLUS the changed files themselves. This is the file-level "blast radius" of a
 * change, derived from the call graph rather than a path glob.
 *
 * Output is DETERMINISTIC (unique, sorted by path) and filled up to `cap` rows.
 * *truncated is set true iff the closure hit an internal size cap, a per-query
 * fan-out cap, or overflowed `cap` — i.e. the returned set may be INCOMPLETE,
 * so a caller building a test plan MUST fall back to path-only rather than
 * trust a silently-partial set. `max_depth <= 0` selects CI_CLOSURE_DEFAULT_DEPTH
 * (depth exhaustion is a normal bound, NOT truncation: the file set returned for
 * the walked depth is complete). Returns the file count (>=0), -1 on hard error.
 * `changed_files` is an array of NUL-terminated repo-relative paths. */
#define CI_CLOSURE_DEFAULT_DEPTH 8
int codeindex_impact_closure(struct codeindex *ci,
                             const char (*changed_files)[256], int n_changed,
                             int max_depth,
                             char (*out)[256], int cap, bool *truncated);

/* ── Forward (callee) input-closure query — the content-addressed test cache
 * key input (symmetric mirror of codeindex_impact_closure) ──────────────
 *
 * Given a ROOT SYMBOL (e.g. a test entry point "test_<name>"), compute the set
 * of in-tree source FILES whose byte content can change the behavior reachable
 * from that symbol: every file that DEFINES a symbol transitively reachable via
 * the FORWARD callee graph (refs.enclosing walk), PLUS every in-tree header
 * those definition files include (compiler-depfile edges — sound + complete for
 * the include dimension). This is the INPUT closure a caller content-addresses
 * to decide whether re-running the symbol is necessary.
 *
 * Where codeindex_impact_closure answers "who is impacted when X changes"
 * (reverse callers, a conservative test-selection superset), this answers "what
 * does X depend on" (forward callees, the input set). The two walk the same
 * call graph in opposite directions and share its one intrinsic limit: an edge
 * that source scanning never recorded (an indirect/function-pointer/dlopen
 * dispatch) is invisible to both. A SOUNDNESS-sensitive caller (a result cache)
 * MUST therefore treat a *truncated result as UNCACHEABLE and MUST back the
 * cache with a cold-audit path that never trusts it — see lib/test/src/test_cache.c.
 *
 * Output is DETERMINISTIC (unique, sorted by path) and filled up to `cap` rows.
 * *truncated is set true iff the closure hit an internal size cap, a per-symbol
 * callee fan-out cap, overflowed `cap`, OR the depth ceiling was reached with
 * the frontier still non-empty — i.e. the returned set may be INCOMPLETE.
 * *root_found (may be NULL) is set false iff root_symbol is not a known in-tree
 * symbol (then the closure is empty — the caller cannot bound the inputs, so it
 * too is UNCACHEABLE). Returns the file count (>=0), -1 on hard error. */
int codeindex_forward_closure(struct codeindex *ci, const char *root_symbol,
                              char (*out)[256], int cap,
                              bool *truncated, bool *root_found);

#endif /* ZCL_CODEINDEX_H */
