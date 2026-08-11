/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: content.v2 carrier for one verified ZVCS source bundle. */

#ifndef ZCL_VCS_SOURCE_PACKAGE_TRANSPORT_H
#define ZCL_VCS_SOURCE_PACKAGE_TRANSPORT_H

#include "vcs/source_bundle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_SOURCE_PACKAGE_BUNDLE_PATH "zclassic23-source.zvsb"
#define VCS_SOURCE_PACKAGE_LANE_PATH "zcode-lane-receipt.v1"
#define VCS_SOURCE_PACKAGE_MARKER_PATH "zcode-source-transport.c"
#define VCS_SOURCE_PACKAGE_LICENSE_PATH "LICENSE"

struct vcs_source_package_transport {
    uint8_t package_root[32];
    uint8_t recipe_root[32];
    uint8_t *manifest_wire;
    size_t manifest_wire_len;
    uint8_t *recipe_wire;
    size_t recipe_wire_len;
    uint8_t *bundle_wire;
    size_t bundle_wire_len;
    uint8_t *license_bytes;
    size_t license_len;
    struct vcs_source_bundle_metrics bundle_metrics;
};

void vcs_source_package_transport_init(
    struct vcs_source_package_transport *transport);
void vcs_source_package_transport_free(
    struct vcs_source_package_transport *transport);

/* Derive one ordinary content.v2 package from an authoritative ZVCS tree.
 * The package carries the exact top-level LICENSE, verified source bundle,
 * signed lane-receipt wire, and an inert compilable transport marker.
 * transport must first be initialized with
 * vcs_source_package_transport_init(). */
bool vcs_source_package_transport_build(
    const char *workspace, const uint8_t source_root[32],
    const uint8_t *lane_wire, size_t lane_wire_len,
    struct vcs_source_package_transport *transport);

const uint8_t *vcs_source_package_transport_marker(size_t *len_out);

#endif /* ZCL_VCS_SOURCE_PACKAGE_TRANSPORT_H */
