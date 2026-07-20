/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * App-feature schema migrations v30+ for node.db — continuation of
 * database_migrate_features.c (E1 file-size split; same idempotent
 * versioned-block pattern documented at the top of that file and in
 * database_migrate.c). node_db_migrate_features() hands off here at the
 * v30 boundary via node_db_migrate_features2().
 *
 * ar-validate-skip:connection-handle-not-a-row
 *   Same rationale as database_migrate_features.c: operates on the
 *   struct node_db connection handle + schema_migrations bookkeeping,
 *   never a row record. */

#include "models/database.h"
#include "models/database_internal.h"

int node_db_migrate_features2(struct node_db *ndb, int *version)
{
    int applied = 0;
    int current_ver = *version;

    if (current_ver < 30) {
        /* v30: ZCL Anchors (ZANC) — software/package digest anchoring. One
         * rebuildable projection row per confirmed ZANC OP_RETURN, keyed by
         * txid. Not consulted by consensus; rebuilt from block history. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zanc_anchors ("
            "txid BLOB PRIMARY KEY CHECK(length(txid)=32),"
            "height INTEGER NOT NULL,"
            "hash_type INTEGER NOT NULL,"
            "digest BLOB NOT NULL CHECK(length(digest)=32),"
            "label TEXT NOT NULL DEFAULT '') WITHOUT ROWID");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zanc_digest "
            "ON zanc_anchors(hash_type, digest, height)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('030')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 30);
        current_ver = 30;
        applied++;
    }

    if (current_ver < 31) {
        /* v31: OP_RETURN catalog (op_return_index) — one rebuildable
         * projection row per OP_RETURN OUTPUT (not per tx; a tx with
         * several OP_RETURN outputs gets several rows), covering ZNAM,
         * ZSLP, ZANC, and unrecognized lokad tags alike. See
         * models/op_return_index.h. Not consulted by consensus; rebuilt
         * from block history (op_return_index_truncate). */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS op_return_index ("
            "txid BLOB NOT NULL CHECK(length(txid)=32),"
            "vout_n INTEGER NOT NULL,"
            "height INTEGER NOT NULL,"
            "tag BLOB NOT NULL,"
            "tag_text TEXT NOT NULL,"
            "payload_len INTEGER NOT NULL,"
            "payload_sha3 BLOB NOT NULL CHECK(length(payload_sha3)=32),"
            "PRIMARY KEY (txid, vout_n)) WITHOUT ROWID");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_op_return_index_height "
            "ON op_return_index(height)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_op_return_index_tag "
            "ON op_return_index(tag_text, height)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('031')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 31);
        current_ver = 31;
        applied++;
    }

    if (current_ver < 32) {
        /* v32: ZSLP per-(token, outpoint) ledger (zslp_ledger) — the
         * debit-correct token-balance projection that finally makes token
         * balances fully chain-derived (the credit-only zslp_balances is
         * left untouched). One row per token-bearing transaction output (an
         * SLP UTXO): created by GENESIS/MINT/SEND, marked spent when a later
         * tx consumes the outpoint (the always-on tx_inputs spend graph). A
         * holder's balance = SUM(amount) over their UNSPENT rows. See
         * models/zslp_ledger.h. Not consulted by consensus; rebuilt from the
         * zslp_transfers / tx_outputs / tx_inputs projections
         * (zslp_ledger_truncate). token_id is internal (node) byte order. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zslp_ledger ("
            "token_id BLOB NOT NULL CHECK(length(token_id)=32),"
            "txid BLOB NOT NULL CHECK(length(txid)=32),"
            "vout INTEGER NOT NULL,"
            "amount INTEGER NOT NULL CHECK(amount>=0),"
            "address BLOB CHECK(address IS NULL OR length(address)=20),"
            "created_height INTEGER NOT NULL,"
            "spent_by_txid BLOB "
            "  CHECK(spent_by_txid IS NULL OR length(spent_by_txid)=32),"
            "spent_height INTEGER,"
            "PRIMARY KEY (token_id, txid, vout)) WITHOUT ROWID");

        /* Reconciliation surface: SUM(amount) over a holder's unspent rows. */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_ledger_addr "
            "ON zslp_ledger(token_id, address) WHERE spent_by_txid IS NULL");

        /* Spend-marking lookup by consumed outpoint (across tokens). */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_ledger_outpoint "
            "ON zslp_ledger(txid, vout)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('032')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 32);
        current_ver = 32;
        applied++;
    }

    if (current_ver < 33) {
        /* v33: parity_samples — bounded, retained history of the mirror's
         * per-tick consensus-parity comparison against the co-located
         * zclassicd oracle (models/parity_sample.h). One row per
         * legacy_mirror_sync comparison outcome. Purely observational —
         * never consulted by consensus. Bounded retention: the mirror
         * prunes to the newest N rows. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS parity_samples ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ts INTEGER NOT NULL,"
            "our_height INTEGER NOT NULL DEFAULT -1,"
            "oracle_height INTEGER NOT NULL DEFAULT -1,"
            "heights_equal_at INTEGER NOT NULL DEFAULT -1,"
            "hash_equal INTEGER NOT NULL DEFAULT 0 CHECK(hash_equal IN (0,1)),"
            "oracle_reachable INTEGER NOT NULL DEFAULT 0 "
            "  CHECK(oracle_reachable IN (0,1)))");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_parity_samples_ts "
            "ON parity_samples(ts DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('033')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 33);
        current_ver = 33;
        applied++;
    }

    if (current_ver < 34) {
        /* v34: block_index_cache integrity envelope — one row (envelope_id=1)
         * carrying an XOR-combined SHA3-256 over every block_index_cache row
         * + its row_count, verified at load. See
         * services/block_index_cache_envelope.h and docs/work/FORWARD_PLAN.md
         * item 7.3. Appended here (not next to block_index_cache's v4 DDL in
         * database_migrate.c) because this is the next free schema slot. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS block_index_cache_envelope ("
            "envelope_id INTEGER PRIMARY KEY CHECK(envelope_id=1),"
            "row_count INTEGER NOT NULL,"
            "content_sha3 BLOB NOT NULL CHECK(length(content_sha3)=32),"
            "written_at INTEGER NOT NULL)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('034')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 34);
        current_ver = 34;
        applied++;
    }

    *version = current_ver;
    return applied;
}
