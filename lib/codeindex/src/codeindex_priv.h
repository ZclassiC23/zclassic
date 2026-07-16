/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex_priv — private declarations shared across the lib/codeindex/
 * implementation translation units. NOT a public header: nothing outside
 * lib/codeindex/src/ includes this.
 *
 * Holds the store handle API (a dedicated derived SQLite store below the
 * AR layer, exactly like lib/vcs's index.kv), the source enumerator, the C
 * scanner, the depfile parser, and the group taxonomy — the pieces the public
 * query/build surfaces are assembled from.
 */

#ifndef ZCL_CODEINDEX_PRIV_H
#define ZCL_CODEINDEX_PRIV_H

#include "codeindex/codeindex.h"

#include "crypto/sha3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#define CI_PATH_MAX 4096

/* ── the public handle (defined here so every TU can reach the store) ── */
struct ci_store;
struct codeindex {
    struct ci_store *store;
    char             root[CI_PATH_MAX];
};

/* ── store: derived SQLite DB at <root>/.codeindex/index.kv ───────────
 * Raw sqlite3_step in the store carries `// raw-sql-ok:codeindex-derived`. */
/* Canonical generations are query-only. NULL also means "not published yet";
 * only the rebuild path may create a writable staging database. */
struct ci_store *ci_store_open(const char *root);
struct ci_store *ci_store_open_path(const char *dbpath);/* an explicit path (tmp build) */
void             ci_store_close(struct ci_store *s);

/* Lazy-open refresh: coalesces concurrent stale opens under the cross-process
 * rebuild lock. Public codeindex_rebuild() remains a forced recompute. */
bool ci_codeindex_refresh(struct codeindex *ci);

/* single-writer transaction control */
bool ci_store_begin(struct ci_store *s);
bool ci_store_commit(struct ci_store *s);
bool ci_store_rollback(struct ci_store *s);

/* writes (require an open txn) */
bool ci_store_clear(struct ci_store *s);
bool ci_store_put_file(struct ci_store *s, const struct ci_file *f,
                       const uint8_t content_sha3[32], int64_t mtime,
                       int64_t *out_file_id);
bool ci_store_put_symbol(struct ci_store *s, const struct ci_symbol *sym);
bool ci_store_put_include(struct ci_store *s, int64_t file_id,
                          const char *dep_path);
bool ci_store_put_ref(struct ci_store *s, const char *callee,
                      const char *ref_file, int ref_line);
bool ci_store_put_group(struct ci_store *s, const struct ci_group *g);
bool ci_store_meta_set(struct ci_store *s, const char *k, const void *v,
                       size_t vlen);
/* Serialize a committed in-memory store directly into an already-open private
 * regular-file capability. No pathname is opened or followed. */
bool ci_store_write_image_fd(struct ci_store *s, int fd);

/* reads (self-locking; no open txn required) */
bool ci_store_meta_get(struct ci_store *s, const char *k, void *buf,
                       size_t cap, size_t *outlen, bool *found);
bool ci_store_symbol_by_name(struct ci_store *s, const char *name,
                             struct ci_symbol *out, bool *found);
int  ci_store_find_symbols(struct ci_store *s, const char *q,
                           struct ci_symbol *out, int cap);
int  ci_store_refs_by_callee(struct ci_store *s, const char *callee,
                             struct ci_ref *out, int cap);
bool ci_store_file_by_path(struct ci_store *s, const char *path,
                           struct ci_file *out, bool *found);
int  ci_store_list_groups(struct ci_store *s, struct ci_group *out, int cap);
int  ci_store_files_in_group(struct ci_store *s, const char *group,
                             struct ci_file *out, int cap);
int  ci_store_count_files_in_group(struct ci_store *s, const char *group,
                                   bool recursive);
int  ci_store_symbols_in_file(struct ci_store *s, const char *path,
                              struct ci_symbol *out, int cap);
int  ci_store_includes_of_file(struct ci_store *s, const char *path,
                               char (*out)[256], int cap);

/* Canonical per-symbol row hash used for verify-on-read. Deterministic over
 * all card fields. */
void ci_symbol_row_hash(const struct ci_symbol *sym, uint8_t out[32]);

/* ── source enumeration ───────────────────────────────────────────────
 * Deterministic, sorted, repo-relative .c/.h paths across the configured
 * roots (lib/<mod>/{src,include}, app/<shape>/{src,include}, core, config/src,
 * tools, domain, adapters). cb returns false to abort. */
typedef bool (*ci_enum_cb)(const char *relpath, const struct stat *st,
                           void *user);
bool ci_enumerate_sources(const char *root, ci_enum_cb cb, void *user);
/* Fast freshness key for an exact source root already sealed in a generation.
 * It binds path + inode + size + mtime + ctime and never reads file bytes. */
bool ci_source_stat_root_sha3(const char *root, uint8_t out[32]);
/* Exact bytes and their metadata cache key from the same opened inodes. */
bool ci_source_roots_sha3(const char *root, uint8_t exact_out[32],
                          uint8_t stat_out[32]);

/* ── the C scanner (codeindex_scan.c) ─────────────────────────────────
 * Scan one in-tree file's text. Emits symbols and refs through callbacks.
 * relpath is repo-relative; is_header selects DEFINITION vs DECLARATION
 * handling for prototypes. Never aborts on messy input — degrades to
 * partial=true. `content_sha3` of the scanned bytes is returned in out_sha3.
 * `purpose_out` (may be NULL) receives the file's one-line self-description
 * derived from its leading block comment (§1.1 of docs/work/palace-design.md);
 * "" when no leading comment precedes the first code token. */
typedef void (*ci_sym_cb)(const struct ci_symbol *sym, void *user);
typedef void (*ci_ref_cb)(const char *callee, const char *ref_file,
                          int ref_line, void *user);
bool ci_scan_file(const char *root, const char *relpath,
                  ci_sym_cb on_sym, ci_ref_cb on_ref, void *user,
                  uint8_t out_sha3[32], char purpose_out[160]);

/* Pure text scanner (no file I/O) — the testable core. `src`/`len` is the raw
 * file text; the group is stamped into every emitted symbol. `purpose_out`
 * (may be NULL) receives the derived file purpose (see ci_scan_file). */
void ci_scan_text(const char *src, size_t len, const char *relpath,
                  bool is_header, const char *group,
                  ci_sym_cb on_sym, ci_ref_cb on_ref, void *user,
                  char purpose_out[160]);

/* ── depfile parsing (codeindex_deps.c) ───────────────────────────────
 * Parse one make depfile: yields (source_relpath, dep_relpath) edges for
 * in-tree prerequisites. */
typedef void (*ci_dep_cb)(const char *src_relpath, const char *dep_relpath,
                          void *user);
/* Deterministically scan sorted .d inputs below build/. `cb` may be NULL for a
 * digest-only freshness check. out_root binds the exact depfile bytes parsed,
 * including an explicit marker when build/ is absent. */
bool ci_deps_scan(const char *root, ci_dep_cb cb, void *user,
                  uint8_t out_root[32]);
bool ci_deps_scan_roots(const char *root, ci_dep_cb cb, void *user,
                        uint8_t exact_out[32], uint8_t stat_out[32]);
/* Metadata cache key for ci_deps_scan's exact root. Historical compile epochs
 * are excluded by both functions; this path reads no depfile content. */
bool ci_deps_stat_root_sha3(const char *root, uint8_t out_root[32]);

#ifdef ZCL_TESTING
void ci_test_note_exact_bytes(uint64_t bytes);
#else
#define ci_test_note_exact_bytes(...) ((void)0)
#endif

/* ── group taxonomy (codeindex_group.c) ───────────────────────────────*/
/* Map a repo-relative path to its group id (e.g. "lib/net", "app/services",
 * "core", "tools", "config"). Writes "" for an unclassified path. */
void ci_group_for_path(const char *relpath, char out[64]);
/* Human-readable one-liner for a known group ("" if none). */
const char *ci_group_purpose(const char *group);
/* Emit the full fixed hierarchy into an open store txn. */
bool ci_group_emit_all(struct ci_store *s);

#endif /* ZCL_CODEINDEX_PRIV_H */
