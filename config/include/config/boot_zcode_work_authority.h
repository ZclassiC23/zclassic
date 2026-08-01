/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Import immutable ZCODE task/candidate authority for swarm work. */

#ifndef ZCL_CONFIG_BOOT_ZCODE_WORK_AUTHORITY_H
#define ZCL_CONFIG_BOOT_ZCODE_WORK_AUTHORITY_H

#include "util/result.h"

#include <stdint.h>

struct vcs_zcode_work_context_v1;
struct vcs_package_store;

struct zcl_result boot_zcode_work_authority_import(
    struct vcs_package_store *store, const uint8_t context_root[32],
    const char *workspace, const struct vcs_zcode_work_context_v1 *context);

#endif /* ZCL_CONFIG_BOOT_ZCODE_WORK_AUTHORITY_H */
