/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Strict validation for external zcl.consensus_state_bundle.v1 files. */

#include "config/consensus_state_bundle_validate.h"
#include "consensus_state_sqlite_text.h"

#include "coins/utxo_commitment.h"
#include "core/amount.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "sapling/incremental_merkle_tree.h"
#include "script/script.h"
#include "storage/anchor_kv.h"
#include "util/log_macros.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VALIDATE_SUBSYS "consensus_bundle_validate"

static bool validation_fail(struct consensus_state_install_result *result,
                            enum consensus_state_install_status status,
                            const char *fmt, ...)
{
    char reason[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    if (result) {
        result->status = status;
        snprintf(result->reason, sizeof(result->reason), "%s", reason);
    }
    LOG_WARN(VALIDATE_SUBSYS, "%s", reason);
    return false;
}

static bool digest_nonzero(const uint8_t digest[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++)
        any |= digest[i];
    return any != 0;
}

static bool integrity_check(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check",
                           -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL(VALIDATE_SUBSYS, "integrity_check prepare: %s",
                 sqlite3_errmsg(db));
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    bool ok = rc == SQLITE_ROW &&
              consensus_state_sqlite_text_equal(st, 0, "ok");
    sqlite3_finalize(st);
    if (!ok)
        LOG_WARN(VALIDATE_SUBSYS, "integrity_check failed");
    return ok;
}

struct canonical_column {
    const char *name;
    const char *type;
    int primary_key_ordinal;
};

struct canonical_table {
    const char *name;
    const struct canonical_column *columns;
    size_t column_count;
    const char *required_sql_token;
    const char *required_sql_token_2;
};

#define COL(n, t, pk) { (n), (t), (pk) }
static const struct canonical_column k_bundle_meta_columns[] = {
    COL("singleton","INTEGER",1), COL("schema","TEXT",0),
    COL("height","INTEGER",0), COL("block_hash","BLOB",0),
    COL("history_complete","INTEGER",0),
    COL("source_clean","INTEGER",0),
    COL("validation_profile","INTEGER",0),
    COL("activation_boundary","INTEGER",0), COL("utxo_root","BLOB",0),
    COL("utxo_count","INTEGER",0), COL("total_supply","INTEGER",0),
    COL("anchor_digest","BLOB",0), COL("anchor_count","INTEGER",0),
    COL("sprout_frontier_root","BLOB",0),
    COL("sprout_frontier_height","INTEGER",0),
    COL("sapling_frontier_root","BLOB",0),
    COL("sapling_frontier_height","INTEGER",0),
    COL("nullifier_digest","BLOB",0), COL("nullifier_count","INTEGER",0),
    COL("sprout_source_cursor","INTEGER",0),
    COL("sapling_source_cursor","INTEGER",0),
    COL("nullifier_source_cursor","INTEGER",0),
    COL("source_fold_cursor","INTEGER",0),
    COL("proof_manifest_digest","BLOB",0), COL("source_digest","BLOB",0),
    COL("artifact_digest","BLOB",0),
};
static const struct canonical_column k_source_receipt_columns[] = {
    COL("singleton","INTEGER",1), COL("schema","TEXT",0),
    COL("source_epoch_digest","BLOB",0),
    COL("source_tree_root","BLOB",0),
    COL("running_binary_digest","BLOB",0),
    COL("toolchain_digest","BLOB",0), COL("build_inputs_digest","BLOB",0),
    COL("chain_corpus_digest","BLOB",0), COL("source_clean","INTEGER",0),
    COL("validation_profile","INTEGER",0),
    COL("producer_commit","TEXT",0), COL("fold_cursor","INTEGER",0),
    COL("receipt_digest","BLOB",0),
};
static const struct canonical_column k_bundle_proof_columns[] = {
    COL("ordinal","INTEGER",1), COL("component","TEXT",0),
    COL("cursor","INTEGER",0), COL("first_height","INTEGER",0),
    COL("last_height","INTEGER",0), COL("row_count","INTEGER",0),
    COL("hash_bound_count","INTEGER",0),
    COL("component_digest","BLOB",0),
};
static const struct canonical_column k_coins_columns[] = {
    COL("txid","BLOB",1), COL("vout","INTEGER",2),
    COL("value","INTEGER",0), COL("script","BLOB",0),
    COL("height","INTEGER",0), COL("is_coinbase","INTEGER",0),
};
static const struct canonical_column k_anchors_columns[] = {
    COL("pool","INTEGER",1), COL("anchor","BLOB",2),
    COL("height","INTEGER",0), COL("tree","BLOB",0),
};
static const struct canonical_column k_nullifiers_columns[] = {
    COL("pool","INTEGER",1), COL("nf","BLOB",2),
    COL("height","INTEGER",0),
};
#undef COL

static const struct canonical_table k_canonical_tables[] = {
    {"anchors", k_anchors_columns,
     sizeof(k_anchors_columns) / sizeof(k_anchors_columns[0]),
     "CHECK(pool IN(0,1))", "UNIQUE(pool,height)"},
    {"bundle_meta", k_bundle_meta_columns,
     sizeof(k_bundle_meta_columns) / sizeof(k_bundle_meta_columns[0]),
     "CHECK(singleton=1)", NULL},
    {"bundle_proof", k_bundle_proof_columns,
     sizeof(k_bundle_proof_columns) / sizeof(k_bundle_proof_columns[0]),
     "UNIQUE", NULL},
    {"coins", k_coins_columns,
     sizeof(k_coins_columns) / sizeof(k_coins_columns[0]), "WITHOUT ROWID",
     NULL},
    {"nullifiers", k_nullifiers_columns,
     sizeof(k_nullifiers_columns) / sizeof(k_nullifiers_columns[0]),
     "CHECK(pool IN(0,1))", NULL},
    {"source_receipt", k_source_receipt_columns,
     sizeof(k_source_receipt_columns) / sizeof(k_source_receipt_columns[0]),
     "CHECK(singleton=1)", NULL},
};

static bool canonical_table_columns(sqlite3 *db,
                                    const struct canonical_table *table)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT name,type,pk,hidden FROM pragma_table_xinfo(?) "
                "ORDER BY cid", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, table->name, -1, SQLITE_STATIC);
    bool ok = true;
    for (size_t i = 0; i < table->column_count; i++) {
        int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
        if (rc != SQLITE_ROW ||
            !consensus_state_sqlite_text_equal(
                st, 0, table->columns[i].name) ||
            !consensus_state_sqlite_text_equal(
                st, 1, table->columns[i].type) ||
            sqlite3_column_type(st, 2) != SQLITE_INTEGER ||
            sqlite3_column_int(st, 2) !=
                table->columns[i].primary_key_ordinal ||
            sqlite3_column_type(st, 3) != SQLITE_INTEGER ||
            sqlite3_column_int(st, 3) != 0) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(st);
    return ok;
}

static bool canonical_schema(sqlite3 *db,
                             struct consensus_state_install_result *result)
{
    sqlite3_stmt *st = NULL;
    static const char sql[] =
        "SELECT type,name,sql FROM sqlite_schema "
        "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "bundle schema catalog unreadable");
    bool ok = true;
    for (size_t i = 0;
         i < sizeof(k_canonical_tables) / sizeof(k_canonical_tables[0]); i++) {
        int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
        const struct canonical_table *want = &k_canonical_tables[i];
        int definition_type = rc == SQLITE_ROW
            ? sqlite3_column_type(st, 2) : SQLITE_NULL;
        const char *definition = definition_type == SQLITE_TEXT
            ? (const char *)sqlite3_column_text(st, 2) : NULL;
        if (rc != SQLITE_ROW ||
            !consensus_state_sqlite_text_equal(st, 0, "table") ||
            !consensus_state_sqlite_text_equal(st, 1, want->name) ||
            !definition ||
            sqlite3_column_bytes(st, 2) != (int)strlen(definition) ||
            !strstr(definition, want->required_sql_token) ||
            (want->required_sql_token_2 &&
             !strstr(definition, want->required_sql_token_2)) ||
            ((strcmp(want->name, "anchors") == 0 ||
              strcmp(want->name, "nullifiers") == 0) &&
             !strstr(definition, "WITHOUT ROWID")) ||
            !canonical_table_columns(db, want)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(st);
    if (!ok)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "bundle schema is not the canonical closed set");

    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM bundle_meta", -1,
                           &st, NULL) != SQLITE_OK)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "bundle_meta cardinality unavailable");
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    ok = rc == SQLITE_ROW && sqlite3_column_int64(st, 0) == 1 &&
         sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(st);
    if (!ok)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "bundle_meta must contain exactly one row");
    return true;
}

static bool copy_blob32(sqlite3_stmt *st, int column, uint8_t out[32])
{
    if (sqlite3_column_type(st, column) != SQLITE_BLOB)
        return false;
    const void *blob = sqlite3_column_blob(st, column);
    if (!blob || sqlite3_column_bytes(st, column) != 32)
        return false;
    memcpy(out, blob, 32);
    return true;
}

static bool copy_int64_exact(sqlite3_stmt *st, int column, int64_t *out)
{
    if (sqlite3_column_type(st, column) != SQLITE_INTEGER)
        return false;
    *out = sqlite3_column_int64(st, column);
    return true;
}

static bool read_manifest(sqlite3 *db,
                          struct consensus_state_bundle_manifest *m,
                          struct consensus_state_install_result *r)
{
    static const char *const sql =
        "SELECT schema,height,block_hash,history_complete,source_clean,"
        "validation_profile,activation_boundary,"
        "utxo_root,utxo_count,total_supply,anchor_digest,anchor_count,"
        "sprout_frontier_root,sprout_frontier_height,"
        "sapling_frontier_root,sapling_frontier_height,"
        "nullifier_digest,nullifier_count,sprout_source_cursor,"
        "sapling_source_cursor,nullifier_source_cursor,source_fold_cursor,"
        "proof_manifest_digest,source_digest,artifact_digest "
        "FROM bundle_meta WHERE singleton=1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "bundle_meta missing or malformed");
    memset(m, 0, sizeof(*m));
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    bool ok = rc == SQLITE_ROW &&
              consensus_state_sqlite_text_equal(
                  st, 0, CONSENSUS_STATE_BUNDLE_SCHEMA) &&
              sqlite3_column_type(st, 1) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 3) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 4) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 5) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 6) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 8) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 9) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 11) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 13) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 15) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 17) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 18) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 19) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 20) == SQLITE_INTEGER &&
              sqlite3_column_type(st, 21) == SQLITE_INTEGER &&
              copy_blob32(st, 2, m->block_hash) &&
              copy_blob32(st, 7, m->utxo_root) &&
              copy_blob32(st, 10, m->anchor_digest) &&
              copy_blob32(st, 12, m->sprout_frontier_root) &&
              copy_blob32(st, 14, m->sapling_frontier_root) &&
              copy_blob32(st, 16, m->nullifier_digest) &&
              copy_blob32(st, 22, m->proof_manifest_digest) &&
              copy_blob32(st, 23, m->source_digest) &&
              copy_blob32(st, 24, m->artifact_digest);
    if (ok) {
        int64_t height = sqlite3_column_int64(st, 1);
        int history = sqlite3_column_int(st, 3);
        int source_clean = sqlite3_column_int(st, 4);
        int validation_profile = sqlite3_column_int(st, 5);
        int64_t counts[3] = {
            sqlite3_column_int64(st, 8), sqlite3_column_int64(st, 11),
            sqlite3_column_int64(st, 17),
        };
        int64_t supply = sqlite3_column_int64(st, 9);
        int64_t sprout_frontier_height = sqlite3_column_int64(st, 13);
        int64_t sapling_frontier_height = sqlite3_column_int64(st, 15);
        ok = height >= 0 && height < INT32_MAX && MoneyRange(supply) &&
             (history == 0 || history == 1) &&
             (source_clean == 0 || source_clean == 1) &&
             (validation_profile == CONSENSUS_STATE_VALIDATION_FULL ||
              validation_profile ==
                  CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD) &&
             counts[0] > 0 &&
             counts[1] >= 0 && counts[2] >= 0 &&
             sprout_frontier_height >= 0 &&
             sprout_frontier_height <= height &&
             sapling_frontier_height >= 0 &&
             sapling_frontier_height <= height &&
             digest_nonzero(m->sprout_frontier_root) &&
             digest_nonzero(m->sapling_frontier_root);
        if (ok) {
            m->height = (int32_t)height;
            m->history_complete = history == 1;
            m->source_clean = source_clean == 1;
            m->validation_profile = (uint8_t)validation_profile;
            m->activation_boundary = sqlite3_column_int64(st, 6);
            m->utxo_count = (uint64_t)counts[0];
            m->total_supply = supply;
            m->anchor_count = (uint64_t)counts[1];
            m->nullifier_count = (uint64_t)counts[2];
            m->sprout_frontier_height = sprout_frontier_height;
            m->sapling_frontier_height = sapling_frontier_height;
            m->sprout_source_cursor = sqlite3_column_int64(st, 18);
            m->sapling_source_cursor = sqlite3_column_int64(st, 19);
            m->nullifier_source_cursor = sqlite3_column_int64(st, 20);
            m->source_fold_cursor = sqlite3_column_int64(st, 21);
        }
    }
    sqlite3_finalize(st);
    if (!ok || !digest_nonzero(m->block_hash))
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "bundle_meta has invalid schema/types/bounds");
    if (m->history_complete) {
        if (m->activation_boundary != 0 || m->sprout_source_cursor != 0 ||
            m->sapling_source_cursor != 0 ||
            m->nullifier_source_cursor != 0 ||
            m->source_fold_cursor != (int64_t)m->height + 1 ||
            !digest_nonzero(m->proof_manifest_digest) ||
            !digest_nonzero(m->source_digest) ||
            !digest_nonzero(m->artifact_digest))
            return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                                   "complete history lacks genesis cursors/provenance");
    } else if (m->activation_boundary <= 0 ||
               m->activation_boundary > m->height ||
               m->sprout_source_cursor < 0 ||
               m->sapling_source_cursor < 0 ||
               m->nullifier_source_cursor < 0 ||
               m->source_fold_cursor < 0 ||
               m->source_fold_cursor > (int64_t)m->height + 1) {
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "current-only history lacks positive boundary");
    }
    return true;
}

static bool validate_source_receipt(
    sqlite3 *db, const struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_source_receipt *receipt_out,
    struct consensus_state_install_result *result)
{
    static const char sql[] =
        "SELECT singleton,schema,source_epoch_digest,source_tree_root,"
        "running_binary_digest,toolchain_digest,build_inputs_digest,"
        "chain_corpus_digest,source_clean,validation_profile,producer_commit,"
        "fold_cursor,receipt_digest FROM source_receipt ORDER BY singleton";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "source_receipt missing or malformed");
    struct consensus_state_source_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    int commit_type = rc == SQLITE_ROW ? sqlite3_column_type(st, 10)
                                       : SQLITE_NULL;
    const unsigned char *commit = commit_type == SQLITE_TEXT
                                      ? sqlite3_column_text(st, 10) : NULL;
    int commit_len = commit ? sqlite3_column_bytes(st, 10) : -1;
    const unsigned char *schema =
        rc == SQLITE_ROW && sqlite3_column_type(st, 1) == SQLITE_TEXT
            ? sqlite3_column_text(st, 1) : NULL;
    int schema_len = schema ? sqlite3_column_bytes(st, 1) : -1;
    uint8_t receipt_version = CONSENSUS_STATE_SOURCE_RECEIPT_INVALID;
    bool schema_ok = schema && schema_len >= 0 &&
        consensus_state_source_receipt_schema_version(
            (const char *)schema, (size_t)schema_len, &receipt_version);
    bool legacy_v1 = schema_ok &&
        receipt_version == CONSENSUS_STATE_SOURCE_RECEIPT_V1;
    bool ok = rc == SQLITE_ROW && commit &&
              sqlite3_column_type(st, 0) == SQLITE_INTEGER &&
              sqlite3_column_int(st, 0) == 1 &&
              schema_ok &&
              copy_blob32(st, 2, receipt.source_epoch_digest) &&
              copy_blob32(st, 3, receipt.source_tree_root) &&
              copy_blob32(st, 4, receipt.running_binary_digest) &&
              copy_blob32(st, 5, receipt.toolchain_digest) &&
              copy_blob32(st, 6, receipt.build_inputs_digest) &&
              copy_blob32(st, 7, receipt.chain_corpus_digest) &&
              sqlite3_column_type(st, 8) == SQLITE_INTEGER &&
              (sqlite3_column_int(st, 8) == 0 ||
               sqlite3_column_int(st, 8) == 1) &&
              sqlite3_column_type(st, 9) == SQLITE_INTEGER &&
              (sqlite3_column_int(st, 9) == CONSENSUS_STATE_VALIDATION_FULL ||
               sqlite3_column_int(st, 9) ==
                   CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD) &&
              commit_type == SQLITE_TEXT &&
              commit_len >= 0 &&
              consensus_state_source_receipt_commit_valid(
                  receipt_version, (const char *)commit,
                  (size_t)commit_len) &&
              sqlite3_column_type(st, 11) == SQLITE_INTEGER &&
              copy_blob32(st, 12, receipt.receipt_digest);
    if (ok) {
        receipt.schema_version = receipt_version;
        memcpy(receipt.producer_commit, commit, (size_t)commit_len);
        receipt.producer_commit[commit_len] = '\0';
        receipt.source_clean = sqlite3_column_int(st, 8) == 1;
        receipt.validation_profile = (uint8_t)sqlite3_column_int(st, 9);
        receipt.fold_cursor = sqlite3_column_int64(st, 11);
        rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
        ok = rc == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    /* The v1 codec remains readable for historical tooling, but its Git-SHA-1-
     * derived source claim is never sufficient authority for installation. */
    if (legacy_v1)
        return validation_fail(
            result, CONSENSUS_INSTALL_REFUSED,
            "legacy v1 source receipt is inspection-only and cannot install");
    uint8_t recomputed[32];
    uint8_t source_epoch[32];
    if (ok) {
        consensus_state_source_epoch_digest(&receipt, source_epoch);
        consensus_state_source_receipt_digest(&receipt, recomputed);
        ok = digest_nonzero(receipt.source_epoch_digest) &&
             digest_nonzero(receipt.source_tree_root) &&
             digest_nonzero(receipt.running_binary_digest) &&
             digest_nonzero(receipt.toolchain_digest) &&
             digest_nonzero(receipt.build_inputs_digest) &&
             digest_nonzero(receipt.chain_corpus_digest) &&
             consensus_state_source_receipt_commit_valid(
                 receipt.schema_version, receipt.producer_commit,
                 strnlen(receipt.producer_commit,
                         sizeof(receipt.producer_commit))) &&
             receipt.fold_cursor == manifest->source_fold_cursor &&
             receipt.source_clean == manifest->source_clean &&
             receipt.validation_profile == manifest->validation_profile &&
             memcmp(source_epoch, receipt.source_epoch_digest, 32) == 0 &&
             memcmp(recomputed, receipt.receipt_digest, 32) == 0 &&
             memcmp(recomputed, manifest->source_digest, 32) == 0;
    }
    if (!ok)
        return validation_fail(
            result, CONSENSUS_INSTALL_REFUSED,
            "source receipt types/digest/fold binding mismatch");
    /* The source-tree and toolchain values are producer claims bound to this
     * receipt and its known executable digest. They are not, by themselves,
     * proof that an independent builder reproduced the source epoch. */
    if (receipt_out)
        *receipt_out = receipt;
    return true;
}

static bool validate_bundle_proof(
    sqlite3 *db, const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_source_receipt *receipt,
    struct consensus_state_install_result *result)
{
    static const char *const names[CONSENSUS_STATE_BUNDLE_PROOF_COUNT] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
    };
    static const bool hash_bound[CONSENSUS_STATE_BUNDLE_PROOF_COUNT] = {
        true, true, true, false, true, true, true, false,
    };
    static const char sql[] =
        "SELECT ordinal,component,cursor,first_height,last_height,row_count,"
        "hash_bound_count,component_digest FROM bundle_proof ORDER BY ordinal";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "bundle_proof missing or malformed");
    struct consensus_state_bundle_proof_summary
        proofs[CONSENSUS_STATE_BUNDLE_PROOF_COUNT];
    memset(proofs, 0, sizeof(proofs));
    bool ok = true;
    uint64_t expected_rows = (uint64_t)manifest->height + 1;
    for (size_t i = 0; i < CONSENSUS_STATE_BUNDLE_PROOF_COUNT; i++) {
        int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
        int64_t ordinal = -1, cursor = -1, first = -1, last = -1;
        int64_t rows = -1, hashes = -1;
        if (rc != SQLITE_ROW ||
            !copy_int64_exact(st, 0, &ordinal) || ordinal != (int64_t)i ||
            !consensus_state_sqlite_text_equal(st, 1, names[i]) ||
            !copy_int64_exact(st, 2, &cursor) || cursor < 0 ||
            !copy_int64_exact(st, 3, &first) || first != 0 ||
            !copy_int64_exact(st, 4, &last) || last != manifest->height ||
            !copy_int64_exact(st, 5, &rows) || rows < 0 ||
            (uint64_t)rows != expected_rows ||
            !copy_int64_exact(st, 6, &hashes) || hashes < 0 ||
            (uint64_t)hashes != (hash_bound[i] ? expected_rows : 0) ||
            !copy_blob32(st, 7, proofs[i].component_digest)) {
            ok = false;
            break;
        }
        snprintf(proofs[i].component, sizeof(proofs[i].component), "%s",
                 names[i]);
        proofs[i].cursor = (uint64_t)cursor;
        proofs[i].first_height = 0;
        proofs[i].last_height = manifest->height;
        proofs[i].row_count = expected_rows;
        proofs[i].hash_bound_count = (uint64_t)hashes;
        uint64_t minimum = i == CONSENSUS_STATE_BUNDLE_PROOF_COUNT - 1
                               ? (uint64_t)manifest->height
                               : expected_rows;
        if (proofs[i].cursor < minimum ||
            (i == 6 && proofs[i].cursor != expected_rows) ||
            (i == 7 && proofs[i].cursor > minimum + 1) ||
            !digest_nonzero(proofs[i].component_digest)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    sqlite3_finalize(st);
    uint8_t recomputed[32];
    if (ok) {
        consensus_state_bundle_proof_manifest_digest(
            proofs, CONSENSUS_STATE_BUNDLE_PROOF_COUNT, recomputed);
        ok = memcmp(recomputed, manifest->proof_manifest_digest, 32) == 0 &&
             memcmp(proofs[0].component_digest,
                    receipt->chain_corpus_digest, 32) == 0;
    }
    if (!ok)
        return validation_fail(
            result, CONSENSUS_INSTALL_REFUSED,
            "bundle proof order/range/cursor/hash/digest mismatch");
    /* These summaries are producer evidence bound by source_receipt's known
     * executable. ZClassic headers do not commit these reducer state rows. */
    return true;
}

static bool validate_coins(sqlite3 *db,
                           const struct consensus_state_bundle_manifest *m,
                           struct consensus_state_install_result *r)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT txid,vout,value,script,height,is_coinbase "
            "FROM coins ORDER BY txid,vout", -1, &st, NULL) != SQLITE_OK)
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "bundle coins table missing/malformed");
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint64_t count = 0;
    int64_t supply = 0;
    bool ok = true;
    uint8_t prior_txid[32] = {0};
    uint32_t prior_vout = 0;
    bool have_prior = false;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { // raw-sql-ok:read-only-introspection
        int txid_type = sqlite3_column_type(st, 0);
        int script_type = sqlite3_column_type(st, 3);
        const uint8_t *txid = txid_type == SQLITE_BLOB
            ? sqlite3_column_blob(st, 0) : NULL;
        const uint8_t *script = script_type == SQLITE_BLOB
            ? sqlite3_column_blob(st, 3) : NULL;
        int64_t vout = -1, value = -1, height = -1, coinbase = -1;
        bool numeric = copy_int64_exact(st, 1, &vout) &&
                       copy_int64_exact(st, 2, &value) &&
                       copy_int64_exact(st, 4, &height) &&
                       copy_int64_exact(st, 5, &coinbase);
        int script_len = sqlite3_column_bytes(st, 3);
        int order = (have_prior && txid && sqlite3_column_bytes(st, 0) == 32)
                        ? memcmp(prior_txid, txid, 32)
                        : -1;
        if (txid_type != SQLITE_BLOB || script_type != SQLITE_BLOB ||
            !numeric || !txid ||
            sqlite3_column_bytes(st, 0) != 32 || script_len < 0 || vout < 0 ||
            script_len > MAX_SCRIPT_SIZE || vout > UINT32_MAX ||
            !MoneyRange(value) ||
            supply > MAX_MONEY - value || height < 0 || height > m->height ||
            (coinbase != 0 && coinbase != 1) || count == UINT64_MAX ||
            (have_prior && (order > 0 ||
                            (order == 0 && prior_vout >= (uint32_t)vout)))) {
            ok = false;
            break;
        }
        utxo_commitment_sha3_write_record(
            &ctx, txid, (uint32_t)vout, value, script_len ? script : NULL,
            (uint32_t)script_len, (uint32_t)height, (uint8_t)coinbase);
        supply += value;
        memcpy(prior_txid, txid, sizeof(prior_txid));
        prior_vout = (uint32_t)vout;
        have_prior = true;
        count++;
    }
    if (rc != SQLITE_DONE)
        ok = false;
    sqlite3_finalize(st);
    uint8_t root[32];
    sha3_256_finalize(&ctx, root);
    if (!ok || count != m->utxo_count || supply != m->total_supply ||
        memcmp(root, m->utxo_root, 32) != 0)
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "UTXO root/count/supply mismatch");
    return true;
}

static bool validate_anchors(sqlite3 *db,
                             const struct consensus_state_bundle_manifest *m,
                             struct consensus_state_install_result *r)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT pool,anchor,height,tree "
            "FROM anchors ORDER BY pool,anchor", -1, &st, NULL) != SQLITE_OK)
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "bundle anchors table missing/malformed");
    struct sha3_256_ctx ctx;
    consensus_state_bundle_anchor_digest_begin(&ctx);
    bool have_pool[2] = {false, false};
    int64_t frontier_height[2] = {-1, -1};
    uint8_t frontier_root[2][32] = {{0}, {0}};
    uint8_t prior_root[32] = {0};
    int prior_pool = -1;
    uint64_t count = 0;
    bool ok = true;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { // raw-sql-ok:read-only-introspection
        int root_type = sqlite3_column_type(st, 1);
        int tree_type = sqlite3_column_type(st, 3);
        const uint8_t *root = root_type == SQLITE_BLOB
            ? sqlite3_column_blob(st, 1) : NULL;
        int64_t pool = -1, height = -1;
        bool numeric = copy_int64_exact(st, 0, &pool) &&
                       copy_int64_exact(st, 2, &height);
        const uint8_t *tree_blob = tree_type == SQLITE_BLOB
            ? sqlite3_column_blob(st, 3) : NULL;
        int tree_len = tree_blob ? sqlite3_column_bytes(st, 3) : 0;
        if (!numeric || root_type != SQLITE_BLOB || tree_type != SQLITE_BLOB ||
            (pool != 0 && pool != 1) || !root ||
            sqlite3_column_bytes(st, 1) != 32 || !tree_blob || tree_len <= 0 ||
            height < 0 || height > m->height || count == UINT64_MAX ||
            (prior_pool == pool && memcmp(prior_root, root, 32) >= 0)) {
            ok = false;
            break;
        }
        struct incremental_merkle_tree tree;
        if (pool == ANCHOR_POOL_SPROUT)
            sprout_tree_init(&tree);
        else
            sapling_tree_init(&tree);
        struct byte_stream stream;
        stream_init_from_data(&stream, tree_blob, (size_t)tree_len);
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
            &ctx, (uint8_t)pool, root, (uint64_t)height, tree_blob,
            (uint32_t)tree_len);
        int pool_index = (int)pool;
        have_pool[pool_index] = true;
        if (height == frontier_height[pool_index]) {
            ok = false;
            break;
        }
        if (height > frontier_height[pool_index]) {
            frontier_height[pool_index] = height;
            memcpy(frontier_root[pool_index], root, 32);
        }
        prior_pool = pool;
        memcpy(prior_root, root, sizeof(prior_root));
        count++;
    }
    if (rc != SQLITE_DONE)
        ok = false;
    sqlite3_finalize(st);
    uint8_t digest[32];
    sha3_256_finalize(&ctx, digest);
    if (!ok || count != m->anchor_count || !have_pool[0] || !have_pool[1] ||
        frontier_height[ANCHOR_POOL_SPROUT] != m->sprout_frontier_height ||
        frontier_height[ANCHOR_POOL_SAPLING] != m->sapling_frontier_height ||
        memcmp(frontier_root[ANCHOR_POOL_SPROUT],
               m->sprout_frontier_root, 32) != 0 ||
        memcmp(frontier_root[ANCHOR_POOL_SAPLING],
               m->sapling_frontier_root, 32) != 0 ||
        memcmp(digest, m->anchor_digest, 32) != 0)
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "anchor structure/root/digest mismatch");
    return true;
}

static bool validate_nullifiers(
    sqlite3 *db, const struct consensus_state_bundle_manifest *m,
    struct consensus_state_install_result *r)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT pool,nf,height FROM nullifiers ORDER BY pool,nf",
            -1, &st, NULL) != SQLITE_OK)
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "bundle nullifiers table missing/malformed");
    struct sha3_256_ctx ctx;
    consensus_state_bundle_nullifier_digest_begin(&ctx);
    uint64_t count = 0;
    bool ok = true;
    uint8_t prior_nf[32] = {0};
    int prior_pool = -1;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { // raw-sql-ok:read-only-introspection
        int nf_type = sqlite3_column_type(st, 1);
        const uint8_t *nf = nf_type == SQLITE_BLOB
            ? sqlite3_column_blob(st, 1) : NULL;
        int64_t pool = -1, height = -1;
        bool numeric = copy_int64_exact(st, 0, &pool) &&
                       copy_int64_exact(st, 2, &height);
        if (!numeric || nf_type != SQLITE_BLOB ||
            (pool != 0 && pool != 1) || !nf ||
            sqlite3_column_bytes(st, 1) != 32 || height < 0 ||
            height > m->height || count == UINT64_MAX ||
            (prior_pool == pool && memcmp(prior_nf, nf, 32) >= 0)) {
            ok = false;
            break;
        }
        consensus_state_bundle_nullifier_digest_row(
            &ctx, (uint8_t)pool, nf, (uint64_t)height);
        prior_pool = pool;
        memcpy(prior_nf, nf, sizeof(prior_nf));
        count++;
    }
    if (rc != SQLITE_DONE)
        ok = false;
    sqlite3_finalize(st);
    uint8_t digest[32];
    sha3_256_finalize(&ctx, digest);
    if (!ok || count != m->nullifier_count ||
        memcmp(digest, m->nullifier_digest, 32) != 0)
        return validation_fail(r, CONSENSUS_INSTALL_REFUSED,
                               "nullifier structure/digest mismatch");
    return true;
}

bool consensus_state_bundle_validate(
    sqlite3 *db, struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_install_result *result)
{
    if (!db || !manifest)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "NULL db/manifest");
    if (!integrity_check(db) || !canonical_schema(db, result) ||
        !read_manifest(db, manifest, result))
        return false; // raw-return-ok:logged-by-callee
    uint8_t computed[32];
    consensus_state_bundle_artifact_digest(manifest, computed);
    if (memcmp(computed, manifest->artifact_digest, 32) != 0)
        return validation_fail(result, CONSENSUS_INSTALL_REFUSED,
                               "artifact digest mismatch");
    struct consensus_state_source_receipt receipt;
    return validate_source_receipt(db, manifest, &receipt, result) &&
           validate_bundle_proof(db, manifest, &receipt, result) &&
           validate_coins(db, manifest, result) &&
           validate_anchors(db, manifest, result) &&
           validate_nullifiers(db, manifest, result);
}
