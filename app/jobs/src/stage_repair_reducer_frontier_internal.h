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
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out);

bool stage_reducer_frontier_reconcile_validate_hash_split_cursor(
    struct sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out);

/* FIX-2a: pre-refusal clamp of the script_validate / proof_validate cursors
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
 * (relabel/reorg residue — e.g. the 2026-06-10 -2 incident's false
 * bad-cb-height verdicts), plus the hashless downstream rows at those
 * heights. Dry-run (apply=false) only counts into out->noncanonical_*.
 * Purged rows become ordinary rowless holes for the existing refill +
 * cursor machinery. Genuine consensus rejects keep their rows (their
 * hash IS canonical). Returns false only on store errors.
 * Implemented in stage_repair_reducer_frontier_purge.c. */
bool stage_reducer_frontier_purge_noncanonical(
    struct sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out);

/* FIX-1: pre-refusal tip_finalize_log backfill of the span below the pinned
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
