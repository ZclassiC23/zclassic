/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_delta_repair — one-shot stale verdict repairs.
 *
 * These repairs reuse the inverse-delta machinery from
 * utxo_apply_delta_reorg.c, but they are not fork reorg handling. Each repair
 * is consensus-critical and gated by current-binary dry-runs plus a
 * (height,block_hash) one-shot marker. */

#include "jobs/utxo_apply_delta.h"
#include "utxo_apply_delta_internal.h"

#include "jobs/stage_helpers.h"
#include "jobs/utxo_apply_stage.h"
#include "coins/coins.h"
#include "primitives/block.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "storage/repair_marker.h"
#include "storage/utxo_projection.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VALUE_OVERFLOW_REPAIR_ACK_ENV \
    "ZCL_REDUCER_VALUE_OVERFLOW_REPAIR_ACK"

static bool owner_ack_value_overflow_repair(void)
{
    const char *v = getenv(VALUE_OVERFLOW_REPAIR_ACK_ENV);
    return v && strcmp(v, "1") == 0;
}

static int repair_row_still_present(sqlite3 *db, int height,
                                    const char *want_status)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok, status FROM utxo_apply_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair row prepare failed: %s",
                 sqlite3_errmsg(db));
        return -1;  // raw-return-ok:tri-state-error-logged
    }
    sqlite3_bind_int(st, 1, height);

    bool ok = false;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const unsigned char *status = sqlite3_column_text(st, 1);
        ok = sqlite3_column_int(st, 0) == 0 &&
             status && strcmp((const char *)status, want_status) == 0;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair row step failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;  // raw-return-ok:tri-state-error-logged
    }
    sqlite3_finalize(st);
    return ok ? 1 : 0;
}

/* Inverse-walk precondition over [first_h, last_h]: every height has a
 * utxo_apply_log row, and a delta row exists iff that row has ok=1 (forward
 * apply persists a delta only on a successful apply, atomically with its log
 * row). utxo_apply_emit_inverse_delta silently no-ops on a missing delta row,
 * so any violation means the rewind would be PARTIAL on a torn datadir —
 * coins keeping heights the cursor no longer covers. Returns 1 consistent,
 * 0 on a violation (counts logged), -1 on error (logged). Caller holds the
 * progress_store tx lock. */
static int inverse_walk_consistent(sqlite3 *db, int first_h, int last_h)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT (?2 - ?1 + 1) - "
            "       (SELECT COUNT(*) FROM utxo_apply_log "
            "         WHERE height BETWEEN ?1 AND ?2), "
            "       (SELECT COUNT(*) FROM utxo_apply_log l "
            "         WHERE l.height BETWEEN ?1 AND ?2 AND l.ok = 1 "
            "           AND NOT EXISTS (SELECT 1 FROM utxo_apply_delta d "
            "                            WHERE d.height = l.height)), "
            "       (SELECT COUNT(*) FROM utxo_apply_delta d "
            "         WHERE d.height BETWEEN ?1 AND ?2 "
            "           AND NOT EXISTS (SELECT 1 FROM utxo_apply_log l "
            "                            WHERE l.height = d.height "
            "                              AND l.ok = 1))",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] inverse walk guard prepare failed: %s",
                 sqlite3_errmsg(db));
        return -1;  // raw-return-ok:tri-state-error-logged
    }
    sqlite3_bind_int(st, 1, first_h);
    sqlite3_bind_int(st, 2, last_h);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc != SQLITE_ROW) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] inverse walk guard step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;  // raw-return-ok:tri-state-error-logged
    }
    sqlite3_int64 missing_log = sqlite3_column_int64(st, 0);
    sqlite3_int64 ok_no_delta = sqlite3_column_int64(st, 1);
    sqlite3_int64 delta_no_ok = sqlite3_column_int64(st, 2);
    sqlite3_finalize(st);
    if (missing_log == 0 && ok_no_delta == 0 && delta_no_ok == 0)
        return 1;
    LOG_WARN("utxo_apply",
             "[utxo_apply] inverse walk range [%d..%d] torn: missing_log=%lld "
             "ok_without_delta=%lld delta_without_ok=%lld",
             first_h, last_h, (long long)missing_log,
             (long long)ok_no_delta, (long long)delta_no_ok);
    return 0;
}

static bool repair_live_lookup(const struct uint256 *txid, uint32_t vout,
                               struct utxo_apply_lookup *out, void *user)
{
    sqlite3 *db = user;
    if (!txid || !out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!db)
        return true;

    int64_t value = 0;
    int32_t height = 0;
    bool is_coinbase = false;
    size_t slen = 0;
    if (!coins_kv_get_prevout(db, txid->data, vout, &value, out->script,
                              UTXO_APPLY_SCRIPT_MAX, &slen, &height,
                              &is_coinbase))
        return true;

    if (slen > UTXO_APPLY_SCRIPT_MAX)
        return false;

    out->found = true;
    out->value = value;
    out->height = (uint32_t)(height < 0 ? 0 : height);
    out->is_coinbase = is_coinbase;
    out->script_len = (uint32_t)slen;
    return true;
}

static bool dry_run_after_inverse(sqlite3 *db, int height, int cursor,
                                  const struct block *blk,
                                  struct delta_summary *dry)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] repair dry-run BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    for (int h = cursor - 1; h >= height; h--) {
        if (!utxo_apply_emit_inverse_delta(db, h)) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
    }

    utxo_apply_compute_block_delta(blk, (uint32_t)height,
                                   repair_live_lookup, db, dry);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return true;
}

bool utxo_apply_repair_value_overflow_hole(
    sqlite3 *db,
    int height,
    uint64_t cursor,
    const struct uint256 *block_hash,
    const struct block *blk,
    struct utxo_apply_value_overflow_repair_result *out)
{
    struct utxo_apply_value_overflow_repair_result local;
    memset(&local, 0, sizeof(local));
    local.height = height;
    local.cursor_before = cursor;
    local.cursor_after = cursor;
    if (out)
        *out = local;

    if (!db || !block_hash || !blk) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair refused: bad input "
                 "db=%p block_hash=%p blk=%p",
                 (void *)db, (const void *)block_hash, (const void *)blk);
        return false;
    }
    if (height < 0 || cursor == 0 || (uint64_t)height >= cursor) {
        if (out)
            *out = local;
        return true;
    }

    local.attempted = true;

    if (utxo_projection_get_author() != UTXO_AUTHOR_STAGE) {
        local.author_refused = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair refused h=%d: "
                 "utxo author is not stage",
                 height);
        if (out)
            *out = local;
        return true;
    }

    if (!owner_ack_value_overflow_repair()) {
        local.owner_refused = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair owner-gated h=%d: "
                 "set %s=1 only on an operator-approved datadir copy",
                 height, VALUE_OVERFLOW_REPAIR_ACK_ENV);
        if (out)
            *out = local;
        return true;
    }

    progress_store_tx_lock();

    /* TOCTOU guard (same family as the Wave-1 stale-script fix): the caller
     * snapshotted `cursor` under the progress lock, released it to read the
     * block body off disk, and re-entered here. If utxo_apply advanced in
     * that gap, an inverse walk keyed to the stale C-1 would rewind the
     * cursor past coins it never unwound. Every cursor writer holds this
     * (recursive) lock through COMMIT, so one re-read covers both walks. */
    uint64_t cursor_now = stage_cursor_persisted(db, "utxo_apply",
                                                 "utxo_apply");
    if (cursor_now != cursor) {
        local.cursor_stale_refused = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair refused h=%d: cursor "
                 "moved %llu -> %llu since snapshot (retry next tick)",
                 height, (unsigned long long)cursor,
                 (unsigned long long)cursor_now);
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }

    int C = (int)cursor;
    if ((uint64_t)C != cursor || C <= height) {
        progress_store_tx_unlock();
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair cursor invalid h=%d "
                 "cursor=%llu",
                 height, (unsigned long long)cursor);
        return false;
    }

    int row_present = repair_row_still_present(db, height, "value_overflow");
    if (row_present < 0) {
        progress_store_tx_unlock();
        return false;
    }
    if (row_present == 0) {
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }

    bool marker_seen = false;
    if (!repair_marker_have(db, REPAIR_MARKER_KIND_UTXO_VALUE_OVERFLOW,
                            height, block_hash->data, &marker_seen,
                            NULL, 0, NULL)) {
        progress_store_tx_unlock();
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair marker read failed h=%d",
                 height);
        return false;
    }
    if (marker_seen) {
        local.marker_seen = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair skipped h=%d: "
                 "one-shot marker already present",
                 height);
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }

    /* Both inverse walks below (the dry-run, then the committing one) span
     * [height .. C-1]; the lock held through COMMIT keeps this verdict valid
     * for both. */
    int walk_ok = inverse_walk_consistent(db, height, C - 1);
    if (walk_ok < 0) {
        progress_store_tx_unlock();
        return false;
    }
    if (walk_ok == 0) {
        local.walk_torn_refused = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair refused h=%d: torn "
                 "log/delta range below cursor %d",
                 height, C);
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }

    struct delta_summary dry;
    if (!dry_run_after_inverse(db, height, C, blk, &dry)) {
        progress_store_tx_unlock();
        return false;
    }
    local.dry_run_ok = dry.ok;
    if (!dry.ok) {
        local.genuinely_invalid = true;
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair: H genuinely invalid "
                 "height=%d status=%s kind=%s",
                 height, dry.status ? dry.status : "(null)",
                 dry.failure_kind ? dry.failure_kind : "(null)");
        free_delta(&dry);
        progress_store_tx_unlock();
        if (out)
            *out = local;
        return true;
    }
    free_delta(&dry);

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    for (int h = C - 1; h >= height; h--) {
        if (!utxo_apply_emit_inverse_delta(db, h)) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            progress_store_tx_unlock();
            return false;
        }
    }
    if (!utxo_apply_delete_rows_above(db, height, C - 1)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    if (!utxo_apply_unwind_write_cursor(db, (uint64_t)height)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    if (!utxo_apply_frontier_set_in_tx(db, height)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    static const uint8_t present = 1;  /* legacy progress_meta presence value */
    if (!repair_marker_note_in_tx(db, REPAIR_MARKER_KIND_UTXO_VALUE_OVERFLOW,
                                  height, block_hash->data, &present,
                                  sizeof(present))) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("utxo_apply",
                 "[utxo_apply] value_overflow repair COMMIT failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    progress_store_tx_unlock();

    local.repaired = true;
    local.cursor_after = (uint64_t)height;
    LOG_WARN("utxo_apply",
             "[utxo_apply] value_overflow repair rewound cursor %llu -> %d "
             "for stale hole h=%d",
             (unsigned long long)cursor, height, height);

    utxo_projection_t *proj = utxo_projection_get_global();
    if (proj) {
        progress_store_tx_lock();
        bool reseeded = utxo_projection_reseed_from_coins_kv(proj, db);
        progress_store_tx_unlock();
        if (!reseeded) {
            LOG_WARN("utxo_apply",
                     "[utxo_apply] value_overflow repair: projection reseed "
                     "from coins_kv failed after consensus rewind "
                     "(non-blocking)");
        }
    }
    if (out)
        *out = local;
    return true;
}
