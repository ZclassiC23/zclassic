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
    if (current_ver < 68) {
        /* v68: append-only requester-local async proof events. Existing
         * build_actions remain the exact work identity; signed work_receipts
         * remain evidence authority. This table is a rebuildable, root-bound
         * lifecycle projection and grants neither acceptance nor publication. */
        if (!node_db_exec(ndb,
                "CREATE TABLE IF NOT EXISTS build_proof_events("
                "event_root TEXT NOT NULL UNIQUE CHECK(length(event_root)=64),"
                "prior_event_root TEXT NOT NULL DEFAULT '' "
                "CHECK(length(prior_event_root) IN (0,64)),"
                "action_id TEXT NOT NULL REFERENCES build_actions(action_id) "
                "ON DELETE CASCADE CHECK(length(action_id)=64),"
                "task_root_sha3 TEXT NOT NULL CHECK(length(task_root_sha3)=64),"
                "candidate_root_sha3 TEXT NOT NULL "
                "CHECK(length(candidate_root_sha3)=64),"
                "proof_policy_root_sha3 TEXT NOT NULL "
                "CHECK(length(proof_policy_root_sha3)=64),"
                "context_root_sha3 TEXT NOT NULL DEFAULT '' "
                "CHECK(length(context_root_sha3) IN (0,64)),"
                "receipt_root_sha3 TEXT NOT NULL DEFAULT '' "
                "CHECK(length(receipt_root_sha3) IN (0,64)),"
                "workspace TEXT NOT NULL CHECK(length(workspace) BETWEEN 1 AND 4095),"
                "state TEXT NOT NULL CHECK(state IN ('REQUESTED',"
                "'PEER_DISCOVERED','RUNNING','REMOTE_GREEN','REMOTE_RED',"
                "'RECEIPT_VERIFIED','REPRODUCED','SUPERSEDED',"
                "'READY_FOR_ACCEPTANCE')),"
                "peer_id INTEGER NOT NULL CHECK(peer_id>=0),"
                "request_id BLOB NOT NULL CHECK(length(request_id)=8),"
                "deadline_at INTEGER NOT NULL CHECK(deadline_at>=0),"
                "elapsed_us INTEGER NOT NULL CHECK(elapsed_us>=0),"
                "created_at INTEGER NOT NULL CHECK(created_at>0))"))
            LOG_ERR("db", "migrate v68: build_proof_events table failed");
        if (!node_db_exec(ndb,
                "CREATE INDEX IF NOT EXISTS idx_build_proof_events_action "
                "ON build_proof_events(action_id,created_at,event_root)"))
            LOG_ERR("db", "migrate v68: action event index failed");
        if (!node_db_exec(ndb,
                "CREATE INDEX IF NOT EXISTS idx_build_proof_events_task "
                "ON build_proof_events(task_root_sha3,created_at)"))
            LOG_ERR("db", "migrate v68: task event index failed");
        if (!node_db_exec(ndb,
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "idx_build_proof_events_one_successor "
                "ON build_proof_events(prior_event_root) "
                "WHERE prior_event_root<>''"))
            LOG_ERR("db", "migrate v68: event chain index failed");
        if (!node_db_exec(ndb,
                "INSERT OR IGNORE INTO schema_migrations(version) "
                "VALUES('068')"))
            LOG_ERR("db", "migrate v68: migration stamp failed");
        DB_MIGRATE_PERSIST_VERSION(ndb, 68);
        current_ver = 68;
        applied++;
    }
    *version = current_ver;
    return applied;
}
