/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Prove the build-fabric v41 schema, AR writes, and relationships. */

#include "test/test_core.h"

#include "models/build_fabric.h"
#include "models/database.h"
#include "services/build_fabric_service.h"
#include "base/hex.h"
#include "crypto/ed25519.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static const char id_a[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char id_b[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char id_c[] =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char id_d[] =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

static bool bf_open(struct node_db *ndb, char *dir, size_t dir_cap,
                    char *path, size_t path_cap, const char *tag)
{
    test_make_tmpdir(dir, dir_cap, "build_fabric", tag);
    (void)snprintf(path, path_cap, "%s/node.db", dir);
    memset(ndb, 0, sizeof(*ndb));
    return node_db_open(ndb, path);
}

static void bf_job(struct db_build_job *row)
{
    memset(row, 0, sizeof(*row));
    (void)snprintf(row->job_id, sizeof(row->job_id), "%s", id_a);
    (void)snprintf(row->source_sha256, sizeof(row->source_sha256), "%s", id_b);
    (void)snprintf(row->source_cas_sha3, sizeof(row->source_cas_sha3), "%s", id_c);
    (void)snprintf(row->toolchain_sha3, sizeof(row->toolchain_sha3), "%s", id_d);
    (void)snprintf(row->profile, sizeof(row->profile), "dev-x86-64-v3");
    (void)snprintf(row->state, sizeof(row->state), "PLANNED");
    row->created_at = 100;
    row->updated_at = 100;
}

static void bf_action(struct db_build_action *row)
{
    memset(row, 0, sizeof(*row));
    (void)snprintf(row->action_id, sizeof(row->action_id), "%s", id_b);
    (void)snprintf(row->job_id, sizeof(row->job_id), "%s", id_a);
    row->sequence = 0;
    (void)snprintf(row->kind, sizeof(row->kind),
                   "c23.compile.preprocessed.v1");
    (void)snprintf(row->state, sizeof(row->state), "SNAPSHOTTED");
    (void)snprintf(row->input_root_sha3, sizeof(row->input_root_sha3), "%s",
                   id_c);
    row->created_at = 101;
    row->updated_at = 101;
}

static void bf_worker(struct db_build_worker *row)
{
    memset(row, 0, sizeof(*row));
    (void)snprintf(row->worker_id, sizeof(row->worker_id), "%s", id_c);
    (void)snprintf(row->signer_pubkey, sizeof(row->signer_pubkey), "%s", id_d);
    (void)snprintf(row->capabilities, sizeof(row->capabilities),
                   "linux,x86-64-v3,gcc,c23.compile.preprocessed.v1");
    row->approved = 1;
    row->approved_at = 102;
    row->last_seen_at = 102;
}

static void bf_receipt(struct db_build_receipt *row)
{
    memset(row, 0, sizeof(*row));
    (void)snprintf(row->receipt_id, sizeof(row->receipt_id), "%s", id_d);
    (void)snprintf(row->action_id, sizeof(row->action_id), "%s", id_b);
    (void)snprintf(row->job_id, sizeof(row->job_id), "%s", id_a);
    (void)snprintf(row->worker_id, sizeof(row->worker_id), "%s", id_c);
    (void)snprintf(row->action_sha3, sizeof(row->action_sha3), "%s", id_b);
    (void)snprintf(row->output_sha3, sizeof(row->output_sha3), "%s", id_c);
    memset(row->signature, 'e', BUILD_FABRIC_SIGNATURE_HEX);
    row->signature[BUILD_FABRIC_SIGNATURE_HEX] = '\0';
    (void)snprintf(row->confinement, sizeof(row->confinement),
                   "landlock=1,seccomp=1,network=0");
    row->exit_status = 0;
    row->created_at = 103;
}

static int test_bf_migration(void)
{
    int failures = 0;
    TEST("build_fabric: v41 migration creates every indexed resource") {
        struct node_db ndb;
        char dir[256], path[320];
        ASSERT(bf_open(&ndb, dir, sizeof(dir), path, sizeof(path), "migration"));
        ASSERT_EQ(node_db_schema_version(&ndb), NODE_DB_MAX_SCHEMA);
        sqlite3_stmt *st = NULL;
        ASSERT(sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
            "name IN ('build_jobs','build_actions','build_workers','build_receipts')",
            -1, &st, NULL) == SQLITE_OK);
        ASSERT(sqlite3_step(st) == SQLITE_ROW); /* raw-sql-ok:test-readonly-count */
        ASSERT_EQ(sqlite3_column_int(st, 0), 4);
        sqlite3_finalize(st);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_lifecycle(void)
{
    int failures = 0;
    TEST("build_fabric: AR lifecycle preserves indexed relationships") {
        struct node_db ndb;
        char dir[256], path[320];
        ASSERT(bf_open(&ndb, dir, sizeof(dir), path, sizeof(path), "lifecycle"));
        struct db_build_job job;
        struct db_build_action action;
        struct db_build_worker worker;
        struct db_build_receipt receipt;
        bf_job(&job);
        bf_action(&action);
        bf_worker(&worker);
        bf_receipt(&receipt);
        ASSERT(db_build_job_save(&ndb, &job));
        ASSERT(db_build_action_save(&ndb, &action));
        ASSERT(db_build_worker_save(&ndb, &worker));
        ASSERT(db_build_receipt_save(&ndb, &receipt));

        struct db_build_action actions[4];
        struct db_build_receipt receipts[4];
        struct db_build_worker workers[4];
        ASSERT_EQ(db_build_job_actions(&ndb, id_a, actions, 4), 1);
        ASSERT_STR_EQ(actions[0].action_id, id_b);
        ASSERT_EQ(db_build_job_receipts(&ndb, id_a, receipts, 4), 1);
        ASSERT_STR_EQ(receipts[0].worker_id, id_c);
        ASSERT_EQ(db_build_workers_list(&ndb, workers, 4), 1);
        ASSERT(workers[0].approved == 1 && workers[0].revoked == 0);

        /* Updating the parent is an UPSERT, not INSERT OR REPLACE: children
         * and receipts must survive the state transition. */
        (void)snprintf(job.state, sizeof(job.state), "QUEUED");
        job.updated_at = 104;
        ASSERT(db_build_job_save(&ndb, &job));
        ASSERT_EQ(db_build_job_actions(&ndb, id_a, actions, 4), 1);
        ASSERT_EQ(db_build_job_receipts(&ndb, id_a, receipts, 4), 1);

        (void)snprintf(action.state, sizeof(action.state), "ACCEPTED");
        (void)snprintf(action.outcome, sizeof(action.outcome), "ACCEPTED");
        (void)snprintf(action.output_root_sha3,
                       sizeof(action.output_root_sha3), "%s", id_d);
        action.updated_at = 105;
        ASSERT(db_build_action_save(&ndb, &action));
        ASSERT(db_build_action_find(&ndb, id_b, &action));
        ASSERT_STR_EQ(action.state, "ACCEPTED");
        ASSERT_EQ(db_build_job_receipts(&ndb, id_a, receipts, 4), 1);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_validation(void)
{
    int failures = 0;
    TEST("build_fabric: malformed ids and unnamed states fail validation") {
        struct ar_errors errors;
        struct db_build_job job;
        bf_job(&job);
        ASSERT(db_build_job_validate(&job, &errors));
        (void)snprintf(job.state, sizeof(job.state), "MAYBE");
        ASSERT(!db_build_job_validate(&job, &errors));
        bf_job(&job);
        (void)snprintf(job.source_cas_sha3, sizeof(job.source_cas_sha3), "abc");
        ASSERT(!db_build_job_validate(&job, &errors));
        struct db_build_worker worker;
        bf_worker(&worker);
        worker.revoked = 2;
        ASSERT(!db_build_worker_validate(&worker, &errors));
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_service(void)
{
    int failures = 0;
    TEST("build_fabric: service gates transitions and verifies signed receipts") {
        struct node_db ndb;
        char dir[256], path[320];
        ASSERT(bf_open(&ndb, dir, sizeof(dir), path, sizeof(path), "service"));
        struct db_build_job job;
        struct db_build_action action;
        bf_job(&job);
        bf_action(&action);
        struct db_build_job planned_job = job;
        struct db_build_action planned_action = action;
        ASSERT(build_fabric_plan(&ndb, &job, &action).ok);
        ASSERT(build_fabric_plan(&ndb, &job, &action).ok); /* idempotent */
        ASSERT(build_fabric_submit(&ndb, id_a, 110).ok);
        ASSERT(build_fabric_submit(&ndb, id_a, 111).ok); /* idempotent */
        ASSERT(build_fabric_plan(&ndb, &planned_job, &planned_action).ok);
        ASSERT(db_build_action_find(&ndb, id_b, &action));
        ASSERT_STR_EQ(action.state, "QUEUED");
        ASSERT(db_build_job_find(&ndb, id_a, &job));
        ASSERT_STR_EQ(job.state, "QUEUED");

        uint8_t seed[32], pubkey[32], secret[32];
        memset(seed, 7, sizeof(seed));
        ed25519_keypair(pubkey, secret, seed);
        struct db_build_worker worker;
        bf_worker(&worker);
        zcl_hex_encode(pubkey, sizeof(pubkey), worker.signer_pubkey);
        ASSERT(build_fabric_worker_approve(&ndb, &worker, 112).ok);

        (void)snprintf(action.state, sizeof(action.state), "VERIFYING");
        action.updated_at = 113;
        ASSERT(db_build_action_save(&ndb, &action));
        struct db_build_receipt receipt;
        bf_receipt(&receipt);
        ASSERT(build_fabric_receipt_id(&receipt, receipt.receipt_id).ok);
        uint8_t receipt_id[32], signature[64];
        ASSERT(zcl_hex_decode_lower(receipt.receipt_id, receipt_id,
                                    sizeof(receipt_id)));
        ed25519_sign(signature, receipt_id, sizeof(receipt_id), secret, pubkey);
        zcl_hex_encode(signature, sizeof(signature), receipt.signature);
        ASSERT(build_fabric_receipt_accept(&ndb, &receipt, 114).ok);
        ASSERT(db_build_action_find(&ndb, id_b, &action));
        ASSERT_STR_EQ(action.state, "ACCEPTED");
        ASSERT_STR_EQ(action.output_root_sha3, id_c);

        /* Revocation is durable and makes a newly bound receipt fail before
         * signature acceptance; old evidence remains queryable. */
        ASSERT(build_fabric_worker_revoke(&ndb, id_c, 115).ok);
        ASSERT(build_fabric_worker_revoke(&ndb, id_c, 116).ok);
        receipt.created_at = 117;
        ASSERT(build_fabric_receipt_id(&receipt, receipt.receipt_id).ok);
        ASSERT(!build_fabric_receipt_accept(&ndb, &receipt, 117).ok);
        struct db_build_receipt rows[2];
        ASSERT_EQ(db_build_job_receipts(&ndb, id_a, rows, 2), 1);
        ASSERT(!build_fabric_cancel(&ndb, id_a, 118).ok);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

int test_build_fabric(void)
{
    int failures = 0;
    failures += test_bf_migration();
    failures += test_bf_lifecycle();
    failures += test_bf_validation();
    failures += test_bf_service();
    printf("=== build_fabric: %d failures ===\n", failures);
    return failures;
}
