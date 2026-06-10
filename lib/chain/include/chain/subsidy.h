/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SUBSIDY_H
#define ZCL_SUBSIDY_H

#include "core/amount.h"
#include "consensus/params.h"
#include <stdint.h>

/* Consensus block reward (in zatoshi, 1 COIN = 1e8) for the coinbase at
 * `nHeight`. This is the maximum a coinbase may mint at that height; it
 * is part of consensus, so all nodes must agree on this value.
 *
 * Schedule (base reward = 12.5 COIN):
 *   - Slow-start ramp-up over the first `nSubsidySlowStartInterval`
 *     blocks. For heights below interval/2 the reward is
 *     (12.5 / interval) * nHeight; from interval/2 to interval it is
 *     (12.5 / interval) * (nHeight + 1). Height 0 therefore pays 0.
 *   - After slow-start: the standard halving schedule. `halvings` =
 *     consensus_halving(params, nHeight) (a pre/post-BUTTERCUP-aware
 *     count using the slow-start shift); the reward is base >> halvings.
 *     Once BUTTERCUP is active the base is first divided by
 *     BUTTERCUP_POW_TARGET_SPACING_RATIO (the faster block spacing keeps
 *     emission constant per unit time) before the shift.
 *   - At >= 64 halvings the reward underflows to 0.
 *
 * Fail-safe contract (matches legacy zclassicd): NULL params or a
 * negative `nHeight` is a contract violation — the reason is logged and
 * 0 is returned rather than crashing. Pure: no clock/RNG/IO.
 *
 * Source: src/subsidy.c -> domain/consensus/src/subsidy.c
 * (domain_consensus_block_subsidy); schedule helpers in
 * lib/consensus/src/params.c (consensus_halving). */
int64_t get_block_subsidy(int nHeight, const struct consensus_params *params);

#endif
