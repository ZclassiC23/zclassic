/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier — L1 reducer-frontier reconcile.
 *
 * This is deliberately limited to block_index mirror flags plus the
 * body_fetch and tip_finalize cursors. It never deletes reducer logs and never
 * mutates coins.
 */

#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"
#include "stage_repair_reducer_frontier_internal.h"

#include "jobs/block_header_emit.h"
#include "jobs/reducer_frontier.h"
#include "storage/coins_kv.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct rf_log_evidence {
    bool validate_ok_hash;
    bool script_ok_hash;
    bool body_ok;
    bool proof_ok;
    bool utxo_ok;
};

static bool log_ok_unlocked(sqlite3 *db, const char *table, int height,
                            bool *found, bool *ok)
{
    *found = false;
    *ok = false;

    char sql[128];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT ok FROM %s WHERE height = ?", table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("stage_repair", "log_ok sql overflow table=%s", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] log_ok prepare failed table=%s: %s",
                 table, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found = true;
        *ok = sqlite3_column_int(st, 0) == 1;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] log_ok step failed table=%s h=%d rc=%d: %s",
                 table, height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool hash_log_ok_matches_unlocked(sqlite3 *db, const char *table,
                                         const char *hash_col, int height,
                                         const struct uint256 *want,
                                         bool *matches)
{
    *matches = false;
    if (!want)
        return true;

    char sql[160];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT ok, %s FROM %s WHERE height = ?",
                     hash_col, table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("stage_repair", "hash_log sql overflow table=%s col=%s",
                 table, hash_col);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] hash_log prepare failed table=%s: %s",
                 table, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        int row_ok = sqlite3_column_int(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int blen = sqlite3_column_bytes(st, 1);
        if (row_ok == 1 && blob && blen == 32 &&
            memcmp(blob, want->data, 32) == 0)
            *matches = true;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] hash_log step failed table=%s h=%d rc=%d: %s",
                 table, height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool evidence_for_block_unlocked(sqlite3 *db,
                                        const struct block_index *bi,
                                        struct rf_log_evidence *ev)
{
    memset(ev, 0, sizeof(*ev));
    if (!bi || !bi->phashBlock)
        return true;

    if (!hash_log_ok_matches_unlocked(db, "validate_headers_log", "hash",
                                      bi->nHeight, bi->phashBlock,
                                      &ev->validate_ok_hash))
        return false;
    if (!hash_log_ok_matches_unlocked(db, "script_validate_log",
                                      "block_hash", bi->nHeight,
                                      bi->phashBlock, &ev->script_ok_hash))
        return false;

    bool found = false;
    if (!log_ok_unlocked(db, "body_persist_log", bi->nHeight, &found,
                         &ev->body_ok))
        return false;
    ev->body_ok = found && ev->body_ok;

    found = false;
    if (!log_ok_unlocked(db, "proof_validate_log", bi->nHeight, &found,
                         &ev->proof_ok))
        return false;
    ev->proof_ok = found && ev->proof_ok;

    found = false;
    if (!log_ok_unlocked(db, "utxo_apply_log", bi->nHeight, &found,
                         &ev->utxo_ok))
        return false;
    ev->utxo_ok = found && ev->utxo_ok;
    return true;
}

static bool block_pos_readable_hash(const struct block_index *bi,
                                    const char *datadir)
{
    if (!bi || !bi->phashBlock || !datadir || bi->nFile < 0)
        return false;

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    pos.nFile = bi->nFile;
    pos.nPos = bi->nDataPos;

    struct block blk;
    block_init(&blk);
    bool ok = read_block_from_disk_pread(&blk, &pos, datadir);
    if (ok) {
        struct uint256 got;
        block_get_hash(&blk, &got);
        ok = uint256_cmp(&got, bi->phashBlock) == 0;
    }
    block_free(&blk);
    return ok;
}

static bool read_frontier_snapshot(sqlite3 *db,
                                   struct stage_reducer_frontier_reconcile_result *out)
{
    progress_store_tx_lock();

    int32_t hstar = 0;
    int32_t served_floor = 0;
    if (!reducer_frontier_compute_hstar(db, &hstar, &served_floor)) {
        progress_store_tx_unlock();
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier_compute_hstar failed");
        return false;
    }

    int tip_cursor = -1;
    if (!stage_repair_cursor_at_unlocked(db, "tip_finalize", &tip_cursor)) {
        progress_store_tx_unlock();
        return false;
    }

    int validate_headers_cursor = -1;
    if (!stage_repair_cursor_at_unlocked(db, "validate_headers",
                                         &validate_headers_cursor)) {
        progress_store_tx_unlock();
        return false;
    }

    int body_fetch_cursor = -1;
    if (!stage_repair_cursor_at_unlocked(db, "body_fetch",
                                         &body_fetch_cursor)) {
        progress_store_tx_unlock();
        return false;
    }

    int body_persist_cursor = -1;
    if (!stage_repair_cursor_at_unlocked(db, "body_persist",
                                         &body_persist_cursor)) {
        progress_store_tx_unlock();
        return false;
    }

    int32_t coins_applied = 0;
    bool coins_found = false;
    if (!coins_kv_get_applied_height(db, &coins_applied, &coins_found)) {
        progress_store_tx_unlock();
        LOG_WARN("stage_repair",
                 "[stage_repair] coins_applied_height read failed");
        return false;
    }

    static const char *const stages[] = {
        "validate_headers",
        "body_fetch",
        "body_persist",
        "script_validate",
        "proof_validate",
        "utxo_apply",
        "tip_finalize",
    };
    int sweep_top = served_floor;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        int cursor = -1;
        if (!stage_repair_cursor_at_unlocked(db, stages[i], &cursor)) {
            progress_store_tx_unlock();
            return false;
        }
        if (cursor > 0 && cursor - 1 > sweep_top)
            sweep_top = cursor - 1;
    }

    progress_store_tx_unlock();

    out->hstar = hstar;
    out->served_floor = served_floor;
    out->validate_headers_cursor_before = validate_headers_cursor;
    out->validate_headers_cursor_after = validate_headers_cursor;
    out->body_fetch_cursor_before = body_fetch_cursor;
    out->body_fetch_cursor_after = body_fetch_cursor;
    out->body_persist_cursor_before = body_persist_cursor;
    out->body_persist_cursor_after = body_persist_cursor;
    out->tip_finalize_cursor_before = tip_cursor;
    out->tip_finalize_cursor_after = tip_cursor;
    out->sweep_top = sweep_top;
    out->lowest_have_data_cleared = -1;
    out->lowest_validate_headers_refill_hole = -1;
    out->lowest_validate_headers_hash_split = -1;
    out->lowest_body_fetch_refill_hole = -1;
    out->lowest_body_persist_refill_hole = -1;
    out->coins_applied_found = coins_found;
    out->coins_applied_height = coins_found ? coins_applied : -1;
    if (!coins_found)
        out->refused_coin_unknown = true;
    else if (coins_applied > hstar + 1)
        out->refused_coin_tear = true;
    return true;
}

static bool maybe_emit_header(struct block_index *bi, bool apply,
                              const char *why,
                              struct stage_reducer_frontier_reconcile_result *out)
{
    if (!apply)
        return true;
    block_index_emit_header_event(bi, why, NULL, NULL);
    out->header_events_emitted++;
    return true;
}

static bool reconcile_block_index_flags(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));

    progress_store_tx_lock();
    zcl_mutex_lock(&ms->cs_main);

    bool ok = true;
    size_t iter = 0;
    struct block_index *bi = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (!bi || bi->nHeight <= out->hstar || bi->nHeight > out->sweep_top)
            continue;

        struct rf_log_evidence ev;
        if (!evidence_for_block_unlocked(db, bi, &ev)) {
            ok = false;
            break;
        }

        bool changed = false;
        bool readable = block_pos_readable_hash(bi, datadir);

        if (ev.script_ok_hash &&
            (bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) {
            if (apply) {
                bi->nStatus = (bi->nStatus & ~(unsigned)BLOCK_VALID_MASK)
                              | BLOCK_VALID_SCRIPTS;
            }
            out->scripts_set++;
            changed = true;
        }

        if (ev.validate_ok_hash && ev.body_ok && readable &&
            (bi->nStatus & BLOCK_HAVE_DATA) == 0) {
            if (apply)
                bi->nStatus |= BLOCK_HAVE_DATA;
            out->have_data_set++;
            changed = true;
        }

        if ((bi->nStatus & BLOCK_HAVE_DATA) && !readable) {
            if (apply) {
                bi->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
                bi->nFile = -1;
                bi->nDataPos = 0;
            }
            out->have_data_cleared++;
            if (out->lowest_have_data_cleared < 0 ||
                bi->nHeight < out->lowest_have_data_cleared)
                out->lowest_have_data_cleared = bi->nHeight;
            changed = true;
        }

        if ((bi->nStatus & BLOCK_FAILED_MASK) &&
            ev.script_ok_hash && ev.proof_ok && ev.utxo_ok) {
            if (apply)
                bi->nStatus &= ~(unsigned)BLOCK_FAILED_MASK;
            out->failed_mask_cleared++;
            changed = true;
        }

        if (changed)
            maybe_emit_header(bi, apply,
                              "reducer_frontier_reconcile_light", out);
    }

    zcl_mutex_unlock(&ms->cs_main);
    progress_store_tx_unlock();
    return ok;
}

static bool reconcile_tip_finalize_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int floor = out->hstar + 1;
    if (out->coins_applied_found &&
        out->coins_applied_height >= 0 &&
        out->coins_applied_height < floor)
        floor = out->coins_applied_height;
    if (out->tip_finalize_cursor_before == floor) {
        out->tip_finalize_cursor_after = floor;
        return true;
    }

    out->clamped_tip_finalize = true;
    out->tip_finalize_cursor_after = floor;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "tip_finalize", "L1", floor);
}

static bool reducer_frontier_reconcile_light_impl(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db)
        LOG_FAIL("stage_repair", "reducer_frontier_reconcile: NULL db");
    if (!ms)
        LOG_FAIL("stage_repair", "reducer_frontier_reconcile: NULL main_state");

    struct stage_reducer_frontier_reconcile_result local;
    memset(&local, 0, sizeof(local));
    local.tip_finalize_cursor_before = -1;
    local.tip_finalize_cursor_after = -1;
    local.validate_headers_cursor_before = -1;
    local.validate_headers_cursor_after = -1;
    local.body_fetch_cursor_before = -1;
    local.body_fetch_cursor_after = -1;
    local.body_persist_cursor_before = -1;
    local.body_persist_cursor_after = -1;
    local.lowest_have_data_cleared = -1;
    local.lowest_validate_headers_refill_hole = -1;
    local.lowest_validate_headers_hash_split = -1;
    local.lowest_body_fetch_refill_hole = -1;
    local.lowest_body_persist_refill_hole = -1;
    local.coins_applied_height = -1;

    if (!stage_table_ensure(db))
        return false;
    if (!read_frontier_snapshot(db, &local))
        return false;

    if (local.refused_coin_unknown) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier L1 refused: "
                 "coins_applied_height absent (coin frontier unknown)");
        if (out)
            *out = local;
        return true;
    }

    bool handled_replay = false;
    if (!stage_reducer_frontier_try_replay_repairs(
            db, ms, apply, &local, &handled_replay))
        return false;
    if (handled_replay) {
        if (out)
            *out = local;
        return true;
    }

    if (local.refused_coin_tear) {
        if (!stage_reducer_frontier_reconcile_validate_hash_split_cursor(
                db, apply, &local))
            return false;
        if (local.clamped_validate_headers) {
            local.refused_coin_tear = false;
            local.repaired = true;
            if (apply) {
                LOG_WARN("stage_repair",
                         "[stage_repair] reducer_frontier repaired stale "
                         "validate hash before coin-tear refusal "
                         "hstar=%d coins_applied=%d validate_headers=%d->%d "
                         "validate_hash_split=%d",
                         local.hstar, local.coins_applied_height,
                         local.validate_headers_cursor_before,
                         local.validate_headers_cursor_after,
                         local.lowest_validate_headers_hash_split);
            }
            if (out)
                *out = local;
            return true;
        }
    }

    if (local.refused_coin_tear) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier L1 refused: "
                 "coins_applied_height=%d > hstar_cursor=%d (L2 required)",
                 local.coins_applied_height, local.hstar + 1);
        if (out)
            *out = local;
        return true;
    }

    if (local.sweep_top > local.hstar &&
        !reconcile_block_index_flags(db, ms, apply, &local))
        return false;

    if (!stage_reducer_frontier_reconcile_refill_cursors(db, apply, &local))
        return false;

    if (!reconcile_tip_finalize_cursor(db, apply, &local))
        return false;

    local.repaired = local.repaired ||
                     local.clamped_tip_finalize ||
                     local.clamped_validate_headers ||
                     local.clamped_body_fetch ||
                     local.clamped_body_persist ||
                     local.scripts_set > 0 ||
                     local.have_data_set > 0 ||
                     local.have_data_cleared > 0 ||
                     local.failed_mask_cleared > 0;

    if (apply && local.repaired) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier L1 repaired hstar=%d "
                 "served_floor=%d coins_applied=%d sweep_top=%d "
                 "validate_headers=%d->%d body_fetch=%d->%d "
                 "body_persist=%d->%d tip_finalize=%d->%d scripts_set=%d "
                 "have_data_set=%d have_data_cleared=%d "
                 "validate_refill_hole=%d body_refill_hole=%d "
                 "validate_hash_split=%d body_persist_refill_hole=%d "
                 "failed_mask_cleared=%d",
                 local.hstar, local.served_floor, local.coins_applied_height,
                 local.sweep_top, local.validate_headers_cursor_before,
                 local.validate_headers_cursor_after,
                 local.body_fetch_cursor_before,
                 local.body_fetch_cursor_after,
                 local.body_persist_cursor_before,
                 local.body_persist_cursor_after,
                 local.tip_finalize_cursor_before,
                 local.tip_finalize_cursor_after,
                 local.scripts_set, local.have_data_set,
                 local.have_data_cleared,
                 local.lowest_validate_headers_refill_hole,
                 local.lowest_body_fetch_refill_hole,
                 local.lowest_validate_headers_hash_split,
                 local.lowest_body_persist_refill_hole,
                 local.failed_mask_cleared);
    }

    if (out)
        *out = local;
    return true;
}

bool stage_reducer_frontier_reconcile_light_needed(
    sqlite3 *db,
    struct main_state *ms,
    struct stage_reducer_frontier_reconcile_result *out)
{
    return reducer_frontier_reconcile_light_impl(db, ms, false, out);
}

bool stage_reducer_frontier_reconcile_light(
    sqlite3 *db,
    struct main_state *ms,
    struct stage_reducer_frontier_reconcile_result *out)
{
    return reducer_frontier_reconcile_light_impl(db, ms, true, out);
}
