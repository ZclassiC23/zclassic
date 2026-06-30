/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage — implementation. See jobs/utxo_apply_stage.h.
 *
 * Consumes proof_validate_log and computes a transparent UTXO delta.
 * It writes only utxo_apply_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/utxo_apply_delta.h"
#include "jobs/replay_count_only.h"
#include "jobs/utxo_apply_nullifiers.h"
#include "jobs/stage_helpers.h"
#include "utxo_apply_log_store.h"
#include "utxo_apply_stage_internal.h"
#include "utxo_apply_stage_observe.h"
#include "script_validate_log_store.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "services/seal_service.h"
#include "services/anchor_selfmint.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/coins_kv.h"
#include "storage/coins_ram.h"
#include "storage/disk_block_io.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/main_state.h"
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAGE_NAME "utxo_apply"

/* struct proof_validate_row + the utxo_apply_log schema/read/write helpers
 * live in utxo_apply_log_store.c (pure sqlite kernel helpers below the AR
 * layer); the delta structs, free_delta(_arr), the block-delta builder
 * (utxo_apply_compute_block_delta) and the inverse-delta persistence +
 * reorg-unwind machinery live in jobs/utxo_apply_delta.h / _delta.c. */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static utxo_apply_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;
static utxo_apply_lookup_fn g_lookup = NULL;
static void *g_lookup_user = NULL;

/* Module state shared with utxo_apply_stage_dump.c (the zcl_state dump TU)
 * via utxo_apply_stage_internal.h — written here, atomic_load-only there. */
_Atomic uint64_t g_ua_verified_total = 0;
_Atomic uint64_t g_ua_spend_unknown_total = 0;
_Atomic uint64_t g_ua_utxo_collision_total = 0;
_Atomic uint64_t g_ua_value_overflow_total = 0;
_Atomic uint64_t g_ua_coinbase_protect_total = 0;
_Atomic uint64_t g_ua_bad_cb_amount_total = 0;
_Atomic uint64_t g_ua_shielded_double_spend_total = 0;
_Atomic uint64_t g_ua_upstream_failed_total = 0;
_Atomic uint64_t g_ua_internal_error_total = 0;
_Atomic uint64_t g_ua_reorg_unwound_total = 0;
_Atomic uint64_t g_ua_total_outputs_added = 0;
_Atomic uint64_t g_ua_total_outputs_spent = 0;
_Atomic int64_t  g_ua_last_step_unix = 0;
_Atomic int64_t  g_ua_last_blocked_unix = 0;
_Atomic int64_t  g_ua_last_advance_height = -1;
_Atomic uint64_t g_ua_upstream_hole_total = 0;
_Atomic int64_t  g_ua_upstream_hole_height = -1;
_Atomic int64_t  g_ua_upstream_hole_first_unix = 0;
_Atomic uint64_t g_ua_upstream_hole_consec = 0;
_Atomic uint64_t g_ua_upstream_hole_warn_total = 0;
_Atomic uint64_t g_ua_label_splice_total = 0;

/* Author the validated block delta into the progress.kv `coins` table
 * (coins_kv) on the stage's own db handle, INSIDE stage_run_once's BEGIN
 * IMMEDIATE — the coin mutation commits or rolls back as ONE atomic unit
 * with the cursor + inverse-delta + log row, closing the tip-wedge tear
 * class (docs/work/tip-durability-collapse.md). coins_kv is the SOLE live
 * UTXO author and read source (the projection is seed-only). Adds run BEFORE
 * spends and the ORDER IS LOAD-BEARING: an intra-block create-then-spend coin
 * is in both arrays and coins_kv_spend no-ops on an absent row, so spends-first
 * would leave a phantom UTXO (compute_block_delta rejects duplicate creates). */
static bool apply_coins_kv(sqlite3 *db, const struct delta_summary *s,
                           uint32_t height)
{
    /* Batched apply: one prepared statement reused across all adds, then one
     * across all spends. Adds STILL run before spends (the create-then-spend
     * intra-block invariant), and within each phase the original (txid,vout)
     * order is preserved by walking added[]/spent[] in index order — so the
     * coins set, and coins_kv_commitment over it, is bit-identical to the
     * per-row coins_kv_add / coins_kv_spend loop. The block height (param) is
     * stamped as each added coin's height, EXACTLY as the per-row path did
     * (NOT added[i].height). Row arrays alias the summary/block buffers, which
     * the caller keeps live until after this returns (SQLITE_STATIC binds). */
    bool ok = true;

    if (s->added_count > 0) {
        struct coins_kv_add_row *adds =
            zcl_malloc(s->added_count * sizeof(*adds), "apply_coins_kv_adds");
        if (!adds) return false;
        for (size_t i = 0; i < s->added_count; i++) {
            adds[i].txid        = s->added[i].txid.data;
            adds[i].vout        = s->added[i].vout;
            adds[i].value       = s->added[i].value;
            adds[i].height      = (int32_t)height;
            adds[i].is_coinbase = s->added[i].is_coinbase;
            adds[i].script      = s->added[i].script;
            adds[i].script_len  = s->added[i].script_len;
        }
        ok = coins_kv_add_many(db, adds, s->added_count);
        free(adds);
        if (!ok) return false;
    }

    if (s->spent_count > 0) {
        struct coins_kv_spend_row *spends =
            zcl_malloc(s->spent_count * sizeof(*spends), "apply_coins_kv_spends");
        if (!spends) return false;
        for (size_t i = 0; i < s->spent_count; i++) {
            spends[i].txid = s->spent[i].txid.data;
            spends[i].vout = s->spent[i].vout;
        }
        ok = coins_kv_spend_many(db, spends, s->spent_count);
        free(spends);
    }
    return ok;
}

/* The reducer's port of zclassicd's shielded-double-spend gate (C-3) lives
 * in utxo_apply_nullifiers.c (utxo_apply_check_and_insert_nullifiers + the
 * activation-gap blocker), split out along the utxo_apply_delta*.c seam. */

static job_result_t block_apply_failure(struct stage_step_ctx *c, int height,
                                        const char *status,
                                        const char *kind,
                                        const uint8_t detail[36])
{
    char reason[BLOCKER_REASON_MAX];
    char txid_hex[65] = {0};
    uint32_t vout = 0;

    if (detail) {
        struct uint256 txid;
        memcpy(txid.data, detail, sizeof(txid.data));
        uint256_get_hex(&txid, txid_hex);
        vout = (uint32_t)detail[32] |
               ((uint32_t)detail[33] << 8) |
               ((uint32_t)detail[34] << 16) |
               ((uint32_t)detail[35] << 24);
    }

    if (txid_hex[0]) {
        snprintf(reason, sizeof(reason),
                 "height=%d status=%s kind=%s txid=%s vout=%u; "
                 "utxo_apply cursor held to prevent applying coins above "
                 "an unresolved hole",
                 height, status ? status : "unknown",
                 kind ? kind : "", txid_hex, vout);
    } else {
        snprintf(reason, sizeof(reason),
                 "height=%d status=%s kind=%s; utxo_apply cursor held to "
                 "prevent applying coins above an unresolved hole",
                 height, status ? status : "unknown", kind ? kind : "");
    }

    blocker_init(&c->blocker, "utxo_apply.apply_failed", STAGE_NAME,
                 BLOCKER_TRANSIENT, reason);
    c->blocker.escape_deadline_secs = 60;
    c->blocker.retry_budget = 5;
    atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
    return JOB_BLOCKED;
}

/* Surface a genuine permanent store-corruption / unrecoverable-read failure as
 * a NAMED typed blocker before the JOB_FATAL verdict. Without this, a torn
 * progress.kv makes step_apply return JOB_FATAL every boot: stage_run_once
 * latches the FATAL and the drain emits one EV_OPERATOR_NEEDED, but that alert
 * is in-memory and is CLEARED on every process restart, so
 * wd_deterministic_stall_cause() finds no PERMANENT blocker and the
 * chain_tip_watchdog treats the stall as genuine liveness loss — burning its
 * bounded restart budget power-cycling against the same wedge. A
 * PERMANENT blocker is re-derived from the same torn store on the first tick
 * of every boot (before the watchdog's restart threshold), so the stall is
 * immediately classified "permanent_blocker_active" and the node stays up
 * degraded with a named halt an operator can see via zcl_blockers instead of
 * crash-looping. blocker_set's token bucket dedups the per-tick re-fire.
 * Transient conditions are NOT routed here — they stay JOB_IDLE/JOB_BLOCKED. */
static void ua_fatal_permanent_blocker(int height, const char *reason_tail)
{
    struct blocker_record rec;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "utxo_apply permanent store failure at height=%d: %s "
             "(cursor held; progress.kv / coins_kv is torn or unreadable — "
             "a restart re-derives the identical failure; operator must "
             "repair/reindex the store, then clear this blocker)",
             height, reason_tail ? reason_tail : "unrecoverable error");
    if (!blocker_init(&rec, "utxo_apply.fatal_store", STAGE_NAME,
                      BLOCKER_PERMANENT, reason))
        return;   /* blocker_init logged the overflow */
    blocker_set(&rec);  /* -1 (cap exhausted) / 1 (rate-limited dup) already
                         * logged by blocker_set; the fact persists */
}

static job_result_t step_apply(struct stage_step_ctx *c)
{
    atomic_store(&g_ua_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) {
        ua_fatal_permanent_blocker((int)c->cursor_in,
                                   "stage cursor persisted negative");
        return JOB_FATAL;
    }

    uint64_t pv_cursor = 0;
    if (!stage_cursor_read_or_zero(db, "proof_validate", STAGE_NAME,
                                   &pv_cursor)) {
        ua_fatal_permanent_blocker(next_h,
                                   "proof_validate cursor read failed");
        return JOB_FATAL;
    }
    if ((uint64_t)next_h >= pv_cursor) {
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct proof_validate_row upstream;
    int found = utxo_apply_proof_validate_log_at(db, next_h, &upstream);
    if (found < 0) {
        ua_fatal_permanent_blocker(next_h,
                                   "proof_validate_log read returned error");
        return JOB_FATAL;
    }
    if (found == 0) {
        /* Reaching here implies next_h < pv_cursor (the >= guard above
         * returned otherwise), so this is a DURABLE upstream hole — a
         * stale-replay / self-restart artifact — never "not yet".
         * DELIBERATELY JOB_IDLE, not JOB_BLOCKED: JOB_BLOCKED feeds the
         * supervisor escalation/restart ladder, and a watchdog self-restart
         * is what manufactured this hole class in the first place. The L1
         * reconcile-light Condition is the healer; the counters/WARN here
         * are the alarm. */
        utxo_apply_upstream_hole_note(next_h, pv_cursor);
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }
    utxo_apply_upstream_hole_healed(next_h);

    if (upstream.ok == 0) {
        atomic_fetch_add(&g_ua_upstream_failed_total, 1);
        return block_apply_failure(c, next_h, "upstream_failed",
                                   "proof_validate", NULL);
    }

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA)) {
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    /* Hash-bound verdict gate — the structural stop for the header
     * height-splice class. Stage logs are keyed BY HEIGHT, so after an
     * in-memory header relabel the script_validate_log row at next_h can be
     * the verdict for a DIFFERENT block than the one chain[] now exposes;
     * applying with that stale verdict tears the coin set at the splice
     * height (the TRUE block never applies; surfaces ~28 labels later as
     * bad-cb-height). Same hash-identity guard as tip_finalize's
     * finalize_script_log_ok: a row provably bound to another hash refuses
     * the apply with a typed transient blocker until script_validate re-binds
     * the height. A NULL hash or an absent row cannot prove a mismatch and
     * passes through — the proof_validate cursor guard above covers ordering. */
    struct script_validate_verdict_row sv_row;
    int sv_found = script_validate_log_verdict_at(db, next_h, &sv_row);
    if (sv_found < 0) {
        ua_fatal_permanent_blocker(next_h,
                                   "script_validate_log verdict read returned error");
        return JOB_FATAL;
    }
    if (sv_found == 1 && sv_row.has_block_hash &&
        !uint256_eq(&sv_row.block_hash, bi->phashBlock)) {
        return utxo_apply_label_splice_refuse(
            c, next_h, bi->phashBlock, &sv_row.block_hash);
    }
    /* The previously-refused verdict is re-bound (or no longer provably
     * foreign): close the memo so a re-opened splice counts as new. */
    utxo_apply_label_splice_healed(next_h);

    struct block blk;
    block_init(&blk);
    if (!stage_read_block(&blk, bi, next_h, g_datadir, g_reader, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct delta_summary summary;
    utxo_apply_compute_block_delta(&blk, (uint32_t)next_h,
                                   g_lookup, g_lookup_user, &summary);

    /* REPLAY GATE (D2 count-and-continue, env-gated ZCL_REPLAY_COUNT_ONLY).
     * compute_block_delta sets count_only_d2_skip ONLY in count-only mode when
     * the coinbase-maturity predicate fired (already logged+counted there).
     * STRICTLY read/log/continue: author NO coins, insert NO nullifiers,
     * persist NO inverse-delta, write NO utxo_apply_log row, and DO NOT take
     * the reject/halt path — just advance the read-only cursor so the walk
     * reaches tip and counts EVERY offender, never the first only. The
     * offending block's coins are NEVER authored, so the copy's coins_kv is
     * not corrupted past a real reject. This whole branch is unreachable when
     * the env is unset (count_only_d2_skip is always false then), so the live
     * fold runs exactly as it does today. */
    if (summary.count_only_d2_skip) {
        replay_count_only_note_block_replayed((uint32_t)next_h);
        free_delta(&summary);
        block_free(&blk);
        /* Advance the cursor WITHOUT authoring/sealing/minting: this is a
         * diagnostic walk over a copy, not a real apply. coins_applied_height
         * is intentionally NOT co-committed (no coins were written), so the
         * copy's coins_kv reflects the truth that this block was skipped. */
        atomic_store(&g_ua_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        /* Emit the running summary when we reach the proven tip (cursor caught
         * up to the upstream proof_validate frontier — see the pv_cursor guard
         * at step_apply entry; one more tick returns JOB_IDLE). target_tip is
         * the header_admit cursor (top of the staged pipeline == header tip),
         * so the gate can assert the walk reached the FULL header chain, not a
         * subset stalled below it. */
        if (c->cursor_out >= pv_cursor)
            replay_count_only_emit_summary(
                (int64_t)stage_cursor_persisted(db, "header_admit", STAGE_NAME));
        return JOB_ADVANCED;
    }

    /* Shielded-nullifier double-spend gate (C-3) — BEFORE the coins write,
     * under the same author gate. May flip summary.ok to a consensus reject
     * (which then takes the regular counter/log/JOB_BLOCKED path below); a
     * store error is fatal like any other in-txn store failure. */
    if (summary.ok && utxo_projection_get_author() == UTXO_AUTHOR_STAGE) {
        if (!utxo_apply_check_and_insert_nullifiers(db, &blk, next_h,
                                                    &summary)) {
            ua_fatal_permanent_blocker(next_h,
                                       "nullifier_kv insert store failure");
            free_delta(&summary);
            block_free(&blk);
            return JOB_FATAL;
        }
    }

    /* Author the canonical coins_kv set when the stage holds UTXO authority
     * (UTXO_AUTHOR_STAGE — it guards the sole live UTXO author). In-txn: a
     * failure rolls back the whole stage txn (cursor + inverse-delta + log
     * row + coins) — never a torn partial apply. Scripts in `summary.added`
     * alias into `blk`, so apply before block_free. */
    if (summary.ok && utxo_projection_get_author() == UTXO_AUTHOR_STAGE) {
        if (!apply_coins_kv(db, &summary, (uint32_t)next_h)) {
            ua_fatal_permanent_blocker(next_h,
                                       "coins_kv author store failure");
            free_delta(&summary);
            block_free(&blk);
            return JOB_FATAL;
        }
    }

    /* Persist the per-block inverse-delta (the later-disconnect source — no
     * legacy undo files), stamped with the OLD branch hash so a same-height
     * fork is distinguishable. In the stage txn so delta + log row + cursor
     * land atomically; only a successful apply persists one (failure rows
     * have nothing to invert). */
    if (summary.ok) {
        if (!utxo_apply_delta_persist(db, next_h, bi->phashBlock, &summary)) {
            ua_fatal_permanent_blocker(next_h,
                                       "inverse-delta persist store failure");
            free_delta(&summary);
            block_free(&blk);
            return JOB_FATAL;
        }
    }

    if (summary.ok) {
        atomic_fetch_add(&g_ua_verified_total, 1);
        atomic_fetch_add(&g_ua_total_outputs_added,
                         (uint64_t)summary.added_count);
        atomic_fetch_add(&g_ua_total_outputs_spent,
                         (uint64_t)summary.spent_count);
    } else {
        /* The event label is zclassicd's exact reject string where one
         * exists; dedup + keep-alive policy lives in reject_count_and_emit. */
        _Atomic uint64_t *counter = &g_ua_internal_error_total;
        const char *label = "internal_error";
        if (strcmp(summary.status, "spend_unknown_utxo") == 0) {
            counter = &g_ua_spend_unknown_total;
            label = "spend_unknown_utxo";
        } else if (strcmp(summary.status, "utxo_collision") == 0) {
            counter = &g_ua_utxo_collision_total;
            label = "utxo_collision";
        } else if (strcmp(summary.status, "value_overflow") == 0) {
            counter = &g_ua_value_overflow_total;
            label = "value_overflow";
        } else if (strcmp(summary.status, "coinbase_protect") == 0) {
            counter = &g_ua_coinbase_protect_total;
            label = "bad-txns-coinbase-spend-has-transparent-outputs";
        } else if (strcmp(summary.status, "bad_cb_amount") == 0) {
            counter = &g_ua_bad_cb_amount_total;
            label = "bad-cb-amount";
        } else if (strcmp(summary.status, "shielded_double_spend") == 0) {
            counter = &g_ua_shielded_double_spend_total;
            label = "bad-txns-joinsplit-requirements-not-met";
        }
        utxo_apply_reject_count_and_emit(next_h, summary.status, counter,
                                         label);
    }

    if (!utxo_apply_log_insert(db, next_h, summary.status, summary.ok,
                    summary.spent_count, summary.added_count,
                    summary.total_value_delta, summary.failure_kind,
                    summary.ok ? NULL : summary.failure_detail)) {
        ua_fatal_permanent_blocker(next_h,
                                   "utxo_apply_log insert store failure");
        free_delta(&summary);
        block_free(&blk);
        return JOB_FATAL;
    }

    if (!summary.ok) {
        const char *status = summary.status;
        const char *kind = summary.failure_kind;
        uint8_t detail[36];
        memcpy(detail, summary.failure_detail, sizeof(detail));
        free_delta(&summary);
        block_free(&blk);
        return block_apply_failure(c, next_h, status, kind, detail);
    }

    free_delta(&summary);
    block_free(&blk);

    /* Co-commit the contiguous applied frontier = the SAME value written to
     * the stage cursor (cursor_in + 1), so coins_applied_height ==
     * utxo_apply cursor by construction on every successful apply. Failed
     * verdicts return JOB_BLOCKED above and hold cursor/frontier at the
     * unresolved height (scratch log row rolled back) — fail-closed, never a
     * mixed coins window over a hole. The seal hook rides THIS txn, never fails it. */
    uint64_t next_cursor = c->cursor_in + 1;
    if (!coins_kv_set_applied_height_in_tx(db, (int32_t)next_cursor)) {
        ua_fatal_permanent_blocker(next_h,
                                   "coins_applied_height co-commit store failure");
        return JOB_FATAL;
    }
    seal_candidate_hook_in_tx(db, g_ms, (int32_t)next_cursor);
    /* SELF-MINT the SHA3-verified anchor snapshot the first time the fold lands
     * the compiled checkpoint height — coins_kv provably holds the
     * applied-through-anchor set inside THIS txn. Observe-only + best-effort:
     * persists already-validated state (no consensus/fold change), one-shot at
     * the exact anchor height, idempotent, and HARD-VERIFIES SHA3 before trust.
     * Makes <datadir>/utxo-anchor.snapshot reachable for the torn-import
     * self-heal without an operator running the offline -mint-anchor ceremony. */
    anchor_selfmint_hook_in_tx(db, g_datadir, (int32_t)next_cursor);

    atomic_store(&g_ua_last_advance_height, (int64_t)next_h);
    utxo_apply_reject_memo_clear();
    c->cursor_out = next_cursor;

    utxo_apply_progress_note(next_h, next_cursor);

    /* REPLAY GATE coverage accounting (no-op unless ZCL_REPLAY_COUNT_ONLY):
     * count every successfully-applied block toward blocks_replayed so the
     * contiguity check (blocks_replayed == tip+1) can detect a sparse / non
     * -genesis walk (a FALSE 0). Mark genesis readable when this is h=0.
     * Emit the running summary once the cursor catches the proven tip. */
    if (replay_count_only_active()) {
        if (next_h == 0)
            replay_count_only_mark_genesis_readable();
        replay_count_only_note_block_replayed((uint32_t)next_h);
        if (next_cursor >= pv_cursor)
            replay_count_only_emit_summary(
                (int64_t)stage_cursor_persisted(db, "header_admit", STAGE_NAME));
    }
    return JOB_ADVANCED;
}

bool utxo_apply_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("utxo_apply", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("utxo_apply", "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("utxo_apply",
                "init: already bound to a different main_state");
        return true;
    }

    if (!utxo_apply_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    /* step_apply's hash-bound verdict gate reads script_validate_log; ensure
     * it exists (idempotent) so init-ordering never breaks the read. */
    if (!script_validate_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!utxo_apply_ensure_delta_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!coins_kv_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    /* Consensus shielded-nullifier set (C-3). Existence is probed BEFORE the
     * ensure so FIRST creation can stamp the activation marker: heights
     * at/below the cursor at activation were applied WITHOUT nullifier
     * enforcement (their nullifiers are not in the table). The marker keeps
     * `zcl_sql` forensics honest about where enforcement began AND drives
     * the activation-gap blocker (refresh below). It is NOT a consensus
     * input (no verdict reads it). Marker failure is non-fatal (logged). */
    bool nf_existed = nullifier_kv_table_exists(db);
    if (!nullifier_kv_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!nf_existed) {
        char cur[24];
        int len = snprintf(cur, sizeof(cur), "%llu",
                           (unsigned long long)stage_cursor_persisted(
                               db, STAGE_NAME, STAGE_NAME));
        if (len <= 0 ||
            !progress_meta_set_in_tx(db, "nullifier_kv.activation_cursor",
                                     cur, (size_t)len))
            LOG_WARN(STAGE_NAME,
                     "[utxo_apply] nullifier_kv activation marker write "
                     "failed (diagnostics-only, not retried)");
    }
    /* C-3 activation gap, owner-visible: pre-activation history has no
     * nullifier rows (no backfill exists yet), so a marker > 0 registers
     * the PERMANENT blocker UTXO_APPLY_NF_GAP_BLOCKER_ID every boot until
     * an owner-gated backfill (or a from-genesis resync) closes the gap. */
    utxo_apply_nullifier_gap_blocker_refresh(db);

    /* One-time backfill for existing datadirs that predate coins_applied_height:
     * seed the canonical contiguous frontier from the already-trusted utxo_apply
     * cursor (NEVER from MAX(coins.height)). No-op once the key exists; a virgin
     * datadir (no cursor row) is left ABSENT so the first forward apply writes it
     * in lockstep with the cursor. Non-fatal — next boot retries. */
    if (!coins_kv_backfill_applied_height_if_absent(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    /* Bulk-fold in-RAM hot store (flag-gated, ZCL_FOLD_INRAM). BOTH calls are
     * no-ops (return true) when the flag is off, so steady-state init is
     * unchanged. reconcile_boot runs FIRST: if a prior bulk fold crashed
     * between flushes, the durable utxo_apply cursor is ahead of the last
     * coins_kv flush watermark — rewind it so the fold re-applies the
     * un-flushed tail (the from-genesis self-verify still gates correctness).
     * Then init allocates the overlay and binds it to the coins_kv handle. */
    if (!coins_ram_reconcile_boot(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!coins_ram_init(db, 0)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    GetDataDir(true, g_datadir, sizeof(g_datadir));

    stage_t *s = stage_create(STAGE_NAME, step_apply, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("utxo_apply", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    /* Wire the production UTXO-set resolver unless a caller (a test)
     * already installed one: with g_lookup NULL the delta builder treats
     * EVERY external coin as absent and rejects every cross-block
     * transparent spend as spend_unknown_utxo. */
    if (!g_lookup)
        g_lookup = utxo_apply_stage_lookup_live;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("utxo_apply", "[utxo_apply] stage initialised");
    return true;
}

job_result_t utxo_apply_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    /* Chain-extender: keep the visible chain[] window extended to the
     * most-work candidate so both the reorg-unwind detection and the
     * forward-apply below (each reads active_chain_at) see the winning
     * branch. This runs only when the stage owns UTXO projection authorship;
     * otherwise it leaves the active-chain window untouched. */
    reducer_extend_window_to_candidate(
        g_ms, utxo_projection_get_author() == UTXO_AUTHOR_STAGE);

    /* Mark THIS thread as the coins_ram writer for the fold step (the UAF
     * guard — see coins_ram.h). The overlay is mutated by apply_coins_kv →
     * coins_ram_* inside stage_run_once AND by coins_ram_note_applied below,
     * so the bracket covers the whole critical region: from progress_store_tx
     * lock acquisition through the post-commit note_applied. Every return path
     * out of this region calls coins_ram_writer_exit() so the counter balances.
     * A no-op (one relaxed load) when the overlay is inactive. */
    coins_ram_writer_enter();

    /* Drain any pending stage-side reorg disconnect BEFORE the next
     * forward apply (and before tip_finalize, which the supervisor drains
     * after us, reads our cursor). Self-contained txn; on failure the
     * cursor is untouched so the next tick retries. */
    progress_store_tx_lock();
    bool unwind_ok =
        utxo_apply_reorg_unwind_if_needed(db, g_stage, g_ms,
                                          &g_ua_reorg_unwound_total,
                                          &g_ua_last_blocked_unix);
    if (!unwind_ok) {
        progress_store_tx_unlock();
        coins_ram_writer_exit();
        return JOB_FATAL;
    }
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    /* No projection catch_up fold needed: utxo_apply_stage_lookup_live reads the
     * coins_kv rows apply_coins_kv writes IN this stage's BEGIN IMMEDIATE
     * (see its FRESHNESS CONTRACT). */

    /* Bulk-fold in-RAM hot store (flag-gated): after the stage txn COMMITS, the
     * just-applied height's coins live in the RAM overlay (apply_coins_kv routed
     * through coins_kv_*_many → coins_ram_*). coins_ram_note_applied bumps the
     * since-flush counter and, every K blocks, drains the overlay into coins_kv
     * in its OWN BEGIN IMMEDIATE (which is why it CANNOT run inside the stage
     * txn). A no-op (returns true) when the flag is off / store inactive. The
     * flush co-writes the cursor + frontier + watermark so the durable state
     * stays self-consistent; a crash between flushes is rewound by
     * coins_ram_reconcile_boot at the next boot. */
    if (r == JOB_ADVANCED && coins_ram_active()) {
        int64_t applied = atomic_load(&g_ua_last_advance_height);
        if (applied >= 0 && !coins_ram_note_applied((int32_t)applied)) {
            coins_ram_writer_exit();
            return JOB_FATAL;
        }
    }
    coins_ram_writer_exit();
    return r;
}

STAGE_DRAIN_IMPL(utxo_apply)

void utxo_apply_stage_shutdown(void)
{
    /* Clean-stop flush of the in-RAM bulk-fold overlay BEFORE we tear down (and
     * before the progress store closes): persist the un-flushed tail so a
     * graceful stop does not force a crash-replay rewind on the next boot.
     * No-op when the flag is off / store inactive / overlay empty. Done before
     * taking g_lock since the flush takes progress_store_tx_lock internally. */
    if (coins_ram_active() && !coins_ram_flush_final())
        LOG_WARN("utxo_apply",
                 "[utxo_apply] shutdown: coins_ram final flush failed; the "
                 "un-flushed tail will be re-applied on next boot via the "
                 "crash-replay watermark");
    coins_ram_shutdown();

    pthread_mutex_lock(&g_lock);
    /* Registry hygiene (tests re-init in-process): init re-registers the
     * gap blocker from the durable marker, so clearing here loses nothing. */
    blocker_clear(UTXO_APPLY_NF_GAP_BLOCKER_ID);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_datadir[0] = '\0';
    g_reader = NULL;
    g_reader_user = NULL;
    g_lookup = NULL;
    g_lookup_user = NULL;
    atomic_store(&g_ua_verified_total, (uint64_t)0);
    atomic_store(&g_ua_spend_unknown_total, (uint64_t)0);
    atomic_store(&g_ua_utxo_collision_total, (uint64_t)0);
    atomic_store(&g_ua_value_overflow_total, (uint64_t)0);
    atomic_store(&g_ua_coinbase_protect_total, (uint64_t)0);
    atomic_store(&g_ua_bad_cb_amount_total, (uint64_t)0);
    atomic_store(&g_ua_shielded_double_spend_total, (uint64_t)0);
    atomic_store(&g_ua_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_ua_internal_error_total, (uint64_t)0);
    atomic_store(&g_ua_reorg_unwound_total, (uint64_t)0);
    atomic_store(&g_ua_total_outputs_added, (uint64_t)0);
    atomic_store(&g_ua_total_outputs_spent, (uint64_t)0);
    atomic_store(&g_ua_last_step_unix, (int64_t)0);
    atomic_store(&g_ua_last_blocked_unix, (int64_t)0);
    atomic_store(&g_ua_last_advance_height, (int64_t)-1);
    atomic_store(&g_ua_upstream_hole_total, (uint64_t)0);
    atomic_store(&g_ua_upstream_hole_height, (int64_t)-1);
    atomic_store(&g_ua_upstream_hole_first_unix, (int64_t)0);
    atomic_store(&g_ua_upstream_hole_consec, (uint64_t)0);
    atomic_store(&g_ua_upstream_hole_warn_total, (uint64_t)0);
    atomic_store(&g_ua_label_splice_total, (uint64_t)0);
    utxo_apply_observe_reset();
    pthread_mutex_unlock(&g_lock);
}

void utxo_apply_stage_set_reader(utxo_apply_reader_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_reader = fn;
    g_reader_user = user;
    pthread_mutex_unlock(&g_lock);
}

void utxo_apply_stage_set_lookup(utxo_apply_lookup_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_lookup = fn;
    g_lookup_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t utxo_apply_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

/* Internal (utxo_apply_stage_internal.h): the dump TU reads the live stage
 * handle lock-free, exactly as the pre-extraction in-TU dump did. */
stage_t *utxo_apply_stage_handle(void)
{
    return g_stage;
}

/* utxo_apply_stage_succeeded_at + the per-status counter total accessors
 * live in utxo_apply_stage_accessors.c; utxo_apply_dump_state_json (+ its
 * first-failure summary helper) lives in utxo_apply_stage_dump.c — see
 * utxo_apply_stage_internal.h. */
