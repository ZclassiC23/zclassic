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

#endif /* ZCL_JOBS_STAGE_REPAIR_REDUCER_FRONTIER_INTERNAL_H */
