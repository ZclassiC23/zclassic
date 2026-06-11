/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
// one-result-type-ok:frontier-gate-primitives — these are the shared gate
// primitives (frontier read, candidate clamp, evidence-based floor rewind)
// consumed by the zcl_result-returning commit/restore surfaces in
// utxo_recovery_service.c / utxo_recovery_restore.c. Every refusal travels
// via structured LOG_WARN + EV_RECOVERY_ACTION, and the callers wrap the
// verdict in their own zcl_result.
/*
 * INVARIANT A — never INSTALL a tip above what can be DERIVED.
 *
 * The live wedge (2026-06-11): a boot restore installed tip_h=3,143,175
 * from a finalized-floor row recorded at h=3,143,171 (the row's own hash
 * resolved to a DIFFERENT height) on a detached 375-block index island
 * rooted at 3,142,801, while the hash-linked index extent died at
 * 3,141,533. Post-restore integrity reported 1,267 tip-window holes plus
 * 631 active-chain mismatches and the node crash-looped.
 *
 * The copy-prove of the first gate build taught the second lesson: the
 * LOG half alone is fabricatable. The wedged run had stamped a
 * tip_finalize status='anchor' row at 3,143,171 AND advanced every stage
 * cursor past it, so reducer_trusted_anchor accepted the anchor and the
 * validate_headers frontier read 3,143,206 — ABOVE the floor. Both
 * authorities must agree before an install:
 *
 *   LOG half   — utxo_recovery_header_frontier: the contiguous ok=1
 *       prefix of validate_headers_log (DRY reader
 *       reducer_frontier_log_frontier). No log evidence => fail open.
 *   INDEX half — utxo_recovery_block_trust_rooted: the candidate must be
 *       hash-linked (contiguous pprev) down to a trust root the node can
 *       actually serve from: genesis, or the compiled SHA3 UTXO anchor.
 *       A detached island above the anchor is not derivable state no
 *       matter what the logs claim.
 *
 * This file is the gate, not a repair ladder:
 *
 *   utxo_recovery_clamp_tip_to_header_frontier — committed tip =
 *       min(candidate, frontier), resolved hash-linked (pprev descent,
 *       falling back to the log's OWN recorded hash when the extent is
 *       torn) — never a height-only lookup — and refused outright when
 *       the resolved block is not trust-rooted.
 *   utxo_recovery_rewind_finalized_floor — a finalized floor that
 *       neither authority can back is provably unbackable; flip its
 *       ok=1 rows to ok=0 (history preserved, status marks provenance).
 *       Loud.
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
#include "chain/checkpoints.h"
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

bool utxo_recovery_block_trust_rooted(const struct block_index *bi)
{
    if (!bi || bi->nHeight < 0)
        return false;

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    const int32_t anchor = cp ? cp->height
                              : REDUCER_FRONTIER_TRUSTED_ANCHOR;

    /* Contiguous pprev descent: every hop must decrement the height by
     * exactly 1, so the walk terminates in at most bi->nHeight steps and
     * a height tear (the detached-island shape) is detected immediately.
     * Early-exit once the walk crosses the compiled SHA3 anchor — the
     * extent below it is attested by the checkpoint, so contiguity above
     * the anchor is the whole proof (caps the walk at ~tip-anchor hops). */
    const struct block_index *p = bi;
    while (p->pprev) {
        if (p->pprev->nHeight != p->nHeight - 1)
            return false;
        p = p->pprev;
        if (p->nHeight <= anchor)
            return true;
    }
    /* Root reached: genesis, or an extent rooted at/just above the
     * compiled anchor (snapshot-anchored nodes and the test segments
     * root exactly there). A root strictly above anchor+1 is a detached
     * island — not derivable state. */
    return p->nHeight == 0 || p->nHeight <= anchor + 1;
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
    if (candidate->nHeight <= fh) {
        /* Log-derivable — but the INDEX must agree (the 2026-06-11 wedge:
         * the log frontier was fabricated ABOVE the candidate, so the
         * height test alone would have installed a detached-island tip). */
        if (utxo_recovery_block_trust_rooted(candidate))
            return candidate;          /* derivable by BOTH authorities */
        LOG_WARN("utxo_recovery",
                 "Invariant A clamp: candidate h=%d is within the validated "
                 "header frontier h=%d but is NOT hash-linked to a trust "
                 "root (genesis / SHA3 anchor) — detached island; refusing "
                 "install (reason=%s)", candidate->nHeight, fh, reason);
        event_emitf(EV_RECOVERY_ACTION, 0,
                    "action=restore_tip_refused candidate=%d frontier=%d "
                    "via=unrooted reason=%s", candidate->nHeight, fh, reason);
        return NULL;
    }

    /* LOG-UNKNOWN carve-out: the stage logs are rolling windows. A
     * candidate BELOW the oldest row the log still covers cannot be
     * refuted by the log — only fail to be vouched for — so the log half
     * abstains and the index half (trust-rooted ancestry) decides alone.
     * Without this, rewinding fabricated anchor rows collapses the
     * frontier to the compiled SHA3 anchor and the clamp drags a
     * perfectly trust-rooted candidate 86K blocks down to it (the
     * 2026-06-11 copy-prove regression: candidate 3,137,373 was clamped
     * to 3,056,758 because validate_headers_log only covered
     * [3,142,802 .. 3,143,206]). */
    {
        int32_t log_lo = 0;
        bool lo_found = false;
        if (reducer_frontier_log_coverage_floor(progress_store_db(),
                "validate_headers_log", &log_lo, &lo_found) &&
            lo_found && candidate->nHeight < log_lo) {
            if (utxo_recovery_block_trust_rooted(candidate)) {
                LOG_INFO("utxo_recovery",
                         "Invariant A clamp: candidate h=%d is below the "
                         "validate_headers_log coverage window (oldest row "
                         "h=%d, frontier h=%d) — log abstains; committing "
                         "on trust-rooted index ancestry (reason=%s)",
                         candidate->nHeight, log_lo, fh, reason);
                return candidate;
            }
            LOG_WARN("utxo_recovery",
                     "Invariant A clamp: candidate h=%d is below the log "
                     "coverage window (oldest row h=%d) AND not trust-rooted; "
                     "refusing install (reason=%s)",
                     candidate->nHeight, log_lo, reason);
            event_emitf(EV_RECOVERY_ACTION, 0,
                        "action=restore_tip_refused candidate=%d frontier=%d "
                        "via=unrooted_below_log_window reason=%s",
                        candidate->nHeight, fh, reason);
            return NULL;
        }
    }

    /* 1) hash-linked descent: pprev only, never by-height. O(candidate -
     * frontier) pointer hops — boot/recovery only, never the hot tick. */
    struct block_index *walk = candidate;
    while (walk && walk->nHeight > fh)
        walk = walk->pprev;
    if (walk && walk->nHeight == fh &&
        utxo_recovery_block_trust_rooted(walk)) {
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
        if (fb && fb->nHeight == fh &&
            utxo_recovery_block_trust_rooted(fb)) {
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
             "h=%d via a trust-rooted chain and the frontier block does not "
             "resolve; refusing install (reason=%s)",
             candidate->nHeight, fh, reason);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=restore_tip_refused candidate=%d frontier=%d "
                "reason=%s", candidate->nHeight, fh, reason);
    return NULL;
}

int utxo_recovery_settle_finalized_floor(struct utxo_recovery_ctx *ctx,
                                         int scan_fallback_h,
                                         int served_floor,
                                         struct uint256 *served_hash,
                                         bool *have_served_hash)
{
    /* Only a floor BOTH Invariant A authorities can BACK is real:
     *
     *   LOG half   — the floor sits within the validated header frontier
     *                (abstains fail-open with no log evidence, or when
     *                the floor predates the log's rolling window);
     *   INDEX half — the floor row's OWN hash resolves in the index AT
     *                the recorded height, on a trust-rooted chain.
     *
     * The 2026-06-11 wedge floor failed the index half twice over: its
     * row hash mapped to a DIFFERENT height (3,143,175 vs 3,143,171) on
     * a detached 375-block island, while fabricated anchor rows had
     * pushed the LOG frontier to 3,143,206 so a log-only test passed and
     * the fabricated restored_height=floor was installed raw downstream
     * — crash-only reindex. Flip unbackable rows (loudest-first, history
     * preserved) until the floor is evidence again or stops outranking
     * scan_fallback. */
    if (!ctx || !ctx->state || !served_hash || !have_served_hash)
        return served_floor;

    int32_t fh = -1;
    bool have_fh = utxo_recovery_header_frontier(&fh);
    int32_t log_lo = 0;
    bool have_log_lo = false;
    {
        bool lo_found = false;
        if (reducer_frontier_log_coverage_floor(progress_store_db(),
                "validate_headers_log", &log_lo, &lo_found) && lo_found)
            have_log_lo = true;
    }
    int flipped_passes = 0;
    while (served_floor > scan_fallback_h) {
        struct block_index *fb = *have_served_hash
            ? block_map_find(&ctx->state->map_block_index, served_hash)
            : NULL;
        bool log_backed = !have_fh || served_floor <= fh ||
            (have_log_lo && served_floor < log_lo);
        bool index_backed = fb && fb->nHeight == served_floor &&
            utxo_recovery_block_trust_rooted(fb);
        if (log_backed && index_backed)
            break;  /* real authority — hold the floor */

        /* A floor above the log frontier rewinds to the frontier in one
         * pass; an index-unbackable row flips alone so any backable row
         * beneath it survives. */
        int32_t bound = (!log_backed && fh < served_floor)
            ? fh : served_floor - 1;
        if (!utxo_recovery_rewind_finalized_floor(
                bound, served_floor, "scan_fallback_guard"))
            break;  /* write failure: hold (fail closed) */
        flipped_passes++;
        int prev_floor = served_floor;
        served_floor = utxo_recovery_finalized_served_floor(
            served_hash, have_served_hash);
        if (served_floor >= prev_floor) {
            LOG_WARN("utxo_recovery",
                     "floor rewind made no progress (h=%d); holding floor",
                     served_floor);
            break;  /* defensive: never spin */
        }
    }
    if (flipped_passes > 0)
        LOG_WARN("utxo_recovery",
                 "finalized-floor rewind settled after %d pass(es): floor "
                 "now h=%d (scan_fallback h=%d, header frontier h=%d)",
                 flipped_passes, served_floor, scan_fallback_h,
                 have_fh ? fh : -1);
    return served_floor;
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
             "FINALIZED FLOOR REWIND: floor h=%d is above the highest "
             "servable height h=%d (unbackable evidence); rewound %d ok=1 "
             "rows reason=%s", floor, frontier, rows, reason);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=finalized_floor_rewind floor=%d frontier=%d rows=%d "
                "reason=%s", floor, frontier, rows, reason);
    return ok;
}
