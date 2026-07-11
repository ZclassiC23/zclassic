/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2018 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure protocol-upgrade activation-height arithmetic. Replays from
 * (n_height, consensus_params, idx). No clock, RNG, allocation, or
 * I/O. Read-only consultation of NetworkUpgradeInfo (a compile-time
 * const table) is permitted.
 *
 * The legacy lib/consensus/src/upgrades.c is now a thin wrapper over
 * these functions; signatures and observable behaviour are preserved
 * there. */

#include "domain/consensus/upgrades.h"

#include "consensus/params.h"
#include "consensus/upgrades.h"

static bool dcu_idx_valid(enum upgrade_index idx)
{
    return (int)idx >= (int)BASE_SPROUT && (int)idx < (int)MAX_NETWORK_UPGRADES;
}

/* Local copy of the legacy is-active predicate. Defined here to keep
 * the domain function self-contained (does not call into the legacy
 * lib/consensus layer for its own answer). */
static bool dcu_is_active(int n_height, const struct consensus_params *params,
                          enum upgrade_index idx)
{
    int activation = params->vUpgrades[idx].nActivationHeight;
    if (activation == NETWORK_UPGRADE_NO_ACTIVATION)
        return false;
    return n_height >= activation;
}

struct zcl_result domain_consensus_upgrade_state(
        int n_height,
        const struct consensus_params *params,
        enum upgrade_index idx,
        enum upgrade_state *out_state)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS,
                       "upgrade_state: null params at height %d", n_height);
    if (!out_state)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "upgrade_state: null out at height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT,
                       "upgrade_state: negative height %d", n_height);
    if (!dcu_idx_valid(idx))
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_BAD_IDX,
                       "upgrade_state: idx %d out of range", (int)idx);

    int activation = params->vUpgrades[idx].nActivationHeight;
    if (activation == NETWORK_UPGRADE_NO_ACTIVATION)
        *out_state = UPGRADE_DISABLED;
    else if (n_height >= activation)
        *out_state = UPGRADE_ACTIVE;
    else
        *out_state = UPGRADE_PENDING;
    return ZCL_OK;
}

struct zcl_result domain_consensus_current_epoch(
        int n_height,
        const struct consensus_params *params,
        int *out_epoch)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS,
                       "current_epoch: null params at height %d", n_height);
    if (!out_epoch)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "current_epoch: null out at height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT,
                       "current_epoch: negative height %d", n_height);

    for (int i = (int)MAX_NETWORK_UPGRADES - 1; i >= (int)BASE_SPROUT; i--) {
        if (dcu_is_active(n_height, params, (enum upgrade_index)i)) {
            *out_epoch = i;
            return ZCL_OK;
        }
    }
    *out_epoch = (int)BASE_SPROUT;
    return ZCL_OK;
}

struct zcl_result domain_consensus_current_epoch_branch_id(
        int n_height,
        const struct consensus_params *params,
        uint32_t *out_branch_id)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS,
                       "current_epoch_branch_id: null params at height %d", n_height);
    if (!out_branch_id)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "current_epoch_branch_id: null out at height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT,
                       "current_epoch_branch_id: negative height %d", n_height);

    int epoch = (int)BASE_SPROUT;
    struct zcl_result r = domain_consensus_current_epoch(n_height, params, &epoch);
    if (!r.ok)
        return r;
    *out_branch_id = NetworkUpgradeInfo[epoch].nBranchId;
    return ZCL_OK;
}

struct zcl_result domain_consensus_is_branch_id(
        uint32_t branch_id,
        bool *out_known)
{
    if (!out_known)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "is_branch_id: null out for branch 0x%08x", branch_id);

    for (int i = (int)BASE_SPROUT; i < (int)MAX_NETWORK_UPGRADES; i++) {
        if (branch_id == NetworkUpgradeInfo[i].nBranchId) {
            *out_known = true;
            return ZCL_OK;
        }
    }
    *out_known = false;
    return ZCL_OK;
}

struct zcl_result domain_consensus_is_activation_height(
        int n_height,
        const struct consensus_params *params,
        enum upgrade_index idx,
        bool *out_match)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS,
                       "is_activation_height: null params at height %d", n_height);
    if (!out_match)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "is_activation_height: null out at height %d", n_height);
    if (!dcu_idx_valid(idx))
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_BAD_IDX,
                       "is_activation_height: idx %d out of range", (int)idx);

    /* BASE_SPROUT has no activation height: never matches. Legacy
     * code logged this as a fail; we report out_match=false and let
     * the wrapper preserve the log line if it wants. */
    if (idx == BASE_SPROUT) {
        *out_match = false;
        return ZCL_OK;
    }

    *out_match = (n_height >= 0)
              && (n_height == params->vUpgrades[idx].nActivationHeight);
    return ZCL_OK;
}

struct zcl_result domain_consensus_is_activation_height_any(
        int n_height,
        const struct consensus_params *params,
        bool *out_match)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS,
                       "is_activation_height_any: null params at height %d", n_height);
    if (!out_match)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "is_activation_height_any: null out at height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT,
                       "is_activation_height_any: negative height %d", n_height);

    for (int i = (int)BASE_SPROUT + 1; i < (int)MAX_NETWORK_UPGRADES; i++) {
        if (n_height == params->vUpgrades[i].nActivationHeight) {
            *out_match = true;
            return ZCL_OK;
        }
    }
    *out_match = false;
    return ZCL_OK;
}

struct zcl_result domain_consensus_next_epoch(
        int n_height,
        const struct consensus_params *params,
        int *out_epoch)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS,
                       "next_epoch: null params at height %d", n_height);
    if (!out_epoch)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "next_epoch: null out at height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT,
                       "next_epoch: negative height %d", n_height);

    for (int i = (int)BASE_SPROUT + 1; i < (int)MAX_NETWORK_UPGRADES; i++) {
        int activation = params->vUpgrades[i].nActivationHeight;
        if (activation != NETWORK_UPGRADE_NO_ACTIVATION
            && n_height < activation) {
            *out_epoch = i;
            return ZCL_OK;
        }
    }
    return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NO_PENDING,
                   "next_epoch: no pending upgrade at height %d", n_height);
}

struct zcl_result domain_consensus_next_activation_height(
        int n_height,
        const struct consensus_params *params,
        int *out_activation)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS,
                       "next_activation_height: null params at height %d", n_height);
    if (!out_activation)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT,
                       "next_activation_height: null out at height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT,
                       "next_activation_height: negative height %d", n_height);

    int idx = 0;
    struct zcl_result r = domain_consensus_next_epoch(n_height, params, &idx);
    if (!r.ok)
        return r;
    *out_activation = params->vUpgrades[idx].nActivationHeight;
    return ZCL_OK;
}
