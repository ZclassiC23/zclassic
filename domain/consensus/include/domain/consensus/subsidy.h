/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * domain/consensus/subsidy.h — pure block reward arithmetic.
 *
 * The block subsidy is a pure function of (height, consensus_params).
 * It is the canonical schedule of new coins entering circulation:
 *
 *   - Slow-start ramp-up over the first nSubsidySlowStartInterval blocks
 *   - 12.5 ZCL full reward after slow-start
 *   - Halving every nPreButtercupSubsidyHalvingInterval blocks (Bitcoin-style)
 *   - Post-Buttercup: faster halvings at a Ratio-times-shorter interval,
 *     with each emitted block scaled down by BUTTERCUP_POW_TARGET_SPACING_RATIO
 *   - Zero after 64 halvings (the int64_t cap)
 *
 * No clock, no RNG, no I/O. Replays from inputs alone.
 *
 * Layering: domain/consensus/ may #include from util/, core/, chain/,
 * consensus/, crypto/, sapling/, script/, primitives/. The fact this
 * function depends only on consensus/params.h and core/amount.h is
 * what makes it eligible to live here.
 */

#ifndef ZCL_DOMAIN_CONSENSUS_SUBSIDY_H
#define ZCL_DOMAIN_CONSENSUS_SUBSIDY_H

#include <stdint.h>

#include "util/result.h"

struct consensus_params;

/* Compute the block subsidy (new coins) at the given height under the
 * supplied consensus parameters. Pure: no side effects.
 *
 * On success returns ZCL_OK and writes the subsidy in satoshis to
 * *out_subsidy. On failure returns one of:
 *   DOMAIN_CONSENSUS_SUBSIDY_ERR_NULL_PARAMS  params == NULL
 *   DOMAIN_CONSENSUS_SUBSIDY_ERR_NULL_OUT     out_subsidy == NULL
 *   DOMAIN_CONSENSUS_SUBSIDY_ERR_NEG_HEIGHT   n_height < 0
 *
 * The subsidy value 0 is a legal success result (post-64-halvings).
 */
struct zcl_result domain_consensus_block_subsidy(
        int n_height,
        const struct consensus_params *params,
        int64_t *out_subsidy);

/* Error codes used by domain/consensus/subsidy.{c,h}. Stable across
 * builds; new codes are appended. Returned via zcl_result.code. */
enum domain_consensus_subsidy_err {
    DOMAIN_CONSENSUS_SUBSIDY_ERR_NULL_PARAMS = 1101,
    DOMAIN_CONSENSUS_SUBSIDY_ERR_NULL_OUT    = 1102,
    DOMAIN_CONSENSUS_SUBSIDY_ERR_NEG_HEIGHT  = 1103,
};

#endif /* ZCL_DOMAIN_CONSENSUS_SUBSIDY_H */
