/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_tipfin — FIX-1: hash-bound tip_finalize_log
 * hole backfill (row-only repair, runs before the L1 coin-tear refusal).
 *
 * The failure class this repairs: a rowless tip_finalize_log span [H*+1 .. served_floor)
 * pins H* while every upstream log (including utxo_apply_log — coins DID
 * apply those heights) is ok=1 through the span. Re-finalizing instead was
 * REJECTED: clamping tip_finalize back trips the live-UTXO-count check
 * (tip_finalize_stage.c:479-498), manufacturing utxo_count_diverged ok=0
 * evidence rows that pin H* permanently, and active_chain_move_window_tip
 * (:508) would regress the public tip below served finality. The honest
 * repair is the bookkeeping row itself: an ok=1 row above H (the served-floor
 * anchor) plus all-five-logs ok=1 AT H proves the pipeline advanced through
 * H; only the tip_finalize row is missing.
 *
 * Guard ladder (each refusal is one named LOG_WARN; see tipfin_refuse):
 *   G2 pin identification — the row at p = H*+1 must be ABSENT (tri-state,
 *      reducer_frontier.c LOG_ROW semantics). An ok=0 row is EVIDENCE.
 *   G3 unique binder — all five other logs ok=1 at p, validate vs script
 *      hashes at p agree (C3-style, both 32B). A miss strictly below the
 *      coins frontier stays the L2 refusal domain — named loudly here.
 *   G4 below served finality — p < served_floor, STRICT.
 *   G5 hash evidence for the row (row-H -> hash-H+1 convention) — the
 *      validate/script dual binder at p+1, both ok=1, both 32B, equal.
 *      Durable logs only; never active_chain_at.
 *   G6 write — log_insert(p, "finalize_backfill", ok=1, zero work, -1, 0,
 *      hash) with the ABSENT re-check inside the BEGIN IMMEDIATE tx
 *      (plain-insert semantics; the store's OR REPLACE never clobbers).
 *   G7 one-shot span marker keyed on the SPAN start; multi-tick resume
 *      rides the W witness record, not the marker.
 *   W  witness record progress_meta["tipfin_backfill.progress"] =
 *      [last_backfilled_height i32 LE][total_count u32 LE], UPSERTed in the
 *      batch tx and DELETED in the tx of the batch that exhausts the span —
 *      the condition-engine witness channel for this row-only repair.
 *
 * INVARIANTS: inserts only (the sole deletion is the witness record above);
 * no cursor write, no coins write, no tip write; never writes at/above
 * served_floor; never writes where any row exists. A 'finalize_backfill'
 * row is inert: not is_anchor (reducer_frontier.c:171-205), and
 * finalized_row_active_match treats its lookahead hash exactly like a
 * finalized row — reorg-correct. */

#include "stage_repair_reducer_frontier_internal.h"
#include "tip_finalize_log_store.h"

#include "jobs/stage_repair.h"

#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TIPFIN_WITNESS_KEY "tipfin_backfill.progress"
#define TIPFIN_BATCH_CAP 256

/* Reason codes surfaced in result.tipfin_backfill_refused_reason
 * (stage_repair.h: "tipfin TU reason code; 0 = none"). The refusal WARN
 * names the guard in text; the result carries the code. Mirrored by
 * test_stage_repair_tipfin_backfill.c — keep the values stable. */
enum tipfin_refused_reason {
    TIPFIN_REFUSED_NONE = 0,
    TIPFIN_REFUSED_G1_COIN_UNKNOWN = 1,
    TIPFIN_REFUSED_G2_EVIDENCE_ROW = 2,
    TIPFIN_REFUSED_G2_ROW_PRESENT = 3,
    TIPFIN_REFUSED_G3_MISSING_EVIDENCE = 4,
    TIPFIN_REFUSED_G4_AT_SERVED_FLOOR = 5,
    TIPFIN_REFUSED_G5_BINDER_MISSING = 6,
    TIPFIN_REFUSED_G6_IN_TX_RECHECK = 7,
    TIPFIN_REFUSED_G7_MARKER_SEEN = 8,
    TIPFIN_REFUSED_HSTAR_RANGE = 9,
};

/* Tri-state row read, reducer_frontier.c:25-29 semantics. */
enum tipfin_row_state {
    TIPFIN_ROW_ABSENT = 0,
    TIPFIN_ROW_OK,
    TIPFIN_ROW_FAIL,
};

enum tipfin_p_verdict {
    TIPFIN_P_WRITE = 0,  /* G2-G5 all pass — write the row at p */
    TIPFIN_P_SPAN_END,   /* row exists at p, or p at/above served_floor */
    TIPFIN_P_BLOCKED,    /* evidence missing (G3/G5) — stop, keep witness */
};

struct tipfin_p_eval {
    enum tipfin_p_verdict verdict;
    enum tipfin_refused_reason reason;  /* code when not WRITE */
    const char *guard;        /* guard NAME for the refusal WARN */
    const char *binding_log;  /* log naming the refusal, or NULL */
    int binding_height;
    struct uint256 tip_hash;  /* the G5 binder hash; valid when WRITE */
};

static bool tipfin_row_state_at(sqlite3 *db, int height,
                                enum tipfin_row_state *out)
{
    *out = TIPFIN_ROW_ABSENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM tip_finalize_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("stage_repair",
                 "tipfin row state prepare failed h=%d: %s",
                 height, sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, height);
    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0) != 0 ? TIPFIN_ROW_OK
                                              : TIPFIN_ROW_FAIL;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tipfin row state step failed h=%d rc=%d: %s",
                 height, rc, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* `table` is always a fixed literal below (never caller input). */
static bool tipfin_log_ok_at(sqlite3 *db, const char *table, int height,
                             bool *found, bool *ok)
{
    *found = false;
    *ok = false;
    char sql[128];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT ok FROM %s WHERE height = ?", table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("stage_repair", "tipfin log_ok sql overflow table=%s", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("stage_repair", "tipfin log_ok prepare failed table=%s: %s",
                 table, sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, height);
    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found = true;
        *ok = sqlite3_column_int(st, 0) == 1;
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tipfin log_ok step failed table=%s h=%d "
                 "rc=%d: %s",
                 table, height, rc, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* *ok_hash is true only when the row exists with ok=1 AND a 32-byte hash. */
static bool tipfin_log_ok_hash_at(sqlite3 *db, const char *table,
                                  const char *hash_col, int height,
                                  uint8_t out_hash[32], bool *ok_hash)
{
    *ok_hash = false;
    char sql[160];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT ok, %s FROM %s WHERE height = ?",
                     hash_col, table);
    if (n < 0 || n >= (int)sizeof(sql))
        LOG_FAIL("stage_repair", "tipfin hash sql overflow table=%s", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        LOG_FAIL("stage_repair", "tipfin hash prepare failed table=%s: %s",
                 table, sqlite3_errmsg(db));
    sqlite3_bind_int(st, 1, height);
    bool rc_ok = true;
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 1);
        int blen = sqlite3_column_bytes(st, 1);
        if (sqlite3_column_int(st, 0) == 1 && blob && blen == 32) {
            memcpy(out_hash, blob, 32);
            *ok_hash = true;
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tipfin hash step failed table=%s h=%d "
                 "rc=%d: %s",
                 table, height, rc, sqlite3_errmsg(db));
        rc_ok = false;
    }
    sqlite3_finalize(st);
    return rc_ok;
}

/* The validate/script dual binder at `height`: both rows ok=1 with 32-byte
 * hashes that AGREE. On a miss, *missing_log names the failing side. */
static bool tipfin_dual_binder_at(sqlite3 *db, int height, bool *bound,
                                  struct uint256 *hash_out,
                                  const char **missing_log)
{
    *bound = false;
    *missing_log = NULL;
    uint8_t vh[32], sh[32];
    bool v_ok = false, s_ok = false;
    if (!tipfin_log_ok_hash_at(db, "validate_headers_log", "hash", height,
                               vh, &v_ok))
        return false;
    if (!v_ok) {
        *missing_log = "validate_headers_log";
        return true;
    }
    if (!tipfin_log_ok_hash_at(db, "script_validate_log", "block_hash",
                               height, sh, &s_ok))
        return false;
    if (!s_ok) {
        *missing_log = "script_validate_log";
        return true;
    }
    if (memcmp(vh, sh, 32) != 0) {
        *missing_log = "validate_headers_log/script_validate_log split";
        return true;
    }
    if (hash_out)
        memcpy(hash_out->data, vh, 32);
    *bound = true;
    return true;
}

/* The G2-G5 ladder for one height. Returns false only on a DB read error. */
static bool tipfin_eval_p(sqlite3 *db, int p, int served_floor,
                          struct tipfin_p_eval *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->verdict = TIPFIN_P_BLOCKED;
    ev->binding_height = p;

    /* G2 — pin identification: the row at p must be ABSENT. */
    enum tipfin_row_state row;
    if (!tipfin_row_state_at(db, p, &row))
        return false;
    if (row != TIPFIN_ROW_ABSENT) {
        ev->verdict = TIPFIN_P_SPAN_END;
        if (row == TIPFIN_ROW_FAIL) {
            ev->reason = TIPFIN_REFUSED_G2_EVIDENCE_ROW;
            ev->guard = "G2_evidence_row";
        } else {
            ev->reason = TIPFIN_REFUSED_G2_ROW_PRESENT;
            ev->guard = "G2_row_present";
        }
        ev->binding_log = "tip_finalize_log";
        return true;
    }

    /* G3 — unique binder at p: five logs ok=1, validate/script hashes at p
     * agree. A miss below the coins frontier is the L2 refusal domain; the
     * refusal names the binding log + height (delta rows feed conservation
     * utxo_apply_sums_through and rewindability — never fabricated here). */
    bool bound = false;
    const char *missing = NULL;
    struct uint256 at_p;
    if (!tipfin_dual_binder_at(db, p, &bound, &at_p, &missing))
        return false;
    if (!bound) {
        ev->reason = TIPFIN_REFUSED_G3_MISSING_EVIDENCE;
        ev->guard = "G3_missing_evidence";
        ev->binding_log = missing;
        return true;
    }
    static const char *const k_simple_logs[] = {
        "body_persist_log",
        "proof_validate_log",
        "utxo_apply_log",
    };
    for (size_t i = 0; i < sizeof(k_simple_logs) / sizeof(k_simple_logs[0]);
         i++) {
        bool found = false, okrow = false;
        if (!tipfin_log_ok_at(db, k_simple_logs[i], p, &found, &okrow))
            return false;
        if (!found || !okrow) {
            ev->reason = TIPFIN_REFUSED_G3_MISSING_EVIDENCE;
            ev->guard = "G3_missing_evidence";
            ev->binding_log = k_simple_logs[i];
            return true;
        }
    }

    /* G4 — STRICTLY below served finality: never write at/above the tip. */
    if (p >= served_floor) {
        ev->verdict = TIPFIN_P_SPAN_END;
        ev->reason = TIPFIN_REFUSED_G4_AT_SERVED_FLOOR;
        ev->guard = "G4_at_served_floor";
        ev->binding_log = "tip_finalize_log";
        return true;
    }

    /* G5 — hash evidence for the ROW (row-H -> hash-H+1 convention). The
     * boundary p+1 == coins_applied is resolved by ORDERING (FIX-2a refills
     * the script/proof rows first), never by weakening this dual binding. */
    bound = false;
    missing = NULL;
    if (!tipfin_dual_binder_at(db, p + 1, &bound, &ev->tip_hash, &missing))
        return false;
    if (!bound) {
        ev->reason = TIPFIN_REFUSED_G5_BINDER_MISSING;
        ev->guard = "G5_binder_missing";
        ev->binding_log = missing;
        ev->binding_height = p + 1;
        return true;
    }

    ev->verdict = TIPFIN_P_WRITE;
    return true;
}

/* Storm-safe refusal diagnostics: WARN on (guard, height) TRANSITION,
 * carrying the suppressed-repeat count; the first occurrence is never
 * suppressed. The L1 reconcile path is serialized (condition-engine tick
 * under progress_store_tx_lock), so plain statics are safe. */
static void tipfin_refuse(struct stage_reducer_frontier_reconcile_result *out,
                          enum tipfin_refused_reason reason,
                          const char *guard, const char *binding_log,
                          int height)
{
    static const char *s_last_guard = NULL;
    static int s_last_height = -1;
    static unsigned long long s_reps = 0;

    out->tipfin_backfill_refused_reason = (int)reason;
    if (guard == s_last_guard && height == s_last_height) {
        s_reps++;
        return;
    }
    unsigned long long suppressed = s_reps;
    s_last_guard = guard;
    s_last_height = height;
    s_reps = 0;
    LOG_WARN("stage_repair",
             "[stage_repair] tipfin backfill refused guard=%s reason=%d "
             "binding_log=%s height=%d hstar=%d served_floor=%d "
             "coins_applied=%d (suppressed_repeats_of_prior=%llu)",
             guard, (int)reason, binding_log ? binding_log : "-", height,
             out->hstar, out->served_floor, out->coins_applied_height,
             suppressed);
}

/* G7 — one-shot span marker, the reducer_repair_marker pattern
 * (stage_repair_reducer_frontier_coin.c:161-189; those helpers are
 * file-static there, so the shape is mirrored here). */
static bool tipfin_marker_key(char key[192], int first_p,
                              const struct uint256 *binder_hash)
{
    char hex[65];
    uint256_get_hex(binder_hash, hex);
    int n = snprintf(key, 192, "reducer_frontier.tipfin_backfill_repair.%d.%s",
                     first_p, hex);
    return n > 0 && n < 192;
}

static bool tipfin_marker_seen(sqlite3 *db, const char *key, bool *seen)
{
    *seen = false;
    uint8_t blob[8] = {0};
    size_t n = 0;
    if (!progress_meta_get(db, key, blob, sizeof(blob), &n, seen))
        LOG_FAIL("stage_repair", "tipfin marker read failed key=%s", key);
    return true;
}

static bool tipfin_marker_record_in_tx(sqlite3 *db, const char *key)
{
    uint8_t one = 1;
    if (!progress_meta_set_in_tx(db, key, &one, sizeof(one)))
        LOG_FAIL("stage_repair", "tipfin marker write failed key=%s", key);
    return true;
}

/* W — witness record codec: [last_backfilled_height i32 LE][total u32 LE]. */
static void tipfin_witness_encode(uint8_t buf[8], int32_t last, uint32_t total)
{
    for (int i = 0; i < 4; i++)
        buf[i] = (uint8_t)((uint32_t)last >> (8 * i));
    for (int i = 0; i < 4; i++)
        buf[4 + i] = (uint8_t)(total >> (8 * i));
}

static bool tipfin_witness_read(sqlite3 *db, bool *found, int32_t *last,
                                uint32_t *total)
{
    *found = false;
    *last = -1;
    *total = 0;
    uint8_t buf[8] = {0};
    size_t n = 0;
    bool present = false;
    if (!progress_meta_get(db, TIPFIN_WITNESS_KEY, buf, sizeof(buf), &n,
                           &present))
        LOG_FAIL("stage_repair", "tipfin witness read failed");
    if (!present)
        return true;
    if (n != sizeof(buf)) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tipfin witness malformed len=%zu "
                 "(expected 8); treating as absent",
                 n);
        return true;
    }
    uint32_t l = 0, t = 0;
    for (int i = 0; i < 4; i++)
        l |= (uint32_t)buf[i] << (8 * i);
    for (int i = 0; i < 4; i++)
        t |= (uint32_t)buf[4 + i] << (8 * i);
    *last = (int32_t)l;
    *total = t;
    *found = true;
    return true;
}

/* G6+G8 — the batch transaction: re-evaluate G2-G5 per height INSIDE the
 * BEGIN IMMEDIATE (the absent re-check), insert hash-bound ok=1 rows up to
 * the cap, then settle the marker + witness in the same tx. Zero-progress
 * returns true with *inserted == 0 (the caller refuses, named). */
static bool tipfin_batch_tx(sqlite3 *db, int span_first, int served_floor,
                            const char *marker, uint32_t prior_total,
                            int *inserted, int *last_p, bool *exhausted)
{
    *inserted = 0;
    *last_p = -1;
    *exhausted = false;

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tipfin backfill BEGIN failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    struct arith_uint256 zero_work;
    arith_uint256_set_u64(&zero_work, 0);

    bool ok = true;
    int p = span_first;
    while (*inserted < TIPFIN_BATCH_CAP) {
        struct tipfin_p_eval ev;
        if (!tipfin_eval_p(db, p, served_floor, &ev)) {
            ok = false;
            break;
        }
        if (ev.verdict == TIPFIN_P_SPAN_END) {
            *exhausted = true;
            break;
        }
        if (ev.verdict == TIPFIN_P_BLOCKED)
            break;
        if (!log_insert(db, p, "finalize_backfill", true, &zero_work, -1, 0,
                        &ev.tip_hash)) {
            ok = false;
            break;
        }
        (*inserted)++;
        *last_p = p;
        p++;
    }

    if (ok && *inserted > 0) {
        if (marker && !tipfin_marker_record_in_tx(db, marker))
            ok = false;
        if (ok && *exhausted) {
            /* The ONLY deletion in this repair: its own witness record,
             * in the tx of the batch that exhausts the span. */
            if (!progress_meta_delete_in_tx(db, TIPFIN_WITNESS_KEY)) {
                LOG_WARN("stage_repair",
                         "[stage_repair] tipfin witness delete failed");
                ok = false;
            }
        } else if (ok) {
            uint8_t buf[8];
            tipfin_witness_encode(buf, (int32_t)*last_p,
                                  prior_total + (uint32_t)*inserted);
            if (!progress_meta_set_in_tx(db, TIPFIN_WITNESS_KEY, buf,
                                         sizeof(buf))) {
                LOG_WARN("stage_repair",
                         "[stage_repair] tipfin witness write failed");
                ok = false;
            }
        }
    }

    if (!ok || *inserted == 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        *inserted = 0;
        *last_p = -1;
        *exhausted = false;
        return ok;
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] tipfin backfill COMMIT failed: %s",
                 err ? err : "(no message)");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        *inserted = 0;
        *last_p = -1;
        *exhausted = false;
        return false;
    }
    return true;
}

bool stage_reducer_frontier_try_tipfin_backfill(
    sqlite3 *db,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out,
    bool *handled)
{
    if (!out || !handled)
        LOG_FAIL("stage_repair", "tipfin backfill: NULL out/handled");
    *handled = false;
    out->tipfin_backfill_height = -1;
    out->tipfin_backfill_count = 0;
    out->tipfin_backfill_marker_seen = false;
    out->tipfin_backfill_refused_reason = TIPFIN_REFUSED_NONE;
    if (!db)
        LOG_FAIL("stage_repair", "tipfin backfill: NULL db");

    /* G1 — coin frontier unknown is returned upstream; refuse defensively
     * if reached anyway. */
    if (out->refused_coin_unknown) {
        tipfin_refuse(out, TIPFIN_REFUSED_G1_COIN_UNKNOWN, "G1_coin_unknown",
                      NULL, out->hstar + 1);
        return true;
    }

    /* Scope: this repair exists for the coin-tear pin only. Without a tear
     * the normal forward tip_finalize replay at H*+1 == coins frontier is
     * correct and there is no row to restore — not applicable, no WARN. */
    if (!out->refused_coin_tear)
        return true;

    if (out->hstar < 0 || out->hstar >= INT32_MAX - 2) {
        tipfin_refuse(out, TIPFIN_REFUSED_HSTAR_RANGE, "hstar_out_of_range",
                      NULL, out->hstar);
        return true;
    }

    progress_store_tx_lock();

    if (!ensure_log_schema(db) || !progress_meta_table_ensure(db)) {
        progress_store_tx_unlock();
        LOG_FAIL("stage_repair", "tipfin backfill: schema ensure failed");
    }

    int span_first = out->hstar + 1;

    struct tipfin_p_eval ev;
    if (!tipfin_eval_p(db, span_first, out->served_floor, &ev)) {
        progress_store_tx_unlock();
        LOG_FAIL("stage_repair", "tipfin backfill: guard read failed h=%d",
                 span_first);
    }
    if (ev.verdict != TIPFIN_P_WRITE) {
        tipfin_refuse(out, ev.reason, ev.guard, ev.binding_log,
                      ev.binding_height);
        progress_store_tx_unlock();
        return true;  /* zero progress — fall through to the next repair */
    }

    /* W — resume channel. A multi-tick span resumes on the witness record;
     * the G7 marker only gates the SPAN start. */
    bool resuming = false;
    int32_t witness_last = -1;
    uint32_t prior_total = 0;
    if (!tipfin_witness_read(db, &resuming, &witness_last, &prior_total)) {
        progress_store_tx_unlock();
        return false;
    }
    if (resuming && witness_last + 1 != span_first)
        LOG_WARN("stage_repair",
                 "[stage_repair] tipfin witness resume skew last=%d "
                 "span_first=%d (H* recompute moved the pin; continuing)",
                 witness_last, span_first);

    char marker[192];
    if (!tipfin_marker_key(marker, span_first, &ev.tip_hash)) {
        progress_store_tx_unlock();
        LOG_FAIL("stage_repair", "tipfin marker key overflow h=%d",
                 span_first);
    }
    if (!resuming) {
        bool seen = false;
        if (!tipfin_marker_seen(db, marker, &seen)) {
            progress_store_tx_unlock();
            return false;
        }
        if (seen) {
            out->tipfin_backfill_marker_seen = true;
            tipfin_refuse(out, TIPFIN_REFUSED_G7_MARKER_SEEN,
                          "G7_marker_seen", "tip_finalize_log", span_first);
            progress_store_tx_unlock();
            return true;
        }
    }

    if (!apply) {
        progress_store_tx_unlock();
        out->repaired = true;
        out->tipfin_backfill_height = span_first;
        *handled = true;
        return true;
    }

    int inserted = 0;
    int last_p = -1;
    bool exhausted = false;
    if (!tipfin_batch_tx(db, span_first, out->served_floor,
                         resuming ? NULL : marker, prior_total, &inserted,
                         &last_p, &exhausted)) {
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();

    if (inserted == 0) {
        /* The pre-check passed but the in-tx re-check refused (G6). */
        tipfin_refuse(out, TIPFIN_REFUSED_G6_IN_TX_RECHECK,
                      "G6_in_tx_recheck", "tip_finalize_log", span_first);
        return true;
    }

    out->repaired = true;
    out->refused_coin_tear = false;  /* result-reporting only (:449-450) */
    out->tipfin_backfill_height = last_p;
    out->tipfin_backfill_count = inserted;
    *handled = true;
    LOG_WARN("stage_repair",
             "[stage_repair] tipfin backfill repaired rowless span "
             "first=%d last=%d count=%d total=%u exhausted=%d hstar=%d "
             "served_floor=%d coins_applied=%d",
             span_first, last_p, inserted,
             prior_total + (uint32_t)inserted, exhausted ? 1 : 0, out->hstar,
             out->served_floor, out->coins_applied_height);
    return true;
}
