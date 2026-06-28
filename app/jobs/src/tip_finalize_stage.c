/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * tip_finalize_stage — implementation. See jobs/tip_finalize_stage.h. */

#include "platform/time_compat.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_helpers.h"
#include "jobs/refold_progress.h"
#include "jobs/reducer_frontier.h"
#include "jobs/block_header_emit.h"
#include "tip_finalize_anchor_internal.h"
#include "tip_finalize_post_step.h"
#include "tip_finalize_log_store.h"
#include "script_validate_log_store.h"

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/log_throttle.h"
#include "util/stage.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STAGE_NAME "tip_finalize"


static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static tip_finalize_utxo_count_fn g_utxo_counter = NULL;
static void *g_utxo_counter_user = NULL;

static _Atomic uint64_t g_finalized_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_reorg_detected_total = 0;
static _Atomic uint64_t g_utxo_count_diverged_total = 0;
static _Atomic uint64_t g_precondition_failed_total = 0;
/* Lookahead successor (H+1) is not yet body-on-disk / script-valid: the
 * finalize of H is correctly DEFERRED (cursor held, JOB_IDLE), not skipped.
 * Distinct from g_precondition_failed_total, which counts ONLY the genuine
 * competing-fork skip (chainwork_not_greater). */
static _Atomic uint64_t g_successor_pending_total = 0;
/* Header-only canonical-successor finalizes (deadlock-cure step 3): N finalized
 * on N+1's HEADER witness because N+1's body/scripts were not yet pipelined.
 * A SUBSET of g_finalized_total (which counts these too); the complement of
 * g_successor_pending_total (the HOLD when N+1 is NOT yet a canonical successor).
 * One zcl_state call confirms the cure is firing live. */
static _Atomic uint64_t g_header_witness_total = 0;
static _Atomic uint64_t g_total_work_added_high = 0;
static _Atomic uint64_t g_total_work_added_low = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;

/* WHY the last idle/blocked tick idled. step_finalize has five distinct
 * JOB_IDLE exits; a held frontier is observationally identical across all
 * five in zcl_state, so one token + one counter per class makes the cause
 * one zcl_state call away (no code read needed to tell them apart).
 * TF_BLOCKED_AT_UV_FRONTIER is the HEALTHY at-tip steady state (waiting
 * for the next block), not a fault. */
enum tf_blocked_class {
    TF_BLOCKED_NONE = 0,
    TF_BLOCKED_UV_CURSOR_GAP,     /* cursor ran ahead of utxo_apply */
    TF_BLOCKED_AT_UV_FRONTIER,    /* caught up; next block not applied yet */
    TF_BLOCKED_UV_ROW_MISSING,    /* utxo_apply log row absent at cursor */
    TF_BLOCKED_LOOKAHEAD_MISSING, /* chain window has no successor H+1 */
    TF_BLOCKED_TIP_MISSING,       /* chain window has no block at H */
    TF_BLOCKED_SUCCESSOR_PENDING, /* successor known but not yet finalizable
                                   * (detail in last_precondition_reason) */
    TF_BLOCKED_CLASS_N
};
static const char *const k_tf_blocked_name[TF_BLOCKED_CLASS_N] = {
    "", "uv_cursor_gap", "at_utxo_frontier", "utxo_apply_row_missing",
    "lookahead_tip_missing", "current_tip_missing", "successor_pending",
};
static _Atomic int      g_last_blocked_class = TF_BLOCKED_NONE;
static _Atomic uint64_t g_blocked_class_total[TF_BLOCKED_CLASS_N];

static void tf_mark_blocked(enum tf_blocked_class cls)
{
    atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
    atomic_store(&g_last_blocked_class, (int)cls);
    if (cls > TF_BLOCKED_NONE && cls < TF_BLOCKED_CLASS_N)
        atomic_fetch_add(&g_blocked_class_total[cls], 1);
}
static uint8_t         g_last_advance_hash[32];
static zcl_mutex_t     g_last_advance_hash_mu;

/* Last specific precondition that blocked tip_finalize. The persisted
 * tip_finalize_log status column stays the generic "precondition_failed"
 * token (downstream + tests match on it); this names WHICH check failed so
 * a script-validation stall is not masked. Updated on EVERY blocked tick for
 * zcl_state freshness even when the WARN is throttled (below). Guarded by
 * g_block_reason_mu. */
static _Atomic int64_t  g_last_precondition_height = -1;
static char             g_last_precondition_reason[40] = "";
static zcl_mutex_t      g_block_reason_mu;

/* CS-F1/F3 WARN-storm throttle. reducer_drain_to_convergence runs up to 4096
 * rounds per activation kick, so a per-tick WARN on a held frontier repeats
 * the SAME line millions of times in minutes. Emit only on a pair TRANSITION
 * (reporting the prior pair's suppressed count) or as a 300 s keep-alive with
 * the running count; suppressed calls still count. Both throttles are the
 * shared log_throttle de-storm primitive: F1 (precondition) keys on a
 * (height, reason-string) tuple so it uses the boolean-change entry point and
 * is updated under g_block_reason_mu while its repeat counter is read
 * lock-free by the dump; F3 (cursor-gap) keys on the (cursor_in, utxo_apply)
 * height pair and is touched only by the single-threaded step path. */
static struct log_throttle g_precondition_throttle = LOG_THROTTLE_INIT;
static struct log_throttle g_cursor_gap_throttle = LOG_THROTTLE_INIT;

static void update_last_advance(int height, const uint8_t hash[32])
{
    atomic_store(&g_last_advance_height, (int64_t)height);
    zcl_mutex_lock(&g_last_advance_hash_mu);
    memcpy(g_last_advance_hash, hash, 32);
    zcl_mutex_unlock(&g_last_advance_hash_mu);
}

static bool get_last_advance(int64_t *height, uint8_t hash[32])
{
    *height = atomic_load(&g_last_advance_height);
    if (*height < 0) return false;
    zcl_mutex_lock(&g_last_advance_hash_mu);
    memcpy(hash, g_last_advance_hash, 32);
    zcl_mutex_unlock(&g_last_advance_hash_mu);
    return true;
}

static bool publish_resolved_durable_tip(sqlite3 *db, const char *reason)
{
    int h = -1;
    uint8_t hash[32];
    if (!tip_finalize_stage_resolve_durable_tip(db, &h, hash))
        return false;
    update_last_advance(h, hash);
    LOG_INFO("tip_finalize",
             "[tip_finalize] authority publish durable h=%d reason=%s",
             h, reason ? reason : "");
    return true;
}

static bool has_no_durable_tip_history(sqlite3 *db)
{
    uint64_t cursor = 0;
    if (!stage_cursor_read_or_zero(db, STAGE_NAME, STAGE_NAME, &cursor))
        return false;
    return cursor == 0 &&
           stage_log_row_count(db, STAGE_NAME, "tip_finalize_log") <= 0;
}

static bool hydrate_stage_cursor_from_store(sqlite3 *db, stage_t *stage,
                                            const char *reason)
{
    if (!stage)
        return true;
    uint64_t cursor = 0;
    if (!stage_cursor_read_or_zero(db, STAGE_NAME, STAGE_NAME, &cursor))
        return false;
    if (!stage_set_cursor(stage, db, cursor)) {
        LOG_WARN("tip_finalize",
                 "[tip_finalize] cursor hydrate failed cursor=%llu reason=%s",
                 (unsigned long long)cursor, reason ? reason : "");
        return false;
    }
    return true;
}

static void publish_resolved_or_fresh_tip(
    sqlite3 *db, const struct block_index *existing_tip, const char *reason)
{
    if (publish_resolved_durable_tip(db, reason))
        return;
    if (existing_tip && existing_tip->phashBlock &&
        has_no_durable_tip_history(db)) {
        update_last_advance(existing_tip->nHeight,
                            existing_tip->phashBlock->data);
        LOG_INFO("tip_finalize",
                 "[tip_finalize] authority publish fresh h=%d reason=%s",
                 existing_tip->nHeight, reason ? reason : "");
    }
}

/* Recompute H* (the deepest provably-consistent height) from the durable
 * progress.kv state and publish it into the external provable-tip cache.
 *
 * Called at exactly two chokepoints — the finalize ADVANCE (step_finalize,
 * once per finalized block) and the reorg REWIND
 * (rewind_cursor_if_active_chain_reorged, once per detected reorg) — both of
 * which already hold progress_store_tx_lock() (acquired by
 * tip_finalize_stage_step_once). reducer_frontier_compute_hstar is PURE
 * SELECT-only and REQUIRES the caller hold that lock, so this must never run
 * off those two paths. Cost is one O(cursor-anchor) fold per RARE event, never
 * per RPC. On a read error it leaves the cache unchanged (logs, does not crash)
 * — a stale-but-bounded H* is strictly better than serving -1 or a wrong tip.
 *
 * CALLER MUST hold progress_store_tx_lock(). */
static void tf_refresh_provable_tip(sqlite3 *db)
{
    if (!db)
        return;
    int32_t hs = 0, sf = 0;
    if (!reducer_frontier_compute_hstar(db, &hs, &sf)) {
        LOG_WARN("tip_finalize",
                 "[tip_finalize] provable-tip refresh: compute_hstar failed "
                 "(cache holds prior H*)");
        return;
    }
    reducer_frontier_provable_tip_set(hs);
}


/* Cross-TU seam for tip_finalize_anchor.c (tip_finalize_anchor_internal.h):
 * the anchor/seed TU reads the live stage handle at call time and publishes
 * the served tip through the same single update path as the step body. */
stage_t *tip_finalize_stage_handle(void)
{
    return g_stage;
}

void tip_finalize_publish_last_advance(int height, const uint8_t hash[32])
{
    update_last_advance(height, hash);
}

static int reorg_depth_from(struct block_index *old_tip,
                            struct block_index *new_tip)
{
    int depth = 0;
    struct block_index *p = new_tip;
    while (p && p->nHeight > old_tip->nHeight) {
        p = p->pprev;
        depth++;
    }
    while (p && old_tip && p != old_tip) {
        p = p->pprev;
        old_tip = old_tip->pprev;
        depth++;
    }
    return depth > 0 ? depth : 1;
}

/* Returns the specific precondition that fails, or NULL if all pass.
 * The derived bool (reason == NULL) is the finalize-gate verdict. Set:
 *   block_missing       — no candidate block_index for this height
 *   have_data_missing   — body not on disk (BLOCK_HAVE_DATA clear)
 *   not_script_valid    — validity below BLOCK_VALID_SCRIPTS (script stall)
 *   not_header_valid    — validity below BLOCK_VALID_HEADER */
static const char *precondition_block_reason(const struct block_index *bi)
{
    if (!bi) return "block_missing";
    if (!(bi->nStatus & BLOCK_HAVE_DATA)) return "have_data_missing";
    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS)
        return "not_script_valid";
    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_HEADER)
        return "not_header_valid";
    return NULL;
}

/* HEADER-ONLY canonical-successor witness (deadlock-cure step 3,
 * docs/work/sync-deadlock-cure-2026-06-27.md).
 *
 * Returns true iff new_tip (N+1) is sufficient HEADER-level evidence that
 * old_tip (N) is on the canonical most-work chain — WITHOUT requiring N+1's
 * body / scripts / utxo verdict. N is finalizable on its OWN proven verdict
 * (utxo_apply_log_at(N).ok==1, already checked at the upstream.ok gate); N+1 is
 * consulted only as the most-work successor that pins N to the canonical chain.
 *
 * All five checks are HEADER-only and reorg-safe:
 *   - new_tip is not a failed block (block_has_any_failure clear) — never
 *     finalize past a consensus-invalid successor;
 *   - new_tip carries >= BLOCK_VALID_HEADER (Equihash PoW verified);
 *   - new_tip->pprev == old_tip BY BLOCK HASH (canonical parent; a duplicate
 *     same-hash object must not defeat it — the bde617a7e lesson — so this is
 *     hash identity, never pointer identity);
 *   - new_tip->nChainWork strictly greater than old_tip's;
 *   - new_tip is best_header's own ancestor at its height, compared BY HASH
 *     (on the most-work header chain).
 * N+1 is NEVER finalized or served on this evidence: tip_finalize still requires
 * N+1's own ok=1 before advancing past it, and H* is capped by the contiguous
 * ok=1 prefix (reducer_frontier), so it climbs only to N. */
static bool is_canonical_header_successor(struct block_index *old_tip,
                                          struct block_index *new_tip,
                                          struct block_index *best_header)
{
    if (!old_tip || !new_tip || !best_header)
        return false;
    if (!old_tip->phashBlock || !new_tip->phashBlock)
        return false;
    if (block_has_any_failure(new_tip))
        return false;
    if ((new_tip->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_HEADER)
        return false;
    if (!new_tip->pprev || !new_tip->pprev->phashBlock ||
        !uint256_eq(new_tip->pprev->phashBlock, old_tip->phashBlock))
        return false;
    if (arith_uint256_compare(&new_tip->nChainWork, &old_tip->nChainWork) <= 0)
        return false;
    if (new_tip->nHeight > best_header->nHeight)
        return false;
    struct block_index *anc =
        block_index_get_ancestor(best_header, new_tip->nHeight);
    if (!anc || !anc->phashBlock ||
        !uint256_eq(anc->phashBlock, new_tip->phashBlock))
        return false;
    return true;
}

/* Authoritative, reorg-safe script-validity check for the finalize gate.
 *
 * The block_index BLOCK_VALID_SCRIPTS bit consulted by precondition_block_reason
 * is a best-effort in-RAM mirror (set + emitted opportunistically by
 * script_validate_stage) that can drift CLEAR on a restored datadir — stranding
 * tip_finalize on a block whose scripts the reducer DID validate. The reducer's
 * script_validate_log is the authority: ok=1 is written only after the consensus
 * verifier passed every input (no assumevalid/checkpoint shortcut).
 *
 * The log is keyed by height, so a height-only read is reorg-UNSAFE: an orphaned
 * block that once held this height left an ok=1 row under a DIFFERENT hash.
 * We therefore trust ok=1 ONLY when the row's block_hash equals the hash of the
 * block being finalized — the same hash-identity guard reducer_read_back_verdict
 * applies to tip_finalize_log. Rows predating the block_hash column are NULL and
 * are never trusted. Returns 1 iff ok=1 AND block_hash == want_hash; else 0. */
static int finalize_script_log_ok(sqlite3 *db, int height, const struct uint256 *want_hash)
{
    if (!db || !want_hash)
        return 0;
    struct script_validate_verdict_row row;
    if (script_validate_log_verdict_at(db, height, &row) != 1)
        return 0;  /* absent row or store error (logged by the accessor) */
    return (row.ok == 1 && row.has_block_hash &&
            uint256_eq(&row.block_hash, want_hash)) ? 1 : 0;
}

static void record_precondition_block(int height, const char *reason)
{
    const char *r = reason ? reason : "";
    int64_t now = platform_time_wall_unix();
    uint64_t shown = 0;
    zcl_mutex_lock(&g_block_reason_mu);
    bool changed = (int64_t)height != atomic_load(&g_last_precondition_height)
                   || strcmp(r, g_last_precondition_reason) != 0;
    atomic_store(&g_last_precondition_height, (int64_t)height);
    snprintf(g_last_precondition_reason, sizeof g_last_precondition_reason, "%s", r);
    bool emit = log_throttle_should_emit_changed(&g_precondition_throttle,
                                                 changed, now, 300, &shown);
    zcl_mutex_unlock(&g_block_reason_mu);
    if (emit)
        LOG_WARN("tip_finalize", "[tip_finalize] precondition_failed height=%d reason=%s repeats=%llu",
                 height, r, (unsigned long long)shown);
}

static bool finalized_row_active_match(sqlite3 *db, int row_height,
                                       bool *out_known, bool *out_matches)
{
    *out_known = false;
    *out_matches = false;
    struct finalized_tip_row row;
    if (!finalized_tip_row_at(db, row_height, &row))
        return false;
    if (!row.found || !row.ok || !row.has_tip_hash)
        return true;
    /* Skip tip SEED rows. An anchor row stores the block's OWN hash (row H ->
     * hash H), not the finalized lookahead convention (row H -> hash H+1) that
     * this match assumes. Comparing an anchor's hash(H) to active_chain_at(H+1)
     * ALWAYS mismatches, which false-detects a reorg and rewinds the cursor
     * back onto the seed forever (a finalize-frontier oscillation). A genuine
     * reorg at/around the seed is still caught by the real finalized rows. */
    if (row.is_anchor)
        return true;  /* out_known stays false → no-op for the rewind scan */

    struct main_state *ms = g_ms;
    struct block_index *active = ms ? active_chain_at(&ms->chain_active, row_height + 1) : NULL;
    if (!active || !active->phashBlock)
        return true;
    *out_known = true;
    *out_matches = uint256_eq(&row.tip_hash, active->phashBlock);
    return true;
}

static bool rewind_cursor_if_active_chain_reorged(sqlite3 *db)
{
    if (!g_stage || !g_ms)
        return true;

    uint64_t cursor = 0;
    if (!stage_cursor_read_or_zero(db, STAGE_NAME, STAGE_NAME, &cursor))
        return false;
    if (cursor == 0)
        return true;
    if (cursor > (uint64_t)INT32_MAX) {
        LOG_WARN("tip_finalize", "[tip_finalize] reorg rewind cursor too large: %llu", (unsigned long long)cursor);
        return false;
    }

    bool known = false;
    bool matches = false;
    if (!finalized_row_active_match(db, (int)cursor - 1, &known, &matches))
        return false;
    if (!known || matches)
        return true;

    uint64_t rewind_to = 0;
    for (int h = (int)cursor - 2; h >= 0; h--) {
        known = false;
        matches = false;
        if (!finalized_row_active_match(db, h, &known, &matches))
            return false;
        if (known && matches) {
            rewind_to = (uint64_t)h + 1u;
            break;
        }
    }
    if (rewind_to == cursor)
        return true;

    if (!stage_set_cursor(g_stage, db, rewind_to)) {
        LOG_WARN("tip_finalize", "[tip_finalize] reorg rewind failed from=%llu to=%llu", (unsigned long long)cursor, (unsigned long long)rewind_to);
        return false;
    }

    /* LOWER the external provable-tip cache (H*) on the reorg rewind. This is
     * the #1 site: stage_set_cursor just dropped the tip_finalize cursor, but
     * g_last_advance_height (and thus active_chain_height) is raise-only and
     * stays stale-high until a new finalize republishes. Without this refresh
     * the external readers (getblockcount / P2P start_height) would serve an
     * unproven height across the reorg window. We hold progress_store_tx_lock
     * here (tip_finalize_stage_step_once), and this is the SAME chokepoint every
     * other unwind path (process_block_invalidate, utxo_apply_delta_reorg,
     * process_block_self_heal) flows through via reducer_kick -> tip_finalize
     * drain, so refreshing here lowers the cache for ALL of them. */
    tf_refresh_provable_tip(db);

    atomic_fetch_add(&g_reorg_detected_total, 1);
    atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
    event_emitf(EV_BLOCK_REJECTED, 0,
                "tip_finalize reorg_cursor_rewind from=%llu to=%llu",
                (unsigned long long)cursor, (unsigned long long)rewind_to);
    return true;
}

static bool live_utxo_count_after(int height_after, int64_t *out_count)
{
    *out_count = -1;
    if (!g_utxo_counter)
        return true;
    return g_utxo_counter(height_after, out_count, g_utxo_counter_user);
}

static job_result_t step_finalize(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t uv_cursor = 0;
    if (!stage_cursor_read_or_zero(db, "utxo_apply", STAGE_NAME,
                                   &uv_cursor))
        return JOB_FATAL;
    if ((uint64_t)next_h > uv_cursor) {
        int64_t now = platform_time_wall_unix();
        /* Key the de-storm on the (cursor_in, utxo_apply) height pair: a
         * change in either height re-emits immediately, otherwise a 300 s
         * keep-alive. Both fit 32 bits (block heights), so the pack is
         * lossless and key-equality is exact. */
        uint64_t key = ((uint64_t)(uint32_t)next_h << 32)
                       | (uint32_t)uv_cursor;
        uint64_t shown = 0;
        if (log_throttle_should_emit(&g_cursor_gap_throttle, key, now, 300,
                                     &shown))
            LOG_WARN("tip_finalize",
                "[tip_finalize] cursor_in=%d exceeds utxo_apply cursor=%llu repeats=%llu",
                next_h, (unsigned long long)uv_cursor, (unsigned long long)shown);
        tf_mark_blocked(TF_BLOCKED_UV_CURSOR_GAP);
        return JOB_IDLE;
    }
    if ((uint64_t)next_h >= uv_cursor) {
        tf_mark_blocked(TF_BLOCKED_AT_UV_FRONTIER);
        return JOB_IDLE;
    }

    struct utxo_apply_row upstream;
    int found = utxo_apply_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        tf_mark_blocked(TF_BLOCKED_UV_ROW_MISSING);
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        struct arith_uint256 zero;
        arith_uint256_set_zero(&zero);
        if (!log_insert(db, next_h, "upstream_failed", false, &zero, -1, 0, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_upstream_failed_total, 1);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    struct block_index *old_tip = active_chain_at(&ms->chain_active, next_h);
    struct block_index *new_tip = active_chain_at(&ms->chain_active, next_h + 1);
    /* WINDOW-SLOT SELF-HEAL. The active-chain window's lower slot at next_h can
     * read NULL while next_h is genuinely on the finalized chain: a blocks-less
     * snapshot boot retracts the window to the seed, and as the body-dependent
     * stages extend it UP the seed-region slot is left empty even though the
     * authority still names next_h finalized. active_chain_at then returns NULL
     * → tip_finalize idles on current_tip_missing forever and H* pins at the
     * seed even though utxo_apply is folding ok=1 rows past it (observed:
     * ua_cursor climbing, ua_ok=1, tf_blocked=current_tip_missing). Re-resolve
     * the slot from the durable finalized-hash table + the block map (the SAME
     * authority active_chain_tip() uses), so finalize can proceed; a real
     * absence (no finalized row / not in map) still falls through to the
     * blocked-IDLE below. Read-only on the window. */
    if (!old_tip) {
        struct uint256 oh;
        if (tip_finalize_stage_block_hash_at(db, next_h, oh.data))
            old_tip = block_map_find(&ms->map_block_index, &oh);
    }
    if (!new_tip) {
        struct uint256 nh;
        if (tip_finalize_stage_block_hash_at(db, next_h + 1, nh.data))
            new_tip = block_map_find(&ms->map_block_index, &nh);
    }
    /* HEADER-CHAIN SELF-HEAL (deadlock-cure step 3). The lookahead successor
     * N+1 (and, on a deeply-retracted window, even N) can be genuinely on the
     * canonical most-work chain yet ABSENT from BOTH the active-chain window
     * (the have-data extender stalled at the body frontier a block below it —
     * the live 3162166 wedge: new_tip=active_chain_at(N+1)=NULL) AND the
     * finalized-hash table (N+1 is not finalized — enabling that is the whole
     * point). Resolve it from the best-header ancestry, the same slot-
     * independent authority validate_headers_stage's vh_resolve_bi uses. This is
     * the missing link: without it, a body-level lookahead miss returns JOB_IDLE
     * here (TF_BLOCKED_LOOKAHEAD_MISSING) BEFORE the canonical-successor gate
     * below can ever run. Read-only on the window; the gate re-proves PoW +
     * most-work + ancestry before any finalize, and a genuine absence still
     * falls through to the blocked-IDLE returns. */
    if (ms->pindex_best_header) {
        if (!old_tip && next_h <= ms->pindex_best_header->nHeight)
            old_tip = block_index_get_ancestor(ms->pindex_best_header, next_h);
        if (!new_tip && next_h + 1 <= ms->pindex_best_header->nHeight)
            new_tip = block_index_get_ancestor(ms->pindex_best_header,
                                               next_h + 1);
    }
    if (!new_tip) {
        tf_mark_blocked(TF_BLOCKED_LOOKAHEAD_MISSING);
        return JOB_IDLE;
    }
    if (!old_tip) {
        tf_mark_blocked(TF_BLOCKED_TIP_MISSING);
        return JOB_IDLE;
    }

    struct arith_uint256 work_delta;
    arith_uint256_set_zero(&work_delta);

    /* Canonical-parent check BY BLOCK HASH, not pointer identity. old_tip and
     * new_tip can now be resolved from DIFFERENT authorities (active-chain
     * window vs best-header ancestry vs block map via the self-heals above), so
     * a duplicate same-hash block_index object would make a pointer compare
     * false-detect a reorg and write an ok=0 row that caps H* a block below the
     * truth — the exact class bde617a7e fixed in the window extender. Contiguity
     * is the consensus property child.hashPrevBlock == parent.GetBlockHash();
     * test THAT. A genuine fork (different parent hash, or a NULL/severed pprev)
     * still takes the reorg_detected ok=0 advance below — invariant 4 intact. */
    if (!new_tip->pprev || !new_tip->pprev->phashBlock || !old_tip->phashBlock ||
        !uint256_eq(new_tip->pprev->phashBlock, old_tip->phashBlock)) {
        int depth = reorg_depth_from(old_tip, new_tip);
        if (!log_insert(db, next_h, "reorg_detected", false, &work_delta, -1, depth, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_reorg_detected_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "tip_finalize reorg_detected height=%d depth=%d", next_h, depth);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    /* Reaching here we KNOW new_tip->pprev == old_tip (the structural-reorg
     * branch above returned first) — a LINEAR one-block lookahead extension.
     * Two outcomes are NOT the same and must be handled differently:
     *
     *  (a) TRANSIENT (block_missing / have_data_missing / not_script_valid /
     *      not_header_valid): the successor H+1 has simply not finished the
     *      body_persist -> script_validate -> utxo_apply pipeline yet. H is
     *      genuinely finalizable; we are only missing its lookahead witness.
     *      We must NOT advance the cursor — advancing strands H forever
     *      because anchor_cursor_to_authority is MONOTONIC (never pulls back),
     *      producing a finalize-frontier oscillation. Return JOB_IDLE:
     *      cursor unchanged, framework rolls back the txn (no junk row), and
     *      the frontier retries on the next tick once the successor lands.
     *
     *  (b) chainwork_not_greater: a LINEAR successor that adds no work. On a
     *      valid PoW chain GetBlockProof() is strictly >= 1 per block, so this
     *      is unreachable for a real header; it appears only from a
     *      corrupt/zero-work synthetic candidate that must NEVER finalize.
     *      Persist the precondition_failed ok=0 row, count it, emit the
     *      reject, and ADVANCE past it so the pipeline cannot deadlock on an
     *      unfinalizable lighter candidate. */
    const char *transient_reason = precondition_block_reason(new_tip);
    /* not_script_valid from the block_index mirror is NOT authoritative: the bit
     * can drift CLEAR on a restored datadir while the reducer's hash-bound
     * script_validate_log still proves THIS block's scripts were verified. When
     * (and only when) the reason is the script-validity level, the candidate is
     * not a failed block, and the log carries a hash-matched ok=1 row, treat the
     * scripts as valid and heal the in-RAM bit so other nStatus readers + the
     * persisted projection converge. The have_data_missing / not_header_valid /
     * block_missing reasons are unchanged — only the script-validity source is
     * rerouted to the authority. */
    if (transient_reason != NULL &&
        strcmp(transient_reason, "not_script_valid") == 0 &&
        !(new_tip->nStatus & BLOCK_FAILED_MASK) &&
        finalize_script_log_ok(db, new_tip->nHeight, new_tip->phashBlock) == 1) {
        new_tip->nStatus = (new_tip->nStatus & ~(unsigned)BLOCK_VALID_MASK)
                           | BLOCK_VALID_SCRIPTS;
        block_index_emit_header_event(new_tip, "tip_finalize_selfheal",
                                      NULL, NULL);
        transient_reason = NULL;
    }
    if (transient_reason != NULL) {
        /* HEADER-ONLY FINALIZE (deadlock-cure step 3). N's own upstream verdict
         * is ok=1 (the upstream.ok gate above). If N+1 is a CANONICAL header
         * successor — PoW-verified, strictly-greater-work, best-header ancestor,
         * canonical parent — then N is provably on the most-work chain and is
         * finalizable NOW using N+1 as a HEADER witness, even though N+1's body /
         * scripts / utxo verdict are not yet pipelined. Requiring them here is
         * the body-level lookahead that deadlocks the frontier (live 3162166):
         * the downstream stages cannot fold N+1 until the window exposes it, and
         * the window will not pass a frozen finalize tip. Clear transient_reason
         * and FALL THROUGH to the normal finalize tail — it consumes only N+1's
         * HEADER fields (nChainWork, phashBlock, nHeight), so the durable row
         * (lookahead convention, hash(N+1), status "finalized"), the reorg-rewind
         * scan, and the H* refresh are exactly the normal-finalize semantics
         * (no anchor row, so no reorg-rewind blind spot). H* still climbs
         * only to N (utxo_apply caps the contiguous ok=1 prefix), so N+1 is never
         * served. The window move + served-tip publish in the tail are
         * HAVE_DATA-gated, so a still-bodiless N+1 finalizes N (H* += 1) WITHOUT
         * pinning a bodiless slot or advertising an unbodied active-chain tip. */
        if (is_canonical_header_successor(old_tip, new_tip,
                                          ms->pindex_best_header)) {
            atomic_fetch_add(&g_header_witness_total, 1);
            transient_reason = NULL;  /* proceed to the normal finalize tail */
        } else {
            atomic_fetch_add(&g_successor_pending_total, 1);
            record_precondition_block(next_h, transient_reason);
            tf_mark_blocked(TF_BLOCKED_SUCCESSOR_PENDING);
            /* Genuinely not a canonical successor (no/!PoW header, off
             * best_header, or not greater work): hold H until its successor is
             * ready. No DB row, no cursor move. */
            return JOB_IDLE;
        }
    }
    if (arith_uint256_compare(&new_tip->nChainWork, &old_tip->nChainWork) <= 0) {
        if (!log_insert(db, next_h, "precondition_failed", false, &work_delta, -1, 0, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_precondition_failed_total, 1);
        record_precondition_block(next_h, "chainwork_not_greater");
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "tip_finalize precondition_failed height=%d reason=%s",
                    next_h, "chainwork_not_greater");
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    arith_uint256_sub(&work_delta, &new_tip->nChainWork, &old_tip->nChainWork);

    int64_t utxo_size_after = -1;
    if (!live_utxo_count_after(next_h + 1, &utxo_size_after))
        return JOB_FATAL;
    if (utxo_size_after >= 0) {
        int64_t spent = 0, added = 0;
        if (!utxo_apply_sums_through(db, next_h, &spent, &added))
            return JOB_FATAL;
        int64_t expected = added - spent;
        if (utxo_size_after != expected) {
            if (!log_insert(db, next_h, "utxo_count_diverged", false,
                            &work_delta, utxo_size_after, 0, NULL))
                return JOB_FATAL;
            atomic_fetch_add(&g_utxo_count_diverged_total, 1);
            event_emitf(EV_BLOCK_REJECTED, 0,
                        "tip_finalize utxo_count_diverged height=%d live=%lld expected=%lld",
                        next_h, (long long)utxo_size_after, (long long)expected);
            c->cursor_out = c->cursor_in + 1;
            return JOB_ADVANCED;
        }
    }

    if (!log_insert(db, next_h, "finalized", true, &work_delta,
                    utxo_size_after, 0, new_tip->phashBlock))
        return JOB_FATAL;

    int64_t published_before = atomic_load(&g_last_advance_height);
    /* Advertise the served-tip authority (g_last_advance_height ->
     * active_chain_height / active_chain_tip) ONLY when new_tip has its body: a
     * header-only finalize of N on a still-bodiless N+1 must not move the
     * active-chain tip onto N+1. H* (getblockcount / P2P start_height) is
     * published separately by tf_refresh_provable_tip below and still climbs to
     * N. For a normal finalize new_tip always has BLOCK_HAVE_DATA (precondition
     * passed), so this is a no-op there. */
    bool publish = (new_tip->nStatus & BLOCK_HAVE_DATA) &&
                   (published_before < 0 || new_tip->nHeight >= (int)published_before);

    /* Durable row first; then move the local chain[] cache/window — EXCEPT
     * during a from-genesis refold. There the stage extend already widened the
     * window to best_header, so retracting it to next_h+1 here forces the next
     * stage step to re-walk ~3.1M pprev nodes (active_chain_fill_window) — the
     * dominant cold-refold cost (~3 blk/s, CPU cache-bound). The served-tip
     * AUTHORITY is g_last_advance_height (published by update_last_advance
     * below), NOT c->height, so leaving the window wide keeps active_chain_height
     * / getblockcount correct; only active_chain_at visibility stays wide, which
     * is safe during a refold — the stages read at/below the fold frontier and
     * the watchdog/reconcile are suspended (refold_in_progress()). Normal sync
     * keeps the retraction (the window must track the finalized frontier). See
     * docs/work/refold-fold-rate-bottlenecks.md (#1). */
    if (!refold_in_progress() && (new_tip->nStatus & BLOCK_HAVE_DATA) &&
        !active_chain_move_window_tip(&ms->chain_active, new_tip)) { // one-write-path-ok:reducer-tip-authority
        /* HAVE_DATA gate (deadlock-cure step 3): never move the window onto a
         * body-missing N+1 — that is the bodiless-slot pin the have-data extender
         * is built to refuse (false-reorg cascade). When N+1's body is present
         * (the live 3162167 case, and ALWAYS for a normal finalize) the window
         * advances so the downstream body stages can fold N+1 and the frontier
         * cascades; when it is absent we still finalize N (H* += 1) and the body
         * is fetched by gap_fill / stall recovery before the window catches up. */
        LOG_WARN("tip_finalize", "[tip_finalize] chain_active set_tip failed height=%d", next_h);
        return JOB_FATAL;
    }

    atomic_fetch_add(&g_finalized_total, 1);
    atomic_fetch_add(&g_total_work_added_low, arith_uint256_get_low64(&work_delta));
    atomic_fetch_add(&g_total_work_added_high,
                     ((uint64_t)work_delta.pn[3] << 32) | work_delta.pn[2]);
    if (publish) {
        tip_finalize_run_post_finalize(new_tip);
        /* Publish the SELF-CONSISTENT authority pair: the served tip block's
         * OWN height with its OWN hash — derive the label from the block,
         * never the cursor. Publishing (next_h, hash(next_h+1)) makes
         * active_chain_height() == active_chain_tip()->nHeight - 1 at the
         * finalize frontier, and accept_block_header's label-trust install
         * turns that inconsistent pair into a -1 height splice across the
         * whole header graph when a peer re-delivers the tip header. */
        update_last_advance(new_tip->nHeight, new_tip->phashBlock->data);
    }
    /* Refresh the EXTERNAL provable-tip cache (H*) on the finalize advance.
     * The durable "finalized" ok=1 row was inserted above (log_insert), and we
     * still hold progress_store_tx_lock (tip_finalize_stage_step_once), so the
     * recompute reads the just-committed prefix. One O(n) fold per finalized
     * block — never per RPC. Runs on EVERY finalized advance (not only when
     * `publish` fires): H* is derived from durable state and must not stay stale
     * high even on a non-republishing re-finalize. */
    tf_refresh_provable_tip(db);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

static bool is_authoritative(void)
{
    return true;
}

static int64_t get_height(void)
{
    return tip_finalize_stage_last_height();
}

static bool get_hash(uint8_t hash[32])
{
    int64_t h;
    return get_last_advance(&h, hash);
}

bool tip_finalize_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("tip_finalize", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("tip_finalize", "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    zcl_mutex_init(&g_last_advance_hash_mu);
    zcl_mutex_init(&g_block_reason_mu);
    g_ms = ms;

    struct block_index *existing_tip = active_chain_cached_tip(&ms->chain_active);

    struct active_chain_authority auth = { .get_height = get_height,
        .get_hash = get_hash, .is_authoritative = is_authoritative };
    active_chain_register_authority(&auth);
    active_chain_register_block_map(&ms->map_block_index);

    if (g_stage != NULL) {
        if (existing_tip && existing_tip->phashBlock &&
            !tip_finalize_anchor_cursor_to_authority(db, existing_tip->nHeight,
                existing_tip->phashBlock->data, false, false, "init_existing_tip_reanchor")) {
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        if (!hydrate_stage_cursor_from_store(db, g_stage,
                                             "init_existing_tip_reanchor")) {
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        publish_resolved_or_fresh_tip(db, existing_tip,
                                      "init_existing_tip_reanchor");
        pthread_mutex_unlock(&g_lock);
        return true;
    }

    if (!ensure_log_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_finalize, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("tip_finalize", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    /* The active-chain cache is a served-tip authority, not reducer progress.
     * On recovered datadirs it may sit above H-star/coins while upstream stage
     * cursors are deliberately clamped for repair. Publish the tip_finalize
     * authority cursor, but only explicit seed anchors may align upstream
     * reducer cursors. */
    if (existing_tip && existing_tip->phashBlock &&
        !tip_finalize_anchor_cursor_to_authority(db, existing_tip->nHeight,
                existing_tip->phashBlock->data, false, true, "init_existing_tip")) {
        stage_destroy(s);
        g_stage = NULL;
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!hydrate_stage_cursor_from_store(db, s, "init_existing_tip")) {
        stage_destroy(s);
        g_stage = NULL;
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    publish_resolved_or_fresh_tip(db, existing_tip, "init_existing_tip");
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("tip_finalize", "[tip_finalize] stage initialised (authoritative)");
    return true;
}

job_result_t tip_finalize_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    progress_store_tx_lock();
    /* Re-widen chain[] INSIDE the lock so the extend, reorg-check, read
     * (active_chain_at), and write (active_chain_move_window_tip) are
     * atomic from this thread's perspective — closing the race where a
     * concurrent active_chain write changed chain[next_h+1] between the
     * extend and step_finalize's decision. stage_helpers.h
     * Lock-order: progress_store_tx_lock -> active_chain.write_lock;
     * reverse does not exist (active_chain_fill_window is array-only). */
    reducer_extend_window_to_candidate(g_ms, true);
    bool rewind_ok = rewind_cursor_if_active_chain_reorged(db);
    if (!rewind_ok) {
        progress_store_tx_unlock();
        return JOB_FATAL;
    }
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    return r;
}

STAGE_DRAIN_IMPL(tip_finalize)

void tip_finalize_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_utxo_counter = NULL;
    g_utxo_counter_user = NULL;
    atomic_store(&g_finalized_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_reorg_detected_total, (uint64_t)0);
    atomic_store(&g_utxo_count_diverged_total, (uint64_t)0);
    atomic_store(&g_precondition_failed_total, (uint64_t)0);
    atomic_store(&g_successor_pending_total, (uint64_t)0);
    atomic_store(&g_header_witness_total, (uint64_t)0);
    atomic_store(&g_total_work_added_high, (uint64_t)0);
    atomic_store(&g_total_work_added_low, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_blocked_class, TF_BLOCKED_NONE);
    for (int i = 0; i < TF_BLOCKED_CLASS_N; i++)
        atomic_store(&g_blocked_class_total[i], (uint64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    /* Mirror the served-tip reset into the external provable-tip cache so a
     * stale-high H* from this run cannot leak into the next boot. */
    reducer_frontier_provable_tip_reset();
    atomic_store(&g_last_precondition_height, (int64_t)-1);
    log_throttle_reset(&g_precondition_throttle);
    log_throttle_reset(&g_cursor_gap_throttle);
    zcl_mutex_lock(&g_block_reason_mu);
    g_last_precondition_reason[0] = '\0';
    zcl_mutex_unlock(&g_block_reason_mu);
    zcl_mutex_destroy(&g_last_advance_hash_mu);
    zcl_mutex_destroy(&g_block_reason_mu);
    pthread_mutex_unlock(&g_lock);
}

/* tip_finalize_stage_set_authoritative_tip and tip_finalize_stage_seed_anchor
 * live in tip_finalize_anchor.c. */

bool tip_finalize_stage_finalized_tip_at(sqlite3 *db, int height,
                                         uint8_t out_hash[32])
{
    if (!db || !out_hash || height < 0)
        return false;
    progress_store_tx_lock();
    struct finalized_tip_row row;
    if (!finalized_tip_row_at(db, height, &row)) {
        progress_store_tx_unlock();
        return false;
    }
    if (!row.found || !row.ok || !row.has_tip_hash) {
        progress_store_tx_unlock();
        return false;
    }
    memcpy(out_hash, row.tip_hash.data, 32);
    progress_store_tx_unlock();
    return true;
}

bool tip_finalize_stage_block_hash_at(sqlite3 *db, int height,
                                      uint8_t out_hash[32])
{
    if (!db || !out_hash || height < 0)
        return false;
    progress_store_tx_lock();

    /* FINALIZED convention (step_finalize): the ok=1 row at height-1 binds
     * the LOOKAHEAD new_tip = active_chain_at(height), so its tip_hash IS
     * this height's own hash. An anchor row at height-1 carries hash(height-1)
     * (the seed's own hash) and must be skipped here — exactly the
     * finalized_row_active_match discrimination. */
    if (height > 0) {
        struct finalized_tip_row prev;
        if (finalized_tip_row_at(db, height - 1, &prev) &&
            prev.found && prev.ok && prev.has_tip_hash && !prev.is_anchor) {
            memcpy(out_hash, prev.tip_hash.data, 32);
            progress_store_tx_unlock();
            return true;
        }
    }

    /* ANCHOR convention (tip_finalize_stage_seed_anchor): a seed row at
     * height carries the block's OWN hash. A finalized row at height carries
     * hash(height+1) — the successor's hash — and must NOT be returned as
     * this height's hash (an inconsistent authority pair). */
    struct finalized_tip_row own;
    if (finalized_tip_row_at(db, height, &own) &&
        own.found && own.ok && own.has_tip_hash && own.is_anchor) {
        memcpy(out_hash, own.tip_hash.data, 32);
        progress_store_tx_unlock();
        return true;
    }

    progress_store_tx_unlock();
    return false;
}

bool tip_finalize_stage_resolve_durable_tip(sqlite3 *db, int *out_height,
                                            uint8_t out_hash[32])
{
    if (!db || !out_height || !out_hash)
        return false;
    uint64_t cursor = 0;
    if (!stage_cursor_read_or_zero(db, STAGE_NAME, STAGE_NAME, &cursor))
        return false;
    if (cursor == 0)
        return false;
    /* Try cursor (anchor-at-cursor steady state), then cursor-1 (legacy +1
     * lattice / finalized-row convention). block_hash_at discriminates the
     * row types, so whichever height resolves owns the returned hash. */
    for (int back = 0; back <= 1; back++) {
        int h = (int)cursor - back;
        if (h < 0)
            break;
        if (tip_finalize_stage_block_hash_at(db, h, out_hash)) {
            *out_height = h;
            return true;
        }
    }
    return false;
}

void tip_finalize_stage_set_utxo_counter(tip_finalize_utxo_count_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_utxo_counter = fn;
    g_utxo_counter_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t tip_finalize_stage_cursor(void) { return g_stage ? stage_cursor(g_stage) : 0; }
int64_t tip_finalize_stage_last_height(void) { return atomic_load(&g_last_advance_height); }

/* Test-only: reset the published served-tip height to -1 (a stale high value
 * from a prior group without shutdown() poisons later active_chain_tip reads). */
void tip_finalize_stage_test_reset(void) { atomic_store(&g_last_advance_height, (int64_t)-1); reducer_frontier_provable_tip_reset(); }
uint64_t tip_finalize_stage_finalized_total(void) { return atomic_load(&g_finalized_total); }
uint64_t tip_finalize_stage_upstream_failed_total(void) { return atomic_load(&g_upstream_failed_total); }
uint64_t tip_finalize_stage_reorg_detected_total(void) { return atomic_load(&g_reorg_detected_total); }
uint64_t tip_finalize_stage_utxo_count_diverged_total(void) { return atomic_load(&g_utxo_count_diverged_total); }
uint64_t tip_finalize_stage_precondition_failed_total(void) { return atomic_load(&g_precondition_failed_total); }
uint64_t tip_finalize_stage_successor_pending_total(void) { return atomic_load(&g_successor_pending_total); }
uint64_t tip_finalize_stage_header_witness_total(void) { return atomic_load(&g_header_witness_total); }
uint64_t tip_finalize_stage_total_work_added_high(void) { return atomic_load(&g_total_work_added_high); }
uint64_t tip_finalize_stage_total_work_added_low(void) { return atomic_load(&g_total_work_added_low); }

/* Lock-free blocked-class snapshot for the supervisor stall log. Returns a
 * process-lifetime string literal (safe for LOG_WARN); "" when none yet. */
const char *tip_finalize_stage_last_blocked_reason(void)
{
    int cls = atomic_load(&g_last_blocked_class);
    if (cls <= TF_BLOCKED_NONE || cls >= TF_BLOCKED_CLASS_N) return "";
    return k_tf_blocked_name[cls];
}

bool tip_finalize_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = atomic_load(&g_last_step_unix);

    stage_dump_header(out, STAGE_NAME, g_stage);
    json_push_kv_int (out, "finalized_total", (int64_t)atomic_load(&g_finalized_total));
    json_push_kv_int (out, "upstream_failed_total", (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "reorg_detected_total", (int64_t)atomic_load(&g_reorg_detected_total));
    json_push_kv_int (out, "utxo_count_diverged_total", (int64_t)atomic_load(&g_utxo_count_diverged_total));
    json_push_kv_int (out, "precondition_failed_total", (int64_t)atomic_load(&g_precondition_failed_total));
    json_push_kv_int (out, "successor_pending_total", (int64_t)atomic_load(&g_successor_pending_total));
    json_push_kv_int (out, "header_witness_total", (int64_t)atomic_load(&g_header_witness_total));
    json_push_kv_int (out, "last_precondition_height", atomic_load(&g_last_precondition_height));
    json_push_kv_int (out, "precondition_repeat_count", (int64_t)log_throttle_reps(&g_precondition_throttle));
    {
        /* g_block_reason_mu is only live while the stage is (init..shutdown);
         * guard the read so a dump before init / after shutdown is safe. */
        char reason_buf[40] = "";
        if (g_stage) {
            zcl_mutex_lock(&g_block_reason_mu);
            snprintf(reason_buf, sizeof reason_buf, "%s", g_last_precondition_reason);
            zcl_mutex_unlock(&g_block_reason_mu);
        }
        json_push_kv_str(out, "last_precondition_reason", reason_buf);
    }
    json_push_kv_int (out, "total_work_added_high", (int64_t)atomic_load(&g_total_work_added_high));
    json_push_kv_int (out, "total_work_added_low", (int64_t)atomic_load(&g_total_work_added_low));
    json_push_kv_int (out, "last_advance_height", atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds", last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix", atomic_load(&g_last_blocked_unix));
    {
        int cls = atomic_load(&g_last_blocked_class);
        if (cls < 0 || cls >= TF_BLOCKED_CLASS_N)
            cls = TF_BLOCKED_NONE;
        json_push_kv_str(out, "last_blocked_reason", k_tf_blocked_name[cls]);
        for (int i = TF_BLOCKED_NONE + 1; i < TF_BLOCKED_CLASS_N; i++) {
            char key[64];
            snprintf(key, sizeof key, "blocked_%s_total",
                     k_tf_blocked_name[i]);
            json_push_kv_int(out, key,
                             (int64_t)atomic_load(&g_blocked_class_total[i]));
        }
    }
    json_push_kv_int (out, "log_rows", db ? stage_log_row_count(db, STAGE_NAME, "tip_finalize_log") : 0);
    stage_dump_counters(out, g_stage);
    return true;
}
