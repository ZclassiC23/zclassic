/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_anchors -- reducer-owned shielded anchor validation/fold.
 *
 * The writer runs inside utxo_apply's stage transaction.  It validates every
 * transaction against the pre-block active-chain anchor set, advances Sprout
 * and Sapling frontiers only after the whole block passes, cross-checks the
 * Sapling frontier against the header, and inserts the new roots atomically
 * with coins/nullifiers/log/cursor. */

#ifndef ZCL_JOBS_UTXO_APPLY_ANCHORS_H
#define ZCL_JOBS_UTXO_APPLY_ANCHORS_H

#include <stdbool.h>
#include <stdint.h>

struct block;
struct delta_summary;
struct sqlite3;

#define UTXO_APPLY_ANCHOR_GAP_BLOCKER_ID \
    "utxo_apply.anchor_backfill_gap"

/* Store error => false (caller rolls back/fatals).  Consensus failure or an
 * incomplete historical frontier => true with summary->ok=false and a
 * distinct status/failure_kind. */
bool utxo_apply_check_and_insert_anchors(struct sqlite3 *db,
                                         const struct block *blk,
                                         int height,
                                         struct delta_summary *summary);

/* Register/clear the durable history-gap blocker from anchor_state. */
void utxo_apply_anchor_gap_blocker_refresh(struct sqlite3 *db);

#endif /* ZCL_JOBS_UTXO_APPLY_ANCHORS_H */
