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

#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
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
 * layer).
 *
 * struct delta_entry / struct delta_summary plus inverse-delta persistence
 * and reorg-unwind machinery live in jobs/utxo_apply_delta.h /
 * utxo_apply_delta.c. */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static utxo_apply_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;
static utxo_apply_lookup_fn g_lookup = NULL;
static void *g_lookup_user = NULL;

static _Atomic uint64_t g_verified_total = 0;
static _Atomic uint64_t g_spend_unknown_total = 0;
static _Atomic uint64_t g_utxo_collision_total = 0;
static _Atomic uint64_t g_value_overflow_total = 0;
static _Atomic uint64_t g_coinbase_protect_total = 0;
static _Atomic uint64_t g_bad_cb_amount_total = 0;
static _Atomic uint64_t g_shielded_double_spend_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_internal_error_total = 0;
static _Atomic uint64_t g_reorg_unwound_total = 0;
static _Atomic uint64_t g_total_outputs_added = 0;
static _Atomic uint64_t g_total_outputs_spent = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;

/* The delta structs, free_delta(_arr), and the block-delta builder
 * (utxo_apply_compute_block_delta) live in utxo_apply_delta.c, shared
 * with the inverse-delta persistence + reorg-unwind path. */

/* Author the validated block delta into the progress.kv `coins` table (coins_kv)
 * on the stage's own db handle, so it lands INSIDE stage_run_once's BEGIN
 * IMMEDIATE — the coin mutation commits or rolls back as ONE atomic unit with
 * the cursor + inverse-delta + log row, closing the tip-wedge tear class
 * (docs/work/tip-durability-collapse.md). coins_kv is now the SOLE live UTXO
 * author and read source (the projection is seed-only). Adds are applied before
 * spends: every UTXO key created in this block is unique (compute_block_delta
 * rejects collisions), and the only intra-block key interaction is
 * create-then-spend of the same output — which add-then-spend resolves to
 * "absent". The coins set is a set, so the final state is order-independent. */
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

/* compute_block_delta now lives in utxo_apply_delta.c as
 * utxo_apply_compute_block_delta (it owns the delta structs + the
 * persistence/inversion of the same arrays). */

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
    atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
    return JOB_BLOCKED;
}

static job_result_t step_apply(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t pv_cursor = stage_cursor_persisted(db, "proof_validate",
                                               STAGE_NAME);
    if ((uint64_t)next_h >= pv_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct proof_validate_row upstream;
    int found = utxo_apply_proof_validate_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        atomic_fetch_add(&g_upstream_failed_total, 1);
        return block_apply_failure(c, next_h, "upstream_failed",
                                   "proof_validate", NULL);
    }

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA)) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct block blk;
    block_init(&blk);
    utxo_apply_reader_fn reader = g_reader ? g_reader
                                           : stage_default_block_reader;
    if (!reader(&blk, bi, g_datadir, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
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

    /* Author the canonical coins_kv set from this validated delta when the
     * stage holds UTXO authority. coins_kv is written in-txn so a failure
     * here rolls back the whole stage txn (cursor + inverse-delta + log row
     * + coins) — never a torn partial apply. Scripts in `summary.added`
     * alias into `blk`, so apply before block_free. The author gate is
     * retained (UTXO_AUTHOR_STAGE) — it now guards the coins_kv write, the
     * sole live UTXO author after the projection dual-write was removed. */
    if (summary.ok && utxo_projection_get_author() == UTXO_AUTHOR_STAGE) {
        if (!apply_coins_kv(db, &summary, (uint32_t)next_h)) {
            free_delta(&summary);
            block_free(&blk);
            return JOB_FATAL;
        }
    }

    /* Persist the per-block inverse-delta so a later disconnect can be
     * reconstructed without re-reading legacy undo files. Stamped with the
     * OLD branch hash so a fork at the same height is distinguishable. Inside
     * the stage txn (stage_run_once's BEGIN IMMEDIATE), so the delta + log row
     * + cursor land atomically. Persisted only on a successful apply; failure
     * rows have nothing to invert. */
    if (summary.ok) {
        if (!utxo_apply_delta_persist(db, next_h, bi->phashBlock, &summary)) {
            free_delta(&summary);
            block_free(&blk);
            return JOB_FATAL;
        }
    }

    if (summary.ok) {
        atomic_fetch_add(&g_verified_total, 1);
        atomic_fetch_add(&g_total_outputs_added,
                         (uint64_t)summary.added_count);
        atomic_fetch_add(&g_total_outputs_spent,
                         (uint64_t)summary.spent_count);
    } else if (strcmp(summary.status, "spend_unknown_utxo") == 0) {
        atomic_fetch_add(&g_spend_unknown_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply spend_unknown_utxo height=%d", next_h);
    } else if (strcmp(summary.status, "utxo_collision") == 0) {
        atomic_fetch_add(&g_utxo_collision_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply utxo_collision height=%d", next_h);
    } else if (strcmp(summary.status, "value_overflow") == 0) {
        atomic_fetch_add(&g_value_overflow_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply value_overflow height=%d", next_h);
    } else if (strcmp(summary.status, "coinbase_protect") == 0) {
        atomic_fetch_add(&g_coinbase_protect_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply bad-txns-coinbase-spend-has-transparent-outputs "
                    "height=%d", next_h);
    } else if (strcmp(summary.status, "bad_cb_amount") == 0) {
        atomic_fetch_add(&g_bad_cb_amount_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply bad-cb-amount height=%d", next_h);
    } else if (strcmp(summary.status, "shielded_double_spend") == 0) {
        atomic_fetch_add(&g_shielded_double_spend_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply bad-txns-joinsplit-requirements-not-met "
                    "height=%d", next_h);
    } else {
        atomic_fetch_add(&g_internal_error_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "utxo_apply internal_error height=%d", next_h);
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

    /* Co-commit the contiguous applied frontier = the SAME value written to the
     * stage cursor (cursor_in + 1), so coins_applied_height == utxo_apply cursor
     * by construction on every successful apply. Failed verdicts return
     * JOB_BLOCKED above, the framework rolls back their scratch log row, and
     * the cursor/frontier stay at the unresolved height. That fail-closed
     * policy prevents a mixed coins window where later heights are applied over
     * an un-applied hole. */
    uint64_t next_cursor = c->cursor_in + 1;
    if (!coins_kv_set_applied_height_in_tx(db, (int32_t)next_cursor))
        return JOB_FATAL;

    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = next_cursor;
    return JOB_ADVANCED;
}

/* Production prevout resolver for utxo_apply, the init-time default for
 * g_lookup — the analogue of script_validate's created_index_prevout
 * self-default, but with the CORRECT semantics for utxo_apply: it must mean
 * "currently UNSPENT". coins_kv DELETEs a coin on spend, so a hit from
 * coins_kv_get_coins == the coin is live/unspent. This is the
 * double-spend-safe source; a creation index (which never deletes spent rows)
 * would report found=true for an already-spent coin and let utxo_apply accept
 * a double-spend (monetary inflation / hard fork) AND false-trip BIP30
 * collision — so it MUST NOT be used here. The full pre-image
 * (value/height/is_coinbase/script) is required for the inverse-delta
 * restore-ADD. A genuine miss returns found=false (compute_block_delta then
 * records spend_unknown_utxo with the exact outpoint — never a silent pass).
 *
 * FRESHNESS CONTRACT: this reads the authoritative coins set (coins_kv) on the
 * progress.kv handle. Because utxo_apply authors coins_kv IN the stage txn (step
 * 2/3), a coin created by an earlier block is already committed to coins_kv
 * before a later block's step_apply runs — reads are inherently fresh with no
 * catch_up dependency (the projection's last_consumed_offset freshness hack is
 * gone). The read runs inside the apply path's progress_store_tx_lock, so it is
 * consistent with the apply txn. coins_kv DELETEs a coin on spend, so a hit ==
 * the coin is live/unspent (double-spend-safe); a genuine miss returns
 * found=false (compute_block_delta records spend_unknown_utxo with the exact
 * outpoint — never a silent pass). */
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
    /* Wire the production UTXO-set resolver unless a caller (e.g. a test)
     * already installed one. Without it g_lookup stays NULL and
     * utxo_apply_compute_block_delta treats EVERY external coin as absent,
     * rejecting every cross-block transparent spend as spend_unknown_utxo
     * (live-wedge blocker #5). Symmetric with script_validate's
     * created_index_prevout self-default (script_validate_stage.c). */
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
                                          &g_reorg_unwound_total,
                                          &g_last_blocked_unix);
    if (!unwind_ok) {
        progress_store_tx_unlock();
        return JOB_FATAL;
    }
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    /* No projection catch_up fold: the prevout resolver (projection_live_lookup)
     * reads coins_kv, which apply_coins_kv writes IN this stage's BEGIN
     * IMMEDIATE — a coin created by an earlier block is already committed to
     * coins_kv before a later block in the same drain resolves it, so reads are
     * inherently fresh with no fold needed (the projection's last_consumed_offset
     * freshness hack is gone with the dual-write). */
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
    atomic_store(&g_verified_total, (uint64_t)0);
    atomic_store(&g_spend_unknown_total, (uint64_t)0);
    atomic_store(&g_utxo_collision_total, (uint64_t)0);
    atomic_store(&g_value_overflow_total, (uint64_t)0);
    atomic_store(&g_coinbase_protect_total, (uint64_t)0);
    atomic_store(&g_bad_cb_amount_total, (uint64_t)0);
    atomic_store(&g_shielded_double_spend_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_internal_error_total, (uint64_t)0);
    atomic_store(&g_reorg_unwound_total, (uint64_t)0);
    atomic_store(&g_total_outputs_added, (uint64_t)0);
    atomic_store(&g_total_outputs_spent, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
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

bool utxo_apply_stage_succeeded_at(int height)
{
    if (height < 0)
        return false;
    sqlite3 *db = progress_store_db();
    if (!db)
        return false;
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM utxo_apply_log WHERE height = ?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, height);
        if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
            ok = sqlite3_column_int(st, 0) == 1;
        sqlite3_finalize(st);
    }
    progress_store_tx_unlock();
    return ok;
}

uint64_t utxo_apply_stage_verified_total(void)
{
    return atomic_load(&g_verified_total);
}

uint64_t utxo_apply_stage_spend_unknown_total(void)
{
    return atomic_load(&g_spend_unknown_total);
}

uint64_t utxo_apply_stage_utxo_collision_total(void)
{
    return atomic_load(&g_utxo_collision_total);
}

uint64_t utxo_apply_stage_value_overflow_total(void)
{
    return atomic_load(&g_value_overflow_total);
}

uint64_t utxo_apply_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t utxo_apply_stage_internal_error_total(void)
{
    return atomic_load(&g_internal_error_total);
}

uint64_t utxo_apply_stage_reorg_unwound_total(void)
{
    return atomic_load(&g_reorg_unwound_total);
}

uint64_t utxo_apply_stage_outputs_added_total(void)
{
    return atomic_load(&g_total_outputs_added);
}

uint64_t utxo_apply_stage_outputs_spent_total(void)
{
    return atomic_load(&g_total_outputs_spent);
}

/* Surface the lowest ok=0 row (status/reason kind/txid/vout) into `out`,
 * mirroring the validate_headers_report failure-summary query convention.
 * The reason kind is utxo_apply's first_failure_kind (e.g. lookup_spend,
 * spend_unknown_utxo); the txid|vout is decoded from the 36-byte detail
 * blob. No-op if the db is unavailable or there is no failing row. Takes
 * its own tx lock since dump_state runs outside any stage txn. */
static void dump_first_failure(struct json_value *out, sqlite3 *db)
{
    if (!db) return;
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT height, COALESCE(status,''), "
        "       COALESCE(first_failure_kind,''), first_failure_detail "
        "  FROM utxo_apply_log WHERE ok=0 "
        " ORDER BY height ASC LIMIT 1",
        -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        json_push_kv_int(out, "first_failure_height",
                         sqlite3_column_int64(st, 0));
        const unsigned char *status = sqlite3_column_text(st, 1);
        const unsigned char *kind = sqlite3_column_text(st, 2);
        json_push_kv_str(out, "first_failure_status",
                         status ? (const char *)status : "");
        json_push_kv_str(out, "first_failure_kind",
                         kind ? (const char *)kind : "");
        const uint8_t *d = sqlite3_column_blob(st, 3);
        char hex[65] = {0};
        int64_t vout = -1;
        if (d && sqlite3_column_bytes(st, 3) == 36) {
            struct uint256 t;
            memcpy(t.data, d, 32);
            uint256_get_hex(&t, hex);
            vout = (int64_t)d[32] | ((int64_t)d[33] << 8) |
                   ((int64_t)d[34] << 16) | ((int64_t)d[35] << 24);
        }
        json_push_kv_str(out, "first_failure_txid", hex);
        json_push_kv_int(out, "first_failure_vout", vout);
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();
}

bool utxo_apply_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = atomic_load(&g_last_step_unix);

    stage_dump_header(out, STAGE_NAME, g_stage);
    json_push_kv_int (out, "verified_total",
                      (int64_t)atomic_load(&g_verified_total));
    json_push_kv_int (out, "spend_unknown_total",
                      (int64_t)atomic_load(&g_spend_unknown_total));
    json_push_kv_int (out, "utxo_collision_total",
                      (int64_t)atomic_load(&g_utxo_collision_total));
    json_push_kv_int (out, "value_overflow_total",
                      (int64_t)atomic_load(&g_value_overflow_total));
    json_push_kv_int (out, "coinbase_protect_total",
                      (int64_t)atomic_load(&g_coinbase_protect_total));
    json_push_kv_int (out, "bad_cb_amount_total",
                      (int64_t)atomic_load(&g_bad_cb_amount_total));
    json_push_kv_int (out, "shielded_double_spend_total",
                      (int64_t)atomic_load(&g_shielded_double_spend_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "internal_error_total",
                      (int64_t)atomic_load(&g_internal_error_total));
    json_push_kv_int (out, "reorg_unwound_total",
                      (int64_t)atomic_load(&g_reorg_unwound_total));
    json_push_kv_int (out, "outputs_added_total",
                      (int64_t)atomic_load(&g_total_outputs_added));
    json_push_kv_int (out, "outputs_spent_total",
                      (int64_t)atomic_load(&g_total_outputs_spent));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds",
                      last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "log_rows",
                      db ? stage_log_row_count(db, STAGE_NAME,
                                               "utxo_apply_log") : 0);

    /* P2 self-heal input: the contiguous applied frontier and whether it equals
     * the durable utxo_apply cursor (the invariant the co-commit sites enforce).
     * Surfaced here so `zcl_state subsystem=utxo_apply` shows the invariant
     * directly — frontier_eq_cursor must be true on every quiescent path.
     * coins_applied_height == -1 means ABSENT (a virgin / un-synced datadir),
     * which is a clean "unknown", not a violation. */
    if (db) {
        int32_t frontier = -1;
        bool fr_found = false;
        bool fr_ok = coins_kv_get_applied_height(db, &frontier, &fr_found);
        uint64_t ua_cursor = stage_cursor_persisted(db, STAGE_NAME, STAGE_NAME);
        json_push_kv_int(out, "coins_applied_height",
                         (fr_ok && fr_found) ? (int64_t)frontier : -1);
        json_push_kv_bool(out, "frontier_eq_cursor",
                          fr_ok && fr_found &&
                          (uint64_t)frontier == ua_cursor);
    }
    dump_first_failure(out, db);
    stage_dump_counters(out, g_stage);
    return true;
}
