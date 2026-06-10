/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CONTEXTUAL_CHECK_TX_H
#define ZCL_CONTEXTUAL_CHECK_TX_H

#include "consensus/params.h"
#include "consensus/validation.h"
#include "domain/consensus/locktime.h"
#include "primitives/transaction.h"
#include <stdbool.h>

/* The pure lock-time predicates is_final_tx / is_expired_tx /
 * is_expiring_soon_tx have moved to domain/consensus/locktime.{h,c}.
 * These inline wrappers preserve the exact legacy signatures used by
 * check_block.c, txmempool.c, and the BIP113/BIP65 test corpus.
 *
 * The wrappers stay thin and inline: callers pass already-computed
 * scalars (block height, MTP cutoff) — the impure work of computing
 * those scalars from chain state lives in the wrapper's caller, not
 * here. See domain/consensus/locktime.h for the contract. */

/* TX_EXPIRING_SOON_THRESHOLD is also defined (identically) in
 * lib/validation/include/validation/main_constants.h. That definition
 * is canonical at the lib/ layer. The domain layer mirrors the same
 * literal under DOMAIN_CONSENSUS_TX_EXPIRING_SOON_THRESHOLD, and a
 * compile-time assert here pins them together so they cannot drift. */
#define LOCKTIME_THRESHOLD_TX DOMAIN_CONSENSUS_LOCKTIME_THRESHOLD

_Static_assert(DOMAIN_CONSENSUS_TX_EXPIRING_SOON_THRESHOLD == 3,
               "domain expiring-soon threshold drift");

static inline bool is_expired_tx(const struct transaction *tx, int nHeight)
{
    return domain_consensus_tx_is_expired(tx, nHeight);
}

static inline bool is_expiring_soon_tx(const struct transaction *tx,
                                       int nNextBlockHeight)
{
    return domain_consensus_tx_is_expiring_soon(tx, nNextBlockHeight);
}

static inline bool is_final_tx(const struct transaction *tx,
                                int nBlockHeight, int64_t nBlockTime)
{
    return domain_consensus_tx_is_final(tx, nBlockHeight, nBlockTime);
}

bool contextual_check_transaction(const struct transaction *tx,
                                   struct validation_state *state,
                                   const struct consensus_params *params,
                                   int nHeight,
                                   int dosLevel);

/* Skip Groth16 proof verification for blocks at or below this height.
 * Set via -deferproofvalidationbelow=<hash>. Default: latest checkpoint height.
 * Value of -1 disables (verify everything).
 * Atomic: read from validation threads, written by bg_validation + boot. */
extern _Atomic int g_deferred_proof_validation_below_height;

#endif
