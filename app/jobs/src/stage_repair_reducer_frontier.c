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
#include "platform/time_compat.h"
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
#include <string.h>

struct rf_log_evidence {
    bool validate_ok_hash;
    bool script_ok_hash;
    bool body_ok;
    bool proof_ok;
    bool utxo_ok;
};

/* The five per-height evidence point reads, prepared ONCE per flag sweep and
 * reset/rebound per height (created_outputs_index_put_block's prepare-once
 * batch idiom): the sweep walks every height in (hstar, sweep_top] and a
 * per-row sqlite3_prepare_v2 of constant SQL paid the SQL parser five times
 * per height while holding the progress + cs_main locks. */
struct rf_evidence_stmts {
    sqlite3_stmt *validate_hash; /* ok, hash       FROM validate_headers_log */
    sqlite3_stmt *script_hash;   /* ok, block_hash FROM script_validate_log  */
    sqlite3_stmt *body_ok;       /* ok FROM body_persist_log  */
    sqlite3_stmt *proof_ok;      /* ok FROM proof_validate_log */
    sqlite3_stmt *utxo_ok;       /* ok FROM utxo_apply_log    */
};

static void rf_evidence_stmts_finalize(struct rf_evidence_stmts *es)
{
    sqlite3_finalize(es->validate_hash);
    sqlite3_finalize(es->script_hash);
    sqlite3_finalize(es->body_ok);
    sqlite3_finalize(es->proof_ok);
    sqlite3_finalize(es->utxo_ok);
    memset(es, 0, sizeof(*es));
}

static bool rf_evidence_stmts_prepare(sqlite3 *db, struct rf_evidence_stmts *es)
{
    memset(es, 0, sizeof(*es));
    if (sqlite3_prepare_v2(db,
            "SELECT ok, hash FROM validate_headers_log WHERE height = ?",
            -1, &es->validate_hash, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
            "SELECT ok, block_hash FROM script_validate_log WHERE height = ?",
            -1, &es->script_hash, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
            "SELECT ok FROM body_persist_log WHERE height = ?",
            -1, &es->body_ok, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
            "SELECT ok FROM proof_validate_log WHERE height = ?",
            -1, &es->proof_ok, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
            "SELECT ok FROM utxo_apply_log WHERE height = ?",
            -1, &es->utxo_ok, NULL) == SQLITE_OK)
        return true;

    LOG_WARN("stage_repair",
             "[stage_repair] evidence stmt prepare failed: %s",
             sqlite3_errmsg(db));
    rf_evidence_stmts_finalize(es);
    return false;
}

static bool log_ok_unlocked(sqlite3_stmt *st, const char *table, int height,
                            bool *found, bool *ok)
{
    *found = false;
    *ok = false;

    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, height);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found = true;
        *ok = sqlite3_column_int(st, 0) == 1;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] log_ok step failed table=%s h=%d rc=%d: %s",
                 table, height, rc, sqlite3_errmsg(sqlite3_db_handle(st)));
        return false;
    }

    return true;
}

static bool hash_log_ok_matches_unlocked(sqlite3_stmt *st, const char *table,
                                         int height,
                                         const struct uint256 *want,
                                         bool *matches)
{
    *matches = false;
    if (!want)
        return true;

    sqlite3_reset(st);
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
                 table, height, rc, sqlite3_errmsg(sqlite3_db_handle(st)));
        return false;
    }

    return true;
}

static bool evidence_for_block_unlocked(struct rf_evidence_stmts *es,
                                        const struct block_index *bi,
                                        struct rf_log_evidence *ev)
{
    memset(ev, 0, sizeof(*ev));
    if (!bi || !bi->phashBlock)
        return true;

    if (!hash_log_ok_matches_unlocked(es->validate_hash,
                                      "validate_headers_log",
                                      bi->nHeight, bi->phashBlock,
                                      &ev->validate_ok_hash))
        return false;
    if (!hash_log_ok_matches_unlocked(es->script_hash,
                                      "script_validate_log", bi->nHeight,
                                      bi->phashBlock, &ev->script_ok_hash))
        return false;

    bool found = false;
    if (!log_ok_unlocked(es->body_ok, "body_persist_log", bi->nHeight,
                         &found, &ev->body_ok))
        return false;
    ev->body_ok = found && ev->body_ok;

    found = false;
    if (!log_ok_unlocked(es->proof_ok, "proof_validate_log", bi->nHeight,
                         &found, &ev->proof_ok))
        return false;
    ev->proof_ok = found && ev->proof_ok;

    found = false;
    if (!log_ok_unlocked(es->utxo_ok, "utxo_apply_log", bi->nHeight,
                         &found, &ev->utxo_ok))
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

    int32_t coins_applied = 0;
    bool coins_found = false;
    if (!coins_kv_get_applied_height(db, &coins_applied, &coins_found)) {
        progress_store_tx_unlock();
        LOG_WARN("stage_repair",
                 "[stage_repair] coins_applied_height read failed");
        return false;
    }

    /* One read per stage cursor: the named indices below feed the result
     * fields, every cursor feeds sweep_top. */
    static const char *const stages[] = {
        "validate_headers", /* [0] */
        "body_fetch",       /* [1] */
        "body_persist",     /* [2] */
        "script_validate",
        "proof_validate",
        "utxo_apply",
        "tip_finalize",     /* [6] */
    };
    int cursors[sizeof(stages) / sizeof(stages[0])];
    int sweep_top = served_floor;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        cursors[i] = -1;
        if (!stage_repair_cursor_at_unlocked(db, stages[i], &cursors[i])) {
            progress_store_tx_unlock();
            return false;
        }
        if (cursors[i] > 0 && cursors[i] - 1 > sweep_top)
            sweep_top = cursors[i] - 1;
    }

    progress_store_tx_unlock();

    out->hstar = hstar;
    out->served_floor = served_floor;
    out->validate_headers_cursor_before = cursors[0];
    out->validate_headers_cursor_after = cursors[0];
    out->body_fetch_cursor_before = cursors[1];
    out->body_fetch_cursor_after = cursors[1];
    out->body_persist_cursor_before = cursors[2];
    out->body_persist_cursor_after = cursors[2];
    out->tip_finalize_cursor_before = cursors[6];
    out->tip_finalize_cursor_after = cursors[6];
    out->sweep_top = sweep_top;
    out->lowest_have_data_cleared = -1;
    out->lowest_validate_headers_refill_hole = -1;
    out->lowest_validate_headers_hash_split = -1;
    out->lowest_body_fetch_refill_hole = -1;
    out->lowest_body_persist_refill_hole = -1;
    out->lowest_script_validate_refill_hole = -1;
    out->lowest_proof_validate_refill_hole = -1;
    out->script_validate_cursor_before = -1;
    out->script_validate_cursor_after = -1;
    out->proof_validate_cursor_before = -1;
    out->proof_validate_cursor_after = -1;
    out->tipfin_backfill_height = -1;
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

    struct rf_evidence_stmts es;
    if (!rf_evidence_stmts_prepare(db, &es)) {
        zcl_mutex_unlock(&ms->cs_main);
        progress_store_tx_unlock();
        return false;
    }

    bool ok = true;
    size_t iter = 0;
    struct block_index *bi = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (!bi || bi->nHeight <= out->hstar || bi->nHeight > out->sweep_top)
            continue;

        struct rf_log_evidence ev;
        if (!evidence_for_block_unlocked(&es, bi, &ev)) {
            ok = false;
            break;
        }

        bool changed = false;
        /* Full-block read + rehash ONLY when the verdict can depend on it:
         * the HAVE_DATA-set check needs it only with validate+body evidence
         * present, the HAVE_DATA-clear check only when the flag is set. The
         * set-then-clear interleave cannot misread the gate: the set fires
         * only when `readable` just proved true, making the clear's
         * !readable false. Everything else below never reads `readable`. */
        bool readable = false;
        if ((bi->nStatus & BLOCK_HAVE_DATA) ||
            (ev.validate_ok_hash && ev.body_ok))
            readable = block_pos_readable_hash(bi, datadir);

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

    rf_evidence_stmts_finalize(&es);
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

/* ── detect-path memo ───────────────────────────────────────────────────
 * The reconcile-light Condition polls the dry-run every 5 s; on a stalled
 * node each poll paid the full pipeline sweep (compute_hstar log walks,
 * per-height evidence reads, hash-verified block reads) under the global
 * locks. A dry-run against an unchanged store is deterministic, so the last
 * CLEAN full-pipeline dry-run is cached keyed on sqlite3_total_changes64
 * (any row write through the handle invalidates) and re-swept at least
 * every RF_DETECT_MEMO_TTL_SECS to bound staleness from the two inputs the
 * change counter cannot see: in-memory block_index flags mutated with no
 * progress.kv write, and block-file bytes on disk. Engages ONLY on the
 * progress_store_db() singleton, whose handle and change counter are stable
 * for the process lifetime (a closed-and-reopened private db could replay
 * both the pointer and the counter). Actionable, refused, or early-returned
 * results are never cached; every apply run invalidates. Plain statics:
 * detect and remedy both run on the serial condition-engine tick thread
 * (the g_cursor_gap_warn convention); test callers are serial. */
#define RF_DETECT_MEMO_TTL_SECS 60
static struct {
    bool valid;
    int64_t swept_unix;
    int64_t total_changes;
    struct stage_reducer_frontier_reconcile_result result;
} g_detect_memo;

/* Cache only a result the Condition treats as fully quiet: no repair, no
 * refusal, and none of the repair-evidence side channels that
 * repair_evidence_pending() (the Condition's gate-loudness probe) reads. */
static bool rf_result_clean(
    const struct stage_reducer_frontier_reconcile_result *r)
{
    return !r->repaired &&
           !r->refused_coin_tear &&
           !r->refused_coin_unknown &&
           !r->value_overflow_repair_attempted &&
           !r->value_overflow_repair_owner_refused &&
           !r->stale_script_repair_attempted &&
           !r->coin_backfill_attempted &&
           !r->coin_backfill_owner_refused &&
           r->tipfin_backfill_count == 0 &&
           r->tipfin_backfill_refused_reason == 0 &&
           r->noncanonical_found == 0;
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

    bool memo_eligible = db == progress_store_db();
    if (apply) {
        /* The repair may mutate flags/cursors/logs; never serve a result
         * captured before it (failed applies invalidate too — safe side). */
        g_detect_memo.valid = false;
    } else if (memo_eligible && g_detect_memo.valid &&
               sqlite3_total_changes64(db) == g_detect_memo.total_changes &&
               platform_time_wall_unix() - g_detect_memo.swept_unix <
                   RF_DETECT_MEMO_TTL_SECS) {
        if (out)
            *out = g_detect_memo.result;
        return true;
    }
    /* Sampled BEFORE the sweep: a write racing the sweep moves the counter
     * past this sample, so the next dry-run misses the memo and re-sweeps. */
    int64_t changes_at_entry =
        memo_eligible ? sqlite3_total_changes64(db) : 0;
    g_detect_memo.valid = false;

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
    local.lowest_script_validate_refill_hole = -1;
    local.lowest_proof_validate_refill_hole = -1;
    local.script_validate_cursor_before = -1;
    local.script_validate_cursor_after = -1;
    local.proof_validate_cursor_before = -1;
    local.proof_validate_cursor_after = -1;
    local.tipfin_backfill_height = -1;
    local.coins_applied_height = -1;
    local.lowest_noncanonical = -1;

    if (!stage_table_ensure(db))
        return false;
    if (!read_frontier_snapshot(db, &local))
        return false;

    /* Non-canonical residue purge runs FIRST: rows describing the wrong
     * block at their height (relabel/reorg residue) become ordinary
     * rowless holes, so every repair below sees a consistent world. */
    if (!stage_reducer_frontier_purge_noncanonical(db, ms, apply, &local))
        return false;
    if (local.noncanonical_purged > 0)
        local.repaired = true;

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

    /* Pre-refusal repairs, ordered: FIX-2a (clamp script/proof cursors back
     * to the lowest unapplied rowless hole) then FIX-1 (tip_finalize_log
     * backfill). Both exist for the coin-tear pin ONLY (see the decls in
     * stage_repair_reducer_frontier_internal.h) and run BEFORE the
     * coin-tear refusal so a healed frontier passes it arithmetically;
     * diagnostics live inside the callees. The tear gate lives HERE, not
     * just in the callees: the reconcile-light Condition polls _needed()
     * every 5s on any stalled node (tear or not), and the FIX-2a callee
     * treats a no-tear invocation as a contract violation worth an
     * unthrottled WARN — gating keeps the healthy/no-tear path silent. */
    if (local.refused_coin_tear) {
        bool handled_pre_refusal = false;
        if (!stage_reducer_frontier_try_unapplied_hole_clamp(
                db, apply, &local, &handled_pre_refusal))
            return false;
        if (!handled_pre_refusal &&
            !stage_reducer_frontier_try_tipfin_backfill(
                db, apply, &local, &handled_pre_refusal))
            return false;
        if (handled_pre_refusal) {
            if (out)
                *out = local;
            return true;
        }
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
                     local.clamped_script_validate ||
                     local.clamped_proof_validate ||
                     local.pre_refusal_unapplied_clamp ||
                     local.tipfin_backfill_count > 0 ||
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
                 "failed_mask_cleared=%d "
                 "script_validate=%d->%d proof_validate=%d->%d "
                 "script_refill_hole=%d proof_refill_hole=%d "
                 "unapplied_clamp=%d tipfin_backfill_h=%d "
                 "tipfin_backfill_n=%d",
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
                 local.failed_mask_cleared,
                 local.script_validate_cursor_before,
                 local.script_validate_cursor_after,
                 local.proof_validate_cursor_before,
                 local.proof_validate_cursor_after,
                 local.lowest_script_validate_refill_hole,
                 local.lowest_proof_validate_refill_hole,
                 (int)local.pre_refusal_unapplied_clamp,
                 local.tipfin_backfill_height,
                 local.tipfin_backfill_count);
    }

    /* Only this terminal exit caches: every early return above is an
     * actionable/abnormal state the next tick must re-derive fresh. */
    if (!apply && memo_eligible && rf_result_clean(&local)) {
        g_detect_memo.valid = true;
        g_detect_memo.swept_unix = platform_time_wall_unix();
        g_detect_memo.total_changes = changes_at_entry;
        g_detect_memo.result = local;
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
