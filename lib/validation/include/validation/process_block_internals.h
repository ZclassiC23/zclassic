/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal helpers exported from lib/validation/src/process_block.c for
 * the chain-advance atomic protocol.
 *
 * Not for general use. These are low-level validation persistence and
 * tip-publication helpers composed by the reducer path. */

#ifndef ZCL_VALIDATION_PROCESS_BLOCK_INTERNALS_H
#define ZCL_VALIDATION_PROCESS_BLOCK_INTERNALS_H

#include <stdbool.h>

struct main_state;
struct coins_view_cache;
struct block_index;
struct coins_view_sqlite;
struct block_tree_db;

/* Accessors for file-static globals/handles. */
struct coins_view_sqlite *process_block_get_coins_sqlite(void);
struct block_tree_db *process_block_get_block_tree(void);

/* csr_commit_tip wrapper used internally by update_tip + the
 * chain-advance protocol. Returns true on CSR_OK or the test-harness
 * fallback path; false on any real CSR rejection (caller must abort
 * the in-flight chain advance). */
bool process_block_commit_tip_ext(struct main_state *ms,
                                  struct coins_view_cache *coins_tip,
                                  struct block_index *new_tip,
                                  const char *reason,
                                  bool update_header_tip);

/* Force a coins flush (force=true) or a policy-gated flush
 * (force=false). Returns false on a hard flush failure that the
 * caller must surface. */
bool process_block_flush_coins(struct coins_view_cache *coins_tip,
                               bool force);

/* Persist the Sapling commitment tree row through node_db's currently
 * open transaction (or autocommit if none). Best-effort; failures are
 * logged but never abort the chain advance. */
bool process_block_persist_sapling_tree(void);

#endif /* ZCL_VALIDATION_PROCESS_BLOCK_INTERNALS_H */
