/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Epoch-I thin wrapper. The pure block-reward arithmetic lives in
 * domain/consensus/subsidy.{h,c}; this file preserves the legacy
 * bool-shaped int64_t-returning signature so existing callers stay
 * unchanged while the domain function returns a typed zcl_result. */

#include "chain/subsidy.h"
#include "domain/consensus/subsidy.h"
#include <stdio.h>

int64_t get_block_subsidy(int nHeight, const struct consensus_params *params)
{
    int64_t subsidy = 0;
    struct zcl_result r = domain_consensus_block_subsidy(nHeight, params, &subsidy);
    if (!r.ok) {
        /* Preserve legacy behaviour: log on the same conditions the
         * pre-extraction code logged on (null params, negative height)
         * and fail-safe to 0 instead of crashing. The domain function
         * already populated zcl_result with a precise reason. */
        fprintf(stderr,  // obs-ok:legacy-wrapper-preserves-pre-extraction-stderr
                "FATAL: get_block_subsidy failed at height %d: %s\n",
                nHeight,
                r.message[0] ? r.message : "(no message)");
        return 0;
    }
    return subsidy;
}
