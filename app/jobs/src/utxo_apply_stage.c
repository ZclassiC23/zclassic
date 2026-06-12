/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage — implementation. See jobs/utxo_apply_stage.h.
 *
 * Consumes proof_validate_log and computes a transparent UTXO delta.
 * It writes only utxo_apply_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/utxo_apply_delta.h"
#include "jobs/utxo_apply_nullifiers.h"
#include "jobs/stage_helpers.h"
#include "utxo_apply_log_store.h"
#include "utxo_apply_stage_internal.h"
#include "script_validate_log_store.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "services/seal_service.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/coins_kv.h"
#include "storage/disk_block_io.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "coins/coins.h"
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

/* CS-F4 blocked-reject dedup. A consensus-rejected height returns
 * JOB_BLOCKED and recomputes every tick BY DESIGN (an upstream repair can
 * clear it), but the per-status totals must count BLOCKS — not ticks — and
 * re-emitting the same EV_BLOCK_REJECTED every tick evicts the
 * original-failure evidence from the bounded event ring. Counter + emit
 * fire only on a NEW (height,status) pair, plus a keep-alive re-emit every
 * REJECT_REEMIT_SECS carrying the cumulative suppressed-tick count (counter
 * NOT re-bumped). Cleared on JOB_ADVANCED (and shutdown) so a pair re-hit
 * after a rewind-then-reapply counts as a new block. g_reject_lock guards
 * the non-atomic fields. */
#define REJECT_REEMIT_SECS 300
static _Atomic int64_t g_last_reject_height = -1;
static pthread_mutex_t g_reject_lock = PTHREAD_MUTEX_INITIALIZER;
static char     g_last_reject_status[24];
static int64_t  g_last_reject_emit_unix;
static uint64_t g_reject_repeat_ticks;

static void reject_count_and_emit(int height, const char *status,
                                  _Atomic uint64_t *counter,
                                  const char *label)
{
    if (!status) status = "unknown";
    int64_t now = platform_time_wall_unix();
    uint64_t repeats = 0;
    pthread_mutex_lock(&g_reject_lock);
    bool same = atomic_load(&g_last_reject_height) == (int64_t)height &&
                strncmp(g_last_reject_status, status,
                        sizeof(g_last_reject_status) - 1) == 0;
    if (same) {
        repeats = ++g_reject_repeat_ticks;
        if (now - g_last_reject_emit_unix < REJECT_REEMIT_SECS) {
            pthread_mutex_unlock(&g_reject_lock);
            return;
        }
    } else {
        atomic_store(&g_last_reject_height, (int64_t)height);
        snprintf(g_last_reject_status, sizeof(g_last_reject_status), "%s",
                 status);
        g_reject_repeat_ticks = 0;
    }
    g_last_reject_emit_unix = now;
    pthread_mutex_unlock(&g_reject_lock);
    if (!same) {
        atomic_fetch_add(counter, 1);
        event_emitf(EV_BLOCK_REJECTED, 0, "utxo_apply %s height=%d",
                    label, height);
    } else {
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply %s height=%d repeats=%llu",
                    label, height, (unsigned long long)repeats);
    }
}

static void reject_memo_clear(void)
{
    pthread_mutex_lock(&g_reject_lock);
    atomic_store(&g_last_reject_height, (int64_t)-1);
    g_last_reject_status[0] = '\0';
    g_reject_repeat_ticks = 0;
    pthread_mutex_unlock(&g_reject_lock);
}

/* FIX-4 durable-upstream-hole observability (counters declared above, dumped
 * by utxo_apply_stage_dump.c). The transition memo is touched ONLY by the
 * single-threaded step path — the g_cursor_gap_warn convention from
 * tip_finalize_stage.c — so it needs no lock. */
static struct { int64_t h; uint64_t reps; int64_t last_log; }
    g_upstream_hole_warn = { .h = -1 };

/* label_splice WARN memo — same shape/contract as g_upstream_hole_warn. */
static struct { int64_t h; uint64_t reps; int64_t last_log; }
    g_label_splice_warn = { .h = -1 };

/* tipfin_warn_throttled shape (tip_finalize_stage.c:78): emit on a height
 * TRANSITION (first occurrence never suppressed) or as a 300 s keep-alive
 * carrying the suppressed-tick count; suppressed calls still count. */
static bool upstream_hole_warn_throttled(bool changed, int64_t now,
                                         uint64_t *reps, int64_t *last_log,
                                         uint64_t *out_shown)
{
    if (changed) {
        *out_shown = *reps;  /* repeats of the PRIOR hole */
        *reps = 0;
        *last_log = now;
        return true;
    }
    *out_shown = ++*reps;
    if (now - *last_log < 300) return false;
    *last_log = now;
    return true;
}

static void upstream_hole_note(int height, uint64_t pv_cursor)
{
    int64_t now = platform_time_wall_unix();
    bool changed = g_upstream_hole_warn.h != (int64_t)height;
    if (changed) {
        g_upstream_hole_warn.h = (int64_t)height;
        atomic_fetch_add(&g_ua_upstream_hole_total, 1);
        atomic_store(&g_ua_upstream_hole_height, (int64_t)height);
        atomic_store(&g_ua_upstream_hole_first_unix, now);
        atomic_store(&g_ua_upstream_hole_consec, (uint64_t)1);
    } else {
        atomic_fetch_add(&g_ua_upstream_hole_consec, 1);
    }
    uint64_t shown = 0;
    if (upstream_hole_warn_throttled(changed, now,
                                     &g_upstream_hole_warn.reps,
                                     &g_upstream_hole_warn.last_log,
                                     &shown)) {
        atomic_fetch_add(&g_ua_upstream_hole_warn_total, 1);
        LOG_WARN(STAGE_NAME,
                 "[utxo_apply] durable upstream hole: proof_validate_log row "
                 "ABSENT at height=%d while proof_validate cursor=%llu is "
                 "already past it (suppressed=%llu); stage stays JOB_IDLE by "
                 "design — the reconcile-light Condition is the healer, this "
                 "WARN/dump is the alarm",
                 height, (unsigned long long)pv_cursor,
                 (unsigned long long)shown);
    }
}

/* The row at the previously-holed height is present again: close the
 * transition memo so a RE-opened hole at the same height counts and logs as
 * a new hole, and zero consec (the dump's "currently observing" signal).
 * total/height/first_unix stay as last-hole evidence. */
static void upstream_hole_healed(int height)
{
    if (g_upstream_hole_warn.h == (int64_t)height) {
        g_upstream_hole_warn.h = -1;
        g_upstream_hole_warn.reps = 0;
        g_upstream_hole_warn.last_log = 0;
        atomic_store(&g_ua_upstream_hole_consec, (uint64_t)0);
    }
}

/* Author the validated block delta into the progress.kv `coins` table
 * (coins_kv) on the stage's own db handle, INSIDE stage_run_once's BEGIN
 * IMMEDIATE — the coin mutation commits or rolls back as ONE atomic unit
 * with the cursor + inverse-delta + log row, closing the tip-wedge tear
 * class (docs/work/tip-durability-collapse.md). coins_kv is the SOLE live
 * UTXO author and read source (the projection is seed-only). Adds before
 * spends: created keys are unique (compute_block_delta rejects collisions)
 * and intra-block create-then-spend resolves to "absent", so the final set
 * is order-independent. */
static bool apply_coins_kv(sqlite3 *db, const struct delta_summary *s,
                           uint32_t height)
{
    for (size_t i = 0; i < s->added_count; i++)
        if (!coins_kv_add(db, s->added[i].txid.data, s->added[i].vout,
                          s->added[i].value, (int32_t)height,
                          s->added[i].is_coinbase,
                          s->added[i].script, s->added[i].script_len))
            return false;
    for (size_t i = 0; i < s->spent_count; i++)
        if (!coins_kv_spend(db, s->spent[i].txid.data, s->spent[i].vout))
            return false;
    return true;
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

static job_result_t step_apply(struct stage_step_ctx *c)
{
    atomic_store(&g_ua_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t pv_cursor = stage_cursor_persisted(db, "proof_validate",
                                               STAGE_NAME);
    if ((uint64_t)next_h >= pv_cursor) {
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct proof_validate_row upstream;
    int found = utxo_apply_proof_validate_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        /* Reaching here implies next_h < pv_cursor (the >= guard above
         * returned otherwise), so this is a DURABLE upstream hole — a
         * stale-replay / self-restart artifact — never "not yet" (FIX-4).
         * DELIBERATELY JOB_IDLE, not JOB_BLOCKED: JOB_BLOCKED feeds the
         * supervisor escalation/restart ladder, and a watchdog self-restart
         * is what manufactured this hole class in the first place. The L1
         * reconcile-light Condition is the healer; the counters/WARN here
         * are the alarm. */
        upstream_hole_note(next_h, pv_cursor);
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }
    upstream_hole_healed(next_h);

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
     * height-splice class (forensic 2026-06-11, splice at h=3143355). Stage
     * logs are keyed BY HEIGHT, so after an in-memory header relabel the
     * script_validate_log row at next_h can be the verdict for a DIFFERENT
     * block than the one chain[] now exposes; applying with that stale
     * verdict tears the coin set at the splice height (the TRUE block never
     * applies; surfaces ~28 labels later as bad-cb-height). Same
     * hash-identity guard as tip_finalize's finalize_script_log_ok: a row
     * provably bound to another hash refuses the apply with a typed
     * transient blocker until script_validate re-binds the height. A NULL
     * (pre-column) hash or an absent row cannot prove a mismatch and passes
     * through — the proof_validate cursor guard above covers ordering. */
    struct script_validate_verdict_row sv_row;
    int sv_found = script_validate_log_verdict_at(db, next_h, &sv_row);
    if (sv_found < 0)
        return JOB_FATAL;
    if (sv_found == 1 && sv_row.has_block_hash &&
        !uint256_eq(&sv_row.block_hash, bi->phashBlock)) {
        char want_hex[65] = {0}, got_hex[65] = {0};
        uint256_get_hex(bi->phashBlock, want_hex);
        uint256_get_hex(&sv_row.block_hash, got_hex);
        /* WARN on a height TRANSITION or as a 300 s keep-alive (memo touched
         * only by this single-threaded step path — the g_upstream_hole_warn
         * convention); the counter counts refusal HEIGHTS, not ticks. The
         * blocker registry's rate limit dedups the JOB_BLOCKED re-fires. */
        int64_t now_w = platform_time_wall_unix();
        bool changed = g_label_splice_warn.h != (int64_t)next_h;
        if (changed) {
            g_label_splice_warn.h = (int64_t)next_h;
            atomic_fetch_add(&g_ua_label_splice_total, 1);
        }
        uint64_t shown = 0;
        if (upstream_hole_warn_throttled(changed, now_w,
                                         &g_label_splice_warn.reps,
                                         &g_label_splice_warn.last_log,
                                         &shown))
            LOG_WARN(STAGE_NAME,
                     "[utxo_apply] label_splice height=%d: script_validate_log "
                     "row is hash-bound to %s but the block being applied is "
                     "%s; refusing apply until the verdict is re-bound "
                     "(suppressed=%llu)",
                     next_h, got_hex, want_hex, (unsigned long long)shown);
        char reason[BLOCKER_REASON_MAX];
        snprintf(reason, sizeof(reason),
                 "height=%d script_validate_log block_hash %.16s.. != "
                 "applying block %.16s..; height-keyed verdict belongs to a "
                 "different block (label splice) — apply refused",
                 next_h, got_hex, want_hex);
        blocker_init(&c->blocker, "utxo_apply.label_splice", STAGE_NAME,
                     BLOCKER_TRANSIENT, reason);
        c->blocker.escape_deadline_secs = 60;
        c->blocker.retry_budget = 5;
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_BLOCKED;
    }
    if (g_label_splice_warn.h == (int64_t)next_h) {
        /* The previously-refused verdict is re-bound (or no longer provably
         * foreign): close the memo so a re-opened splice counts as new. */
        g_label_splice_warn.h = -1;
        g_label_splice_warn.reps = 0;
        g_label_splice_warn.last_log = 0;
    }

    struct block blk;
    block_init(&blk);
    utxo_apply_reader_fn reader = g_reader ? g_reader
                                           : stage_default_block_reader;
    if (!reader(&blk, bi, g_datadir, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_ua_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct delta_summary summary;
    utxo_apply_compute_block_delta(&blk, (uint32_t)next_h,
                                   g_lookup, g_lookup_user, &summary);

    /* Shielded-nullifier double-spend gate (C-3) — BEFORE the coins write,
     * under the same author gate. May flip summary.ok to a consensus reject
     * (which then takes the regular counter/log/JOB_BLOCKED path below); a
     * store error is fatal like any other in-txn store failure. */
    if (summary.ok && utxo_projection_get_author() == UTXO_AUTHOR_STAGE) {
        if (!utxo_apply_check_and_insert_nullifiers(db, &blk, next_h,
                                                    &summary)) {
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
        reject_count_and_emit(next_h, summary.status, counter, label);
    }

    if (!utxo_apply_log_insert(db, next_h, summary.status, summary.ok,
                    summary.spent_count, summary.added_count,
                    summary.total_value_delta, summary.failure_kind,
                    summary.ok ? NULL : summary.failure_detail)) {
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
    if (!coins_kv_set_applied_height_in_tx(db, (int32_t)next_cursor))
        return JOB_FATAL;
    seal_candidate_hook_in_tx(db, g_ms, (int32_t)next_cursor);

    atomic_store(&g_ua_last_advance_height, (int64_t)next_h);
    reject_memo_clear();
    c->cursor_out = next_cursor;
    return JOB_ADVANCED;
}

/* Production prevout resolver for utxo_apply, the init-time default for
 * g_lookup — the analogue of script_validate's created_index_prevout
 * self-default, but with the CORRECT semantics for utxo_apply: found must
 * mean "currently UNSPENT". coins_kv DELETEs a coin on spend, so a hit from
 * coins_kv_get_coins == the coin is live (double-spend-safe); a creation
 * index (which never deletes spent rows) would report found=true for an
 * already-spent coin and let utxo_apply accept a double-spend (monetary
 * inflation / hard fork) AND false-trip BIP30 collision — it MUST NOT be
 * used here. The full pre-image (value/height/is_coinbase/script) is
 * required for the inverse-delta restore-ADD. A genuine miss returns
 * found=false (compute_block_delta then records spend_unknown_utxo with the
 * exact outpoint — never a silent pass).
 *
 * FRESHNESS CONTRACT: reads the authoritative coins set on the progress.kv
 * handle inside the apply path's progress_store_tx_lock, and apply_coins_kv
 * authors coins_kv IN the stage txn — a coin created by an earlier block is
 * already committed before a later block's step_apply resolves it, so reads
 * are inherently fresh with no catch_up dependency (the projection's
 * last_consumed_offset freshness hack is gone). */
static bool projection_live_lookup(const struct uint256 *txid, uint32_t vout,
                                   struct utxo_apply_lookup *out, void *user)
{
    (void)user;
    if (!txid || !out)
        return false;
    memset(out, 0, sizeof(*out));

    sqlite3 *db = progress_store_db();
    if (!db)
        return true;   /* store not open yet → treat as absent (found=0),
                        * matching the lookup==NULL "all external absent"
                        * contract; never a false-accept. */

    struct coins c;
    coins_init(&c);
    if (!coins_kv_get_coins(db, txid->data, &c)) {
        coins_free(&c);
        return true;   /* no live output at this txid → found stays false */
    }

    bool ok = true;
    if (vout < c.num_vout && !tx_out_is_null(&c.vout[vout])) {
        const struct tx_out *o = &c.vout[vout];
        size_t slen = o->script_pub_key.size;
        if (slen > UTXO_APPLY_SCRIPT_MAX) {
            /* Contract violation (a UTXO scriptPubKey is <= MAX_SCRIPT_SIZE ==
             * UTXO_APPLY_SCRIPT_MAX). Fail the resolver (compute_block_delta
             * turns this into an internal_error) rather than truncate or
             * over-read a consensus script. */
            ok = false;
        } else {
            out->found       = true;
            out->value       = o->value;
            out->height      = (uint32_t)(c.height < 0 ? 0 : c.height);
            out->is_coinbase = c.is_coinbase;
            out->script_len  = (uint32_t)slen;
            if (slen)
                memcpy(out->script, o->script_pub_key.data, slen);
        }
    }
    coins_free(&c);
    return ok;
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
     * transparent spend as spend_unknown_utxo (live-wedge blocker #5). */
    if (!g_lookup)
        g_lookup = projection_live_lookup;
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
        return JOB_FATAL;
    }
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    /* No projection catch_up fold needed: projection_live_lookup reads the
     * coins_kv rows apply_coins_kv writes IN this stage's BEGIN IMMEDIATE
     * (see its FRESHNESS CONTRACT). */
    return r;
}

STAGE_DRAIN_IMPL(utxo_apply)

void utxo_apply_stage_shutdown(void)
{
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
    g_upstream_hole_warn.h = -1;
    g_upstream_hole_warn.reps = 0;
    g_upstream_hole_warn.last_log = 0;
    g_label_splice_warn.h = -1;
    g_label_splice_warn.reps = 0;
    g_label_splice_warn.last_log = 0;
    reject_memo_clear();
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
