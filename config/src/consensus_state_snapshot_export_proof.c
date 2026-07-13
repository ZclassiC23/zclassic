/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Frozen-source proof gate for zcl.consensus_state_bundle.v1 export. */

#include "consensus_state_snapshot_export_internal.h"

#include "crypto/sha3.h"
#include "jobs/reducer_frontier.h"
#include "jobs/refold_progress.h"
#include "jobs/tip_finalize_stage.h"
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
};

static const struct export_stage_proof k_stages[] = {
    {"validate_headers", "validate_headers_log", false, "hash"},
    {"body_fetch", "body_fetch_log", false, "hash"},
    {"body_persist", "body_persist_log", false, NULL},
    {"script_validate", "script_validate_log", false, "block_hash"},
    {"proof_validate", "proof_validate_log", false, "block_hash"},
    {"utxo_apply", "utxo_apply_log", false, NULL},
    {"tip_finalize", "tip_finalize_log", true, NULL},
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

static bool lowercase_full_commit(const char commit[41])
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
    const void *blob = sqlite3_column_blob(st, column);
    if (sqlite3_column_type(st, column) != SQLITE_BLOB || !blob ||
        sqlite3_column_bytes(st, column) != 32)
        return false;
    memcpy(out, blob, 32);
    return true;
}

static bool prove_source_receipt(sqlite3 *db, int64_t fold_cursor,
                                 const uint8_t chain_corpus_digest[32],
                                 struct consensus_state_source_receipt *receipt,
                                 uint8_t source_digest[32])
{
    static const char sql[] =
        "SELECT singleton,schema,source_tree_root,running_binary_digest,"
        "toolchain_digest,chain_corpus_digest,producer_commit,fold_cursor,"
        "receipt_digest FROM consensus_state_source_receipt ORDER BY singleton";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(EXPORT_PROOF_SUBSYS, "source provenance receipt unavailable");
        return false;
    }
    memset(receipt, 0, sizeof(*receipt));
    int rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
    const unsigned char *schema = rc == SQLITE_ROW
                                      ? sqlite3_column_text(st, 1) : NULL;
    const unsigned char *commit = rc == SQLITE_ROW
                                      ? sqlite3_column_text(st, 6) : NULL;
    bool ok = rc == SQLITE_ROW && schema && commit &&
              sqlite3_column_type(st, 0) == SQLITE_INTEGER &&
              sqlite3_column_int(st, 0) == 1 &&
              sqlite3_column_type(st, 1) == SQLITE_TEXT &&
              strcmp((const char *)schema,
                     CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA) == 0 &&
              copy_receipt_blob(st, 2, receipt->source_tree_root) &&
              copy_receipt_blob(st, 3, receipt->running_binary_digest) &&
              copy_receipt_blob(st, 4, receipt->toolchain_digest) &&
              copy_receipt_blob(st, 5, receipt->chain_corpus_digest) &&
              sqlite3_column_type(st, 6) == SQLITE_TEXT &&
              sqlite3_column_bytes(st, 6) == 40 &&
              sqlite3_column_type(st, 7) == SQLITE_INTEGER &&
              sqlite3_column_int64(st, 7) == fold_cursor &&
              copy_receipt_blob(st, 8, receipt->receipt_digest);
    if (ok) {
        memcpy(receipt->producer_commit, commit, 40);
        receipt->producer_commit[40] = '\0';
        receipt->fold_cursor = sqlite3_column_int64(st, 7);
        rc = sqlite3_step(st); // raw-sql-ok:progress-kv-kernel-store
        ok = rc == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    uint8_t executable[32];
    uint8_t recomputed[32];
    if (ok)
        ok = digest_nonzero(receipt->source_tree_root) &&
             digest_nonzero(receipt->toolchain_digest) &&
             lowercase_full_commit(receipt->producer_commit) &&
             memcmp(receipt->chain_corpus_digest, chain_corpus_digest, 32) == 0 &&
             running_binary_digest(executable) &&
             memcmp(receipt->running_binary_digest, executable, 32) == 0;
    if (ok) {
        consensus_state_source_receipt_digest(receipt, recomputed);
        ok = memcmp(receipt->receipt_digest, recomputed, 32) == 0 &&
             digest_nonzero(recomputed);
    }
    if (!ok) {
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "source provenance receipt missing, malformed, or stale");
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
        const void *hash = sqlite3_column_blob(st, 1);
        const void *parent = sqlite3_column_blob(st, 2);
        int parent_len = sqlite3_column_bytes(st, 2);
        int64_t row_height = sqlite3_column_int64(st, 0);
        bool genesis_parent = row_height == 0 &&
                              sqlite3_column_type(st, 2) == SQLITE_NULL;
        bool linked_parent = row_height > 0 && parent && parent_len == 32 &&
                             memcmp(parent, prior, 32) == 0;
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER ||
            sqlite3_column_type(st, 1) != SQLITE_BLOB || !hash ||
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
                             struct consensus_state_bundle_proof_summary *summary)
{
    char sql[256];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT height,ok FROM %s WHERE height BETWEEN 0 AND ? "
                     "ORDER BY height", stage->table);
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

    int64_t expected_height = 0;
    bool ok = true;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { // raw-sql-ok:progress-kv-kernel-store
        int64_t row_height = sqlite3_column_int64(st, 0);
        int verdict = sqlite3_column_int(st, 1);
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER ||
            sqlite3_column_type(st, 1) != SQLITE_INTEGER ||
            row_height != expected_height || verdict != 1) {
            ok = false;
            break;
        }
        proof_u64(&component, (uint64_t)row_height);
        uint8_t accepted = 1;
        sha3_256_write(&component, &accepted, 1);
        expected_height++;
    }
    if (rc != SQLITE_DONE || expected_height != (int64_t)height + 1)
        ok = false;
    sqlite3_finalize(st);
    if (!ok) {
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "stage proof is not a complete ok=1 prefix table=%s height=%d",
                 stage->table, height);
        return false;
    }

    if (!stage->hash_column) {
        proof_u64(&component, 0);
        sha3_256_finalize(&component, summary->component_digest);
        return true;
    }
    n = snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM %s s JOIN header_admit_log h "
                 "ON h.height=s.height AND h.hash=s.%s "
                 "WHERE s.height BETWEEN 0 AND ? AND s.ok=1",
                 stage->table, stage->hash_column);
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
    int64_t hash_bound_count =
        rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    ok = rc == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER &&
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
    int durable_height = -1;
    uint8_t durable_hash[32];
    if (!tip_finalize_stage_resolve_durable_tip(
            source, &durable_height, durable_hash) ||
        durable_height != request->expected_height ||
        memcmp(durable_hash, request->expected_block_hash, 32) != 0)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "durable served tip does not own expected height/hash");

    if (!prove_source_receipt(source, manifest->source_fold_cursor,
                              chain_corpus_digest, receipt,
                              manifest->source_digest))
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_MISSING_PROOF,
            "durable producer source provenance receipt unavailable");

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
                              cursor, &proofs[i + 1]))
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
