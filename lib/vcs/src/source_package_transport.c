/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: build the existing content.v2 carrier for verified source. */

#include "vcs/source_package_transport.h"

#include "util/safe_alloc.h"
#include "vcs/package_manifest.h"
#include "vcs/package_recipe.h"
#include "vcs/package_store.h"
#include "vcs/vcs.h"
#include "vcs/vcs_object.h"

#include <stdlib.h>
#include <string.h>

#define SOURCE_PACKAGE_MAX_AUTHORITY_BYTES 4096u

static const uint8_t source_transport_marker[] =
    "/* Inert source carrier. Product source is zclassic23-source.zvsb. */\n"
    "const char zclassic23_source_transport_v1[] = \"vcs_source_bundle.v1\";\n";

void vcs_source_package_transport_init(
    struct vcs_source_package_transport *transport)
{
    if (transport) memset(transport, 0, sizeof(*transport));
}

void vcs_source_package_transport_free(
    struct vcs_source_package_transport *transport)
{
    if (!transport) return;
    free(transport->manifest_wire);
    free(transport->recipe_wire);
    free(transport->bundle_wire);
    free(transport->license_bytes);
    memset(transport, 0, sizeof(*transport));
}

const uint8_t *vcs_source_package_transport_marker(size_t *len_out)
{
    if (len_out) *len_out = sizeof(source_transport_marker) - 1u;
    return source_transport_marker;
}

static bool source_package_add(struct vcs_package_manifest *manifest,
                               const char *path, const uint8_t *bytes,
                               size_t len)
{
    uint64_t chunk_count64 = len == 0 ? 0 :
        1u + ((uint64_t)len - 1u) / VCS_PACKAGE_CHUNK_BYTES;
    if (chunk_count64 > UINT32_MAX) return false;
    uint32_t chunk_count = (uint32_t)chunk_count64;
    uint8_t *hashes = chunk_count > 0
        ? zcl_malloc((size_t)chunk_count * 32u,
                     "vcs.source_package.hashes") : NULL;
    if (chunk_count > 0 && !hashes) return false;
    bool ok = true;
    for (uint32_t i = 0; ok && i < chunk_count; i++) {
        size_t off = (size_t)i * VCS_PACKAGE_CHUNK_BYTES;
        size_t take = len - off;
        if (take > VCS_PACKAGE_CHUNK_BYTES) take = VCS_PACKAGE_CHUNK_BYTES;
        ok = vcs_package_chunk_hash(bytes + off, take, hashes + i * 32u);
    }
    ok = ok && vcs_package_manifest_add(
        manifest, path, VCS_PACKAGE_MODE_FILE, len, hashes, chunk_count);
    free(hashes);
    return ok;
}

static bool source_package_load_license(
    const char *workspace, const struct vcs_manifest *tree,
    uint8_t **bytes_out, size_t *len_out)
{
    *bytes_out = NULL;
    *len_out = 0;
    const struct vcs_entry *license = NULL;
    for (size_t i = 0; i < tree->count; i++) {
        if (strcmp(tree->entries[i].path,
                   VCS_SOURCE_PACKAGE_LICENSE_PATH) == 0) {
            license = &tree->entries[i];
            break;
        }
    }
    return license && license->size <= SIZE_MAX &&
        vcs_object_get(workspace, license->blob, VCS_TAG_BLOB,
                       bytes_out, len_out) == 0 &&
        *len_out == (size_t)license->size;
}

static bool source_package_recipe(
    struct vcs_source_package_transport *transport)
{
    struct vcs_package_recipe recipe;
    vcs_package_recipe_init(&recipe);
    enum vcs_package_recipe_error error = VCS_PACKAGE_RECIPE_OK;
    bool ok = vcs_package_recipe_add_source(
        &recipe, VCS_SOURCE_PACKAGE_MARKER_PATH, &error);
    vcs_package_recipe_set_test_limits(
        &recipe, 0, 60, UINT64_C(64) * 1024u * 1024u);
    ok = ok && vcs_package_recipe_serialize(
        &recipe, &transport->recipe_wire,
        &transport->recipe_wire_len) == VCS_PACKAGE_RECIPE_OK &&
        vcs_package_recipe_root(
            &recipe, transport->recipe_root) == VCS_PACKAGE_RECIPE_OK;
    vcs_package_recipe_free(&recipe);
    return ok;
}

bool vcs_source_package_transport_build(
    const char *workspace, const uint8_t source_root[32],
    const uint8_t *lane_wire, size_t lane_wire_len,
    struct vcs_source_package_transport *transport)
{
    if (!workspace || !source_root || !lane_wire || lane_wire_len == 0 ||
        lane_wire_len > SOURCE_PACKAGE_MAX_AUTHORITY_BYTES || !transport)
        return false;
    vcs_source_package_transport_free(transport);
    struct vcs_manifest tree;
    if (!vcs_tree_load(workspace, source_root, &tree)) return false;
    bool ok = source_package_load_license(
        workspace, &tree, &transport->license_bytes,
        &transport->license_len);
    vcs_manifest_free(&tree);
    enum vcs_source_bundle_result bundle_result = ok
        ? vcs_source_bundle_create(
              workspace, source_root, &transport->bundle_wire,
              &transport->bundle_wire_len, &transport->bundle_metrics)
        : VCS_SOURCE_BUNDLE_ERR_SOURCE;
    ok = ok && bundle_result == VCS_SOURCE_BUNDLE_OK;

    size_t marker_len = 0;
    const uint8_t *marker = vcs_source_package_transport_marker(&marker_len);
    uint64_t total = (uint64_t)transport->license_len +
        transport->bundle_wire_len + lane_wire_len + marker_len;
    ok = ok && total <= VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES;
    struct vcs_package_manifest manifest;
    vcs_package_manifest_init(&manifest);
    if (ok) ok = source_package_add(
        &manifest, VCS_SOURCE_PACKAGE_LICENSE_PATH,
        transport->license_bytes, transport->license_len);
    if (ok) ok = source_package_add(
        &manifest, VCS_SOURCE_PACKAGE_BUNDLE_PATH,
        transport->bundle_wire, transport->bundle_wire_len);
    if (ok) ok = source_package_add(
        &manifest, VCS_SOURCE_PACKAGE_LANE_PATH,
        lane_wire, lane_wire_len);
    if (ok) ok = source_package_add(
        &manifest, VCS_SOURCE_PACKAGE_MARKER_PATH, marker, marker_len);
    if (ok) ok = vcs_package_manifest_root(
        &manifest, transport->package_root) &&
        vcs_package_manifest_serialize(
            &manifest, &transport->manifest_wire,
            &transport->manifest_wire_len);
    vcs_package_manifest_free(&manifest);
    if (ok) ok = source_package_recipe(transport);
    if (!ok) vcs_source_package_transport_free(transport);
    return ok;
}
