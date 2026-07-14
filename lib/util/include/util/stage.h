/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Stage primitive — a saga-style step that owns a persistent cursor.
 *
 * Why this exists
 * ----------------
 * The wedge class this primitive removes is
 * "chain advance silently stops making progress and nobody notices."
 * Today that progress lives in transient memory inside the chain-
 * advance coordinator. A stage primitive turns chain advance — and
 * every other long-running, batched workflow — into the same shape:
 *
 *   - A 64-bit cursor on disk identifies the last consumed unit.
 *   - A step function consumes the next unit (or batch), produces
 *     output, and writes the new cursor in the SAME transaction.
 *   - On crash-mid-step, the cursor is unchanged on next boot, so the
 *     work is replayed idempotently.
 *
 * Stage states (every step returns a job_result_t; see jobs/job.h):
 *
 *   JOB_ADVANCED  — cursor moved; output committed
 *   JOB_BLOCKED   — typed blocker preventing progress; cursor unchanged
 *   JOB_IDLE      — no work available right now; cursor unchanged
 *   JOB_FATAL     — unexpected failure; cursor unchanged
 *
 * Persistence (v1)
 * -----------------
 * One SQLite table, `stage_cursor`, keyed by stage name. Reads and
 * writes use direct prepared statements rather than the AR lifecycle
 * because:
 *
 *   - A cursor is a single column, not a row aggregate.
 *   - Stages are kernel primitives; the AR lifecycle is designed for
 *     app-layer models. Pulling AR in here adds dependencies and
 *     erases the "one writer per cursor" simplicity.
 *
 * Both sqlite3_step call sites carry a `// raw-sql-ok:kernel-primitive`
 * marker so the lint gate doesn't fire.
 *
 * Threading
 * ----------
 * The stage struct is single-writer. Caller orchestrates one step at a
 * time per stage; concurrent stage_run_once calls on the same stage are
 * undefined behaviour. Multiple stages can run in parallel because each
 * one owns a distinct (name, sqlite_db_handle) pair. */

#ifndef ZCL_UTIL_STAGE_H
#define ZCL_UTIL_STAGE_H

#include "jobs/job.h"
#include "util/blocker.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STAGE_NAME_MAX 64

const char *stage_result_name(job_result_t r);

/* ── FATAL witness (loud + drain-distinguishable) ──────────────────────
 *
 * A JOB_FATAL from stage_run_once is a terminal verdict: the runner rolls
 * back, leaves the cursor unchanged, and returns. On its own that is
 * indistinguishable from a quiet JOB_IDLE to a drain driver that only
 * sums advance counts, so a FATAL-looping stage masquerades as a healthy
 * idle one. stage_run_once now latches every FATAL (process-global,
 * thread-safe) and emits one throttled loud line to node.log.
 *
 * A drain driver reads the generation BEFORE a pass and AFTER it: if it
 * moved while the pass advanced nothing, a stage went FATAL this pass (not
 * merely idle), and the driver should escalate (e.g. EV_OPERATOR_NEEDED)
 * rather than treat the no-advance as convergence. */

/* Monotonically increasing count of FATAL verdicts since process start.
 * A change across a drain pass means at least one stage went FATAL. */
uint64_t stage_fatal_generation(void);

/* Snapshot the most recent FATAL's (stage name, reason) into the caller's
 * buffers. Returns false (buffers untouched) if no FATAL has occurred. */
bool stage_last_fatal(char *stage_out, size_t stage_cap,
                      char *reason_out, size_t reason_cap);

/* Record a FATAL verdict for code that participates in a stage drain but is
 * not running inside stage_run_once() at the failure point. This updates the
 * same generation/last-fatal latch used by stage_run_once(), so supervisors
 * can distinguish "made no progress because fatal" from healthy idle. */
job_result_t stage_record_fatal(const char *stage_name, const char *reason);

/* Context passed to a step function. The step:
 *   - Reads `cursor_in` (the current persisted cursor).
 *   - Does bounded work.
 *   - On advance: writes `cursor_out` (must be > cursor_in) and returns
 *     JOB_ADVANCED. The framework commits the new cursor.
 *   - On blocked: fills `blocker` (caller-owned record) and returns
 *     JOB_BLOCKED. The framework records the blocker via blocker_set
 *     and leaves cursor untouched.
 *   - On idle: returns JOB_IDLE.
 *   - On error: returns JOB_FATAL. */
struct stage_step_ctx {
    uint64_t              cursor_in;
    uint64_t              cursor_out;
    struct blocker_record blocker;     /* populated iff JOB_BLOCKED */
    void                 *user;
};

typedef job_result_t (*stage_step_fn)(struct stage_step_ctx *ctx);

typedef struct stage stage_t;

/* Construct a stage. `name` must be non-empty and ≤ STAGE_NAME_MAX - 1.
 * `step` must be non-NULL. `user` is opaque, passed to the step on
 * every invocation. Returns NULL on bad input. */
stage_t *stage_create(const char *name, stage_step_fn step, void *user);
void     stage_destroy(stage_t *s);

const char *stage_name(const stage_t *s);
uint64_t    stage_cursor(const stage_t *s);

/* Counters (for observability / Prometheus). */
uint64_t stage_advanced_count(const stage_t *s);
uint64_t stage_blocked_count(const stage_t *s);
uint64_t stage_idle_count(const stage_t *s);
uint64_t stage_error_count(const stage_t *s);

/* ── Step timing (for observability — "how fast is this stage stepping") ─
 *
 * stage_run_once() times exactly one thing: the call to the stage's own
 * `step` function (the per-stage step_once body), the same seam every one
 * of the eight Job stages flows through. Every outcome (ADVANCED, BLOCKED,
 * IDLE, FATAL) is timed and folded in — this answers "is this stage's step
 * function getting slower" independent of whether it happens to be finding
 * work right now.
 *
 * last_step_us  — wall-clock duration (via GetTimeMicros()) of the most
 *                 recent step() call, floored to 1 (GetTimeMicros() has
 *                 ~1us granularity, so a sub-tick step is reported as 1,
 *                 not 0 — 0 is reserved for "never sampled", i.e. before
 *                 the first step).
 * step_us_ewma  — exponential moving average of step duration, alpha = 1/16
 *                 (integer arithmetic: ewma += (sample - ewma) / 16), seeded
 *                 directly from the first sample so there is no slow ramp
 *                 from zero. 0 before the first step.
 *
 * Both are updated with a plain atomic_store after the step returns — the
 * struct is single-writer (see "Threading" above), and dump readers on other
 * threads tolerate a benign torn/stale read exactly like the existing
 * advanced/blocked/idle/error counters, so no lock is taken here. A caller
 * wanting a rate can derive steps_per_sec ≈ 1e6 / step_us_ewma; that is
 * computed at dump time (see app/jobs/include/jobs/stage_helpers.h
 * stage_dump_counters()) rather than stored, so there is exactly one new
 * unit of state per stage (the duration), not a second redundant counter. */
int64_t stage_last_step_us(const stage_t *s);
int64_t stage_step_us_ewma(const stage_t *s);

/* Initialize the `stage_cursor` table on the given DB handle. Safe to
 * call repeatedly. */
bool stage_table_ensure(sqlite3 *db);

/* Run one step:
 *   1. Read the current cursor from `stage_cursor` (defaults to 0 on
 *      first run).
 *   2. Invoke the step function with cursor_in populated.
 *   3. If the step returns JOB_ADVANCED, persist cursor_out atomically
 *      in the same transaction (the step body should itself enroll any
 *      output writes into the outer txn via the user pointer).
 *   4. If JOB_BLOCKED, call blocker_set with the filled record.
 *
 * Returns the step's result code. JOB_FATAL if persistence fails. */
job_result_t stage_run_once(stage_t *s, sqlite3 *db);

/* ── Batched drain (one COMMIT per batch of steps, not one per step) ────
 *
 * Each stage_run_once normally wraps its step in its own BEGIN IMMEDIATE /
 * COMMIT, so a drain of N blocks issues N commits (N fsync points in the
 * worst case). A drain driver may instead open ONE outer transaction around
 * a bounded batch of steps and commit it once:
 *
 *     progress_store_tx_lock();
 *     stage_batch_begin(db);
 *     for (i = 0; i < cap; i++) {
 *         if (stage_run_once(s, db) != JOB_ADVANCED) break;
 *     }
 *     stage_batch_end(db, advanced > 0);   // COMMIT if any step advanced
 *     progress_store_tx_unlock();
 *
 * While a batch is open, each stage_run_once uses a per-step SAVEPOINT
 * instead of BEGIN/COMMIT: an advancing step RELEASEs (its coin write +
 * cursor + *_log row stay atomic, exactly as before), and a non-advancing
 * step ROLLBACK-TOs its savepoint (discarding only that block's partial
 * work, leaving earlier advanced blocks in the open batch intact). This
 * changes ONLY when bytes are flushed (one COMMIT per batch) — never WHAT
 * is computed, written, or accepted, and never the per-block atomicity.
 *
 * The batch flag is process-global and guarded by progress_store_tx_lock
 * (a recursive mutex that already serializes every progress.kv txn), so at
 * most one batch is open at a time. The caller MUST hold that lock across
 * begin..end and MUST pair every begin with exactly one end.
 *
 * stage_batch_active() reports whether a batch txn is currently open on the
 * calling path (for assertions / tests). */
bool stage_batch_begin(sqlite3 *db);
bool stage_batch_end(sqlite3 *db, bool commit);
bool stage_batch_active(void);

/* Global EWMA (us, alpha=1/16, seeded from the first sample) of the outer
 * batch-COMMIT wall time — the one per-batch fsync point a batched drain
 * still pays. At most one batch commits at a time (see above), so a single
 * process-global sample is exact. 0 before the first batched COMMIT. */
int64_t stage_batch_commit_us_ewma(void);

/* ── Pre-commit ordering hook ──────────────────────────────────────────
 * stage_batch_end() invokes this hook (when set) immediately BEFORE the outer
 * COMMIT, and only when it is about to commit. If the hook returns false it
 * VETOES the commit: stage_batch_end() ROLLBACKs the batch and returns false,
 * so no stage cursor / *_log row becomes durable. This is the seam that keeps
 * a durable stage marker from outliving an unsynced on-disk artifact it
 * references — the reducer registers disk_block_io_sync_pending() here so
 * deferred block-body writes are fdatasync()ed before any row referencing them
 * commits (see storage/disk_block_io.h). NULL clears the hook (the default).
 *
 * Set once at boot, before any drain thread runs; process-global, and the hook
 * fires under the same progress_store_tx_lock the batch is held under. The hook
 * must not itself open a progress.kv transaction. */
typedef bool (*stage_batch_precommit_fn)(void);
void stage_batch_set_precommit_hook(stage_batch_precommit_fn fn);

/* A drain commits its batch only when at least one step ADVANCED. But a step
 * can do durable, correct work WITHOUT advancing the cursor — a reorg unwind
 * removes the losing branch and rewinds the cursor, then the winning block is
 * (correctly) not re-applied in the same drain. stage_batch_mark_dirty() flags
 * that such durable work is enrolled in the open batch so stage_batch_end()
 * COMMITs it instead of rolling it back (the pre-batch unwind committed in its
 * own txn; batching must not silently discard it). Cleared by stage_batch_begin.
 */
void stage_batch_mark_dirty(void);
bool stage_batch_dirty(void);

/* Boot-time restore: explicitly set the cursor. Persists immediately.
 * Intended for replaying a known-good cursor on import. */
bool stage_set_cursor(stage_t *s, sqlite3 *db, uint64_t value);

/* Stamp a cursor row when the stage object is not locally available.
 * Unlike stage_set_named_cursor_if_behind(), this may rewind. Use only when a
 * validated repair has proven that rows at or above value must be replayed. */
bool stage_set_named_cursor(sqlite3 *db, const char *name, uint64_t value);

/* Stamp a cursor row when the stage object is not locally available.
 * Used by trusted bootstrap/restore anchors that must align a pipeline
 * boundary before the stage's next tick reloads the persisted cursor. This
 * never rewinds: if the stored cursor is already >= value it is a no-op. */
bool stage_set_named_cursor_if_behind(sqlite3 *db, const char *name,
                                      uint64_t value);

#endif /* ZCL_UTIL_STAGE_H */
