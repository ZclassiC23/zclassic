/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * domain/consensus/upgrades.h — pure protocol-upgrade activation-height
 * arithmetic.
 *
 * Network upgrades (Sprout → Overwinter → Sapling → Bubbles → DiffAdj →
 * Buttercup) are scheduled by activation height in consensus_params. All
 * functions in this module answer pure questions about that schedule:
 *
 *   - state at height (DISABLED / PENDING / ACTIVE)
 *   - current epoch / branch id
 *   - is the given branch id known to consensus?
 *   - is the given height an activation height (for one upgrade, or any)?
 *   - what's the next pending epoch / activation height after here?
 *
 * Inputs are (height, consensus_params, upgrade_index). No clock, RNG,
 * allocation, or I/O. Replays from inputs alone. Read-only globals
 * (NetworkUpgradeInfo) are consulted but the table is itself
 * compile-time const.
 *
 * Layering: domain/consensus/ may #include from util/, core/, consensus/.
 * The legacy consensus_*() API stays unchanged — lib/consensus/src/
 * upgrades.c becomes a thin wrapper layer that calls these functions
 * and preserves the legacy bool / int / uint32_t signatures.
 */

#ifndef ZCL_DOMAIN_CONSENSUS_UPGRADES_H
#define ZCL_DOMAIN_CONSENSUS_UPGRADES_H

#include <stdbool.h>
#include <stdint.h>

#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "util/result.h"

/* Compute the state of network upgrade `idx` at height `n_height`.
 * Returns one of UPGRADE_DISABLED / UPGRADE_PENDING / UPGRADE_ACTIVE.
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS  params == NULL
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT     out_state == NULL
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT   n_height < 0
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_BAD_IDX      idx not in [BASE_SPROUT, MAX_NETWORK_UPGRADES)
 */
struct zcl_result domain_consensus_upgrade_state(
        int n_height,
        const struct consensus_params *params,
        enum upgrade_index idx,
        enum upgrade_state *out_state);

/* Compute the highest-numbered upgrade that is ACTIVE at height
 * `n_height`. Returns at minimum BASE_SPROUT (the always-active base
 * epoch). On success returns ZCL_OK and writes the upgrade_index to
 * *out_epoch.
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT
 */
struct zcl_result domain_consensus_current_epoch(
        int n_height,
        const struct consensus_params *params,
        int *out_epoch);

/* Compute the consensus branch id of the upgrade currently active at
 * height `n_height`. (NetworkUpgradeInfo[current_epoch].nBranchId.)
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT
 */
struct zcl_result domain_consensus_current_epoch_branch_id(
        int n_height,
        const struct consensus_params *params,
        uint32_t *out_branch_id);

/* Determine whether `branch_id` is a recognised consensus branch id —
 * i.e. appears in NetworkUpgradeInfo. On success writes the answer to
 * *out_known (true if recognised, false otherwise). This is a pure
 * lookup; an unrecognised branch id is NOT an error code — it's a
 * success with out_known=false. (Legacy LOG_FAIL chatter is reproduced
 * by the wrapper.)
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT
 */
struct zcl_result domain_consensus_is_branch_id(
        uint32_t branch_id,
        bool *out_known);

/* Determine whether `n_height` is exactly the activation height for
 * upgrade `idx`. Mirrors the legacy contract:
 *   - Returns out_match=false if n_height < 0.
 *   - BASE_SPROUT has no activation height; returns out_match=false.
 *   - Otherwise returns true iff n_height == params->vUpgrades[idx].
 *     nActivationHeight.
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_BAD_IDX
 */
struct zcl_result domain_consensus_is_activation_height(
        int n_height,
        const struct consensus_params *params,
        enum upgrade_index idx,
        bool *out_match);

/* Determine whether `n_height` is the activation height for ANY
 * upgrade after BASE_SPROUT. out_match=false if no upgrade activates
 * at this height.
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT
 */
struct zcl_result domain_consensus_is_activation_height_any(
        int n_height,
        const struct consensus_params *params,
        bool *out_match);

/* Find the smallest upgrade_index > BASE_SPROUT whose state at
 * n_height is UPGRADE_PENDING. On success returns ZCL_OK and writes
 * the index to *out_epoch.
 *
 * If no upgrade is pending (i.e. all upgrades are either DISABLED or
 * already ACTIVE), returns DOMAIN_CONSENSUS_UPGRADES_ERR_NO_PENDING.
 * This mirrors the legacy LOG_ERR("no pending upgrade") contract.
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NO_PENDING
 */
struct zcl_result domain_consensus_next_epoch(
        int n_height,
        const struct consensus_params *params,
        int *out_epoch);

/* Find the activation height of the next pending upgrade after
 * n_height. On success returns ZCL_OK and writes the height to
 * *out_activation.
 *
 * Error codes:
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT
 *   DOMAIN_CONSENSUS_UPGRADES_ERR_NO_PENDING
 */
struct zcl_result domain_consensus_next_activation_height(
        int n_height,
        const struct consensus_params *params,
        int *out_activation);

/* Error codes used by domain/consensus/upgrades.{c,h}. Stable across
 * builds; new codes are appended. Returned via zcl_result.code. */
enum domain_consensus_upgrades_err {
    DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_PARAMS = 1301,
    DOMAIN_CONSENSUS_UPGRADES_ERR_NULL_OUT    = 1302,
    DOMAIN_CONSENSUS_UPGRADES_ERR_NEG_HEIGHT  = 1303,
    DOMAIN_CONSENSUS_UPGRADES_ERR_BAD_IDX     = 1304,
    DOMAIN_CONSENSUS_UPGRADES_ERR_NO_PENDING  = 1305,
};

#endif /* ZCL_DOMAIN_CONSENSUS_UPGRADES_H */
