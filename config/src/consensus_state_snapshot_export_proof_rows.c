/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Reducer-stage + header-chain row-scan proofs for the
 * zcl.consensus_state_bundle.v1 export gate. Split out of
 * consensus_state_snapshot_export_proof.c along the file-size ceiling seam
 * (E1, docs/DEFENSIVE_CODING.md): these two functions are the O(H)-row scans
 * (up to ~3.1M rows on a full-history datadir) that consensus_export_
 * prove_source (still in _proof.c) orchestrates alongside its own fast,
 * non-scanning checks. Both emit "[export] ..." start/progress/done markers
 * via consensus_export_progress_emit (observability only — never touches gate
 * semantics) so the otherwise-silent 30-60+ minute -export-consensus-bundle
 * prove path is never quiet for more than ~500k rows at a time. */

#include "consensus_state_snapshot_export_internal.h"

#include "crypto/sha3.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>

#define EXPORT_PROOF_SUBSYS "consensus_bundle_export"

const struct export_stage_proof k_stages[CONSENSUS_EXPORT_STAGE_COUNT] = {
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

bool consensus_export_prove_header_chain(sqlite3 *db, int32_t height,
                                         const uint8_t expected_hash[32],
                                         uint8_t source_digest[32])
{
    int64_t t0 = consensus_export_clock_ms();
    consensus_export_progress_emit(
        "prove_header_chain start height=%d rows=%lld", height,
        (long long)height + 1);
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
    int64_t fail_height = -1;
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
            fail_height = row_height;
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
        if (expected_height % 500000 == 0)
            consensus_export_progress_emit(
                "prove_header_chain: %lld/%lld rows, %llds elapsed",
                (long long)expected_height, (long long)height + 1,
                (long long)((consensus_export_clock_ms() - t0) / 1000));
    }
    bool tip_hash_mismatch = false;
    if (rc != SQLITE_DONE || expected_height != (int64_t)height + 1 ||
        memcmp(prior, expected_hash, 32) != 0) {
        tip_hash_mismatch = rc == SQLITE_DONE &&
            expected_height == (int64_t)height + 1;
        ok = false;
    }
    sqlite3_finalize(st);
    if (ok)
        sha3_256_finalize(&ctx, source_digest);
    else if (tip_hash_mismatch) {
        char derived_hex[65], expected_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(derived_hex + 2 * i, 3, "%02x", prior[i]);
            snprintf(expected_hex + 2 * i, 3, "%02x", expected_hash[i]);
        }
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "header chain reaches height=%d but its tip hash disagrees "
                 "derived=%s expected=%s",
                 height, derived_hex, expected_hex);
    } else
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "header chain is incomplete or discontinuous rows_read=%lld "
                 "required=%lld fail_height=%lld",
                 (long long)expected_height, (long long)height + 1,
                 (long long)fail_height);
    consensus_export_progress_emit(
        "prove_header_chain done rows=%lld ok=%d elapsed=%lldms",
        (long long)expected_height, ok ? 1 : 0,
        (long long)(consensus_export_clock_ms() - t0));
    return ok;
}

bool consensus_export_prove_stage_rows(
    sqlite3 *db, const struct export_stage_proof *stage, int32_t height,
    uint64_t cursor, uint8_t validation_profile,
    const uint8_t source_epoch_digest[32],
    struct consensus_state_bundle_proof_summary *summary)
{
    int64_t t0 = consensus_export_clock_ms();
    consensus_export_progress_emit(
        "prove_stage_rows(%s) start height=%d rows=%lld cursor=%llu",
        stage->name, height, (long long)height + 1,
        (unsigned long long)cursor);
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
    int64_t fail_height = -1;
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
            fail_height = row_height;
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
        if (expected_height % 500000 == 0)
            consensus_export_progress_emit(
                "prove_stage_rows(%s): %lld/%lld rows, %llds elapsed",
                stage->name, (long long)expected_height,
                (long long)height + 1,
                (long long)((consensus_export_clock_ms() - t0) / 1000));
    }
    if (rc != SQLITE_DONE || expected_height != (int64_t)height + 1)
        ok = false;
    sqlite3_finalize(st);
    if (!ok) {
        LOG_WARN(EXPORT_PROOF_SUBSYS,
                 "stage proof is not a complete profile-bound ok=1 prefix "
                 "table=%s rows_read=%lld required=%lld fail_height=%lld "
                 "profile=%u",
                 stage->table, (long long)expected_height,
                 (long long)height + 1, (long long)fail_height,
                 (unsigned)validation_profile);
        consensus_export_progress_emit(
            "prove_stage_rows(%s) done rows=%lld ok=0 elapsed=%lldms",
            stage->name, (long long)expected_height,
            (long long)(consensus_export_clock_ms() - t0));
        return false;
    }

    if (!stage->hash_column) {
        proof_u64(&component, 0);
        sha3_256_finalize(&component, summary->component_digest);
        consensus_export_progress_emit(
            "prove_stage_rows(%s) done rows=%lld ok=1 elapsed=%lldms",
            stage->name, (long long)expected_height,
            (long long)(consensus_export_clock_ms() - t0));
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
                 "stage rows are not bound to header hashes table=%s "
                 "hash_bound_count=%lld required=%lld",
                 stage->table, (long long)hash_bound_count,
                 (long long)height + 1);
    if (ok) {
        summary->hash_bound_count = (uint64_t)hash_bound_count;
        proof_u64(&component, summary->hash_bound_count);
        sha3_256_finalize(&component, summary->component_digest);
    }
    consensus_export_progress_emit(
        "prove_stage_rows(%s) done rows=%lld ok=%d elapsed=%lldms",
        stage->name, (long long)expected_height, ok ? 1 : 0,
        (long long)(consensus_export_clock_ms() - t0));
    return ok;
}
