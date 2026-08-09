/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Pure simulation-only economics calculations. No issuance, wallet, storage,
 * clock, network, process, or node-global authority belongs here. */
// one-result-type-ok:pure-vtable-preserves-versioned-vcs-error-enums

#include "services/zcode_c23_economics_service.h"

#include "hotswap/hotswap_service.h"

#include <stdio.h>
#include <string.h>

static bool render_status(struct zcode_c23_economics_status_result_v1 *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->challenge_blocks = VCS_ZCODE_COMMONS_CHALLENGE_BLOCKS;
    out->challenge_seconds = VCS_ZCODE_COMMONS_CHALLENGE_SECONDS;
    for (uint16_t i = 0; i < VCS_ZCODE_COMMONS_CATEGORY_COUNT; i++)
        out->award_atoms[i] = vcs_zcode_creation_award_atoms_v2(i);
    (void)snprintf(out->queue_order, sizeof(out->queue_order),
                   "maturity_height,maturity_mtp,claim_root");
    (void)snprintf(out->category_order, sizeof(out->category_order),
                   "previous-epoch-root rotation, cyclic");
    (void)snprintf(out->concentration_cap, sizeof(out->concentration_cap),
                   "min(epoch_capacity,max(1 ZC23,floor(epoch_capacity/100)))");
    return true;
}

static const struct zcode_c23_economics_service_v1 k_builtin = {
    .award_atoms = vcs_zcode_creation_award_atoms_v2,
    .policy_init = vcs_zcode_policy_candidate_v2_init,
    .policy_validate = vcs_zcode_policy_candidate_v2_validate,
    .policy_root = vcs_zcode_policy_candidate_v2_root,
    .epoch_select = vcs_zcode_epoch_select_v2,
    .render_status = render_status,
};

ZCL_HOTSWAP_SERVICE_EXPORT(
    ZCODE_C23_ECONOMICS_SERVICE_ID, k_builtin,
    ZCODE_C23_ECONOMICS_ABI_FINGERPRINT,
    ZCODE_C23_ECONOMICS_SCHEMA_FINGERPRINT,
    ZCODE_C23_ECONOMICS_WIRE_FINGERPRINT,
    ZCODE_C23_ECONOMICS_KAT_FINGERPRINT)

const struct zcode_c23_economics_service_v1 *
zcode_c23_economics_service_builtin(void)
{
    return &k_builtin;
}
