/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_refill — refill forward-only reducer stages
 * after a hole is discovered below their cursor.
 *
 * FIX-2 (script/proof missing-row refill) lives here too: one shared
 * scan+clamp core with TWO invocations —
 *   FIX-2a stage_reducer_frontier_try_unapplied_hole_clamp(): pre-refusal,
 *          bounds [max(hstar+1, coins_applied), min(cursor-1, sweep_top)],
 *          so only provably-UNAPPLIED heights are eligible. On clamp it
 *          clears refused_coin_tear (the pre-refusal hash-split precedent
 *          in stage_repair_reducer_frontier.c).
 *   FIX-2b stage_reducer_frontier_reconcile_refill_cursors(): post-refusal,
 *          bounds [hstar+1, min(cursor-1, sweep_top)] with the same coins
 *          floor on the clamp target.
 *
 * Coins floor (hard): a clamp target h must satisfy
 * h >= coins_applied_height. Coins are applied THROUGH coins_applied - 1
 * (utxo_apply_stage.c co-commit), so the height at coins_applied is
 * provably unapplied and a forward re-walk cannot hit spent prevouts. A
 * hole strictly below the coins frontier is the stale-script replay's
 * domain (inverse-delta machinery, stage_repair_reducer_frontier_coin.c).
 *
 * NEVER touch the utxo_apply cursor here: rewinding it would re-apply
 * coin deltas that are already committed (double-applied coins). Refill
 * clamps are limited to the script_validate / proof_validate cursors,
 * whose log stores are INSERT OR REPLACE — the forward re-walk rewrites
 * fresh verdicts and deletes nothing.
 */

#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"
#include "stage_repair_reducer_frontier_internal.h"

#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stddef.h>

/* Shared scan runner: `sql` selects the single lowest hole height and binds
 * exactly two integer bounds (start, end). *out_height = -1 when no hole. */
static bool find_lowest_hole_unlocked(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int start_height,
    int end_height,
    int *out_height)
{
    if (out_height)
        *out_height = -1;
    if (!db || !out_height)
        return false;
    if (start_height > end_height)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s scan prepare failed: %s",
                 label, sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(st, 1, start_height);
    sqlite3_bind_int(st, 2, end_height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s scan failed rc=%d: %s",
                 label, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool find_lowest_validate_headers_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    return find_lowest_hole_unlocked(db, "validate_headers refill",
        "SELECT h.height "
        "FROM header_admit_log h "
        "LEFT JOIN validate_headers_log v ON v.height = h.height "
        "WHERE h.height >= ? AND h.height <= ? "
        "AND v.height IS NULL "
        "ORDER BY h.height ASC LIMIT 1",
        start_height, end_height, out_height);
}

static bool find_lowest_validate_headers_hash_split_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    return find_lowest_hole_unlocked(db, "validate_headers hash-split",
        "SELECT v.height "
        "FROM validate_headers_log v "
        "JOIN script_validate_log s ON s.height = v.height "
        "WHERE v.height >= ? AND v.height <= ? "
        "AND v.ok = 1 AND s.ok = 1 "
        "AND length(v.hash) = 32 AND length(s.block_hash) = 32 "
        "AND v.hash <> s.block_hash "
        "ORDER BY v.height ASC LIMIT 1",
        start_height, end_height, out_height);
}

static bool find_lowest_body_fetch_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    return find_lowest_hole_unlocked(db, "body_fetch refill",
        "SELECT v.height "
        "FROM validate_headers_log v "
        "LEFT JOIN body_fetch_log b ON b.height = v.height "
        "WHERE v.height >= ? AND v.height <= ? "
        "AND v.ok = 1 AND b.height IS NULL "
        "ORDER BY v.height ASC LIMIT 1",
        start_height, end_height, out_height);
}

static bool find_lowest_body_persist_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    return find_lowest_hole_unlocked(db, "body_persist refill",
        "SELECT b.height "
        "FROM body_fetch_log b "
        "LEFT JOIN body_persist_log p ON p.height = b.height "
        "WHERE b.height >= ? AND b.height <= ? "
        "AND b.ok = 1 AND p.height IS NULL "
        "ORDER BY b.height ASC LIMIT 1",
        start_height, end_height, out_height);
}

/* FIX-2 scan: lowest height whose body persisted ok=1 but whose
 * script_validate_log row is MISSING entirely (rowless hole, not an ok=0
 * verdict — real verdicts are never bulldozed). */
static bool find_lowest_script_validate_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    return find_lowest_hole_unlocked(db, "script_validate refill",
        "SELECT b.height "
        "FROM body_persist_log b "
        "LEFT JOIN script_validate_log s ON s.height = b.height "
        "WHERE b.height >= ? AND b.height <= ? "
        "AND b.ok = 1 AND s.height IS NULL "
        "ORDER BY b.height ASC LIMIT 1",
        start_height, end_height, out_height);
}

/* FIX-2 scan: lowest height with script ok=1 but no proof_validate_log
 * row. NOTE: this join is blind to a height whose script row is itself
 * missing (the live 3,138,947 shape) — the caller folds the script hole
 * in when no proof row exists there. */
static bool find_lowest_proof_validate_refill_hole_unlocked(
    sqlite3 *db,
    int start_height,
    int end_height,
    int *out_height)
{
    return find_lowest_hole_unlocked(db, "proof_validate refill",
        "SELECT s.height "
        "FROM script_validate_log s "
        "LEFT JOIN proof_validate_log p ON p.height = s.height "
        "WHERE s.height >= ? AND s.height <= ? "
        "AND s.ok = 1 AND p.height IS NULL "
        "ORDER BY s.height ASC LIMIT 1",
        start_height, end_height, out_height);
}

/* Shared row-presence probe: `sql` is "SELECT 1 FROM <log> WHERE height = ?
 * LIMIT 1". A present row (any verdict) is evidence the stage already
 * spoke at that height. */
static bool log_row_present_unlocked(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int height,
    bool *present)
{
    if (present)
        *present = false;
    if (!db || !present)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s presence prepare failed: %s",
                 label, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *present = true;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s presence failed h=%d rc=%d: %s",
                 label, height, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }

    sqlite3_finalize(st);
    return true;
}

static bool body_persist_log_present_unlocked(sqlite3 *db,
                                              int height,
                                              bool *present)
{
    return log_row_present_unlocked(db, "body_persist",
        "SELECT 1 FROM body_persist_log WHERE height = ? LIMIT 1",
        height, present);
}

static bool proof_validate_log_present_unlocked(sqlite3 *db,
                                                int height,
                                                bool *present)
{
    return log_row_present_unlocked(db, "proof_validate",
        "SELECT 1 FROM proof_validate_log WHERE height = ? LIMIT 1",
        height, present);
}

static bool reconcile_validate_headers_refill_holes(
    sqlite3 *db,
    struct main_state *ms,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->validate_headers_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->validate_headers_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    if (!find_lowest_validate_headers_refill_hole_unlocked(
            db, out->hstar + 1, end_height,
            &out->lowest_validate_headers_refill_hole))
        return false;
    if (!find_lowest_validate_headers_hash_split_unlocked(
            db, out->hstar + 1, end_height,
            &out->lowest_validate_headers_hash_split))
        return false;

    /* VALIDATE-SIDE-ONLY clamp: the cursor rewind below re-derives the
     * canonical HEADER, which cures a stale validate_headers verdict but is a
     * semantic no-op for a SCRIPT-side split (it leaves the stale script row, so
     * H* cannot climb and the clamp would self-clear without progress). Classify
     * the split by the canonical active header; route a script-side split to the
     * coins-rewinding dual replay (lowest_script_validate_hash_split) and drop it
     * from the validate clamp target. INDETERMINATE (active header unavailable)
     * keeps the existing direction-blind clamp as a fallback. */
    if (out->lowest_validate_headers_hash_split >= 0) {
        bool err = false;
        enum rf_hash_split_side side = stage_repair_classify_hash_split(
            ms, db, out->lowest_validate_headers_hash_split, &err);
        if (err)
            return false;
        if (side == RF_SPLIT_SCRIPT_SIDE) {
            out->lowest_script_validate_hash_split =
                out->lowest_validate_headers_hash_split;
            out->lowest_validate_headers_hash_split = -1;
        }
    }
    return true;
}

static bool reconcile_body_fetch_refill_holes(
    sqlite3 *db,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->body_fetch_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->body_fetch_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    return find_lowest_body_fetch_refill_hole_unlocked(
        db, out->hstar + 1, end_height,
        &out->lowest_body_fetch_refill_hole);
}

static bool reconcile_body_persist_refill_holes(
    sqlite3 *db,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->body_persist_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->body_persist_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    return find_lowest_body_persist_refill_hole_unlocked(
        db, out->hstar + 1, end_height,
        &out->lowest_body_persist_refill_hole);
}

bool stage_reducer_frontier_force_stage_cursor_in_tx(
    sqlite3 *db,
    const char *stage_name,
    const char *label,
    int target)
{
    progress_store_tx_lock();

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier %s BEGIN failed: %s",
                 label, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    if (!stage_repair_force_stage_cursor(db, stage_name, target)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier %s COMMIT failed: %s",
                 label, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        return false;
    }

    progress_store_tx_unlock();
    return true;
}

bool stage_reducer_frontier_reconcile_validate_hash_split_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (out->validate_headers_cursor_before <= out->hstar + 1)
        return true;

    int end_height = out->validate_headers_cursor_before - 1;
    if (out->sweep_top < end_height)
        end_height = out->sweep_top;
    if (!find_lowest_validate_headers_hash_split_unlocked(
            db, out->hstar + 1, end_height,
            &out->lowest_validate_headers_hash_split))
        return false;
    if (out->lowest_validate_headers_hash_split < 0)
        return true;

    if (out->validate_headers_cursor_before <=
        out->lowest_validate_headers_hash_split) {
        out->validate_headers_cursor_after =
            out->validate_headers_cursor_before;
        return true;
    }

    out->clamped_validate_headers = true;
    out->validate_headers_cursor_after =
        out->lowest_validate_headers_hash_split;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "validate_headers", "validate_headers-hash-split",
        out->lowest_validate_headers_hash_split);
}

static int lower_refill_target(int target, int candidate)
{
    if (candidate >= 0 && (target < 0 || candidate < target))
        return candidate;
    return target;
}

static bool reconcile_validate_headers_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_validate_headers_refill_hole;
    target = lower_refill_target(target,
                                 out->lowest_validate_headers_hash_split);
    if (target < 0) {
        out->validate_headers_cursor_after =
            out->validate_headers_cursor_before;
        return true;
    }

    if (out->validate_headers_cursor_before <= target) {
        out->validate_headers_cursor_after =
            out->validate_headers_cursor_before;
        return true;
    }

    out->clamped_validate_headers = true;
    out->validate_headers_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "validate_headers", "validate_headers", target);
}

static bool reconcile_body_fetch_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_have_data_cleared;
    target = lower_refill_target(target,
                                 out->lowest_validate_headers_refill_hole);
    target = lower_refill_target(target, out->lowest_body_fetch_refill_hole);
    if (target < 0) {
        out->body_fetch_cursor_after = out->body_fetch_cursor_before;
        return true;
    }

    if (out->body_fetch_cursor_before <= target) {
        out->body_fetch_cursor_after = out->body_fetch_cursor_before;
        return true;
    }

    out->clamped_body_fetch = true;
    out->body_fetch_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "body_fetch", "body_fetch", target);
}

static bool reconcile_body_persist_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_body_persist_refill_hole;
    int upstream_targets[] = {
        out->lowest_validate_headers_refill_hole,
        out->lowest_body_fetch_refill_hole,
    };
    for (size_t i = 0;
         i < sizeof(upstream_targets) / sizeof(upstream_targets[0]);
         i++) {
        int candidate = upstream_targets[i];
        if (candidate < 0)
            continue;
        bool present = false;
        if (!body_persist_log_present_unlocked(db, candidate, &present))
            return false;
        if (!present)
            target = lower_refill_target(target, candidate);
    }
    if (target < 0) {
        out->body_persist_cursor_after = out->body_persist_cursor_before;
        return true;
    }

    if (out->body_persist_cursor_before <= target) {
        out->body_persist_cursor_after = out->body_persist_cursor_before;
        return true;
    }

    out->clamped_body_persist = true;
    out->body_persist_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "body_persist", "body_persist", target);
}

/* Snapshot the script_validate / proof_validate cursors into the result
 * (before == after until a clamp lands). */
static bool read_script_proof_cursors(
    sqlite3 *db,
    struct stage_reducer_frontier_reconcile_result *out)
{
    progress_store_tx_lock();

    int script_cursor = -1;
    if (!stage_repair_cursor_at_unlocked(db, "script_validate",
                                         &script_cursor)) {
        progress_store_tx_unlock();
        return false;
    }

    int proof_cursor = -1;
    if (!stage_repair_cursor_at_unlocked(db, "proof_validate",
                                         &proof_cursor)) {
        progress_store_tx_unlock();
        return false;
    }

    progress_store_tx_unlock();

    out->script_validate_cursor_before = script_cursor;
    out->script_validate_cursor_after = script_cursor;
    out->proof_validate_cursor_before = proof_cursor;
    out->proof_validate_cursor_after = proof_cursor;
    return true;
}

/* FIX-2 coins floor (hard): only clamp to heights whose coins are provably
 * UNAPPLIED — coins are applied THROUGH coins_applied_height - 1
 * (utxo_apply_stage.c co-commit), so h >= coins_applied_height means the
 * forward re-walk re-validates fresh and cannot double-apply deltas or hit
 * spent prevouts. Anything below the frontier is refused here and left to
 * the stale-script replay (inverse-delta machinery). */
static bool refill_target_in_unapplied_domain(
    const struct stage_reducer_frontier_reconcile_result *out,
    const char *stage_name,
    int target)
{
    if (!out->coins_applied_found || out->coins_applied_height < 0) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s refill clamp refused: coins frontier "
                 "unknown (target h=%d)",
                 stage_name, target);
        return false;
    }
    if (target < out->coins_applied_height) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s refill hole h=%d is strictly below the "
                 "coins frontier coins_applied=%d: replay domain "
                 "(inverse-delta), refusing forward refill clamp",
                 stage_name, target, out->coins_applied_height);
        return false;
    }
    return true;
}

static bool reconcile_script_validate_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_script_validate_refill_hole;
    if (target < 0)
        return true;
    if (!refill_target_in_unapplied_domain(out, "script_validate", target))
        return true;
    if (out->script_validate_cursor_before <= target)
        return true;

    out->clamped_script_validate = true;
    out->script_validate_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "script_validate", "script_validate-refill", target);
}

static bool reconcile_proof_validate_cursor(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int target = out->lowest_proof_validate_refill_hole;

    /* Fold in the script hole when NO proof row exists there: the proof
     * scan's upstream join (script ok=1) cannot see a height whose script
     * row is itself the missing one. An existing proof row at that height
     * is a real verdict and is left alone (never deleted). */
    int script_hole = out->lowest_script_validate_refill_hole;
    if (script_hole >= 0) {
        bool present = false;
        if (!proof_validate_log_present_unlocked(db, script_hole, &present))
            return false;
        if (!present)
            target = lower_refill_target(target, script_hole);
    }

    if (target < 0)
        return true;
    if (!refill_target_in_unapplied_domain(out, "proof_validate", target))
        return true;
    if (out->proof_validate_cursor_before <= target)
        return true;

    out->clamped_proof_validate = true;
    out->proof_validate_cursor_after = target;
    if (!apply)
        return true;

    return stage_reducer_frontier_force_stage_cursor_in_tx(
        db, "proof_validate", "proof_validate-refill", target);
}

/* FIX-2 shared core (ONE implementation, TWO call sites): scan for rowless
 * script/proof holes from `scan_floor` up to min(cursor-1, sweep_top) per
 * stage, then clamp each cursor to its hole subject to the coins floor.
 * Touches ONLY the script_validate / proof_validate cursors; both log
 * stores are INSERT OR REPLACE, so the forward re-walk rewrites fresh
 * verdicts and no log row is ever deleted. */
static bool reconcile_script_proof_refill_cursors(
    sqlite3 *db,
    bool apply,
    int scan_floor,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        return false;
    if (!read_script_proof_cursors(db, out))
        return false;

    out->lowest_script_validate_refill_hole = -1;
    out->lowest_proof_validate_refill_hole = -1;

    if (out->script_validate_cursor_before > scan_floor) {
        int end_height = out->script_validate_cursor_before - 1;
        if (out->sweep_top < end_height)
            end_height = out->sweep_top;
        if (!find_lowest_script_validate_refill_hole_unlocked(
                db, scan_floor, end_height,
                &out->lowest_script_validate_refill_hole))
            return false;
    }

    if (out->proof_validate_cursor_before > scan_floor) {
        int end_height = out->proof_validate_cursor_before - 1;
        if (out->sweep_top < end_height)
            end_height = out->sweep_top;
        if (!find_lowest_proof_validate_refill_hole_unlocked(
                db, scan_floor, end_height,
                &out->lowest_proof_validate_refill_hole))
            return false;
    }

    if (!reconcile_script_validate_cursor(db, apply, out))
        return false;
    return reconcile_proof_validate_cursor(db, apply, out);
}

/* FIX-2a — pre-refusal invocation. Runs while refused_coin_tear is pending,
 * after the replay repairs and BEFORE the coin-tear refusal (call site in
 * stage_repair_reducer_frontier.c). Bounds start at
 * max(hstar+1, coins_applied) so by construction only provably-unapplied
 * heights are eligible. On clamp it clears refused_coin_tear and claims the
 * tick (the exact shape of the pre-refusal hash-split repair), which lets
 * the script stage's next tick rewrite the missing row within seconds. */
bool stage_reducer_frontier_try_unapplied_hole_clamp(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out,
    bool *handled)
{
    if (handled)
        *handled = false;
    if (!db || !out || !handled) {
        LOG_WARN("stage_repair",
                 "[stage_repair] unapplied-hole clamp: NULL argument "
                 "(db=%d out=%d handled=%d)",
                 db != NULL, out != NULL, handled != NULL);
        return false;
    }
    /* No coin-tear refusal pending: the post-refusal FIX-2b invocation in
     * reconcile_refill_cursors owns this tick (and the tip_finalize floor
     * reconcile must still run after it). Silent no-op — this is the
     * healthy per-tick path, not a refusal. */
    if (!out->refused_coin_tear)
        return true;
    if (!out->coins_applied_found || out->coins_applied_height < 0) {
        LOG_WARN("stage_repair",
                 "[stage_repair] unapplied-hole clamp refused: coins "
                 "frontier unknown (coins_applied_found=%d height=%d)",
                 out->coins_applied_found, out->coins_applied_height);
        return true;
    }

    int scan_floor = out->hstar + 1;
    if (out->coins_applied_height > scan_floor)
        scan_floor = out->coins_applied_height;

    if (!reconcile_script_proof_refill_cursors(db, apply, scan_floor, out))
        return false;

    if (!out->clamped_script_validate && !out->clamped_proof_validate)
        return true; /* no unapplied hole in bounds; the refusal proceeds */

    out->pre_refusal_unapplied_clamp = true;
    out->refused_coin_tear = false;
    out->repaired = true;
    *handled = true;
    if (apply) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier clamped unapplied "
                 "script/proof refill hole before coin-tear refusal "
                 "hstar=%d coins_applied=%d script_validate=%d->%d "
                 "proof_validate=%d->%d script_hole=%d proof_hole=%d",
                 out->hstar, out->coins_applied_height,
                 out->script_validate_cursor_before,
                 out->script_validate_cursor_after,
                 out->proof_validate_cursor_before,
                 out->proof_validate_cursor_after,
                 out->lowest_script_validate_refill_hole,
                 out->lowest_proof_validate_refill_hole);
    }
    return true;
}

bool stage_reducer_frontier_reconcile_refill_cursors(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!reconcile_validate_headers_refill_holes(db, ms, out))
        return false;

    if (!reconcile_body_fetch_refill_holes(db, out))
        return false;

    if (!reconcile_body_persist_refill_holes(db, out))
        return false;

    if (!reconcile_validate_headers_cursor(db, apply, out))
        return false;

    if (!reconcile_body_fetch_cursor(db, apply, out))
        return false;

    if (!reconcile_body_persist_cursor(db, apply, out))
        return false;

    /* FIX-2b — post-refusal invocation of the script/proof refill core:
     * bounds [hstar+1, min(cursor-1, sweep_top)], same coins floor. */
    if (!reconcile_script_proof_refill_cursors(db, apply, out->hstar + 1,
                                               out))
        return false;

    return true;
}
