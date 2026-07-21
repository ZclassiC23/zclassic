/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Internal cross-translation-unit glue for the sync controller.
 *
 * The public surface lives in controllers/sync_controller.h. This
 * header is private to app/controllers/src/sync_controller*.c and
 * declares helpers that needed to become non-static so the sync
 * controller could be split across multiple files. Do not include
 * from outside app/controllers/src/. */

#ifndef ZCL_APP_CONTROLLERS_SRC_SYNC_CONTROLLER_INTERNAL_H
#define ZCL_APP_CONTROLLERS_SRC_SYNC_CONTROLLER_INTERNAL_H

#include "controllers/sync_controller.h"
#include "config/db_service.h"
#include "models/database.h"
#include "models/db_txn.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "sapling/incremental_merkle_tree.h"
#include "wallet/wallet.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Atomic job-status globals (definitions live in sync_controller.c) ── */
extern _Atomic bool g_catchup_active;
extern _Atomic int g_catchup_height;
extern _Atomic int g_catchup_target_height;
extern _Atomic int64_t g_catchup_started_at;
extern _Atomic int64_t g_catchup_last_progress_at;
extern _Atomic bool g_import_active;
extern _Atomic int g_import_rows_written;
extern _Atomic int64_t g_import_started_at;
extern _Atomic int64_t g_import_last_progress_at;

/* ── Job-status setters (definitions in sync_controller.c) ── */
int64_t sync_job_now(void);
void sync_job_catchup_begin(int start_height, int target_height);
void sync_job_catchup_progress(int height);
void sync_job_catchup_finish(void);
void sync_job_import_begin(void);
void sync_job_import_progress(int total_rows);
void sync_job_import_finish(int total_rows);

/* ── DB-service helpers (definitions in sync_controller.c) ── */
struct db_service *sync_db_service_for(struct node_db *ndb);
bool sync_db_enter_turbo_mode(struct node_db *ndb);
bool sync_db_restore_normal_mode(struct node_db *ndb);

struct sync_db_turbo_scope {
    struct node_db *ndb;
    bool entered;
};

bool sync_db_turbo_scope_begin(struct sync_db_turbo_scope *scope,
                               struct node_db *ndb,
                               bool enabled);
bool sync_db_turbo_scope_end(struct sync_db_turbo_scope *scope);

bool sync_run_write(struct node_db *ndb,
                    db_service_write_fn fn,
                    void *ctx);

/* ── Cross-file context structs ── */
struct wallet_tx_sync_ctx {
    const struct transaction *tx;
    const struct wallet *wallet;
    int block_height;
    bool is_ours;
    bool ok;
};

/* ── Helpers exposed across sync_controller_*.c files ── */

/* Defined in sync_controller_writers.c — used by sync_controller.c
 * (in node_db_sync_wallet_tx_checked). */
bool node_db_sync_wallet_tx_write(struct node_db *ndb, void *ctx);

/* Defined in sync_controller.c — used by sync_controller_catchup.c. */
bool node_db_sync_wallet_tx_checked(struct node_db *ndb,
                                    const struct transaction *tx,
                                    const struct wallet *w,
                                    int block_height,
                                    bool *is_ours_out,
                                    bool *success_out);

/* Defined in sync_controller_blocks.c — used by sync_controller_catchup.c
 * (advance_wallet_witnesses) and sync_controller_catchup.c (serialize_tx
 * via the mempool_save path that lives in sync_controller_catchup.c). */
uint8_t *serialize_tx(const struct transaction *tx, size_t *out_len);
bool advance_wallet_witnesses(struct node_db *ndb,
                              const struct block *blk,
                              struct incremental_merkle_tree *tree,
                              int height);
uint8_t *sync_controller_mmap_block_file(const char *datadir,
                                         int file_num,
                                         size_t *out_size);

/* ── Sapling-tree persist (definitions in sync_controller_sapling_tree_persist.c) ──
 * Tri-state outcome of a persist attempt. DEFERRED is distinct from FAILED:
 * it means "wrote nothing on purpose, retry later" (a foreign open tx owned
 * the connection), NOT a derived-state error — the rebuild loop must not
 * fail-close on it. */
enum sapling_persist_status {
    SAPLING_PERSIST_OK = 0,
    SAPLING_PERSIST_DEFERRED,
    SAPLING_PERSIST_FAILED,
};

/* Persist node_state["sapling_tree"] + ["sapling_tree_rebuild_height"] as ONE
 * atomic write. Shared between the rebuild replay (sync_controller_sapling_tree.c)
 * and the public bool wrapper. See the definition for the DEFERRED/BEGIN-nesting
 * contract. */
enum sapling_persist_status
sapling_tree_persist_pair_status(struct node_db *ndb,
                                 const void *blob, size_t blob_len,
                                 int64_t height);

#endif
