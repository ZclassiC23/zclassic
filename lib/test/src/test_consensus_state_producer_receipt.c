/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Producer-owned source receipt: a freshly-begun producer datadir earns a
 * receipt the contained full-history exporter admits end-to-end, and a
 * missing / tampered / unowned receipt is refused (fail closed). */

#define _GNU_SOURCE

#include "test/test_helpers.h"

#include "config/consensus_state_producer_receipt.h"
#include "config/consensus_state_snapshot_export.h"
#include "chain/checkpoints.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
#include "storage/consensus_state_bundle_codec.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void reducer_frontier_test_set_compiled_anchor(int32_t height);

/* A deterministic hermetic 40-hex commit so the test does not depend on the
 * build's baked git identity. */
#define PR_TEST_COMMIT "0123456789abcdef0123456789abcdef01234567"

#define PR_CHECK(label, expr) do {                                        \
    printf("producer_receipt: %s... ", (label));                         \
    if (expr) printf("OK\n");                                            \
    else { printf("FAIL\n"); failures++; }                               \
} while (0)

static bool pr_exec(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    bool ok = sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK;
    if (error)
        sqlite3_free(error);
    return ok;
}

static bool pr_insert_hash_row(sqlite3 *db, const char *sql, int height,
                               const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    bool ok = sqlite3_bind_int(st, 1, height) == SQLITE_OK &&
              sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool pr_insert_header(sqlite3 *db, int height, const uint8_t hash[32],
                             const uint8_t parent[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO header_admit_log(height,hash,parent_hash) "
            "VALUES(?,?,?)", -1, &st, NULL) != SQLITE_OK)
        return false;
    bool ok = sqlite3_bind_int(st, 1, height) == SQLITE_OK &&
              sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC) == SQLITE_OK;
    if (ok && height == 0)
        ok = sqlite3_bind_null(st, 3) == SQLITE_OK;
    else if (ok)
        ok = sqlite3_bind_blob(st, 3, parent, 32, SQLITE_STATIC) == SQLITE_OK;
    if (ok)
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool pr_insert_tip(sqlite3 *db, int height, const char *status,
                          const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO tip_finalize_log(height,ok,status,tip_hash) "
            "VALUES(?,1,?,?)", -1, &st, NULL) != SQLITE_OK)
        return false;
    bool ok = sqlite3_bind_int(st, 1, height) == SQLITE_OK &&
              sqlite3_bind_text(st, 2, status, -1, SQLITE_STATIC) ==
                  SQLITE_OK &&
              sqlite3_bind_blob(st, 3, hash, 32, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

/* Build the tables + cursors that must exist BEFORE the producer begins its
 * fold (begin sets the source-epoch authority the stage rows are stamped
 * with). */
static bool pr_seed_structure(sqlite3 *db)
{
    static const char schema[] =
        "CREATE TABLE header_admit_log("
        "height INTEGER PRIMARY KEY,hash BLOB NOT NULL,parent_hash BLOB);"
        "CREATE TABLE validate_headers_log("
        "height INTEGER PRIMARY KEY,hash BLOB NOT NULL,ok INTEGER NOT NULL);"
        "CREATE TABLE body_fetch_log("
        "height INTEGER PRIMARY KEY,hash BLOB NOT NULL,ok INTEGER NOT NULL);"
        "CREATE TABLE body_persist_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL);"
        "CREATE TABLE script_validate_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL,status TEXT NOT NULL,"
        "block_hash BLOB,source_epoch_digest BLOB);"
        "CREATE TABLE proof_validate_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL,status TEXT NOT NULL,"
        "block_hash BLOB,source_epoch_digest BLOB);"
        "CREATE TABLE utxo_apply_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL,status TEXT NOT NULL);"
        "CREATE TABLE utxo_apply_delta("
        "height INTEGER PRIMARY KEY,branch_hash BLOB NOT NULL,"
        "spent_blob BLOB NOT NULL,added_blob BLOB NOT NULL);"
        "CREATE TABLE tip_finalize_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL,status TEXT NOT NULL,"
        "tip_hash BLOB NOT NULL);"
        "INSERT INTO stage_cursor(name,cursor,updated_at) VALUES"
        "('header_admit',2,1),('validate_headers',2,1),"
        "('body_fetch',2,1),('body_persist',2,1),"
        "('script_validate',2,1),('proof_validate',2,1),"
        "('utxo_apply',2,1),('tip_finalize',1,1);"
        "INSERT INTO body_persist_log VALUES(0,1),(1,1);"
        "INSERT INTO utxo_apply_log VALUES(0,1,'verified'),(1,1,'verified');";
    return pr_exec(db, schema) && coins_kv_ensure_schema(db) &&
           anchor_kv_initialize_history(db, 0) &&
           nullifier_kv_initialize_history(db, 0);
}

/* Seed the fold-produced rows AFTER begin, so the crypto stage rows pick up the
 * source-epoch authority the producer just published (exactly the ordering a
 * real -mint-anchor fold follows). */
static bool pr_seed_fold_rows(sqlite3 *db, uint8_t hash[2][32], uint8_t nf[32])
{
    if (!pr_insert_header(db, 0, hash[0], NULL) ||
        !pr_insert_header(db, 1, hash[1], hash[0]) ||
        !pr_insert_tip(db, 0, "finalized", hash[1]) ||
        !pr_insert_tip(db, 1, "anchor", hash[1]))
        return false;
    static const char *const hash_sql[] = {
        "INSERT INTO validate_headers_log VALUES(?,?,1)",
        "INSERT INTO body_fetch_log VALUES(?,?,1)",
        "INSERT INTO script_validate_log("
        "height,block_hash,ok,status,source_epoch_digest) "
        "VALUES(?,?,1,'verified',(SELECT value FROM progress_meta WHERE key='"
        CONSENSUS_STATE_SOURCE_EPOCH_META_KEY "'))",
        "INSERT INTO proof_validate_log("
        "height,block_hash,ok,status,source_epoch_digest) "
        "VALUES(?,?,1,'verified',(SELECT value FROM progress_meta WHERE key='"
        CONSENSUS_STATE_SOURCE_EPOCH_META_KEY "'))",
        "INSERT INTO utxo_apply_delta(height,branch_hash,spent_blob,added_blob) "
        "VALUES(?,?,X'',X'')",
    };
    for (size_t table = 0; table < sizeof(hash_sql) / sizeof(hash_sql[0]);
         table++) {
        for (int height = 0; height <= 1; height++) {
            if (!pr_insert_hash_row(db, hash_sql[table], height, hash[height]))
                return false;
        }
    }

    uint8_t txid[32];
    for (size_t i = 0; i < sizeof(txid); i++)
        txid[i] = (uint8_t)(0xc1u + i);
    const uint8_t script[] = {0x51};
    if (!coins_kv_add(db, txid, 0, 5000, 0, true, script, sizeof(script)))
        return false;
    struct incremental_merkle_tree sprout;
    struct incremental_merkle_tree sapling;
    struct uint256 leaf;
    sprout_tree_init(&sprout);
    sapling_tree_init(&sapling);
    for (size_t i = 0; i < sizeof(leaf.data); i++)
        leaf.data[i] = (uint8_t)(0xe1u + i);
    incremental_tree_append(&sprout, &leaf);
    incremental_tree_append(&sapling, &leaf);
    if (!anchor_kv_add_tree(db, ANCHOR_POOL_SPROUT, &sprout, 1) ||
        !anchor_kv_add_tree(db, ANCHOR_POOL_SAPLING, &sapling, 1) ||
        !nullifier_kv_add(db, nf, 0, 1))
        return false;

    const uint8_t one = 1;
    if (!progress_meta_set(db, COINS_KV_MIGRATION_COMPLETE_KEY, &one, 1) ||
        !coins_kv_mark_self_folded(db) ||
        !pr_exec(db, "BEGIN IMMEDIATE") ||
        !coins_kv_set_applied_height_in_tx(db, 2) ||
        !pr_exec(db, "COMMIT")) {
        (void)pr_exec(db, "ROLLBACK");
        return false;
    }
    return true;
}

/* Arm the compiled checkpoint override so the exporter's checkpoint match
 * succeeds for this h=1 fixture generation. */
static bool pr_arm_checkpoint(sqlite3 *db, const uint8_t hash1[32])
{
    static struct sha3_utxo_checkpoint checkpoint;
    memset(&checkpoint, 0, sizeof(checkpoint));
    checkpoint.height = 1;
    memcpy(checkpoint.block_hash, hash1, 32);
    checkpoint.utxo_count = 1;
    checkpoint.total_supply = 5000;
    if (coins_kv_commitment(db, checkpoint.sha3_hash) != 0)
        return false;
    checkpoints_set_sha3_override_for_test(&checkpoint);
    reducer_frontier_test_set_compiled_anchor(0);
    return true;
}

static bool pr_run_export(sqlite3 *db, int output_dir_fd, const char *name,
                          const uint8_t hash1[32],
                          struct consensus_state_export_result *out)
{
    struct consensus_state_snapshot_export_request request = {
        .output_dir_fd = output_dir_fd,
        .output_name = name,
        .expected_height = 1,
    };
    memcpy(request.expected_block_hash, hash1, 32);
    return consensus_state_snapshot_export(db, &request, out);
}

int test_consensus_state_producer_receipt(void)
{
    int failures = 0;
    consensus_state_producer_receipt_test_set_identity(PR_TEST_COMMIT, true);

    char dir_template[] = "/tmp/zcl-pr-XXXXXX";
    char *dir = mkdtemp(dir_template);
    PR_CHECK("fixture directory", dir != NULL);
    if (!dir) {
        consensus_state_producer_receipt_test_set_identity(NULL, false);
        return failures;
    }
    PR_CHECK("owned progress store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();

    uint8_t hash[2][32] = {{0}};
    uint8_t nf[32] = {0};
    for (size_t i = 0; i < 32; i++) {
        hash[0][i] = (uint8_t)(0x21u + i);
        hash[1][i] = (uint8_t)(0x61u + i);
        nf[i] = (uint8_t)(0xa1u + i);
    }

    PR_CHECK("structural tables seed", db && pr_seed_structure(db));

    /* finalize BEFORE begin fails closed: no start session exists. */
    char err[256] = {0};
    PR_CHECK("finalize without a start session refuses",
             !consensus_state_producer_receipt_finalize(db, 1, hash[1], err,
                                                         sizeof(err)) &&
             strstr(err, "no start session") != NULL);

    /* Producer START ownership. */
    PR_CHECK("producer begin writes the durable session",
             consensus_state_producer_receipt_begin(
                 db, CONSENSUS_STATE_VALIDATION_FULL, err, sizeof(err)));
    /* Idempotent resume by the same binary + claim is a no-op success. */
    PR_CHECK("producer begin is idempotent for the same binary",
             consensus_state_producer_receipt_begin(
                 db, CONSENSUS_STATE_VALIDATION_FULL, err, sizeof(err)));
    /* A different validation profile on the same datadir refuses. */
    PR_CHECK("producer begin refuses a conflicting profile",
             !consensus_state_producer_receipt_begin(
                 db, CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD, err,
                 sizeof(err)));

    /* The fold runs; its crypto rows are stamped with the published epoch. */
    PR_CHECK("fold rows seed with the published source epoch",
             pr_seed_fold_rows(db, hash, nf));

    /* Producer END ownership: finalize binds the receipt to the fold. */
    PR_CHECK("producer finalize writes the source receipt",
             consensus_state_producer_receipt_finalize(db, 1, hash[1], err,
                                                        sizeof(err)));
    PR_CHECK("producer finalize exact retry is idempotent",
             consensus_state_producer_receipt_finalize(db, 1, hash[1], err,
                                                        sizeof(err)));

    PR_CHECK("compiled checkpoint override arms", pr_arm_checkpoint(db, hash[1]));

    char export_dir[512];
    snprintf(export_dir, sizeof(export_dir), "%s/export", dir);
    PR_CHECK("descriptor-bound output directory",
             mkdir(export_dir, 0700) == 0);
    int output_dir_fd = open(export_dir,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    PR_CHECK("output directory descriptor", output_dir_fd >= 0);

    char accepted_output[640];
    snprintf(accepted_output, sizeof(accepted_output), "%s/accepted.bundle.db",
             export_dir);
    struct consensus_state_export_result accepted;
    bool exported = pr_run_export(db, output_dir_fd, "accepted.bundle.db",
                                  hash[1], &accepted);
    struct stat st;
    PR_CHECK("producer-earned receipt is admitted end-to-end",
             exported && accepted.status == CONSENSUS_EXPORT_EXPORTED &&
             accepted.history_complete && accepted.height == 1 &&
             accepted.source_clean &&
             accepted.validation_profile == CONSENSUS_STATE_VALIDATION_FULL &&
             accepted.utxo_count == 1 && accepted.anchor_count == 2 &&
             accepted.nullifier_count == 1 &&
             lstat(accepted_output, &st) == 0 && S_ISREG(st.st_mode));

    /* TAMPER: flip a byte of the receipt digest — the exporter refuses. */
    char tamper_output[640];
    snprintf(tamper_output, sizeof(tamper_output), "%s/tamper.bundle.db",
             export_dir);
    PR_CHECK("corrupt the finalized receipt digest",
             pr_exec(db,
                 "UPDATE consensus_state_source_receipt "
                 "SET receipt_digest=zeroblob(32)"));
    PR_CHECK("finalize refuses to overwrite conflicting durable evidence",
             !consensus_state_producer_receipt_finalize(db, 1, hash[1], err,
                                                         sizeof(err)) &&
             strstr(err, "conflicting finalized receipt") != NULL);
    struct consensus_state_export_result tampered;
    PR_CHECK("tampered receipt is refused and publishes nothing",
             !pr_run_export(db, output_dir_fd, "tamper.bundle.db", hash[1],
                            &tampered) &&
             tampered.status == CONSENSUS_EXPORT_MISSING_PROOF &&
             access(tamper_output, F_OK) != 0);

    /* MISSING: delete the receipt — the exporter refuses. */
    char missing_output[640];
    snprintf(missing_output, sizeof(missing_output), "%s/missing.bundle.db",
             export_dir);
    PR_CHECK("delete the finalized receipt",
             pr_exec(db, "DELETE FROM consensus_state_source_receipt"));
    struct consensus_state_export_result missing;
    PR_CHECK("missing receipt is refused and publishes nothing",
             !pr_run_export(db, output_dir_fd, "missing.bundle.db", hash[1],
                            &missing) &&
             missing.status == CONSENSUS_EXPORT_MISSING_PROOF &&
             access(missing_output, F_OK) != 0);

    /* UNOWNED START SESSION: corrupt the recorded running-binary digest so the
     * running executable no longer owns the session — finalize refuses. */
    PR_CHECK("corrupt the start session's running-binary digest",
             pr_exec(db,
                 "UPDATE consensus_state_producer_session "
                 "SET running_binary_digest=zeroblob(32)"));
    PR_CHECK("finalize refuses a session the running binary does not own",
             !consensus_state_producer_receipt_finalize(db, 1, hash[1], err,
                                                         sizeof(err)) &&
             strstr(err, "does not own the start session") != NULL);

    reducer_frontier_test_set_compiled_anchor(-1);
    checkpoints_reset_sha3_override_for_test();
    consensus_state_producer_receipt_test_set_identity(NULL, false);
    progress_store_close();
    if (output_dir_fd >= 0)
        close(output_dir_fd);
    (void)unlink(accepted_output);
    (void)rmdir(export_dir);
    (void)rmdir(dir);
    return failures;
}
