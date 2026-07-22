/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_fetch_stage — implementation. See jobs/body_fetch_stage.h.
 *
 * Single-process singleton, single-step (no worker pool). The work per
 * step is one in-memory flag check + one SQL insert, so batching adds
 * complexity without throughput. The F-2 stage primitive does the
 * cursor + replay heavy lifting; this module is the step body and the
 * schema-bootstrap glue for the `body_fetch_log` table that lives in
 * progress.kv alongside the cursor table. */

#include "platform/time_compat.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/stage_helpers.h"
#include "jobs/stage_body_index.h"
#include "body_fetch_log_store.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "json/json.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE_NAME "body_fetch"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static _Atomic uint64_t g_observed_total = 0;
static _Atomic uint64_t g_skipped_total  = 0;
static _Atomic int64_t  g_last_advance_height = -1;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;

/* ── Schema + log I/O ─────────────────────────────────────────────────
 * The body_fetch_log schema, its insert, and the upstream
 * validate_headers_log ok-flag reader live in body_fetch_log_store.c
 * (pure sqlite kernel helpers below the AR layer). The upstream cursor is
 * read via stage_cursor_read_or_zero() (jobs/stage_helpers.h) so
 * body_fetch's floor check reflects what is DURABLY committed, not the
 * in-memory value which is 0 on a fresh init until the first
 * stage_run_once. */

/* ── Step body ─────────────────────────────────────────────────────── */

static job_result_t step_body_fetch(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    /* Floor: never overrun validate_headers' DURABLY persisted cursor.
     * validate cursor = "next height to validate" → heights
     * [0, vh_cursor-1] are validated. We can fetch up to vh_cursor-1.
     * Reading from disk (vs the in-memory accessor) means body_fetch
     * never advances past what is actually committed upstream, and
     * keeps body_fetch testable in isolation. */
    uint64_t vh_cursor = 0;
    if (!stage_cursor_read_or_zero(db, "validate_headers", STAGE_NAME,
                                   &vh_cursor))
        return JOB_FATAL;
    if ((uint64_t)next_h >= vh_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;  /* not BLOCKED — validate will catch up */
    }

    /* Read the validate_headers_log row to learn pass/fail. Floor
     * guarantees the row exists; defend against torn writes anyway. */
    int vh_ok = -1;
    char vh_reason[96];
    int found = body_fetch_vh_log_ok_at(db, next_h, &vh_ok,
                                        vh_reason, sizeof(vh_reason));
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        /* Row missing despite floor — a durable upstream-log hole, not
         * "not yet" (see stage_upstream_log_hole_note). JOB_IDLE, never
         * JOB_BLOCKED: reducer_frontier_reconcile_light is the healer. */
        stage_upstream_log_hole_note(STAGE_NAME, "validate_headers_log",
                                     next_h, vh_cursor, &g_last_blocked_unix);
        return JOB_IDLE;
    }
    stage_upstream_log_hole_clear(STAGE_NAME);

    /* Look up the in-memory block_index entry — we need the hash and
     * the BLOCK_HAVE_DATA flag. */
    struct block_index *bi = stage_body_index_at(ms, next_h);
    if (!bi || !bi->phashBlock) {
        /* Concurrent reorg through this height between validate and
         * fetch. Surface as IDLE so the supervisor retries; the chain
         * will stabilise. */
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (vh_ok == 0) {
        if (strcmp(vh_reason,
                   "no-header-solution-backfill-required") == 0) {
            blocker_init(&c->blocker,
                         "body_fetch.header_solution_missing",
                         STAGE_NAME,
                         BLOCKER_TRANSIENT,
                         "validate_headers is waiting for a real "
                         "Equihash solution, not rejecting consensus");
            atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
            return JOB_BLOCKED;
        }
        /* Header failed PoW/Equihash earlier — record skip + advance. */
        if (!body_fetch_log_insert(db, next_h, bi->phashBlock,
                                   "skipped_invalid", 0, false,
                                   "header_validation_failed"))
            return JOB_FATAL;
        atomic_fetch_add(&g_skipped_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    /* Header passed validation; check body availability. */
    if (!(bi->nStatus & BLOCK_HAVE_DATA)) {
        /* Body not yet on disk — JOB_IDLE, don't advance. The natural
         * backpressure: cursor stays put until msg_blocks brings it in. */
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    /* Body observed on disk. Record presence; bytes=0 because size probing
     * would add per-height pread cost. */
    if (!body_fetch_log_insert(db, next_h, bi->phashBlock, "disk", 0, true, NULL))
        return JOB_FATAL;

    atomic_fetch_add(&g_observed_total, 1);
    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

/* ── Public API ────────────────────────────────────────────────────── */

bool body_fetch_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("body_fetch", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("body_fetch",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);

    /* Idempotent: same ms, already initialised → success. */
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("body_fetch",
                "init: already bound to a different main_state");
        return true;
    }

    if (!body_fetch_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_body_fetch, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("body_fetch", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("body_fetch", "[body_fetch] stage initialised");
    return true;
}

STAGE_STEP_ONCE_SIMPLE(body_fetch)

STAGE_DRAIN_IMPL(body_fetch)

void body_fetch_stage_shutdown(void)
{
    /* Registry hygiene (tests re-init in-process): re-derived from live
     * state the next time the condition fires, so clearing here loses
     * nothing. */
    stage_upstream_log_hole_clear(STAGE_NAME);
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    /* Reset per-init observability state. Persisted cursor + log rows
     * are preserved — that is the saga contract. */
    atomic_store(&g_observed_total, (uint64_t)0);
    atomic_store(&g_skipped_total, (uint64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    pthread_mutex_unlock(&g_lock);
}

uint64_t body_fetch_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

int64_t body_fetch_stage_step_us_ewma(void)
{
    return g_stage ? stage_step_us_ewma(g_stage) : 0;
}

uint64_t body_fetch_stage_observed_total(void)
{
    return atomic_load(&g_observed_total);
}

uint64_t body_fetch_stage_skipped_total(void)
{
    return atomic_load(&g_skipped_total);
}

bool body_fetch_stage_dump_state_json(struct json_value *out,
                                       const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    stage_dump_header(out, STAGE_NAME, g_stage);
    json_push_kv_int (out, "observed_total",
                      (int64_t)atomic_load(&g_observed_total));
    json_push_kv_int (out, "skipped_total",
                      (int64_t)atomic_load(&g_skipped_total));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix",
                      atomic_load(&g_last_step_unix));
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    stage_dump_counters(out, g_stage);
    stage_dump_health(out, STAGE_NAME, g_stage);
    return true;
}
