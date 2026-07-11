/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_EQUIHASH_H
#define ZCL_EQUIHASH_H

#include "chain/chainparams.h"
#include "primitives/block.h"
#include <stdbool.h>

/* Consensus Equihash check: returns true IFF `header`'s `nSolution`
 * (length `nSolutionSize`) is a valid Equihash solution for the
 * BLAKE2b("ZcashPoW") personalised challenge derived from the header's
 * pre-solution bytes (nVersion, hashPrevBlock, hashMerkleRoot,
 * hashFinalSaplingRoot, nTime, nBits, and the 32-byte nNonce, all in
 * canonical little-endian on-wire order).
 *
 * The (N,K) parameters are selected from `nSolutionSize` alone
 * (e.g. 200/9); an unrecognised solution size returns false. This
 * proves the miner found a valid Equihash collision binding the
 * solution to this exact header — it is the proof-of-work *solution*
 * half. It does NOT check the difficulty target (`hash <= target`);
 * that is CheckProofOfWork's job. Both must pass for a valid block.
 *
 * `params` is currently unused (reserved for future activation-gated
 * algorithm switches) and may be NULL — the legacy code never read it
 * here. A NULL `header` returns false. On any contract failure the
 * reason is logged and false is returned (never aborts).
 *
 * Source: src/equihash.c -> domain/consensus/src/equihash.c
 * (domain_consensus_verify_equihash_solution). */
bool check_equihash_solution(const struct block_header *header,
                             const struct chain_params *params);

#endif
