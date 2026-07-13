/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * PURPOSE: Independent read-only validation of a closed consensus-state
 * candidate after its writable SQLite handle has been strictly closed. */

#include "consensus_state_snapshot_install_internal.h"

#include "coins/utxo_commitment.h"
#include "core/amount.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "jobs/reducer_frontier.h"
#include "sapling/incremental_merkle_tree.h"
#include "script/script.h"
#include "services/nullifier_backfill_service.h"
#include "storage/anchor_kv.h"
#include "storage/consensus_state_bundle_codec.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static bool candidate_invalid(struct consensus_state_candidate_result *result,
                              const char *reason)
{
    return consensus_state_candidate_fail(
        result, CONSENSUS_CANDIDATE_OUTPUT_ERROR,
        "candidate reopen validation: %s", reason);
}

static bool blob32_equal(sqlite3_stmt *stmt, int column,
                         const uint8_t expected[32])
{
    const void *blob = sqlite3_column_blob(stmt, column);
    return sqlite3_column_type(stmt, column) == SQLITE_BLOB && blob &&
           sqlite3_column_bytes(stmt, column) == 32 &&
           memcmp(blob, expected, 32) == 0;
}

static bool blob32_copy(sqlite3_stmt *stmt, int column, uint8_t out[32])
{
    const void *blob = sqlite3_column_blob(stmt, column);
    if (sqlite3_column_type(stmt, column) != SQLITE_BLOB || !blob ||
        sqlite3_column_bytes(stmt, column) != 32)
        return false;
    memcpy(out, blob, 32);
    return true;
}

static bool candidate_integrity(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, NULL) !=
        SQLITE_OK)
        return false;
    int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
    const unsigned char *text = rc == SQLITE_ROW
                                    ? sqlite3_column_text(stmt, 0) : NULL;
    bool ok = text && strcmp((const char *)text, "ok") == 0 &&
              sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    return ok;
}

struct schema_object {
    const char *type;
    const char *name;
    int column_count;
};

static bool schema_column_count(sqlite3 *db, const char *name, int expected)
{
    if (expected < 0)
        return true;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT count(*) FROM pragma_table_xinfo(?) WHERE hidden=0",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_ROW && // raw-sql-ok:read-only-introspection
              sqlite3_column_int(stmt, 0) == expected &&
              sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    return ok;
}

static bool candidate_closed_schema(sqlite3 *db)
{
    static const struct schema_object expected[] = {
        {"index", "idx_nullifiers_height", -1},
        {"index", "idx_sapling_anchors_height", -1},
        {"index", "idx_sprout_anchors_height", -1},
        {"table", "anchor_state", 2},
        {"table", "coins", 6},
        {"table", "consensus_state_bundle_proof", 8},
        {"table", "consensus_state_candidate_meta", 25},
        {"table", "consensus_state_source_receipt", 9},
        {"table", "nullifiers", 3},
        {"table", "progress_meta", 2},
        {"table", "sapling_anchors", 3},
        {"table", "sprout_anchors", 3},
        {"table", "stage_cursor", 3},
        {"table", "tip_finalize_log", 9},
        {"table", "utxo_apply_log", 9},
    };
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT type,name,sql FROM sqlite_schema "
            "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(expected) / sizeof(expected[0]); i++) {
        int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
        const char *type = rc == SQLITE_ROW
                               ? (const char *)sqlite3_column_text(stmt, 0)
                               : NULL;
        const char *name = rc == SQLITE_ROW
                               ? (const char *)sqlite3_column_text(stmt, 1)
                               : NULL;
        const char *sql = rc == SQLITE_ROW
                              ? (const char *)sqlite3_column_text(stmt, 2)
                              : NULL;
        ok = type && name && sql && strcmp(type, expected[i].type) == 0 &&
             strcmp(name, expected[i].name) == 0 &&
             schema_column_count(db, name, expected[i].column_count);
    }
    if (ok)
        ok = sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    return ok;
}

static bool candidate_meta_matches(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m,
    const uint8_t admission_receipt[32])
{
    static const char sql[] =
        "SELECT schema,height,block_hash,history_complete,activation_boundary,"
        "utxo_root,utxo_count,total_supply,anchor_digest,anchor_count,"
        "sprout_frontier_root,sprout_frontier_height,sapling_frontier_root,"
        "sapling_frontier_height,nullifier_digest,nullifier_count,"
        "sprout_source_cursor,sapling_source_cursor,nullifier_source_cursor,"
        "source_fold_cursor,proof_manifest_digest,source_digest,artifact_digest,"
        "admission_receipt_digest FROM consensus_state_candidate_meta "
        "WHERE singleton=1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
    const char *schema = rc == SQLITE_ROW
                             ? (const char *)sqlite3_column_text(stmt, 0)
                             : NULL;
    bool ok = schema &&
        strcmp(schema, CONSENSUS_STATE_CANDIDATE_SCHEMA) == 0 &&
        sqlite3_column_type(stmt, 1) == SQLITE_INTEGER &&
        sqlite3_column_int64(stmt, 1) == m->height &&
        blob32_equal(stmt, 2, m->block_hash) &&
        sqlite3_column_type(stmt, 3) == SQLITE_INTEGER &&
        sqlite3_column_int(stmt, 3) == 1 &&
        sqlite3_column_type(stmt, 4) == SQLITE_INTEGER &&
        sqlite3_column_int64(stmt, 4) == 0 &&
        blob32_equal(stmt, 5, m->utxo_root) &&
        sqlite3_column_type(stmt, 6) == SQLITE_INTEGER &&
        sqlite3_column_int64(stmt, 6) == (sqlite3_int64)m->utxo_count &&
        sqlite3_column_type(stmt, 7) == SQLITE_INTEGER &&
        sqlite3_column_int64(stmt, 7) == m->total_supply &&
        blob32_equal(stmt, 8, m->anchor_digest) &&
        sqlite3_column_type(stmt, 9) == SQLITE_INTEGER &&
        sqlite3_column_int64(stmt, 9) == (sqlite3_int64)m->anchor_count &&
        blob32_equal(stmt, 10, m->sprout_frontier_root) &&
        sqlite3_column_int64(stmt, 11) == m->sprout_frontier_height &&
        blob32_equal(stmt, 12, m->sapling_frontier_root) &&
        sqlite3_column_int64(stmt, 13) == m->sapling_frontier_height &&
        blob32_equal(stmt, 14, m->nullifier_digest) &&
        sqlite3_column_type(stmt, 15) == SQLITE_INTEGER &&
        sqlite3_column_int64(stmt, 15) == (sqlite3_int64)m->nullifier_count &&
        sqlite3_column_int64(stmt, 16) == 0 &&
        sqlite3_column_int64(stmt, 17) == 0 &&
        sqlite3_column_int64(stmt, 18) == 0 &&
        sqlite3_column_int64(stmt, 19) == m->source_fold_cursor &&
        blob32_equal(stmt, 20, m->proof_manifest_digest) &&
        blob32_equal(stmt, 21, m->source_digest) &&
        blob32_equal(stmt, 22, m->artifact_digest) &&
        blob32_equal(stmt, 23, admission_receipt) &&
        sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    return ok;
}

static bool candidate_source_receipt(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m,
    struct consensus_state_source_receipt *receipt)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT schema,source_tree_root,running_binary_digest,"
            "toolchain_digest,chain_corpus_digest,producer_commit,fold_cursor,"
            "receipt_digest FROM consensus_state_source_receipt "
            "WHERE singleton=1",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    memset(receipt, 0, sizeof(*receipt));
    int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
    const char *schema = rc == SQLITE_ROW
                             ? (const char *)sqlite3_column_text(stmt, 0)
                             : NULL;
    const char *commit = rc == SQLITE_ROW
                             ? (const char *)sqlite3_column_text(stmt, 5)
                             : NULL;
    bool ok = schema && commit &&
        strcmp(schema, CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA) == 0 &&
        sqlite3_column_bytes(stmt, 5) == 40 &&
        blob32_copy(stmt, 1, receipt->source_tree_root) &&
        blob32_copy(stmt, 2, receipt->running_binary_digest) &&
        blob32_copy(stmt, 3, receipt->toolchain_digest) &&
        blob32_copy(stmt, 4, receipt->chain_corpus_digest) &&
        blob32_copy(stmt, 7, receipt->receipt_digest) &&
        sqlite3_column_type(stmt, 6) == SQLITE_INTEGER;
    if (ok) {
        memcpy(receipt->producer_commit, commit, 40);
        receipt->producer_commit[40] = '\0';
        receipt->fold_cursor = sqlite3_column_int64(stmt, 6);
        uint8_t digest[32];
        consensus_state_source_receipt_digest(receipt, digest);
        ok = receipt->fold_cursor == m->source_fold_cursor &&
             memcmp(digest, receipt->receipt_digest, 32) == 0 &&
             memcmp(digest, m->source_digest, 32) == 0 &&
             sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool candidate_proofs(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m,
    const struct consensus_state_source_receipt *receipt)
{
    static const char *const names[CONSENSUS_STATE_BUNDLE_PROOF_COUNT] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
    };
    static const bool hash_bound[CONSENSUS_STATE_BUNDLE_PROOF_COUNT] = {
        true, true, true, false, true, true, false, false,
    };
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ordinal,component,cursor,first_height,last_height,row_count,"
            "hash_bound_count,component_digest "
            "FROM consensus_state_bundle_proof ORDER BY ordinal",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    struct consensus_state_bundle_proof_summary
        proofs[CONSENSUS_STATE_BUNDLE_PROOF_COUNT];
    memset(proofs, 0, sizeof(proofs));
    uint64_t rows = (uint64_t)m->height + 1;
    bool ok = true;
    for (size_t i = 0; ok && i < CONSENSUS_STATE_BUNDLE_PROOF_COUNT; i++) {
        int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
        const char *component = rc == SQLITE_ROW
            ? (const char *)sqlite3_column_text(stmt, 1) : NULL;
        const void *digest = rc == SQLITE_ROW
            ? sqlite3_column_blob(stmt, 7) : NULL;
        int64_t cursor = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 2) : -1;
        uint64_t minimum = i == 7 ? (uint64_t)m->height : rows;
        ok = component && digest &&
             sqlite3_column_int64(stmt, 0) == (sqlite3_int64)i &&
             strcmp(component, names[i]) == 0 && cursor >= 0 &&
             (uint64_t)cursor >= minimum &&
             (i != 6 || (uint64_t)cursor == rows) &&
             (i != 7 || (uint64_t)cursor <= minimum + 1) &&
             sqlite3_column_int64(stmt, 3) == 0 &&
             sqlite3_column_int64(stmt, 4) == m->height &&
             sqlite3_column_int64(stmt, 5) == (sqlite3_int64)rows &&
             sqlite3_column_int64(stmt, 6) ==
                 (sqlite3_int64)(hash_bound[i] ? rows : 0) &&
             sqlite3_column_bytes(stmt, 7) == 32;
        if (ok) {
            snprintf(proofs[i].component, sizeof(proofs[i].component), "%s",
                     names[i]);
            proofs[i].cursor = (uint64_t)cursor;
            proofs[i].first_height = 0;
            proofs[i].last_height = m->height;
            proofs[i].row_count = rows;
            proofs[i].hash_bound_count = hash_bound[i] ? rows : 0;
            memcpy(proofs[i].component_digest, digest, 32);
        }
    }
    if (ok)
        ok = sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    uint8_t digest[32];
    if (ok) {
        consensus_state_bundle_proof_manifest_digest(
            proofs, CONSENSUS_STATE_BUNDLE_PROOF_COUNT, digest);
        ok = memcmp(digest, m->proof_manifest_digest, 32) == 0 &&
             memcmp(proofs[0].component_digest,
                    receipt->chain_corpus_digest, 32) == 0;
    }
    return ok;
}

static bool candidate_coins(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT txid,vout,value,script,height,is_coinbase "
            "FROM coins ORDER BY txid,vout",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    struct sha3_256_ctx context;
    sha3_256_init(&context);
    uint64_t count = 0;
    int64_t supply = 0;
    bool ok = true;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { // raw-sql-ok:read-only-introspection
        const uint8_t *txid = sqlite3_column_blob(stmt, 0);
        const uint8_t *script = sqlite3_column_blob(stmt, 3);
        int64_t vout = sqlite3_column_int64(stmt, 1);
        int64_t value = sqlite3_column_int64(stmt, 2);
        int script_size = sqlite3_column_bytes(stmt, 3);
        int64_t height = sqlite3_column_int64(stmt, 4);
        int coinbase = sqlite3_column_int(stmt, 5);
        if (sqlite3_column_type(stmt, 0) != SQLITE_BLOB || !txid ||
            sqlite3_column_bytes(stmt, 0) != 32 || vout < 0 ||
            vout > UINT32_MAX || !MoneyRange(value) ||
            supply > MAX_MONEY - value ||
            sqlite3_column_type(stmt, 3) != SQLITE_BLOB || script_size < 0 ||
            script_size > MAX_SCRIPT_SIZE || height < 0 || height > m->height ||
            (coinbase != 0 && coinbase != 1) || count == UINT64_MAX) {
            ok = false;
            break;
        }
        utxo_commitment_sha3_write_record(
            &context, txid, (uint32_t)vout, value,
            script_size ? script : NULL, (uint32_t)script_size,
            (uint32_t)height, (uint8_t)coinbase);
        supply += value;
        count++;
    }
    if (rc != SQLITE_DONE)
        ok = false;
    sqlite3_finalize(stmt);
    uint8_t root[32];
    sha3_256_finalize(&context, root);
    return ok && count == m->utxo_count && supply == m->total_supply &&
           memcmp(root, m->utxo_root, 32) == 0;
}

static bool candidate_anchors(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT pool,anchor,height,tree FROM ("
            "SELECT 0 AS pool,anchor,height,tree FROM sprout_anchors UNION ALL "
            "SELECT 1 AS pool,anchor,height,tree FROM sapling_anchors) "
            "ORDER BY pool,anchor",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    struct sha3_256_ctx context;
    consensus_state_bundle_anchor_digest_begin(&context);
    int64_t frontier_height[2] = {-1, -1};
    uint8_t frontier_root[2][32] = {{0}, {0}};
    uint64_t count = 0;
    bool ok = true;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { // raw-sql-ok:read-only-introspection
        int pool = sqlite3_column_int(stmt, 0);
        const uint8_t *root = sqlite3_column_blob(stmt, 1);
        int64_t height = sqlite3_column_int64(stmt, 2);
        const uint8_t *tree_blob = sqlite3_column_blob(stmt, 3);
        int tree_size = sqlite3_column_bytes(stmt, 3);
        if ((pool != 0 && pool != 1) || !root ||
            sqlite3_column_bytes(stmt, 1) != 32 || !tree_blob ||
            tree_size <= 0 || height < 0 || height > m->height ||
            count == UINT64_MAX) {
            ok = false;
            break;
        }
        struct incremental_merkle_tree tree;
        if (pool == ANCHOR_POOL_SPROUT)
            sprout_tree_init(&tree);
        else
            sapling_tree_init(&tree);
        struct byte_stream stream;
        stream_init_from_data(&stream, tree_blob, (size_t)tree_size);
        struct uint256 computed;
        bool parsed = incremental_tree_deserialize(&tree, &stream) &&
                      stream_remaining(&stream) == 0;
        if (parsed)
            incremental_tree_root(&tree, &computed);
        if (!parsed || memcmp(computed.data, root, 32) != 0) {
            ok = false;
            break;
        }
        consensus_state_bundle_anchor_digest_row(
            &context, (uint8_t)pool, root, (uint64_t)height, tree_blob,
            (uint32_t)tree_size);
        if (height > frontier_height[pool]) {
            frontier_height[pool] = height;
            memcpy(frontier_root[pool], root, 32);
        }
        count++;
    }
    if (rc != SQLITE_DONE)
        ok = false;
    sqlite3_finalize(stmt);
    uint8_t digest[32];
    sha3_256_finalize(&context, digest);
    return ok && count == m->anchor_count &&
        frontier_height[0] == m->sprout_frontier_height &&
        frontier_height[1] == m->sapling_frontier_height &&
        memcmp(frontier_root[0], m->sprout_frontier_root, 32) == 0 &&
        memcmp(frontier_root[1], m->sapling_frontier_root, 32) == 0 &&
        memcmp(digest, m->anchor_digest, 32) == 0;
}

static bool candidate_nullifiers(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT pool,nf,height FROM nullifiers ORDER BY pool,nf",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    struct sha3_256_ctx context;
    consensus_state_bundle_nullifier_digest_begin(&context);
    uint64_t count = 0;
    bool ok = true;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { // raw-sql-ok:read-only-introspection
        int pool = sqlite3_column_int(stmt, 0);
        const uint8_t *nf = sqlite3_column_blob(stmt, 1);
        int64_t height = sqlite3_column_int64(stmt, 2);
        if ((pool != 0 && pool != 1) || !nf ||
            sqlite3_column_bytes(stmt, 1) != 32 || height < 0 ||
            height > m->height || count == UINT64_MAX) {
            ok = false;
            break;
        }
        consensus_state_bundle_nullifier_digest_row(
            &context, (uint8_t)pool, nf, (uint64_t)height);
        count++;
    }
    if (rc != SQLITE_DONE)
        ok = false;
    sqlite3_finalize(stmt);
    uint8_t digest[32];
    sha3_256_finalize(&context, digest);
    return ok && count == m->nullifier_count &&
           memcmp(digest, m->nullifier_digest, 32) == 0;
}

static int64_t le64_decode(const uint8_t value[8])
{
    uint64_t decoded = 0;
    for (size_t i = 0; i < 8; i++)
        decoded |= (uint64_t)value[i] << (8u * i);
    return (int64_t)decoded;
}

static bool candidate_meta_value(sqlite3 *db, const char *key,
                                 uint8_t *value, size_t capacity,
                                 size_t *size_out)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM progress_meta WHERE key=?",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
    int size = rc == SQLITE_ROW ? sqlite3_column_bytes(stmt, 0) : -1;
    const void *blob = rc == SQLITE_ROW ? sqlite3_column_blob(stmt, 0) : NULL;
    bool ok = rc == SQLITE_ROW && size >= 0 && (size_t)size <= capacity &&
              (size == 0 || blob);
    if (ok && size > 0)
        memcpy(value, blob, (size_t)size);
    if (ok)
        ok = sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    if (ok)
        *size_out = (size_t)size;
    return ok;
}

static bool candidate_reducer_state(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m)
{
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(db,
        "SELECT pool,activation_cursor FROM anchor_state ORDER BY pool",
        -1, &stmt, NULL) == SQLITE_OK;
    for (int pool = 0; ok && pool < 2; pool++) {
        int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
        ok = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == pool &&
             sqlite3_column_int64(stmt, 1) == 0;
    }
    if (ok)
        ok = sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    if (stmt)
        sqlite3_finalize(stmt);
    if (!ok)
        return false;
    uint8_t value[32];
    size_t size = 0;
#define META_EQ(key, length, expression) \
    (candidate_meta_value(db, (key), value, sizeof(value), &size) && \
     size == (length) && (expression))
    ok = META_EQ("coins_applied_height", 8,
                 le64_decode(value) == (int64_t)m->height + 1) &&
         META_EQ("coins_kv_migration_complete", 1, value[0] == 1) &&
         META_EQ("coins_kv_self_folded", 1, value[0] == 1) &&
         META_EQ(NULLIFIER_BACKFILL_ACTIVATION_KEY, 1, value[0] == '0') &&
         META_EQ(REDUCER_TRUSTED_BASE_HEIGHT_KEY, 8,
                 le64_decode(value) == m->height) &&
         META_EQ(REDUCER_TRUSTED_BASE_HASH_KEY, 32,
                 memcmp(value, m->block_hash, 32) == 0);
#undef META_EQ
    if (!ok)
        return false;
    if (sqlite3_prepare_v2(db,
            "SELECT count(*) FROM progress_meta", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    ok = sqlite3_step(stmt) == SQLITE_ROW && // raw-sql-ok:read-only-introspection
         sqlite3_column_int64(stmt, 0) == 6;
    sqlite3_finalize(stmt);
    if (!ok)
        return false;

    static const char *const stages[] = {
        "body_fetch", "body_persist", "header_admit", "proof_validate",
        "script_validate", "tip_finalize", "utxo_apply", "validate_headers",
    };
    if (sqlite3_prepare_v2(db,
            "SELECT name,cursor,updated_at FROM stage_cursor ORDER BY name",
            -1, &stmt, NULL) != SQLITE_OK)
        return false;
    for (size_t i = 0; ok && i < sizeof(stages) / sizeof(stages[0]); i++) {
        int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
        const char *name = rc == SQLITE_ROW
                               ? (const char *)sqlite3_column_text(stmt, 0)
                               : NULL;
        int64_t want = strcmp(stages[i], "tip_finalize") == 0
                           ? m->height : (int64_t)m->height + 1;
        ok = name && strcmp(name, stages[i]) == 0 &&
             sqlite3_column_int64(stmt, 1) == want &&
             sqlite3_column_int64(stmt, 2) == 0;
    }
    if (ok)
        ok = sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    if (!ok)
        return false;

    if (sqlite3_prepare_v2(db,
            "SELECT height,status,ok,spent_count,added_count,total_value_delta,"
            "first_failure_kind,first_failure_detail,applied_at "
            "FROM utxo_apply_log", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    int rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
    const char *status = rc == SQLITE_ROW
                             ? (const char *)sqlite3_column_text(stmt, 1) : NULL;
    ok = status && strcmp(status, "anchor") == 0 &&
         sqlite3_column_int64(stmt, 0) == m->height &&
         sqlite3_column_int(stmt, 2) == 1 &&
         sqlite3_column_int64(stmt, 3) == 0 &&
         sqlite3_column_int64(stmt, 4) == 0 &&
         sqlite3_column_int64(stmt, 5) == 0 &&
         sqlite3_column_type(stmt, 6) == SQLITE_NULL &&
         sqlite3_column_type(stmt, 7) == SQLITE_NULL &&
         sqlite3_column_int64(stmt, 8) == 0 &&
         sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    if (!ok)
        return false;
    if (sqlite3_prepare_v2(db,
            "SELECT height,status,ok,work_delta_high,work_delta_low,"
            "utxo_size_after,reorg_depth,finalized_at,tip_hash "
            "FROM tip_finalize_log", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    rc = sqlite3_step(stmt); // raw-sql-ok:read-only-introspection
    status = rc == SQLITE_ROW
                 ? (const char *)sqlite3_column_text(stmt, 1) : NULL;
    ok = status && strcmp(status, "anchor") == 0 &&
         sqlite3_column_int64(stmt, 0) == m->height &&
         sqlite3_column_int(stmt, 2) == 1 &&
         sqlite3_column_int64(stmt, 3) == 0 &&
         sqlite3_column_int64(stmt, 4) == 0 &&
         sqlite3_column_int64(stmt, 5) == (sqlite3_int64)m->utxo_count &&
         sqlite3_column_int64(stmt, 6) == 0 &&
         sqlite3_column_int64(stmt, 7) == 0 &&
         blob32_equal(stmt, 8, m->block_hash) &&
         sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(stmt);
    return ok;
}

bool consensus_state_candidate_validate_reopened(
    sqlite3 *candidate,
    const struct consensus_state_bundle_manifest *expected,
    const uint8_t expected_admission_receipt[32],
    struct consensus_state_candidate_result *result)
{
    if (!candidate || !expected || !expected_admission_receipt)
        return candidate_invalid(result, "null validation input");
    struct consensus_state_source_receipt receipt;
    if (!candidate_integrity(candidate))
        return candidate_invalid(result, "integrity check failed");
    if (!candidate_closed_schema(candidate))
        return candidate_invalid(result, "schema is not the closed set");
    if (!candidate_meta_matches(candidate, expected,
                                expected_admission_receipt))
        return candidate_invalid(result, "manifest/admission receipt mismatch");
    if (!candidate_source_receipt(candidate, expected, &receipt) ||
        !candidate_proofs(candidate, expected, &receipt))
        return candidate_invalid(result, "source proof provenance mismatch");
    if (!candidate_coins(candidate, expected))
        return candidate_invalid(result, "UTXO root/count/supply mismatch");
    if (!candidate_anchors(candidate, expected))
        return candidate_invalid(result, "anchor digest/frontier mismatch");
    if (!candidate_nullifiers(candidate, expected))
        return candidate_invalid(result, "nullifier digest/count mismatch");
    if (!candidate_reducer_state(candidate, expected))
        return candidate_invalid(result, "reducer cursor/log/base mismatch");
    return true;
}
