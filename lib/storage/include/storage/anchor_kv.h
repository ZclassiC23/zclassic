/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * anchor_kv -- durable Sprout/Sapling note-commitment anchors in progress.kv.
 *
 * zclassicd stores every active-chain commitment-tree root in chainstate and
 * HaveShieldedRequirements rejects a spend whose root is absent.  These
 * tables are the C23 equivalent.  A row contains the complete incremental
 * frontier, not only the root: Sprout permits a later JoinSplit in the SAME
 * transaction to spend against the intermediate root produced by an earlier
 * JoinSplit, so the validator must be able to append to an historical tree.
 *
 * Writes participate in the caller's progress.kv transaction.  The reducer
 * inserts anchors before committing its cursor/log/coins state, and every
 * rewind deletes abandoned-branch rows in that same transaction.
 *
 * HISTORY GAP: an existing snapshot/import datadir has no historical anchor
 * rows.  anchor_state.activation_cursor records the first height for which
 * this store can derive anchors.  Zero means a from-genesis history.  A
 * missing root while activation_cursor > 0 is INCOMPLETE, never proof that
 * the root was forged; callers must fail closed with a named blocker until a
 * body replay backfills [0, activation_cursor). */

#ifndef STORAGE_ANCHOR_KV_H
#define STORAGE_ANCHOR_KV_H

#include "core/uint256.h"
#include "sapling/incremental_merkle_tree.h"

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

/* Durable values: never renumber.  They intentionally match nullifier_kv. */
#define ANCHOR_POOL_SPROUT  0
#define ANCHOR_POOL_SAPLING 1

enum anchor_kv_lookup_result {
    ANCHOR_KV_ERROR = -1,
    ANCHOR_KV_MISSING = 0,
    ANCHOR_KV_FOUND = 1,
    ANCHOR_KV_HISTORY_INCOMPLETE = 2,
};

/* Create sprout_anchors, sapling_anchors and anchor_state. */
bool anchor_kv_ensure_schema(struct sqlite3 *db);

/* Ensure both anchor_state rows exist.  `activation_cursor` is the reducer's
 * next-height cursor at first adoption (0 on a from-genesis store).  Existing
 * rows are never overwritten. */
bool anchor_kv_initialize_history(struct sqlite3 *db,
                                  int64_t activation_cursor);

/* Read the adoption cursor.  Returns false on store/argument error. */
bool anchor_kv_activation_cursor(struct sqlite3 *db, int pool,
                                 int64_t *cursor_out, bool *found_out);

/* Point lookup.  Empty roots are protocol-defined and always FOUND.  For a
 * stored root, `tree_out` (optional) receives a verified/deserialized tree.
 * The row is rejected as ERROR if its blob does not hash back to `root`.
 * A missing non-empty root reports HISTORY_INCOMPLETE when the store was
 * adopted above genesis, otherwise MISSING (positive proof of absence). */
enum anchor_kv_lookup_result anchor_kv_get(
    struct sqlite3 *db, int pool, const struct uint256 *root,
    struct incremental_merkle_tree *tree_out, int64_t *height_out);

/* Current active-chain frontier: highest-height stored row, or the empty tree
 * when history is complete and the pool has not changed yet. */
enum anchor_kv_lookup_result anchor_kv_latest_tree(
    struct sqlite3 *db, int pool, struct incremental_merkle_tree *tree_out,
    struct uint256 *root_out, int64_t *height_out);

/* Insert a newly-derived active-chain frontier.  Idempotent for a root already
 * present (the earliest creation height is preserved). */
bool anchor_kv_add_tree(struct sqlite3 *db, int pool,
                        const struct incremental_merkle_tree *tree,
                        int64_t height);

/* Remove roots first created in the abandoned height range. */
bool anchor_kv_delete_range(struct sqlite3 *db, int64_t first_height,
                            int64_t last_height);

/* Clear both root sets and reset their adoption cursor.  This does not open a
 * transaction: boot/refold callers already own one. */
bool anchor_kv_reset_in_tx(struct sqlite3 *db, int64_t activation_cursor);

#endif /* STORAGE_ANCHOR_KV_H */
