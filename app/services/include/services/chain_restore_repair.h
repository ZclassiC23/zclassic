/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Repair — post-restore active-chain and block-index repair. */

#ifndef ZCL_CHAIN_RESTORE_REPAIR_H
#define ZCL_CHAIN_RESTORE_REPAIR_H

#include <stdbool.h>

#include "util/result.h"

struct main_state;
struct block_index;

/* After an anchor-restore / snapshot-restore / block-file-scan path
 * completes, repair the two state shapes that the normal validation path
 * would have established: active_chain slots and persisted nBits values. */
int chain_restore_rebuild_active_chain(struct main_state *ms,
                                       struct block_index *tip,
                                       const char *datadir);

int chain_restore_backfill_nbits_from_disk(struct main_state *ms,
                                           const char *datadir);

/* Clear BLOCK_FAILED_VALID + BLOCK_FAILED_CHILD on entries strictly
 * above the active tip. After a body-pull / direct-import path writes
 * new blocks past a previously-stuck tip, stale FAILED flags from old
 * IBD attempts prevent find_most_work_chain from selecting through them.
 * Re-validation under evidence-mode is cheap; genuinely-invalid blocks get
 * re-flagged by the next reducer validation pass. Returns the number of
 * entries cleared. */
int chain_restore_clear_failed_above_tip(struct main_state *ms);

bool chain_restore_block_is_consensus_backed(const struct block_index *tip);

bool chain_restore_block_is_consensus_backed_on_disk(
    const struct block_index *tip,
    const char *datadir);

struct block_index *chain_restore_nearest_consensus_backed_ancestor(
    struct block_index *tip);

struct block_index *chain_restore_nearest_consensus_backed_ancestor_on_disk(
    struct block_index *tip,
    const char *datadir);

struct zcl_result chain_restore_finalize(struct main_state *ms, const char *datadir);

#endif
