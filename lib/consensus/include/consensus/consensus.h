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
 * ("a little extra"). The prior 2000000U here was an accidental mirror of
 * MAX_BLOCK_SIZE and let c23 accept ~20x-oversize txs that zclassicd rejects
 * (bad-txns-oversize, DoS 100) — a forward consensus fork. No post-Sapling
 * tx on the mainnet chain exceeds 102000, so tightening this re-validates
 * all history byte-identically. */
#define MAX_TX_SIZE_AFTER_SAPLING  102000U
#define COINBASE_MATURITY          100
#define TX_EXPIRY_HEIGHT_THRESHOLD 500000000U

enum {
    LOCKTIME_MEDIAN_TIME_PAST = (1 << 1),
};

#define STANDARD_LOCKTIME_VERIFY_FLAGS LOCKTIME_MEDIAN_TIME_PAST

#endif
