/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CONSENSUS_CONSENSUS_H
#define BITCOIN_CONSENSUS_CONSENSUS_H

#include <stdint.h>

#define MIN_BLOCK_VERSION          4
#define SPROUT_MIN_TX_VERSION      1
#define OVERWINTER_MIN_TX_VERSION  3
#define OVERWINTER_MAX_TX_VERSION  3
#define SAPLING_MIN_TX_VERSION     4
#define SAPLING_MAX_TX_VERSION     4
#ifndef MAX_BLOCK_SIZE  /* also defined (same value) in primitives/block.h + validation/main_constants.h */
#define MAX_BLOCK_SIZE             2000000U
#endif
#define MAX_BLOCK_SIGOPS           20000U
#define MAX_TX_SIZE_BEFORE_SAPLING 100000U
/* zclassicd src/consensus/consensus.h:27: MAX_TX_SIZE_AFTER_SAPLING = 102000
 * ("a little extra"), enforced unconditionally in its CheckTransaction
 * (main.cpp:1196-1200). The prior 2000000U here was an accidental mirror of
 * MAX_BLOCK_SIZE and let c23 accept ~20x-oversize txs that zclassicd rejects
 * (bad-txns-oversize, DoS 100) — a forward consensus fork.
 *
 * HOWEVER (proven 2026-06-11): the canonical mainnet chain contains 413
 * post-Sapling txs ABOVE 102000 (heights 478544..1968856, max 1922197 —
 * legal at mine time when the effective cap was MAX_BLOCK_SIZE; zclassicd
 * tightened the constant later without grandfathering, so its own text
 * false-rejects block 478544 on a from-genesis replay). Running zclassicd
 * nodes accept that history only because already-validated blocks are never
 * re-checked, and enforce 102000 on every NEW block. zclassic23 matches the
 * LIVE behavior: those exact txs are excused via the empirical {txid,size}
 * allowlist in domain/consensus/src/tx_structural.c (BLOCK context only;
 * mempool/relay stays strict). The constant stays 102000 — what the running
 * network enforces on all new blocks. */
#define MAX_TX_SIZE_AFTER_SAPLING  102000U
#define COINBASE_MATURITY          100
#define TX_EXPIRY_HEIGHT_THRESHOLD 500000000U

enum {
    LOCKTIME_MEDIAN_TIME_PAST = (1 << 1),
};

#define STANDARD_LOCKTIME_VERIFY_FLAGS LOCKTIME_MEDIAN_TIME_PAST

#endif
