/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * App-feature schema migrations (v14+) for node.db: store products and
 * orders, ZCL Market file offers, ZNAM name registry, ZMSG messaging,
 * ZSWP atomic-swap contracts, HODL wave history, and the
 * content-addressed blob store. node_db_migrate() in database_migrate.c
 * owns the core chain/explorer schema versions and hands off here at
 * the v14 boundary; every block follows the same idempotent
 * versioned-block pattern documented there (check schema version, run
 * DDL, stamp schema_migrations + schema_version).
 *
 * ar-validate-skip:connection-handle-not-a-row
 *   These functions operate on the struct node_db connection handle and
 *   the schema_migrations bookkeeping — none of which are row records.
 *   Row-level validation lives on the models that use this handle (same
 *   rationale as database.c). */

#include "util/log_macros.h"
#include "models/database.h"
#include "models/database_internal.h"

int node_db_migrate_features(struct node_db *ndb, int *version)
{
    int applied = 0;
    int current_ver = *version;

    if (current_ver < 14) {
        /* v14: Store product/order tables as model-owned app schema. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS products ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL,"
            "description TEXT,"
            "price_zatoshi INTEGER NOT NULL,"
            "token_id TEXT,"
            "tokens_per_purchase INTEGER NOT NULL DEFAULT 1,"
            "active INTEGER NOT NULL DEFAULT 1)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS orders ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "product_id INTEGER NOT NULL,"
            "customer_addr TEXT,"
            "payment_addr TEXT NOT NULL,"
            "amount_zatoshi INTEGER NOT NULL,"
            "payment_txid TEXT,"
            "mint_txid TEXT,"
            "status INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL,"
            "paid_at INTEGER)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_orders_status_created "
            "ON orders(status, created_at DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('014')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 14);
        current_ver = 14;
        applied++;
    }

    if (current_ver < 15) {
        /* v15: ZCL Market — file offers for crypto-incentivized sharing */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS file_offers ("
            "root_hash BLOB NOT NULL PRIMARY KEY,"
            "filename TEXT NOT NULL,"
            "size_bytes INTEGER NOT NULL,"
            "num_chunks INTEGER NOT NULL,"
            "price_per_mb INTEGER NOT NULL,"
            "z_addr BLOB,"
            "peer_ip BLOB,"
            "peer_port INTEGER,"
            "last_seen INTEGER,"
            "ttl INTEGER DEFAULT 4)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_file_offers_last_seen "
            "ON file_offers(last_seen DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('015')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 15);
        current_ver = 15;
        applied++;
    }

    if (current_ver < 16) {
        /* v16: ZCL Names (ZNAM) — on-chain name registry */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS znam_names ("
            "name TEXT PRIMARY KEY,"
            "owner_address TEXT NOT NULL,"
            "target_type INTEGER NOT NULL,"
            "target_value TEXT NOT NULL,"
            "reg_txid BLOB NOT NULL,"
            "reg_height INTEGER NOT NULL,"
            "last_update_txid BLOB NOT NULL)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_znam_owner "
            "ON znam_names(owner_address)");

        /* ENS-inspired text records (TextResolver) */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS znam_text_records ("
            "name TEXT NOT NULL,"
            "key TEXT NOT NULL,"
            "value TEXT,"
            "PRIMARY KEY(name, key))");

        /* ENS-inspired multi-coin address records (AddrResolver) */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS znam_addr_records ("
            "name TEXT NOT NULL,"
            "coin_type INTEGER NOT NULL,"
            "address TEXT NOT NULL,"
            "PRIMARY KEY(name, coin_type))");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('016')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 16);
        current_ver = 16;
        applied++;
    }

    if (current_ver < 17) {
        /* v17: ZCL Messaging (ZMSG) — encrypted P2P messages */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zmsg_messages ("
            "msg_id BLOB PRIMARY KEY,"
            "direction INTEGER NOT NULL,"
            "channel INTEGER NOT NULL,"
            "sender TEXT NOT NULL,"
            "recipient TEXT NOT NULL,"
            "body TEXT NOT NULL,"
            "timestamp INTEGER NOT NULL,"
            "txid BLOB,"
            "read INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zmsg_time "
            "ON zmsg_messages(timestamp DESC)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zmsg_unread "
            "ON zmsg_messages(read) WHERE read=0");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('017')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 17);
        current_ver = 17;
        applied++;
    }

    if (current_ver < 18) {
        /* v18: Atomic swaps (ZSWP) — HTLC contracts for BTC/LTC/DOGE */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zswp_contracts ("
            "swap_id TEXT PRIMARY KEY,"
            "role INTEGER NOT NULL,"
            "state INTEGER NOT NULL,"
            "chain INTEGER NOT NULL DEFAULT 0,"
            "secret_hash BLOB NOT NULL,"
            "secret BLOB,"
            "amount INTEGER NOT NULL,"
            "locktime INTEGER NOT NULL,"
            "my_address TEXT NOT NULL,"
            "counter_address TEXT NOT NULL,"
            "funding_txid BLOB,"
            "funding_vout INTEGER,"
            "redeem_script BLOB NOT NULL,"
            "redeem_script_len INTEGER NOT NULL,"
            "p2sh_address TEXT NOT NULL,"
            "created_at INTEGER NOT NULL)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zswp_state "
            "ON zswp_contracts(state)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('018')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 18);
        current_ver = 18;
        applied++;
    }

    if (current_ver < 19) {
        /* v19: HODL wave history — one row per sample height, populated
         * lazily by app/services/src/hodl_history_service.c. Each row
         * is the reconstructed UTXO snapshot as of that height:
         *   total_zat = sum of unspent output value at H
         *   older_1y_zat = subset that was older than 1 year at H
         *   older_1y_pct = older_1y_zat / total_zat * 100
         * Source: tx_outputs + tx_inputs, no current-UTXO-table dep. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS hodl_history ("
            "height INTEGER PRIMARY KEY,"
            "time INTEGER NOT NULL,"
            "total_zat INTEGER NOT NULL DEFAULT 0 CHECK(total_zat >= 0),"
            "older_1y_zat INTEGER NOT NULL DEFAULT 0 "
            "  CHECK(older_1y_zat >= 0 AND older_1y_zat <= total_zat),"
            "older_1y_pct REAL NOT NULL DEFAULT 0,"
            "calc_version INTEGER NOT NULL DEFAULT 0,"
            "source_tip_height INTEGER NOT NULL DEFAULT -1)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_hodl_history_time "
            "ON hodl_history(time)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('019')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 19);
        current_ver = 19;
        applied++;
    }

    if (current_ver < 20) {
        /* v20: content-addressed blob store; products.content_hash NULL = none. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS store_blobs ("
            "content_hash BLOB NOT NULL PRIMARY KEY,"
            "content_type TEXT NOT NULL DEFAULT 'application/octet-stream',"
            "filename TEXT,"
            "size_bytes INTEGER NOT NULL,"
            "data BLOB NOT NULL)");
        node_db_exec(ndb, "ALTER TABLE products ADD COLUMN content_hash BLOB");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('020')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 20);
        current_ver = 20;
        applied++;
    }

    if (current_ver < 21) {
        /* v21: distinguish durable wallet notes from zclassicd view-only
         * placeholders. Existing notes default to local, and only the
         * wallet-view mirror path opts into source='view'. */
        if (db_exec_tolerant(ndb->db,
            "ALTER TABLE wallet_sapling_notes "
            "ADD COLUMN source TEXT NOT NULL DEFAULT 'local'",
            "v21: add wallet_sapling_notes.source",
            "duplicate column name") != SQLITE_OK)
            LOG_ERR("db", "v21 migration failed adding wallet_sapling_notes.source");
        if (db_exec_checked(ndb->db,
            "CREATE INDEX IF NOT EXISTS idx_snote_view_address "
            "ON wallet_sapling_notes(address) "
            "WHERE spent_txid IS NULL AND source='view'",
            "v21: idx_snote_view_address") != SQLITE_OK)
            LOG_ERR("db", "v21 migration failed creating idx_snote_view_address");
        if (!node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('021')"))
            LOG_ERR("db", "v21 migration failed stamping schema_migrations");
        DB_MIGRATE_PERSIST_VERSION(ndb, 21);
        current_ver = 21;
        applied++;
    }

    if (current_ver < 22) {
        /* v22: covering indexes for HODL-history reconstruction. The
         * background filler asks historical "alive at H" questions across
         * tx_outputs/tx_inputs; these keep post-rebuild gap repair bounded. */
        if (db_exec_checked(ndb->db,
            "CREATE INDEX IF NOT EXISTS idx_txo_hodl_scan "
            "ON tx_outputs(block_height, value, txid, vout)",
            "v22: idx_txo_hodl_scan") != SQLITE_OK)
            LOG_ERR("db", "v22 migration failed creating idx_txo_hodl_scan");
        if (db_exec_checked(ndb->db,
            "CREATE INDEX IF NOT EXISTS idx_txi_prev_height "
            "ON tx_inputs(prev_txid, prev_vout, block_height)",
            "v22: idx_txi_prev_height") != SQLITE_OK)
            LOG_ERR("db", "v22 migration failed creating idx_txi_prev_height");
        if (!node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('022')"))
            LOG_ERR("db", "v22 migration failed stamping schema_migrations");
        DB_MIGRATE_PERSIST_VERSION(ndb, 22);
        current_ver = 22;
        applied++;
    }

    if (current_ver < 23) {
        /* v23: HODL history repair provenance. Existing rows default to
         * calc_version=0/source_tip=-1, so the lazy filler refreshes them
         * only after the explorer projection cursor proves source coverage. */
        if (db_exec_checked(ndb->db,
            "CREATE TABLE IF NOT EXISTS hodl_history ("
            "height INTEGER PRIMARY KEY,"
            "time INTEGER NOT NULL,"
            "total_zat INTEGER NOT NULL DEFAULT 0 CHECK(total_zat >= 0),"
            "older_1y_zat INTEGER NOT NULL DEFAULT 0 "
            "  CHECK(older_1y_zat >= 0 AND older_1y_zat <= total_zat),"
            "older_1y_pct REAL NOT NULL DEFAULT 0,"
            "calc_version INTEGER NOT NULL DEFAULT 0,"
            "source_tip_height INTEGER NOT NULL DEFAULT -1)",
            "v23: create hodl_history if missing") != SQLITE_OK)
            LOG_ERR("db", "v23 migration failed ensuring hodl_history table");
        if (db_exec_tolerant(ndb->db,
            "ALTER TABLE hodl_history "
            "ADD COLUMN calc_version INTEGER NOT NULL DEFAULT 0",
            "v23: add hodl_history.calc_version",
            "duplicate column name") != SQLITE_OK)
            LOG_ERR("db", "v23 migration failed adding hodl_history.calc_version");
        if (db_exec_tolerant(ndb->db,
            "ALTER TABLE hodl_history "
            "ADD COLUMN source_tip_height INTEGER NOT NULL DEFAULT -1",
            "v23: add hodl_history.source_tip_height",
            "duplicate column name") != SQLITE_OK)
            LOG_ERR("db", "v23 migration failed adding hodl_history.source_tip_height");
        if (db_exec_checked(ndb->db,
            "CREATE INDEX IF NOT EXISTS idx_hodl_history_time "
            "ON hodl_history(time)",
            "v23: idx_hodl_history_time") != SQLITE_OK)
            LOG_ERR("db", "v23 migration failed creating idx_hodl_history_time");
        if (!node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('023')"))
            LOG_ERR("db", "v23 migration failed stamping schema_migrations");
        DB_MIGRATE_PERSIST_VERSION(ndb, 23);
        current_ver = 23;
        applied++;
    }

    *version = current_ver;
    return applied;
}
