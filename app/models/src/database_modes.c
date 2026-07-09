/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * node.db UTXO lifecycle + performance modes: IBD turbo / normal mode,
 * index drop/rebuild, UTXO wiping, counting, and WAL checkpoint control.
 *
 * ar-validate-skip:connection-handle-not-a-row
 *   These functions operate on the struct node_db connection handle and
 *   the UTXO table at the bulk-lifecycle level (wipe/count, index
 *   drop/rebuild, PRAGMA modes) — not on row records. Row-level
 *   validation lives on the models that use this handle (same rationale
 *   as database.c). */

#include "util/log_macros.h"
#include "models/database.h"
#include "models/database_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── UTXO Lifecycle ────────────────────────────────────────────── */

bool node_db_wipe_utxos(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    int64_t existing = node_db_utxo_count(ndb);
    const char *offline_repair = getenv("ZCL_OFFLINE_REPAIR");
    if (existing > 1000 &&
        (!offline_repair || strcmp(offline_repair, "1") != 0)) {
        LOG_INFO("db", "db: refused to wipe %lld UTXOs without " "ZCL_OFFLINE_REPAIR=1", (long long)existing);
        return false;
    }
    bool ok = true;
    ok &= node_db_exec(ndb, "DELETE FROM utxos");
    ok &= node_db_exec(ndb, "DELETE FROM node_state WHERE key='coins_best_block'");
    ok &= node_db_exec(ndb, "DELETE FROM node_state WHERE key='utxo_commitment'");
    if (ok)
        printf("db: wiped UTXO set + coins state\n");
    return ok;
}

int64_t node_db_utxo_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    sqlite3_stmt *stmt = NULL;
    int64_t count = 0;
    if (sqlite3_prepare_v2(ndb->db, "SELECT count(*) FROM utxos",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)  // raw-sql-ok:read-only-introspection
            count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

/* ── Performance Modes ─────────────────────────────────────────── */

/* Secondary indexes dropped during IBD for throughput, rebuilt after. */
static const char *const DB_DROP_INDEXES[] = {
    "DROP INDEX IF EXISTS idx_utxo_address",
    "DROP INDEX IF EXISTS idx_utxo_value",
    "DROP INDEX IF EXISTS idx_utxo_height",
    "DROP INDEX IF EXISTS idx_utxo_height_value",
    "DROP INDEX IF EXISTS idx_tx_block",
    "DROP INDEX IF EXISTS idx_tx_height",
    /* Explorer projection secondary indexes — deferred during the bulk
     * per-block reindex (PKs stay to enforce idempotent overwrite). */
    "DROP INDEX IF EXISTS idx_txo_addr",
    "DROP INDEX IF EXISTS idx_txo_height",
    "DROP INDEX IF EXISTS idx_txo_hodl_scan",
    "DROP INDEX IF EXISTS idx_txi_prev",
    "DROP INDEX IF EXISTS idx_txi_prev_height",
    "DROP INDEX IF EXISTS idx_txi_height",
    "DROP INDEX IF EXISTS idx_ss_nf",
    "DROP INDEX IF EXISTS idx_ss_height",
    "DROP INDEX IF EXISTS idx_so_height",
    "DROP INDEX IF EXISTS idx_js_height",
    "DROP INDEX IF EXISTS idx_spnf_height",
    "DROP INDEX IF EXISTS idx_opret_height",
    "DROP INDEX IF EXISTS idx_opret_slp",
};
static const char *const DB_CREATE_INDEXES[] = {
    "CREATE INDEX IF NOT EXISTS idx_utxo_address"
        " ON utxos(address_hash) WHERE address_hash IS NOT NULL",
    "CREATE INDEX IF NOT EXISTS idx_utxo_value"
        " ON utxos(value DESC)",
    "CREATE INDEX IF NOT EXISTS idx_utxo_height"
        " ON utxos(height)",
    "CREATE INDEX IF NOT EXISTS idx_utxo_height_value"
        " ON utxos(height, value)",
    "CREATE INDEX IF NOT EXISTS idx_tx_block"
        " ON transactions(block_hash)",
    "CREATE INDEX IF NOT EXISTS idx_tx_height"
        " ON transactions(block_height)",
    /* Explorer projection secondary indexes (mirror database_migrate.c v9). */
    "CREATE INDEX IF NOT EXISTS idx_txo_addr"
        " ON tx_outputs(address_hash) WHERE address_hash IS NOT NULL",
    "CREATE INDEX IF NOT EXISTS idx_txo_height"
        " ON tx_outputs(block_height)",
    "CREATE INDEX IF NOT EXISTS idx_txo_hodl_scan"
        " ON tx_outputs(block_height, value, txid, vout)",
    "CREATE INDEX IF NOT EXISTS idx_txi_prev"
        " ON tx_inputs(prev_txid, prev_vout)",
    "CREATE INDEX IF NOT EXISTS idx_txi_prev_height"
        " ON tx_inputs(prev_txid, prev_vout, block_height)",
    "CREATE INDEX IF NOT EXISTS idx_txi_height"
        " ON tx_inputs(block_height)",
    "CREATE INDEX IF NOT EXISTS idx_ss_nf"
        " ON sapling_spends(nullifier)",
    "CREATE INDEX IF NOT EXISTS idx_ss_height"
        " ON sapling_spends(block_height)",
    "CREATE INDEX IF NOT EXISTS idx_so_height"
        " ON sapling_outputs(block_height)",
    "CREATE INDEX IF NOT EXISTS idx_js_height"
        " ON joinsplits(block_height)",
    "CREATE INDEX IF NOT EXISTS idx_spnf_height"
        " ON sprout_nullifiers(block_height)",
    "CREATE INDEX IF NOT EXISTS idx_opret_height"
        " ON op_returns(block_height)",
    "CREATE INDEX IF NOT EXISTS idx_opret_slp"
        " ON op_returns(is_slp) WHERE is_slp = 1",
};
#define NUM_DB_INDEXES (sizeof(DB_DROP_INDEXES) / sizeof(DB_DROP_INDEXES[0]))

bool node_db_drop_indexes(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    bool all_ok = true;
    for (size_t i = 0; i < NUM_DB_INDEXES; i++) {
        if (db_exec_checked(ndb->db, DB_DROP_INDEXES[i],
                            "drop_indexes") != SQLITE_OK)
            all_ok = false;
    }
    return all_ok;
}

bool node_db_rebuild_indexes(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    bool all_ok = true;
    for (size_t i = 0; i < NUM_DB_INDEXES; i++) {
        if (db_exec_checked(ndb->db, DB_CREATE_INDEXES[i],
                            "rebuild_indexes") != SQLITE_OK)
            all_ok = false;
    }
    return all_ok;
}

bool node_db_ibd_turbo_mode(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    /* Turbo-mode PRAGMAs are performance optimisations, not integrity
     * invariants — if any of them fail, fall back to the safe
     * defaults and carry on.  The previous silent path left the DB in
     * a partial-turbo state (e.g. synchronous=OFF succeeded but
     * wal_autocheckpoint=0 did not, so WAL grew unbounded). */
    static const char *const turbo_pragmas[] = {
        "PRAGMA synchronous=OFF",
        "PRAGMA cache_size=-524288",
        "PRAGMA wal_autocheckpoint=0",
        NULL,
    };
    bool turbo_ok = true;
    for (int i = 0; turbo_pragmas[i]; i++) {
        if (db_exec_checked(ndb->db, turbo_pragmas[i],
                            "ibd_turbo_mode pragma") != SQLITE_OK)
            turbo_ok = false;
    }
    sqlite3_busy_timeout(ndb->db, 10000);

    if (!turbo_ok) {
        LOG_WARN("db", "[db] ibd_turbo_mode: one or more PRAGMAs failed; " "falling back to safe defaults (IBD will be slower " "but correct)");
        db_exec_checked(ndb->db, "PRAGMA synchronous=NORMAL",
                        "turbo_fallback synchronous");
        db_exec_checked(ndb->db, "PRAGMA cache_size=-65536",
                        "turbo_fallback cache_size");
        db_exec_checked(ndb->db, "PRAGMA wal_autocheckpoint=1000",
                        "turbo_fallback wal_autocheckpoint");
        db_exec_checked(ndb->db, "PRAGMA journal_size_limit=67108864",
                        "turbo_fallback journal_size_limit");
        node_db_note_turbo_mode(ndb, false, "ibd_turbo_mode_fallback",
                                SQLITE_ERROR);
        return false;
    }

    node_db_drop_indexes(ndb);
    node_db_note_turbo_mode(ndb, true, "ibd_turbo_mode", SQLITE_OK);
    printf("db: IBD turbo mode (synchronous=OFF, indexes dropped)\n");
    return true;
}

bool node_db_normal_mode(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    db_exec_checked(ndb->db, "PRAGMA synchronous=NORMAL",
                    "normal_mode synchronous");
    db_exec_checked(ndb->db, "PRAGMA cache_size=-65536",
                    "normal_mode cache_size");
    db_exec_checked(ndb->db, "PRAGMA wal_autocheckpoint=1000",
                    "normal_mode wal_autocheckpoint");
    /* Cap the WAL FILE size after checkpoint. wal_autocheckpoint is PASSIVE: it
     * folds WAL pages into the db but leaves the .wal file at its high-water
     * mark, so one write burst can leave a large file sitting for the entire
     * life of a days-long node (a slow disk-fill the disk_full condition cannot
     * reclaim). journal_size_limit truncates the .wal back to the cap after each
     * checkpoint, bounding disk at the source — no reclaim thread, no deleting a
     * live WAL out from under an open handle. 64 MiB is generous headroom over
     * the ~4 MiB autocheckpoint trigger; it only caps pathological bursts. */
    db_exec_checked(ndb->db, "PRAGMA journal_size_limit=67108864",
                    "normal_mode journal_size_limit");
    node_db_rebuild_indexes(ndb);
    node_db_wal_checkpoint(ndb);
    node_db_note_turbo_mode(ndb, false, "normal_mode", SQLITE_OK);
    printf("db: normal mode (synchronous=NORMAL, indexes rebuilt)\n");
    return true;
}

bool node_db_wal_checkpoint(struct node_db *ndb)
{
    sqlite3_stmt *stmt = NULL;
    bool is_wal = false;

    if (!ndb || !ndb->open) return false;
    if (sqlite3_prepare_v2(ndb->db, "PRAGMA journal_mode", -1, &stmt, NULL) == SQLITE_OK &&
        stmt && sqlite3_step(stmt) == SQLITE_ROW) {  // raw-sql-ok:read-only-introspection
        /* sqlite3_column_text points INTO the stmt; it is freed by
         * sqlite3_finalize below. Capture the comparison while the stmt is
         * still live — reading `mode` after finalize is a use-after-free. */
        const char *mode = (const char *)sqlite3_column_text(stmt, 0);
        is_wal = (mode && strcmp(mode, "wal") == 0);
    }
    if (stmt)
        sqlite3_finalize(stmt);
    if (!is_wal) {
        node_db_note_activity(ndb, "wal_checkpoint", SQLITE_OK);
        return true;
    }
    /* TRUNCATE resets the WAL file to zero bytes but needs an exclusive
     * checkpoint moment: on a busy multi-connection node.db (the periodic
     * checkpoint, catch-up, and the many onion/explorer/coordinator readers
     * all share this file) it returns SQLITE_BUSY and reclaims NOTHING, so
     * the WAL grew without bound (observed ~196 MB on the canonical node) and
     * an ever-larger WAL lengthens every write-lock hold window until the
     * wallet-key flush can no longer win the lock within its retry budget.
     * Fall back to a PASSIVE checkpoint when TRUNCATE is busy: PASSIVE never
     * blocks on other connections and reclaims every WAL frame up to the
     * oldest live reader, so the WAL stops growing (frames are reused) even
     * when it cannot be truncated to zero. A PASSIVE pass that checkpoints
     * some frames is progress, not a failure. */
    int rc = sqlite3_wal_checkpoint_v2(ndb->db, NULL,
                                       SQLITE_CHECKPOINT_TRUNCATE,
                                       NULL, NULL);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        int prc = sqlite3_wal_checkpoint_v2(ndb->db, NULL,
                                            SQLITE_CHECKPOINT_PASSIVE,
                                            NULL, NULL);
        node_db_note_activity(ndb, "wal_checkpoint_passive", prc);
        return prc == SQLITE_OK;
    }
    node_db_note_activity(ndb, "wal_checkpoint", rc);
    return rc == SQLITE_OK;
}
