/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Frozen-source proof gate for zcl.consensus_state_bundle.v1 export. */

#include "consensus_state_snapshot_export_internal.h"
#include "consensus_state_sqlite_text.h"

#include "chain/checkpoints.h"
#include "crypto/sha3.h"
#include "jobs/reducer_frontier.h"
#include "jobs/refold_progress.h"
#include "jobs/tip_finalize_stage.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
#include "storage/coins_ram.h"
#include "storage/nullifier_kv.h"
#include "util/log_macros.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EXPORT_PROOF_SUBSYS "consensus_bundle_export"

struct export_stage_proof {
    const char *name;
    const char *table;
    bool served_tip_cursor;
    const char *hash_column;
    bool profile_bound;
    bool source_epoch_bound;
};

static const struct export_stage_proof k_stages[] = {
    {"validate_headers", "validate_headers_log", false, "hash", false, false},
    {"body_fetch", "body_fetch_log", false, "hash", false, false},
    {"body_persist", "body_persist_log", false, NULL, false, false},
    {"script_validate", "script_validate_log", false, "block_hash", true, true},
    {"proof_validate", "proof_validate_log", false, "block_hash", true, true},
    {"utxo_apply", "utxo_apply_log", false, "branch_hash", true, false},
    {"tip_finalize", "tip_finalize_log", true, NULL, false, false},
};

static void proof_u64(struct sha3_256_ctx *ctx, uint64_t value)
{
    uint8_t le[8];
    for (size_t i = 0; i < sizeof(le); i++)
        le[i] = (uint8_t)(value >> (8u * i));
    sha3_256_write(ctx, le, sizeof(le));
}

static bool digest_nonzero(const uint8_t digest[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++)
        any |= digest[i];
    return any != 0;
}

static bool running_binary_digest(uint8_t out[32])
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_WARN(EXPORT_PROOF_SUBSYS, "running executable open failed");
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
        LOG_WARN(EXPORT_PROOF_SUBSYS, "running executable digest failed");
    return ok;
}

static bool copy_receipt_blob(sqlite3_stmt *st, int column, uint8_t out[32])
{
    if (sqlite3_column_type(st, column) != SQLITE_BLOB)
        return false;
    const void *blob = sqlite3_column_blob(st, column);
    if (!blob || sqlite3_column_bytes(st, column) != 32)
        return false;
    memcpy(out, blob, 32);
    return true;
}

static bool prove_source_receipt(sqlite3 *db, int64_t fold_cursor,
                                 const uint8_t chain_corpus_digest[32],
                                 bool checkpoint_content_export,
                                 struct consensus_state_source_receipt *receipt,
                                 uint8_t source_digest[32])
{
    static const char sql[] =
        "SELECT singleton,schema,source_epoch_digest,source_tree_root,"
        "running_binary_digest,toolchain_digest,build_inputs_digest,"
        "chain_corpus_digest,source_clean,validation_profile,producer_commit,"
        "fold_cursor,receipt_digest "
        "FROM consensus_state_source_receipt ORDER BY singleton";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(EXPORT_PROOF_SUBSYS, "source provenance receipt unavailable");
        return false;
    }
    memset(receipt, 0, sizeof(*receipt));
    int rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
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
            (const char *)schema, (size_t)schema_len, &receipt_version) &&
        receipt_version == CONSENSUS_STATE_SOURCE_RECEIPT_V2;
    bool ok = rc == SQLITE_ROW && commit &&
              sqlite3_column_type(st, 0) == SQLITE_INTEGER &&
              sqlite3_column_int(st, 0) == 1 &&
              schema_ok &&
              copy_receipt_blob(st, 2, receipt->source_epoch_digest) &&
              copy_receipt_blob(st, 3, receipt->source_tree_root) &&
              copy_receipt_blob(st, 4, receipt->running_binary_digest) &&
              copy_receipt_blob(st, 5, receipt->toolchain_digest) &&
              copy_receipt_blob(st, 6, receipt->build_inputs_digest) &&
              copy_receipt_blob(st, 7, receipt->chain_corpus_digest) &&
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
              sqlite3_column_int64(st, 11) == fold_cursor &&
              copy_receipt_blob(st, 12, receipt->receipt_digest);
    if (ok) {
        receipt->schema_version = receipt_version;
        memcpy(receipt->producer_commit, commit, (size_t)commit_len);
        receipt->producer_commit[commit_len] = '\0';
        receipt->source_clean = sqlite3_column_int(st, 8) == 1;
        receipt->validation_profile =
            (uint8_t)sqlite3_column_int(st, 9);
        receipt->fold_cursor = sqlite3_column_int64(st, 11);
        rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
        ok = rc == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    uint8_t executable[32];
    uint8_t recomputed[32];
    uint8_t source_epoch[32];
    if (ok)
        ok = digest_nonzero(receipt->source_epoch_digest) &&
             digest_nonzero(receipt->source_tree_root) &&
             digest_nonzero(receipt->toolchain_digest) &&
             digest_nonzero(receipt->build_inputs_digest) &&
             consensus_state_source_receipt_commit_valid(
                 receipt->schema_version, receipt->producer_commit,
                 strnlen(receipt->producer_commit,
                         sizeof(receipt->producer_commit))) &&
             memcmp(receipt->chain_corpus_digest, chain_corpus_digest, 32) == 0 &&
             /* Fold-binary provenance vs checkpoint-content proof. The default
              * export binds the receipt to the running binary that folded the
              * state. The checkpoint-content path (caller-gated by an exact
              * compiled-checkpoint + PoW-header content match in
              * prove_checkpoint_content) instead only requires the receipt's
              * running-binary digest to be well-formed (nonzero) — the state's
              * authority comes from the SHA3 + header-root proof, not from
              * re-running the exact fold binary. No downstream gate re-checks
              * this digest, so the emitted bundle is byte-identical in shape. */
             (checkpoint_content_export
                  ? digest_nonzero(receipt->running_binary_digest)
                  : (running_binary_digest(executable) &&
                     memcmp(receipt->running_binary_digest, executable, 32) ==
                         0));
    if (ok) {
        consensus_state_source_epoch_digest(receipt, source_epoch);
        consensus_state_source_receipt_digest(receipt, recomputed);
        ok = memcmp(receipt->source_epoch_digest, source_epoch, 32) == 0 &&
             memcmp(receipt->receipt_digest, recomputed, 32) == 0 &&
             digest_nonzero(recomputed);
    }
    if (!ok) {
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "source provenance receipt missing, malformed, stale, or "
                 "legacy inspection-only v1");
        return false;
    }
    memcpy(source_digest, recomputed, 32);
    return true;
}

static bool read_cursor(sqlite3 *db, const char *name, uint64_t *out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name=?", -1, &st,
            NULL) != SQLITE_OK) {
        LOG_WARN(EXPORT_PROOF_SUBSYS, "cursor prepare failed stage=%s: %s",
                 name, sqlite3_errmsg(db));
        return false;
    }
    bool ok = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC) == SQLITE_OK;
    int rc = ok ? sqlite3_step(st) : SQLITE_ERROR; // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER &&
        sqlite3_column_int64(st, 0) >= 0) {
        *out = (uint64_t)sqlite3_column_int64(st, 0);
        rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
        ok = rc == SQLITE_DONE;
    } else {
        ok = false;
    }
    sqlite3_finalize(st);
    if (!ok)
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "cursor missing/malformed stage=%s", name);
    return ok;
}

static bool prove_header_chain(sqlite3 *db, int32_t height,
                               const uint8_t expected_hash[32],
                               uint8_t source_digest[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height,hash,parent_hash FROM header_admit_log "
            "WHERE height BETWEEN 0 AND ? ORDER BY height", -1, &st,
            NULL) != SQLITE_OK) {
        LOG_WARN(EXPORT_PROOF_SUBSYS, "header proof prepare failed: %s",
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
        bool genesis_parent = row_height == 0 &&
                              parent_type == SQLITE_NULL;
        bool linked_parent = row_height > 0 && parent_type == SQLITE_BLOB &&
                             parent && parent_len == 32 &&
                             memcmp(parent, prior, 32) == 0;
        if (height_type != SQLITE_INTEGER ||
            hash_type != SQLITE_BLOB || !hash ||
            sqlite3_column_bytes(st, 1) != 32 ||
            row_height != expected_height ||
            (!genesis_parent && !linked_parent)) {
            ok = false;
            break;
        }
        proof_u64(&ctx, (uint64_t)row_height);
        sha3_256_write(&ctx, hash, 32);
        if (row_height > 0)
            sha3_256_write(&ctx, parent, 32);
        else {
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
        sha3_256_finalize(&ctx, source_digest);
    else
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "header chain is incomplete, discontinuous, or at wrong tip");
    return ok;
}

static bool prove_stage_rows(sqlite3 *db,
                             const struct export_stage_proof *stage,
                             int32_t height, uint64_t cursor,
                             uint8_t validation_profile,
                             const uint8_t source_epoch_digest[32],
                             struct consensus_state_bundle_proof_summary *summary)
{
    char sql[384];
    const char *columns = stage->source_epoch_bound
        ? "height,ok,status,source_epoch_digest"
        : stage->profile_bound ? "height,ok,status" : "height,ok";
    int n = snprintf(
        sql, sizeof(sql), "SELECT %s FROM %s "
        "WHERE height BETWEEN 0 AND ? ORDER BY height", columns,
        stage->table);
    if (n <= 0 || (size_t)n >= sizeof(sql))
        LOG_FAIL(EXPORT_PROOF_SUBSYS, "stage proof SQL overflow");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(EXPORT_PROOF_SUBSYS, "stage proof missing table=%s: %s",
                 stage->table, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    memset(summary, 0, sizeof(*summary));
    snprintf(summary->component, sizeof(summary->component), "%s",
             stage->name);
    summary->cursor = cursor;
    summary->first_height = 0;
    summary->last_height = height;
    summary->row_count = (uint64_t)height + 1;
    struct sha3_256_ctx component;
    sha3_256_init(&component);
    static const char domain[] =
        "zcl.consensus_state_bundle.v1/proof-component";
    sha3_256_write(&component, (const uint8_t *)domain, sizeof(domain));
    proof_u64(&component, (uint64_t)strlen(stage->name));
    sha3_256_write(&component, (const uint8_t *)stage->name,
                   strlen(stage->name));
    proof_u64(&component, cursor);
    if (stage->source_epoch_bound)
        sha3_256_write(&component, source_epoch_digest, 32);

    int64_t expected_height = 0;
    bool ok = true;
    int rc;
    const char *required_status =
        validation_profile == CONSENSUS_STATE_VALIDATION_FULL
            ? "verified" : "checkpoint_fold";
    size_t required_status_len = strlen(required_status);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { // raw-sql-ok:progress-kv-kernel-store
        int height_type = sqlite3_column_type(st, 0);
        int verdict_type = sqlite3_column_type(st, 1);
        int status_type = stage->profile_bound
            ? sqlite3_column_type(st, 2) : SQLITE_NULL;
        int epoch_type = stage->source_epoch_bound
            ? sqlite3_column_type(st, 3) : SQLITE_NULL;
        int64_t row_height = height_type == SQLITE_INTEGER
            ? sqlite3_column_int64(st, 0) : -1;
        int verdict = verdict_type == SQLITE_INTEGER
            ? sqlite3_column_int(st, 1) : -1;
        const unsigned char *status = status_type == SQLITE_TEXT
                                          ? sqlite3_column_text(st, 2) : NULL;
        const void *row_epoch = epoch_type == SQLITE_BLOB
                                    ? sqlite3_column_blob(st, 3) : NULL;
        if (height_type != SQLITE_INTEGER || verdict_type != SQLITE_INTEGER ||
            row_height != expected_height || verdict != 1 ||
            (stage->profile_bound &&
             (status_type != SQLITE_TEXT || !status ||
              sqlite3_column_bytes(st, 2) != (int)required_status_len ||
              memcmp(status, required_status, required_status_len) != 0)) ||
            (stage->source_epoch_bound &&
             (epoch_type != SQLITE_BLOB || !row_epoch ||
              sqlite3_column_bytes(st, 3) != 32 ||
              memcmp(row_epoch, source_epoch_digest, 32) != 0))) {
            ok = false;
            break;
        }
        proof_u64(&component, (uint64_t)row_height);
        uint8_t accepted = 1;
        sha3_256_write(&component, &accepted, 1);
        if (stage->profile_bound) {
            proof_u64(&component, (uint64_t)required_status_len);
            sha3_256_write(&component, (const uint8_t *)required_status,
                           required_status_len);
        }
        expected_height++;
    }
    if (rc != SQLITE_DONE || expected_height != (int64_t)height + 1)
        ok = false;
    sqlite3_finalize(st);
    if (!ok) {
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "stage proof is not a complete profile-bound ok=1 prefix "
                 "table=%s height=%d profile=%u",
                 stage->table, height, (unsigned)validation_profile);
        return false;
    }

    if (!stage->hash_column) {
        proof_u64(&component, 0);
        sha3_256_finalize(&component, summary->component_digest);
        return true;
    }
    if (strcmp(stage->name, "utxo_apply") == 0) {
        n = snprintf(sql, sizeof(sql),
                     "SELECT COUNT(*) FROM utxo_apply_log s "
                     "JOIN utxo_apply_delta d ON d.height=s.height "
                     "JOIN header_admit_log h ON h.height=s.height "
                     "AND h.hash=d.branch_hash "
                     "WHERE s.height BETWEEN 0 AND ? "
                     "AND typeof(s.ok)='integer' AND s.ok=1 "
                     "AND typeof(d.branch_hash)='blob' "
                     "AND length(d.branch_hash)=32");
    } else {
        n = snprintf(sql, sizeof(sql),
                     "SELECT COUNT(*) FROM %s s JOIN header_admit_log h "
                     "ON h.height=s.height AND h.hash=s.%s "
                     "WHERE s.height BETWEEN 0 AND ? "
                     "AND typeof(s.ok)='integer' AND s.ok=1 "
                     "AND typeof(s.%s)='blob' AND length(s.%s)=32",
                     stage->table, stage->hash_column, stage->hash_column,
                     stage->hash_column);
    }
    if (n <= 0 || (size_t)n >= sizeof(sql))
        LOG_FAIL(EXPORT_PROOF_SUBSYS, "stage hash SQL overflow");
    st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(EXPORT_PROOF_SUBSYS, "stage hash proof prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
    int count_type = rc == SQLITE_ROW ? sqlite3_column_type(st, 0) : SQLITE_NULL;
    int64_t hash_bound_count = rc == SQLITE_ROW && count_type == SQLITE_INTEGER
        ? sqlite3_column_int64(st, 0) : -1;
    ok = rc == SQLITE_ROW && count_type == SQLITE_INTEGER &&
         hash_bound_count == (int64_t)height + 1;
    if (ok)
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (!ok)
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "stage rows are not bound to header hashes table=%s",
                 stage->table);
    if (ok) {
        summary->hash_bound_count = (uint64_t)hash_bound_count;
        proof_u64(&component, summary->hash_bound_count);
        sha3_256_finalize(&component, summary->component_digest);
    }
    return ok;
}

/* Cryptographic checkpoint-content proof that authorizes a checkpoint-content
 * export in place of the fold-binary-identity receipt gate. Every rung is a
 * content fact:
 *   1. expected_height == the compiled SHA3 UTXO checkpoint height (the export
 *      is only admissible AT the checkpoint — the strongest content anchor);
 *   2. the frozen transparent coins reproduce the checkpoint's SHA3 + count
 *      bit-for-bit (coins_kv_commitment canonically encodes each coin's value,
 *      so a matching SHA3 also fixes the total supply — no separate scan);
 *   3. the Sapling tip frontier Pedersen-roots to the block header's committed
 *      hashFinalSaplingRoot at that height (checkpoint_sapling_root, supplied
 *      from this node's validated header chain) — the shielded tip is bound to
 *      PoW. The install side re-derives this same binding against block_index;
 *      requiring it here fails a header-inconsistent frontier at export time.
 * Any mismatch refuses and emits nothing. Does not mutate the source. */
static bool prove_checkpoint_content(
    sqlite3 *source,
    const struct consensus_state_snapshot_export_request *request,
    struct consensus_state_export_result *result)
{
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "no compiled SHA3 UTXO checkpoint to bind the export against");
    if (request->expected_height != cp->height)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "checkpoint-content export is admissible only at the compiled "
            "checkpoint height (want=%d got=%d)",
            cp->height, request->expected_height);

    uint8_t coins_sha3[32];
    if (coins_kv_commitment(source, coins_sha3) != 0)
        return consensus_export_fail(result, CONSENSUS_EXPORT_STORE_ERROR,
                                     "coins commitment computation failed");
    int64_t coins_count = coins_kv_count(source);
    if (coins_count < 0)
        return consensus_export_fail(result, CONSENSUS_EXPORT_STORE_ERROR,
                                     "coins count read failed");
    if (memcmp(coins_sha3, cp->sha3_hash, 32) != 0 ||
        (uint64_t)coins_count != cp->utxo_count)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "coins do not reproduce the compiled SHA3 UTXO checkpoint");

    struct incremental_merkle_tree sapling_tip;
    struct uint256 sapling_root;
    int64_t sapling_height = -1;
    if (anchor_kv_latest_tree(source, ANCHOR_POOL_SAPLING, &sapling_tip,
                              &sapling_root, &sapling_height) != ANCHOR_KV_FOUND)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "Sapling tip frontier is absent; cannot bind it to the PoW header "
            "committed final Sapling root");
    if (memcmp(sapling_root.data, request->checkpoint_sapling_root, 32) != 0)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "Sapling tip frontier does not Pedersen-root to the header "
            "committed hashFinalSaplingRoot at the checkpoint height");
    return true;
}

bool consensus_export_prove_source(
    sqlite3 *source,
    const struct consensus_state_snapshot_export_request *request,
    struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_source_receipt *receipt,
    struct consensus_state_bundle_proof_summary
        proofs[CONSENSUS_STATE_BUNDLE_PROOF_COUNT],
    struct consensus_state_export_result *result)
{
    if (coins_ram_active())
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "durable export refused while coins RAM overlay is active");
    if (!coins_kv_is_proven_authority(source, NULL) ||
        !coins_kv_contains_refold_marker(source))
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "coins source lacks durable migration and self-folded proof");

    int32_t applied = -1;
    bool applied_found = false;
    if (!coins_kv_get_applied_height(source, &applied, &applied_found))
        return consensus_export_fail(result, CONSENSUS_EXPORT_STORE_ERROR,
                                     "coins applied-height read failed");
    if (!applied_found || applied != request->expected_height + 1)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "coins applied-height does not equal expected H+1");

    int64_t sprout_cursor = -1;
    int64_t sapling_cursor = -1;
    int64_t nullifier_cursor = -1;
    bool sprout_found = false;
    bool sapling_found = false;
    bool nullifier_found = false;
    if (!anchor_kv_activation_cursor(source, ANCHOR_POOL_SPROUT,
                                     &sprout_cursor, &sprout_found) ||
        !anchor_kv_activation_cursor(source, ANCHOR_POOL_SAPLING,
                                     &sapling_cursor, &sapling_found) ||
        !nullifier_kv_activation_cursor(source, &nullifier_cursor,
                                        &nullifier_found))
        return consensus_export_fail(result, CONSENSUS_EXPORT_STORE_ERROR,
                                     "shielded activation cursor read failed");
    if (!sprout_found || !sapling_found || !nullifier_found ||
        sprout_cursor != 0 || sapling_cursor != 0 || nullifier_cursor != 0)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "shielded history is not explicitly complete from genesis");

    uint64_t header_cursor = 0;
    if (!read_cursor(source, "header_admit", &header_cursor) ||
        header_cursor < (uint64_t)request->expected_height + 1)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "header reducer cursor does not cover requested height");

    memset(manifest, 0, sizeof(*manifest));
    manifest->height = request->expected_height;
    memcpy(manifest->block_hash, request->expected_block_hash, 32);
    manifest->history_complete = true;
    manifest->activation_boundary = 0;
    manifest->sprout_source_cursor = 0;
    manifest->sapling_source_cursor = 0;
    manifest->nullifier_source_cursor = 0;
    manifest->source_fold_cursor = (int64_t)request->expected_height + 1;
    uint8_t chain_corpus_digest[32];
    if (!prove_header_chain(source, request->expected_height,
                            request->expected_block_hash,
                            chain_corpus_digest))
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "complete genesis-to-height header proof is unavailable");

    /* Checkpoint-content export authority: a compiled-checkpoint + PoW-header
     * content proof that authorizes relaxing the receipt's fold-binary-identity
     * bind below. It TIGHTENS the admission (exact checkpoint coins SHA3 + count
     * + header Sapling-root match) — every other rung stays identical. */
    if (request->checkpoint_content_export &&
        !prove_checkpoint_content(source, request, result))
        return false;
    if (!prove_source_receipt(source, manifest->source_fold_cursor,
                              chain_corpus_digest,
                              request->checkpoint_content_export, receipt,
                              manifest->source_digest))
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "durable producer source provenance receipt unavailable");
    manifest->validation_profile = receipt->validation_profile;
    manifest->source_clean = receipt->source_clean;
    if (manifest->validation_profile != CONSENSUS_STATE_VALIDATION_FULL)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "checkpoint-fold state is non-serving producer evidence and "
            "cannot be published by the canonical bundle exporter");

    /* Refresh the durable refold mode before computing H*: its floor is a
     * cached atomic, so a stale process value can otherwise validate the
     * wrong lattice. H* and the convention-aware durable tip must both name
     * this exact generation. */
    if (!refold_progress_refresh(source))
        return consensus_export_fail(result, CONSENSUS_EXPORT_STORE_ERROR,
                                     "durable refold mode refresh failed");
    int32_t hstar = -1;
    int32_t served_floor = -1;
    if (!reducer_frontier_compute_hstar(source, &hstar, &served_floor))
        return consensus_export_fail(result, CONSENSUS_EXPORT_STORE_ERROR,
                                     "reducer H* computation failed");
    if (hstar != request->expected_height ||
        served_floor < request->expected_height)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "frozen source is not the exact durable reducer generation");
    /* Bind the SERVED tip's own block hash. H* (== expected_height, proven
     * just above) IS the reducer's provable served tip, so the height is
     * already bound exactly — the remaining property is that the served tip
     * owns expected_block_hash. Read that height's hash witness CONVENTION-
     * AWARE via tip_finalize_stage_block_hash_at (the finalized ok=1 row at
     * expected_height-1 carries the LOOKAHEAD hash(expected_height); an anchor
     * seed row at expected_height carries its own hash) — the same served-tip
     * hash binding derive_coins_best uses at applied-1. Do NOT resolve via the
     * cursor: a fold-to-anchor producer's tip_finalize cursor sits at
     * expected_height+1 (the H+1 steady-state convention), so
     * tip_finalize_stage_resolve_durable_tip floats one above the served tip
     * through the finalized lookahead — a different notion from the served H*
     * that false-rejects a complete producer. */
    uint8_t served_tip_hash[32];
    if (!tip_finalize_stage_block_hash_at(
            source, request->expected_height, served_tip_hash) ||
        memcmp(served_tip_hash, request->expected_block_hash, 32) != 0)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "durable served tip does not own expected height/hash");

    memset(proofs, 0, sizeof(*proofs) * CONSENSUS_STATE_BUNDLE_PROOF_COUNT);
    snprintf(proofs[0].component, sizeof(proofs[0].component),
             "header_admit");
    proofs[0].cursor = header_cursor;
    proofs[0].first_height = 0;
    proofs[0].last_height = request->expected_height;
    proofs[0].row_count = (uint64_t)request->expected_height + 1;
    proofs[0].hash_bound_count = proofs[0].row_count;
    memcpy(proofs[0].component_digest, chain_corpus_digest, 32);

    for (size_t i = 0; i < sizeof(k_stages) / sizeof(k_stages[0]); i++) {
        uint64_t cursor = 0;
        if (!read_cursor(source, k_stages[i].name, &cursor))
            return consensus_export_fail(
                result, CONSENSUS_EXPORT_MISSING_PROOF,
                "required reducer cursor is unavailable stage=%s",
                k_stages[i].name);
        uint64_t required = k_stages[i].served_tip_cursor
                                ? (uint64_t)request->expected_height
                                : (uint64_t)request->expected_height + 1;
        bool cursor_ok = k_stages[i].served_tip_cursor
                             ? cursor >= required && cursor <= required + 1
                             : cursor >= required;
        if (!cursor_ok)
            return consensus_export_fail(
                result, CONSENSUS_EXPORT_MISSING_PROOF,
                "reducer cursor does not cover frozen generation stage=%s",
                k_stages[i].name);
        if (strcmp(k_stages[i].name, "utxo_apply") == 0 &&
            cursor != (uint64_t)request->expected_height + 1)
            return consensus_export_fail(
                result, CONSENSUS_EXPORT_MISSING_PROOF,
                "utxo cursor does not equal frozen coin generation");
        if (!prove_stage_rows(source, &k_stages[i], request->expected_height,
                              cursor, manifest->validation_profile,
                              receipt->source_epoch_digest,
                              &proofs[i + 1]))
            return consensus_export_fail(
                result, CONSENSUS_EXPORT_MISSING_PROOF,
                "complete reducer proof rows unavailable stage=%s",
                k_stages[i].name);
    }
    consensus_state_bundle_proof_manifest_digest(
        proofs, CONSENSUS_STATE_BUNDLE_PROOF_COUNT,
        manifest->proof_manifest_digest);
    return true;
}
