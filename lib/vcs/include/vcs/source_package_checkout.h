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
    uint32_t authority_objects;
    uint32_t work_receipts;
    uint8_t accepted_signer[32];
};

const char *vcs_source_package_checkout_result_string(
    enum vcs_source_package_checkout_result result);

/* Discover the exact accepted-work root carried by a complete source
 * package.  This re-verifies the content.v2 manifest and PROVEN lane receipt
 * and requires the closed authority bundle to be present; it does not accept
 * the receipt as sufficient proof.  Pass the returned root to
 * vcs_source_package_checkout_accepted(), which imports and verifies the full
 * task/candidate/policy/receipt chain before materializing source. */
enum vcs_source_package_checkout_result
vcs_source_package_accepted_work_discover(
    struct vcs_package_store *store, const uint8_t package_root[32],
    const uint8_t source_root[32], uint8_t accepted_work_root[32]);

/* Read one complete content.v2 package from the existing store, rederive its
 * root and exact source-carrier shape, verify every source shard before any
 * write, import the ZVCS tree, then materialize source and pinned vendor
 * archives into separate explicit scratch roots. Downloaded source is never
 * executed. */
enum vcs_source_package_checkout_result vcs_source_package_checkout(
    struct vcs_package_store *store, const uint8_t package_root[32],
    const uint8_t source_root[32], const uint8_t expected_signer[32],
    const char *workspace,
    const char *destination,
    struct vcs_source_package_checkout_metrics *metrics);

/* Full publication consumer path. accepted_work_root selects the complete
 * carried proof chain; the expected signer is derived from its candidate and
 * is never accepted as an input or learned from a lone receipt. */
enum vcs_source_package_checkout_result
vcs_source_package_checkout_accepted(
    struct vcs_package_store *store, const uint8_t package_root[32],
    const uint8_t source_root[32], const uint8_t accepted_work_root[32],
    const char *workspace, const char *destination,
    struct vcs_source_package_checkout_metrics *metrics);

/* Re-derive the self-describing PROVEN carrier identity, reconstruct it in
 * fresh private scratch, verify the complete accepted-work authority chain,
 * and remove the scratch tree before returning. This is the proof primitive
 * for source-reproduction evidence; it grants no execution, install,
 * acceptance, network, wallet, or deployment authority. */
enum vcs_source_package_checkout_result
vcs_source_package_reconstruct_verify(
    struct vcs_package_store *store, const uint8_t package_root[32],
    uint8_t source_root_out[32], uint8_t accepted_work_root_out[32],
    struct vcs_source_package_checkout_metrics *metrics);

#endif /* ZCL_VCS_SOURCE_PACKAGE_CHECKOUT_H */
