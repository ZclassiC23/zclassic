/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_coin — L1 coin-tear discriminators.
 *
 * This file contains the guarded exceptions that can run before L1 refuses
 * coins_applied_height > H*: the one-shot value_overflow stale-verdict repair,
 * the frontier coin backfill hook, and the retained verdict-replay dispatch.
 * The script/proof/hash-split replay machinery lives in reducer_frontier_replay.c
 * because it is retained crash/reorg recovery, not borrowed coin-backfill code.
 */

#include "stage_repair_reducer_frontier_internal.h"

#include "config/runtime.h"
#include "jobs/stage_repair.h"
#include "jobs/stage_repair_coin_backfill.h"
#include "jobs/stage_repair_internal.h"
#include "jobs/utxo_apply_delta.h"
#include "primitives/block.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>

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
              stage_reducer_frontier_log_hole_below_unlocked(
                  db,
                  "SELECT height FROM utxo_apply_log "
                  "WHERE ok = 0 AND status = 'value_overflow' AND height < ? "
                  "ORDER BY height LIMIT 1",
                  "value_overflow", cursor, &height);
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
     * REPAIRED claim the tick as progress. Refusals claim the tick as a named
     * blocker without setting `repaired`; falling through lets unrelated cursor
     * clamps masquerade as success while the coin hole remains unresolved. */
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
    if (cb.status != COIN_BACKFILL_NOT_APPLICABLE) {
        *handled = true;
        return true;
    }

    if (!stage_reducer_frontier_try_stale_script_replay(db, ms, apply, out))
        LOG_RETURN(false, "stage_repair",
                   "[stage_repair] reducer_frontier stale script replay "
                   "step failed");
    if (out->stale_script_repaired) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier repaired stale script "
                 "hole h=%d; forward stages must replay from the hole "
                 "before L1 continues",
                 out->stale_script_repair_height);
        *handled = true;
        return true;
    }

    /* hash_split (validate-script-hash-mismatch): script_validate passed a
     * non-canonical body so its block_hash disagrees with the canonical header
     * validate_headers re-derived, capping H* with no other repair owner. Same
     * one-shot replay (re-derive the script verdict against the canonical
     * active block) — the missing complement to the validate_headers-cursor
     * rewind in reconcile_refill_cursors, which alone never clears the split. */
    if (!stage_reducer_frontier_try_validate_script_hash_split_replay(
            db, ms, apply, out))
        LOG_RETURN(false, "stage_repair",
                   "[stage_repair] reducer_frontier validate/script "
                   "hash-split replay step failed");
    if (out->stale_script_repaired) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier repaired validate-script "
                 "hash split h=%d; script_validate must re-derive the verdict "
                 "against the canonical body before L1 continues",
                 out->stale_script_repair_height);
        *handled = true;
        return true;
    }

    /* Proof-stage symmetry (self-verified-tip-plan Act 1): a transient
     * proof_validate internal_error at a height where script already passed is
     * NOT caught by the script hole sweep (it scans script_validate_log only).
     * Without this it would stay a permanent ok=0 — "couldn't determine
     * validity" frozen as "invalid", the inverse Law-7 lie. Re-derive it. */
    if (!stage_reducer_frontier_try_stale_proof_replay(db, ms, apply, out))
        LOG_RETURN(false, "stage_repair",
                   "[stage_repair] reducer_frontier stale proof replay "
                   "step failed");
    if (out->stale_script_repaired) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier repaired transient proof "
                 "internal_error hole h=%d; proof_validate must re-derive the "
                 "verdict before L1 continues",
                 out->stale_script_repair_height);
        *handled = true;
        return true;
    }

    return true;
}
