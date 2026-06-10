/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_recovery_internal.h — shared declarations across the files that
 * make up the UTXO recovery service:
 *
 *   utxo_recovery_service.c   — public API: wipe, prepare_reimport,
 *                               count-check classify, execute (validation
 *                               recovery), clean_above_tip, backfill
 *   utxo_recovery_restore.c   — heavy boot recovery paths:
 *                               import_ldb (LevelDB→SQLite migration) and
 *                               restore_chain_tip (coins-best restoration)
 *
 * NOT a public header. Only included by the two files above. The
 * trailing "_internal" suffix marks these symbols as file-local to the
 * utxo_recovery translation units; they are the shared CSR-commit
 * primitives used by both the validation-recovery path and the boot
 * import/restore path, and should not be referenced from elsewhere. */

#ifndef ZCL_UTXO_RECOVERY_INTERNAL_H
#define ZCL_UTXO_RECOVERY_INTERNAL_H

#include "services/utxo_recovery_service.h"

#include <stdbool.h>

struct block_index;

/* Commit `tip` as the active chain tip + coins-best cursor through the
 * chain_state_repository (CSR). `reason` is a grep-able tag. When
 * `persist_coins_best` is true the coins_best_block key is durably
 * written. Returns ZCL_OK on success; a non-ok zcl_result with a
 * self-describing message on failure (code -43 invalid args, -44 CSR
 * rejected the promotion).
 *
 * This is the single CSR-gated promotion primitive shared by the
 * validation-recovery path (utxo_recovery_service.c) and the boot
 * import/restore path (utxo_recovery_restore.c). */
struct zcl_result utxo_recovery_commit_tip(struct utxo_recovery_ctx *ctx,
                              struct block_index *tip,
                              const char *reason,
                              bool persist_coins_best);

/* Reset the coins-best cursor + active tip to the genesis block and
 * flush the coins cache. Returns ZCL_OK on success; a non-ok zcl_result
 * on failure (code -45 invalid args, -46 genesis missing from the block
 * index, or the propagated commit_tip error). */
struct zcl_result utxo_recovery_commit_genesis(struct utxo_recovery_ctx *ctx,
                                  const char *reason);

/* Point-in-time copy of a (possibly live) zclassicd chainstate LevelDB
 * from cs_path to import_path. Retries until no source file changed
 * while the copy ran, so the image can never be torn mid-write; refuses
 * with a non-ok zcl_result if the source never goes quiet. Implemented
 * in utxo_recovery_ldb_copy.c. */
struct zcl_result utxo_recovery_copy_chainstate_stable(const char *cs_path,
                                                       const char *import_path);

#endif /* ZCL_UTXO_RECOVERY_INTERNAL_H */
