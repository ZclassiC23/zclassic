/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Full-history consensus-state exporter containment tests. */

#include "test/test_helpers.h"

#include "config/consensus_state_snapshot_export.h"
#include "config/consensus_state_snapshot_install.h"
#include "chain/checkpoints.h"
#include "crypto/sha3.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
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

#define CSE_CHECK(label, expr) do {                                      \
    printf("consensus_state_export: %s... ", (label));                  \
    if (expr) printf("OK\n");                                         \
    else { printf("FAIL\n"); failures++; }                            \
} while (0)

static bool cse_exec(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    bool ok = sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK;
    if (error)
        sqlite3_free(error);
    return ok;
}

static bool cse_insert_hash_row(sqlite3 *db, const char *sql, int height,
                                const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    bool ok = sqlite3_bind_int(st, 1, height) == SQLITE_OK &&
              sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC) ==
                  SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool cse_insert_header(sqlite3 *db, int height,
                              const uint8_t hash[32],
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

static void cse_u64(struct sha3_256_ctx *ctx, uint64_t value)
{
    uint8_t le[8];
    for (size_t i = 0; i < sizeof(le); i++)
        le[i] = (uint8_t)(value >> (8u * i));
    sha3_256_write(ctx, le, sizeof(le));
}

static void cse_chain_corpus_digest(uint8_t hash[2][32], uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    static const char domain[] =
        "zcl.consensus_state_bundle.v1/source-header-chain";
    sha3_256_write(&ctx, (const uint8_t *)domain, sizeof(domain));
    uint8_t zero[32] = {0};
    cse_u64(&ctx, 0);
    sha3_256_write(&ctx, hash[0], 32);
    sha3_256_write(&ctx, zero, 32);
    cse_u64(&ctx, 1);
    sha3_256_write(&ctx, hash[1], 32);
    sha3_256_write(&ctx, hash[0], 32);
    sha3_256_finalize(&ctx, out);
}

static bool cse_binary_digest(uint8_t out[32])
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint8_t buffer[32768];
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            sha3_256_write(&ctx, buffer, (size_t)n);
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        ok = false;
        break;
    }
    if (close(fd) != 0)
        ok = false;
    if (ok)
        sha3_256_finalize(&ctx, out);
    return ok;
}

static bool cse_seed_receipt(sqlite3 *db, uint8_t hash[2][32], bool corrupt)
{
    if (!cse_exec(db,
            "CREATE TABLE IF NOT EXISTS consensus_state_source_receipt("
            "singleton INTEGER PRIMARY KEY,schema TEXT NOT NULL,"
            "source_tree_root BLOB NOT NULL,running_binary_digest BLOB NOT NULL,"
            "toolchain_digest BLOB NOT NULL,chain_corpus_digest BLOB NOT NULL,"
            "producer_commit TEXT NOT NULL,fold_cursor INTEGER NOT NULL,"
            "receipt_digest BLOB NOT NULL)"))
        return false;
    struct consensus_state_source_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    for (size_t i = 0; i < 32; i++) {
        receipt.source_tree_root[i] = (uint8_t)(0x31u + i);
        receipt.toolchain_digest[i] = (uint8_t)(0x71u + i);
    }
    if (!cse_binary_digest(receipt.running_binary_digest))
        return false;
    cse_chain_corpus_digest(hash, receipt.chain_corpus_digest);
    snprintf(receipt.producer_commit, sizeof(receipt.producer_commit),
             "0123456789abcdef0123456789abcdef01234567");
    receipt.fold_cursor = 2;
    consensus_state_source_receipt_digest(&receipt, receipt.receipt_digest);
    if (corrupt)
        receipt.receipt_digest[0] ^= 1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO consensus_state_source_receipt VALUES"
            "(1,?,?,?,?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
        return false;
    int i = 1;
    bool ok = sqlite3_bind_text(st, i++,
                                CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA, -1,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, receipt.source_tree_root, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, receipt.running_binary_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, receipt.toolchain_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, receipt.chain_corpus_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_text(st, i++, receipt.producer_commit, 40,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int64(st, i++, receipt.fold_cursor) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, receipt.receipt_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
}

static bool cse_insert_tip(sqlite3 *db, int height, const char *status,
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

static bool cse_seed_source(sqlite3 *db, uint8_t hash[2][32], uint8_t nf[32])
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
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL,block_hash BLOB);"
        "CREATE TABLE proof_validate_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL);"
        "CREATE TABLE utxo_apply_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL);"
        "CREATE TABLE tip_finalize_log("
        "height INTEGER PRIMARY KEY,ok INTEGER NOT NULL,status TEXT NOT NULL,"
        "tip_hash BLOB NOT NULL);"
        "INSERT INTO stage_cursor(name,cursor,updated_at) VALUES"
        "('header_admit',2,1),('validate_headers',2,1),"
        "('body_fetch',2,1),('body_persist',2,1),"
        "('script_validate',2,1),('proof_validate',2,1),"
        "('utxo_apply',2,1),('tip_finalize',1,1);"
        "INSERT INTO body_persist_log VALUES(0,1),(1,1);"
        "INSERT INTO proof_validate_log VALUES(0,1),(1,1);"
        "INSERT INTO utxo_apply_log VALUES(0,1),(1,1);";
    if (!cse_exec(db, schema) || !coins_kv_ensure_schema(db) ||
        !anchor_kv_initialize_history(db, 0) ||
        !nullifier_kv_initialize_history(db, 0))
        return false;

    for (size_t i = 0; i < 32; i++) {
        hash[0][i] = (uint8_t)(0x21u + i);
        hash[1][i] = (uint8_t)(0x61u + i);
        nf[i] = (uint8_t)(0xa1u + i);
    }
    if (!cse_insert_header(db, 0, hash[0], NULL) ||
        !cse_insert_header(db, 1, hash[1], hash[0]) ||
        !cse_insert_tip(db, 0, "finalized", hash[1]) ||
        !cse_insert_tip(db, 1, "anchor", hash[1]))
        return false;
    static const char *const hash_sql[] = {
        "INSERT INTO validate_headers_log VALUES(?,?,1)",
        "INSERT INTO body_fetch_log VALUES(?,?,1)",
        "INSERT INTO script_validate_log(height,block_hash,ok) VALUES(?,?,1)",
    };
    for (size_t table = 0; table < sizeof(hash_sql) / sizeof(hash_sql[0]);
         table++) {
        for (int height = 0; height <= 1; height++) {
            if (!cse_insert_hash_row(db, hash_sql[table], height,
                                     hash[height]))
                return false;
        }
    }

    uint8_t txid[32];
    for (size_t i = 0; i < sizeof(txid); i++)
        txid[i] = (uint8_t)(0xc1u + i);
    const uint8_t script[] = {0x51};
    if (!coins_kv_add(db, txid, 0, 5000, 0, true, script,
                      sizeof(script)))
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
        !cse_exec(db, "BEGIN IMMEDIATE") ||
        !coins_kv_set_applied_height_in_tx(db, 2) ||
        !cse_exec(db, "COMMIT") || !cse_seed_receipt(db, hash, false)) {
        (void)cse_exec(db, "ROLLBACK");
        return false;
    }
    return true;
}

int test_consensus_state_snapshot_export(void)
{
    int failures = 0;
    char dir_template[] = "/tmp/zcl-cse-XXXXXX";
    char *dir = mkdtemp(dir_template);
    CSE_CHECK("fixture directory", dir != NULL);
    if (!dir)
        return failures;
    CSE_CHECK("owned progress store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    uint8_t hash[2][32] = {{0}};
    uint8_t nf[32] = {0};
    CSE_CHECK("complete source generation", db && cse_seed_source(db, hash, nf));

    char output[512];
    char missing_output[512];
    char malformed_output[512];
    snprintf(output, sizeof(output), "%s/complete.bundle.db", dir);
    snprintf(missing_output, sizeof(missing_output), "%s/missing.bundle.db",
             dir);
    snprintf(malformed_output, sizeof(malformed_output),
             "%s/malformed.bundle.db", dir);
    struct sha3_utxo_checkpoint checkpoint;
    memset(&checkpoint, 0, sizeof(checkpoint));
    checkpoint.height = 1;
    memcpy(checkpoint.block_hash, hash[1], 32);
    checkpoint.utxo_count = 1;
    checkpoint.total_supply = 5000;
    CSE_CHECK("fixture checkpoint commitment",
              coins_kv_commitment(db, checkpoint.sha3_hash) == 0);
    checkpoints_set_sha3_override_for_test(&checkpoint);
    reducer_frontier_test_set_compiled_anchor(0);
    struct consensus_state_snapshot_export_request request = {
        .output_path = output,
        .expected_height = 1,
    };
    memcpy(request.expected_block_hash, hash[1], 32);
    struct consensus_state_export_result result;
    bool exported = consensus_state_snapshot_export(db, &request, &result);
    struct stat st;
    CSE_CHECK("complete generation exports", exported &&
              result.status == CONSENSUS_EXPORT_EXPORTED &&
              result.history_complete && result.height == 1 &&
              result.utxo_count == 1 && result.anchor_count == 2 &&
              result.nullifier_count == 1);
    CSE_CHECK("final artifact is immutable", lstat(output, &st) == 0 &&
              S_ISREG(st.st_mode) &&
              (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0);

    struct consensus_state_snapshot_install_request install_request = {
        .bundle_path = output,
        .expected_height = 1,
        .failpoint = CONSENSUS_INSTALL_FAIL_NONE,
    };
    memcpy(install_request.expected_block_hash, hash[1], 32);
    struct consensus_state_install_result install_result;
    CSE_CHECK("production validator reopens artifact",
              !consensus_state_snapshot_install(db, &install_request,
                                                &install_result) &&
              install_result.status == CONSENSUS_INSTALL_VERIFIED_CONTAINED &&
              install_result.history_complete);

    struct consensus_state_export_result repeat_result;
    CSE_CHECK("existing final is never replaced",
              !consensus_state_snapshot_export(db, &request, &repeat_result) &&
              repeat_result.status == CONSENSUS_EXPORT_REFUSED);

    CSE_CHECK("remove required source receipt",
              cse_exec(db, "DELETE FROM consensus_state_source_receipt"));
    request.output_path = missing_output;
    struct consensus_state_export_result missing_result;
    CSE_CHECK("missing proof fails named and publishes nothing",
              !consensus_state_snapshot_export(db, &request, &missing_result) &&
              missing_result.status == CONSENSUS_EXPORT_MISSING_PROOF &&
              access(missing_output, F_OK) != 0);

    CSE_CHECK("seed malformed source receipt",
              cse_seed_receipt(db, hash, true));
    request.output_path = malformed_output;
    struct consensus_state_export_result malformed_result;
    CSE_CHECK("malformed receipt fails named and publishes nothing",
              !consensus_state_snapshot_export(db, &request,
                                               &malformed_result) &&
              malformed_result.status == CONSENSUS_EXPORT_MISSING_PROOF &&
              access(malformed_output, F_OK) != 0);

    reducer_frontier_test_set_compiled_anchor(-1);
    checkpoints_reset_sha3_override_for_test();
    progress_store_close();
    (void)unlink(output);
    (void)unlink(missing_output);
    (void)unlink(malformed_output);
    (void)rmdir(dir);
    return failures;
}
