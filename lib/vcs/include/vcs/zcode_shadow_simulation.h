/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: deterministic scratch-only Living Commons shadow plans. */
#ifndef ZCL_VCS_ZCODE_SHADOW_SIMULATION_H
#define ZCL_VCS_ZCODE_SHADOW_SIMULATION_H

#include "vcs/zcode_epoch_creation.h"
#include "vcs/zcode_reproduction_qualification.h"

#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_SHADOW_FIXTURE_DOMAIN \
    "zcl.zcode.shadow_chain_fixture.v1"

enum vcs_zcode_shadow_simulation_error {
    VCS_ZCODE_SHADOW_SIMULATION_OK = 0,
    VCS_ZCODE_SHADOW_SIMULATION_NULL,
    VCS_ZCODE_SHADOW_SIMULATION_CAS,
    VCS_ZCODE_SHADOW_SIMULATION_QUALIFICATION,
    VCS_ZCODE_SHADOW_SIMULATION_POLICY,
    VCS_ZCODE_SHADOW_SIMULATION_AWARD,
    VCS_ZCODE_SHADOW_SIMULATION_ANCHOR,
    VCS_ZCODE_SHADOW_SIMULATION_ATTRIBUTION,
    VCS_ZCODE_SHADOW_SIMULATION_DUPLICATE,
    VCS_ZCODE_SHADOW_SIMULATION_PREDECESSOR,
    VCS_ZCODE_SHADOW_SIMULATION_EPOCH,
    VCS_ZCODE_SHADOW_SIMULATION_OVERFLOW,
};

struct vcs_zcode_shadow_attribution_input {
    const char *workspace;
    const uint8_t *score_receipt_root;
    const uint8_t *policy_candidate_root;
    const uint8_t *reproduction_request_root;
    const uint8_t *reproduction_proof_set_root;
    const uint8_t *contributor_binding_root;
    uint64_t epoch;
    int64_t now_unix;
};

struct vcs_zcode_shadow_attribution_plan {
    struct vcs_zcode_creation_attribution_v1 attribution;
    struct vcs_zcode_reproduction_qualification_report qualification;
    uint8_t attribution_root[32];
    uint8_t fixture_branch_root[32];
};

struct vcs_zcode_shadow_epoch_input {
    const char *workspace;
    const uint8_t *policy_candidate_root;
    const uint8_t *attribution_root;
    const uint8_t *fixture_branch_root;
    const uint8_t *previous_epoch_creation_root;
    int64_t now_unix;
};

struct vcs_zcode_shadow_epoch_plan {
    struct vcs_zcode_epoch_creation_set_v1 epoch;
    uint8_t attribution_root[32];
    uint8_t epoch_root[32];
};

const char *vcs_zcode_shadow_simulation_error_string(
    enum vcs_zcode_shadow_simulation_error error);

enum vcs_zcode_shadow_simulation_error
vcs_zcode_shadow_attribution_plan_cas(
    const struct vcs_zcode_shadow_attribution_input *input,
    struct vcs_zcode_shadow_attribution_plan *out);

enum vcs_zcode_shadow_simulation_error vcs_zcode_shadow_epoch_plan_cas(
    const struct vcs_zcode_shadow_epoch_input *input,
    struct vcs_zcode_shadow_epoch_plan *out);

void vcs_zcode_shadow_epoch_plan_free(
    struct vcs_zcode_shadow_epoch_plan *plan);

#endif /* ZCL_VCS_ZCODE_SHADOW_SIMULATION_H */
