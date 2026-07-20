/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_H
#define ZCL_DB_H

#include "util/sync.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NODE_DB_SCHEMA_LATEST 34

struct node_db_status {
    bool open;
    bool tx_open;
    bool turbo_mode;
    int sync_batch_size;
    int sync_pending_blocks;
    int64_t last_activity_time;
    int last_sqlite_rc;
    char last_op[64];
};

struct node_db {
    sqlite3 *db;
    bool open;
    char path[1024];

    /* Prepared statements cached for hot paths */
    sqlite3_stmt *stmt_utxo_insert;
    sqlite3_stmt *stmt_snapshot_staging_insert;
    sqlite3_stmt *stmt_utxo_delete;
    sqlite3_stmt *stmt_utxo_find;
    sqlite3_stmt *stmt_block_insert;
    sqlite3_stmt *stmt_block_by_hash;
    sqlite3_stmt *stmt_block_by_height;
    sqlite3_stmt *stmt_tx_insert;
    sqlite3_stmt *stmt_tx_find;
    sqlite3_stmt *stmt_wallet_utxo_insert;
    sqlite3_stmt *stmt_wallet_utxo_spend;
    sqlite3_stmt *stmt_wallet_balance;
    sqlite3_stmt *stmt_nullifier_insert;
    sqlite3_stmt *stmt_nullifier_exists;
    sqlite3_stmt *stmt_state_set;
    sqlite3_stmt *stmt_state_get;

    /* Peer model — cached for hot P2P paths */
    sqlite3_stmt *stmt_peer_save;
    sqlite3_stmt *stmt_peer_find;
    sqlite3_stmt *stmt_peer_delete;
    sqlite3_stmt *stmt_peer_count;

    /* File service model */
    sqlite3_stmt *stmt_file_service_save;
    sqlite3_stmt *stmt_file_service_find;

    /* Explorer projection model (full per-block indexer) — cached for
     * the per-tx hot path in sync_block_lean. See explorer_index.c. */
    sqlite3_stmt *stmt_txo_insert;
    sqlite3_stmt *stmt_txi_insert;
    sqlite3_stmt *stmt_opret_insert;
    sqlite3_stmt *stmt_sspend_insert;
    sqlite3_stmt *stmt_soutput_insert;
    sqlite3_stmt *stmt_js_insert;
    sqlite3_stmt *stmt_spnf_insert;
    sqlite3_stmt *stmt_vint_insert;

    /* Batch sync: accumulate N blocks before COMMIT for throughput.
     * batch_size=1 is the safe default (per-block COMMIT).
     * During IBD, set batch_size=100+ for 10-50x SQLite throughput. */
    int  sync_batch_size;       /* target blocks per COMMIT (default 1) */
    int  sync_pending_blocks;   /* blocks since last COMMIT */
    bool sync_in_batch;         /* true if a batch transaction is open */

    /* Runtime ownership/health state for SQLite access. */
    zcl_mutex_t state_mutex;
    bool state_mutex_init;
    bool tx_open;
    bool turbo_mode;
    int64_t last_activity_time;
    int last_sqlite_rc;
    char last_op[64];
    /* Transient: set for a runtime reopen so node_db_migrate() suppresses the
     * boot-only "current schema version" banner. Never mistake a background
     * reopen for a boot. See node_db_open_runtime(). */
    bool suppress_migrate_banner;
};

/* Open or create the node database at path (e.g. ~/.zclassic-c23/node.db).
 * Runs the BOOT ceremony: PRAGMA quick_check (integrity), schema migration
 * (with the version banner), and crash-recovery staging cleanup. This is the
 * one-time-per-process boot open. Creates all tables/indexes if absent. */
bool node_db_open(struct node_db *ndb, const char *path);

/* Open the node database for an IN-PROCESS RUNTIME REOPEN (a background worker
 * or request handler after boot already opened + verified the file once).
 *
 * Skips the boot-only ceremony — the ~280 ms PRAGMA quick_check, the
 * snapshot-staging crash-recovery DELETEs, and the schema-version banner — none
 * of which are meaningful on a reopen and all of which, emitted every cycle,
 * make a periodic reopen indistinguishable from a boot loop in a filtered log
 * (the exact "silent halt looks like a restart" trap this project forbids).
 *
 * `reason` is MANDATORY and must be a short, non-empty label naming the opener
 * (e.g. "store.payment_scan"); it is logged once per open so no DB open is ever
 * anonymous. A NULL/empty reason is a programming error and is refused. */
bool node_db_open_runtime(struct node_db *ndb, const char *path,
                          const char *reason);
void node_db_close(struct node_db *ndb);

/* Optional quick_check-skip probe (Tier-2 fast restart). When registered,
 * node_db_open() calls it with the db path BEFORE opening the sqlite handle;
 * a true return means "the previous shutdown proved this file clean — skip the
 * expensive PRAGMA quick_check on this open". Default (unset) = quick_check
 * always runs, identical to prior behavior. The probe must be a cheap, pure
 * read of on-disk state; it must never mutate the file. Registered by the boot
 * layer (config/boot_shutdown_marker.c) to keep app/models decoupled. */
typedef bool (*node_db_quick_check_skip_probe_fn)(const char *path);
void node_db_set_quick_check_skip_probe(node_db_quick_check_skip_probe_fn fn);

/* Execute raw SQL (for migrations, debugging). */
bool node_db_exec(struct node_db *ndb, const char *sql);
bool node_db_prepare_readonly_query(struct node_db *ndb, const char *sql,
                                    sqlite3_stmt **stmt_out);

/* Transaction control for batch operations. */
bool node_db_begin(struct node_db *ndb);
bool node_db_commit(struct node_db *ndb);
bool node_db_rollback(struct node_db *ndb);

/* Set SQLite sync batch size (blocks per COMMIT).
 * 1 = safe default (per-block). 100+ = aggressive IBD mode. */
void node_db_set_sync_batch_size(struct node_db *ndb, int batch_size);

/* Flush any pending batch transaction (call on shutdown or before reorg). */
bool node_db_sync_flush(struct node_db *ndb);

/* Runtime state snapshot for health/diagnostics surfaces. */
void node_db_get_status(struct node_db *ndb, struct node_db_status *out);

/* Key-value state store (replaces misc flags). */
bool node_db_state_set(struct node_db *ndb, const char *key,
                       const void *value, size_t len);
bool node_db_state_set_detached(struct node_db *ndb, const char *key,
                                const void *value, size_t len);
bool node_db_state_get(struct node_db *ndb, const char *key,
                       void *value, size_t max_len, size_t *out_len);
bool node_db_state_set_int(struct node_db *ndb, const char *key, int64_t val);
bool node_db_state_get_int(struct node_db *ndb, const char *key, int64_t *val);
/* Delete a node_state key. Returns true when the key is gone afterwards
 * (including when it was already absent — deleting a missing key is success). */
bool node_db_state_delete(struct node_db *ndb, const char *key);

/* ── UTXO Lifecycle ─────────────────────────────────────────────── */

/* Wipe all UTXOs and related state (coins_best_block, utxo_commitment).
 * Refuses to wipe more than 1,000 rows unless the process is running
 * an explicit offline repair path (ZCL_OFFLINE_REPAIR=1). */
bool node_db_wipe_utxos(struct node_db *ndb);

/* Count UTXOs in the database. */
int64_t node_db_utxo_count(struct node_db *ndb);

/* ── Performance Modes ─────────────────────────────────────────── */

/* IBD turbo: synchronous=OFF, large cache, drop secondary indexes.
 * Call before bulk block import. ~60x throughput improvement. */
bool node_db_ibd_turbo_mode(struct node_db *ndb);

/* Normal mode: synchronous=NORMAL, rebuild indexes, truncate WAL.
 * Call after IBD completes. */
bool node_db_normal_mode(struct node_db *ndb);

/* WAL checkpoint (TRUNCATE). Call after large bulk operations. */
bool node_db_wal_checkpoint(struct node_db *ndb);

/* Drop secondary indexes for bulk load throughput. */
bool node_db_drop_indexes(struct node_db *ndb);

/* Rebuild secondary indexes after bulk load. */
bool node_db_rebuild_indexes(struct node_db *ndb);

/* ── Schema ────────────────────────────────────────────────────── */

/* Highest schema_version this binary knows how to read or migrate.
 * Bump in lockstep with the last `if (current_ver < N)` block in
 * node_db_migrate(). A node.db with `schema_version > NODE_DB_MAX_SCHEMA`
 * was written by a newer binary and is unsafe to open — its tables
 * may use columns this binary doesn't understand, leading to silent
 * data corruption on writes. node_db_migrate() refuses to proceed
 * in that case (Campaign C3: schema-downgrade detection). */
#define NODE_DB_MAX_SCHEMA 34

/* Schema version for future migrations. */
int node_db_schema_version(struct node_db *ndb);

/* Rails-style migration runner.
 * Runs all pending migrations from db/migrate/ directory.
 * Tracks applied migrations in schema_migrations table.
 * Returns number of migrations applied, or -1 on error.
 * Returns -2 if the on-disk schema_version exceeds NODE_DB_MAX_SCHEMA
 * (downgrade attempted — fatal). */
int node_db_migrate(struct node_db *ndb, const char *datadir);

#endif
