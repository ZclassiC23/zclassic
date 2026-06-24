/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_helpers — shared static-inline helpers for the eight Job stages
 * in app/jobs/src.
 *
 *   stage_cursor_persisted    — read the DURABLY committed cursor of an
 *                               upstream stage from the stage_cursor table.
 *   stage_default_block_reader — read a block body from disk via the
 *                               block_index entry (HAVE_DATA guarded).
 *   stage_log_row_count       — SELECT COUNT(*) over a stage's log table
 *                               for the *_dump_state_json observability.
 *
 * Each takes a `tag` argument so LOG_WARN attribution stays with the calling
 * stage name ("body_persist", "script_validate", ...). */

#ifndef ZCL_JOBS_STAGE_HELPERS_H
#define ZCL_JOBS_STAGE_HELPERS_H

#include "core/uint256.h"
#include "json/json.h"
#include "storage/block_parse_cache.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct stage_cursor_read_result {
    bool ok;        /* false = sqlite/read error; caller must not trust cursor */
    bool found;     /* false + ok = no durable row yet; cursor is 0 */
    uint64_t cursor;
    int sqlite_rc;
};

/* Read the persisted cursor of an upstream stage. Query the stage_cursor
 * table directly rather than the in-memory accessor so the floor reflects
 * what is DURABLY committed, not the in-memory value (0 on fresh init
 * until the first stage_run_once). `tag` is the calling stage's name for
 * log attribution. Missing row is a valid fresh-stage result
 * (ok=true/found=false/cursor=0); sqlite/schema/read failures are explicit
 * (ok=false) so stage gates cannot silently reinterpret corruption as
 * cursor 0. */
static inline struct stage_cursor_read_result
stage_cursor_read_persisted(sqlite3 *db, const char *name, const char *tag)
{
    struct stage_cursor_read_result r = {
        .ok = false,
        .found = false,
        .cursor = 0,
        .sqlite_rc = SQLITE_MISUSE,
    };
    const char *log_tag = (tag && tag[0]) ? tag : "stage_cursor";

    if (!db || !name || !name[0]) {
        LOG_WARN(log_tag, "[%s] upstream cursor read invalid args db=%p name=%s",
                 log_tag, (void *)db, name ? name : "(null)");
        return r;
    }

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT cursor FROM stage_cursor WHERE name = ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        r.sqlite_rc = rc;
        LOG_WARN(log_tag, "[%s] upstream cursor prepare failed stage=%s rc=%d: %s",
                 log_tag, name, rc, sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return r;
    }

    rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        r.sqlite_rc = rc;
        LOG_WARN(log_tag, "[%s] upstream cursor bind failed stage=%s rc=%d: %s",
                 log_tag, name, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        progress_store_tx_unlock();
        return r;
    }

    rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    r.sqlite_rc = rc;
    if (rc == SQLITE_ROW) {
        r.ok = true;
        r.found = true;
        r.cursor = (uint64_t)sqlite3_column_int64(st, 0);
    } else if (rc == SQLITE_DONE) {
        r.ok = true;
    } else {
        LOG_WARN(log_tag, "[%s] upstream cursor step failed stage=%s rc=%d: %s",
                 log_tag, name, rc, sqlite3_errmsg(db));
    }

    int frc = sqlite3_finalize(st);
    if (r.ok && frc != SQLITE_OK) {
        r.ok = false;
        r.sqlite_rc = frc;
        LOG_WARN(log_tag,
                 "[%s] upstream cursor finalize failed stage=%s rc=%d: %s",
                 log_tag, name, frc, sqlite3_errmsg(db));
    }
    progress_store_tx_unlock();
    return r;
}

static inline bool stage_cursor_read_or_zero(sqlite3 *db, const char *name,
                                             const char *tag, uint64_t *out)
{
    if (out)
        *out = 0;
    struct stage_cursor_read_result r =
        stage_cursor_read_persisted(db, name, tag);
    if (!r.ok)
        return false;
    if (out)
        *out = r.found ? r.cursor : 0;
    return true;
}

/* Compatibility wrapper for non-critical observers/tests that still want
 * the historical scalar contract. New liveness gates should call
 * stage_cursor_read_or_zero() and handle ok=false explicitly. */
static inline uint64_t stage_cursor_persisted(sqlite3 *db, const char *name,
                                              const char *tag)
{
    uint64_t out = 0;
    (void)stage_cursor_read_or_zero(db, name, tag, &out);
    return out;
}

/* Shared block-body reader function-pointer type. Every stage that pulls a
 * block body off disk (body_persist, script_validate, proof_validate,
 * utxo_apply) injects a reader of this exact signature; stage_default_block_reader
 * below is the production implementation. The per-stage `*_reader_fn` typedefs
 * in the stage headers alias onto this single type so the four stages share one
 * setter shape without changing their public setter names. */
typedef bool (*stage_block_reader_fn)(struct block *out,
                                      const struct block_index *bi,
                                      const char *datadir,
                                      void *user);

/* Default block-body reader used by stages whose injectable reader is
 * NULL. Guards on out/bi/HAVE_DATA and reads via pread from the block's
 * (nFile, nDataPos) on-disk position. Matches the stage_block_reader_fn
 * signature of every consuming stage. */
static inline bool stage_default_block_reader(struct block *out,
                                              const struct block_index *bi,
                                              const char *datadir, void *user)
{
    (void)user;
    if (!out || !bi || !(bi->nStatus & BLOCK_HAVE_DATA))
        return false;

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    pos.nFile = bi->nFile;
    pos.nPos = bi->nDataPos;
    return read_block_from_disk_pread(out, &pos, datadir ? datadir : "");
}

/* Read the block body for `height`/`bi`, sharing the deserialized result across
 * the five stages via block_parse_cache when the PRODUCTION default reader is
 * in effect (injected==NULL). A test-injected reader bypasses the cache so the
 * staged-pipeline fake readers keep exact control. The cache hands back a
 * COMPLETE deep clone (block_serialize<->block_deserialize round-trip), so the
 * `out` block every stage receives is exactly equal to a fresh
 * stage_default_block_reader read and is owned by the caller (block_free as
 * before). Same true/false success contract as stage_default_block_reader. */
static inline bool stage_read_block(struct block *out,
                                    const struct block_index *bi,
                                    int height, const char *datadir,
                                    stage_block_reader_fn injected, void *user)
{
    if (injected)
        return injected(out, bi, datadir, user);
    if (!out || !bi || !bi->phashBlock)
        return false;
    return block_parse_cache_get((int32_t)height, bi->phashBlock->data,
                                 bi, datadir, out);
}

/* SELECT COUNT(*) FROM <table>, for the *_dump_state_json log_rows field.
 * `tag` is the calling stage's name for log attribution. Returns -1 on
 * prepare failure or no row. */
static inline int64_t stage_log_row_count(sqlite3 *db, const char *tag,
                                          const char *table)
{
    if (!db || !table || !table[0])
        return -1;

    progress_store_tx_lock();
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(tag, "[%s] log count prepare failed: %s",
                 tag, sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return -1;
    }
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return n;
}

/* Reducer chain-window extender (the missing chain-extender).
 *
 * The reducer's tip_finalize uses a one-block lookahead (finalize H by
 * reading active_chain_at(H+1)) and then collapses the visible chain[]
 * window back to the finalized height via active_chain_move_window_tip, then
 * publishes the authority through the reducer's explicit tip publication.
 * This helper forward-extends the visible chain[] window to the already
 * tracked best-header pointer WITHOUT moving the authoritative tip. Stages
 * that read active_chain_at() call it at the top of step_once so the window is
 * supplied to the height they are about to process.
 *
 * Do not scan the full block map from a stage tick. The live map is millions
 * of entries; a full most-work sweep inside the supervisor can monopolize the
 * liveness thread and prevent repaired cursors from resuming. Header ingress
 * already maintains pindex_best_header as the current best known header, so
 * the reducer can use that O(delta) chain pointer directly and let the normal
 * downstream stages classify missing bodies, forks, and precondition failures.
 *
 * STRICTLY a no-op unless the caller owns the active-chain window for the
 * current stage step. */
static inline void reducer_extend_window_to_candidate(struct main_state *ms,
                                                       bool authoritative)
{
    if (!authoritative || !ms)
        return;
    struct block_index *cand = ms->pindex_best_header;
    if (!cand)
        cand = active_chain_most_work_candidate(&ms->chain_active,
                                                &ms->map_block_index);
    if (cand)
        (void)active_chain_extend_window(&ms->chain_active, cand);
}

/* Emit the four generic stage-machine counters (advanced/blocked/idle/error)
 * into a *_dump_state_json object. These come straight off the stage_t and are
 * identical across every Job stage; the stage's distinctive counters stay
 * inline at each call site. No-op when s is NULL (stage not yet started). */
static inline void stage_dump_counters(struct json_value *out, const stage_t *s)
{
    if (!s)
        return;
    json_push_kv_int(out, "advanced_count", (int64_t)stage_advanced_count(s));
    json_push_kv_int(out, "blocked_count",  (int64_t)stage_blocked_count(s));
    json_push_kv_int(out, "idle_count",     (int64_t)stage_idle_count(s));
    json_push_kv_int(out, "error_count",    (int64_t)stage_error_count(s));
}

/* Emit the three universal opening fields of a *_dump_state_json object —
 * "initialised", "stage_name", "cursor" — in that exact order. Seven of the
 * eight Job stages open with this identical triple; emitting it from one helper
 * keeps the same field values and the same field order downstream consumers
 * see as each stage hand-wrote. The lone exception is validate_headers, which
 * interleaves an "authority" field BETWEEN stage_name and cursor, so it keeps
 * its inline form. Pass the stage's own STAGE_NAME and g_stage so the cursor
 * and `initialised` flag reflect THIS stage (the helper owns no globals — the
 * registry hazard is avoided by keeping ownership at the call site). */
static inline void stage_dump_header(struct json_value *out, const char *name,
                                     const stage_t *s)
{
    json_push_kv_bool(out, "initialised", s != NULL);
    json_push_kv_str (out, "stage_name", name);
    json_push_kv_int (out, "cursor", (int64_t)(s ? stage_cursor(s) : 0));
}

/* Define the shared step_once entry point for a stage whose step body is the
 * uniform shape: bail to JOB_IDLE if the stage or progress.kv handle is not up,
 * extend the active-chain window to the best candidate, then run one cursor-
 * stamped step under the progress.kv tx lock. Four of the eight stages
 * (body_fetch, body_persist, script_validate, proof_validate) share this exact
 * body. The macro re-expands the per-TU file-local `g_stage`/`g_ms`
 * inside each stage's own translation unit, so the emitted machine code is
 * identical to the hand-written form — this collapses the DUPLICATED BODY only,
 * never any stage-specific wiring or ordering. Stages that need a reorg unwind,
 * a recheck pass, a mailbox drain, or projection catch-up keep their bespoke
 * step_once and do NOT use this macro. Pair it with STAGE_DRAIN_IMPL(prefix). */
#define STAGE_STEP_ONCE_SIMPLE(prefix)                              \
    job_result_t prefix##_stage_step_once(void) {                  \
        if (!g_stage) return JOB_IDLE;                             \
        sqlite3 *db = progress_store_db();                        \
        if (!db) return JOB_IDLE;                                 \
        reducer_extend_window_to_candidate(g_ms, true);           \
        progress_store_tx_lock();                                 \
        job_result_t r = stage_run_once(g_stage, db);             \
        progress_store_tx_unlock();                               \
        return r;                                                 \
    }

#endif /* ZCL_JOBS_STAGE_HELPERS_H */
