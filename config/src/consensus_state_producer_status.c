/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Read-only, node-free status projection for a consensus-state producer. */

#include "config/consensus_state_producer_receipt.h"

#include "storage/consensus_state_bundle_codec.h"

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool status_set_err(char *err, size_t err_size, const char *msg)
{
    if (err && err_size)
        snprintf(err, err_size, "%s", msg);
    return false; /* raw-return-ok:bounded status reason returned to caller */
}

static bool status_set_sqlite_err(char *err, size_t err_size,
                                  sqlite3 *db, const char *operation)
{
    char why[256];
    snprintf(why, sizeof(why), "producer status: %s: %s", operation,
             db ? sqlite3_errmsg(db) : "sqlite handle unavailable");
    return status_set_err(err, err_size, why);
}

enum pkv_read_result {
    PKV_READ_ERROR = -1,
    PKV_READ_ABSENT = 0,
    PKV_READ_PRESENT = 1,
};

/* An old/empty progress.kv may legitimately predate a projection table. Once
 * a table exists, however, every read is strict: SQL errors and malformed
 * values are corruption/unreadability, never silently projected as absence. */
static bool pkv_table_exists(sqlite3 *db, const char *table, bool *exists,
                             char *err, size_t err_size)
{
    static const char sql[] =
        "SELECT EXISTS(SELECT 1 FROM sqlite_schema "
        "WHERE type='table' AND name=?1)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return status_set_sqlite_err(err, err_size, db,
                                     "inspect schema prepare");
    if (sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(st);
        return status_set_sqlite_err(err, err_size, db,
                                     "inspect schema bind");
    }
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    bool ok = rc == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER;
    int64_t value = ok ? sqlite3_column_int64(st, 0) : -1;
    if (ok)
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:read-only-introspection
    int finalize_rc = sqlite3_finalize(st);
    if (!ok || finalize_rc != SQLITE_OK || (value != 0 && value != 1))
        return status_set_sqlite_err(err, err_size, db,
                                     "inspect schema result");
    *exists = value == 1;
    return true;
}

static enum pkv_read_result pkv_query_i64(sqlite3 *db, const char *table,
                                          const char *sql, int64_t *out,
                                          char *err, size_t err_size)
{
    bool table_present = false;
    if (!pkv_table_exists(db, table, &table_present, err, err_size))
        return PKV_READ_ERROR;
    if (!table_present)
        return PKV_READ_ABSENT;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        status_set_sqlite_err(err, err_size, db, "integer query prepare");
        return PKV_READ_ERROR;
    }
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    if (rc == SQLITE_DONE) {
        int finalize_rc = sqlite3_finalize(st);
        if (finalize_rc != SQLITE_OK) {
            status_set_sqlite_err(err, err_size, db,
                                  "integer query finalize");
            return PKV_READ_ERROR;
        }
        return PKV_READ_ABSENT;
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(st, 0) != SQLITE_INTEGER) {
        sqlite3_finalize(st);
        status_set_err(err, err_size,
                       "producer status: integer projection is malformed");
        return PKV_READ_ERROR;
    }
    int64_t value = sqlite3_column_int64(st, 0);
    rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    int finalize_rc = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || finalize_rc != SQLITE_OK) {
        status_set_sqlite_err(err, err_size, db, "integer query result");
        return PKV_READ_ERROR;
    }
    *out = value;
    return PKV_READ_PRESENT;
}

static void status_hex32(const uint8_t bytes[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[64] = '\0';
}

static bool status_bytes_nonzero(const uint8_t *bytes, size_t len)
{
    uint8_t any = 0;
    for (size_t i = 0; i < len; i++)
        any |= bytes[i];
    return any != 0;
}

static bool status_schema_version(const char *schema, bool receipt,
                                  uint8_t *out_version)
{
    if (!schema || !out_version)
        return false;
    if (receipt)
        return consensus_state_source_receipt_schema_version(
            schema, strlen(schema), out_version);
    if (strcmp(schema, "zcl.consensus_state_producer_session.v1") == 0) {
        *out_version = CONSENSUS_STATE_SOURCE_RECEIPT_V1;
        return true;
    }
    if (strcmp(schema, "zcl.consensus_state_producer_session.v2") == 0) {
        *out_version = CONSENSUS_STATE_SOURCE_RECEIPT_V2;
        return true;
    }
    return false;
}

static bool status_copy_blob32(sqlite3_stmt *st, int column, uint8_t out[32])
{
    if (sqlite3_column_type(st, column) != SQLITE_BLOB ||
        sqlite3_column_bytes(st, column) != 32)
        return false;
    const void *blob = sqlite3_column_blob(st, column);
    if (!blob)
        return false;
    memcpy(out, blob, 32);
    return true;
}

/* A receipt row is not a finalized receipt merely because `fold_cursor`
 * exists. Parse every authority-bearing field, require the cryptographic
 * claims to be nonzero, and reproduce both the source epoch and complete
 * receipt digest before exposing `receipt_finalized=true`. */
static enum pkv_read_result pkv_read_final_receipt(
    sqlite3 *db, struct producer_status_read *out,
    char *err, size_t err_size)
{
    static const char table[] = "consensus_state_source_receipt";
    static const char sql[] =
        "SELECT schema,source_epoch_digest,source_tree_root,"
        "running_binary_digest,toolchain_digest,build_inputs_digest,"
        "chain_corpus_digest,source_clean,validation_profile,producer_commit,"
        "fold_cursor,receipt_digest FROM consensus_state_source_receipt "
        "WHERE singleton=1";
    bool table_present = false;
    if (!pkv_table_exists(db, table, &table_present, err, err_size))
        return PKV_READ_ERROR;
    if (!table_present)
        return PKV_READ_ABSENT;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        status_set_sqlite_err(err, err_size, db,
                              "finalized receipt prepare");
        return PKV_READ_ERROR;
    }
    int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    if (rc == SQLITE_DONE) {
        int finalize_rc = sqlite3_finalize(st);
        if (finalize_rc != SQLITE_OK) {
            status_set_sqlite_err(err, err_size, db,
                                  "finalized receipt finalize");
            return PKV_READ_ERROR;
        }
        return PKV_READ_ABSENT;
    }

    struct consensus_state_source_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    const unsigned char *schema = rc == SQLITE_ROW &&
        sqlite3_column_type(st, 0) == SQLITE_TEXT
        ? sqlite3_column_text(st, 0) : NULL;
    int schema_len = schema ? sqlite3_column_bytes(st, 0) : -1;
    bool schema_ok = schema && schema_len > 0 &&
        (size_t)schema_len < sizeof(out->receipt_schema) &&
        memchr(schema, '\0', (size_t)schema_len) == NULL &&
        consensus_state_source_receipt_schema_version(
            (const char *)schema, (size_t)schema_len,
            &receipt.schema_version);
    char schema_copy[sizeof(out->receipt_schema)] = {0};
    if (schema_ok) {
        memcpy(schema_copy, schema, (size_t)schema_len);
        schema_copy[schema_len] = '\0';
    }
    const unsigned char *commit = rc == SQLITE_ROW &&
        sqlite3_column_type(st, 9) == SQLITE_TEXT
        ? sqlite3_column_text(st, 9) : NULL;
    int commit_len = commit ? sqlite3_column_bytes(st, 9) : -1;
    bool commit_ok = commit && commit_len >= 0 &&
        (size_t)commit_len < sizeof(receipt.producer_commit) &&
        memchr(commit, '\0', (size_t)commit_len) == NULL &&
        consensus_state_source_receipt_commit_valid(
            receipt.schema_version, (const char *)commit,
            (size_t)commit_len);
    bool clean_ok = rc == SQLITE_ROW &&
        sqlite3_column_type(st, 7) == SQLITE_INTEGER &&
        (sqlite3_column_int(st, 7) == 0 ||
         sqlite3_column_int(st, 7) == 1);
    bool profile_ok = rc == SQLITE_ROW &&
        sqlite3_column_type(st, 8) == SQLITE_INTEGER &&
        (sqlite3_column_int(st, 8) == CONSENSUS_STATE_VALIDATION_FULL ||
         sqlite3_column_int(st, 8) ==
             CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD);
    bool cursor_ok = rc == SQLITE_ROW &&
        sqlite3_column_type(st, 10) == SQLITE_INTEGER &&
        sqlite3_column_int64(st, 10) > 0 &&
        sqlite3_column_int64(st, 10) <= INT32_MAX;
    bool fields_ok = schema_ok && commit_ok && clean_ok && profile_ok &&
        cursor_ok && status_copy_blob32(st, 1,
                                        receipt.source_epoch_digest) &&
        status_copy_blob32(st, 2, receipt.source_tree_root) &&
        status_copy_blob32(st, 3, receipt.running_binary_digest) &&
        status_copy_blob32(st, 4, receipt.toolchain_digest) &&
        status_copy_blob32(st, 5, receipt.build_inputs_digest) &&
        status_copy_blob32(st, 6, receipt.chain_corpus_digest) &&
        status_copy_blob32(st, 11, receipt.receipt_digest);
    if (fields_ok) {
        receipt.source_clean = sqlite3_column_int(st, 7) == 1;
        receipt.validation_profile = (uint8_t)sqlite3_column_int(st, 8);
        memcpy(receipt.producer_commit, commit, (size_t)commit_len);
        receipt.producer_commit[commit_len] = '\0';
        receipt.fold_cursor = sqlite3_column_int64(st, 10);
        fields_ok = status_bytes_nonzero(receipt.source_epoch_digest, 32) &&
            status_bytes_nonzero(receipt.source_tree_root, 32) &&
            status_bytes_nonzero(receipt.running_binary_digest, 32) &&
            status_bytes_nonzero(receipt.toolchain_digest, 32) &&
            status_bytes_nonzero(receipt.build_inputs_digest, 32) &&
            status_bytes_nonzero(receipt.chain_corpus_digest, 32) &&
            status_bytes_nonzero(receipt.receipt_digest, 32) &&
            (out->validation_profile < 0 ||
             out->validation_profile == receipt.validation_profile);
    }
    uint8_t recomputed_epoch[32] = {0};
    uint8_t recomputed_receipt[32] = {0};
    if (fields_ok) {
        consensus_state_source_epoch_digest(&receipt, recomputed_epoch);
        consensus_state_source_receipt_digest(&receipt, recomputed_receipt);
        fields_ok = memcmp(recomputed_epoch,
                           receipt.source_epoch_digest, 32) == 0 &&
                    memcmp(recomputed_receipt,
                           receipt.receipt_digest, 32) == 0;
    }
    rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
    int finalize_rc = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || finalize_rc != SQLITE_OK) {
        status_set_sqlite_err(err, err_size, db,
                              "finalized receipt result");
        return PKV_READ_ERROR;
    }
    if (!fields_ok) {
        status_set_err(err, err_size,
                       "producer status: finalized receipt is malformed or "
                       "fails digest verification");
        return PKV_READ_ERROR;
    }

    memcpy(out->receipt_schema, schema_copy, (size_t)schema_len + 1u);
    status_hex32(receipt.source_tree_root, out->source_tree_root);
    status_hex32(receipt.source_epoch_digest, out->source_epoch_digest);
    memcpy(out->producer_commit, receipt.producer_commit,
           (size_t)commit_len + 1u);
    out->validation_profile = receipt.validation_profile;
    out->fold_cursor = receipt.fold_cursor;
    out->receipt_finalized = true;
    return PKV_READ_PRESENT;
}

/* When no finalized receipt exists, expose the open session's bounded source
 * claim. This is progress telemetry only and never sets receipt_finalized. */
static bool pkv_read_session_identity(sqlite3 *db,
                                      struct producer_status_read *out,
                                      char *err, size_t err_size)
{
    static const struct {
        const char *table;
        const char *sql;
        bool receipt;
    } projections[] = {
        {
            "consensus_state_producer_session",
            "SELECT schema,source_tree_root,source_epoch_digest,producer_commit,"
            "toolchain_digest,build_inputs_digest,source_clean,validation_profile "
            "FROM consensus_state_producer_session WHERE singleton=1",
            false,
        },
    };
    for (size_t i = 0; i < sizeof(projections) / sizeof(projections[0]); i++) {
        bool table_present = false;
        if (!pkv_table_exists(db, projections[i].table, &table_present,
                              err, err_size))
            return false;
        if (!table_present)
            continue;
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, projections[i].sql, -1, &st, NULL) !=
            SQLITE_OK)
            return status_set_sqlite_err(err, err_size, db,
                                         "source identity prepare");
        int rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
        if (rc == SQLITE_DONE) {
            int finalize_rc = sqlite3_finalize(st);
            if (finalize_rc != SQLITE_OK)
                return status_set_sqlite_err(err, err_size, db,
                                             "source identity finalize");
            continue;
        }
        const unsigned char *schema = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 0) == SQLITE_TEXT
            ? sqlite3_column_text(st, 0) : NULL;
        int schema_len = schema ? sqlite3_column_bytes(st, 0) : -1;
        const uint8_t *root = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 1) == SQLITE_BLOB &&
            sqlite3_column_bytes(st, 1) == 32
            ? sqlite3_column_blob(st, 1) : NULL;
        const uint8_t *epoch = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 2) == SQLITE_BLOB &&
            sqlite3_column_bytes(st, 2) == 32
            ? sqlite3_column_blob(st, 2) : NULL;
        const unsigned char *commit = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 3) == SQLITE_TEXT
            ? sqlite3_column_text(st, 3) : NULL;
        int commit_len = commit ? sqlite3_column_bytes(st, 3) : -1;
        const uint8_t *toolchain = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 4) == SQLITE_BLOB &&
            sqlite3_column_bytes(st, 4) == 32
            ? sqlite3_column_blob(st, 4) : NULL;
        const uint8_t *build_inputs = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 5) == SQLITE_BLOB &&
            sqlite3_column_bytes(st, 5) == 32
            ? sqlite3_column_blob(st, 5) : NULL;
        bool clean_ok = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 6) == SQLITE_INTEGER &&
            (sqlite3_column_int(st, 6) == 0 ||
             sqlite3_column_int(st, 6) == 1);
        bool profile_ok = rc == SQLITE_ROW &&
            sqlite3_column_type(st, 7) == SQLITE_INTEGER &&
            (sqlite3_column_int(st, 7) == CONSENSUS_STATE_VALIDATION_FULL ||
             sqlite3_column_int(st, 7) ==
                 CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD);
        bool schema_ok = schema && schema_len > 0 &&
            (size_t)schema_len < sizeof(out->receipt_schema) &&
            memchr(schema, '\0', (size_t)schema_len) == NULL;
        uint8_t version = CONSENSUS_STATE_SOURCE_RECEIPT_INVALID;
        char schema_copy[sizeof(out->receipt_schema)];
        if (schema_ok) {
            memcpy(schema_copy, schema, (size_t)schema_len);
            schema_copy[schema_len] = '\0';
            schema_ok = status_schema_version(schema_copy,
                                              projections[i].receipt,
                                              &version);
        }
        bool commit_ok = commit && commit_len >= 0 &&
            (size_t)commit_len < sizeof(out->producer_commit) &&
            memchr(commit, '\0', (size_t)commit_len) == NULL &&
            consensus_state_source_receipt_commit_valid(
                version, (const char *)commit, (size_t)commit_len);
        struct consensus_state_source_receipt claim;
        memset(&claim, 0, sizeof(claim));
        bool claim_ok = schema_ok && root && epoch && toolchain &&
                        build_inputs && clean_ok && profile_ok &&
                        commit_ok && status_bytes_nonzero(root, 32) &&
                        status_bytes_nonzero(epoch, 32) &&
                        status_bytes_nonzero(toolchain, 32) &&
                        status_bytes_nonzero(build_inputs, 32);
        if (claim_ok) {
            claim.schema_version = version;
            memcpy(claim.source_tree_root, root, 32);
            memcpy(claim.source_epoch_digest, epoch, 32);
            memcpy(claim.toolchain_digest, toolchain, 32);
            memcpy(claim.build_inputs_digest, build_inputs, 32);
            memcpy(claim.producer_commit, commit, (size_t)commit_len);
            claim.producer_commit[commit_len] = '\0';
            claim.source_clean = sqlite3_column_int(st, 6) == 1;
            claim.validation_profile =
                (uint8_t)sqlite3_column_int(st, 7);
            uint8_t recomputed_epoch[32];
            consensus_state_source_epoch_digest(&claim, recomputed_epoch);
            claim_ok = memcmp(recomputed_epoch, epoch, 32) == 0 &&
                       (out->validation_profile < 0 ||
                        out->validation_profile == claim.validation_profile);
        }
        if (claim_ok) {
            memcpy(out->receipt_schema, schema, (size_t)schema_len);
            out->receipt_schema[schema_len] = '\0';
            status_hex32(root, out->source_tree_root);
            status_hex32(epoch, out->source_epoch_digest);
            memcpy(out->producer_commit, commit, (size_t)commit_len);
            out->producer_commit[commit_len] = '\0';
            out->validation_profile = claim.validation_profile;
            rc = sqlite3_step(st); // raw-sql-ok:read-only-introspection
            int finalize_rc = sqlite3_finalize(st);
            if (rc != SQLITE_DONE || finalize_rc != SQLITE_OK)
                return status_set_sqlite_err(err, err_size, db,
                                             "source identity result");
            return true;
        }
        sqlite3_finalize(st);
        return status_set_err(err, err_size,
                              "producer status: source identity is malformed");
    }
    return true;
}

bool consensus_state_producer_status_read(const char *datadir,
                                          struct producer_status_read *out,
                                          char *err, size_t err_size)
{
    if (!out)
        return status_set_err(err, err_size, "producer status: null out");
    memset(out, 0, sizeof(*out));
    out->utxo_apply_cursor = -1;
    out->tip_finalize_cursor = -1;
    out->fold_cursor = -1;
    out->validation_profile = -1;
    if (!datadir || !datadir[0])
        return status_set_err(err, err_size, "producer status: empty datadir");
    if (strlen(datadir) >= CONSENSUS_STATE_PRODUCER_DATADIR_MAX)
        return status_set_err(err, err_size,
                              "producer status: datadir exceeds 1023 bytes");

    char path[CONSENSUS_STATE_PRODUCER_DATADIR_MAX +
              sizeof("/progress.kv")];
    int path_len = snprintf(path, sizeof(path), "%s/progress.kv", datadir);
    if (path_len < 0 || (size_t)path_len >= sizeof(path))
        return status_set_err(err, err_size,
                              "producer status: progress path overflow");
    struct stat stbuf;
    if (stat(path, &stbuf) != 0) {
        if (errno != ENOENT) {
            char why[256];
            snprintf(why, sizeof(why), "producer status: stat %s: %s", path,
                     strerror(errno));
            return status_set_err(err, err_size, why);
        }
        out->progress_kv_present = false;
        return true;
    }
    if (!S_ISREG(stbuf.st_mode))
        return status_set_err(err, err_size,
                              "producer status: progress.kv is not regular");

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        const char *message = db ? sqlite3_errmsg(db) : "open failed";
        char why[256];
        snprintf(why, sizeof(why), "producer status: open %s: %s", path,
                 message);
        if (db)
            sqlite3_close(db);
        return status_set_err(err, err_size, why);
    }
    out->progress_kv_present = true;

    int64_t value = 0;
    enum pkv_read_result read_result = pkv_query_i64(
            db, "stage_cursor",
            "SELECT cursor FROM stage_cursor WHERE name='utxo_apply'",
            &value, err, err_size);
    if (read_result == PKV_READ_ERROR)
        goto fail;
    if (read_result == PKV_READ_PRESENT)
        out->utxo_apply_cursor = value;
    read_result = pkv_query_i64(
            db, "stage_cursor",
            "SELECT cursor FROM stage_cursor WHERE name='tip_finalize'",
            &value, err, err_size);
    if (read_result == PKV_READ_ERROR)
        goto fail;
    if (read_result == PKV_READ_PRESENT)
        out->tip_finalize_cursor = value;
    read_result = pkv_query_i64(
            db, "consensus_state_producer_session",
            "SELECT start_time_us FROM consensus_state_producer_session "
                "WHERE singleton=1", &value, err, err_size);
    if (read_result == PKV_READ_ERROR)
        goto fail;
    if (read_result == PKV_READ_PRESENT) {
        out->session_open = true;
        out->start_time_us = value;
    }
    read_result = pkv_query_i64(
            db, "consensus_state_producer_session",
            "SELECT validation_profile "
                "FROM consensus_state_producer_session WHERE singleton=1",
            &value, err, err_size);
    if (read_result == PKV_READ_ERROR)
        goto fail;
    if (read_result == PKV_READ_PRESENT) {
        if (value != CONSENSUS_STATE_VALIDATION_FULL &&
            value != CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD) {
            status_set_err(err, err_size,
                           "producer status: validation profile is malformed");
            goto fail;
        }
        out->validation_profile = (int)value;
    }
    read_result = pkv_read_final_receipt(db, out, err, err_size);
    if (read_result == PKV_READ_ERROR)
        goto fail;
    if (read_result == PKV_READ_ABSENT &&
        !pkv_read_session_identity(db, out, err, err_size))
        goto fail;
    if (sqlite3_close(db) != SQLITE_OK)
        return status_set_err(err, err_size,
                              "producer status: close progress.kv failed");
    return true;

fail:
    sqlite3_close(db);
    return false;
}
