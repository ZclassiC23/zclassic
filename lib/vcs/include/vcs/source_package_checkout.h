/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: Git-free reconstruction of a verified PROVEN source carrier. */

#ifndef ZCL_VCS_SOURCE_PACKAGE_CHECKOUT_H
#define ZCL_VCS_SOURCE_PACKAGE_CHECKOUT_H

#include "vcs/package_store.h"
#include "vcs/source_bundle.h"

#include <stdint.h>

enum vcs_source_package_checkout_result {
    VCS_SOURCE_PACKAGE_CHECKOUT_OK = 0,
    VCS_SOURCE_PACKAGE_CHECKOUT_NULL,
    VCS_SOURCE_PACKAGE_CHECKOUT_INCOMPLETE,
    VCS_SOURCE_PACKAGE_CHECKOUT_MANIFEST,
    VCS_SOURCE_PACKAGE_CHECKOUT_SHAPE,
    VCS_SOURCE_PACKAGE_CHECKOUT_CHUNK,
    VCS_SOURCE_PACKAGE_CHECKOUT_SOURCE,
    VCS_SOURCE_PACKAGE_CHECKOUT_DESTINATION,
};

struct vcs_source_package_checkout_metrics {
    struct vcs_source_bundle_metrics source;
    uint64_t offline_input_bytes;
    uint32_t offline_input_files;
    uint32_t source_shards;
    uint32_t carrier_files;
};

const char *vcs_source_package_checkout_result_string(
    enum vcs_source_package_checkout_result result);

/* Read one complete content.v2 package from the existing store, rederive its
 * root and exact source-carrier shape, verify every source shard before any
 * write, import the ZVCS tree, then materialize source and pinned vendor
 * archives into separate explicit scratch roots. Downloaded source is never
 * executed. */
enum vcs_source_package_checkout_result vcs_source_package_checkout(
    struct vcs_package_store *store, const uint8_t package_root[32],
    const uint8_t source_root[32], const char *workspace,
    const char *destination,
    struct vcs_source_package_checkout_metrics *metrics);

#endif /* ZCL_VCS_SOURCE_PACKAGE_CHECKOUT_H */
