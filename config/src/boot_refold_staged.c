/* boot_refold_staged.c — the -refold-staged reset, extracted from boot.c to
 * hold the E1 file-size ceiling. Resets the staged reducer's durable derived
 * state to genesis so the staged pipeline re-folds forward over on-disk block
 * BODIES. Contract declared in config/boot.h. */
#include "config/boot.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

#include "models/database.h"
#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "config/boot_internal.h"        /* boot_index_clear_coins_state */
#include "jobs/reducer_frontier.h"       /* progress_meta_delete_in_tx,
                                          * REDUCER_TRUSTED_BASE_*_KEY,
                                          * progress_store_tx_lock/unlock */
#include "jobs/stage_repair_internal.h"  /* stage_repair_force_stage_cursor */
#include "jobs/refold_progress.h"        /* refold_progress_boot_init */

/* Declared in app/services/src/utxo_recovery_internal.h (src-private); forward
 * declare for the -refold-staged seed-provenance clear. */
void utxo_recovery_clear_cold_import_seed(struct node_db *ndb);

void boot_refold_staged_init(bool refold_staged)
{
    (void)refold_progress_boot_init(progress_store_db(), refold_staged);
}

void boot_refold_staged_reset(struct node_db *ndb)
{
    sqlite3 *rpdb = progress_store_db();
    if (!rpdb) {
        fprintf(stderr, "[boot] -refold-staged: progress store not open; skip\n");
        return;
    }

    /* Neutralize the cold-import seed provenance so the later stage init
     * (block_index_loader_seed_stages_from_cold_import) does not re-stamp the
     * trusted anchor forward to the checkpoint. */
    utxo_recovery_clear_cold_import_seed(ndb);
    (void)node_db_state_set(ndb, "leveldb_utxo_migrated", NULL, 0);

    /* Phase 1 (own transactions): truncate the coin set — do NOT reseed from
     * the wrong-fork node.db mirror; clear the node.db mirror + commitment
     * keys + header_admit_log + sapling tree. */
    (void)coins_kv_reset_for_reseed(rpdb);
    (void)boot_index_clear_coins_state(ndb);
    (void)node_db_exec(ndb, "DELETE FROM header_admit_log");
    (void)node_db_state_set(ndb, "sapling_tree", NULL, 0);
    (void)node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);

    /* Phase 2 (one progress.kv transaction): clear the reducer-derived tables,
     * force the 8 stage cursors to genesis, drop the trusted-base declaration.
     * stage_repair_force_stage_cursor REQUIRES the caller hold the progress tx
     * lock + an open transaction. */
    static const char *const k_refold_tables[] = {
        "validate_headers_log", "body_fetch_log", "body_persist_log",
        "script_validate_log", "proof_validate_log", "utxo_apply_log",
        "tip_finalize_log", "utxo_apply_delta", "nullifiers",
        "created_outputs",
    };
    static const char *const k_refold_stages[] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
    };
    bool refold_ok = true;
    char *refold_err = NULL;
    progress_store_tx_lock();
    if (sqlite3_exec(rpdb, "BEGIN IMMEDIATE", NULL, NULL, &refold_err)
        != SQLITE_OK)
        refold_ok = false;
    for (size_t i = 0;
         refold_ok && i < sizeof(k_refold_tables) / sizeof(k_refold_tables[0]);
         i++) {
        char dsql[96];
        snprintf(dsql, sizeof dsql, "DELETE FROM %s", k_refold_tables[i]);
        if (sqlite3_exec(rpdb, dsql, NULL, NULL, &refold_err) != SQLITE_OK) {
            /* A table may not exist yet on a given datadir — tolerate "no such
             * table", fail loud on any other error. */
            if (refold_err && strstr(refold_err, "no such table")) {
                sqlite3_free(refold_err);
                refold_err = NULL;
            } else {
                refold_ok = false;
            }
        }
    }
    for (size_t i = 0;
         refold_ok && i < sizeof(k_refold_stages) / sizeof(k_refold_stages[0]);
         i++)
        if (!stage_repair_force_stage_cursor(rpdb, k_refold_stages[i], 0))
            refold_ok = false;
    if (refold_ok && !coins_kv_set_applied_height_in_tx(rpdb, 0))
        refold_ok = false;
    if (refold_ok) {
        (void)progress_meta_delete_in_tx(rpdb, REDUCER_TRUSTED_BASE_HEIGHT_KEY);
        (void)progress_meta_delete_in_tx(rpdb, REDUCER_TRUSTED_BASE_HASH_KEY);
    }
    if (refold_ok && sqlite3_exec(rpdb, "COMMIT", NULL, NULL, &refold_err)
        != SQLITE_OK)
        refold_ok = false;
    if (!refold_ok)
        sqlite3_exec(rpdb, "ROLLBACK", NULL, NULL, NULL);
    if (refold_err)
        sqlite3_free(refold_err);
    progress_store_tx_unlock();

    fprintf(stderr,
            "[boot] -refold-staged: staged reducer reset to genesis %s; "
            "the staged pipeline re-folds forward over on-disk bodies "
            "(H* climbs as the logs fill)\n",
            refold_ok ? "OK" : "FAILED");
}
