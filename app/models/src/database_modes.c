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
    node_db_rebuild_indexes(ndb);
    node_db_wal_checkpoint(ndb);
    node_db_note_turbo_mode(ndb, false, "normal_mode", SQLITE_OK);
    printf("db: normal mode (synchronous=NORMAL, indexes rebuilt)\n");
    return true;
}

bool node_db_wal_checkpoint(struct node_db *ndb)
{
    sqlite3_stmt *stmt = NULL;
    const char *mode = NULL;

    if (!ndb || !ndb->open) return false;
    if (sqlite3_prepare_v2(ndb->db, "PRAGMA journal_mode", -1, &stmt, NULL) == SQLITE_OK &&
        stmt && sqlite3_step(stmt) == SQLITE_ROW) {  // raw-sql-ok:read-only-introspection
        mode = (const char *)sqlite3_column_text(stmt, 0);
    }
    if (stmt)
        sqlite3_finalize(stmt);
    if (!mode || strcmp(mode, "wal") != 0) {
        node_db_note_activity(ndb, "wal_checkpoint", SQLITE_OK);
        return true;
    }
    int rc = sqlite3_wal_checkpoint_v2(ndb->db, NULL,
                                       SQLITE_CHECKPOINT_TRUNCATE,
                                       NULL, NULL);
    node_db_note_activity(ndb, "wal_checkpoint", rc);
    return rc == SQLITE_OK;
}
