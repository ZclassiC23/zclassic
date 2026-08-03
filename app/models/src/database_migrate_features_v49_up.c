/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * App-feature schema migrations v49+ for node.db — continuation of
 * database_migrate_features_v30_up.c (E1 file-size split; same idempotent
 * versioned-block pattern documented at the top of
 * database_migrate_features.c). node_db_migrate_features_v30_up() hands
 * off here at the v49 boundary via node_db_migrate_features_v49_up().
 *
 * ar-validate-skip:connection-handle-not-a-row
 *   Same rationale as database_migrate_features.c: operates on the
 *   struct node_db connection handle + schema_migrations bookkeeping,
 *   never a row record. */

#include "models/database.h"
#include "models/database_internal.h"

int node_db_migrate_features_v49_up(struct node_db *ndb, int *version)
{
    int applied = 0;
    int current_ver = *version;

    if (current_ver < 49) {
        /* v49: ZCODE science — the durable plan/commit idempotency ledger
         * (zcode_science_plans: exact wire + request identity + expiry +
         * result root per write) and the six rebuildable projections of the
         * canonical science CAS objects (study_spec.v1, benchmark_result.v2,
         * reproduction.v1, science_findings.v1, curation_vote.v1,
         * review.v1). Projection columns are lookup keys only; the addressed
         * wires under .zvcs/objects stay the authority and the projections
         * may be dropped and rebuilt at any time. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_plans ("
            "plan_root TEXT PRIMARY KEY CHECK(length(plan_root)=64),"
            "kind TEXT NOT NULL CHECK(kind IN ('study','work','review','vote')),"
            "request_hash TEXT NOT NULL UNIQUE CHECK(length(request_hash)=64),"
            "wire_hex TEXT NOT NULL,"
            "result_root TEXT NOT NULL CHECK(length(result_root) IN (0,64)),"
            "state TEXT NOT NULL CHECK(state IN ('PLANNED','COMMITTED')),"
            "expires_unix INTEGER NOT NULL CHECK(expires_unix>0),"
            "created_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_studies ("
            "root TEXT PRIMARY KEY CHECK(length(root)=64),"
            "study_root TEXT NOT NULL,"
            "link_root TEXT NOT NULL,"
            "aux_root TEXT NOT NULL,"
            "author TEXT NOT NULL,"
            "code INTEGER NOT NULL,"
            "flags INTEGER NOT NULL,"
            "sequence INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "expires_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_results ("
            "root TEXT PRIMARY KEY CHECK(length(root)=64),"
            "study_root TEXT NOT NULL,"
            "link_root TEXT NOT NULL,"
            "aux_root TEXT NOT NULL,"
            "author TEXT NOT NULL,"
            "code INTEGER NOT NULL,"
            "flags INTEGER NOT NULL,"
            "sequence INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "expires_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zcode_science_results_study "
            "ON zcode_science_results(study_root)");
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_reproductions ("
            "root TEXT PRIMARY KEY CHECK(length(root)=64),"
            "study_root TEXT NOT NULL,"
            "link_root TEXT NOT NULL,"
            "aux_root TEXT NOT NULL,"
            "author TEXT NOT NULL,"
            "code INTEGER NOT NULL,"
            "flags INTEGER NOT NULL,"
            "sequence INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "expires_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zcode_science_repros_study "
            "ON zcode_science_reproductions(study_root)");
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_findings ("
            "root TEXT PRIMARY KEY CHECK(length(root)=64),"
            "study_root TEXT NOT NULL,"
            "link_root TEXT NOT NULL,"
            "aux_root TEXT NOT NULL,"
            "author TEXT NOT NULL,"
            "code INTEGER NOT NULL,"
            "flags INTEGER NOT NULL,"
            "sequence INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "expires_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zcode_science_findings_study "
            "ON zcode_science_findings(study_root)");
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_votes ("
            "root TEXT PRIMARY KEY CHECK(length(root)=64),"
            "study_root TEXT NOT NULL,"
            "link_root TEXT NOT NULL,"
            "aux_root TEXT NOT NULL,"
            "author TEXT NOT NULL,"
            "code INTEGER NOT NULL,"
            "flags INTEGER NOT NULL,"
            "sequence INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "expires_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_zcode_science_votes_replay "
            "ON zcode_science_votes(author,sequence)");
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_reviews ("
            "root TEXT PRIMARY KEY CHECK(length(root)=64),"
            "study_root TEXT NOT NULL,"
            "link_root TEXT NOT NULL,"
            "aux_root TEXT NOT NULL,"
            "author TEXT NOT NULL,"
            "code INTEGER NOT NULL,"
            "flags INTEGER NOT NULL,"
            "sequence INTEGER NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "expires_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('049')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 49);
        current_ver = 49;
        applied++;
    }

    if (current_ver < 50) {
        /* v50: yardsale wallet glue — the durable plan/commit idempotency
         * ledger (yardsale_plans) behind yardsale.seller.arm and
         * yardsale.buy: request identity + the exact planned terms
         * (canonical accept-data serialization + sign root, never key
         * material) + expiry + state per wallet-touching write. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS yardsale_plans ("
            "plan_root TEXT PRIMARY KEY CHECK(length(plan_root)=64),"
            "kind TEXT NOT NULL CHECK(kind IN ('arm','buy')),"
            "request_hash TEXT NOT NULL UNIQUE CHECK(length(request_hash)=64),"
            "payload_hex TEXT NOT NULL,"
            "result TEXT NOT NULL,"
            "state TEXT NOT NULL CHECK(state IN ('PLANNED','COMMITTED','EXPIRED')),"
            "expires_unix INTEGER NOT NULL CHECK(expires_unix>0),"
            "created_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('050')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 50);
        current_ver = 50;
        applied++;
    }

    *version = current_ver;
    return applied;
}
