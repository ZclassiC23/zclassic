/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Stage primitive — implementation. See util/stage.h for design notes.
 *
 * Persistence is a single SQLite table:
 *
 *   CREATE TABLE IF NOT EXISTS stage_cursor (
 *       name TEXT PRIMARY KEY,
 *       cursor INTEGER NOT NULL,
 *       updated_at INTEGER NOT NULL
 *   );
 *
 * One row per stage. The step function runs inside an explicit
 * transaction so that output writes (which the step performs against
 * the same sqlite handle through its `user` pointer) commit atomically
 * with the cursor advance.
 *
 * Direct prepared-statement calls carry the `raw-sql-ok:kernel-primitive`
 * marker on each call site — this primitive intentionally sits below the
 * AR lifecycle because it is the foundation other models will use, and a
 * cursor row is not a model. */

#include "platform/time_compat.h"
#include "util/stage.h"

#include "core/utiltime.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct stage {
    char           name[STAGE_NAME_MAX];
    stage_step_fn  step;
    void          *user;

    /* Last persisted cursor value (mirrored in stage_cursor table).
     * Updated only on successful commit (under `lock`); read lock-free via
     * stage_cursor() from each Job's dump_state_json on other threads, so it
     * is _Atomic to make those concurrent reads race-free. */
    pthread_mutex_t lock;
    _Atomic uint64_t cursor;

    _Atomic uint64_t advanced_count;
    _Atomic uint64_t blocked_count;
    _Atomic uint64_t idle_count;
    _Atomic uint64_t error_count;

    /* Step timing — see util/stage.h "Step timing" for the contract. Written
     * only from inside stage_run_once() (single-writer, matches the counters
     * above); read lock-free from dump_state_json on other threads. */
    _Atomic int64_t last_step_us;
    _Atomic int64_t step_us_ewma;
};

/* ── Helpers ───────────────────────────────────────────────────────── */

const char *stage_result_name(job_result_t r)
{
    switch (r) {
    case JOB_ADVANCED: return "advanced";
    case JOB_BLOCKED:  return "blocked";
    case JOB_IDLE:     return "idle";
    case JOB_FATAL:    return "error";
    }
    return "(invalid)";
}

static int64_t wall_now_s(void)
{
    struct timespec ts;
    platform_time_realtime_timespec(&ts);
    return (int64_t)ts.tv_sec;
}

/* ── FATAL latch (loud-once + drain witness) ───────────────────────────
 *
 * A JOB_FATAL from a stage step is a terminal verdict: the runner rolls
 * back, bumps error_count, and returns. Historically that bump was the
 * ONLY witness — visible in zcl_state JSON but never in node.log, and
 * indistinguishable from a healthy JOB_IDLE to a count-only drain driver.
 *
 * This latch fixes both: every FATAL return funnels through
 * stage_note_fatal(), which (a) emits ONE rate-limited LOUD line to
 * node.log naming the stage + reason, and (b) records a monotonically
 * increasing fatal generation that a drain driver reads after a pass via
 * stage_fatal_generation()/stage_last_fatal() to tell FATAL from IDLE.
 * The snapshot is mutex-guarded because distinct stages run on distinct
 * threads. The throttle mirrors the "loud once per window" idiom used by
 * the supervisor: at most one log line per FATAL_LOG_WINDOW_S per stage. */
#define FATAL_LOG_WINDOW_S 5

static pthread_mutex_t  g_fatal_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_fatal_generation; /* bumped on EVERY fatal */
static char             g_fatal_stage[STAGE_NAME_MAX];
static char             g_fatal_reason[128];
static int64_t          g_fatal_last_log_s; /* throttle: per-stage window */
static char             g_fatal_last_log_stage[STAGE_NAME_MAX];

uint64_t stage_fatal_generation(void)
{
    return atomic_load(&g_fatal_generation);
}

bool stage_last_fatal(char *stage_out, size_t stage_cap,
                      char *reason_out, size_t reason_cap)
{
    if (atomic_load(&g_fatal_generation) == 0)
        return false;
    pthread_mutex_lock(&g_fatal_lock);
    if (stage_out && stage_cap)
        snprintf(stage_out, stage_cap, "%s", g_fatal_stage);
    if (reason_out && reason_cap)
        snprintf(reason_out, reason_cap, "%s", g_fatal_reason);
    pthread_mutex_unlock(&g_fatal_lock);
    return true;
}

job_result_t stage_record_fatal(const char *stage_name, const char *reason)
{
    const char *name = (stage_name && stage_name[0]) ? stage_name : "(null)";

    pthread_mutex_lock(&g_fatal_lock);
    atomic_fetch_add(&g_fatal_generation, 1u);
    snprintf(g_fatal_stage,  sizeof(g_fatal_stage),  "%s", name);
    snprintf(g_fatal_reason, sizeof(g_fatal_reason), "%s",
             reason ? reason : "(no reason)");
    /* Throttle: log loud at most once per window per stage name. A new
     * stage name always logs immediately so no FATAL stage is masked. */
    int64_t now = wall_now_s();
    bool same_stage = (strcmp(g_fatal_last_log_stage, name) == 0);
    bool emit = !same_stage || (now - g_fatal_last_log_s >= FATAL_LOG_WINDOW_S);
    if (emit) {
        g_fatal_last_log_s = now;
        snprintf(g_fatal_last_log_stage, sizeof(g_fatal_last_log_stage),
                 "%s", name);
    }
    pthread_mutex_unlock(&g_fatal_lock);

    if (emit)
        fprintf(stderr,  // obs-ok:stage-fatal-loud
                "[stage] FATAL %s: %s (cursor unchanged; error verdict)\n",
                name, reason ? reason : "(no reason)");
    return JOB_FATAL;
}

/* Record a FATAL verdict: bump error_count, latch (stage, reason) for the
 * drain driver, and emit ONE throttled LOUD line. Always returns JOB_FATAL
 * so call sites read `return stage_note_fatal(s, "...");`. */
static job_result_t stage_note_fatal(stage_t *s, const char *reason)
{
    if (s) atomic_fetch_add(&s->error_count, 1u);
    return stage_record_fatal(s ? s->name : "(null)", reason);
}

/* ── Schema bootstrap ──────────────────────────────────────────────── */

bool stage_table_ensure(sqlite3 *db)
{
    if (!db) LOG_FAIL("stage", "table_ensure: null db");
    const char *sql =
        "CREATE TABLE IF NOT EXISTS stage_cursor ("
        "  name TEXT PRIMARY KEY,"
        "  cursor INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] table_ensure: %s\n",  // obs-ok:stage-schema-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

/* ── Cursor read / write (direct sqlite3) ─────────────────────────── */

/* Read the persisted cursor for `name`. On miss, *out is set to 0 and
 * the function returns true (a fresh stage starts at cursor 0). */
static bool cursor_read(sqlite3 *db, const char *name, uint64_t *out)
{
    *out = 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT cursor FROM stage_cursor WHERE name = ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[stage] cursor_read prepare: %s\n",  // obs-ok:stage-prepare-failure
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW) {
        *out = (uint64_t)sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        fprintf(stderr, "[stage] cursor_read step: rc=%d %s\n",  // obs-ok:stage-step-failure
                rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

/* UPSERT the cursor row. Caller is responsible for transaction
 * lifecycle — this function only runs the UPSERT statement. */
static bool cursor_write_locked(sqlite3 *db, const char *name, uint64_t value)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO stage_cursor (name, cursor, updated_at) "
        "VALUES (?1, ?2, ?3) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  cursor = excluded.cursor, "
        "  updated_at = excluded.updated_at";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[stage] cursor_write prepare: %s\n",  // obs-ok:stage-prepare-failure
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text (stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)value);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)wall_now_s());
    int rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[stage] cursor_write step: rc=%d %s\n",  // obs-ok:stage-step-failure
                rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

stage_t *stage_create(const char *name, stage_step_fn step, void *user)
{
    if (!name || name[0] == '\0') LOG_NULL("stage", "create: empty name");
    if (!step)                    LOG_NULL("stage", "create: null step");
    size_t nlen = strlen(name);
    if (nlen >= STAGE_NAME_MAX)
        LOG_NULL("stage", "create: name too long: %zu", nlen);

    stage_t *s = zcl_calloc(1, sizeof(*s), "stage_t");
    if (!s) LOG_NULL("stage", "create: alloc failed for '%s'", name);
    memcpy(s->name, name, nlen + 1);
    s->step = step;
    s->user = user;
    pthread_mutex_init(&s->lock, NULL);
    atomic_store(&s->advanced_count, 0u);
    atomic_store(&s->blocked_count,  0u);
    atomic_store(&s->idle_count,     0u);
    atomic_store(&s->error_count,    0u);
    atomic_store(&s->last_step_us,   0);
    atomic_store(&s->step_us_ewma,   0);
    return s;
}

void stage_destroy(stage_t *s)
{
    if (!s) return;
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const char *stage_name(const stage_t *s)          { return s ? s->name : ""; }
uint64_t    stage_cursor(const stage_t *s)
{
    if (!s) return 0;
    /* The lock guards the cursor field but a torn read of 64 bits is
     * not possible on the platforms we target; readers tolerate a
     * very-slightly-stale value, so we skip locking on the hot read
     * path. */
    return s->cursor;
}

uint64_t stage_advanced_count(const stage_t *s)
{ return s ? atomic_load(&s->advanced_count) : 0u; }
uint64_t stage_blocked_count(const stage_t *s)
{ return s ? atomic_load(&s->blocked_count) : 0u; }
uint64_t stage_idle_count(const stage_t *s)
{ return s ? atomic_load(&s->idle_count) : 0u; }
uint64_t stage_error_count(const stage_t *s)
{ return s ? atomic_load(&s->error_count) : 0u; }

int64_t stage_last_step_us(const stage_t *s)
{ return s ? atomic_load(&s->last_step_us) : 0; }
int64_t stage_step_us_ewma(const stage_t *s)
{ return s ? atomic_load(&s->step_us_ewma) : 0; }

/* EWMA update, alpha = 1/16, integer arithmetic (matches the kernel's other
 * load-average-style smoothing, e.g. the supervisor's tick-age tracking):
 * next = prev + (sample - prev) / 16. The first sample seeds the EWMA
 * directly (prev == 0 is indistinguishable from "never sampled", and a
 * seeded start avoids a many-step ramp from zero on stages that step
 * rarely). Called only from stage_run_once(), which is single-writer per
 * stage_t (see "Threading" in util/stage.h), so plain atomic_load/store is
 * sufficient — no read-modify-write race is possible. */
static void stage_record_step_timing(stage_t *s, int64_t elapsed_us)
{
    if (!s || elapsed_us < 0)
        return;
    /* GetTimeMicros() has ~1us granularity: a trivial step body (a fixture,
     * or a real step that only touches in-memory state) can complete inside
     * the same tick as its start read, measuring exactly 0. Floor to 1 so a
     * step that genuinely ran is always distinguishable from "never sampled"
     * (also 0, before the first step) rather than aliasing onto it. */
    if (elapsed_us == 0)
        elapsed_us = 1;
    atomic_store(&s->last_step_us, elapsed_us);
    int64_t prev = atomic_load(&s->step_us_ewma);
    int64_t next = (prev == 0) ? elapsed_us : prev + (elapsed_us - prev) / 16;
    atomic_store(&s->step_us_ewma, next);
}

/* ── Batched drain (one COMMIT per batch, not one per step) ─────────────
 *
 * When a drain driver opens a batch txn (stage_batch_begin), the per-step
 * BEGIN/COMMIT in stage_run_once is replaced by a per-step SAVEPOINT so the
 * whole batch commits once. The flag is process-global because
 * progress_store_tx_lock (recursive) already serializes every progress.kv
 * txn — at most one batch can be open at a time. The caller holds that lock
 * across begin..end (see util/stage.h). */
#define STAGE_SAVEPOINT_NAME "stage_step"
static _Atomic bool g_batch_open = false;
/* Set when a step enrols durable, correct work into the open batch that did
 * NOT advance the forward cursor (a reorg unwind). stage_batch_end then COMMITs
 * the batch even when advanced==0, instead of rolling the unwind back. Cleared
 * on every begin. */
static _Atomic bool g_batch_dirty = false;

/* Pre-commit hook (see util/stage.h). Set once at boot before any drain
 * thread runs, so a plain pointer read in stage_batch_end() is race-free. */
static stage_batch_precommit_fn g_precommit_hook = NULL;

void stage_batch_set_precommit_hook(stage_batch_precommit_fn fn)
{
    g_precommit_hook = fn;
}

/* Wall time of the outer batch COMMIT — the one fsync point a batched drain
 * still pays per batch. Process-global like g_batch_open (at most one batch
 * commits at a time), same seeded 1/16 EWMA + floor-to-1 idiom as
 * stage_record_step_timing above. */
static _Atomic int64_t g_batch_commit_last_us;
static _Atomic int64_t g_batch_commit_us_ewma;

static void stage_batch_record_commit_timing(int64_t elapsed_us)
{
    if (elapsed_us <= 0)
        elapsed_us = 1;
    atomic_store(&g_batch_commit_last_us, elapsed_us);
    int64_t prev = atomic_load(&g_batch_commit_us_ewma);
    int64_t next = (prev == 0) ? elapsed_us : prev + (elapsed_us - prev) / 16;
    atomic_store(&g_batch_commit_us_ewma, next);
}

int64_t stage_batch_commit_us_ewma(void)
{
    return atomic_load(&g_batch_commit_us_ewma);
}

bool stage_batch_active(void)
{
    return atomic_load(&g_batch_open);
}

void stage_batch_mark_dirty(void)
{
    atomic_store(&g_batch_dirty, true);
}

bool stage_batch_dirty(void)
{
    return atomic_load(&g_batch_dirty);
}

bool stage_batch_begin(sqlite3 *db)
{
    if (!db) return false;
    if (atomic_load(&g_batch_open)) {
        /* Nested begin without an end is a caller bug; refuse rather than
         * silently mask it (a stray COMMIT would defeat the co-commit
         * invariant). */
        fprintf(stderr, "[stage] batch_begin: a batch is already open\n");  // obs-ok:stage-begin-failure
        return false;
    }
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] batch BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    atomic_store(&g_batch_open, true);
    atomic_store(&g_batch_dirty, false);
    return true;
}

bool stage_batch_end(sqlite3 *db, bool commit)
{
    if (!db) return false;
    if (!atomic_load(&g_batch_open)) {
        /* No batch open — nothing to finish. Treat as benign no-op. */
        return true;
    }
    /* Ordering seam: before the outer COMMIT, give the pre-commit hook a
     * chance to flush any on-disk artifact a committed row will reference
     * (deferred block-body fdatasync). A veto (false) means those bytes are
     * NOT durable, so committing the rows that reference them would break the
     * crash-ordering invariant — ROLLBACK the whole batch instead. Only on the
     * commit path; a ROLLBACK never needs the flush. */
    if (commit && g_precommit_hook && !g_precommit_hook()) {
        fprintf(stderr, "[stage] batch pre-commit hook vetoed COMMIT; "  // obs-ok:stage-commit-failure
                "rolling back\n");
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        atomic_store(&g_batch_open, false);
        return false;
    }
    char *err = NULL;
    const char *fini = commit ? "COMMIT" : "ROLLBACK";
    int64_t commit_t0 = commit ? GetTimeMicros() : 0;
    int rc = sqlite3_exec(db, fini, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[stage] batch %s: %s\n",  // obs-ok:stage-commit-failure
                fini, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        /* The outer txn may still be open; force it closed so the next
         * batch can BEGIN cleanly. */
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        atomic_store(&g_batch_open, false);
        return false;
    }
    if (err) sqlite3_free(err);
    if (commit)
        stage_batch_record_commit_timing(GetTimeMicros() - commit_t0);
    atomic_store(&g_batch_open, false);
    return true;
}

/* Commit this step's work. Batched: RELEASE the savepoint (the step's
 * writes stay enrolled in the still-open outer batch txn, to be COMMITted
 * once by stage_batch_end). Unbatched: COMMIT the step's own txn. Returns
 * the sqlite rc; *err set on failure (caller frees). */
static int stage_step_commit(sqlite3 *db, bool batched, char **err)
{
    return sqlite3_exec(db,
        batched ? "RELEASE SAVEPOINT " STAGE_SAVEPOINT_NAME : "COMMIT",
        NULL, NULL, err);
}

/* Discard this step's work. Batched: ROLLBACK TO the savepoint (undo only
 * this block's partial writes) then RELEASE it (so the savepoint name is
 * free for the next step), leaving the outer batch txn — and every earlier
 * advanced block in it — open and intact. Unbatched: ROLLBACK the step's
 * own txn. Best-effort (matches the pre-batch ROLLBACK call sites). */
static void stage_step_rollback(sqlite3 *db, bool batched)
{
    if (batched) {
        sqlite3_exec(db, "ROLLBACK TO SAVEPOINT " STAGE_SAVEPOINT_NAME,
                     NULL, NULL, NULL);
        sqlite3_exec(db, "RELEASE SAVEPOINT " STAGE_SAVEPOINT_NAME,
                     NULL, NULL, NULL);
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
}

/* ── Run one step ──────────────────────────────────────────────────── */

job_result_t stage_run_once(stage_t *s, sqlite3 *db)
{
    if (!s || !db) {
        fprintf(stderr, "[stage] run_once: null arg s=%p db=%p\n",  // obs-ok:stage-arg-failure
                (void *)s, (void *)db);
        return stage_note_fatal(s, "run_once: null arg");
    }

    pthread_mutex_lock(&s->lock);
    progress_store_tx_lock();

    /* Load the persisted cursor before invoking the step. We don't
     * trust the cached `s->cursor` because a sibling process (cold
     * import) could have updated it; reading inside the txn gives us
     * the freshest value visible to this connection. */
    uint64_t cur = 0;
    if (!cursor_read(db, s->name, &cur)) {
        progress_store_tx_unlock();
        pthread_mutex_unlock(&s->lock);
        return stage_note_fatal(s, "cursor_read failed (sqlite)");
    }
    s->cursor = cur;

    struct stage_step_ctx ctx = {
        .cursor_in  = cur,
        .cursor_out = cur,
        .user       = s->user,
    };
    /* Caller-prepared blocker_record fields start zero. */
    memset(&ctx.blocker, 0, sizeof(ctx.blocker));

    /* Begin an explicit transaction so the step's writes + our cursor
     * UPSERT land atomically. If the step itself wants to do bulk I/O
     * outside the transaction it must roll its own; the contract here
     * is "cursor advance and step output are atomic."
     *
     * Batched drain: when a drain driver has an outer batch txn open
     * (stage_batch_begin), open a SAVEPOINT instead of a new BEGIN — the
     * per-block atomicity is preserved (the savepoint isolates this block's
     * coin write + cursor + *_log row), but the whole batch commits ONCE.
     * `batched` is sampled once and used for every open/finish call below so
     * the txn shape is internally consistent for this step. */
    const bool batched = atomic_load(&g_batch_open);
    const char *txn_open  = batched ? "SAVEPOINT " STAGE_SAVEPOINT_NAME
                                    : "BEGIN IMMEDIATE";
    char *err = NULL;
    if (sqlite3_exec(db, txn_open, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        pthread_mutex_unlock(&s->lock);
        return stage_note_fatal(s, "BEGIN IMMEDIATE failed");
    }

    /* Time exactly the per-stage step body — the single seam every one of
     * the eight Job stages flows through (see util/stage.h "Step timing").
     * One GetTimeMicros() pair per step is cheap relative to the step
     * itself (SQL reads/writes, block parsing, proof verification). */
    int64_t step_t0 = GetTimeMicros();
    job_result_t r = s->step(&ctx);
    stage_record_step_timing(s, GetTimeMicros() - step_t0);

    if (r == JOB_ADVANCED) {
        if (ctx.cursor_out <= cur) {
            /* Contract violation: JOB_ADVANCED must move the cursor
             * forward. Roll back to keep the invariant intact. */
            fprintf(stderr,  // obs-ok:stage-advance-noop
                "[stage] %s: ADVANCED but cursor_out(%llu) <= cursor_in(%llu); rolling back\n",
                s->name,
                (unsigned long long)ctx.cursor_out,
                (unsigned long long)cur);
            stage_step_rollback(db, batched);
            progress_store_tx_unlock();
            pthread_mutex_unlock(&s->lock);
            return stage_note_fatal(s, "ADVANCED but cursor did not move");
        }
        if (!cursor_write_locked(db, s->name, ctx.cursor_out)) {
            stage_step_rollback(db, batched);
            progress_store_tx_unlock();
            pthread_mutex_unlock(&s->lock);
            return stage_note_fatal(s, "cursor_write failed (sqlite)");
        }
        if (stage_step_commit(db, batched, &err) != SQLITE_OK) {
            fprintf(stderr, "[stage] COMMIT: %s\n",  // obs-ok:stage-commit-failure
                    err ? err : "(no message)");
            if (err) sqlite3_free(err);
            stage_step_rollback(db, batched);
            progress_store_tx_unlock();
            pthread_mutex_unlock(&s->lock);
            return stage_note_fatal(s, "COMMIT failed");
        }
        s->cursor = ctx.cursor_out;
        atomic_fetch_add(&s->advanced_count, 1u);
        progress_store_tx_unlock();
        pthread_mutex_unlock(&s->lock);
        return JOB_ADVANCED;
    }

    /* Non-advancing outcomes: roll back this step's work (the step may have
     * touched scratch state) and surface the result. Batched: undo only this
     * block's savepoint, leaving earlier advanced blocks in the open batch. */
    stage_step_rollback(db, batched);
    progress_store_tx_unlock();
    pthread_mutex_unlock(&s->lock);

    if (r == JOB_BLOCKED) {
        /* Record the blocker if the step filled out an id. Empty id
         * means the step signalled BLOCKED but didn't fill the record
         * — log and treat as ERROR so the caller is not misled. */
        if (ctx.blocker.id[0] == '\0') {
            return stage_note_fatal(s, "BLOCKED with empty blocker id");
        }
        if (ctx.blocker.owner_subsystem[0] == '\0') {
            /* Owner field is bounded; truncate the (longer) stage name
             * with explicit precision so -Wformat-truncation is happy. */
            snprintf(ctx.blocker.owner_subsystem,
                     sizeof(ctx.blocker.owner_subsystem),
                     "%.*s",
                     (int)(sizeof(ctx.blocker.owner_subsystem) - 1),
                     s->name);
        }
        (void)blocker_set(&ctx.blocker);
        atomic_fetch_add(&s->blocked_count, 1u);
        return JOB_BLOCKED;
    }

    if (r == JOB_IDLE) {
        atomic_fetch_add(&s->idle_count, 1u);
        return JOB_IDLE;
    }

    /* JOB_FATAL or any other value: the step itself signalled a terminal
     * error verdict (cursor unchanged). This is the dominant FATAL source —
     * surface it loudly and latch it for the drain driver. */
    return stage_note_fatal(s, "step returned FATAL verdict");
}

/* ── Boot-time restore ─────────────────────────────────────────────── */

/* Batch-aware transaction verbs for the standalone cursor writers below
 * (stage_set_cursor, stage_set_named_cursor_if_behind). These run on their
 * own when unbatched, but a drain batch (stage_batch_begin) may already hold
 * an outer BEGIN IMMEDIATE open when a stage's step body calls them on its
 * pre-step path — e.g. tip_finalize's rewind_cursor_if_active_chain_reorged
 * calls stage_set_cursor before opening its own per-step savepoint. SQLite
 * rejects a nested BEGIN ("cannot start a transaction within a transaction"),
 * so when a batch is open these must nest as a named SAVEPOINT instead. The
 * savepoint name is distinct from STAGE_SAVEPOINT_NAME so it never collides.
 * progress_store_tx_lock is recursive, so re-acquiring inside a batch is safe. */
#define STAGE_CURSOR_SP "stage_cursor_write"
static int cursor_txn_begin(sqlite3 *db, bool batched, char **err)
{
    return sqlite3_exec(db,
        batched ? "SAVEPOINT " STAGE_CURSOR_SP : "BEGIN IMMEDIATE", NULL, NULL, err);
}
static int cursor_txn_commit(sqlite3 *db, bool batched, char **err)
{
    return sqlite3_exec(db,
        batched ? "RELEASE SAVEPOINT " STAGE_CURSOR_SP : "COMMIT", NULL, NULL, err);
}
static void cursor_txn_rollback(sqlite3 *db, bool batched)
{
    if (batched) {
        sqlite3_exec(db, "ROLLBACK TO SAVEPOINT " STAGE_CURSOR_SP, NULL, NULL, NULL);
        sqlite3_exec(db, "RELEASE SAVEPOINT " STAGE_CURSOR_SP, NULL, NULL, NULL);
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
}

bool stage_set_cursor(stage_t *s, sqlite3 *db, uint64_t value)
{
    if (!s || !db) LOG_FAIL("stage", "set_cursor: null arg");
    pthread_mutex_lock(&s->lock);
    progress_store_tx_lock();
    bool batched = stage_batch_active();
    char *err = NULL;
    if (cursor_txn_begin(db, batched, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_cursor BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    if (!cursor_write_locked(db, s->name, value)) {
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    if (cursor_txn_commit(db, batched, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_cursor COMMIT: %s\n",  // obs-ok:stage-commit-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    s->cursor = value;
    /* A standalone cursor set is durable, non-advancing work (e.g.
     * tip_finalize's reorg rewind). When it enrolled in an outer drain batch
     * it persists only if that batch COMMITs — flag it dirty so a drain ending
     * advanced==0 commits instead of rolling the rewind back (the "persists
     * immediately" contract above; same class as the utxo_apply unwind). */
    if (batched) stage_batch_mark_dirty();
    progress_store_tx_unlock();
    pthread_mutex_unlock(&s->lock);
    return true;
}

bool stage_set_named_cursor(sqlite3 *db, const char *name, uint64_t value)
{
    if (!db || !name || name[0] == '\0')
        LOG_FAIL("stage", "set_named_cursor: invalid arg db=%p name=%p",
                 (void *)db, (const void *)name);

    if (!stage_table_ensure(db))
        return false;

    progress_store_tx_lock();
    bool batched = stage_batch_active();
    char *err = NULL;
    if (cursor_txn_begin(db, batched, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_named_cursor BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    if (!cursor_write_locked(db, name, value)) {
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        return false;
    }

    if (cursor_txn_commit(db, batched, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_named_cursor COMMIT: %s\n",  // obs-ok:stage-commit-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        return false;
    }

    if (batched) stage_batch_mark_dirty();
    progress_store_tx_unlock();
    return true;
}

bool stage_set_named_cursor_if_behind(sqlite3 *db, const char *name,
                                      uint64_t value)
{
    if (!db || !name || name[0] == '\0')
        LOG_FAIL("stage", "set_named_cursor: invalid arg db=%p name=%p",
                 (void *)db, (const void *)name);

    if (!stage_table_ensure(db))
        return false;

    progress_store_tx_lock();
    bool batched = stage_batch_active();
    char *err = NULL;
    if (cursor_txn_begin(db, batched, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_named_cursor BEGIN: %s\n",  // obs-ok:stage-begin-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }

    uint64_t current = 0;
    if (!cursor_read(db, name, &current)) {
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        return false;
    }
    if (current >= value) {
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        return true;
    }

    if (!cursor_write_locked(db, name, value)) {
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        return false;
    }
    if (cursor_txn_commit(db, batched, &err) != SQLITE_OK) {
        fprintf(stderr, "[stage] set_named_cursor COMMIT: %s\n",  // obs-ok:stage-commit-failure
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        cursor_txn_rollback(db, batched);
        progress_store_tx_unlock();
        return false;
    }
    /* Durable batched write — keep the batch from rolling it back (see
     * stage_set_cursor). The early "already ahead" path above rolled back and
     * never reaches here, so a no-op never marks the batch dirty. */
    if (batched) stage_batch_mark_dirty();
    progress_store_tx_unlock();
    return true;
}
