/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain restore executor — mutable execution helpers for restore plans. */

#ifndef ZCL_CHAIN_RESTORE_EXECUTOR_H
#define ZCL_CHAIN_RESTORE_EXECUTOR_H

#include <stdbool.h>

#include "util/result.h"

struct main_state;
struct block_index;
struct uint256;
struct chain_restore_plan;

struct zcl_result chain_restore_commit_tip_via_csr(struct main_state *ms,
                                                   struct block_index *target,
                                                   bool update_header_tip,
                                                   const char *reason);

struct zcl_result chain_restore_commit_header_via_csr(struct main_state *ms,
                                                      struct block_index *target,
                                                      const char *reason);

/* Create a placeholder anchor block_index at `height` with `hash`.
 * Inserts it into ms->map_block_index as metadata only. The anchor is
 * deliberately not marked BLOCK_HAVE_DATA and receives no synthetic
 * chainwork; it must never win chain selection or become active consensus
 * tip until real block bytes arrive and normal validation fills it in. */
struct block_index *chain_restore_create_anchor(
    struct main_state *ms,
    const struct uint256 *hash,
    int height);

/* Apply the plan: create anchor if needed, set chain tip, etc.
 * Returns the anchor or found block_index, NULL on failure. */
struct block_index *chain_restore_execute(
    const struct chain_restore_plan *plan,
    struct main_state *ms);

#endif /* ZCL_CHAIN_RESTORE_EXECUTOR_H */
