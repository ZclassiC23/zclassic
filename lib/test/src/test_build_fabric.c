/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Prove the build-fabric v42 schema, leases, and relationships. */

#include "test/test_core.h"

#include "models/build_fabric.h"
#include "models/database.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_runtime.h"
#include "services/build_fabric_worker.h"
#include "base/hex.h"
#include "crypto/ed25519.h"
#include "platform/time_compat.h"
#include "command/native_command.h"
#include "controllers/api_controller.h"
#include "json/json.h"
#include "vcs/build_action.h"
#include "vcs/build_artifact_manifest.h"
#include "vcs/package_store.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_dev.h"
#include "crypto/sha3.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    (void)snprintf(row->target, sizeof(row->target), "linux-x86_64-v3");
    (void)snprintf(row->flags_sha3, sizeof(row->flags_sha3), "%s", id_d);
    (void)snprintf(row->environment_sha3, sizeof(row->environment_sha3), "%s",
                   id_a);
    (void)snprintf(row->virtual_workdir, sizeof(row->virtual_workdir),
                   "/zbuild/src");
    (void)snprintf(row->declared_outputs, sizeof(row->declared_outputs),
                   "unit.o");
    (void)snprintf(row->resource_policy, sizeof(row->resource_policy),
                   "cpu=1,memory_mb=2048,timeout_s=120,network=0");
    row->created_at = 101;
    row->updated_at = 101;
}

static void bf_worker(struct db_build_worker *row)
{
    memset(row, 0, sizeof(*row));
    (void)snprintf(row->worker_id, sizeof(row->worker_id), "%s", id_c);
    (void)snprintf(row->signer_pubkey, sizeof(row->signer_pubkey), "%s", id_d);
    (void)snprintf(row->capabilities, sizeof(row->capabilities),
                   "linux,x86-64-v3,gcc,%s,%s", VCS_BUILD_ACTION_KIND_V1,
                   VCS_BUILD_ACTION_KIND_TEST_V1);
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
    (void)snprintf(row->lease_id, sizeof(row->lease_id), "%s", id_d);
    (void)snprintf(row->action_sha3, sizeof(row->action_sha3), "%s", id_b);
    (void)snprintf(row->output_sha3, sizeof(row->output_sha3), "%s", id_c);
    memset(row->signature, 'e', BUILD_FABRIC_SIGNATURE_HEX);
    row->signature[BUILD_FABRIC_SIGNATURE_HEX] = '\0';
    (void)snprintf(row->confinement, sizeof(row->confinement),
                   "landlock=1,seccomp=1,network=0");
    (void)snprintf(row->trust_state, sizeof(row->trust_state),
                   "LOCAL_ACCEPTED");
    row->exit_status = 0;
    row->created_at = 103;
}

static bool bf_canonicalize(struct db_build_job *job,
                            struct db_build_action *action)
{
    char action_id[BUILD_FABRIC_ID_HEX + 1];
    char job_id[BUILD_FABRIC_ID_HEX + 1];
    if (!build_fabric_action_id(job, action, action_id).ok ||
        !build_fabric_job_id(job, action_id, job_id).ok)
        return false;
    (void)snprintf(action->action_id, sizeof(action->action_id), "%s",
                   action_id);
    (void)snprintf(job->job_id, sizeof(job->job_id), "%s", job_id);
    (void)snprintf(action->job_id, sizeof(action->job_id), "%s", job_id);
    return true;
}

static uint8_t *bf_read_fixture(const char *path, size_t *len_out)
{
    *len_out = 0;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 1024 * 1024)
        return NULL;
    uint8_t *bytes = zcl_malloc((size_t)st.st_size, "test.build_fixture");
    if (!bytes) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { free(bytes); return NULL; }
    size_t got = fread(bytes, 1, (size_t)st.st_size, f);
    bool read_ok = got == (size_t)st.st_size && ferror(f) == 0;
    bool ok = fclose(f) == 0 && read_ok;
    if (!ok) { free(bytes); return NULL; }
    *len_out = got;
    return bytes;
}

static int test_bf_migration(void)
{
    int failures = 0;
    TEST("build_fabric: v45 migration creates worker trust and lane index") {
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
        ASSERT(sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM pragma_table_info('build_actions') WHERE "
            "name IN ('lease_id','lease_expires_at','lease_heartbeat_at',"
            "'attempt_count','claimed_at','started_at','finished_at')",
            -1, &st, NULL) == SQLITE_OK);
        ASSERT(sqlite3_step(st) == SQLITE_ROW); /* raw-sql-ok:test-readonly-count */
        ASSERT_EQ(sqlite3_column_int(st, 0), 7);
        sqlite3_finalize(st);
        ASSERT(sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM pragma_table_info('build_receipts') "
            "WHERE name='lease_id'", -1, &st, NULL) == SQLITE_OK);
        ASSERT(sqlite3_step(st) == SQLITE_ROW); /* raw-sql-ok:test-readonly-count */
        ASSERT_EQ(sqlite3_column_int(st, 0), 1);
        sqlite3_finalize(st);
        ASSERT(sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
            "name='zcode_lane_receipts'", -1, &st, NULL) == SQLITE_OK);
        ASSERT(sqlite3_step(st) == SQLITE_ROW); /* raw-sql-ok:test-readonly-count */
        ASSERT_EQ(sqlite3_column_int(st, 0), 1);
        sqlite3_finalize(st);
        ASSERT(sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM sqlite_master WHERE type='index' AND "
            "name='idx_zcode_lane_source'", -1, &st, NULL) == SQLITE_OK);
        ASSERT(sqlite3_step(st) == SQLITE_ROW); /* raw-sql-ok:test-readonly-count */
        ASSERT_EQ(sqlite3_column_int(st, 0), 1);
        sqlite3_finalize(st);
        ASSERT(sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM pragma_table_info('build_receipts') "
            "WHERE name='trust_state'", -1, &st, NULL) == SQLITE_OK);
        ASSERT(sqlite3_step(st) == SQLITE_ROW); /* raw-sql-ok:test-readonly-count */
        ASSERT_EQ(sqlite3_column_int(st, 0), 1);
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
        (void)snprintf(action.worker_id, sizeof(action.worker_id), "%s", id_c);
        (void)snprintf(action.lease_id, sizeof(action.lease_id), "%s", id_b);
        action.lease_expires_at = 500;
        action.lease_heartbeat_at = 450;
        action.attempt_count = 2;
        action.claimed_at = 400;
        action.started_at = 410;
        action.finished_at = 490;
        action.updated_at = 105;
        ASSERT(db_build_action_save(&ndb, &action));
        ASSERT(db_build_action_find(&ndb, id_b, &action));
        ASSERT_STR_EQ(action.state, "ACCEPTED");
        ASSERT_STR_EQ(action.lease_id, id_b);
        ASSERT_EQ(action.attempt_count, 2);
        ASSERT_EQ(action.finished_at, 490);
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
        ASSERT(bf_canonicalize(&job, &action));
        char action_id[65], job_id[65];
        ASSERT(build_fabric_action_id(&job, &action, action_id).ok);
        ASSERT(build_fabric_job_id(&job, action_id, job_id).ok);
        ASSERT(strlen(action_id) == 64 && strlen(job_id) == 64);
        ASSERT(strcmp(action_id, job_id) != 0);
        struct db_build_job planned_job = job;
        struct db_build_action planned_action = action;
        ASSERT(build_fabric_plan(&ndb, &job, &action).ok);
        ASSERT(build_fabric_plan(&ndb, &job, &action).ok); /* idempotent */
        ASSERT(build_fabric_submit(&ndb, job.job_id, 110).ok);
        ASSERT(build_fabric_submit(&ndb, job.job_id, 111).ok); /* idempotent */
        ASSERT(build_fabric_plan(&ndb, &planned_job, &planned_action).ok);
        ASSERT(db_build_action_find(&ndb, planned_action.action_id, &action));
        ASSERT_STR_EQ(action.state, "QUEUED");
        ASSERT(db_build_job_find(&ndb, planned_job.job_id, &job));
        ASSERT_STR_EQ(job.state, "QUEUED");

        uint8_t seed[32], pubkey[32], secret[32];
        memset(seed, 7, sizeof(seed));
        ed25519_keypair(pubkey, secret, seed);
        struct db_build_worker worker;
        bf_worker(&worker);
        zcl_hex_encode(pubkey, sizeof(pubkey), worker.signer_pubkey);
        ASSERT(build_fabric_worker_approve(&ndb, &worker, 112).ok);
        bool claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, id_d, 113, 10,
                                  &action, &claimed).ok);
        ASSERT(claimed);
        ASSERT(build_fabric_start(&ndb, action.action_id, id_d, 114).ok);
        ASSERT(build_fabric_begin_verify(&ndb, action.action_id, id_d, 115).ok);
        struct db_build_receipt receipt;
        bf_receipt(&receipt);
        (void)snprintf(receipt.action_id, sizeof(receipt.action_id), "%s",
                       action.action_id);
        (void)snprintf(receipt.action_sha3, sizeof(receipt.action_sha3), "%s",
                       action.action_id);
        (void)snprintf(receipt.job_id, sizeof(receipt.job_id), "%s",
                       action.job_id);
        ASSERT(build_fabric_receipt_id(&receipt, receipt.receipt_id).ok);
        uint8_t receipt_id[32], signature[64];
        ASSERT(zcl_hex_decode_lower(receipt.receipt_id, receipt_id,
                                    sizeof(receipt_id)));
        ed25519_sign(signature, receipt_id, sizeof(receipt_id), secret, pubkey);
        zcl_hex_encode(signature, sizeof(signature), receipt.signature);
        ASSERT(build_fabric_receipt_accept(&ndb, &receipt, 116).ok);
        ASSERT(db_build_action_find(&ndb, planned_action.action_id, &action));
        ASSERT_STR_EQ(action.state, "ACCEPTED");
        ASSERT_STR_EQ(action.output_root_sha3, id_c);
        ASSERT_EQ(action.finished_at, 116);

        /* A fixed action's nonzero result is still authentic evidence. It is
         * stored atomically while the action and job finish FAILED. */
        struct db_build_job fail_job = planned_job;
        struct db_build_action fail_action = planned_action;
        (void)snprintf(fail_action.input_root_sha3,
                       sizeof(fail_action.input_root_sha3), "%s", id_a);
        fail_job.created_at = fail_job.updated_at = 117;
        fail_action.created_at = fail_action.updated_at = 117;
        ASSERT(bf_canonicalize(&fail_job, &fail_action));
        ASSERT(build_fabric_plan(&ndb, &fail_job, &fail_action).ok);
        ASSERT(build_fabric_submit(&ndb, fail_job.job_id, 117).ok);
        claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, id_b, 118, 10,
                                  &fail_action, &claimed).ok);
        ASSERT(claimed);
        ASSERT(build_fabric_start(
            &ndb, fail_action.action_id, id_b, 119).ok);
        ASSERT(build_fabric_begin_verify(
            &ndb, fail_action.action_id, id_b, 120).ok);
        struct db_build_receipt failed_receipt;
        bf_receipt(&failed_receipt);
        (void)snprintf(failed_receipt.action_id,
                       sizeof(failed_receipt.action_id), "%s",
                       fail_action.action_id);
        (void)snprintf(failed_receipt.action_sha3,
                       sizeof(failed_receipt.action_sha3), "%s",
                       fail_action.action_id);
        (void)snprintf(failed_receipt.job_id,
                       sizeof(failed_receipt.job_id), "%s",
                       fail_action.job_id);
        (void)snprintf(failed_receipt.lease_id,
                       sizeof(failed_receipt.lease_id), "%s", id_b);
        failed_receipt.exit_status = 23;
        failed_receipt.created_at = 121;
        ASSERT(build_fabric_receipt_id(
            &failed_receipt, failed_receipt.receipt_id).ok);
        ASSERT(zcl_hex_decode_lower(failed_receipt.receipt_id, receipt_id,
                                    sizeof(receipt_id)));
        ed25519_sign(signature, receipt_id, sizeof(receipt_id), secret, pubkey);
        zcl_hex_encode(signature, sizeof(signature), failed_receipt.signature);
        ASSERT(build_fabric_receipt_accept(&ndb, &failed_receipt, 121).ok);
        ASSERT(db_build_action_find(
            &ndb, fail_action.action_id, &fail_action));
        ASSERT_STR_EQ(fail_action.state, "FAILED");
        ASSERT_STR_EQ(fail_action.last_error,
                      "fixed-action-reported-failure");
        ASSERT(db_build_receipt_find(
            &ndb, failed_receipt.receipt_id, &failed_receipt));

        /* Revocation is durable and makes a newly bound receipt fail before
         * signature acceptance; old evidence remains queryable. */
        ASSERT(build_fabric_worker_revoke(&ndb, id_c, 122).ok);
        ASSERT(build_fabric_worker_revoke(&ndb, id_c, 123).ok);
        receipt.created_at = 124;
        ASSERT(build_fabric_receipt_id(&receipt, receipt.receipt_id).ok);
        ASSERT(!build_fabric_receipt_accept(&ndb, &receipt, 124).ok);
        struct db_build_receipt rows[2];
        ASSERT_EQ(db_build_job_receipts(&ndb, planned_job.job_id, rows, 2), 1);
        ASSERT(!build_fabric_cancel(&ndb, planned_job.job_id, 125).ok);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_leases(void)
{
    int failures = 0;
    TEST("build_fabric: leases claim once, recover restart, and refuse stale owners") {
        struct node_db ndb;
        char dir[256], path[320];
        ASSERT(bf_open(&ndb, dir, sizeof(dir), path, sizeof(path), "leases"));
        struct db_build_job job;
        struct db_build_action action;
        struct db_build_worker worker;
        bf_job(&job);
        bf_action(&action);
        bf_worker(&worker);
        ASSERT(bf_canonicalize(&job, &action));
        char job_id[BUILD_FABRIC_ID_HEX + 1];
        char action_id[BUILD_FABRIC_ID_HEX + 1];
        (void)snprintf(job_id, sizeof(job_id), "%s", job.job_id);
        (void)snprintf(action_id, sizeof(action_id), "%s", action.action_id);
        ASSERT(build_fabric_plan(&ndb, &job, &action).ok);
        ASSERT(build_fabric_submit(&ndb, job_id, 110).ok);
        ASSERT(build_fabric_worker_approve(&ndb, &worker, 111).ok);

        struct db_build_action claimed_action;
        bool claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, id_d, 120, 10,
                                  &claimed_action, &claimed).ok);
        ASSERT(claimed);
        ASSERT_STR_EQ(claimed_action.action_id, action_id);
        ASSERT_STR_EQ(claimed_action.state, "CLAIMED");
        ASSERT_EQ(claimed_action.attempt_count, 1);
        ASSERT_EQ(claimed_action.lease_expires_at, 130);

        struct db_build_action no_action;
        claimed = true;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, id_b, 121, 10,
                                  &no_action, &claimed).ok);
        ASSERT(!claimed); /* the first compare-and-swap owns it */
        ASSERT(!build_fabric_start(&ndb, action_id, id_b, 121).ok);
        ASSERT(build_fabric_start(&ndb, action_id, id_d, 121).ok);
        ASSERT(build_fabric_heartbeat(&ndb, action_id, id_d, 125, 10).ok);
        ASSERT(build_fabric_begin_verify(&ndb, action_id, id_d, 126).ok);
        ASSERT(db_build_action_find(&ndb, action_id, &claimed_action));
        ASSERT_STR_EQ(claimed_action.state, "VERIFYING");
        ASSERT_EQ(claimed_action.lease_expires_at, 135);

        /* A new process sees the same expired lease and requeues it. */
        node_db_close(&ndb);
        memset(&ndb, 0, sizeof(ndb));
        ASSERT(node_db_open(&ndb, path));
        size_t requeued = 99;
        ASSERT(build_fabric_recover_expired(&ndb, 136, &requeued).ok);
        ASSERT_EQ(requeued, 1);
        ASSERT(db_build_action_find(&ndb, action_id, &claimed_action));
        ASSERT_STR_EQ(claimed_action.state, "QUEUED");
        ASSERT_STR_EQ(claimed_action.last_error, "lease-expired-requeued");
        ASSERT(claimed_action.lease_id[0] == '\0');
        ASSERT_EQ(claimed_action.attempt_count, 1);
        ASSERT(!build_fabric_begin_verify(&ndb, action_id, id_d, 137).ok);

        ASSERT(build_fabric_claim(&ndb, worker.worker_id, id_b, 137, 10,
                                  &claimed_action, &claimed).ok);
        ASSERT(claimed && claimed_action.attempt_count == 2);
        ASSERT(build_fabric_start(&ndb, action_id, id_b, 138).ok);
        ASSERT(build_fabric_cancel(&ndb, job_id, 139).ok);
        ASSERT(!build_fabric_heartbeat(&ndb, action_id, id_b, 140, 10).ok);
        ASSERT(db_build_action_find(&ndb, action_id, &claimed_action));
        ASSERT_STR_EQ(claimed_action.state, "CANCELLED");
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_confined_worker(void)
{
    int failures = 0;
    TEST("build_fabric: fixed worker confines, CAS-stores, signs, and accepts one TU") {
        struct node_db ndb;
        char dir[256], path[320];
        ASSERT(bf_open(&ndb, dir, sizeof(dir), path, sizeof(path), "worker"));
        ASSERT(vcs_object_store_init(dir));
        struct db_build_worker persistent_a, persistent_b;
        uint8_t persistent_sk_a[32], persistent_pk_a[32];
        uint8_t persistent_sk_b[32], persistent_pk_b[32];
        ASSERT(build_fabric_worker_identity_load(
            dir, &persistent_a, persistent_sk_a, persistent_pk_a).ok);
        ASSERT(build_fabric_worker_identity_load(
            dir, &persistent_b, persistent_sk_b, persistent_pk_b).ok);
        ASSERT_STR_EQ(persistent_a.worker_id, persistent_b.worker_id);
        ASSERT(memcmp(persistent_pk_a, persistent_pk_b, 32) == 0);
        char key_path[320];
        (void)snprintf(key_path, sizeof(key_path),
                       "%s/zcode/build-worker.ed25519", dir);
        ASSERT(chmod(key_path, 0644) == 0);
        ASSERT(!build_fabric_worker_identity_load(
            dir, &persistent_b, persistent_sk_b, persistent_pk_b).ok);
        ASSERT(chmod(key_path, 0600) == 0);
        static const uint8_t input[] =
            "int zbuild_fixture(void) { return 23; }\n";
        uint8_t input_root[32];
        sha3_256(input, sizeof(input) - 1u, input_root);
        ASSERT(vcs_object_put_addressed(dir, input_root, input,
                                        sizeof(input) - 1u));
        struct vcs_toolchain_capsule_v1 capsule;
        uint8_t capsule_root[32];
        ASSERT(vcs_toolchain_capsule_v1_capture_gcc(&capsule));
        ASSERT(vcs_toolchain_capsule_v1_root(&capsule, capsule_root));

        struct db_build_job job;
        struct db_build_action action;
        bf_job(&job);
        bf_action(&action);
        zcl_hex_encode(capsule_root, sizeof(capsule_root), job.toolchain_sha3);
        zcl_hex_encode(input_root, sizeof(input_root), action.input_root_sha3);
        uint8_t fixed_flags[32], fixed_environment[32];
        vcs_build_action_v1_fixed_flags_root(fixed_flags);
        vcs_build_action_v1_fixed_environment_root(fixed_environment);
        zcl_hex_encode(fixed_flags, 32, action.flags_sha3);
        zcl_hex_encode(fixed_environment, 32, action.environment_sha3);
        ASSERT(bf_canonicalize(&job, &action));
        char action_id[65];
        (void)snprintf(action_id, sizeof(action_id), "%s", action.action_id);
        ASSERT(build_fabric_plan(&ndb, &job, &action).ok);
        int64_t now = (int64_t)platform_time_wall_unix();
        ASSERT(build_fabric_submit(&ndb, job.job_id, now).ok);

        uint8_t seed[32], pubkey[32], secret[32];
        memset(seed, 29, sizeof(seed));
        ed25519_keypair(pubkey, secret, seed);
        struct db_build_worker worker;
        bf_worker(&worker);
        zcl_hex_encode(pubkey, sizeof(pubkey), worker.signer_pubkey);
        ASSERT(build_fabric_worker_approve(&ndb, &worker, now).ok);
        bool claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, id_d, now, 300,
                                  &action, &claimed).ok);
        ASSERT(claimed);
        struct db_build_receipt receipt;
        struct zcl_result executed = build_fabric_worker_execute(
            &ndb, dir, dir, action_id, id_d, secret, pubkey, &receipt);
        if (!executed.ok)
            printf("worker detail: %s\n", executed.message);
        ASSERT(executed.ok);
        ASSERT(db_build_action_find(&ndb, action_id, &action));
        ASSERT_STR_EQ(action.state, "ACCEPTED");
        ASSERT_STR_EQ(action.output_root_sha3, receipt.output_sha3);
        uint8_t manifest_root[32];
        ASSERT(zcl_hex_decode_lower(receipt.output_sha3, manifest_root, 32));
        uint8_t *wire = NULL;
        size_t wire_len = 0;
        ASSERT_EQ(vcs_object_load_raw(dir, manifest_root, &wire, &wire_len), 0);
        struct vcs_build_artifact_manifest_v1 manifest;
        ASSERT(vcs_build_artifact_manifest_v1_parse(wire, wire_len, &manifest));
        free(wire);
        ASSERT_EQ(manifest.chunk_count, 1);
        uint8_t *object = NULL;
        size_t object_len = 0;
        ASSERT_EQ(vcs_object_load_raw(dir, manifest.chunk_sha3[0], &object,
                                      &object_len), 0);
        ASSERT(vcs_build_artifact_manifest_v1_verify_chunk(
            &manifest, 0, object, object_len));
        ASSERT(object_len >= 20 && object[0] == 0x7f && object[1] == 'E' &&
               object[16] == 1 && object[18] == 62);
        free(object);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_confined_test_worker(void)
{
    int failures = 0;
    TEST("build_fabric: fixed test action executes one exact binary and signs evidence") {
        struct node_db ndb;
        char dir[256], path[320];
        ASSERT(bf_open(&ndb, dir, sizeof(dir), path, sizeof(path),
                       "test-worker"));
        ASSERT(vcs_object_store_init(dir));
        size_t input_len = 0;
        uint8_t *input = bf_read_fixture("/usr/bin/true", &input_len);
        ASSERT(input && input_len > 20);
        uint8_t input_root[32];
        sha3_256(input, input_len, input_root);
        ASSERT(vcs_object_put_addressed(dir, input_root, input, input_len));
        free(input);

        struct vcs_toolchain_capsule_v1 capsule;
        uint8_t capsule_root[32];
        ASSERT(vcs_toolchain_capsule_v1_capture_gcc(&capsule));
        ASSERT(vcs_toolchain_capsule_v1_root(&capsule, capsule_root));
        struct db_build_job job;
        struct db_build_action action;
        bf_job(&job);
        bf_action(&action);
        (void)snprintf(action.kind, sizeof(action.kind), "%s",
                       VCS_BUILD_ACTION_KIND_TEST_V1);
        zcl_hex_encode(capsule_root, 32, job.toolchain_sha3);
        zcl_hex_encode(input_root, 32, action.input_root_sha3);
        uint8_t fixed_flags[32], fixed_environment[32];
        ASSERT(vcs_build_action_v1_fixed_flags_root_for_kind(
            action.kind, fixed_flags));
        ASSERT(vcs_build_action_v1_fixed_environment_root_for_kind(
            action.kind, fixed_environment));
        zcl_hex_encode(fixed_flags, 32, action.flags_sha3);
        zcl_hex_encode(fixed_environment, 32, action.environment_sha3);
        (void)snprintf(action.virtual_workdir,
                       sizeof(action.virtual_workdir), "%s",
                       VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1);
        (void)snprintf(action.declared_outputs,
                       sizeof(action.declared_outputs), "%s",
                       VCS_BUILD_TEST_OUTPUT_V1);
        (void)snprintf(action.resource_policy,
                       sizeof(action.resource_policy), "%s",
                       VCS_BUILD_TEST_RESOURCE_POLICY_V1);
        ASSERT(bf_canonicalize(&job, &action));
        char action_id[65];
        (void)snprintf(action_id, sizeof(action_id), "%s", action.action_id);
        ASSERT(build_fabric_plan(&ndb, &job, &action).ok);
        int64_t now = (int64_t)platform_time_wall_unix();
        ASSERT(build_fabric_submit(&ndb, job.job_id, now).ok);

        uint8_t seed[32], pubkey[32], secret[32];
        memset(seed, 31, sizeof(seed));
        ed25519_keypair(pubkey, secret, seed);
        struct db_build_worker worker;
        bf_worker(&worker);
        zcl_hex_encode(pubkey, 32, worker.signer_pubkey);
        ASSERT(build_fabric_worker_approve(&ndb, &worker, now).ok);
        bool claimed = false;
        ASSERT(build_fabric_claim(&ndb, worker.worker_id, id_d, now, 300,
                                  &action, &claimed).ok);
        ASSERT(claimed);
        struct db_build_receipt receipt;
        struct zcl_result executed = build_fabric_worker_execute(
            &ndb, dir, dir, action_id, id_d, secret, pubkey, &receipt);
        if (!executed.ok) printf("test worker detail: %s\n", executed.message);
        ASSERT(executed.ok);
        ASSERT_EQ(receipt.exit_status, 0);
        ASSERT(receipt.work_receipt_sha3[0] == '\0');
        ASSERT(db_build_action_find(&ndb, action_id, &action));
        ASSERT_STR_EQ(action.state, "ACCEPTED");

        uint8_t manifest_root[32];
        ASSERT(zcl_hex_decode_lower(receipt.output_sha3, manifest_root, 32));
        uint8_t *wire = NULL;
        size_t wire_len = 0;
        ASSERT_EQ(vcs_object_load_raw(dir, manifest_root, &wire, &wire_len), 0);
        struct vcs_build_artifact_manifest_v1 manifest;
        ASSERT(vcs_build_artifact_manifest_v1_parse(wire, wire_len, &manifest));
        free(wire);
        ASSERT_EQ(manifest.chunk_count, 1);
        uint8_t *evidence = NULL;
        size_t evidence_len = 0;
        ASSERT_EQ(vcs_object_load_raw(dir, manifest.chunk_sha3[0], &evidence,
                                      &evidence_len), 0);
        ASSERT_EQ(evidence_len, 84);
        ASSERT(memcmp(evidence, "ZCTEST\r\n", 8) == 0 &&
               evidence[8] == 1 && evidence[10] == 1);
        free(evidence);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_native(void)
{
    int failures = 0;
    TEST("build_fabric: native plan and read-only status share the service") {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "build_fabric", "native");
        struct json_value input;
        json_init(&input); json_set_object(&input);
        (void)json_push_kv_str(&input, "source_sha256", id_a);
        (void)json_push_kv_str(&input, "source_cas_sha3", id_b);
        (void)json_push_kv_str(&input, "toolchain_sha3", id_c);
        (void)json_push_kv_str(&input, "input_root_sha3", id_d);
        (void)json_push_kv_str(&input, "flags_sha3", id_a);
        (void)json_push_kv_str(&input, "environment_sha3", id_b);
        (void)json_push_kv_str(&input, "profile", "dev");
        (void)json_push_kv_str(&input, "datadir", dir);
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.build_plan.v1");
        zcl_native_handle_metaverse_build_plan(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        const struct json_value *job = json_get(&reply.data, "job");
        const char *returned_id = json_get_str(json_get(job, "job_id"));
        ASSERT(returned_id && strlen(returned_id) == 64);
        char job_id[65];
        (void)snprintf(job_id, sizeof(job_id), "%s", returned_id);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        (void)json_push_kv_str(&input, "job_id", job_id);
        (void)json_push_kv_str(&input, "datadir", dir);
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.build_status.v1");
        zcl_native_handle_metaverse_build_status(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "state")), "PLANNED");
        ASSERT_EQ(json_get_int(json_get(&reply.data, "action_count")), 1);
        const struct json_value *actions = json_get(&reply.data, "actions");
        ASSERT(actions && actions->type == JSON_ARR &&
               actions->num_children == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(actions, 0), "kind")),
                     "c23.compile.preprocessed.v1");
        zcl_command_reply_free(&reply);
        json_free(&input);

        char db_path[320];
        (void)snprintf(db_path, sizeof(db_path), "%s/node.db", dir);
        struct node_db api_db;
        memset(&api_db, 0, sizeof(api_db));
        ASSERT(node_db_open(&api_db, db_path));
        api_set_state(NULL, NULL, NULL, &api_db, dir);
        ASSERT(api_route_is_operator_private("/api/v1/builds"));
        ASSERT(api_route_is_operator_private("/api/v1/builds/abc/actions"));
        ASSERT(api_route_is_operator_private("/api/v1/build_workers"));
        ASSERT(api_route_is_operator_private("/api/v1/build_receipts/abc"));
        uint8_t response[16384];
        size_t response_len = api_handle_request(
            "GET", "/api/v1/builds", NULL, 0, response, sizeof(response));
        ASSERT(response_len > 0 && response_len < sizeof(response));
        response[response_len] = '\0';
        ASSERT(strstr((char *)response, "HTTP/1.1 200 OK") != NULL);
        ASSERT(strstr((char *)response, "zcl.builds.index.v1") != NULL);
        char member_path[128];
        (void)snprintf(member_path, sizeof(member_path),
                       "/api/v1/builds/%s/actions", job_id);
        response_len = api_handle_request("GET", member_path, NULL, 0,
                                          response, sizeof(response));
        ASSERT(response_len > 0 && response_len < sizeof(response));
        response[response_len] = '\0';
        ASSERT(strstr((char *)response, "zcl.build_actions.index.v1") != NULL);
        api_set_state(NULL, NULL, NULL, NULL, NULL);
        node_db_close(&api_db);
        test_rm_rf(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_runtime_dump(void)
{
    int failures = 0;
    TEST("build_fabric: runtime diagnostics publish ceilings and supervision") {
        struct json_value state;
        json_init(&state);
        ASSERT(build_fabric_dump_state_json(&state, NULL));
        ASSERT_STR_EQ(json_get_str(json_get(&state, "schema")),
                      "zcl.build_fabric_state.v1");
        ASSERT_EQ(json_get_int(json_get(&state, "max_actions_per_job")), 256);
        ASSERT_EQ(json_get_int(json_get(&state, "worker_cpu_limit")), 1);
        ASSERT(!json_get_bool(json_get(&state, "worker_network_allowed")));
        const struct json_value *health = json_get(&state, "_health");
        ASSERT(health && health->type == JSON_OBJ);
        json_free(&state);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bf_content_contracts(void)
{
    int failures = 0;
    TEST("build_fabric: capsules, actions, and artifact chunks are content-bound") {
        struct vcs_toolchain_capsule_v1 capsule = {0};
        memset(capsule.compiler_driver_sha3, 1, 32);
        memset(capsule.compiler_backend_sha3, 2, 32);
        memset(capsule.assembler_sha3, 3, 32);
        memset(capsule.sysroot_sha3, 4, 32);
        memset(capsule.target_probes_sha3, 5, 32);
        memset(capsule.abi_files_sha3, 6, 32);
        (void)snprintf(capsule.target, sizeof(capsule.target), "%s",
                       VCS_BUILD_TARGET_V1);
        uint8_t capsule_a[32], capsule_b[32];
        ASSERT(vcs_toolchain_capsule_v1_root(&capsule, capsule_a));
        capsule.compiler_backend_sha3[0] ^= 1;
        ASSERT(vcs_toolchain_capsule_v1_root(&capsule, capsule_b));
        ASSERT(memcmp(capsule_a, capsule_b, 32) != 0);

        struct vcs_build_action_v1 action = {0};
        memset(action.source_sha256, 1, 32);
        memset(action.source_cas_sha3, 2, 32);
        memset(action.input_root_sha3, 3, 32);
        memcpy(action.toolchain_capsule_sha3, capsule_b, 32);
        memset(action.flags_sha3, 4, 32);
        memset(action.environment_sha3, 5, 32);
        (void)snprintf(action.target, sizeof(action.target), "%s",
                       VCS_BUILD_TARGET_V1);
        (void)snprintf(action.profile, sizeof(action.profile), "dev");
        (void)snprintf(action.virtual_workdir,
                       sizeof(action.virtual_workdir), "%s",
                       VCS_BUILD_VIRTUAL_ROOT_V1);
        (void)snprintf(action.declared_outputs,
                       sizeof(action.declared_outputs), "%s",
                       VCS_BUILD_OUTPUT_V1);
        (void)snprintf(action.resource_policy,
                       sizeof(action.resource_policy), "%s",
                       VCS_BUILD_RESOURCE_POLICY_V1);
        uint8_t action_a[32], action_b[32];
        ASSERT(vcs_build_action_v1_root(&action, action_a));
        action.environment_sha3[0] ^= 1;
        ASSERT(vcs_build_action_v1_root(&action, action_b));
        ASSERT(memcmp(action_a, action_b, 32) != 0);
        ASSERT_EQ(vcs_build_action_v1_work_kind(VCS_BUILD_ACTION_KIND_V1),
                  VCS_ZCODE_WORK_BUILD);
        ASSERT_EQ(vcs_build_action_v1_work_kind(
                      VCS_BUILD_ACTION_KIND_PACKAGE_V1),
                  VCS_ZCODE_WORK_BUILD);
        ASSERT_EQ(vcs_build_action_v1_work_kind(
                      VCS_BUILD_ACTION_KIND_TEST_V1),
                  VCS_ZCODE_WORK_TEST);
        ASSERT_EQ(vcs_build_action_v1_work_kind(
                      VCS_BUILD_ACTION_KIND_FUZZ_V1),
                  VCS_ZCODE_WORK_FUZZ);
        ASSERT_EQ(vcs_build_action_v1_work_kind(
                      VCS_BUILD_ACTION_KIND_REVIEW_V1),
                  VCS_ZCODE_WORK_REVIEW);
        ASSERT_EQ(vcs_build_action_v1_work_kind("c23.shell.v1"), 0);
        uint8_t test_flags[32], test_env[32], test_action[32];
        ASSERT(vcs_build_action_v1_fixed_flags_root_for_kind(
            VCS_BUILD_ACTION_KIND_TEST_V1, test_flags));
        ASSERT(vcs_build_action_v1_fixed_environment_root_for_kind(
            VCS_BUILD_ACTION_KIND_TEST_V1, test_env));
        memcpy(action.flags_sha3, test_flags, 32);
        memcpy(action.environment_sha3, test_env, 32);
        (void)snprintf(action.virtual_workdir,
                       sizeof(action.virtual_workdir), "%s",
                       VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1);
        (void)snprintf(action.declared_outputs,
                       sizeof(action.declared_outputs), "%s",
                       VCS_BUILD_TEST_OUTPUT_V1);
        (void)snprintf(action.resource_policy,
                       sizeof(action.resource_policy), "%s",
                       VCS_BUILD_TEST_RESOURCE_POLICY_V1);
        ASSERT(vcs_build_action_v1_root_for_kind(
            VCS_BUILD_ACTION_KIND_TEST_V1, &action, test_action));
        ASSERT(memcmp(action_b, test_action, 32) != 0);
        ASSERT(!vcs_build_action_v1_root_for_kind(
            "c23.shell.v1", &action, test_action));

        const uint8_t chunks[2][3] = {{'a','b','c'}, {'d','e','f'}};
        struct vcs_build_artifact_manifest_v1 manifest = {0}, parsed = {0};
        memcpy(manifest.action_sha3, action_b, 32);
        manifest.total_bytes = 6;
        manifest.chunk_bytes = 3;
        manifest.chunk_count = 2;
        sha3_256(chunks[0], sizeof(chunks[0]), manifest.chunk_sha3[0]);
        sha3_256(chunks[1], sizeof(chunks[1]), manifest.chunk_sha3[1]);
        uint8_t root[32], wire[VCS_BUILD_ARTIFACT_WIRE_MAX];
        size_t wire_len = 0;
        ASSERT(vcs_build_artifact_manifest_v1_root(&manifest, root));
        ASSERT(vcs_build_artifact_manifest_v1_serialize(
            &manifest, wire, sizeof(wire), &wire_len));
        ASSERT(vcs_build_artifact_manifest_v1_parse(wire, wire_len, &parsed));
        ASSERT(vcs_build_artifact_manifest_v1_verify_chunk(
            &parsed, 1, chunks[1], sizeof(chunks[1])));
        uint8_t corrupt[3] = {'d','e','x'};
        ASSERT(!vcs_build_artifact_manifest_v1_verify_chunk(
            &parsed, 1, corrupt, sizeof(corrupt)));
        manifest.total_bytes = VCS_BUILD_ARTIFACT_MAX_BYTES + 1;
        ASSERT(!vcs_build_artifact_manifest_v1_valid(&manifest));
        ASSERT_EQ(VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES,
                  UINT64_C(64) * 1024u * 1024u);
        ASSERT(VCS_BUILD_ARTIFACT_MAX_BYTES >
               VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES);
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
    failures += test_bf_leases();
    failures += test_bf_confined_worker();
    failures += test_bf_confined_test_worker();
    failures += test_bf_native();
    failures += test_bf_runtime_dump();
    failures += test_bf_content_contracts();
    printf("=== build_fabric: %d failures ===\n", failures);
    return failures;
}
