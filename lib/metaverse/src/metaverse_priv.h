/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal helpers shared by the metaverse property adapters. Not a public
 * surface: nothing outside lib/metaverse/src includes this.
 *
 * Everything here is READ-ONLY over the frozen <datadir>/zcode layout that
 * vcs/package_store.h documents. It opens no store, because opening one
 * runs the mutating recovery sweep — see the "READ MEANS READ" note in
 * metaverse/property_adapter.h. */

#ifndef ZCL_METAVERSE_PRIV_H
#define ZCL_METAVERSE_PRIV_H

#include "vcs/package_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded scan of <zcode_dir>/manifests. The store's own tracked bound is
 * VCS_PACKAGE_STORE_MAX_TRACKED (4096); a projection page never needs that
 * many, and an unbounded readdir over an operator directory is a cost the
 * caller cannot see. Truncation is always reported, never silent. */
#define MV_MANIFEST_SCAN_MAX 512u

/* One manifest read back from the store, with the facts a view needs. */
struct mv_manifest_read {
    uint8_t root[32];          /* root RE-DERIVED from the parsed wire */
    bool root_matches_name;    /* re-derived root == the filename */
    struct vcs_package_manifest manifest; /* owned; mv_manifest_free() it */
    uint32_t chunk_total;      /* chunks committed by the manifest */
    uint32_t chunks_present;   /* of those, present in the CAS */
    uint64_t total_bytes;
    uint32_t file_count;
};

/* Read + parse <zcode_dir>/manifests/<root_hex> and re-derive its root.
 * False when the file is missing, unreadable, oversize, or not canonical
 * content.v2 wire; *out is then zeroed and needs no free. On true the
 * caller MUST call mv_manifest_free(out).
 *
 * CAS presence is counted per unique chunk hash of the manifest via
 * vcs_package_cas_present_in() — presence only, no re-hash. */
bool mv_manifest_read(const char *zcode_dir, const char *root_hex,
                      struct mv_manifest_read *out);
void mv_manifest_free(struct mv_manifest_read *m);

/* True when the manifest has the frozen blob shape: exactly one file at
 * VCS_BLOB_PATH, one chunk, regular-file mode, size within the blob cap. */
bool mv_manifest_is_blob(const struct vcs_package_manifest *m);

/* Enumerate the 64-hex filenames under <zcode_dir>/manifests in ascending
 * name order (deterministic output regardless of readdir order). Writes up
 * to out_cap names of 65 bytes each; returns the count written.
 * *total_out receives the number of hex64 entries SEEN (capped at
 * MV_MANIFEST_SCAN_MAX) and *truncated_out is set when the scan itself hit
 * that cap or when more entries were seen than written. */
size_t mv_manifest_names(const char *zcode_dir, char (*out)[65],
                         size_t out_cap, size_t *total_out,
                         bool *truncated_out);

/* Can <zcode_dir>/manifests be ENUMERATED right now?
 *
 * True also when the directory does not exist: a datadir that has never
 * published anything is honestly empty, and that is a fact, not a fault.
 * False ONLY when something is there and cannot be read — a plain file
 * where the directory belongs, a permission wall, an I/O error — and
 * `reason` (when non-NULL) then carries what the OS said.
 *
 * This distinction is the whole point. mv_manifest_names() cannot make it:
 * it returns 0 either way, so "the store is unreadable" and "the node owns
 * nothing" would reach the operator as the same empty catalog. That
 * conflation is the defect class this project has already paid for on
 * node.db, and lib/test/src/test_read_leaf_no_datadir_write.c exists to
 * keep it from recurring. Callers ask this FIRST and disclose a false. */
bool mv_store_enumerable(const char *zcode_dir, char *reason, size_t cap);

/* The same question in adapter-hook shape, so a row can wire it directly
 * as metaverse_adapter.store_ready. */
struct metaverse_adapter_ctx;
bool mv_zcode_store_ready(const struct metaverse_adapter_ctx *ctx,
                          char *reason, size_t reason_cap);

/* Read + parse <zcode_dir>/releases/<release_id_hex> and VERIFY its
 * signature (vcs_package_release_verify) during this call. False when the
 * file is absent/unparseable or the signature does not verify — the caller
 * must then not claim the local_signature evidence grade. */
struct vcs_package_release;
bool mv_release_read_verified(const char *zcode_dir,
                             const char *release_id_hex,
                             struct vcs_package_release *out);

/* The adapter rows, one accessor per implementing translation unit. Only
 * adapter_registry.c calls these; the registry is the single dispatch
 * point every consumer goes through. */
struct metaverse_adapter;
const struct metaverse_adapter *metaverse_adapter_content(void);
const struct metaverse_adapter *metaverse_adapter_zcode_package(void);

#endif /* ZCL_METAVERSE_PRIV_H */
