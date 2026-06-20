/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/*
 * verify_queue — tagged batch -> thread_pool fan-out -> AND-reduce.
 * See validation/verify_queue.h for the contract. ADDITIVE; not wired into
 * the staged reducer or any consensus path.
 */

#include "validation/verify_queue.h"

#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "util/util.h"      /* GetNumCores (would-be worker count) */
#include "json/json.h"

#include <stdatomic.h>
#include <stddef.h>

/* ── Observability counters (additive) ───────────────────────────────────
 * Process-lifetime, lock-free. Incremented on the existing hot path inside
 * verify_queue_submit_batch(); they never feed back into a verdict, so they
 * cannot perturb the deterministic result. Zero until the engine is
 * exercised (today: the unit tests in test_verify_engine). */
static _Atomic int64_t g_vq_batches_submitted = 0;  /* every submit call     */
static _Atomic int64_t g_vq_empty_batches     = 0;  /* n==0 (vacuous)        */
static _Atomic int64_t g_vq_serial_batches    = 0;  /* ran inline (-par=1)   */
static _Atomic int64_t g_vq_parallel_batches  = 0;  /* fanned across workers */
static _Atomic int64_t g_vq_jobs_processed    = 0;  /* per-job verdicts run  */

bool verify_queue_submit_batch(struct thread_pool *pool,
                               struct verify_job *jobs, int n)
{
    if (n < 0)
        LOG_FAIL("verify_queue", "negative job count %d", n);
    if (n > 0 && !jobs)
        LOG_FAIL("verify_queue", "NULL jobs with n=%d", n);

    /* Observability (additive): count every well-formed submission. Does not
     * touch the verdict. */
    atomic_fetch_add_explicit(&g_vq_batches_submitted, 1, memory_order_relaxed);

    if (n == 0) {
        atomic_fetch_add_explicit(&g_vq_empty_batches, 1, memory_order_relaxed);
        return true;   /* vacuous: empty batch passes */
    }

    /* Serial path: no pool, or a pool with zero worker threads (-par=1).
     * Run every job inline on the caller; this is the reference order the
     * parallel path must match bit-for-bit. */
    bool serial = (pool == NULL) || (thread_pool_worker_count(pool) == 0);

    /* Observability (additive): route + job-count bookkeeping, lock-free. */
    if (serial)
        atomic_fetch_add_explicit(&g_vq_serial_batches, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&g_vq_parallel_batches, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_vq_jobs_processed, (int64_t)n,
                              memory_order_relaxed);

    if (serial) {
        for (int i = 0; i < n; i++)
            jobs[i].result = jobs[i].fn ? jobs[i].fn(jobs[i].arg) : false;
    } else {
        /* Build a task per job whose result_out points straight at the
         * per-job slot — no shared counter. */
        struct thread_pool_task *tasks =
            zcl_calloc((size_t)n, sizeof(*tasks), "verify_queue tasks");
        if (!tasks) {
            /* Degrade to serial rather than drop the batch. */
            LOG_WARN("verify_queue",
                     "task alloc failed for n=%d; running serial", n);
            for (int i = 0; i < n; i++)
                jobs[i].result = jobs[i].fn ? jobs[i].fn(jobs[i].arg) : false;
        } else {
            for (int i = 0; i < n; i++) {
                tasks[i].fn = jobs[i].fn;
                tasks[i].arg = jobs[i].arg;
                tasks[i].result_out = &jobs[i].result;
            }
            if (!thread_pool_run_batch(pool, tasks, n)) {
                free(tasks);
                LOG_FAIL("verify_queue", "pool run_batch failed for n=%d", n);
            }
            free(tasks);
        }
    }

    /* Single AND-reduce over the per-job verdict slots. */
    bool all_ok = true;
    for (int i = 0; i < n; i++)
        all_ok = all_ok && jobs[i].result;
    return all_ok;
}

/* ── State introspection (additive) ──────────────────────────────────────
 * See CLAUDE.md "Adding state introspection". `out` is already a JSON object
 * (the diagnostics registry called json_set_object()); we only push keys and
 * allocate nothing. Reentrant-safe: every read is an atomic_load. */
bool verify_engine_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;

    /* Worker count a default pool WOULD spawn right now: GetNumCores()-1,
     * clamped to >= 1 (mirrors thread_pool_start()'s sizing). No live pool
     * exists yet, so this is the would-be count, not an active one. */
    int cores = GetNumCores();          /* util.c clamps to >= 1 */
    int would_be_workers = cores - 1;
    if (would_be_workers < 1)
        would_be_workers = 1;

    int64_t batches  = atomic_load_explicit(&g_vq_batches_submitted,
                                            memory_order_relaxed);
    int64_t empty    = atomic_load_explicit(&g_vq_empty_batches,
                                            memory_order_relaxed);
    int64_t serial   = atomic_load_explicit(&g_vq_serial_batches,
                                            memory_order_relaxed);
    int64_t parallel = atomic_load_explicit(&g_vq_parallel_batches,
                                            memory_order_relaxed);
    int64_t jobs     = atomic_load_explicit(&g_vq_jobs_processed,
                                            memory_order_relaxed);

    /* Honest lifecycle flag: the engine is built and exercisable but is NOT
     * wired into the staged reducer or any consensus path, and no production
     * call site instantiates a live pool (only the unit tests do). */
    json_push_kv_bool(out, "wired", false);
    json_push_kv_str(out, "status", "additive_unwired");
    json_push_kv_int(out, "would_be_workers", (int64_t)would_be_workers);
    json_push_kv_int(out, "num_cores", (int64_t)cores);

    json_push_kv_int(out, "batches_submitted", batches);
    json_push_kv_int(out, "empty_batches", empty);
    json_push_kv_int(out, "serial_batches", serial);
    json_push_kv_int(out, "parallel_batches", parallel);
    json_push_kv_int(out, "jobs_processed", jobs);

    return true;
}
