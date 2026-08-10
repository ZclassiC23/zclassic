/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Pure presentation projection for persisted ZCODE package index entries. */

#ifndef ZCL_SERVICES_ZCODE_PACKAGE_VIEW_SERVICE_H
#define ZCL_SERVICES_ZCODE_PACKAGE_VIEW_SERVICE_H

#include "vcs/package_index.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZCODE_PACKAGE_VIEW_SERVICE_ID "zcode.package.view.v1"
#define ZCODE_PACKAGE_VIEW_ABI_FINGERPRINT \
    "zcode.package.view.abi.v1:6cf12b80"
#define ZCODE_PACKAGE_VIEW_SCHEMA_FINGERPRINT \
    "zcl.zcode_package_guide.v1+zcl.zcode_package_search.v1+zcl.zcode_package_show.v1"
#define ZCODE_PACKAGE_VIEW_WIRE_FINGERPRINT \
    "package-index-entry+bounded-view+guide.v1"
#define ZCODE_PACKAGE_VIEW_KAT_FINGERPRINT \
    "fa9b02a7fd52948c536fdb4894eae780e818a5fe2221871c63568130640d6728"

struct zcode_package_view_entry_v1 {
    bool valid;
    char release_id[65];
    char package_root[65];
    char name[VCS_PACKAGE_RELEASE_NAME_MAX + 1u];
    char semver[VCS_PACKAGE_RELEASE_SEMVER_MAX + 1u];
    char license[VCS_PACKAGE_RELEASE_LICENSE_MAX + 1u];
    char publisher[2u * VCS_PACKAGE_RELEASE_PUBKEY_BYTES + 1u];
    char chain_id[VCS_PACKAGE_RELEASE_CHAIN_ID_MAX + 1u];
    char reward_address[VCS_PACKAGE_RELEASE_REWARD_MAX + 1u];
    uint64_t publisher_sequence;
    bool has_parent;
    char parent_root[65];
    bool has_znam;
    char znam[VCS_PACKAGE_RELEASE_ZNAM_MAX + 1u];
    bool manifest_present;
    uint32_t file_count;
    uint64_t total_bytes;
    uint32_t chunk_total;
    bool license_present;
    uint32_t executable_count;
};

struct zcode_package_guide_result_v1 {
    bool cas_authority_static;
    bool index_reads_static;
    bool publication_static;
    bool execution_static;
    char live_surface[128];
    char static_boundary[192];
    char next_command[192];
};

struct zcode_package_view_service_v1 {
    bool (*render_entry)(const struct vcs_package_index_entry *entry,
                         struct zcode_package_view_entry_v1 *out);
    bool (*render_guide)(struct zcode_package_guide_result_v1 *out);
};

const struct zcode_package_view_service_v1 *
zcode_package_view_service_builtin(void);

struct zcl_hotswap_service_contract;
const struct zcl_hotswap_service_contract *
zcl_native_zcode_package_view_service_contract(void);

#endif /* ZCL_SERVICES_ZCODE_PACKAGE_VIEW_SERVICE_H */
