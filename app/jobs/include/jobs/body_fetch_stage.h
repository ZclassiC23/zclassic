/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_fetch_stage — reducer Job stage.
 *
 * Consumes `validate_headers_log` and
 * records, per height, whether the block body is observable on disk.
 * This stage is an authoritative reducer input, but it does not fetch from
 * peers directly; `msg_blocks` and the block source policy still own body
 * acquisition while this stage records the durable observation.
 *
 * Per-height behaviour
 * --------------------
 *   1. Read `(height, ok)` from `validate_headers_log`.
 *   2. If validate ok=0 (header failed PoW/Equihash earlier): log a
 *      `source='skipped_invalid'` row, ok=0, advance cursor — there is
 *      no point fetching a body for a known-bad header.
 *   3. Else: look up `active_chain_at(ms, h)` and check the body
 *      availability flag (`BLOCK_HAVE_DATA` in `bi->nStatus`).
 *        - Available: log `source='disk'`, ok=1, advance cursor.
 *        - Not yet available: JOB_IDLE (cursor unchanged). The next
 *          tick will retry; this is the natural backpressure that keeps
 *          body_fetch from racing past msg_blocks.
 *
 * Cursor floor
 * ------------
 * `body_fetch_cursor ≤ validate_headers_cursor` always. If the floor
 * is reached, `step_once` returns JOB_IDLE; the supervisor will retry
 * each tick. The cursor never advances ahead of validate, so we never
 * record body status for a header whose PoW we haven't checked.
 *
 * Schema
 * -------
 *   CREATE TABLE IF NOT EXISTS body_fetch_log (
 *     height       INTEGER PRIMARY KEY,
 *     hash         BLOB    NOT NULL,
 *     source       TEXT    NOT NULL,   -- 'disk' | 'skipped_invalid'
 *     bytes        INTEGER NOT NULL DEFAULT 0,
 *     fetched_at   INTEGER NOT NULL,
 *     ok           INTEGER NOT NULL,   -- 1 = body observed; 0 = skipped
 *     fail_reason  TEXT
 *   );
 *
 * Test seam
 * ----------
 * No injectable validator — tests drive availability by manipulating
 * `block_index.nStatus` on synthetic blocks. Keeps the production path
 * trivial; the BLOCK_HAVE_DATA bit is the only signal we read.
 *
 * Lifecycle
 * ----------
 * `init` binds the stage to a `main_state`, ensures the
 * `body_fetch_log` schema, and stages the cursor primitive. `step_once`
 * runs one step. `shutdown` disposes the stage and clears per-init
 * counters. Supervisor wiring lives in `config/src/boot_services.c` —
 * `staged.body_fetch` is registered with `period_secs=2`. */

#ifndef ZCL_SERVICES_BODY_FETCH_STAGE_H
#define ZCL_SERVICES_BODY_FETCH_STAGE_H

#include "util/stage.h"

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct json_value;

/* Max steps drained per supervisor tick. Each step is one in-memory
 * flag check + one small SQL insert; 500 keeps churn modest while
 * letting a backlog catch up in minutes, not hours. */
#define BODY_FETCH_BATCH_PER_TICK 500

/* Bind the stage to `ms` and ensure the body_fetch_log schema in
 * progress.kv. Idempotent — a second call against the same `ms`
 * returns true. Requires `progress_store_open` to have succeeded
 * first. */
bool body_fetch_stage_init(struct main_state *ms);

/* Run one stage step. Returns the stage result code. Safe to call before
 * init (returns JOB_IDLE). */
job_result_t body_fetch_stage_step_once(void);

/* Drain up to `max_steps` consecutive ADVANCE steps. Stops early on
 * IDLE, BLOCKED, or ERROR. Returns the count of ADVANCED steps. */
int body_fetch_stage_drain(int max_steps);

/* Disarm + free. Idempotent. */
void body_fetch_stage_shutdown(void);

/* Observability. */
uint64_t body_fetch_stage_cursor(void);
/* Step-timing EWMA (us); see util/stage.h. 0 if never stepped. */
int64_t  body_fetch_stage_step_us_ewma(void);
uint64_t body_fetch_stage_observed_total(void);   /* source='disk' rows */
uint64_t body_fetch_stage_skipped_total(void);    /* source='skipped_invalid' */

/* `zclassic23 dumpstate body_fetch` (dump-state convention). */
bool body_fetch_stage_dump_state_json(struct json_value *out,
                                       const char *key);

#endif /* ZCL_SERVICES_BODY_FETCH_STAGE_H */
