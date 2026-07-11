/* Copyright (c) 2019 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "consensus/params.h"
#include "consensus/upgrades.h"

bool consensus_network_upgrade_active(const struct consensus_params *params,
                                       int nHeight, enum upgrade_index idx)
{
    return consensus_upgrade_state(nHeight, params, idx) == UPGRADE_ACTIVE;
}

int consensus_halving(const struct consensus_params *params, int nHeight)
{
    if (consensus_network_upgrade_active(params, nHeight, UPGRADE_BUTTERCUP)) {
        int buttercup_height = params->vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight;
        int halvings = (nHeight - consensus_subsidy_slow_start_shift(params) - buttercup_height) / params->nPostButtercupSubsidyHalvingInterval;
        return halvings + 3;
    }
    return (nHeight - consensus_subsidy_slow_start_shift(params)) / params->nPreButtercupSubsidyHalvingInterval;
}

int64_t consensus_pow_target_spacing(const struct consensus_params *params, int nHeight)
{
    return consensus_network_upgrade_active(params, nHeight, UPGRADE_BUTTERCUP)
        ? params->nPostButtercupPowTargetSpacing
        : params->nPreButtercupPowTargetSpacing;
}

int64_t consensus_averaging_window_timespan(const struct consensus_params *params, int nHeight)
{
    return params->nPowAveragingWindow * consensus_pow_target_spacing(params, nHeight);
}

int64_t consensus_min_actual_timespan(const struct consensus_params *params, int nHeight)
{
    return (consensus_averaging_window_timespan(params, nHeight) * (100 - params->nPowMaxAdjustUp)) / 100;
}

int64_t consensus_max_actual_timespan(const struct consensus_params *params, int nHeight)
{
    return (consensus_averaging_window_timespan(params, nHeight) * (100 + params->nPowMaxAdjustDown)) / 100;
}
