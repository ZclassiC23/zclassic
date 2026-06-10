/* Copyright (c) 2018 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Epoch-I thin wrapper. The pure protocol-upgrade activation-height
 * arithmetic lives in domain/consensus/upgrades.{h,c}; this file
 * preserves the legacy bool / int / uint32_t-returning signatures so
 * existing callers (wallet, validation, mining, consensus_params,
 * RPC, ...) stay unchanged while the domain functions return typed
 * zcl_result. */

#include "consensus/upgrades.h"
#include "domain/consensus/upgrades.h"
#include "util/log_macros.h"
#include <assert.h>

const struct nu_info NetworkUpgradeInfo[MAX_NETWORK_UPGRADES] = {
    { 0,          "Sprout",      "The Zclassic network at launch" },
    { 0x74736554, "Test dummy",  "Test dummy info" },
    { 0x5ba81b19, "Overwinter",  "See https://z.cash/upgrade/overwinter.html for details." },
    { 0x76b809bb, "Sapling",     "See https://z.cash/upgrade/sapling.html for details." },
    { 0x821a451c, "Bubbles",     "See ZClassic for details." },
    { 0x930b540d, "Bubbly",      "See ZClassic for details." },
    { 0x930b540d, "Buttercup",   "See ZClassic for details." },
};

const uint32_t SPROUT_BRANCH_ID = 0;

struct equihash_info EquihashUpgradeInfo[MAX_NETWORK_UPGRADES] = {
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { EQUIHASH_DEFAULT_PARAMS, EQUIHASH_DEFAULT_PARAMS },
    { 192, 7 },
    { 192, 7 },
    { 192, 7 },
};

enum upgrade_state consensus_upgrade_state(int nHeight, const struct consensus_params *params,
                                            enum upgrade_index idx)
{
    assert(nHeight >= 0);
    assert(idx >= BASE_SPROUT && idx < MAX_NETWORK_UPGRADES);
    enum upgrade_state st = UPGRADE_DISABLED;
    struct zcl_result r = domain_consensus_upgrade_state(nHeight, params, idx, &st);
    (void)r; /* asserts above match the domain contract; on success r.ok */
    return st;
}

int consensus_current_epoch(int nHeight, const struct consensus_params *params)
{
    int epoch = (int)BASE_SPROUT;
    struct zcl_result r = domain_consensus_current_epoch(nHeight, params, &epoch);
    (void)r;
    return epoch;
}

uint32_t consensus_current_epoch_branch_id(int nHeight, const struct consensus_params *params)
{
    uint32_t branch = 0;
    struct zcl_result r = domain_consensus_current_epoch_branch_id(nHeight, params, &branch);
    (void)r;
    return branch;
}

bool consensus_is_branch_id(int branchId)
{
    bool known = false;
    struct zcl_result r = domain_consensus_is_branch_id((uint32_t)branchId, &known);
    if (!r.ok)
        LOG_FAIL("consensus", "is_branch_id: %s", r.message);
    if (!known)
        LOG_FAIL("consensus", "unrecognized branch id 0x%08x", (uint32_t)branchId);
    return true;
}

bool consensus_is_activation_height(int nHeight, const struct consensus_params *params,
                                     enum upgrade_index idx)
{
    assert(idx >= BASE_SPROUT && idx < MAX_NETWORK_UPGRADES);
    if (idx == BASE_SPROUT)
        LOG_FAIL("consensus", "BASE_SPROUT has no activation height");
    bool match = false;
    struct zcl_result r = domain_consensus_is_activation_height(nHeight, params, idx, &match);
    if (!r.ok)
        LOG_FAIL("consensus", "is_activation_height: %s", r.message);
    return match;
}

bool consensus_is_activation_height_any(int nHeight, const struct consensus_params *params)
{
    if (nHeight < 0)
        LOG_FAIL("consensus", "is_activation_height_any: negative height %d", nHeight);
    bool match = false;
    struct zcl_result r = domain_consensus_is_activation_height_any(nHeight, params, &match);
    if (!r.ok)
        LOG_FAIL("consensus", "is_activation_height_any: %s", r.message);
    if (!match)
        LOG_FAIL("consensus", "height %d is not an activation height for any upgrade", nHeight);
    return true;
}

int consensus_next_epoch(int nHeight, const struct consensus_params *params)
{
    if (nHeight < 0)
        LOG_ERR("consensus", "next_epoch: negative height %d", nHeight);
    int epoch = 0;
    struct zcl_result r = domain_consensus_next_epoch(nHeight, params, &epoch);
    if (!r.ok)
        LOG_ERR("consensus", "next_epoch: no pending upgrade at height %d", nHeight);
    return epoch;
}

int consensus_next_activation_height(int nHeight, const struct consensus_params *params)
{
    int act = 0;
    struct zcl_result r = domain_consensus_next_activation_height(nHeight, params, &act);
    if (!r.ok)
        LOG_ERR("consensus", "next_activation_height: no next epoch at height %d", nHeight);
    return act;
}
