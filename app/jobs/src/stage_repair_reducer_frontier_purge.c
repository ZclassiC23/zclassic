/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_purge.c — the non-canonical stage-log
 * row purge of the L1 reducer-frontier reconcile. Split from the refill
 * TU (file-size ceiling); shares the family's internal header. */

#include "stage_repair_reducer_frontier_internal.h"

#include "jobs/stage_repair.h"
#include "tip_finalize_log_store.h"

#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Non-canonical row purge ──────────────────────────────────────
 * Delete hash-bearing stage-log rows whose stored hash does not match
 * the canonical active-chain block at their height, plus the hashless
 * downstream rows (proof/body_persist/tip_finalize) at those heights.
 * Rows like this exist only after a height-relabel or reorg residue:
 * a height-relabel can process network blocks under labels below their
 * true heights, persisting false ok=0 bad-cb-height verdicts that NO
 * other repair touches (their status is outside every replay filter,
 * the refills fire only on ROWLESS holes, and the cursors moved past
 * them) — pinning H* forever. Purging converts them
 * into ordinary rowless holes the existing refill + cursor machinery
 * already heals. Genuine consensus rejects are SAFE: their stored hash
 * IS the canonical block at that height, so they never match the
 * predicate. Heights above the active tip carry no canonical hash and
 * are left alone. Caller holds the progress-store tx lock. */
#define RF_NONCANON_MAX_PER_PASS 8192

/* True iff `h` carries an ok=1/ok=1 validate-vs-script hash_split: both
 * validate_headers_log and script_validate_log hold a 32-byte hash at h, both
 * ok=1, and the two disagree. Such a height is owned EXCLUSIVELY by the
 * coins-rewinding stale-script replay (maybe_repair_validate_script_hash_split,
 * rewind_headers=true) — the purge must NOT delete either side below the coins
 * frontier or it blinds that replay's JOIN detector. Returns false only on a
 * real DB read error. Caller holds the progress-store tx lock. */
static bool rf_height_is_vs_hash_split(sqlite3 *db, int h, bool *out_split)
{
    *out_split = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM validate_headers_log v "
            "JOIN script_validate_log s ON s.height = v.height "
            "WHERE v.height = ? AND v.ok = 1 AND s.ok = 1 "
            "  AND length(v.hash) = 32 AND length(s.block_hash) = 32 "
            "  AND v.hash <> s.block_hash LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair", "purge_noncanonical: vs_split prepare: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(st, 1, h);
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
        *out_split = true;
    sqlite3_finalize(st);
    return true;
}

bool stage_reducer_frontier_purge_noncanonical(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !ms || !out)
        LOG_FAIL("stage_repair", "purge_noncanonical: invalid args");

    int tip_h = active_chain_height(&ms->chain_active);
    int lo = out->hstar + 1;
    int hi = out->sweep_top < tip_h ? out->sweep_top : tip_h;
    if (hi - lo + 1 > RF_NONCANON_MAX_PER_PASS)
        hi = lo + RF_NONCANON_MAX_PER_PASS - 1;
    if (lo < 0 || hi < lo)
        return true;

    static const struct {
        const char *table;
        const char *hash_col;
    } hash_logs[] = {
        { "script_validate_log",  "block_hash" },
        { "validate_headers_log", "hash" },
        { "body_fetch_log",       "hash" },
    };
    static const char *const dep_logs[] = {
        "proof_validate_log", "body_persist_log", "tip_finalize_log",
    };

    for (int h = lo; h <= hi; h++) {
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (!bi || !bi->phashBlock)
            continue;

        /* A validate-vs-script ok=1/ok=1 hash_split BELOW the coins frontier is
         * owned by the coins-rewinding stale-script replay, which needs BOTH
         * ok=1 rows intact to detect (its JOIN) and re-derive against the
         * canonical body. Deleting either side here would blind that detector
         * and strand a rowless hole the refill REFUSES below the coins frontier
         * (refill_target_in_unapplied_domain). COUNT it so the condition stays
         * actionable (and the peer-gate bypass / sticky_escalator can arm), but
         * leave the evidence rows intact for the replay owner. */
        bool below_frontier = out->coins_applied_found &&
                              out->coins_applied_height >= 0 &&
                              h < out->coins_applied_height;
        if (below_frontier) {
            bool vs_split = false;
            if (!rf_height_is_vs_hash_split(db, h, &vs_split))
                return false; // raw-return-ok:vs_split-db-error-logged-in-callee
            if (vs_split) {
                out->noncanonical_found++;
                if (out->lowest_noncanonical < 0 ||
                    h < out->lowest_noncanonical)
                    out->lowest_noncanonical = h;
                continue; /* leave evidence rows for the stale-script replay */
            }
        }

        bool stale_here = false;
        for (size_t t = 0; t < sizeof(hash_logs) / sizeof(hash_logs[0]); t++) {
            char sql[160];
            snprintf(sql, sizeof(sql),
                     "SELECT %s FROM %s WHERE height = ? "
                     "AND length(%s) = 32",
                     hash_logs[t].hash_col, hash_logs[t].table,
                     hash_logs[t].hash_col);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
                LOG_WARN("stage_repair", "purge_noncanonical: prepare %s: %s",
                         hash_logs[t].table, sqlite3_errmsg(db));
                return false;
            }
            sqlite3_bind_int64(st, 1, h);
            bool mismatch = false;
            if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
                const void *blob = sqlite3_column_blob(st, 0);
                mismatch = blob &&
                    memcmp(blob, bi->phashBlock->data, 32) != 0;
            }
            sqlite3_finalize(st);
            if (!mismatch)
                continue;
            stale_here = true;
            out->noncanonical_found++;
            if (out->lowest_noncanonical < 0 ||
                h < out->lowest_noncanonical)
                out->lowest_noncanonical = h;
            if (!apply)
                continue;
            snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE height = ?",
                     hash_logs[t].table);
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
                LOG_WARN("stage_repair", "purge_noncanonical: del prepare %s: %s",
                         hash_logs[t].table, sqlite3_errmsg(db));
                return false;
            }
            sqlite3_bind_int64(st, 1, h);
            int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                LOG_WARN("stage_repair", "purge_noncanonical: delete %s h=%d rc=%d",
                         hash_logs[t].table, h, rc);
                return false;
            }
            out->noncanonical_purged++;
        }
        if (stale_here && apply) {
            /* Hashless downstream rows at this height describe the same
             * wrong block — transitively stale. */
            for (size_t t = 0; t < sizeof(dep_logs) / sizeof(dep_logs[0]); t++) {
                char sql[96];
                snprintf(sql, sizeof(sql),
                         "DELETE FROM %s WHERE height = ?", dep_logs[t]);
                sqlite3_stmt *st = NULL;
                if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
                    continue; /* table may not exist yet; holes self-heal */
                sqlite3_bind_int64(st, 1, h);
                if (sqlite3_step(st) == SQLITE_DONE &&  // raw-sql-ok:progress-kv-kernel-store
                    sqlite3_changes(db) > 0)
                    out->noncanonical_purged++;
                sqlite3_finalize(st);
            }
        }
    }

    if (out->noncanonical_found > 0)
        LOG_WARN("stage_repair",
                 "[stage_repair] non-canonical stage-log rows: found=%d "
                 "purged=%d lowest=%d window=[%d,%d] (relabel/reorg residue"
                 "%s)", out->noncanonical_found, out->noncanonical_purged,
                 out->lowest_noncanonical, lo, hi,
                 apply ? "" : "; dry-run");
    return true;
}

/* ── stale reorg-residue tip_finalize verdict replacement ────────────
 * Tri-state read of a tip_finalize_log row's ok column at `height`:
 * RF_TIPFIN_ABSENT (no row), RF_TIPFIN_OK (ok=1), RF_TIPFIN_FAIL (ok=0).
 * Only an ok=0 row is eligible for replacement (an absent row is the
 * tipfin-backfill domain; an ok=1 row needs no repair). */
enum rf_tipfin_state {
    RF_TIPFIN_ABSENT = 0,
    RF_TIPFIN_OK,
    RF_TIPFIN_FAIL,
};

static bool rf_tipfin_state_at(sqlite3 *db, int height,
                               enum rf_tipfin_state *out_state,
                               char status_out[64])
{
    *out_state = RF_TIPFIN_ABSENT;
    if (status_out)
        status_out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok, status FROM tip_finalize_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("stage_repair",
                 "reorg-residue tipfin state prepare failed h=%d: %s",
                 height, sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, height);
    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_state = sqlite3_column_int(st, 0) != 0 ? RF_TIPFIN_OK
                                                    : RF_TIPFIN_FAIL;
        if (status_out) {
            const unsigned char *s = sqlite3_column_text(st, 1);
            snprintf(status_out, 64, "%s", s ? (const char *)s : "");
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reorg-residue tipfin state step failed h=%d "
                 "rc=%d: %s", height, rc, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* Read the header_admit_log hash at `height` (the canonical admitted block's
 * hash). *found is true only when a row exists with a 32-byte hash blob — a
 * row missing or with a malformed hash is success with *found=false (no
 * eligible lookahead binder, so the residue row is left alone). */
static bool rf_header_admit_hash_at(sqlite3 *db, int height,
                                    struct uint256 *out, bool *found)
{
    *found = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hash FROM header_admit_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("stage_repair",
                 "reorg-residue header_admit hash prepare failed h=%d: %s",
                 height, sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, height);
    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int blen = sqlite3_column_bytes(st, 0);
        if (blob && blen == 32) {
            memcpy(out->data, blob, 32);
            *found = true;
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reorg-residue header_admit hash step failed "
                 "h=%d rc=%d: %s", height, rc, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* RF_REORG_RESIDUE_MAX_PER_PASS bounds the scan window like the noncanonical
 * purge above; a contiguous reorg-residue run heals one window per tick. */
#define RF_REORG_RESIDUE_MAX_PER_PASS 8192

bool stage_reducer_frontier_purge_stale_reorg_tipfin(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    if (!db || !out)
        LOG_FAIL("stage_repair", "purge_stale_reorg_tipfin: invalid args");

    /* Eligibility ceiling: only heights ALREADY covered by coins
     * (h <= coins_applied_height - 1) qualify — there the coins are applied
     * and replacing the verdict cannot tear a coin. Unknown coins frontier =>
     * nothing to do here (the unknown path refuses upstream). */
    if (!out->coins_applied_found || out->coins_applied_height <= 0)
        return true;

    int lo = out->hstar + 1;
    int hi = out->coins_applied_height - 1;
    if (out->sweep_top < hi)
        hi = out->sweep_top;
    if (hi - lo + 1 > RF_REORG_RESIDUE_MAX_PER_PASS)
        hi = lo + RF_REORG_RESIDUE_MAX_PER_PASS - 1;
    if (lo < 0 || hi < lo)
        return true;

    progress_store_tx_lock();
    if (!ensure_log_schema(db)) {
        progress_store_tx_unlock();
        LOG_FAIL("stage_repair",
                 "purge_stale_reorg_tipfin: schema ensure failed");
    }

    struct arith_uint256 zero_work;
    arith_uint256_set_u64(&zero_work, 0);

    bool rc_ok = true;
    for (int h = lo; h <= hi; h++) {
        char status[64];
        enum rf_tipfin_state state = RF_TIPFIN_ABSENT;
        if (!rf_tipfin_state_at(db, h, &state, status)) {
            rc_ok = false;
            break;
        }
        /* Gate 1: the row must be PRESENT with ok=0 (a stale skip verdict).
         * Absent (tipfin-backfill domain) and ok=1 (healthy) are skipped. */
        if (state != RF_TIPFIN_FAIL)
            continue;

        /* Gate 2: re-evidenced upstream — header_admit_log present at h with
         * a 32-byte hash, AND the lookahead binder hash(h+1) is available so
         * the replacement carries the row-H -> hash-H+1 finalized convention
         * (finalized_row_active_match compares row.tip_hash to
         * active_chain_at(h+1) — reorg-correct). The residue verdict pins H*
         * one below h, so the column at h+1 is precisely the rowless gap the
         * existing header_admit-keyed refill re-derives. A missing binder
         * leaves the row alone (no fabricated hash). */
        struct uint256 lookahead;
        bool admit_here = false;
        bool admit_next = false;
        struct uint256 admit_dummy;
        if (!rf_header_admit_hash_at(db, h, &admit_dummy, &admit_here) ||
            !rf_header_admit_hash_at(db, h + 1, &lookahead, &admit_next)) {
            rc_ok = false;
            break;
        }
        if (!admit_here || !admit_next)
            continue;

        out->reorg_residue_tipfin_found++;
        if (out->lowest_reorg_residue_tipfin < 0 ||
            h < out->lowest_reorg_residue_tipfin)
            out->lowest_reorg_residue_tipfin = h;
        if (!apply)
            continue;

        /* Replace IN PLACE (INSERT OR REPLACE, same PRIMARY KEY h — the row
         * persists, never deleted; served_floor cannot regress). The fresh
         * verdict is ok=1 status='finalize_backfill' (NOT 'anchor', so it is
         * not an is_anchor seed) with the lookahead binder hash. */
        if (!log_insert(db, h, "finalize_backfill", true, &zero_work, -1, 0,
                        &lookahead)) {
            rc_ok = false;
            break;
        }
        out->reorg_residue_tipfin_replaced++;
        LOG_WARN("stage_repair",
                 "[stage_repair] reorg-residue tip_finalize verdict replaced "
                 "h=%d stale_status=%s -> finalize_backfill ok=1 "
                 "(hstar=%d coins_applied=%d) — false coin-tear cleared",
                 h, status, out->hstar, out->coins_applied_height);
    }

    progress_store_tx_unlock();

    if (out->reorg_residue_tipfin_found > 0 &&
        out->reorg_residue_tipfin_replaced == 0)
        LOG_WARN("stage_repair",
                 "[stage_repair] reorg-residue tip_finalize rows: found=%d "
                 "lowest=%d window=[%d,%d]%s",
                 out->reorg_residue_tipfin_found,
                 out->lowest_reorg_residue_tipfin, lo, hi,
                 apply ? "" : "; dry-run");
    return rc_ok;
}
