/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_purge.c — the non-canonical stage-log
 * row purge of the L1 reducer-frontier reconcile. Split from the refill
 * TU (file-size ceiling); shares the family's internal header. */

#include "stage_repair_reducer_frontier_internal.h"

#include "jobs/stage_repair.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* ── Non-canonical row purge ──────────────────────────────────────
 * Delete hash-bearing stage-log rows whose stored hash does not match
 * the canonical active-chain block at their height, plus the hashless
 * downstream rows (proof/body_persist/tip_finalize) at those heights.
 * Rows like this exist only after a height-relabel or reorg residue:
 * the 2026-06-10 -2 relabel processed network blocks under labels two
 * below their true heights, persisting false ok=0 bad-cb-height
 * verdicts that NO other repair touches (their status is outside every
 * replay filter, the refills fire only on ROWLESS holes, and the
 * cursors moved past them) — pinning H* forever. Purging converts them
 * into ordinary rowless holes the existing refill + cursor machinery
 * already heals. Genuine consensus rejects are SAFE: their stored hash
 * IS the canonical block at that height, so they never match the
 * predicate. Heights above the active tip carry no canonical hash and
 * are left alone. Caller holds the progress-store tx lock. */
#define RF_NONCANON_MAX_PER_PASS 8192

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
