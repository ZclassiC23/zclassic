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

    if (current_ver < 51) {
        /* v51: ZCODE science findings admission (G4) — extend the plan
         * ledger's kind CHECK with 'findings' for
         * zcode.science.findings.plan|commit. SQLite cannot ALTER a CHECK
         * constraint, so the table is rebuilt and rows carry over. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zcode_science_plans_v51 ("
            "plan_root TEXT PRIMARY KEY CHECK(length(plan_root)=64),"
            "kind TEXT NOT NULL CHECK(kind IN ('study','work','findings','review','vote')),"
            "request_hash TEXT NOT NULL UNIQUE CHECK(length(request_hash)=64),"
            "wire_hex TEXT NOT NULL,"
            "result_root TEXT NOT NULL CHECK(length(result_root) IN (0,64)),"
            "state TEXT NOT NULL CHECK(state IN ('PLANNED','COMMITTED')),"
            "expires_unix INTEGER NOT NULL CHECK(expires_unix>0),"
            "created_at INTEGER NOT NULL)");
        node_db_exec(ndb,
            "INSERT INTO zcode_science_plans_v51 "
            "SELECT * FROM zcode_science_plans");
        node_db_exec(ndb, "DROP TABLE zcode_science_plans");
        node_db_exec(ndb,
            "ALTER TABLE zcode_science_plans_v51 "
            "RENAME TO zcode_science_plans");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('051')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 51);
        current_ver = 51;
        applied++;
    }

    if (current_ver < 52) {
        /* v52: stable wallet identity and fail-closed agent-session binding.
         * Existing sessions receive empty binding columns deliberately: they
         * remain listable/revocable, but no money authorization can treat an
         * unbound legacy row as belonging to the current wallet. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS wallet_identity ("
            "id INTEGER PRIMARY KEY CHECK(id=1),"
            "wallet_instance_id TEXT NOT NULL UNIQUE "
            " CHECK(length(wallet_instance_id)=32),"
            "network_genesis BLOB NOT NULL CHECK(length(network_genesis)=32),"
            "operator_lane TEXT NOT NULL CHECK(length(operator_lane)>0),"
            "created_at INTEGER NOT NULL CHECK(created_at>0))");
        node_db_exec(ndb,
            "ALTER TABLE agent_sessions ADD COLUMN wallet_scope TEXT NOT NULL "
            "DEFAULT '' CHECK(wallet_scope IN ('','dev','prod'))");
        node_db_exec(ndb,
            "ALTER TABLE agent_sessions ADD COLUMN wallet_instance_id TEXT "
            "NOT NULL DEFAULT '' CHECK(length(wallet_instance_id) IN (0,32))");
        node_db_exec(ndb,
            "ALTER TABLE agent_sessions ADD COLUMN wallet_genesis TEXT NOT NULL "
            "DEFAULT '' CHECK(length(wallet_genesis) IN (0,64))");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_agent_sessions_wallet "
            "ON agent_sessions(wallet_scope,wallet_instance_id,revoked)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('052')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 52);
        current_ver = 52;
        applied++;
    }

    if (current_ver < 53) {
        /* v53: durable, scope-wide agent allocation accounting. Unlike the
         * rolling rate window this counter never resets; it lets the dev
         * custody floor enforce a lifetime 0.05-ZCL lab allocation across
         * concurrent sessions. A failed pre-broadcast handler releases it. */
        node_db_exec(ndb,
            "ALTER TABLE agent_sessions ADD COLUMN lifetime_spent_zat INTEGER "
            "NOT NULL DEFAULT 0 CHECK(lifetime_spent_zat>=0 AND "
            "lifetime_spent_zat<=2100000000000000)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_agent_sessions_scope_lifetime "
            "ON agent_sessions(wallet_scope,lifetime_spent_zat)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('053')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 53);
        current_ver = 53;
        applied++;
    }

    if (current_ver < 54) {
        /* v54: bind durable transaction intents to the exact custody
         * snapshot and reserve recipient value plus maximum fee. Empty
         * binding fields mark legacy owner plans; agent money paths never
         * infer an identity for them. */
        node_db_exec(ndb,
            "ALTER TABLE vault_intents ADD COLUMN wallet_scope TEXT NOT NULL "
            "DEFAULT '' CHECK(wallet_scope IN ('','dev','prod'))");
        node_db_exec(ndb,
            "ALTER TABLE vault_intents ADD COLUMN wallet_instance_id TEXT "
            "NOT NULL DEFAULT '' CHECK(length(wallet_instance_id) IN (0,32))");
        node_db_exec(ndb,
            "ALTER TABLE vault_intents ADD COLUMN wallet_genesis TEXT NOT NULL "
            "DEFAULT '' CHECK(length(wallet_genesis) IN (0,64))");
        node_db_exec(ndb,
            "ALTER TABLE vault_intents ADD COLUMN snapshot_root BLOB "
            "CHECK(snapshot_root IS NULL OR length(snapshot_root)=32)");
        node_db_exec(ndb,
            "ALTER TABLE vault_intents ADD COLUMN recipient_value_zat INTEGER "
            "NOT NULL DEFAULT 0 CHECK(recipient_value_zat>=0)");
        node_db_exec(ndb,
            "ALTER TABLE vault_intents ADD COLUMN max_fee_zat INTEGER NOT NULL "
            "DEFAULT 0 CHECK(max_fee_zat>=0)");
        node_db_exec(ndb,
            "ALTER TABLE vault_intents ADD COLUMN reserved_zat INTEGER NOT NULL "
            "DEFAULT 0 CHECK(reserved_zat>=0)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_vault_intents_wallet_reserve "
            "ON vault_intents(wallet_scope,wallet_instance_id,state)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('054')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 54);
        current_ver = 54;
        applied++;
    }

    *version = current_ver;
    return applied;
}
