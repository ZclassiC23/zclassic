/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
// one-result-type-ok:frontier-gate-primitives — these are the shared gate
// primitives (frontier read, candidate clamp, evidence-based floor rewind)
// consumed by the zcl_result-returning commit/restore surfaces in
// utxo_recovery_service.c / utxo_recovery_restore.c. Every refusal travels
// via structured LOG_WARN + EV_RECOVERY_ACTION, and the callers wrap the
// verdict in their own zcl_result.
/*
 * INVARIANT A — never INSTALL a tip above what the log can DERIVE.
 *
 * The live wedge (2026-06-11): a boot restore installed tip_h=3,143,175
 * from coins-best while the validated header frontier (validate_headers'
 * own contiguous ok=1 log prefix) was 3,141,533 and the disk-backed index
 * extent died at 3,142,801. The durable finalized floor (3,143,171) then
 * FORCED the tip above what the index could back: post-restore integrity
 * reported 1,267 tip-window holes and the node crash-looped.
 *
 * This file is the gate, not a repair ladder:
 *
 *   utxo_recovery_header_frontier  — the Invariant A authority: the
 *       contiguous ok=1 prefix of validate_headers_log (DRY reader
 *       reducer_frontier_log_frontier). No log evidence => fail open.
 *   utxo_recovery_clamp_tip_to_header_frontier — committed tip =
 *       min(candidate, frontier), resolved hash-linked (pprev descent,
 *       falling back to the log's OWN recorded hash when the extent is
 *       torn) — never a height-only lookup.
 *   utxo_recovery_rewind_finalized_floor — a finalized floor ABOVE the
 *       frontier is provably unbackable; flip its ok=1 rows to ok=0
 *       (history preserved, status marks provenance). Loud.
 *
 * Bodies may legitimately lag the header frontier — the post-restore
 * integrity check counts block-index holes, not missing bodies, so
 * clamping to the validate_headers frontier is sufficient; body_fetch
 * refills. Kept as a dedicated seam file so utxo_recovery_restore.c stays
 * under the app/ file-size ceiling (E1).
 */

#include "services/utxo_recovery_service.h"
#include "validation/main_state.h"
#include "jobs/reducer_frontier.h"
#include "storage/progress_store.h"
#include "event/event.h"

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

#include "utxo_recovery_internal.h"

int utxo_recovery_finalized_served_floor(struct uint256 *hash_out,
                                         bool *have_hash_out)
{
    if (hash_out)
        memset(hash_out, 0, sizeof(*hash_out));
    if (have_hash_out)
        *have_hash_out = false;

    sqlite3 *pdb = progress_store_db();
    if (!pdb)
        LOG_RETURN(-1, "utxo_recovery",
                   "served_floor read skipped: progress_store not open");

    sqlite3_stmt *st = NULL;
    int floor = -1;
    progress_store_tx_lock();
    if (sqlite3_prepare_v2(
            pdb,
            "SELECT height, tip_hash FROM tip_finalize_log "
            "WHERE ok=1 ORDER BY height DESC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        progress_store_tx_unlock();
        LOG_RETURN(-1, "utxo_recovery",
                   "served_floor read prepare failed: %s",
                   sqlite3_errmsg(pdb));
    }
    if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW) {
        floor = sqlite3_column_int(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int blob_len = sqlite3_column_bytes(st, 1);
        if (hash_out && have_hash_out && blob && blob_len == 32) {
            memcpy(hash_out->data, blob, 32);
            *have_hash_out = true;
        }
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return floor;
}

bool utxo_recovery_header_frontier(int32_t *out_h)
{
    sqlite3 *pdb = progress_store_db();
    if (!pdb)
        return false;
    return reducer_frontier_log_frontier(pdb, "validate_headers_log",
                                         "validate_headers", out_h);
}

struct block_index *utxo_recovery_clamp_tip_to_header_frontier(
    struct utxo_recovery_ctx *ctx, struct block_index *candidate,
    const char *reason, int32_t *frontier_out, bool *clamped_out)
{
    if (clamped_out)
        *clamped_out = false;
    if (frontier_out)
        *frontier_out = -1;
    if (!ctx || !ctx->state || !candidate)
        return candidate;
    if (!reason)
        reason = "utxo_recovery";

    int32_t fh = 0;
    if (!utxo_recovery_header_frontier(&fh))
        return candidate;              /* fail-open: behave as today */
    if (frontier_out)
        *frontier_out = fh;
    if (candidate->nHeight <= fh)
        return candidate;              /* already derivable */

    /* 1) hash-linked descent: pprev only, never by-height. O(candidate -
     * frontier) pointer hops — boot/recovery only, never the hot tick. */
    struct block_index *walk = candidate;
    while (walk && walk->nHeight > fh)
        walk = walk->pprev;
    if (walk && walk->nHeight == fh) {
        if (clamped_out)
            *clamped_out = true;
        LOG_WARN("utxo_recovery",
                 "Invariant A clamp: restore candidate h=%d above validated "
                 "header frontier h=%d; committing hash-linked ancestor "
                 "(reason=%s)", candidate->nHeight, fh, reason);
        event_emitf(EV_RECOVERY_ACTION, 0,
                    "action=restore_tip_clamp candidate=%d frontier=%d "
                    "via=pprev reason=%s", candidate->nHeight, fh, reason);
        return walk;
    }

    /* 2) torn extent (pprev dies above fh — the live wedge): derive the
     * frontier tip from validate_headers_log's OWN hash (log-as-truth). */
    uint8_t lh[32];
    bool found = false;
    if (reducer_frontier_log_hash_at(progress_store_db(),
            "validate_headers_log", "hash", fh, lh, &found) && found) {
        struct uint256 fhash;
        memcpy(fhash.data, lh, 32);
        struct block_index *fb =
            block_map_find(&ctx->state->map_block_index, &fhash);
        if (fb && fb->nHeight == fh) {
            if (clamped_out)
                *clamped_out = true;
            LOG_WARN("utxo_recovery",
                     "Invariant A clamp: restore candidate h=%d above "
                     "validated header frontier h=%d and pprev chain is "
                     "torn; committing the frontier block from "
                     "validate_headers_log's own hash (reason=%s)",
                     candidate->nHeight, fh, reason);
            event_emitf(EV_RECOVERY_ACTION, 0,
                        "action=restore_tip_clamp candidate=%d frontier=%d "
                        "via=log_hash reason=%s",
                        candidate->nHeight, fh, reason);
            return fb;
        }
    }

    LOG_WARN("utxo_recovery",
             "Invariant A clamp: candidate h=%d not hash-linked to frontier "
             "h=%d and frontier block not in index; refusing install "
             "(reason=%s)", candidate->nHeight, fh, reason);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=restore_tip_refused candidate=%d frontier=%d "
                "reason=%s", candidate->nHeight, fh, reason);
    return NULL;
}

bool utxo_recovery_rewind_finalized_floor(int32_t frontier, int floor,
                                          const char *reason)
{
    if (floor <= frontier)
        return true;       /* floor is backable: anti-rewind holds */
    if (!reason)
        reason = "utxo_recovery";
    sqlite3 *pdb = progress_store_db();
    if (!pdb)
        LOG_RETURN(false, "utxo_recovery",
                   "floor rewind skipped: progress_store not open");

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(pdb,
        "UPDATE tip_finalize_log SET ok=0, status='floor_rewind' "
        "WHERE ok=1 AND height > ?", -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int(st, 1, frontier);
        ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:progress-kv-kernel-store
    }
    int rows = ok ? sqlite3_changes(pdb) : 0;
    if (st)
        sqlite3_finalize(st);
    progress_store_tx_unlock();
    if (!ok)
        LOG_RETURN(false, "utxo_recovery", "floor rewind UPDATE failed: %s",
                   sqlite3_errmsg(pdb));

    LOG_WARN("utxo_recovery",
             "FINALIZED FLOOR REWIND: floor h=%d is above the validated "
             "header frontier h=%d (unbackable); rewound %d ok=1 rows "
             "reason=%s", floor, frontier, rows, reason);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=finalized_floor_rewind floor=%d frontier=%d rows=%d "
                "reason=%s", floor, frontier, rows, reason);
    return ok;
}
