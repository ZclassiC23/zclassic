/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * consensus_state_snapshot_install_activate.c — ACTIVATE mode for
 * zcl.consensus_state_bundle.v1: the consumer side of the sovereign shielded-
 * state cure. Kept in its own file (one focused responsibility) from the
 * contained admission preview in consensus_state_snapshot_install.c.
 *
 * Atomically installs a complete bundle's coins + Sprout/Sapling anchors +
 * nullifiers + the 8 reducer stage cursors into the LIVE progress store, with a
 * physically restorable prior generation. Full contract:
 * config/consensus_state_snapshot_install.h. */

#include "config/consensus_state_snapshot_install.h"

#include "config/consensus_state_replay_receipt.h" /* independent-replay authority */
#include "consensus_state_snapshot_install_internal.h" /* candidate_lease_begin/end */
#include "jobs/reducer_frontier.h"       /* reducer_frontier_compute_hstar,
                                          * REDUCER_TRUSTED_BASE_*_KEY */
#include "jobs/stage_repair_internal.h"  /* stage_repair_force_stage_cursor */
#include "jobs/tip_finalize_stage.h"     /* tip_finalize_stage_seed_anchor */
#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"      /* progress_store_tx_lock/unlock,
                                          * progress_meta_set/delete_in_tx */
#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACTIVATE_SUBSYS "consensus_bundle_activate"

/* Reducer-derived tables cleared on cutover — the exact list
 * boot_refold_from_anchor_reset uses (a "no such table" is tolerated). */
static const char *const k_activate_derived_tables[] = {
    "validate_headers_log", "body_fetch_log", "body_persist_log",
    "script_validate_log", "proof_validate_log", "utxo_apply_log",
    "tip_finalize_log", "utxo_apply_delta", "created_outputs",
};
static const char *const k_activate_stages[] = {
    "header_admit", "validate_headers", "body_fetch", "body_persist",
    "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
};

#ifdef ZCL_TESTING
static bool g_activate_force_independent_authority = false;
void consensus_state_activate_test_force_independent_authority(bool granted)
{
    g_activate_force_independent_authority = granted;
}
#endif

/* The install is CONTAINED unless an independent replay-derived receipt for
 * EXACTLY this bundle + anchor + component digests exists in the datadir. The
 * bundle can recompute its own digests (self-asserted provenance), and a
 * ZClassic header commits none of the UTXO/anchor/nullifier CONTENTS, so
 * matching height/hash to a local header is not content authentication. The
 * receipt is the only thing that authorizes touching progress.kv in production;
 * the ZCL_TESTING hook lets fixtures exercise the install mechanics. */
static bool activate_independent_authority_available(
    const struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_artifact_evidence *evidence,
    const char *datadir)
{
#ifdef ZCL_TESTING
    if (g_activate_force_independent_authority)
        return true;
#endif
    uint8_t bundle_file_digest[32];
    if (!consensus_state_artifact_evidence_file_digest(evidence,
                                                       bundle_file_digest))
        return false;
    return consensus_state_replay_receipt_authority_available(
        manifest, bundle_file_digest, datadir);
}

static bool activate_fail(struct consensus_state_activate_result *result,
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
        result->activated = false;
        snprintf(result->reason, sizeof(result->reason), "%s", reason);
    }
    LOG_WARN(ACTIVATE_SUBSYS, "%s", reason);
    return false;
}

/* Bind one source column into the destination statement, preserving type.
 * Local copy of the candidate builder's helper (different translation unit). */
static bool activate_bind_column(sqlite3_stmt *dst, int dst_col,
                                 sqlite3_stmt *src, int src_col)
{
    switch (sqlite3_column_type(src, src_col)) {
    case SQLITE_INTEGER:
        return sqlite3_bind_int64(dst, dst_col,
                                  sqlite3_column_int64(src, src_col)) ==
               SQLITE_OK;
    case SQLITE_TEXT:
        return sqlite3_bind_text(dst, dst_col,
                                 (const char *)sqlite3_column_text(src, src_col),
                                 sqlite3_column_bytes(src, src_col),
                                 SQLITE_TRANSIENT) == SQLITE_OK;
    case SQLITE_BLOB:
        return sqlite3_bind_blob(dst, dst_col,
                                 sqlite3_column_blob(src, src_col),
                                 sqlite3_column_bytes(src, src_col),
                                 SQLITE_TRANSIENT) == SQLITE_OK;
    case SQLITE_NULL:
        return sqlite3_bind_null(dst, dst_col) == SQLITE_OK;
    default:
        return false;
    }
}

/* Stream every row selected from the immutable bundle into the live progress
 * store. Runs inside the caller's open BEGIN IMMEDIATE, so a failure rolls back
 * with the whole install. Emitted SQL uses named columns so the on-disk column
 * order of either store is irrelevant. */
static bool activate_stream_copy(sqlite3 *src, sqlite3 *dst,
                                 const char *select_sql, const char *insert_sql,
                                 int columns, uint64_t *rows_out)
{
    sqlite3_stmt *read = NULL;
    sqlite3_stmt *write = NULL;
    bool ok = sqlite3_prepare_v2(src, select_sql, -1, &read, NULL) == SQLITE_OK &&
              sqlite3_prepare_v2(dst, insert_sql, -1, &write, NULL) == SQLITE_OK;
    uint64_t rows = 0;
    int rc = SQLITE_ERROR;
    while (ok && (rc = sqlite3_step(read)) == SQLITE_ROW) { // raw-sql-ok:read-only-introspection
        ok = sqlite3_reset(write) == SQLITE_OK &&
             sqlite3_clear_bindings(write) == SQLITE_OK;
        for (int i = 0; ok && i < columns; i++)
            ok = activate_bind_column(write, i + 1, read, i);
        if (ok)
            ok = sqlite3_step(write) == SQLITE_DONE; // raw-sql-ok:progress-kv-kernel-store
        if (ok && rows == UINT64_MAX)
            ok = false;
        if (ok)
            rows++;
    }
    if (ok)
        ok = rc == SQLITE_DONE;
    if (!ok)
        LOG_WARN(ACTIVATE_SUBSYS, "row stream failed: src=%s dst=%s",
                 sqlite3_errmsg(src), sqlite3_errmsg(dst));
    if (read)
        sqlite3_finalize(read);
    if (write)
        sqlite3_finalize(write);
    if (rows_out)
        *rows_out = rows;
    return ok;
}

/* Capture a physically restorable prior generation of the progress store via
 * SQLite VACUUM INTO (a clean standalone copy of the current committed state).
 * Must run with no open transaction on `progress_db`. Writes the chosen path to
 * out_path. Fails closed. */
static bool activate_backup_prior_generation(sqlite3 *progress_db,
                                             const char *datadir,
                                             char *out_path, size_t out_cap)
{
    if (!progress_db || !datadir || !datadir[0] || !out_path || out_cap == 0)
        return false;
    out_path[0] = '\0';
    int64_t stamp = (int64_t)platform_time_wall_time_t();
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path),
                     "%s/progress.kv.preinstall.%lld.%ld", datadir,
                     (long long)stamp, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(path) || (size_t)n >= out_cap) {
        LOG_WARN(ACTIVATE_SUBSYS, "prior-generation backup path too long");
        return false;
    }
    /* A stale collision must never be silently overwritten. */
    struct stat st;
    if (lstat(path, &st) == 0 || errno != ENOENT) {
        LOG_WARN(ACTIVATE_SUBSYS,
                 "prior-generation backup target already present: %s", path);
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(progress_db, "VACUUM main INTO ?1", -1, &stmt,
                                 NULL) == SQLITE_OK &&
              sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_DONE; // raw-sql-ok:progress-kv-kernel-store
    if (!ok)
        LOG_WARN(ACTIVATE_SUBSYS, "prior-generation VACUUM INTO failed: %s",
                 sqlite3_errmsg(progress_db));
    if (stmt)
        sqlite3_finalize(stmt);
    if (ok)
        snprintf(out_path, out_cap, "%s", path);
    else
        (void)unlink(path);
    return ok;
}

/* The single atomic install transaction. Assumes the progress-store tx lock is
 * held and BEGIN IMMEDIATE is open on progress_db. Returns false (with the
 * caller rolling back) on any failure. Never partially applies. */
static bool activate_apply_in_tx(
    sqlite3 *progress_db, sqlite3 *bundle_db,
    const struct consensus_state_bundle_manifest *m,
    struct consensus_state_activate_result *result)
{
    char *err = NULL;

    /* 1. Clear the reducer-derived logs/deltas (tolerate absent tables). */
    for (size_t i = 0;
         i < sizeof(k_activate_derived_tables) /
                 sizeof(k_activate_derived_tables[0]); i++) {
        char dsql[96];
        snprintf(dsql, sizeof(dsql), "DELETE FROM %s",
                 k_activate_derived_tables[i]);
        if (sqlite3_exec(progress_db, dsql, NULL, NULL, &err) != SQLITE_OK) {
            bool absent = err && strstr(err, "no such table");
            if (err) { sqlite3_free(err); err = NULL; }
            if (!absent)
                return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                                     "clearing derived table %s failed",
                                     k_activate_derived_tables[i]);
        }
    }

    /* 2. Replace the coin set (raw DELETE inside our txn — coins_kv_reset_for_
     *    reseed owns its own BEGIN, so it cannot join our transaction). */
    if (sqlite3_exec(progress_db, "DELETE FROM coins", NULL, NULL, &err)
        != SQLITE_OK) {
        if (err) { sqlite3_free(err); err = NULL; }
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "clearing coins failed");
    }

    /* 3. Reset both anchor tables + nullifier set to a COMPLETE (activation
     *    cursor 0) history, THEN install the actual rows. This is the exact
     *    difference from the wedge-causing refold: complete history, not an
     *    empty table with a positive cursor. The reset primitives join our
     *    open transaction. */
    if (!anchor_kv_reset_in_tx(progress_db, 0) ||
        !nullifier_kv_reset_in_tx(progress_db, 0))
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "shielded history reset to complete failed");

    /* 4. Stream coins + anchors (by pool) + nullifiers from the bundle. */
    uint64_t coins = 0, sprout = 0, sapling = 0, nfs = 0;
    if (!activate_stream_copy(bundle_db, progress_db,
            "SELECT txid,vout,value,height,is_coinbase,script "
            "FROM coins ORDER BY txid,vout",
            "INSERT INTO coins(txid,vout,value,height,is_coinbase,script) "
            "VALUES(?,?,?,?,?,?)", 6, &coins) ||
        coins != m->utxo_count)
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "coin stream count mismatch (%llu want %llu)",
                             (unsigned long long)coins,
                             (unsigned long long)m->utxo_count);
    if (!activate_stream_copy(bundle_db, progress_db,
            "SELECT anchor,height,tree FROM anchors WHERE pool=0 ORDER BY anchor",
            "INSERT INTO sprout_anchors(anchor,height,tree) VALUES(?,?,?)",
            3, &sprout) ||
        !activate_stream_copy(bundle_db, progress_db,
            "SELECT anchor,height,tree FROM anchors WHERE pool=1 ORDER BY anchor",
            "INSERT INTO sapling_anchors(anchor,height,tree) VALUES(?,?,?)",
            3, &sapling) ||
        sprout > UINT64_MAX - sapling ||
        sprout + sapling != m->anchor_count)
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "anchor stream count mismatch (%llu want %llu)",
                             (unsigned long long)(sprout + sapling),
                             (unsigned long long)m->anchor_count);
    if (!activate_stream_copy(bundle_db, progress_db,
            "SELECT nf,pool,height FROM nullifiers ORDER BY pool,nf",
            "INSERT INTO nullifiers(nf,pool,height) VALUES(?,?,?)", 3, &nfs) ||
        nfs != m->nullifier_count)
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "nullifier stream count mismatch (%llu want %llu)",
                             (unsigned long long)nfs,
                             (unsigned long long)m->nullifier_count);

    /* 5. Force the 8 stage cursors to the canonical anchor frontier: the seven
     *    upstream stages "processed through the anchor" (next-height cursor =
     *    height+1) and tip_finalize's next-to-finalize = height. This is the
     *    exact layout the validated candidate generation uses and the one the
     *    tip_finalize anchor seed produces, so the fold resumes AT height+1
     *    against on-disk bodies. */
    for (size_t i = 0;
         i < sizeof(k_activate_stages) / sizeof(k_activate_stages[0]); i++) {
        int cursor = (i == sizeof(k_activate_stages) /
                              sizeof(k_activate_stages[0]) - 1)
                         ? m->height        /* tip_finalize */
                         : m->height + 1;   /* upstream stages */
        if (!stage_repair_force_stage_cursor(progress_db, k_activate_stages[i],
                                             cursor))
            return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                                 "forcing stage cursor %s failed",
                                 k_activate_stages[i]);
    }
    if (!coins_kv_set_applied_height_in_tx(progress_db, m->height + 1))
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "setting coins_applied_height failed");

    /* 6. Provenance: the coin set is a checkpoint-bound self-derived install,
     *    and it provably holds the live set. Drop any stale trusted-base
     *    declaration (the refold discipline). */
    uint8_t one = 1;
    if (!progress_meta_set_in_tx(progress_db, COINS_KV_MIGRATION_COMPLETE_KEY,
                                 &one, 1) ||
        !progress_meta_set_in_tx(progress_db, COINS_KV_SELF_FOLDED_KEY, &one, 1))
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "setting coins provenance markers failed");
    (void)progress_meta_delete_in_tx(progress_db,
                                     REDUCER_TRUSTED_BASE_HEIGHT_KEY);
    (void)progress_meta_delete_in_tx(progress_db,
                                     REDUCER_TRUSTED_BASE_HASH_KEY);

    /* 7. Pre-COMMIT verification: the installed coin set must reproduce the
     *    manifest's UTXO root + count EXACTLY (reads see our uncommitted rows).
     *    A mismatch rolls the whole install back cleanly — nothing commits. */
    uint8_t got_root[32] = {0};
    int64_t got_count = coins_kv_count(progress_db);
    if (coins_kv_commitment(progress_db, got_root) != 0 ||
        got_count != (int64_t)m->utxo_count ||
        memcmp(got_root, m->utxo_root, 32) != 0)
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "installed coin set failed the UTXO root/count "
                             "recheck (count=%lld want=%llu)",
                             (long long)got_count,
                             (unsigned long long)m->utxo_count);

    result->utxo_count = coins;
    result->anchor_count = sprout + sapling;
    result->nullifier_count = nfs;
    return true;
}

bool consensus_state_snapshot_install_activate(
    sqlite3 *progress_db,
    const struct consensus_state_activate_request *request,
    struct consensus_state_activate_result *result)
{
    if (result)
        memset(result, 0, sizeof(*result));
    if (!progress_db || !request || !request->bundle_path || !request->datadir)
        return activate_fail(result, CONSENSUS_INSTALL_REFUSED,
                             "NULL progress_db/request/bundle_path/datadir");

    /* 1. Admit + validate the immutable bundle (recomputes UTXO root/count/
     *    supply, verifies every anchor tree->root, and the nullifier digest). */
    struct consensus_state_artifact_evidence *evidence = NULL;
    struct zcl_result admitted =
        consensus_state_artifact_evidence_open(request->bundle_path, &evidence);
    if (!admitted.ok)
        return activate_fail(result, CONSENSUS_INSTALL_REFUSED,
                             "bundle admission failed: %s", admitted.message);

    struct consensus_state_bundle_manifest manifest;
    if (!consensus_state_artifact_evidence_manifest_copy(evidence, &manifest)) {
        consensus_state_artifact_evidence_free(evidence);
        return activate_fail(result, CONSENSUS_INSTALL_REFUSED,
                             "artifact evidence became stale after admission");
    }

    /* 2. Caller height/hash assertion (catches selecting the wrong artifact). */
    if (request->expected_height != manifest.height ||
        memcmp(request->expected_block_hash, manifest.block_hash, 32) != 0) {
        consensus_state_artifact_evidence_free(evidence);
        return activate_fail(result, CONSENSUS_INSTALL_REFUSED,
                             "bundle height/hash does not match caller "
                             "assertion");
    }

    /* 3. ACTIVATE requires a COMPLETE genesis-derived history — a current-only
     *    (positive activation boundary) bundle would reinstate the very gap this
     *    cure closes. Mixed provenance is forbidden. */
    if (!manifest.history_complete || manifest.activation_boundary != 0 ||
        manifest.sprout_source_cursor != 0 ||
        manifest.sapling_source_cursor != 0 ||
        manifest.nullifier_source_cursor != 0 ||
        manifest.source_fold_cursor != (int64_t)manifest.height + 1) {
        consensus_state_artifact_evidence_free(evidence);
        return activate_fail(result, CONSENSUS_INSTALL_REFUSED,
                             "activation requires a complete genesis-derived "
                             "history bundle (no mixed provenance)");
    }

    result->height = manifest.height;

    /* 3b. Independent-replay authority gate. Without a valid replay receipt the
     *     install stays CONTAINED (VERIFIED_CONTAINED) and touches nothing —
     *     the bundle's self-asserted digests do not authenticate its contents. */
    if (!activate_independent_authority_available(&manifest, evidence,
                                                  request->datadir)) {
        consensus_state_artifact_evidence_free(evidence);
        return activate_fail(result, CONSENSUS_INSTALL_VERIFIED_CONTAINED,
                             "no independent replay-derived receipt authorizes "
                             "this bundle; run -verify-consensus-bundle against "
                             "a datadir folded to the anchor first (install "
                             "stays contained)");
    }

    /* 4. Capture the physically restorable prior generation (no open txn). */
    if (!activate_backup_prior_generation(progress_db, request->datadir,
                                          result->prior_generation_path,
                                          sizeof(result->prior_generation_path))) {
        consensus_state_artifact_evidence_free(evidence);
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "prior-generation backup failed; refusing to "
                             "install without a rollback point");
    }

    /* 5. Ensure the live shielded/coin schemas exist (idempotent). */
    if (!coins_kv_ensure_schema(progress_db) ||
        !anchor_kv_ensure_schema(progress_db) ||
        !nullifier_kv_ensure_schema(progress_db)) {
        consensus_state_artifact_evidence_free(evidence);
        return activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                             "ensuring live coin/shielded schema failed");
    }

    /* 6. Lease the immutable read transaction (revalidates the pinned file),
     *    then run the one atomic install transaction under the progress-store
     *    tx lock (stage_repair_force_stage_cursor requires both). */
    struct consensus_state_bundle_manifest leased_manifest;
    uint8_t receipt_digest[32];
    sqlite3 *bundle_db = NULL;
    if (!consensus_state_artifact_evidence_candidate_lease_begin(
            evidence, &leased_manifest, receipt_digest, &bundle_db)) {
        consensus_state_artifact_evidence_free(evidence);
        return activate_fail(result, CONSENSUS_INSTALL_REFUSED,
                             "artifact evidence lease refused (stale)");
    }

    bool ok = true;
    char *err = NULL;
    progress_store_tx_lock();
    if (sqlite3_exec(progress_db, "BEGIN IMMEDIATE", NULL, NULL, &err)
        != SQLITE_OK) {
        if (err) { sqlite3_free(err); err = NULL; }
        ok = activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                           "install transaction begin failed");
    }
    if (ok)
        ok = activate_apply_in_tx(progress_db, bundle_db, &leased_manifest,
                                  result);
    if (ok && sqlite3_exec(progress_db, "COMMIT", NULL, NULL, &err)
        != SQLITE_OK) {
        if (err) { sqlite3_free(err); err = NULL; }
        ok = activate_fail(result, CONSENSUS_INSTALL_STORE_ERROR,
                           "install transaction commit failed");
    }
    if (!ok)
        sqlite3_exec(progress_db, "ROLLBACK", NULL, NULL, NULL);
    progress_store_tx_unlock();

    consensus_state_artifact_evidence_candidate_lease_end(evidence);
    consensus_state_artifact_evidence_free(evidence);

    if (!ok) {
        /* The transaction rolled back — the live store is byte-for-state
         * identical to before, and result->prior_generation_path still names
         * the standalone backup for an operator-level rollback. */
        return false; // raw-return-ok:logged-by-activate_fail
    }

    /* 7. Seed the tip_finalize served-tip prefix AT the anchor (assisted seed —
     *    the same trust model boot_refold_from_anchor_reset uses). Non-fatal:
     *    the runtime authority re-seeds from the coins tip if this no-ops. */
    if (!tip_finalize_stage_seed_anchor(manifest.height, manifest.block_hash,
                                        true))
        LOG_WARN(ACTIVATE_SUBSYS,
                 "tip_finalize anchor seed returned false (stage not wired?) — "
                 "runtime re-seeds from the coins tip");

    /* 8. Report the durable cursors for the terminal. */
    int32_t hstar = -1, served = -1;
    progress_store_tx_lock();
    if (reducer_frontier_compute_hstar(progress_db, &hstar, &served))
        result->hstar = hstar;
    else
        result->hstar = -1;
    progress_store_tx_unlock();
    int32_t applied = -1;
    bool applied_found = false;
    if (coins_kv_get_applied_height(progress_db, &applied, &applied_found) &&
        applied_found)
        result->coins_applied_height = applied;
    else
        result->coins_applied_height = -1;

    result->status = CONSENSUS_INSTALL_ACTIVATED;
    result->activated = true;
    snprintf(result->reason, sizeof(result->reason),
             "activated %s height=%d coins=%llu anchors=%llu nullifiers=%llu "
             "H*=%d coins_applied=%d; prior generation preserved at %s",
             CONSENSUS_STATE_BUNDLE_SCHEMA, manifest.height,
             (unsigned long long)result->utxo_count,
             (unsigned long long)result->anchor_count,
             (unsigned long long)result->nullifier_count, result->hstar,
             result->coins_applied_height, result->prior_generation_path);
    LOG_INFO(ACTIVATE_SUBSYS, "%s", result->reason);
    return true;
}
