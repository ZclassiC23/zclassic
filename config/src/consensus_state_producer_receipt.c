/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Producer-owned durable source receipt for the full-history exporter. */

#include "config/consensus_state_producer_receipt.h"

#include "storage/consensus_state_bundle_codec.h"
#include "storage/progress_store.h"
#include "crypto/sha3.h"
#include "core/utiltime.h"
#include "util/clientversion.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PRODUCER_RECEIPT_SUBSYS "consensus_producer_receipt"

/* Singleton start-of-fold ownership marker in the producer's progress.kv.
 * Distinct from the exporter-read consensus_state_source_receipt: this records
 * WHO started the fold; the receipt is written only when the fold completes. */
#define PRODUCER_SESSION_SCHEMA "zcl.consensus_state_producer_session.v1"

static const char k_session_schema_sql[] =
    "CREATE TABLE IF NOT EXISTS consensus_state_producer_session("
    "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
    "schema TEXT NOT NULL,running_binary_digest BLOB NOT NULL,"
    "source_tree_root BLOB NOT NULL,toolchain_digest BLOB NOT NULL,"
    "build_inputs_digest BLOB NOT NULL,source_epoch_digest BLOB NOT NULL,"
    "source_clean INTEGER NOT NULL,validation_profile INTEGER NOT NULL,"
    "producer_commit TEXT NOT NULL,datadir TEXT NOT NULL,"
    "start_time_us INTEGER NOT NULL)";

static const char k_receipt_schema_sql[] =
    "CREATE TABLE IF NOT EXISTS consensus_state_source_receipt("
    "singleton INTEGER PRIMARY KEY CHECK(singleton=1),schema TEXT NOT NULL,"
    "source_epoch_digest BLOB NOT NULL,source_tree_root BLOB NOT NULL,"
    "running_binary_digest BLOB NOT NULL,toolchain_digest BLOB NOT NULL,"
    "build_inputs_digest BLOB NOT NULL,chain_corpus_digest BLOB NOT NULL,"
    "source_clean INTEGER NOT NULL,validation_profile INTEGER NOT NULL,"
    "producer_commit TEXT NOT NULL,fold_cursor INTEGER NOT NULL,"
    "receipt_digest BLOB NOT NULL)";

#ifdef ZCL_TESTING
static bool g_override_active;
static char g_override_commit[41];
static bool g_override_clean;

void consensus_state_producer_receipt_test_set_identity(const char *commit,
                                                        bool source_clean)
{
    if (!commit) {
        g_override_active = false;
        g_override_commit[0] = '\0';
        return;
    }
    g_override_active = true;
    snprintf(g_override_commit, sizeof(g_override_commit), "%s", commit);
    g_override_clean = source_clean;
}
#endif

static bool set_err(char *err, size_t err_size, const char *msg)
{
    if (err && err_size)
        snprintf(err, err_size, "%s", msg);
    return false; /* raw-return-ok:bounded policy reason returned to caller */
}

static bool valid_full_commit(const char *commit)
{
    if (strnlen(commit, 41) != 40)
        return false;
    for (size_t i = 0; i < 40; i++) {
        char c = commit[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

/* SHA3-256 of the running executable's on-disk image — the binding the
 * exporter recomputes from /proc/self/exe. Must match running_binary_digest()
 * in consensus_state_snapshot_export_proof.c byte for byte. */
static bool running_binary_digest(uint8_t out[32])
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_WARN(PRODUCER_RECEIPT_SUBSYS, "running executable open failed: %s",
                 strerror(errno));
        return false;
    }
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
    else
        LOG_WARN(PRODUCER_RECEIPT_SUBSYS, "running executable digest failed");
    return ok;
}

static void claim_digest(const char *domain, const uint8_t *extra,
                         size_t extra_len, uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, (const uint8_t *)domain, strlen(domain) + 1);
    sha3_256_write(&ctx, extra, extra_len);
    sha3_256_finalize(&ctx, out);
}

/* Fill the honest source-identity CLAIM fields (producer_commit, source_clean,
 * source_tree_root, toolchain_digest, build_inputs_digest). These are producer
 * assertions whose authority is the running executable that recorded them; they
 * are derived deterministically from the binary's baked build identity (or, in
 * tests, a hermetic override). Returns false if no exact 40-hex commit is
 * available — an unstamped build honestly cannot earn a serving receipt. */
static bool fill_source_identity_claim(struct consensus_state_source_receipt *r)
{
    const char *commit = zcl_build_commit_full();
    bool clean = zcl_build_source_clean();
#ifdef ZCL_TESTING
    if (g_override_active) {
        commit = g_override_commit;
        clean = g_override_clean;
    }
#endif
    if (!commit || !valid_full_commit(commit))
        return false; /* raw-return-ok:unstamped build cannot earn a receipt */

    memcpy(r->producer_commit, commit, 40);
    r->producer_commit[40] = '\0';
    r->source_clean = clean;

    uint8_t clean_byte = clean ? 1u : 0u;
    uint8_t tree_preimage[41];
    memcpy(tree_preimage, r->producer_commit, 40);
    tree_preimage[40] = clean_byte;
    claim_digest("zcl.producer_source_tree.v1", tree_preimage,
                 sizeof(tree_preimage), r->source_tree_root);

    const char *toolchain =
#ifdef __VERSION__
        __VERSION__;
#else
        "unknown-toolchain";
#endif
    claim_digest("zcl.producer_toolchain.v1", (const uint8_t *)toolchain,
                 strlen(toolchain), r->toolchain_digest);

    uint8_t build_inputs_preimage[44];
    uint32_t version = (uint32_t)CLIENT_VERSION;
    for (size_t i = 0; i < 4; i++)
        build_inputs_preimage[i] = (uint8_t)(version >> (8u * i));
    memcpy(build_inputs_preimage + 4, r->producer_commit, 40);
    claim_digest("zcl.producer_build_inputs.v1", build_inputs_preimage,
                 sizeof(build_inputs_preimage), r->build_inputs_digest);
    return true;
}

static void proof_u64(struct sha3_256_ctx *ctx, uint64_t value)
{
    uint8_t le[8];
    for (size_t i = 0; i < sizeof(le); i++)
        le[i] = (uint8_t)(value >> (8u * i));
    sha3_256_write(ctx, le, sizeof(le));
}

/* Recompute the genesis..height header corpus digest and confirm the tip hash.
 * MUST stay byte-identical to prove_header_chain() in
 * consensus_state_snapshot_export_proof.c — the exporter compares the receipt's
 * chain_corpus_digest against its own recomputation. The producer-receipt test
 * runs the real exporter, so any drift here fails the test. */
static bool header_corpus_digest(sqlite3 *db, int32_t height,
                                 const uint8_t expected_hash[32],
                                 uint8_t out[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height,hash,parent_hash FROM header_admit_log "
            "WHERE height BETWEEN 0 AND ? ORDER BY height", -1, &st,
            NULL) != SQLITE_OK) {
        LOG_WARN(PRODUCER_RECEIPT_SUBSYS, "header corpus prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    static const char domain[] =
        "zcl.consensus_state_bundle.v1/source-header-chain";
    sha3_256_write(&ctx, (const uint8_t *)domain, sizeof(domain));

    uint8_t prior[32] = {0};
    int64_t expected_height = 0;
    bool ok = true;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { // raw-sql-ok:progress-kv-kernel-store
        int height_type = sqlite3_column_type(st, 0);
        int hash_type = sqlite3_column_type(st, 1);
        int parent_type = sqlite3_column_type(st, 2);
        const void *hash = hash_type == SQLITE_BLOB
            ? sqlite3_column_blob(st, 1) : NULL;
        const void *parent = parent_type == SQLITE_BLOB
            ? sqlite3_column_blob(st, 2) : NULL;
        int parent_len = parent ? sqlite3_column_bytes(st, 2) : 0;
        int64_t row_height = height_type == SQLITE_INTEGER
            ? sqlite3_column_int64(st, 0) : -1;
        bool genesis_parent = row_height == 0 && parent_type == SQLITE_NULL;
        bool linked_parent = row_height > 0 && parent_type == SQLITE_BLOB &&
                             parent && parent_len == 32 &&
                             memcmp(parent, prior, 32) == 0;
        if (height_type != SQLITE_INTEGER || hash_type != SQLITE_BLOB || !hash ||
            sqlite3_column_bytes(st, 1) != 32 ||
            row_height != expected_height ||
            (!genesis_parent && !linked_parent)) {
            ok = false;
            break;
        }
        proof_u64(&ctx, (uint64_t)row_height);
        sha3_256_write(&ctx, hash, 32);
        if (row_height > 0) {
            sha3_256_write(&ctx, parent, 32);
        } else {
            uint8_t no_parent[32] = {0};
            sha3_256_write(&ctx, no_parent, sizeof(no_parent));
        }
        memcpy(prior, hash, sizeof(prior));
        expected_height++;
    }
    if (rc != SQLITE_DONE || expected_height != (int64_t)height + 1 ||
        memcmp(prior, expected_hash, 32) != 0)
        ok = false;
    sqlite3_finalize(st);
    if (ok)
        sha3_256_finalize(&ctx, out);
    else
        LOG_WARN(PRODUCER_RECEIPT_SUBSYS,
                 "genesis..h=%d header corpus incomplete or wrong tip", height);
    return ok;
}

static bool read_session_blob(sqlite3_stmt *st, int col, uint8_t out[32])
{
    if (sqlite3_column_type(st, col) != SQLITE_BLOB ||
        sqlite3_column_bytes(st, col) != 32)
        return false;
    memcpy(out, sqlite3_column_blob(st, col), 32);
    return true;
}

struct producer_session {
    uint8_t running_binary_digest[32];
    uint8_t source_epoch_digest[32];
    struct consensus_state_source_receipt claim; /* claim fields only */
    bool present;
};

/* Load the singleton start session. Returns false only on a store error;
 * `out->present` reports whether a row exists. */
static bool load_session(sqlite3 *db, struct producer_session *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT running_binary_digest,source_tree_root,toolchain_digest,"
            "build_inputs_digest,source_epoch_digest,source_clean,"
            "validation_profile,producer_commit "
            "FROM consensus_state_producer_session WHERE singleton=1", -1,
            &st, NULL) != SQLITE_OK) {
        /* Table may not exist yet: that is a legitimate "no session". */
        return true;
    }
    int rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
    bool ok = true;
    if (rc == SQLITE_ROW) {
        const unsigned char *commit = sqlite3_column_type(st, 7) == SQLITE_TEXT
            ? sqlite3_column_text(st, 7) : NULL;
        ok = read_session_blob(st, 0, out->running_binary_digest) &&
             read_session_blob(st, 1, out->claim.source_tree_root) &&
             read_session_blob(st, 2, out->claim.toolchain_digest) &&
             read_session_blob(st, 3, out->claim.build_inputs_digest) &&
             read_session_blob(st, 4, out->source_epoch_digest) &&
             sqlite3_column_type(st, 5) == SQLITE_INTEGER &&
             sqlite3_column_type(st, 6) == SQLITE_INTEGER &&
             commit && sqlite3_column_bytes(st, 7) == 40;
        if (ok) {
            out->claim.source_clean = sqlite3_column_int(st, 5) == 1;
            out->claim.validation_profile =
                (uint8_t)sqlite3_column_int(st, 6);
            memcpy(out->claim.producer_commit, commit, 40);
            out->claim.producer_commit[40] = '\0';
            out->present = true;
        }
    } else if (rc != SQLITE_DONE) {
        ok = false;
    }
    sqlite3_finalize(st);
    return ok;
}

static bool insert_session(sqlite3 *db,
                           const struct consensus_state_source_receipt *r,
                           const uint8_t running_binary[32],
                           const char *datadir, int64_t start_us)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO consensus_state_producer_session("
            "singleton,schema,running_binary_digest,source_tree_root,"
            "toolchain_digest,build_inputs_digest,source_epoch_digest,"
            "source_clean,validation_profile,producer_commit,datadir,"
            "start_time_us) VALUES(1,?,?,?,?,?,?,?,?,?,?,?)", -1, &st,
            NULL) != SQLITE_OK)
        return false;
    int i = 1;
    bool ok = sqlite3_bind_text(st, i++, PRODUCER_SESSION_SCHEMA, -1,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, running_binary, 32, SQLITE_STATIC) ==
                  SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->source_tree_root, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->toolchain_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->build_inputs_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->source_epoch_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int(st, i++, r->source_clean ? 1 : 0) == SQLITE_OK &&
              sqlite3_bind_int(st, i++, r->validation_profile) == SQLITE_OK &&
              sqlite3_bind_text(st, i++, r->producer_commit, 40,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_text(st, i++, datadir ? datadir : "", -1,
                                SQLITE_TRANSIENT) == SQLITE_OK &&
              sqlite3_bind_int64(st, i++, start_us) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    return ok;
}

static bool write_final_receipt(
    sqlite3 *db, const struct consensus_state_source_receipt *r)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO consensus_state_source_receipt("
            "singleton,schema,source_epoch_digest,source_tree_root,"
            "running_binary_digest,toolchain_digest,build_inputs_digest,"
            "chain_corpus_digest,source_clean,validation_profile,"
            "producer_commit,fold_cursor,receipt_digest) "
            "VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
        return false;
    int i = 1;
    bool ok = sqlite3_bind_text(st, i++, CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA,
                                -1, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->source_epoch_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->source_tree_root, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->running_binary_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->toolchain_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->build_inputs_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->chain_corpus_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int(st, i++, r->source_clean ? 1 : 0) == SQLITE_OK &&
              sqlite3_bind_int(st, i++, r->validation_profile) == SQLITE_OK &&
              sqlite3_bind_text(st, i++, r->producer_commit, 40,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_int64(st, i++, r->fold_cursor) == SQLITE_OK &&
              sqlite3_bind_blob(st, i++, r->receipt_digest, 32,
                                SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    return ok;
}

enum final_receipt_state {
    FINAL_RECEIPT_READ_ERROR = 0, FINAL_RECEIPT_ABSENT,
    FINAL_RECEIPT_IDENTICAL, FINAL_RECEIPT_CONFLICT,
};
static bool receipt_blob_equal(sqlite3_stmt *st, int column,
                               const uint8_t expected[32])
{
    const void *blob = sqlite3_column_type(st, column) == SQLITE_BLOB
                           ? sqlite3_column_blob(st, column) : NULL;
    return blob && sqlite3_column_bytes(st, column) == 32 &&
           memcmp(blob, expected, 32) == 0;
}

static bool receipt_text_equal(sqlite3_stmt *st, int column,
                               const char *expected, size_t expected_len)
{
    const unsigned char *text =
        sqlite3_column_type(st, column) == SQLITE_TEXT
            ? sqlite3_column_text(st, column) : NULL;
    return text && sqlite3_column_bytes(st, column) == (int)expected_len &&
           memcmp(text, expected, expected_len) == 0;
}

/* A finalized receipt is an append-only ownership record. A retry may observe
 * the exact row it already committed, but must never heal, replace, or silently
 * adopt a malformed/different row. That distinction matters after a crash:
 * exact retry is safe; conflicting durable evidence is a named refusal. */
static enum final_receipt_state final_receipt_state(
    sqlite3 *db, const struct consensus_state_source_receipt *expected)
{
    static const char sql[] =
        "SELECT schema,source_epoch_digest,source_tree_root,"
        "running_binary_digest,toolchain_digest,build_inputs_digest,"
        "chain_corpus_digest,source_clean,validation_profile,producer_commit,"
        "fold_cursor,receipt_digest FROM consensus_state_source_receipt "
        "WHERE singleton=1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return FINAL_RECEIPT_READ_ERROR;
    int rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        return FINAL_RECEIPT_ABSENT;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return FINAL_RECEIPT_READ_ERROR;
    }
    bool identical =
        receipt_text_equal(st, 0, CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA,
                           strlen(CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA)) &&
        receipt_blob_equal(st, 1, expected->source_epoch_digest) &&
        receipt_blob_equal(st, 2, expected->source_tree_root) &&
        receipt_blob_equal(st, 3, expected->running_binary_digest) &&
        receipt_blob_equal(st, 4, expected->toolchain_digest) &&
        receipt_blob_equal(st, 5, expected->build_inputs_digest) &&
        receipt_blob_equal(st, 6, expected->chain_corpus_digest) &&
        sqlite3_column_type(st, 7) == SQLITE_INTEGER &&
        sqlite3_column_int(st, 7) == (expected->source_clean ? 1 : 0) &&
        sqlite3_column_type(st, 8) == SQLITE_INTEGER &&
        sqlite3_column_int(st, 8) == expected->validation_profile &&
        receipt_text_equal(st, 9, expected->producer_commit, 40) &&
        sqlite3_column_type(st, 10) == SQLITE_INTEGER &&
        sqlite3_column_int64(st, 10) == expected->fold_cursor &&
        receipt_blob_equal(st, 11, expected->receipt_digest);
    rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return FINAL_RECEIPT_READ_ERROR;
    return identical ? FINAL_RECEIPT_IDENTICAL : FINAL_RECEIPT_CONFLICT;
}

static bool exec_checked(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    bool ok = sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK;
    if (error) {
        LOG_WARN(PRODUCER_RECEIPT_SUBSYS, "exec failed: %s", error);
        sqlite3_free(error);
    }
    return ok;
}

bool consensus_state_producer_receipt_begin(sqlite3 *pdb,
                                            uint8_t validation_profile,
                                            char *err, size_t err_size)
{
    if (err && err_size)
        err[0] = '\0';
    if (!pdb)
        return set_err(err, err_size, "producer receipt begin: NULL store");
    if (validation_profile != CONSENSUS_STATE_VALIDATION_FULL &&
        validation_profile != CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD)
        return set_err(err, err_size,
                       "producer receipt begin: invalid validation profile");

    struct consensus_state_source_receipt claim;
    memset(&claim, 0, sizeof(claim));
    claim.validation_profile = validation_profile;
    if (!fill_source_identity_claim(&claim))
        return set_err(err, err_size,
                       "producer receipt begin: build has no exact 40-hex "
                       "commit; an unstamped build cannot earn a receipt");
    uint8_t source_epoch[32];
    consensus_state_source_epoch_digest(&claim, source_epoch);
    memcpy(claim.source_epoch_digest, source_epoch, 32);

    uint8_t running_binary[32];
    if (!running_binary_digest(running_binary))
        return set_err(err, err_size,
                       "producer receipt begin: running executable digest "
                       "failed");

    char datadir[576] = {0};
    (void)progress_store_path(datadir, sizeof(datadir));
    int64_t start_us = GetTimeMicros();

    bool committed = false;
    progress_store_tx_lock();
    if (!exec_checked(pdb, "BEGIN IMMEDIATE")) {
        progress_store_tx_unlock();
        return set_err(err, err_size,
                       "producer receipt begin: cannot open transaction");
    }
    bool ok = progress_meta_table_ensure(pdb) &&
              exec_checked(pdb, k_session_schema_sql);
    struct producer_session existing;
    if (ok)
        ok = load_session(pdb, &existing);
    if (ok && existing.present) {
        /* Resume: only the SAME running binary, epoch, and profile may adopt an
         * existing session. Anything else is a foreign generation. */
        if (memcmp(existing.running_binary_digest, running_binary, 32) != 0 ||
            memcmp(existing.source_epoch_digest, source_epoch, 32) != 0 ||
            existing.claim.validation_profile != validation_profile) {
            (void)exec_checked(pdb, "ROLLBACK");
            progress_store_tx_unlock();
            return set_err(err, err_size,
                           "producer receipt begin: datadir bound to a "
                           "different running binary / source epoch / profile");
        }
    } else if (ok) {
        ok = insert_session(pdb, &claim, running_binary, datadir, start_us);
    }
    /* Publish the source-epoch authority so fold stages stamp their rows. */
    if (ok)
        ok = progress_meta_set_in_tx(pdb, CONSENSUS_STATE_SOURCE_EPOCH_META_KEY,
                                     source_epoch, 32);
    if (ok)
        ok = exec_checked(pdb, "COMMIT");
    if (ok)
        committed = true;
    else
        (void)exec_checked(pdb, "ROLLBACK");
    progress_store_tx_unlock();
    if (!committed)
        return set_err(err, err_size,
                       "producer receipt begin: durable session write failed");
    return true;
}

bool consensus_state_producer_receipt_finalize(sqlite3 *pdb, int32_t height,
                                               const uint8_t block_hash[32],
                                               char *err, size_t err_size)
{
    if (err && err_size)
        err[0] = '\0';
    if (!pdb || !block_hash)
        return set_err(err, err_size, "producer receipt finalize: NULL arg");
    if (height < 0 || height == INT32_MAX)
        return set_err(err, err_size,
                       "producer receipt finalize: height out of range");

    uint8_t running_binary[32];
    if (!running_binary_digest(running_binary))
        return set_err(err, err_size,
                       "producer receipt finalize: running executable digest "
                       "failed");

    bool committed = false;
    progress_store_tx_lock();
    if (!exec_checked(pdb, "BEGIN IMMEDIATE")) {
        progress_store_tx_unlock();
        return set_err(err, err_size,
                       "producer receipt finalize: cannot open transaction");
    }

    struct producer_session session;
    bool ok = load_session(pdb, &session);
    if (ok && !session.present) {
        (void)exec_checked(pdb, "ROLLBACK");
        progress_store_tx_unlock();
        return set_err(err, err_size,
                       "producer receipt finalize: no start session — this "
                       "producer was not begun by a receipt-owning binary");
    }
    if (ok &&
        memcmp(session.running_binary_digest, running_binary, 32) != 0) {
        (void)exec_checked(pdb, "ROLLBACK");
        progress_store_tx_unlock();
        return set_err(err, err_size,
                       "producer receipt finalize: running executable does not "
                       "own the start session");
    }

    /* Rebuild the full receipt from the start-session claim, verify epoch
     * integrity, then bind chain corpus + fold cursor to the completed fold. */
    struct consensus_state_source_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    if (ok) {
        memcpy(receipt.source_tree_root, session.claim.source_tree_root, 32);
        memcpy(receipt.toolchain_digest, session.claim.toolchain_digest, 32);
        memcpy(receipt.build_inputs_digest, session.claim.build_inputs_digest,
               32);
        memcpy(receipt.producer_commit, session.claim.producer_commit, 41);
        receipt.source_clean = session.claim.source_clean;
        receipt.validation_profile = session.claim.validation_profile;
        uint8_t recomputed_epoch[32];
        consensus_state_source_epoch_digest(&receipt, recomputed_epoch);
        if (memcmp(recomputed_epoch, session.source_epoch_digest, 32) != 0) {
            (void)exec_checked(pdb, "ROLLBACK");
            progress_store_tx_unlock();
            return set_err(err, err_size,
                           "producer receipt finalize: session claim does not "
                           "reproduce its recorded source epoch");
        }
        memcpy(receipt.source_epoch_digest, recomputed_epoch, 32);
        memcpy(receipt.running_binary_digest, running_binary, 32);
    }

    if (ok &&
        !header_corpus_digest(pdb, height, block_hash,
                              receipt.chain_corpus_digest)) {
        (void)exec_checked(pdb, "ROLLBACK");
        progress_store_tx_unlock();
        return set_err(err, err_size,
                       "producer receipt finalize: genesis..H* header corpus "
                       "does not resolve the completed tip");
    }
    if (ok) {
        receipt.fold_cursor = (int64_t)height + 1;
        consensus_state_source_receipt_digest(&receipt, receipt.receipt_digest);
        ok = exec_checked(pdb, k_receipt_schema_sql);
    }
    enum final_receipt_state prior = FINAL_RECEIPT_READ_ERROR;
    if (ok)
        prior = final_receipt_state(pdb, &receipt);
    if (ok && prior == FINAL_RECEIPT_CONFLICT) {
        (void)exec_checked(pdb, "ROLLBACK");
        progress_store_tx_unlock();
        return set_err(err, err_size,
                       "producer receipt finalize: conflicting finalized "
                       "receipt already exists");
    }
    if (ok && prior == FINAL_RECEIPT_READ_ERROR) {
        (void)exec_checked(pdb, "ROLLBACK");
        progress_store_tx_unlock();
        return set_err(err, err_size,
                       "producer receipt finalize: existing receipt read "
                       "failed");
    }
    if (ok)
        ok = progress_meta_set_in_tx(
                 pdb, CONSENSUS_STATE_SOURCE_EPOCH_META_KEY,
                 receipt.source_epoch_digest, 32) &&
             (prior == FINAL_RECEIPT_IDENTICAL ||
              write_final_receipt(pdb, &receipt));
    if (ok)
        ok = exec_checked(pdb, "COMMIT");
    if (ok)
        committed = true;
    else
        (void)exec_checked(pdb, "ROLLBACK");
    progress_store_tx_unlock();
    if (!committed)
        return set_err(err, err_size,
                       "producer receipt finalize: durable receipt write "
                       "failed");
    return true;
}

/* ── Read-only producer status (node-free operator surface) ────────── */

/* Query one integer column from `sql` (optionally binding one text arg). On a
 * missing table (prepare fails) or no row, returns false and leaves *out. */
static bool pkv_query_i64(sqlite3 *db, const char *sql, const char *bind_text,
                          int64_t *out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    if (bind_text)
        sqlite3_bind_text(st, 1, bind_text, -1, SQLITE_TRANSIENT);
    bool present = false;
    int rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        *out = sqlite3_column_int64(st, 0);
        present = true;
    }
    sqlite3_finalize(st);
    return present;
}

/* Read producer_commit text (40-hex) from the receipt then the session row. */
static void pkv_read_producer_commit(sqlite3 *db, char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    static const char *const sqls[] = {
        "SELECT producer_commit FROM consensus_state_source_receipt "
        "WHERE singleton=1",
        "SELECT producer_commit FROM consensus_state_producer_session "
        "WHERE singleton=1",
    };
    for (size_t i = 0; i < sizeof(sqls) / sizeof(sqls[0]); i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sqls[i], -1, &st, NULL) != SQLITE_OK)
            continue;
        int rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
        if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_TEXT) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            if (t && t[0]) {
                snprintf(out, cap, "%s", (const char *)t);
                sqlite3_finalize(st);
                return;
            }
        }
        sqlite3_finalize(st);
    }
}

bool consensus_state_producer_status_read(const char *datadir,
                                          struct producer_status_read *out,
                                          char *err, size_t err_size)
{
    if (!out)
        return set_err(err, err_size, "producer status: null out");
    memset(out, 0, sizeof(*out));
    out->utxo_apply_cursor = -1;
    out->tip_finalize_cursor = -1;
    out->fold_cursor = -1;
    out->validation_profile = -1;
    if (!datadir || !datadir[0])
        return set_err(err, err_size, "producer status: empty datadir");

    char path[1100];
    snprintf(path, sizeof(path), "%s/progress.kv", datadir);
    struct stat stbuf;
    if (stat(path, &stbuf) != 0 || !S_ISREG(stbuf.st_mode)) {
        /* No progress.kv: a producer that has not started. Not an error. */
        out->progress_kv_present = false;
        return true;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        const char *m = db ? sqlite3_errmsg(db) : "open failed";
        char why[256];
        snprintf(why, sizeof(why), "producer status: open %s: %s", path, m);
        if (db) sqlite3_close(db);
        return set_err(err, err_size, why);
    }
    out->progress_kv_present = true;

    int64_t v = 0;
    if (pkv_query_i64(db,
            "SELECT cursor FROM stage_cursor WHERE name='utxo_apply'", NULL,
            &v))
        out->utxo_apply_cursor = v;
    if (pkv_query_i64(db,
            "SELECT cursor FROM stage_cursor WHERE name='tip_finalize'", NULL,
            &v))
        out->tip_finalize_cursor = v;

    /* session presence + start time + validation profile */
    if (pkv_query_i64(db,
            "SELECT start_time_us FROM consensus_state_producer_session "
            "WHERE singleton=1", NULL, &v)) {
        out->session_open = true;
        out->start_time_us = v;
    }
    if (pkv_query_i64(db,
            "SELECT validation_profile FROM consensus_state_producer_session "
            "WHERE singleton=1", NULL, &v))
        out->validation_profile = (int)v;

    /* receipt presence (fold complete) + fold cursor */
    if (pkv_query_i64(db,
            "SELECT fold_cursor FROM consensus_state_source_receipt "
            "WHERE singleton=1", NULL, &v)) {
        out->receipt_finalized = true;
        out->fold_cursor = v;
    }

    pkv_read_producer_commit(db, out->producer_commit,
                             sizeof(out->producer_commit));

    sqlite3_close(db);
    return true;
}
