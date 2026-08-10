/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Pure Family-policy presentation over caller-owned input and output. */
// one-result-type-ok:pure-vtable-uses-bounded-caller-owned-output-only

#include "services/zcode_moderation_view_service.h"

#include "hotswap/hotswap_service.h"

#include <stdio.h>
#include <string.h>

static bool render_policy(const struct vcs_zcode_family_policy_v1 *policy,
                          const char *policy_root_hex,
                          struct zcode_moderation_policy_view_v1 *out)
{
    if (!policy || !policy_root_hex || !out)
        return false;
    memset(out, 0, sizeof(*out));
    (void)snprintf(out->policy_root, sizeof(out->policy_root), "%s",
                   policy_root_hex);
    out->excluded_reason_mask = policy->excluded_reason_mask;
    out->max_dependency_objects = policy->max_dependency_objects;
    out->max_extracted_bytes = policy->max_extracted_bytes;
    (void)snprintf(out->pass_audiences, sizeof(out->pass_audiences), "%s",
                   "GENERAL|CONTEXTUAL_SCIENCE");
    (void)snprintf(out->pass_behaviors, sizeof(out->pass_behaviors), "%s",
                   "BENIGN|DUAL_USE");
    (void)snprintf(out->incomplete_result, sizeof(out->incomplete_result),
                   "%s", "UNKNOWN");
    (void)snprintf(out->new_content_state, sizeof(out->new_content_state),
                   "%s", "PENDING");
    (void)snprintf(out->contextual_eligibility,
                   sizeof(out->contextual_eligibility), "%s",
                   "neutral scientific, medical, historical, cybersecurity and dual-use education");
    out->separate_from_accuracy_quality_security = true;
    (void)snprintf(out->policy_summary, sizeof(out->policy_summary), "%s",
                   "immutable Family policy and service-roster presentation; enforcement remains resident and incomplete");
    out->valid = strlen(out->policy_root) == 64 &&
                 out->max_dependency_objects > 0 &&
                 out->max_extracted_bytes > 0;
    return true;
}

static const struct zcode_moderation_view_service_v1 k_builtin = {
    .render_policy = render_policy,
};

ZCL_HOTSWAP_SERVICE_EXPORT(
    ZCODE_MODERATION_VIEW_SERVICE_ID, k_builtin,
    ZCODE_MODERATION_VIEW_ABI_FINGERPRINT,
    ZCODE_MODERATION_VIEW_SCHEMA_FINGERPRINT,
    ZCODE_MODERATION_VIEW_WIRE_FINGERPRINT,
    ZCODE_MODERATION_VIEW_KAT_FINGERPRINT)

const struct zcode_moderation_view_service_v1 *
zcode_moderation_view_service_builtin(void)
{
    return &k_builtin;
}
