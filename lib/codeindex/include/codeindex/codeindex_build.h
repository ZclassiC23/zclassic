/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex_build — the rebuild + staleness surface for the codeindex store.
 *
 * The build is a DETERMINISTIC full recompute: enumerate the source set in a
 * fixed sorted order, scan each file for symbols/refs, fold in include edges
 * from build depfiles, write the group hierarchy, all into a fresh `.tmp`
 * database, then atomically rename it over <root>/.codeindex/index.kv and
 * stamp meta.source_root_sha3 (a cheap stat-based tree hash used to detect a
 * changed tree on the next open). "Recompute, never repair."
 */

#ifndef ZCL_CODEINDEX_BUILD_H
#define ZCL_CODEINDEX_BUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct codeindex;

/* Fully rebuild the derived store from the source tree. Builds into a temp DB
 * and renames it into place, so a partial build never corrupts the live store.
 * Reopens the handle's store on success. Returns false on any hard error. */
bool codeindex_rebuild(struct codeindex *ci);

/* Compute whether the on-disk store is stale w.r.t. the current tree (its
 * stored source_root_sha3 != the freshly recomputed one, or it is missing).
 * Returns false only on a hard error. */
bool codeindex_is_stale(struct codeindex *ci, bool *stale);

/* Compute the cheap stat-based tree hash over the enumerated source set:
 * SHA3-256 over sorted (relpath || mtime_ns || size) tuples. Used as the
 * staleness stamp. Returns false on a hard error. */
bool codeindex_source_root_sha3(const char *root, uint8_t out[32]);

/* ── Canonical group taxonomy (mirrors the Makefile / shape layout) ──
 * These arrays are the SINGLE in-code mirror of the Makefile's LIB_MODULES
 * and the eight app/ shape folders. A parity test asserts they match. */
const char *const *ci_lib_modules(size_t *count);
const char *const *ci_app_shapes(size_t *count);

#endif /* ZCL_CODEINDEX_BUILD_H */
