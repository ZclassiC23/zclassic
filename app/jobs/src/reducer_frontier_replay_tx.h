/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Private transaction mechanics for retained reducer-frontier replay.
 *
 * reducer_frontier_replay.c decides whether a stale script/proof/hash-split
 * replay owns a height. This helper owns the progress-store transaction body
 * once the owning height and cursors have been proven. */

#ifndef ZCL_JOBS_REDUCER_FRONTIER_REPLAY_TX_H
#define ZCL_JOBS_REDUCER_FRONTIER_REPLAY_TX_H

#include <stdbool.h>

struct block;
struct main_state;
struct script_validate_dry_run_report;
struct sqlite3;

bool reducer_frontier_replay_delete_log_range(struct sqlite3 *db,
                                              const char *table,
                                              int first_h,
                                              int cursor);

bool reducer_frontier_replay_script_ok_at_unlocked(struct sqlite3 *db,
                                                   int height,
                                                   int *out_ok);

bool reducer_frontier_replay_dry_run_stale_script(
    struct sqlite3 *db,
    struct main_state *ms,
    int height,
    int replay_first,
    int utxo_cursor,
    int backfill_top,
    const struct block *blk,
    struct script_validate_dry_run_report *dry);

bool reducer_frontier_replay_stale_script_tx(
    struct sqlite3 *db,
    struct main_state *ms,
    int height,
    int replay_first,
    int script_cursor,
    int proof_cursor,
    int utxo_cursor,
    int tip_cursor,
    int backfill_top,
    bool rewind_headers);

bool reducer_frontier_replay_stale_proof_tx(struct sqlite3 *db,
                                            int height,
                                            int replay_first,
                                            int proof_cursor,
                                            int utxo_cursor,
                                            const char *marker);

#endif /* ZCL_JOBS_REDUCER_FRONTIER_REPLAY_TX_H */
