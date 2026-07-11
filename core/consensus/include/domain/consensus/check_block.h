/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * domain/consensus/check_block.h — pure block STRUCTURAL checks.
 *
 * The "structural" checks are the consensus invariants that depend only
 * on the block's own bytes — no UTXO lookup, no signature verification,
 * no Proof-of-Work check, no clock. They are the ones that can be
 * decided by reading the block alone:
 *
 *   1. txns-count in [1 .. GENEROUS_BLOCK_SIZE_LIMIT]   → "bad-blk-length"
 *   2. vtx[0] is a coinbase                             → "bad-cb-missing"
 *   3. vtx[i] for i>=1 is NOT coinbase                  → "bad-cb-multiple"
 *   4. computed merkle root matches header              → "bad-txnmrklroot"
 *   5. txid list is not mutated (duplicated subtree)    → "bad-txns-duplicate"
 *   6. total legacy sigop count <= MAX_BLOCK_SIGOPS     → "bad-blk-sigops"
 *
 * The reject reason strings above are byte-identical to those produced
 * by the legacy check_block_impl() wrapper and are emitted on the
 * P2P-visible REJECT path. They MUST NOT change.
 *
 * What is NOT here (intentionally):
 *   - PoW / Equihash       — domain/consensus/verify.h (already extracted)
 *   - Per-tx CheckTransaction — domain/consensus/tx_structural (parallel)
 *   - Timestamp / 2h-future — depends on clock; lives in the wrapper
 *   - Header-version check  — block-header-only, stays in the wrapper
 *
 * Purity contract:
 *   - No clock, no RNG, no I/O, no global state read or write.
 *   - Internal transient malloc/free (e.g. the merkle work buffer) is
 *     OK; nothing leaks across the call boundary.
 *   - Same inputs → same outputs, every call.
 *
 * Layering: domain/consensus/ may #include from util/, core/, chain/,
 * consensus/, crypto/, sapling/, script/, primitives/, validation/
 * (sigops only — get_legacy_sig_op_count is a pure tx-shape function),
 * and bloom/merkle.h (pure merkle math).
 */

#ifndef ZCL_DOMAIN_CONSENSUS_CHECK_BLOCK_H
#define ZCL_DOMAIN_CONSENSUS_CHECK_BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/result.h"

struct block;

/* Maximum length in bytes (including NUL) of the consensus
 * reject_reason token written back to the caller. Matches
 * `MAX_REJECT_REASON` in consensus/validation.h. Keeping a local
 * constant here avoids dragging that header into the domain include
 * graph for what is otherwise a pure size constant. */
#define DOMAIN_CHECK_BLOCK_REASON_MAX 256

/* Check the merkle-root invariant on `block`. Computes the merkle root
 * from the in-block txid list and compares it to header.hashMerkleRoot;
 * also detects "duplicate-subtree" mutation (CVE-2012-2459).
 *
 * Mirrors the `check_merkle_root` branch of legacy check_block_impl().
 *
 * Outputs:
 *   out_reject_reason — optional. On rejection, written with the
 *     byte-identical legacy reason token ("bad-txnmrklroot" or
 *     "bad-txns-duplicate") NUL-terminated.
 *   out_dos — optional. On rejection, written with the legacy DoS
 *     score (always 100 for these rejections).
 *
 * Pure: no I/O, no global state, no clock. Internal transient malloc
 * for the txid scratch buffer is freed before return.
 */
struct zcl_result domain_consensus_check_block_merkle_root(
        const struct block *block,
        char  *out_reject_reason,
        size_t out_reject_reason_size,
        int   *out_dos);

/* Check the txn-count bounds and coinbase placement invariants on
 * `block`. Mirrors the FIRST three checks of the `check_size_limits`
 * branch of legacy check_block_impl(); the per-tx check_transaction()
 * call (NOT structural at the block level) and the sigops total
 * (checked AFTER the per-tx loop in the legacy code, see
 * domain_consensus_check_block_sigops below) are kept separate so the
 * wrapper preserves the exact legacy rejection ordering.
 *
 * Invariants enforced here:
 *   - num_vtx in [1 .. GENEROUS_BLOCK_SIZE_LIMIT (= 2_000_000)]
 *   - vtx[0] is a coinbase
 *   - vtx[i>=1] is NOT a coinbase
 *
 * Outputs:
 *   out_reject_reason — optional. On rejection, one of:
 *     "bad-blk-length", "bad-cb-missing", "bad-cb-multiple"
 *     (byte-identical to legacy).
 *   out_dos — optional. Always 100 on rejection.
 *
 * Pure: no I/O, no global state, no clock, no allocation.
 */
struct zcl_result domain_consensus_check_block_size_and_coinbase(
        const struct block *block,
        char  *out_reject_reason,
        size_t out_reject_reason_size,
        int   *out_dos);

/* Check that the sum of legacy sigops across every transaction in
 * `block` does not exceed MAX_BLOCK_SIGOPS (= 20_000). Mirrors the
 * sigops check that legacy check_block_impl() runs AFTER the per-tx
 * check_transaction() loop.
 *
 * Outputs:
 *   out_reject_reason — optional. On rejection, the byte-identical
 *     legacy token "bad-blk-sigops".
 *   out_dos — optional. Always 100 on rejection.
 *
 * Pure: no I/O, no global state, no clock, no allocation.
 */
struct zcl_result domain_consensus_check_block_sigops(
        const struct block *block,
        char  *out_reject_reason,
        size_t out_reject_reason_size,
        int   *out_dos);

/* Error codes used by domain/consensus/check_block.{c,h}. Stable
 * across builds; new codes are appended. Returned via zcl_result.code.
 *
 * The numeric range 1201-1299 is reserved for this module so that
 * different domain modules cannot collide on a code value. */
enum domain_consensus_check_block_err {
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_NULL_ARG          = 1201,
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_BAD_BLK_LENGTH    = 1202,
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_BAD_CB_MISSING    = 1203,
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_BAD_CB_MULTIPLE   = 1204,
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_BAD_TXNMRKLROOT   = 1205,
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_BAD_TXNS_DUP      = 1206,
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_BAD_BLK_SIGOPS    = 1207,
    DOMAIN_CONSENSUS_CHECK_BLOCK_ERR_OUT_OF_MEMORY     = 1208,
};

#endif /* ZCL_DOMAIN_CONSENSUS_CHECK_BLOCK_H */
