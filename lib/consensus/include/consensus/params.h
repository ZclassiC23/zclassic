/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>

/* The ordered list of network upgrades. The ordinal value is also the
 * index into consensus_params.vUpgrades[] and NetworkUpgradeInfo[], and
 * "epoch" everywhere in the upgrade code means one of these indices.
 * Order is consensus-significant: epoch comparison (current/next epoch)
 * relies on later upgrades having higher ordinals. BASE_SPROUT (0) is
 * the always-active base epoch and has no activation height.
 * MAX_NETWORK_UPGRADES is the count sentinel, not a real upgrade. */
enum upgrade_index {
    BASE_SPROUT = 0,
    UPGRADE_TESTDUMMY,
    UPGRADE_OVERWINTER,
    UPGRADE_SAPLING,
    UPGRADE_BUBBLES,
    UPGRADE_DIFFADJ,
    UPGRADE_BUTTERCUP,
    MAX_NETWORK_UPGRADES
};

/* Sentinel activation heights stored in network_upgrade.nActivationHeight.
 * ALWAYS_ACTIVE (0) → upgrade is active from genesis; NO_ACTIVATION (-1)
 * → upgrade is DISABLED (never activates). Any other value is the height
 * at and above which the upgrade is ACTIVE. consensus_upgrade_state()
 * and consensus_network_upgrade_active() decode these. */
#define NETWORK_UPGRADE_ALWAYS_ACTIVE 0
#define NETWORK_UPGRADE_NO_ACTIVATION (-1)

/* One row of the activation schedule. nActivationHeight is the
 * consensus-critical field (see the sentinels above); nProtocolVersion
 * records the P2P protocol version associated with the upgrade. */
struct network_upgrade {
    int nProtocolVersion;
    int nActivationHeight;
};

#define PRE_BUTTERCUP_POW_TARGET_SPACING 150
#define POST_BUTTERCUP_POW_TARGET_SPACING 75
#define PRE_BUTTERCUP_HALVING_INTERVAL 840000
#define PRE_BUTTERCUP_REGTEST_HALVING_INTERVAL 150
#define BUTTERCUP_POW_TARGET_SPACING_RATIO (PRE_BUTTERCUP_POW_TARGET_SPACING / POST_BUTTERCUP_POW_TARGET_SPACING)
#define POST_BUTTERCUP_HALVING_INTERVAL (PRE_BUTTERCUP_HALVING_INTERVAL * BUTTERCUP_POW_TARGET_SPACING_RATIO)
#define POST_BUTTERCUP_REGTEST_HALVING_INTERVAL (PRE_BUTTERCUP_REGTEST_HALVING_INTERVAL * BUTTERCUP_POW_TARGET_SPACING_RATIO)

/* Miner-signaled Equihash parameter change (no flag-day height).
 * Blocks signal by setting bit nSignalBit of nVersion. Heights are
 * grouped into fixed nWindow-block windows aligned to multiples of
 * nWindow; a window PASSES when at least nThreshold of its blocks
 * signal. After nConsecutiveWindows passing windows in a row (a
 * failing window resets the streak) the deployment is LOCKED_IN at
 * that window boundary, and the new parameters become mandatory
 * nGraceBlocks later (ACTIVE). No expiry: the deployment stays open
 * until miners reach the threshold. Evaluated purely from headers —
 * see consensus/versionbits.h. */
struct eh_upgrade_deployment {
    bool    enabled;             /* false = machinery fully disabled */
    int     nSignalBit;          /* nVersion bit index miners set */
    int64_t nWindow;             /* window size, boundary-aligned */
    int64_t nThreshold;          /* signaling blocks needed per window */
    int     nConsecutiveWindows; /* passing streak for LOCKED_IN */
    int64_t nGraceBlocks;        /* ACTIVE = locked_in + grace */
};

struct consensus_params {
    struct uint256 hashGenesisBlock;
    bool fCoinbaseMustBeProtected;
    int nSubsidySlowStartInterval;
    int nPreButtercupSubsidyHalvingInterval;
    int nPostButtercupSubsidyHalvingInterval;
    int nMajorityEnforceBlockUpgrade;
    int nMajorityRejectBlockOutdated;
    int nMajorityWindow;
    struct network_upgrade vUpgrades[MAX_NETWORK_UPGRADES];
    struct uint256 powLimit;
    int32_t nPowAllowMinDifficultyBlocksAfterHeight; /* -1 = disabled */
    bool nPowAllowMinDifficultyEnabled;
    bool scaleDifficultyAtUpgradeFork;
    int64_t nPowAveragingWindow;
    int64_t nPowMaxAdjustDown;
    int64_t nPowMaxAdjustUp;
    int64_t nPreButtercupPowTargetSpacing;
    int64_t nPostButtercupPowTargetSpacing;
    struct uint256 nMinimumChainWork;
    struct eh_upgrade_deployment ehUpgrade;
};

/* Half of nSubsidySlowStartInterval. This is the block-height offset
 * applied to every subsidy/halving computation so that the slow-start
 * ramp does not shift the halving schedule: heights are measured as
 * (nHeight - shift) before being divided by a halving interval. Pure
 * integer arithmetic on consensus_params; consensus-critical because it
 * is a term in consensus_halving() and consensus_last_founders_reward_height(). */
static inline int consensus_subsidy_slow_start_shift(const struct consensus_params *p)
{
    return p->nSubsidySlowStartInterval / 2;
}

/* True iff network upgrade `idx` is ACTIVE at `nHeight` — i.e. the
 * upgrade has an activation height set (not NETWORK_UPGRADE_NO_ACTIVATION)
 * and nHeight >= that activation height. Thin predicate over
 * consensus_upgrade_state() (see upgrades.h): returns
 * (state == UPGRADE_ACTIVE), so a PENDING (future) or DISABLED
 * (-1 activation) upgrade both report false. This is THE activation
 * gate every consensus rule consults to decide whether an upgrade's
 * rules apply at a height. */
bool consensus_network_upgrade_active(const struct consensus_params *params, int nHeight, enum upgrade_index idx);

/* Number of subsidy halvings that have occurred by `nHeight`, i.e. the
 * shift count used to compute the block subsidy. Halving epoch depends
 * on whether the Buttercup upgrade is active at nHeight:
 *   - Pre-Buttercup: (nHeight - slow_start_shift) /
 *                    nPreButtercupSubsidyHalvingInterval.
 *   - Post-Buttercup (consensus_network_upgrade_active(.,.,UPGRADE_BUTTERCUP)):
 *     halvings counted from the Buttercup activation height against the
 *     (shorter) nPostButtercupSubsidyHalvingInterval, then + 3 to account
 *     for the three pre-Buttercup halvings already passed. Buttercup
 *     keeps coin issuance on schedule despite the halved (75s vs 150s)
 *     block spacing. Consensus-critical: feeds the block reward. */
int consensus_halving(const struct consensus_params *params, int nHeight);

/* Target seconds between blocks at `nHeight`:
 *   - nPostButtercupPowTargetSpacing (75) once Buttercup is active,
 *   - nPreButtercupPowTargetSpacing (150) before. Consensus-critical:
 *   it is the denominator of the difficulty-retargeting timespan. */
int64_t consensus_pow_target_spacing(const struct consensus_params *params, int nHeight);

/* Expected total timespan of the difficulty averaging window at
 * `nHeight`: nPowAveragingWindow * consensus_pow_target_spacing(.,nHeight).
 * This is the "ideal" duration the actual window timespan is compared
 * against during retargeting. */
int64_t consensus_averaging_window_timespan(const struct consensus_params *params, int nHeight);

/* Lower clamp on the measured averaging-window timespan used by
 * difficulty retargeting at `nHeight`:
 *   averaging_window_timespan * (100 - nPowMaxAdjustUp) / 100.
 * A measured timespan shorter than this is clamped UP to it, bounding
 * how fast difficulty may rise. nPowMaxAdjustUp is a percentage. */
int64_t consensus_min_actual_timespan(const struct consensus_params *params, int nHeight);

/* Upper clamp on the measured averaging-window timespan used by
 * difficulty retargeting at `nHeight`:
 *   averaging_window_timespan * (100 + nPowMaxAdjustDown) / 100.
 * A measured timespan longer than this is clamped DOWN to it, bounding
 * how fast difficulty may fall. nPowMaxAdjustDown is a percentage. */
int64_t consensus_max_actual_timespan(const struct consensus_params *params, int nHeight);

/* Last block height at which a founders' reward is paid. Equals
 * nPreButtercupSubsidyHalvingInterval + slow_start_shift - 1, i.e. one
 * block before the first pre-Buttercup halving boundary (shift-adjusted).
 * Pure arithmetic on consensus_params; used by coinbase validation to
 * gate the founders'-reward output requirement. */
static inline int consensus_last_founders_reward_height(const struct consensus_params *p)
{
    return p->nPreButtercupSubsidyHalvingInterval + consensus_subsidy_slow_start_shift(p) - 1;
}

#endif
