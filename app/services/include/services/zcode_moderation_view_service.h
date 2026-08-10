/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Pure presentation projection for the immutable Family moderation policy. */

#ifndef ZCL_SERVICES_ZCODE_MODERATION_VIEW_SERVICE_H
#define ZCL_SERVICES_ZCODE_MODERATION_VIEW_SERVICE_H

#include "vcs/zcode_commons_v2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZCODE_MODERATION_VIEW_SERVICE_ID "zcode.moderation.view.v1"
#define ZCODE_MODERATION_VIEW_ABI_FINGERPRINT \
    "zcode.moderation.view.abi.v1:a7163e2d"
#define ZCODE_MODERATION_VIEW_SCHEMA_FINGERPRINT \
    "zcl.zcode_moderation_status.v1+service-status.v1+policy-list.v1+policy-show.v1"
#define ZCODE_MODERATION_VIEW_WIRE_FINGERPRINT \
    "family-policy+service-readiness-caller-owned-view.v1"
#define ZCODE_MODERATION_VIEW_KAT_FINGERPRINT \
    "6b82ad2e6786c41076bdba5acb8050cbaef01c8d3cdb94dfbf6e99874e027ab3"

struct zcode_moderation_policy_view_v1 {
    bool valid;
    char policy_root[65];
    uint32_t excluded_reason_mask;
    uint32_t max_dependency_objects;
    uint64_t max_extracted_bytes;
    char pass_audiences[48];
    char pass_behaviors[32];
    char incomplete_result[16];
    char new_content_state[16];
    char contextual_eligibility[160];
    bool separate_from_accuracy_quality_security;
    char policy_summary[160];
};

struct zcode_moderation_service_status_input_v1 {
    bool projection_ready;
    uint32_t registered_service_count;
    uint32_t eligible_service_count;
    bool roster_finalized;
    bool classification_enabled;
    bool advertisement_enabled;
    bool chain_selection_enabled;
    bool operator_group_diversity_declared;
};

struct zcode_moderation_service_status_result_v1 {
    bool valid;
    bool ready;
    char bootstrap_label[64];
    char blocker[192];
    char next_command[64];
};

struct zcode_moderation_view_service_v1 {
    bool (*render_policy)(const struct vcs_zcode_family_policy_v1 *policy,
                          const char *policy_root_hex,
                          struct zcode_moderation_policy_view_v1 *out);
    bool (*render_service_status)(
        const struct zcode_moderation_service_status_input_v1 *input,
        struct zcode_moderation_service_status_result_v1 *out);
};

const struct zcode_moderation_view_service_v1 *
zcode_moderation_view_service_builtin(void);

struct zcl_hotswap_service_contract;
const struct zcl_hotswap_service_contract *
zcl_native_zcode_moderation_view_service_contract(void);

#endif /* ZCL_SERVICES_ZCODE_MODERATION_VIEW_SERVICE_H */
