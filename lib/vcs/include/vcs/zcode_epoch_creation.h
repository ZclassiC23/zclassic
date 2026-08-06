/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: ordered epoch accounting for creation-backed ZC23 issuance. */
#ifndef ZCL_VCS_ZCODE_EPOCH_CREATION_H
#define ZCL_VCS_ZCODE_EPOCH_CREATION_H

#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_EPOCH_CREATION_DOMAIN "zcl.zcode.epoch_creation_set.v1"
#define VCS_ZCODE_EPOCH_CREATION_VERSION 1u
#define VCS_ZCODE_EPOCH_CREATION_HEADER_BYTES 276u
#define VCS_ZCODE_EPOCH_CREATION_MAX_ATTRIBUTIONS 4096u
#define VCS_ZCODE_EPOCH_CREATION_MAX_WIRE_BYTES \
    (VCS_ZCODE_EPOCH_CREATION_HEADER_BYTES + \
     VCS_ZCODE_EPOCH_CREATION_MAX_ATTRIBUTIONS * 32u)

enum vcs_zcode_epoch_creation_error {
    VCS_ZCODE_EPOCH_CREATION_OK = 0,
    VCS_ZCODE_EPOCH_CREATION_NULL,
    VCS_ZCODE_EPOCH_CREATION_ALLOC,
    VCS_ZCODE_EPOCH_CREATION_WIRE_SIZE,
    VCS_ZCODE_EPOCH_CREATION_MAGIC,
    VCS_ZCODE_EPOCH_CREATION_SCHEMA,
    VCS_ZCODE_EPOCH_CREATION_RESERVED,
    VCS_ZCODE_EPOCH_CREATION_ROOT,
    VCS_ZCODE_EPOCH_CREATION_ORDER,
    VCS_ZCODE_EPOCH_CREATION_PREDECESSOR,
    VCS_ZCODE_EPOCH_CREATION_CAP,
    VCS_ZCODE_EPOCH_CREATION_SUM,
    VCS_ZCODE_EPOCH_CREATION_TIME,
    VCS_ZCODE_EPOCH_CREATION_OVERFLOW,
};

struct vcs_zcode_epoch_creation_set_v1 {
    uint16_t schema_version;
    uint64_t epoch;
    uint64_t emission_cap_atoms;
    uint64_t actual_mint_atoms;
    uint64_t unissued_atoms;
    uint8_t network_genesis_root[32];
    uint8_t zc23_policy_root[32];
    uint8_t previous_epoch_creation_root[32];
    uint8_t committee_evidence_snapshot_root[32];
    uint64_t opening_height;
    uint8_t opening_hash[32];
    int64_t opening_mtp;
    uint64_t maturity_height;
    uint8_t maturity_hash[32];
    int64_t maturity_mtp;
    uint8_t (*attribution_roots)[32];
    size_t attribution_count;
};

void vcs_zcode_epoch_creation_init(
    struct vcs_zcode_epoch_creation_set_v1 *set);
void vcs_zcode_epoch_creation_free(
    struct vcs_zcode_epoch_creation_set_v1 *set);
const char *vcs_zcode_epoch_creation_error_string(
    enum vcs_zcode_epoch_creation_error error);
enum vcs_zcode_epoch_creation_error vcs_zcode_epoch_creation_validate(
    const struct vcs_zcode_epoch_creation_set_v1 *set);
enum vcs_zcode_epoch_creation_error vcs_zcode_epoch_creation_serialize(
    const struct vcs_zcode_epoch_creation_set_v1 *set,
    uint8_t **out, size_t *out_len);
enum vcs_zcode_epoch_creation_error vcs_zcode_epoch_creation_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_epoch_creation_set_v1 *out);
enum vcs_zcode_epoch_creation_error vcs_zcode_epoch_creation_root(
    const struct vcs_zcode_epoch_creation_set_v1 *set, uint8_t out[32]);

/* Policy epoch 0 is the separately attributable initial 1.00000000 ZC23.
 * Policy epochs >=1 map (epoch-1)/208 to the frozen whole-token era curve. */
enum vcs_zcode_epoch_creation_error vcs_zc23_policy_epoch_cap_atoms(
    uint64_t epoch, uint64_t *out_atoms);

#endif /* ZCL_VCS_ZCODE_EPOCH_CREATION_H */
