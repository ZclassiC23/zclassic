/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Private helpers for the reducer-frontier L1 repair translation units. */

#ifndef ZCL_JOBS_STAGE_REPAIR_REDUCER_FRONTIER_INTERNAL_H
#define ZCL_JOBS_STAGE_REPAIR_REDUCER_FRONTIER_INTERNAL_H

#include <stdbool.h>

struct block;
struct main_state;
struct sqlite3;
struct stage_reducer_frontier_reconcile_result;
struct uint256;

/* Hash-verified active-chain block read: resolves `height` on the ACTIVE
 * chain under cs_main (requires BLOCK_HAVE_DATA), reads the body from disk
 * and re-hashes it against the index entry. Returns false (logged) on any
 * mismatch or missing data. Production `coin_backfill_io.read_block` for
 * the frontier coin backfill (jobs/stage_repair_coin_backfill.h). */
bool stage_repair_read_active_block_checked(struct main_state *ms, int height,
                                            struct block *blk,
                                            struct uint256 *block_hash);

/* Classify a validate/script hash_split at `height` by HASHES ONLY — the
 * in-memory canonical active HEADER (active_chain_at(H)->phashBlock, no disk
 * read, no BLOCK_HAVE_DATA gate) vs the two stored verdicts
 * (validate_headers_log.hash, script_validate_log.block_hash). NEVER gates on
 * body-readability: routing must not stall on a not-yet-fetched body.
 *   RF_SPLIT_VALIDATE_SIDE — validate_headers disagrees with the active header
 *     AND script already matches it (the validate-cursor clamp owns it).
 *   RF_SPLIT_SCRIPT_SIDE   — script disagrees with the active header (or both
 *     verdicts disagree); the coins-rewinding dual replay owns it.
 *   RF_SPLIT_INDETERMINATE — active header unavailable / a verdict hash missing;
 *     default to the replay (never the validate clamp). On a DB read error
 *     *out_err is set true and the return value is INDETERMINATE.
 * Acquires its own locks; safe to call without holding the progress lock. */
enum rf_hash_split_side {
    RF_SPLIT_INDETERMINATE = 0,
    RF_SPLIT_SCRIPT_SIDE,
    RF_SPLIT_VALIDATE_SIDE,
};
enum rf_hash_split_side stage_repair_classify_hash_split(
    struct main_state *ms, struct sqlite3 *db, int height, bool *out_err);

bool stage_reducer_frontier_try_replay_repairs(
    struct sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out,
    bool *handled);

bool stage_reducer_frontier_force_stage_cursor_in_tx(
    struct sqlite3 *db,
    const char *stage_name,
    const char *label,
    int target);

bool stage_reducer_frontier_reconcile_refill_cursors(
    struct sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out);

bool stage_reducer_frontier_reconcile_validate_hash_split_cursor(
    struct sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out);

/* Pre-refusal clamp of the script_validate / proof_validate cursors
 * back to the lowest rowless (unapplied) hole at or above the coins-applied
 * floor. PRECONDITION: a coin-tear refusal must be pending
 * (out->refused_coin_tear set by the frontier snapshot) — the call site
 * MUST gate on it; this repair exists for the coin-tear pin only, and the
 * callee WARNs (unthrottled, contract violation) on a no-tear invocation.
 * Sets *handled=true (and clears refused_coin_tear in `out`) only when a
 * clamp was performed; on zero progress sets *handled=false so the caller
 * falls through to the next repair. Diagnostics are logged by the callee.
 * Returns false only on store errors. */
bool stage_reducer_frontier_try_unapplied_hole_clamp(
    struct sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out,
    bool *handled);

/* Purge hash-bearing stage-log rows in (hstar, min(sweep_top, tip)]
 * whose stored hash != the canonical active-chain block at their height
 * (relabel/reorg residue — e.g. false bad-cb-height verdicts left by a
 * height-relabel), plus the hashless downstream rows at those heights.
 * Dry-run (apply=false) only counts into out->noncanonical_*. Purged rows
 * become ordinary rowless holes for the existing refill + cursor machinery.
 * Genuine consensus rejects keep their rows (their hash IS canonical).
 * Returns false only on store errors.
 * Implemented in stage_repair_reducer_frontier_purge.c. */
bool stage_reducer_frontier_purge_noncanonical(
    struct sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out);

/* REPLACE (never delete — served_floor invariant) a stale ok=0
 * skip-status tip_finalize_log residue row (reorg_detected / utxo_count_
 * diverged) that pins H* below the coins frontier and so manufactures a
 * FALSE coin-tear refusal. A row at height h is eligible ONLY when it is
 * present with ok=0 AND h <= coins_applied_height - 1 (already covered by
 * coins, so no coin can tear) AND header_admit_log holds a row at h (the
 * verdict is re-evidenced upstream — h is a real admitted block). The
 * replacement is a fresh ok=1 'finalize_backfill' row carrying the lookahead
 * hash(h+1) sourced from header_admit_log (the row-H -> hash-H+1 finalized
 * convention, so finalized_row_active_match stays reorg-correct), written via
 * the production log_insert (INSERT OR REPLACE — the row persists, never
 * vanishes from served history). Touches ONLY tip_finalize ok=0 skip rows
 * meeting BOTH gates; never coins, never a cursor, never a row at/above the
 * coins frontier. Dry-run (apply=false) only counts into
 * out->reorg_residue_tipfin_*. The caller MUST re-read the frontier snapshot
 * after an apply that replaced a row (H* moves). Returns false only on store
 * errors. Implemented in stage_repair_reducer_frontier_purge.c. */
bool stage_reducer_frontier_purge_stale_reorg_tipfin(
    struct sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out);

/* Pre-refusal tip_finalize_log backfill of the span below the pinned
 * frontier (insert-only; never writes at/above served_floor and never where
 * any row exists). PRECONDITION: a coin-tear refusal must be pending
 * (out->refused_coin_tear) — the call site MUST gate on it; without a tear
 * there is no pinned span to restore and the callee no-ops. Sets
 * *handled=true when a batch made progress; on zero progress sets
 * *handled=false so the same tick can run the next repair. Diagnostics are
 * logged by the callee. Returns false only on store errors. */
bool stage_reducer_frontier_try_tipfin_backfill(
    struct sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out,
    bool *handled);

#endif /* ZCL_JOBS_STAGE_REPAIR_REDUCER_FRONTIER_INTERNAL_H */
