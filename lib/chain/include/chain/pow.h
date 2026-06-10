/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_POW_H
#define ZCL_POW_H

#include "core/arith_uint256.h"
#include "chain/chain.h"
#include "consensus/params.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>

/* Scale a compact difficulty target by dividing its 256-bit target by
 * `multiplier` (a larger multiplier => smaller target => harder PoW),
 * clamping the result so it can never drop below `params->powLimit`.
 * Returns the new compact nBits.
 *
 * Fail-safe contract (matches legacy zclassicd): on any contract
 * violation in the pure arithmetic (NULL params, multiplier == 0) the
 * input `nBits` is returned UNCHANGED and the reason is logged — the
 * function never aborts or returns a garbage target. Used by
 * GetNextWorkRequired's min-difficulty fork-window ramp.
 *
 * Source: src/pow.c -> domain/consensus/src/pow.c
 * (domain_consensus_increase_difficulty_by). */
unsigned int IncreaseDifficultyBy(unsigned int nBits, int64_t multiplier,
                                  const struct consensus_params *params);

/* Compute the consensus-required compact difficulty (nBits) for the
 * block that would follow `pindexLast`. This is the difficulty rule;
 * the value it returns is the target every honest node must agree on
 * for height `pindexLast->nHeight + 1`.
 *
 * Algorithm (Zcash/ZClassic Digishield-style, per-block retarget over
 * an averaging window):
 *   - `pindexLast == NULL` (genesis case) -> powLimit as compact.
 *   - Inside a DIFFADJ/BUTTERCUP fork activation window with
 *     scaleDifficultyAtUpgradeFork set, apply the min-difficulty time
 *     ramp (>12x spacing -> powLimit; >6x -> /128; >2x -> /256).
 *   - If min-difficulty-blocks is enabled and the candidate is >6x the
 *     target spacing late, allow powLimit (testnet-style rule).
 *   - Otherwise: sum the targets of the last `nPowAveragingWindow`
 *     blocks, divide to get the average target, and hand off to
 *     CalculateNextWorkRequired with the median-time-past of the window
 *     endpoints.
 * A zero/negative averaging window short-circuits to powLimit (the
 * implementation guards against the division-by-zero).
 *
 * Pure function of the block-index chain + params (no clock/RNG/IO);
 * `pblock` is only read for its nTime in the fork-window ramp.
 *
 * Source: src/pow.c (GetNextWorkRequired). */
unsigned int GetNextWorkRequired(const struct block_index *pindexLast,
                                 const struct block_header *pblock,
                                 const struct consensus_params *params);

/* The retarget core: given the average target `bnAvg` over the
 * averaging window and the window's first/last block times, compute the
 * next compact nBits. The actual timespan is damped toward the ideal
 * window timespan (newTimespan = ideal + (actual - ideal)/4) and then
 * clamped to [min, max] actual-timespan bounds before scaling the
 * average target; the result is capped at powLimit so difficulty can
 * never fall below the chain minimum.
 *
 * Fail-safe contract: on NULL params/out the pure function fails and
 * this wrapper falls back to powLimit-as-compact (or 0 if params is
 * also NULL), logging the reason — never aborts. `nextHeight` selects
 * the (pre/post-BUTTERCUP) spacing and window constants.
 *
 * Source: src/pow.c -> domain/consensus/src/pow.c
 * (domain_consensus_calculate_next_work_required). */
unsigned int CalculateNextWorkRequired(struct arith_uint256 bnAvg,
                                       int64_t nLastBlockTime,
                                       int64_t nFirstBlockTime,
                                       const struct consensus_params *params,
                                       int nextHeight);

/* Consensus PoW check: returns true IFF the 256-bit value of `hash`
 * (the block's Equihash-committed PoW hash, interpreted little-endian)
 * is <= the target encoded by compact `nBits`, AND that target is
 * itself valid. A true return PROVES:
 *   1. `nBits` decodes to a positive, non-overflowing, non-zero target;
 *   2. the target is at least as hard as `params->powLimit` (nBits is
 *      not weaker than the chain minimum) — a too-easy target is
 *      rejected, not silently accepted;
 *   3. `hash <= target` (the work was actually done).
 * Any of the three failing returns false and logs the reason. This is
 * the difficulty half of PoW; Equihash solution validity is a separate
 * check (check_equihash_solution). Pure: no clock/RNG/IO.
 *
 * NOTE: `hash` must already be the block's PoW hash; this function does
 * not recompute it from the header.
 *
 * Source: src/pow.c -> domain/consensus/src/verify.c
 * (domain_consensus_verify_pow_solution). */
bool CheckProofOfWork(struct uint256 hash, unsigned int nBits,
                      const struct consensus_params *params);

/* Work contributed by one block = the expected number of hashes to beat
 * its target, computed as 2^256 / (target + 1) from `block->nBits`.
 * This is the per-block increment of the cumulative `nChainWork` used to
 * pick the most-work chain. Returns 0 for a NULL block or any
 * unusable/malformed target (negative, overflow, or zero) so the
 * chainwork accumulator keeps stepping rather than aborting — matches
 * legacy GetBlockProof.
 *
 * Source: src/pow.c -> domain/consensus/src/pow.c
 * (domain_consensus_block_proof). */
struct arith_uint256 GetBlockProof(const struct block_index *block);

/* Estimate the time, in seconds, equivalent to the chainwork difference
 * between `to` and `from`, at the difficulty of `tip`. Returns
 * (work_delta * target_spacing) / GetBlockProof(tip), signed: positive
 * when `to` has more work than `from`, negative otherwise. Saturates to
 * +/- INT64_MAX when the result exceeds 63 bits. Used for time-based
 * reorg/lag heuristics, not consensus. Reads nChainWork of `to`/`from`
 * and nHeight/nBits of `tip`.
 *
 * Source: src/pow.c (GetBlockProofEquivalentTime). */
int64_t GetBlockProofEquivalentTime(const struct block_index *to,
                                    const struct block_index *from,
                                    const struct block_index *tip,
                                    const struct consensus_params *params);

/* Human-readable ZClassic difficulty from compact nBits representation.
 *
 * ZClassic inherited Zcash's Equihash difficulty baseline: the reference
 * target mantissa is 0x07ffff, not Bitcoin's 0x00ffff.  Keeping this
 * centralized avoids RPC/explorer surfaces silently drifting from
 * legacy zclassicd for the same nBits value.
 */
static inline double difficulty_from_bits(uint32_t bits)
{
    if (bits == 0) return 1.0;
    int shift = (int)((bits >> 24) & 0xff) - 29;
    double diff = (double)0x0007ffff / (double)(bits & 0x00ffffff);
    while (shift < 0) { diff /= 256.0; shift++; }
    while (shift > 0) { diff *= 256.0; shift--; }
    return diff;
}

/* Difficulty for a block index, NULL-safe (returns 1.0 for a NULL/genesis
 * baseline). Single source for the RPC + explorer "difficulty" surfaces,
 * replacing the per-controller get_difficulty / explorer_get_difficulty
 * duplicates. */
static inline double difficulty_from_index(const struct block_index *bi)
{
    return bi ? difficulty_from_bits(bi->nBits) : 1.0;
}

#endif
