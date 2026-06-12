/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_frontier — L0 authority. Compute H* and served_floor from
 * durable progress.kv state. See reducer_frontier.h for the contract.
 *
 * PURE SELECT-only: every statement here is a SELECT. There is no INSERT,
 * UPDATE, DELETE, REPLACE, BEGIN, or COMMIT in this translation unit. The
 * raw sqlite3_step sites read the progress.kv kernel store (the same hatch
 * the sibling *_log_store.c readers use) and carry the canonical
 * `// raw-sql-ok:progress-kv-kernel-store` marker. The caller holds
 * progress_store_tx_lock() so the snapshot is internally consistent. */

#include "jobs/reducer_frontier.h"

#include "jobs/tip_finalize_stage.h"

#include "chain/checkpoints.h"
#include "platform/time_compat.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/log_throttle.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

/* Per-row reads return a tri-state so contiguity and discrimination can
 * tell "no row" apart from "ok=0 row". */
enum log_row_state {
    LOG_ROW_ABSENT = 0,   /* no row at this height */
    LOG_ROW_OK,           /* row present, ok=1 */
    LOG_ROW_FAIL,         /* row present, ok=0 */
};

/* The success-checked logs whose contiguous ok=1 prefix bounds H* (C2).
 * Each entry pairs the log table with its stage cursor name (used only for
 * the C1 below-anchor diagnostic). validate_headers and script_validate
 * additionally feed the C3 hash-agreement check below.
 *
 * `served_tip` distinguishes the cursor convention. The upstream reducer
 * stages count the NEXT height to process — cursor U means "rows expected
 * up to U-1; U is the next height". tip_finalize uses the served-tip
 * convention (task #30/#31): cursor C means "served tip at C; the C→C+1
 * transition is pending", so its rows are expected up to C (the anchor row
 * at C plus finalized rows below). frontier_next_cursor() below normalizes
 * tip_finalize's cursor to the upstream "next height" frame (C+1) at every
 * scan/candidate read site, so the contiguity and anchor-candidate algorithms
 * stay byte-identical across the convention change. */
struct frontier_log {
    const char *log_table;
    const char *cursor_name;
    bool        served_tip;
};

static const struct frontier_log k_logs[] = {
    { "validate_headers_log", "validate_headers", false },
    { "script_validate_log",  "script_validate",  false },
    { "body_persist_log",     "body_persist",     false },
    { "proof_validate_log",   "proof_validate",   false },
    { "utxo_apply_log",       "utxo_apply",       false },
    { "tip_finalize_log",     "tip_finalize",     true  },
};
static const int k_logs_n = (int)(sizeof(k_logs) / sizeof(k_logs[0]));

/* Normalize a stage cursor to the "next height to process" frame the
 * contiguity / anchor-candidate scans expect. tip_finalize's served-tip
 * cursor C (served tip at C) is equivalent to next-height C+1 (its rows run
 * up to C, the anchor row included); every other stage already counts the
 * next height, so it passes through unchanged. */
static int64_t frontier_next_cursor(const struct frontier_log *fl,
                                    int64_t cursor)
{
    return (fl && fl->served_tip && cursor > 0) ? cursor + 1 : cursor;
}

/* Read the ok column of a *_log row at `height`. *out is set to one of the
 * tri-states. Returns false ONLY on a real SQLite prepare/step error
 * (caller propagates as a DB read failure). A missing row is success with
 * *out = LOG_ROW_ABSENT — that is the contiguity terminator, not an error. */
static bool log_ok_at(sqlite3 *db, const char *log_table, int32_t height,
                      enum log_row_state *out)
{
    *out = LOG_ROW_ABSENT;
    /* log_table is a fixed string from k_logs[] (never caller input), so the
     * concat below cannot inject. Bind the height parameter. */
    char sql[128];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT ok FROM %s WHERE height = ?", log_table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("reducer", "log_ok_at sql overflow for table=%s", log_table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "prepare %s failed: %s",
                 log_table, sqlite3_errmsg(db));
    sqlite3_bind_int64(st, 1, (sqlite3_int64)height);

    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0) != 0 ? LOG_ROW_OK : LOG_ROW_FAIL;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "step %s at h=%d failed: %s",
                 log_table, height, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* Walk one success-checked log upward from TRUSTED_ANCHOR+1, returning the
 * deepest height whose [anchor+1 .. h] run is all ok=1 (the contiguous
 * prefix). A missing row OR an ok=0 row terminates the run.
 *
 * `cursor` bounds the scan: rows are only expected up to `cursor-1` (the
 * cursor names the NEXT height to process). If cursor <= TRUSTED_ANCHOR the
 * log holds nothing above the anchor and is trivially consistent there, so
 * h_contiguous stays at the anchor (does not lower H*).
 *
 * On a DB read error returns false; *h_contiguous is then meaningless. */
static bool log_contiguous_prefix(sqlite3 *db, const char *log_table,
                                  int32_t anchor, int64_t cursor,
                                  int32_t *h_contiguous)
{
    *h_contiguous = anchor;
    if (cursor <= (int64_t)anchor)
        return true;  /* nothing above the anchor to disprove the prefix */

    /* ONE ranged scan instead of a prepare+bind+step PER HEIGHT (measured
     * 53x cheaper; the walk runs full length precisely on HEALTHY nodes,
     * where it used to cost ~2.3 us/height under the global progress
     * lock). `height` is the table's PRIMARY KEY, so rows stream back in
     * strictly increasing height order: the first height JUMP is a hole
     * (the old per-height probe's LOG_ROW_ABSENT) and the first ok=0 row
     * is a recorded failure — both terminate the contiguous prefix,
     * byte-for-byte the old semantics. */
    char sql[160];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT height, ok FROM %s "
                     "WHERE height > ? AND height < ? ORDER BY height",
                     log_table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("reducer", "log_contiguous_prefix sql overflow for %s",
                 log_table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "prepare %s ranged scan failed: %s",
                 log_table, sqlite3_errmsg(db));
    sqlite3_bind_int64(st, 1, (sqlite3_int64)anchor);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)cursor);

    bool rc_ok = true;
    int64_t expect = (int64_t)anchor + 1;
    while (true) {
        int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
        if (rc == SQLITE_ROW) {
            int64_t h = sqlite3_column_int64(st, 0);
            if (h != expect || sqlite3_column_int(st, 1) == 0)
                break;  /* hole (height jump) or ok=0 — contiguity ends */
            *h_contiguous = (int32_t)h;
            expect++;
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            LOG_WARN("reducer", "ranged scan %s failed: %s",
                     log_table, sqlite3_errmsg(db));
            rc_ok = false;
            break;
        }
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* Read the persisted cursor of a stage off stage_cursor. *out defaults to 0
 * (absent row == fresh init). Returns false only on a real SQLite error. */
static bool cursor_at(sqlite3 *db, const char *name, int64_t *out)
{
    *out = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name = ?",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "prepare stage_cursor failed: %s",
                 sqlite3_errmsg(db));
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "step stage_cursor %s failed: %s",
                 name, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* A tip_finalize status="anchor" row written by tip_finalize_stage_seed_anchor()
 * is the durable marker for a logless trusted reducer base. Generic active-tip
 * reanchors use the same row shape, so a candidate is trusted only when every
 * reducer stage cursor has reached at least H+1 and, if it has advanced beyond
 * H+1, the first row above the anchor is ok=1. That accepts fresh seed anchors
 * with no rows yet while rejecting stale served-tip anchors above the upstream
 * reducer frontier. */
static bool reducer_anchor_candidate_ok(sqlite3 *db, int32_t height)
{
    int64_t first = (int64_t)height + 1;
    if (first > INT32_MAX)
        return false;

    for (int i = 0; i < k_logs_n; i++) {
        int64_t raw_cursor = 0;
        if (!cursor_at(db, k_logs[i].cursor_name, &raw_cursor))
            return false;
        /* Normalize tip_finalize's served-tip cursor to the next-height frame
         * (task #31): a seed anchor at H now stamps tip_finalize cursor == H,
         * whose next-height equivalent is H+1 == first — the same value the
         * old +1-convention cursor held, so this candidate gate is unchanged. */
        int64_t cursor = frontier_next_cursor(&k_logs[i], raw_cursor);
        if (cursor < first)
            return false;
        if (cursor == first)
            continue;

        enum log_row_state row;
        if (!log_ok_at(db, k_logs[i].log_table, (int32_t)first, &row))
            return false;
        if (row != LOG_ROW_OK)
            return false;
    }
    return true;
}

static bool reducer_trusted_anchor(sqlite3 *db, int32_t compiled_anchor,
                                   int32_t *out)
{
    *out = compiled_anchor;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height FROM tip_finalize_log "
            "WHERE ok = 1 AND status = 'anchor' AND height >= ? "
            "ORDER BY height DESC",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "prepare trusted anchor failed: %s",
                 sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, compiled_anchor);

    bool rc_ok = true;
    while (true) {
        int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
        if (rc == SQLITE_ROW) {
            int32_t h = (int32_t)sqlite3_column_int64(st, 0);
            if (reducer_anchor_candidate_ok(db, h)) {
                *out = h;
                break;
            }
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            LOG_WARN("reducer", "step trusted anchor failed: %s",
                     sqlite3_errmsg(db));
            rc_ok = false;
            break;
        }
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* served_floor = MAX(height FROM tip_finalize_log WHERE ok=1), or 0.
 * Scans the WHOLE log (including stale-debris rows below H*): served_floor
 * is reported independently of H* precisely so a torn view (a tip finalized
 * above the provable prefix) is visible to L1 as "hold, don't drop". */
static bool tip_finalize_served_floor(sqlite3 *db, int32_t *served_floor)
{
    *served_floor = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(MAX(height), 0) FROM tip_finalize_log "
            "WHERE ok = 1",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "prepare served_floor failed: %s",
                 sqlite3_errmsg(db));
    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *served_floor = (int32_t)sqlite3_column_int64(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "step served_floor failed: %s",
                 sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* Fetch a 32-byte hash column from a *_log row. *found is set true only when
 * a row exists AND the column is a non-NULL 32-byte blob. Returns false on a
 * real SQLite error. A NULL hash (cold-import prefix) is success with
 * *found=false — it does NOT lower H* (C3). */
static bool log_hash_at(sqlite3 *db, const char *log_table,
                        const char *hash_col, int32_t height,
                        uint8_t out[32], bool *found)
{
    *found = false;
    char sql[160];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT %s FROM %s WHERE height = ?",
                     hash_col, log_table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("reducer", "log_hash_at sql overflow for %s.%s",
                 log_table, hash_col);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("reducer", "prepare %s.%s failed: %s",
                 log_table, hash_col, sqlite3_errmsg(db));
    sqlite3_bind_int64(st, 1, (sqlite3_int64)height);

    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int blen = sqlite3_column_bytes(st, 0);
        if (blob && blen == 32) {
            memcpy(out, blob, 32);
            *found = true;
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "step %s.%s at h=%d failed: %s",
                 log_table, hash_col, height, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* C3 — cap H* at the height BELOW the first hash split between
 * validate_headers_log.hash and script_validate_log.block_hash. A split
 * counts only when BOTH rows are present with non-NULL 32-byte hashes that
 * differ. Walks anchor+1 .. *hstar; lowers *hstar on the first split.
 * Returns false on a DB read error. */
static bool apply_hash_agreement(sqlite3 *db, int32_t anchor, int32_t *hstar)
{
    for (int32_t h = anchor + 1; h <= *hstar; h++) {
        uint8_t vh[32], sv[32];
        bool vh_found = false, sv_found = false;
        if (!log_hash_at(db, "validate_headers_log", "hash", h, vh, &vh_found))
            return false;
        if (!log_hash_at(db, "script_validate_log", "block_hash", h, sv,
                         &sv_found))
            return false;
        if (vh_found && sv_found && memcmp(vh, sv, 32) != 0) {
            /* Both logs recorded a hash for h and they disagree: a genuine
             * defect. H* caps at h-1 (never below the anchor). */
            *hstar = (h - 1 < anchor) ? anchor : (h - 1);
            return true;
        }
    }
    return true;
}

bool reducer_frontier_compute_hstar(sqlite3 *progress_db,
                                    int32_t *hstar,
                                    int32_t *served_floor)
{
    if (!progress_db)
        LOG_FAIL("reducer", "compute_hstar: NULL progress_db");
    if (!hstar || !served_floor)
        LOG_FAIL("reducer", "compute_hstar: NULL out param(s)");

    /* TRUSTED_ANCHOR: the SHA3-verified UTXO checkpoint height. Fall back to
     * the compiled constant if the checkpoint table is somehow empty, so H*
     * still has a hard floor. */
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    int32_t compiled_anchor = cp ? cp->height : REDUCER_FRONTIER_TRUSTED_ANCHOR;
    int32_t anchor = compiled_anchor;
    if (!reducer_trusted_anchor(progress_db, compiled_anchor, &anchor))
        LOG_FAIL("reducer", "trusted anchor read failed");

    *served_floor = 0;
    /* served_floor is independent of H* (C-served): report the deepest
     * finalized ok=1 even when it sits above the provable prefix. */
    if (!tip_finalize_served_floor(progress_db, served_floor))
        LOG_FAIL("reducer", "served_floor read failed");

    /* H* = MIN over every success-checked log of its contiguous ok=1 prefix
     * (C2). MIN-fold from a high sentinel: each log's h_contiguous is >=
     * anchor (its contiguity floor), so the weakest log bounds H*. A log with
     * no rows above the anchor returns exactly `anchor`, which correctly pins
     * H* to the anchor for that log. The hard guard at the end then clamps
     * the result up to the anchor (and covers the all-logs-empty case where
     * the sentinel survives). */
    int32_t hs = INT32_MAX;
    int32_t ua_contig = anchor;  /* utxo_apply's OWN contiguous frontier (C4) */
    for (int i = 0; i < k_logs_n; i++) {
        int64_t raw_cursor = 0;
        if (!cursor_at(progress_db, k_logs[i].cursor_name, &raw_cursor))
            LOG_FAIL("reducer", "cursor read failed: %s",
                     k_logs[i].cursor_name);
        /* Normalize tip_finalize's served-tip cursor (C == served tip) to the
         * next-height frame (C+1) the contiguity scan's exclusive upper bound
         * expects — its rows run up to C (the anchor row included), the same
         * span the old +1-convention cursor named (task #31). */
        int64_t scan_cursor = frontier_next_cursor(&k_logs[i], raw_cursor);

        /* C1 diagnostic: a cursor behind the anchor is a defect state the
         * heal will fix; flag it but do not abort H* computation. The
         * normalized cursor is compared so tip_finalize is not flagged for
         * sitting exactly one below the upstream frame by convention. */
        if (scan_cursor < (int64_t)anchor + 1)
            LOG_WARN("reducer", "cursor below hstar: %s cursor=%lld anchor=%d",
                     k_logs[i].cursor_name, (long long)raw_cursor, anchor);

        int32_t h_contig = anchor;
        if (!log_contiguous_prefix(progress_db, k_logs[i].log_table, anchor,
                                   scan_cursor, &h_contig))
            LOG_FAIL("reducer", "contiguity walk failed: %s",
                     k_logs[i].log_table);

        if (h_contig < hs)
            hs = h_contig;  /* H* = MIN over all logs */
        if (strcmp(k_logs[i].log_table, "utxo_apply_log") == 0)
            ua_contig = h_contig;  /* captured free for the C4 tear check */
    }
    if (hs == INT32_MAX)
        hs = anchor;  /* no logs scanned (k_logs_n>0, but defensive) */

    /* C3 — hash agreement: cap H* below the first validate_headers vs
     * script_validate split within [anchor+1 .. hs]. */
    if (!apply_hash_agreement(progress_db, anchor, &hs))
        LOG_FAIL("reducer", "hash-agreement walk failed");

    /* C4 diagnostic — coins frontier vs utxo_apply's OWN applied frontier.
     * coins_applied_height tracks the utxo_apply cursor by construction (the
     * stage co-commits both in one BEGIN IMMEDIATE), so a REAL tear is coins
     * applied ABOVE utxo_apply's contiguous ok=1 log prefix (ua_contig) — a
     * hole/ok=0 row below the cursor. It is NOT coins leading the global MIN
     * H*: H* is pinned by the SLOWEST log (tip_finalize), which legitimately
     * lags utxo_apply by the pipeline depth (every applied coin already passed
     * headers->script->proof->utxo_apply), and reading that lag as a tear is
     * the false positive that dead-ended at the never-built L2. Compare against
     * ua_contig, never hs. Do NOT lower H* here; absent key == unknown frontier
     * (not a defect for L0). De-stormed via the shared log_throttle: a
     * persistent tear fires once per pair change / 300 s keep-alive with the
     * suppressed count. Caller holds progress_store_tx_lock(); the throttle's
     * atomics keep the counters torn-read-safe regardless. */
    int32_t coins_applied = 0;
    bool coins_found = false;
    if (coins_kv_get_applied_height(progress_db, &coins_applied, &coins_found)
        && coins_found && coins_applied > ua_contig + 1) {
        static struct log_throttle tear_throttle = LOG_THROTTLE_INIT;
        uint64_t pair = ((uint64_t)(uint32_t)coins_applied << 32)
                        | (uint32_t)ua_contig;
        int64_t now = platform_time_wall_unix();
        uint64_t reps = 0;
        if (log_throttle_should_emit(&tear_throttle, pair, now, 300, &reps))
            LOG_WARN("reducer",
                     "coins_applied=%d > utxo_apply_frontier=%d "
                     "(coin tear vs own applied log) repeated=%llu",
                     coins_applied, ua_contig + 1, (unsigned long long)reps);
    }

    /* HARD GUARD — never rewind across finality. */
    if (hs < anchor)
        hs = anchor;

    *hstar = hs;
    return true;
}

bool reducer_frontier_log_frontier(sqlite3 *progress_db,
                                   const char *log_table,
                                   const char *cursor_name,
                                   int32_t *out_h)
{
    if (!progress_db || !log_table || !cursor_name || !out_h)
        LOG_FAIL("reducer", "log_frontier: NULL arg");

    /* Recursive lock: safe whether or not the caller already holds it. */
    progress_store_tx_lock();

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    int32_t compiled_anchor = cp ? cp->height : REDUCER_FRONTIER_TRUSTED_ANCHOR;
    int32_t anchor = compiled_anchor;
    int32_t h = anchor;
    int64_t cursor = 0;

    bool ok = reducer_trusted_anchor(progress_db, compiled_anchor, &anchor);
    if (ok)
        ok = cursor_at(progress_db, cursor_name, &cursor);
    if (ok)
        ok = log_contiguous_prefix(progress_db, log_table, anchor, cursor, &h);

    progress_store_tx_unlock();

    if (!ok)
        return false;  /* the failing inner read already logged the cause */
    *out_h = h;
    return true;
}

bool reducer_frontier_log_frontier_above(sqlite3 *progress_db,
                                         const char *log_table,
                                         const char *cursor_name,
                                         int32_t verified_floor,
                                         int32_t *out_h)
{
    if (!progress_db || !log_table || !cursor_name || !out_h)
        LOG_FAIL("reducer", "log_frontier_above: NULL arg");
    if (verified_floor < 0)
        LOG_FAIL("reducer", "log_frontier_above: negative floor %d",
                 verified_floor);

    /* Recursive lock: safe whether or not the caller already holds it. */
    progress_store_tx_lock();

    int64_t cursor = 0;
    int32_t h = verified_floor;
    bool ok = cursor_at(progress_db, cursor_name, &cursor);
    if (ok)
        ok = log_contiguous_prefix(progress_db, log_table, verified_floor,
                                   cursor, &h);

    progress_store_tx_unlock();

    if (!ok)
        return false;  /* the failing inner read already logged the cause */
    *out_h = h;
    return true;
}

bool reducer_frontier_log_hash_at(sqlite3 *progress_db,
                                  const char *log_table,
                                  const char *hash_col,
                                  int32_t height,
                                  uint8_t out[32], bool *found)
{
    if (!progress_db || !log_table || !hash_col || !out || !found)
        LOG_FAIL("reducer", "log_hash_at: NULL arg");

    /* Recursive lock: safe whether or not the caller already holds it. */
    progress_store_tx_lock();
    bool ok = log_hash_at(progress_db, log_table, hash_col, height,
                          out, found);
    progress_store_tx_unlock();
    return ok;
}

bool reducer_frontier_derive_coins_best(sqlite3 *progress_db,
                                        int32_t *out_height,
                                        uint8_t out_hash[32],
                                        bool *hash_found,
                                        bool *found)
{
    if (!progress_db || !out_height || !out_hash || !hash_found || !found)
        LOG_FAIL("reducer", "derive_coins_best: NULL arg");
    *found = false;
    *hash_found = false;
    *out_height = -1;
    memset(out_hash, 0, 32);

    /* Recursive lock: one consistent durable snapshot across all reads. */
    progress_store_tx_lock();

    /* THE proven-authority predicate (coins_kv.h): applied frontier present
     * AND migration stamp set AND non-empty. A cursor-backfilled frontier on
     * a pre-migration datadir is NOT canonical — *found stays false and the
     * caller keeps its (stricter) legacy gates. Authority-proof read errors
     * also degrade to !found, never to the permissive derived path. */
    int32_t applied = -1;
    bool ok = true;
    if (coins_kv_is_proven_authority(progress_db, &applied)) {
        int32_t h = applied - 1;
        *out_height = h;
        *found = true;
        /* Hash witness 1: tip_finalize_log, read CONVENTION-AWARE via
         * tip_finalize_stage_block_hash_at — the finalized ok=1 row at h-1
         * carries hash(h) (step_finalize binds the LOOKAHEAD successor into
         * the row), and an anchor seed row at h carries the block's own
         * hash(h). Reading the raw row AT h would return hash(h+1) for
         * finalized rows: the inconsistent (height, hash) authority-pair
         * shape the 2026-06-11 splice forensic banned (tip_finalize_stage.c
         * step_finalize publish comment). */
        uint8_t tf[32];
        bool tf_found = tip_finalize_stage_block_hash_at(progress_db, h, tf);
        /* Hash witness 2: validate_headers_log.hash at h (own-hash by
         * construction) — the Invariant A trust root; covers the <=1-block
         * pipeline window where utxo_apply leads tip_finalize (Invariant B
         * bound) and anchor-seeded datadirs whose tip_finalize window is
         * empty at the frontier. */
        uint8_t vh[32];
        bool vh_found = false;
        if (!log_hash_at(progress_db, "validate_headers_log", "hash",
                         h, vh, &vh_found)) {
            ok = false;  /* real DB read error; height outputs stand */
        } else if (tf_found && vh_found && memcmp(tf, vh, 32) != 0) {
            /* Hash-identity guard: two durable logs disagreeing about the
             * SAME height is the don't-guess shape — withhold the hash
             * LOUDLY rather than install either candidate. The height stays
             * authoritative; the caller resolves height->hash via its own
             * index, never the reverse. */
            static struct log_throttle mismatch_throttle = LOG_THROTTLE_INIT;
            uint64_t reps = 0;
            if (log_throttle_should_emit(&mismatch_throttle,
                                         (uint64_t)(uint32_t)h,
                                         platform_time_wall_unix(), 300,
                                         &reps))
                LOG_WARN("reducer",
                         "derive_coins_best: cross-log hash mismatch at h=%d "
                         "(tip_finalize witness %02x%02x%02x%02x.. vs "
                         "validate_headers %02x%02x%02x%02x..) — hash "
                         "withheld, height stands repeated=%llu",
                         h, tf[0], tf[1], tf[2], tf[3],
                         vh[0], vh[1], vh[2], vh[3],
                         (unsigned long long)reps);
        } else if (tf_found) {
            memcpy(out_hash, tf, 32);
            *hash_found = true;
        } else if (vh_found) {
            memcpy(out_hash, vh, 32);
            *hash_found = true;
        }
        /* Neither witness resolving is SUCCESS with *hash_found=false:
         * the height stays authoritative; the caller resolves
         * height->hash via its own index, never the reverse. */
    }

    progress_store_tx_unlock();
    return ok;
}

bool reducer_frontier_derive_coins_best_now(int32_t *out_height,
                                            uint8_t out_hash[32],
                                            bool *out_hash_found)
{
    if (!out_height)
        LOG_FAIL("reducer", "derive_coins_best_now: NULL out_height");
    *out_height = -1;
    if (out_hash) memset(out_hash, 0, 32);
    if (out_hash_found) *out_hash_found = false;

    sqlite3 *pdb = progress_store_db();
    if (!pdb)
        return false;  /* store closed => legacy fallback; not an error */
    uint8_t hash[32];
    bool hf = false, found = false;
    if (!reducer_frontier_derive_coins_best(pdb, out_height, hash,
                                            &hf, &found))
        return false;  /* hard read error already logged */
    if (!found)
        return false;
    if (out_hash && hf) memcpy(out_hash, hash, 32);
    if (out_hash_found) *out_hash_found = hf;
    return true;
}

bool reducer_frontier_log_coverage_floor(sqlite3 *progress_db,
                                         const char *log_table,
                                         int32_t *out_lo, bool *found)
{
    if (!progress_db || !log_table || !out_lo || !found)
        LOG_FAIL("reducer", "log_coverage_floor: NULL arg");
    *found = false;
    *out_lo = 0;

    /* log_table is a fixed caller constant (never network/user input), same
     * concat discipline as log_ok_at. */
    char sql[128];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT MIN(height) FROM %s", log_table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("reducer", "log_coverage_floor sql overflow for %s",
                 log_table);

    /* Recursive lock: safe whether or not the caller already holds it. */
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    bool rc_ok = true;
    if (sqlite3_prepare_v2(progress_db, sql, -1, &st, NULL) != SQLITE_OK) {
        progress_store_tx_unlock();
        LOG_FAIL("reducer", "prepare coverage floor %s failed: %s",
                 log_table, sqlite3_errmsg(progress_db));
    }
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_NULL) {
            *out_lo = (int32_t)sqlite3_column_int64(st, 0);
            *found = true;
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("reducer", "step coverage floor %s failed: %s",
                 log_table, sqlite3_errmsg(progress_db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return rc_ok;
}
