/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_frontier — the L0 authority: compute H* (the deepest
 * provably-consistent height) and served_floor from durable progress.kv
 * state. Read-only; no writes, no transactions of its own.
 *
 * H* is the single number every reconciliation gates on. It is computed
 * once at boot/condition-check time from the durable stage logs and the
 * coins-applied frontier — never read from a drifted in-RAM int or a
 * served-tip that can advance past a torn prefix. L1 (flag/cursor reset)
 * and L2 (coin rewind) both clamp to this value rather than re-deriving
 * a private frontier, so all three layers agree on one boundary.
 *
 * The contract:
 *   - [0, H*] is a provably-consistent prefix: every success-checked log
 *     shows a contiguous ok=1 run up to H*, the validate_headers and
 *     script_validate hashes agree where both are present, and the coins
 *     frontier does not contradict it.
 *   - [H*+1, ...] has SOME defect (a hole, an ok=0 row, or a hash split).
 *   - H* >= TRUSTED_ANCHOR (the SHA3 UTXO checkpoint height) ALWAYS — the
 *     algorithm never rewinds across the irreversible finality floor.
 *   - served_floor = MAX(tip_finalize_log.height WHERE ok=1), reported
 *     SEPARATELY so L1 can HOLD a tip finalized above H* (a T10 torn
 *     view) rather than re-serving unstable blocks.
 */

#ifndef ZCL_JOBS_REDUCER_FRONTIER_H
#define ZCL_JOBS_REDUCER_FRONTIER_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

/* The compiled-in SHA3 UTXO checkpoint height — the irreversible floor H*
 * may never fall below. Mirrors get_sha3_utxo_checkpoint()->height; exposed
 * as a named constant so callers and tests can assert the clamp without
 * pulling in lib/chain. Verified equal to the live checkpoint by the
 * regression test. */
#define REDUCER_FRONTIER_TRUSTED_ANCHOR ((int32_t)3056758)

/* Compute H* (deepest provably-consistent height) and served_floor from
 * durable state.
 *
 * Called under progress_store_tx_lock with read-only access to progress_db
 * (progress.kv: stage_cursor, progress_meta, *_log tables) and the coins
 * store (whose applied-frontier == progress_meta['coins_applied_height']).
 *
 * Returns false on a DB read error; true on success (both out params are
 * always set on success). Sets *hstar to >= REDUCER_FRONTIER_TRUSTED_ANCHOR
 * (never below the SHA3 checkpoint). Sets *served_floor to
 * MAX(tip_finalize_log.height WHERE ok=1), or 0 if no ok=1 rows exist.
 *
 * PURE SELECT-only — issues no INSERT/UPDATE/DELETE and opens no
 * transaction of its own. The caller MUST already hold
 * progress_store_tx_lock() so the durable snapshot is consistent. */
bool reducer_frontier_compute_hstar(
    sqlite3 *progress_db,           /* progress.kv handle (lock held by caller) */
    int32_t *hstar,                 /* OUT: deepest provably-consistent height */
    int32_t *served_floor           /* OUT: MAX(tip_finalize ok=1 height), or 0 */
);

#endif /* ZCL_JOBS_REDUCER_FRONTIER_H */
