/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transaction mechanics for retained reducer-frontier script/proof replay.
 *
 * Keep repair routing and classification in reducer_frontier_replay.c; keep
 * the mutation sequence here so future agents can audit cursor/log/coin
 * rewinds without reading the detector ladder first. */

#include "reducer_frontier_replay_tx.h"
#include "stage_repair_reducer_frontier_internal.h"

#include "jobs/created_outputs_index.h"
#include "jobs/mint_skip_crypto.h"
#include "jobs/script_validate_stage.h"
#include "jobs/stage_repair_internal.h"
#include "primitives/block.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "utxo_apply_delta_internal.h"

#include "core/uint256.h"
#include "util/log_macros.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>

static bool rf_replay_delta_exists(sqlite3 *db, int height, bool *exists)
{
    *exists = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM utxo_apply_delta WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] delta presence prepare failed h=%d: %s",
                 height, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW) {
        *exists = true;
        return true;
    }
    if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] delta presence step failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool rf_replay_rewindable_utxo_row(sqlite3 *db, int height)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM utxo_apply_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] utxo row prepare failed h=%d: %s",
                 height, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc != SQLITE_ROW) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair refused: missing "
                 "utxo_apply_log row h=%d",
                 height);
        sqlite3_finalize(st);
        return false;
    }
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER ||
        (sqlite3_column_int(st, 0) != 0 && sqlite3_column_int(st, 0) != 1)) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair refused: malformed "
                 "utxo ok storage h=%d", height);
        sqlite3_finalize(st);
        return false;
    }
    bool ok_row = sqlite3_column_int(st, 0) == 1;
    sqlite3_finalize(st);

    bool have_delta = false;
    if (!rf_replay_delta_exists(db, height, &have_delta))
        LOG_RETURN(false, "stage_repair",
                   "[stage_repair] stale replay inverse check failed: "
                   "delta presence h=%d", height);
    if (ok_row && !have_delta) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair refused: ok=1 "
                 "utxo row has no inverse delta h=%d",
                 height);
        return false;
    }
    if (!ok_row && have_delta) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair refused: failed "
                 "utxo row unexpectedly has delta h=%d",
                 height);
        return false;
    }
    return true;
}

static bool rf_replay_inverse_checked(sqlite3 *db, int first_h, int cursor)
{
    for (int h = cursor - 1; h >= first_h; h--) {
        if (!rf_replay_rewindable_utxo_row(db, h))
            LOG_RETURN(false, "stage_repair",
                       "[stage_repair] stale replay inverse check failed: "
                       "utxo row h=%d", h);
        if (!utxo_apply_emit_inverse_delta(db, h))
            LOG_RETURN(false, "stage_repair",
                       "[stage_repair] stale replay inverse emit failed h=%d",
                       h);
    }
    return true;
}

static bool rf_replay_created_outputs_height_indexed(sqlite3 *db, int height,
                                                     bool *indexed)
{
    *indexed = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM created_outputs WHERE height = ? LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] created_outputs height prepare failed "
                 "h=%d: %s",
                 height, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW) {
        *indexed = true;
        return true;
    }
    if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] created_outputs height step failed h=%d "
                 "rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool rf_replay_backfill_created_outputs_range(sqlite3 *db,
                                                     struct main_state *ms,
                                                     int first_h,
                                                     int last_h)
{
    if (!created_outputs_index_ensure_schema(db))
        LOG_RETURN(false, "stage_repair",
                   "[stage_repair] created_outputs schema ensure failed "
                   "for replay backfill %d..%d", first_h, last_h);
    if (first_h > last_h)
        return true;

    for (int h = first_h; h <= last_h; h++) {
        bool indexed = false;
        if (!rf_replay_created_outputs_height_indexed(db, h, &indexed))
            LOG_RETURN(false, "stage_repair",
                       "[stage_repair] created_outputs index probe failed "
                       "h=%d", h);
        if (indexed)
            continue;

        struct block blk;
        struct uint256 hash;
        block_init(&blk);
        bool ok = stage_repair_read_active_block_checked(ms, h, &blk, &hash);
        if (ok)
            ok = created_outputs_index_put_block(db, &blk, h);
        block_free(&blk);
        if (!ok) {
            LOG_WARN("stage_repair",
                     "[stage_repair] created_outputs backfill failed h=%d",
                     h);
            return false;
        }
    }
    return true;
}

bool reducer_frontier_replay_delete_log_range(sqlite3 *db, const char *table,
                                              int first_h, int cursor)
{
    if (cursor <= first_h)
        return true;

    char sql[160];
    int n = snprintf(sql, sizeof(sql),
                     "DELETE FROM %s WHERE height >= ? AND height < ?",
                     table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("stage_repair", "delete_log_range sql overflow table=%s",
                 table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] delete %s prepare failed: %s",
                 table, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, first_h);
    sqlite3_bind_int(st, 2, cursor);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] delete %s rc=%d: %s",
                 table, rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

/* Read script_validate_log.ok at `height`: 1 (passed), 0 (failed), -1 (no
 * row). The proof-internal_error rewind only fires when script PASSED at the
 * hole (ok==1): a script hole at the same height is the script path's domain
 * (it deletes proof_validate_log down to its replay_first as part of the same
 * transaction), so the proof path must not double-own it. Caller holds the
 * progress_store tx lock. */
bool reducer_frontier_replay_script_ok_at_unlocked(sqlite3 *db, int height,
                                                   int *out_ok)
{
    *out_ok = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok, status FROM script_validate_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] script_ok prepare failed h=%d: %s",
                 height, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        int ok = sqlite3_column_type(st, 0) == SQLITE_INTEGER
            ? sqlite3_column_int(st, 0) : -1;
        int status_type = sqlite3_column_type(st, 1);
        const void *status = status_type == SQLITE_TEXT
            ? sqlite3_column_text(st, 1) : NULL;
        if (ok == 1)
            *out_ok = status &&
                mint_validation_evidence_parse(
                    status, (size_t)sqlite3_column_bytes(st, 1)) ==
                    MINT_VALIDATION_EVIDENCE_VERIFIED ? 1 : -1;
        else if (ok == 0)
            *out_ok = ok;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] script_ok step failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

bool reducer_frontier_replay_dry_run_stale_script(
    sqlite3 *db,
    struct main_state *ms,
    int height,
    int replay_first,
    int utxo_cursor,
    int backfill_top,
    const struct block *blk,
    struct script_validate_dry_run_report *dry)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script dry-run BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    /* STEP 1 (keystone): the dry-run MUST go through the SAME public dry-run the
     * real stage uses (script_validate_stage_dry_run_block -> the NULL/default
     * script_validate_created_index_prevout resolver) so dry.ok==true PROVABLY
     * implies the real fold writes ok=1 for the same (height, active body). We
     * set up the rolled-back txn so the default resolver sees the SAME state
     * the real fold will:
     *   - created_outputs backfilled over [replay_first, backfill_top] (covers
     *     coins created in the replay span);
     *   - coins_kv rewound (inverse deltas) AND the applied_height marker rolled
     *     back to replay_first, so the resolver's view frontier matches the
     *     rewound coin set (pre-replay coins resolve via coins_kv, in-span
     *     coins via the created_outputs index — exactly the real fold's two
     *     layers). Everything is undone by the ROLLBACK below. */
    bool rewind_coins = utxo_cursor > replay_first;
    bool ok = rf_replay_backfill_created_outputs_range(db, ms, replay_first,
                                                       backfill_top) &&
              (!rewind_coins ||
               rf_replay_inverse_checked(db, replay_first, utxo_cursor)) &&
              (!rewind_coins ||
               coins_kv_set_applied_height_in_tx(db, replay_first)) &&
              script_validate_stage_dry_run_block(blk, height, dry);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

bool reducer_frontier_replay_stale_script_tx(
    sqlite3 *db,
    struct main_state *ms,
    int height,
    int replay_first,
    int script_cursor,
    int proof_cursor,
    int utxo_cursor,
    int tip_cursor,
    int backfill_top,
    bool rewind_headers)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    /* STEP 3: NO write-once marker. The rewind deletes the stale ok=0 row(s) and
     * rewinds the cursor(s), so the ok=0 detector stops matching next tick; STEP
     * 1 (dry==real) guarantees the forward re-fold writes ok=1, so the hole
     * cannot re-form — termination is by the body-vs-row delta, not a guard.
     * For the hash-split class (rewind_headers) we ALSO drop validate_headers'
     * rows over [replay_first, script_cursor) and force its cursor back, so BOTH
     * validate_headers and script_validate re-derive their hash from the SAME
     * on-disk canonical body -> v.hash==s.block_hash by construction, the split
     * cannot persist. */
    bool rewind_coins = utxo_cursor > replay_first;
    if (!rf_replay_backfill_created_outputs_range(db, ms, replay_first,
                                                  backfill_top) ||
        (rewind_coins &&
         !rf_replay_inverse_checked(db, replay_first, utxo_cursor)) ||
        !reducer_frontier_replay_delete_log_range(
            db, "script_validate_log", replay_first, script_cursor) ||
        !reducer_frontier_replay_delete_log_range(
            db, "proof_validate_log", replay_first, proof_cursor) ||
        (rewind_headers &&
         !reducer_frontier_replay_delete_log_range(
             db, "validate_headers_log", replay_first, script_cursor)) ||
        (rewind_coins &&
         !utxo_apply_delete_rows_above(db, replay_first, utxo_cursor - 1)) ||
        (rewind_headers &&
         !stage_repair_force_stage_cursor(db, "validate_headers",
                                          replay_first)) ||
        !stage_repair_force_stage_cursor(db, "script_validate", replay_first) ||
        !stage_repair_force_stage_cursor(db, "proof_validate", replay_first) ||
        !stage_repair_force_stage_cursor(db, "tip_finalize", replay_first) ||
        (rewind_coins &&
         !utxo_apply_unwind_write_cursor(db, (uint64_t)replay_first)) ||
        (rewind_coins &&
         !coins_kv_set_applied_height_in_tx(db, replay_first))) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair COMMIT failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    (void)tip_cursor;
    return true;
}

bool reducer_frontier_replay_stale_proof_tx(sqlite3 *db,
                                            int height,
                                            int replay_first,
                                            int proof_cursor,
                                            int utxo_cursor,
                                            const char *marker)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale proof repair BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    bool rewind_coins = utxo_cursor > replay_first;
    if ((rewind_coins &&
         !rf_replay_inverse_checked(db, replay_first, utxo_cursor)) ||
        !reducer_frontier_replay_delete_log_range(
            db, "proof_validate_log", replay_first, proof_cursor) ||
        (rewind_coins &&
         !utxo_apply_delete_rows_above(db, replay_first, utxo_cursor - 1)) ||
        !stage_repair_force_stage_cursor(db, "proof_validate", replay_first) ||
        !stage_repair_force_stage_cursor(db, "tip_finalize", replay_first) ||
        (rewind_coins &&
         !utxo_apply_unwind_write_cursor(db, (uint64_t)replay_first)) ||
        (rewind_coins &&
         !coins_kv_set_applied_height_in_tx(db, replay_first)) ||
        !stage_reducer_frontier_repair_marker_record_in_tx(
            db, marker, "stale proof")) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale proof repair COMMIT failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    return true;
}
