/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure block-subsidy arithmetic. Replays from (height, params).
 * No clock, RNG, allocation, or I/O. */

#include "domain/consensus/subsidy.h"

#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "core/amount.h"

struct zcl_result domain_consensus_block_subsidy(
        int n_height,
        const struct consensus_params *params,
        int64_t *out_subsidy)
{
    if (!params)
        return ZCL_ERR(DOMAIN_CONSENSUS_SUBSIDY_ERR_NULL_PARAMS,
                       "block_subsidy: null params at height %d", n_height);
    if (!out_subsidy)
        return ZCL_ERR(DOMAIN_CONSENSUS_SUBSIDY_ERR_NULL_OUT,
                       "block_subsidy: null out_subsidy at height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_SUBSIDY_ERR_NEG_HEIGHT,
                       "block_subsidy: negative height %d", n_height);

    int64_t n_subsidy = (int64_t)(12.5 * COIN);

    /* Slow-start ramp-up: first interval/2 blocks pay nHeight/interval of
     * the full reward; the next interval/2 pay (nHeight+1)/interval. */
    if (n_height < params->nSubsidySlowStartInterval / 2) {
        n_subsidy /= params->nSubsidySlowStartInterval;
        n_subsidy *= n_height;
        *out_subsidy = n_subsidy;
        return ZCL_OK;
    } else if (n_height < params->nSubsidySlowStartInterval) {
        n_subsidy /= params->nSubsidySlowStartInterval;
        n_subsidy *= (n_height + 1);
        *out_subsidy = n_subsidy;
        return ZCL_OK;
    }

    /* Post slow-start: standard halving schedule. */
    int halvings = consensus_halving(params, n_height);

    /* Beyond 64 halvings the subsidy underflows to zero. */
    if (halvings >= 64) {
        *out_subsidy = 0;
        return ZCL_OK;
    }

    if (consensus_network_upgrade_active(params, n_height, UPGRADE_BUTTERCUP)) {
        *out_subsidy = (n_subsidy / BUTTERCUP_POW_TARGET_SPACING_RATIO) >> halvings;
        return ZCL_OK;
    }

    *out_subsidy = n_subsidy >> halvings;
    return ZCL_OK;
}
