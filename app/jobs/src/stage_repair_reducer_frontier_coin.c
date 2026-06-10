/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_coin — L1 coin-tear discriminators.
 *
 * This file contains the guarded exceptions that can run before L1 refuses
 * coins_applied_height > H*: the one-shot value_overflow stale-verdict
 * repair, the frontier coin backfill hook, and the stale-script replay.
 * The main reducer-frontier file remains the flag/body/tip sweep. */

#include "stage_repair_reducer_frontier_internal.h"

#include "coins/coins.h"
#include "config/runtime.h"
#include "jobs/stage_repair.h"
#include "jobs/stage_repair_coin_backfill.h"
#include "jobs/stage_repair_internal.h"
#include "jobs/created_outputs_index.h"
#include "jobs/script_validate_stage.h"
#include "jobs/utxo_apply_delta.h"
#include "utxo_apply_delta_internal.h"
#include "storage/coins_kv.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "script/script.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct repair_prevout_view {
    sqlite3 *db;
    int created_first;
    int created_last;
    int coin_frontier;
};

static bool hole_below_cursor_unlocked(sqlite3 *db, int cursor,
                                       const char *status, int *out_height)
{
    *out_height = -1;
    if (cursor <= 0)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height FROM utxo_apply_log "
            "WHERE ok = 0 AND status = ? AND height < ? "
            "ORDER BY height LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s hole prepare failed: %s",
                 status, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, cursor);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s hole step failed rc=%d: %s",
                 status, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool stale_script_hole_unlocked(sqlite3 *db, int cursor,
                                       int *out_height)
{
    *out_height = -1;
    if (cursor <= 0)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height FROM script_validate_log "
            "WHERE ok = 0 "
            "  AND status IN ('internal_error', 'prevout_unresolved', "
            "                 'block_decode_failed') "
            "  AND height < ? "
            "ORDER BY height LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script hole prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, cursor);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script hole step failed rc=%d: %s",
                 rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

bool stage_repair_read_active_block_checked(struct main_state *ms, int height,
                                            struct block *blk,
                                            struct uint256 *block_hash)
{
    if (!ms || !blk || !block_hash)
        LOG_FAIL("stage_repair", "read_active_block_checked: NULL input");

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    bool have = false;

    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (bi && bi->phashBlock && (bi->nStatus & BLOCK_HAVE_DATA) &&
        bi->nFile >= 0) {
        *block_hash = *bi->phashBlock;
        pos.nFile = bi->nFile;
        pos.nPos = bi->nDataPos;
        have = true;
    }
    zcl_mutex_unlock(&ms->cs_main);

    if (!have)
        return false;

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));
    if (!read_block_from_disk_pread(blk, &pos, datadir))
        return false;

    struct uint256 got;
    block_get_hash(blk, &got);
    if (uint256_cmp(&got, block_hash) != 0) {
        char want_hex[65];
        char got_hex[65];
        uint256_get_hex(block_hash, want_hex);
        uint256_get_hex(&got, got_hex);
        LOG_WARN("stage_repair",
                 "[stage_repair] repair read wrong block h=%d want=%s got=%s",
                 height, want_hex, got_hex);
        return false;
    }
    return true;
}

static bool reducer_repair_marker_key(char key[192], const char *kind,
                                      int height,
                                      const struct uint256 *block_hash)
{
    if (!key || !kind || !block_hash)
        return false;
    char hex[65];
    uint256_get_hex(block_hash, hex);
    int n = snprintf(key, 192, "reducer_frontier.%s_repair.%d.%s",
                     kind, height, hex);
    return n > 0 && n < 192;
}

static bool reducer_repair_marker_seen(sqlite3 *db, const char *key,
                                       bool *seen)
{
    *seen = false;
    uint8_t blob[8] = {0};
    size_t n = 0;
    if (!progress_meta_get(db, key, blob, sizeof(blob), &n, seen)) {
        LOG_WARN("stage_repair",
                 "[stage_repair] repair marker read failed key=%s",
                 key ? key : "(null)");
        return false;
    }
    return true;
}

static bool reducer_repair_marker_record_in_tx(sqlite3 *db, const char *key)
{
    uint8_t one = 1;
    if (!progress_meta_set_in_tx(db, key, &one, sizeof(one))) {
        LOG_WARN("stage_repair",
                 "[stage_repair] repair marker write failed key=%s",
                 key ? key : "(null)");
        return false;
    }
    return true;
}

static bool delta_exists(sqlite3 *db, int height, bool *exists)
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

static bool rewindable_utxo_row(sqlite3 *db, int height)
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
    bool ok_row = sqlite3_column_int(st, 0) == 1;
    sqlite3_finalize(st);

    bool have_delta = false;
    if (!delta_exists(db, height, &have_delta))
        return false;
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

static bool inverse_checked(sqlite3 *db, int first_h, int cursor)
{
    for (int h = cursor - 1; h >= first_h; h--) {
        if (!rewindable_utxo_row(db, h))
            return false;
        if (!utxo_apply_emit_inverse_delta(db, h))
            return false;
    }
    return true;
}

static bool created_outputs_height_indexed(sqlite3 *db, int height,
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

static bool backfill_created_outputs_range(sqlite3 *db, struct main_state *ms,
                                           int first_h, int last_h)
{
    if (!created_outputs_index_ensure_schema(db))
        return false;
    if (first_h > last_h)
        return true;

    for (int h = first_h; h <= last_h; h++) {
        bool indexed = false;
        if (!created_outputs_height_indexed(db, h, &indexed))
            return false;
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

static bool repair_prevout_resolver(const struct outpoint *prevout,
                                    struct tx_out *out, void *user)
{
    const struct repair_prevout_view *view = user;
    if (!prevout || !out || !view || !view->db)
        return false;

    int64_t value = 0;
    unsigned char script[MAX_SCRIPT_SIZE];
    size_t slen = 0;
    int created_h = -1;
    if (created_outputs_index_get_bounded(
            view->db, prevout->hash.data, prevout->n, view->created_first,
            view->created_last, &value, script, sizeof(script), &slen,
            &created_h)) {
        if (slen > MAX_SCRIPT_SIZE)
            return false;
        out->value = value;
        script_set(&out->script_pub_key, script, slen);
        return true;
    }

    struct coins c;
    coins_init(&c);
    if (coins_kv_get_coins(view->db, prevout->hash.data, &c)) {
        bool usable = c.height < view->coin_frontier &&
                      c.height <= view->created_last &&
                      prevout->n < c.num_vout &&
                      !tx_out_is_null(&c.vout[prevout->n]);
        if (usable) {
            const struct tx_out *src = &c.vout[prevout->n];
            size_t src_len = src->script_pub_key.size;
            if (src_len <= MAX_SCRIPT_SIZE) {
                out->value = src->value;
                script_set(&out->script_pub_key, src->script_pub_key.data,
                           src_len);
                coins_free(&c);
                return true;
            }
        }
        coins_free(&c);
    }
    return false;
}

static bool delete_log_range(sqlite3 *db, const char *table,
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

static bool dry_run_stale_script_replay(
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
    struct repair_prevout_view view = {
        .db = db,
        .created_first = replay_first,
        .created_last = height,
        .coin_frontier = replay_first,
    };
    bool ok = backfill_created_outputs_range(db, ms, replay_first,
                                             backfill_top) &&
              (utxo_cursor <= replay_first ||
               inverse_checked(db, replay_first, utxo_cursor)) &&
              script_validate_stage_dry_run_block_with_prevout(
                  blk, height, repair_prevout_resolver, &view, dry);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

static bool stale_script_replay_tx(
    sqlite3 *db,
    struct main_state *ms,
    int height,
    int replay_first,
    int script_cursor,
    int proof_cursor,
    int utxo_cursor,
    int tip_cursor,
    int backfill_top,
    const char *marker)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair BEGIN failed h=%d: %s",
                 height, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    bool rewind_coins = utxo_cursor > replay_first;
    if (!backfill_created_outputs_range(db, ms, replay_first, backfill_top) ||
        (rewind_coins && !inverse_checked(db, replay_first, utxo_cursor)) ||
        !delete_log_range(db, "script_validate_log", replay_first,
                          script_cursor) ||
        !delete_log_range(db, "proof_validate_log", replay_first,
                          proof_cursor) ||
        (rewind_coins &&
         !utxo_apply_delete_rows_above(db, replay_first, utxo_cursor - 1)) ||
        !stage_repair_force_stage_cursor(db, "script_validate", replay_first) ||
        !stage_repair_force_stage_cursor(db, "proof_validate", replay_first) ||
        !stage_repair_force_stage_cursor(db, "tip_finalize", replay_first) ||
        (rewind_coins &&
         !utxo_apply_unwind_write_cursor(db, (uint64_t)replay_first)) ||
        (rewind_coins &&
         !coins_kv_set_applied_height_in_tx(db, replay_first)) ||
        !reducer_repair_marker_record_in_tx(db, marker)) {
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

static bool maybe_repair_value_overflow(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int cursor = -1;
    int height = -1;

    progress_store_tx_lock();
    bool ok = stage_repair_cursor_at_unlocked(db, "utxo_apply", &cursor) &&
              hole_below_cursor_unlocked(db, cursor, "value_overflow",
                                         &height);
    progress_store_tx_unlock();
    if (!ok)
        return false;

    out->value_overflow_repair_height = height;
    out->value_overflow_cursor_before = cursor;
    out->value_overflow_cursor_after = cursor;
    if (height < 0 || cursor <= 0 || height >= cursor)
        return true;
    if (!apply) {
        out->repaired = true;
        return true;
    }

    struct block blk;
    struct uint256 block_hash;
    block_init(&blk);
    if (!stage_repair_read_active_block_checked(ms, height, &blk,
                                                &block_hash)) {
        LOG_WARN("stage_repair",
                 "[stage_repair] value_overflow repair refused: cannot read "
                 "canonical block h=%d",
                 height);
        block_free(&blk);
        return true;
    }

    struct utxo_apply_value_overflow_repair_result rr;
    ok = utxo_apply_repair_value_overflow_hole(
        db, height, (uint64_t)cursor, &block_hash, &blk, &rr);
    block_free(&blk);
    if (!ok)
        return false;

    out->value_overflow_repair_attempted = rr.attempted;
    out->value_overflow_repaired = rr.repaired;
    out->value_overflow_repair_owner_refused = rr.owner_refused;
    out->value_overflow_repair_marker_seen = rr.marker_seen;
    out->value_overflow_repair_genuinely_invalid = rr.genuinely_invalid;
    out->value_overflow_cursor_after = (int)rr.cursor_after;
    if (rr.repaired) {
        out->refused_coin_tear = false;
        out->repaired = true;
    }
    return true;
}

static bool maybe_repair_stale_script(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int script_cursor = -1;
    int proof_cursor = -1;
    int utxo_cursor = -1;
    int tip_cursor = -1;
    int body_cursor = -1;
    int height = -1;
    int32_t coins_frontier = -1;
    bool coins_found = false;

    progress_store_tx_lock();
    bool ok = stage_repair_cursor_at_unlocked(db, "script_validate",
                                              &script_cursor) &&
              stage_repair_cursor_at_unlocked(db, "proof_validate",
                                              &proof_cursor) &&
              stage_repair_cursor_at_unlocked(db, "utxo_apply",
                                              &utxo_cursor) &&
              stage_repair_cursor_at_unlocked(db, "tip_finalize",
                                              &tip_cursor) &&
              stage_repair_cursor_at_unlocked(db, "body_persist",
                                              &body_cursor) &&
              coins_kv_get_applied_height(db, &coins_frontier,
                                           &coins_found) &&
              stale_script_hole_unlocked(db, script_cursor, &height);
    progress_store_tx_unlock();
    if (!ok)
        return false;

    int replay_first = (coins_found && coins_frontier >= 0 &&
                        coins_frontier < height)
                           ? coins_frontier
                           : height;
    out->stale_script_repair_height = height;
    out->stale_script_cursor_before = script_cursor;
    out->stale_script_cursor_after = script_cursor;
    out->stale_script_utxo_cursor_before = utxo_cursor;
    out->stale_script_tip_cursor_before = tip_cursor;
    out->stale_script_backfill_first = replay_first;
    out->stale_script_backfill_last = body_cursor > 0 ? body_cursor - 1 : -1;
    if (height < 0 || script_cursor <= 0 || height >= script_cursor ||
        proof_cursor <= height || body_cursor <= height || !coins_found)
        return true;
    if (!apply) {
        out->repaired = true;
        return true;
    }

    struct block blk;
    struct uint256 block_hash;
    block_init(&blk);
    if (!stage_repair_read_active_block_checked(ms, height, &blk,
                                                &block_hash)) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair refused: cannot read "
                 "canonical block h=%d",
                 height);
        block_free(&blk);
        return true;
    }

    out->stale_script_repair_attempted = true;
    if (utxo_projection_get_author() != UTXO_AUTHOR_STAGE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair refused h=%d: "
                 "utxo author is not stage",
                 height);
        block_free(&blk);
        return true;
    }

    progress_store_tx_lock();

    struct script_validate_dry_run_report dry;
    int backfill_top = body_cursor - 1;
    if (!dry_run_stale_script_replay(db, ms, height, replay_first, utxo_cursor,
                                     backfill_top, &blk, &dry)) {
        progress_store_tx_unlock();
        block_free(&blk);
        return false;
    }
    if (!dry.ok) {
        out->stale_script_repair_genuinely_invalid = true;
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair: H genuinely invalid "
                 "height=%d status=%s",
                 height, dry.status);
        progress_store_tx_unlock();
        block_free(&blk);
        return true;
    }

    char marker[192];
    if (!reducer_repair_marker_key(marker, "script_replay", height,
                                   &block_hash)) {
        progress_store_tx_unlock();
        block_free(&blk);
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair marker overflow h=%d",
                 height);
        return false;
    }
    bool marker_seen = false;
    if (!reducer_repair_marker_seen(db, marker, &marker_seen)) {
        progress_store_tx_unlock();
        block_free(&blk);
        return false;
    }
    if (marker_seen) {
        out->stale_script_repair_marker_seen = true;
        LOG_WARN("stage_repair",
                 "[stage_repair] stale script repair skipped h=%d: "
                 "one-shot marker already present",
                 height);
        progress_store_tx_unlock();
        block_free(&blk);
        return true;
    }

    ok = stale_script_replay_tx(db, ms, height, replay_first, script_cursor,
                                proof_cursor, utxo_cursor, tip_cursor,
                                backfill_top, marker);
    progress_store_tx_unlock();
    block_free(&blk);
    if (!ok)
        return false;

    out->stale_script_repaired = true;
    out->stale_script_cursor_after = replay_first;
    out->refused_coin_tear = false;
    out->repaired = true;
    LOG_WARN("stage_repair",
             "[stage_repair] stale script repair rewound replay cursors to "
             "h=%d for stale hole h=%d (script=%d proof=%d utxo=%d tip=%d; "
             "coins_frontier=%d created_outputs backfilled %d..%d)",
             replay_first, height, script_cursor, proof_cursor, utxo_cursor,
             tip_cursor, coins_frontier, replay_first, backfill_top);
    return true;
}

/* Production coin_backfill_io.read_block: user is the main_state. */
static bool repair_read_block_thunk(void *user, int height, struct block *blk,
                                    struct uint256 *hash)
{
    return stage_repair_read_active_block_checked(user, height, blk, hash);
}

bool stage_reducer_frontier_try_replay_repairs(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out,
    bool *handled)
{
    if (!out || !handled)
        LOG_FAIL("stage_repair", "replay repair: NULL output");
    *handled = false;

    if (!maybe_repair_value_overflow(db, ms, apply, out))
        return false;
    if (out->value_overflow_repaired) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier repaired stale "
                 "value_overflow hole h=%d utxo_apply=%d->%d; "
                 "forward stage replay must fill the hole before L1 continues",
                 out->value_overflow_repair_height,
                 out->value_overflow_cursor_before,
                 out->value_overflow_cursor_after);
        *handled = true;
        return true;
    }

    /* Frontier coin backfill (jobs/stage_repair_coin_backfill.h) runs BEFORE
     * the stale-script replay: a prevout_unresolved hole needs its missing
     * coin(s) backfilled before the replay dry-run can resolve. SCANNING /
     * REPAIRED claim the tick; NOT_APPLICABLE and refusals fall through. */
    struct coin_backfill_result cb = {0};
    struct coin_backfill_io io = {
        .read_block = repair_read_block_thunk,
        .user = ms,
        .ndb = app_runtime_node_db(),
    };
    if (!stage_repair_coin_backfill_try(db, ms, &io, apply, &cb))
        return false;
    out->coin_backfill_attempted = cb.status != COIN_BACKFILL_NOT_APPLICABLE;
    out->coin_backfill_status = (int)cb.status;
    out->coin_backfill_hole_height = cb.hole_height;
    out->coin_backfill_unresolved = cb.unresolved_count;
    out->coin_backfill_inserted = cb.inserted_count;
    out->coin_backfill_scan_next = cb.scan_next_height;
    out->coin_backfill_owner_refused =
        cb.status == COIN_BACKFILL_OWNER_REFUSED;
    out->coin_backfill_genuinely_invalid =
        cb.status == COIN_BACKFILL_REFUSED_SPENT;
    if (cb.status == COIN_BACKFILL_SCANNING ||
        cb.status == COIN_BACKFILL_REPAIRED) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier coin backfill %s hole h=%d "
                 "unresolved=%d inserted=%d scan_next=%d top=%d",
                 cb.status == COIN_BACKFILL_REPAIRED ? "repaired" : "scanning",
                 cb.hole_height, cb.unresolved_count, cb.inserted_count,
                 cb.scan_next_height, cb.scan_top_height);
        out->repaired = true;
        *handled = true;
        return true;
    }

    if (!maybe_repair_stale_script(db, ms, apply, out))
        return false;
    if (out->stale_script_repaired) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier repaired stale script "
                 "hole h=%d; forward stages must replay from the hole "
                 "before L1 continues",
                 out->stale_script_repair_height);
        *handled = true;
        return true;
    }

    return true;
}
