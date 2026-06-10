/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CHAIN_H
#define ZCL_CHAIN_H

#include "core/arith_uint256.h"
#include "primitives/block.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SPROUT_VALUE_VERSION 1001400
#define SAPLING_VALUE_VERSION 1010100

enum block_status {
    BLOCK_VALID_UNKNOWN      = 0,
    BLOCK_VALID_HEADER       = 1,
    BLOCK_VALID_TREE         = 2,
    BLOCK_VALID_TRANSACTIONS = 3,
    BLOCK_VALID_CHAIN        = 4,
    BLOCK_VALID_SCRIPTS      = 5,
    BLOCK_VALID_MASK         = 7,
    BLOCK_HAVE_DATA          = 8,
    BLOCK_HAVE_UNDO          = 16,
    BLOCK_HAVE_MASK          = 24,
    /* Three-class typed failure model (Round 6 C4).
     *
     *   VALID  = PERMANENT consensus reject. Bad PoW, bad signature,
     *            failed proof, malformed block. Only the consensus-scope
     *            retry path may legitimately clear this. See
     *            process_block_core.c:973 (rate-limited near-tip retry).
     *
     *   CHILD  = DEPENDENCY failure propagated from a failed ancestor.
     *            Self-clears once the ancestor is cleared (no separate
     *            evidence required).
     *
     *   TRANSIENT = recoverable resource/timing failure. Missing UTXO
     *            row a self-heal can repair, DB writer busy, I/O retry
     *            budget. Distinct from VALID so the eventual retry path
     *            never re-runs full consensus on a known-good block.
     *            Reserved here; no SET site yet — see Round 7 plan.
     *
     * BLOCK_FAILED_MASK (= VALID | CHILD = 96) is preserved for
     * backward compatibility with persisted nStatus on disk; new code
     * should prefer block_has_any_failure() / the typed predicates
     * below so the eventual TRANSIENT introduction is automatic. */
    BLOCK_FAILED_VALID       = 32,
    BLOCK_FAILED_CHILD       = 64,
    BLOCK_FAILED_MASK        = 96,
    BLOCK_ACTIVATES_UPGRADE  = 128,
    BLOCK_PARKED_FLAG        = 256,
    BLOCK_PARKED_PARENT_FLAG = 512,
    BLOCK_PARKED_MASK        = 768,
    BLOCK_FAILED_TRANSIENT   = 1024,
    BLOCK_FAILED_ANY_MASK    = 1120, /* VALID | CHILD | TRANSIENT */
};

#define BLOCK_VALID_CONSENSUS BLOCK_VALID_SCRIPTS

struct disk_block_pos {
    int nFile;
    unsigned int nPos;
};

static inline void disk_block_pos_init(struct disk_block_pos *p)
{
    p->nFile = -1;
    p->nPos = 0;
}

#define OPTIONAL_NONE (-1)

struct block_index {
    /* --- 8-byte aligned pointers first --- */
    const struct uint256 *phashBlock;
    struct block_index *pprev;
    struct block_index *pskip;

    /* --- 32-byte fields --- */
    struct uint256 hashBlock;  /* Stable storage for phashBlock */
    struct arith_uint256 nChainWork;
    struct uint256 hashMerkleRoot;
    struct uint256 hashFinalSaplingRoot;
    struct uint256 nNonce;

    /* --- 8-byte fields --- */
    int64_t nCachedBranchId;
    int64_t nSproutValue;
    int64_t nChainSproutValue;
    int64_t nSaplingValue;
    int64_t nChainSaplingValue;
    uint64_t nTimeReceived;
    unsigned char *nSolution;      /* heap-allocated, NULL if not loaded */
    size_t nSolutionSize;

    /* --- 4-byte fields --- */
    int nHeight;
    int nFile;
    unsigned int nDataPos;
    unsigned int nUndoPos;
    unsigned int nTx;
    unsigned int nChainTx;
    unsigned int nStatus;
    int32_t nVersion;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nSequenceId;

    /* --- 1-byte fields (packed together to avoid padding) --- */
    bool has_sprout_value;
    bool has_chain_sprout_value;
    bool has_chain_sapling_value;
    /* 5 bytes padding to 8-byte boundary — struct ends here */
};

static inline void block_index_init(struct block_index *bi)
{
    memset(bi, 0, sizeof(*bi));
    bi->nCachedBranchId = OPTIONAL_NONE;
    arith_uint256_set_zero(&bi->nChainWork);
}

static inline int64_t block_index_get_time(const struct block_index *bi)
{
    return (int64_t)bi->nTime;
}

#define MEDIAN_TIME_SPAN 11

static inline int64_t block_index_get_median_time_past(const struct block_index *bi)
{
    int64_t pmedian[MEDIAN_TIME_SPAN];
    int count = 0;
    const struct block_index *p = bi;
    for (int i = 0; i < MEDIAN_TIME_SPAN && p; i++, p = p->pprev)
        pmedian[count++] = block_index_get_time(p);

    /* Simple insertion sort */
    for (int i = 1; i < count; i++) {
        int64_t key = pmedian[i];
        int j = i - 1;
        while (j >= 0 && pmedian[j] > key) {
            pmedian[j + 1] = pmedian[j];
            j--;
        }
        pmedian[j + 1] = key;
    }
    return pmedian[count / 2];
}

/* Typed failure classification (Round 6 C4). Prefer these over raw
 * nStatus bit tests so the future TRANSIENT retry policy lands at one
 * site. */
static inline bool block_is_permanently_failed(const struct block_index *bi)
{
    return bi && (bi->nStatus & BLOCK_FAILED_VALID) != 0;
}
static inline bool block_is_dependency_failed(const struct block_index *bi)
{
    return bi && (bi->nStatus & BLOCK_FAILED_CHILD) != 0;
}
static inline bool block_is_transiently_failed(const struct block_index *bi)
{
    return bi && (bi->nStatus & BLOCK_FAILED_TRANSIENT) != 0;
}
static inline bool block_has_any_failure(const struct block_index *bi)
{
    return bi && (bi->nStatus & BLOCK_FAILED_ANY_MASK) != 0;
}

static inline bool block_index_is_valid(const struct block_index *bi,
                                        enum block_status up_to)
{
    if (block_has_any_failure(bi))
        return false;
    return (bi->nStatus & BLOCK_VALID_MASK) >= (unsigned int)up_to;
}

struct block_index *block_index_get_ancestor(struct block_index *bi, int height);
void block_index_build_skip(struct block_index *bi);

/* qsort comparator: sort block_index pointers by height ascending */
static inline int block_index_cmp_height(const void *a, const void *b)
{
    const struct block_index *ba = *(const struct block_index *const *)a;
    const struct block_index *bb = *(const struct block_index *const *)b;
    return (ba->nHeight > bb->nHeight) - (ba->nHeight < bb->nHeight);
}

#endif
