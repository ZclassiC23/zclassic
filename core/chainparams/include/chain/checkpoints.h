/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CHECKPOINTS_H
#define ZCL_CHECKPOINTS_H

#include "chain/chain.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>

struct checkpoint_entry {
    int height;
    struct uint256 hash;
};

struct checkpoint_data {
    const struct checkpoint_entry *entries;
    int nEntries;
    int64_t nTimeLastCheckpoint;
    int64_t nTransactionsLastCheckpoint;
    double fTransactionsPerDay;
};

/* Estimated total block count for the chain, taken as the height of the
 * last (highest) checkpoint entry. Returns 0 if `data` is NULL or has no
 * entries. This is a lower-bound progress denominator, not a consensus
 * value. Source: src/checkpoints.c ->
 * domain/consensus/src/checkpoints.c
 * (domain_consensus_checkpoints_total_blocks_estimate). */
int checkpoints_get_total_blocks_estimate(const struct checkpoint_data *data);

/* Fraction of chain verification work completed at `pindex`, in [0.0,
 * 1.0], for UI/sync-progress display (not consensus). Models work as
 * cheap pre-last-checkpoint transactions plus expensive post-checkpoint
 * ones, extrapolating the remaining tail from `fTransactionsPerDay` and
 * the wall-clock gap to now; `fSigchecks` weights signature-checking
 * work 5x. Reads `pindex->nChainTx` and the block time, plus a single
 * wall-clock read held in this wrapper.
 *
 * Contract: a NULL `pindex` returns 0.0 (handled here, before the clock
 * read); a degenerate/zero denominator also returns 0.0 rather than
 * NaN/inf. Source: src/checkpoints.c ->
 * domain/consensus/src/checkpoints.c
 * (domain_consensus_checkpoints_progress_at_now). */
double checkpoints_guess_verification_progress(
    const struct checkpoint_data *data,
    const struct block_index *pindex, bool fSigchecks);

/* ── Enforcement helpers ────────────────────────────────────
 *
 * These wrap the exact-height checkpoint policy in a testable
 * API. Callers in `contextual_check_block_header` use
 * `checkpoints_validate_header()`; other code (RPC, tests)
 * can use the lower-level lookups.
 */

/* Returns true and writes `*out_hash` if a checkpoint exists
 * at `height`. Returns false if there is no checkpoint at that
 * height. O(nEntries) linear scan — the list is tiny (single
 * digits). */
bool checkpoints_hash_at_height(const struct checkpoint_data *data,
                                 int height,
                                 struct uint256 *out_hash);

/* Returns the highest checkpoint height, or -1 if there are
 * no checkpoints. Used by the "IsInitialBlockDownload" and
 * "deep reorg refusal" predicates. */
int checkpoints_last_height(const struct checkpoint_data *data);

/* Header-validation entry point. Returns true if (height, hash)
 * is consistent with the checkpoint data:
 *   - if no checkpoint exists at `height` → true (nothing to
 *     check)
 *   - if a checkpoint exists and `hash` matches → true
 *   - otherwise → false (fork attempt)
 *
 * Callers are free to ignore this when `fCheckpointsEnabled`
 * is false (e.g. in test fixtures or explicit re-scan modes). */
bool checkpoints_validate_header(const struct checkpoint_data *data,
                                  int height,
                                  const struct uint256 *hash);

/* SHA3 UTXO checkpoint — compiled-in commitment that a new node
 * can verify its UTXO set against without trusting any peer.
 * Verified bit-for-bit against zclassicd reference implementation. */
struct sha3_utxo_checkpoint {
    int32_t  height;            /* block height */
    uint8_t  block_hash[32];    /* block hash at height (hex in source) */
    uint8_t  sha3_hash[32];     /* SHA3-256 over canonical UTXO set */
    uint64_t utxo_count;        /* number of UTXOs */
    int64_t  total_supply;      /* total transparent supply in zatoshi */
};

/* Returns the latest hardcoded SHA3 UTXO checkpoint, or NULL if none.
 * In production this always returns the compiled-in g_sha3_checkpoint. */
const struct sha3_utxo_checkpoint *get_sha3_utxo_checkpoint(void);

/* Test-only seam: install a checkpoint that get_sha3_utxo_checkpoint()
 * returns instead of the compiled-in one. Used by the snapshot-bind
 * tests to assert the anchor-root gate at a scaled-down fixture height
 * whose locally-computed commitment IS the override's sha3_hash by
 * construction (the same utxo_commitment path that derived the real
 * checkpoint). NULL = no override = production behavior. The pointer is
 * borrowed (not copied); the caller keeps it valid until reset. */
void checkpoints_set_sha3_override_for_test(const struct sha3_utxo_checkpoint *cp);
void checkpoints_reset_sha3_override_for_test(void);

#endif
