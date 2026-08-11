/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: compressed transport for an already-authoritative ZVCS source tree. */

#ifndef ZCL_VCS_SOURCE_BUNDLE_H
#define ZCL_VCS_SOURCE_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_SOURCE_BUNDLE_VERSION 1u
#define VCS_SOURCE_BUNDLE_CODEC_ZLIB 1u
#define VCS_SOURCE_BUNDLE_HEADER_BYTES 68u
#define VCS_SOURCE_BUNDLE_MAX_MANIFEST_BYTES (4u * 1024u * 1024u)
#define VCS_SOURCE_BUNDLE_MAX_SOURCE_BYTES \
    (UINT64_C(256) * 1024u * 1024u)
#define VCS_SOURCE_BUNDLE_MAX_WIRE_BYTES \
    (UINT64_C(64) * 1024u * 1024u)

enum vcs_source_bundle_result {
    VCS_SOURCE_BUNDLE_OK = 0,
    VCS_SOURCE_BUNDLE_ERR_NULL,
    VCS_SOURCE_BUNDLE_ERR_SOURCE,
    VCS_SOURCE_BUNDLE_ERR_LIMIT,
    VCS_SOURCE_BUNDLE_ERR_ALLOC,
    VCS_SOURCE_BUNDLE_ERR_CODEC,
    VCS_SOURCE_BUNDLE_ERR_WIRE,
    VCS_SOURCE_BUNDLE_ERR_ROOT,
    VCS_SOURCE_BUNDLE_ERR_BLOB,
    VCS_SOURCE_BUNDLE_ERR_STORE,
};

struct vcs_source_bundle_metrics {
    uint64_t source_bytes;
    uint64_t compressed_bytes;
    uint64_t new_bytes;
    uint64_t reused_bytes;
    uint32_t file_count;
    uint32_t new_blobs;
    uint32_t reused_blobs;
    bool manifest_reused;
    bool repaired;
};

const char *vcs_source_bundle_result_string(
    enum vcs_source_bundle_result result);

/* Build a deterministic zlib-compressed transport from a verified ZVCS tree.
 * The tree manifest and every domain-tagged blob are reloaded and rehashed.
 * The returned wire is transport only: tree_root remains the authority. */
enum vcs_source_bundle_result vcs_source_bundle_create(
    const char *workspace, const uint8_t tree_root[32], uint8_t **wire_out,
    size_t *wire_len_out, struct vcs_source_bundle_metrics *metrics);

/* Parse, decompress and fully rederive a bundle without writing anything. */
enum vcs_source_bundle_result vcs_source_bundle_verify(
    const uint8_t *wire, size_t wire_len,
    const uint8_t expected_tree_root[32],
    struct vcs_source_bundle_metrics *metrics);

/* Verify the complete bundle first, then admit its blobs and manifest into the
 * existing ZVCS CAS. Existing valid objects are reused; corrupt objects at the
 * exact committed addresses are atomically repaired from verified bytes. */
enum vcs_source_bundle_result vcs_source_bundle_import(
    const uint8_t *wire, size_t wire_len,
    const uint8_t expected_tree_root[32], const char *workspace,
    struct vcs_source_bundle_metrics *metrics);

#endif /* ZCL_VCS_SOURCE_BUNDLE_H */
