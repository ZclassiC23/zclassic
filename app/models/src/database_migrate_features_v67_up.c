/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * App-feature schema migrations v67+, split at the E1 file-size ceiling.
 * ar-validate-skip:connection-handle-not-a-row */

#include "models/database.h"
#include "models/database_internal.h"

int node_db_migrate_features_v67_up(struct node_db *ndb, int *version)
{
    int applied = 0;
    int current_ver = *version;
    if (current_ver < 67) {
        /* v67: signed seller claims bound to a want, direct artifact SHA3,
         * content.v2 CAS root, and optional node-verifiable receipts. This
         * is evidence only: no award, escrow, ZCL movement, or ZC23 issuance.
         * review_state and withdrawn_unix are local-only projection state. */
        if (!node_db_exec(ndb,
                "CREATE TABLE IF NOT EXISTS shop_fulfills("
                "fulfill_id BLOB PRIMARY KEY CHECK(length(fulfill_id)=32),"
                "want_id BLOB NOT NULL CHECK(length(want_id)=32),"
                "wire BLOB NOT NULL CHECK(length(wire)=322),"
                "seller_pubkey BLOB NOT NULL CHECK(length(seller_pubkey)=32),"
                "nonce INTEGER NOT NULL CHECK(nonce>0),"
                "artifact_root BLOB NOT NULL CHECK(length(artifact_root)=32),"
                "content_root BLOB NOT NULL CHECK(length(content_root)=32),"
                "build_receipt_id BLOB NOT NULL "
                "CHECK(length(build_receipt_id)=32),"
                "fuzz_receipt_id BLOB NOT NULL "
                "CHECK(length(fuzz_receipt_id)=32),"
                "bench_receipt_id BLOB NOT NULL "
                "CHECK(length(bench_receipt_id)=32),"
                "issued_unix INTEGER NOT NULL CHECK(issued_unix>0),"
                "expires_unix INTEGER NOT NULL CHECK(expires_unix>issued_unix),"
                "review_state TEXT NOT NULL DEFAULT 'unreviewed' "
                "CHECK(review_state IN "
                "('unreviewed','reviewed_ok','sensitive')),"
                "withdrawn_unix INTEGER NOT NULL DEFAULT 0 "
                "CHECK(withdrawn_unix>=0),"
                "posted_unix INTEGER NOT NULL CHECK(posted_unix>0))"))
            LOG_ERR("db", "migrate v67: shop_fulfills table failed");
        if (!node_db_exec(ndb,
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "idx_shop_fulfills_seller_nonce "
                "ON shop_fulfills(seller_pubkey,nonce)"))
            LOG_ERR("db", "migrate v67: seller nonce index failed");
        if (!node_db_exec(ndb,
                "CREATE INDEX IF NOT EXISTS idx_shop_fulfills_want "
                "ON shop_fulfills(want_id,posted_unix DESC)"))
            LOG_ERR("db", "migrate v67: want index failed");
        if (!node_db_exec(ndb,
                "CREATE INDEX IF NOT EXISTS idx_shop_fulfills_open "
                "ON shop_fulfills(want_id,expires_unix,withdrawn_unix)"))
            LOG_ERR("db", "migrate v67: open index failed");
        if (!node_db_exec(ndb,
                "INSERT OR IGNORE INTO schema_migrations(version) "
                "VALUES('067')"))
            LOG_ERR("db", "migrate v67: migration stamp failed");
        DB_MIGRATE_PERSIST_VERSION(ndb, 67);
        current_ver = 67;
        applied++;
    }
    *version = current_ver;
    return applied;
}
