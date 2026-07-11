/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "domain/consensus/verify.h"

#include "consensus/params.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"

struct zcl_result domain_consensus_verify_pow_solution(
        const struct uint256 *block_hash,
        uint32_t n_bits,
        const struct consensus_params *params)
{
    if (!block_hash || !params)
        return ZCL_ERR(DOMAIN_CONSENSUS_ERR_NULL_ARG,
                       "verify_pow_solution: null argument");

    bool fNegative = false;
    bool fOverflow = false;
    struct arith_uint256 target;
    arith_uint256_set_compact(&target, n_bits, &fNegative, &fOverflow);

    if (fNegative || fOverflow || arith_uint256_is_zero(&target))
        return ZCL_ERR(DOMAIN_CONSENSUS_ERR_POW_TARGET_INVALID,
                       "verify_pow_solution: nBits=0x%08x malformed", n_bits);

    /* nBits floor — the difficulty cannot demand less work than the
     * powLimit configured for the chain. */
    struct arith_uint256 pow_limit;
    uint256_to_arith(&pow_limit, &params->powLimit);
    if (arith_uint256_compare(&target, &pow_limit) > 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_ERR_POW_TARGET_BELOW_MIN,
                       "verify_pow_solution: nBits=0x%08x below minimum work",
                       n_bits);

    /* hash <= target check (PoW). */
    struct arith_uint256 hash_arith;
    uint256_to_arith(&hash_arith, block_hash);
    if (arith_uint256_compare(&hash_arith, &target) > 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_ERR_POW_HASH_ABOVE_TARGET,
                       "verify_pow_solution: hash exceeds target (nBits=0x%08x)",
                       n_bits);

    return ZCL_OK;
}
