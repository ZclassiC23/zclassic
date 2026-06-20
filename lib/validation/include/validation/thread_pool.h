/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/*
 * thread_pool — a persistent, core-saturating worker pool for parallel
 * verification.
 *
 * ADDITIVE FOUNDATION. This pool is NOT yet wired into the staged reducer
 * or any consensus path (connect_block / *_stage.c). It exists so the
 * verify_queue (validation/verify_queue.h) can fan a self-contained batch
 * of jobs across (nproc-1) workers and AND-reduce the verdicts.
 *
 * Shape (mirrors vh_pool in app/jobs/src/validate_headers_stage.c):
 *   - workers spawned ONCE at thread_pool_start(), not per batch;
 *   - a single in-flight batch handed off under a mutex; workers pull the
 *     next index off a shared cursor (greedy work-stealing, balances
 *     uneven per-job cost) and write their verdict into a PER-JOB slot —
 *     no shared lock-free counter for results;
 *   - thread_pool_run_batch() blocks the submitter until n_done == n_jobs;
 *   - thread_pool_stop() sets stop, broadcasts, joins every worker (no leak).
 *
 * Sizing: thread_pool_start(workers). Pass 0 to default to GetNumCores()-1
 * (clamped to >= 1). workers == 1 means "no worker threads" — the caller
 * (verify_queue) runs the batch inline on the submitting thread, which is
 * the -par=1 serial path.
 *
 * Concurrency contract: a single submitter at a time. The verify_queue
 * serializes submit_batch() calls; do not call thread_pool_run_batch()
 * concurrently from two threads against the same pool.
 */

#ifndef ZCL_VALIDATION_THREAD_POOL_H
#define ZCL_VALIDATION_THREAD_POOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/* One unit of work. `fn(arg)` must be a pure, self-contained verifier that
 * returns true on pass / false on fail and touches only `arg` (no shared
 * mutable state) so the result is independent of worker scheduling. The
 * pool writes the verdict into `*result_out` (a caller-owned per-job slot)
 * — never a shared counter. */
struct thread_pool_task {
    bool (*fn)(void *arg);
    void *arg;
    bool *result_out;
};

/* Opaque-ish pool handle. Fields are private; do not poke them. */
struct thread_pool {
    pthread_t       *threads;       /* [n_workers], heap (NULL if n_workers==0) */
    int              n_workers;     /* worker threads actually spawned */
    bool            *thread_started;/* [n_workers] */

    /* Current batch — pointers stable for the duration of one run_batch. */
    const struct thread_pool_task *tasks;
    int              n_tasks;
    int              next_to_take;  /* shared cursor (greedy steal) */
    int              n_done;

    bool             stop;
    bool             inited;

    pthread_mutex_t  mu;
    pthread_cond_t   cv_take;       /* workers wait here for a batch */
    pthread_cond_t   cv_done;       /* submitter waits here for completion */
};

/* Spawn the worker pool ONCE. `workers` <= 0 defaults to GetNumCores()-1
 * (clamped to >= 1). `workers == 1` spawns ZERO worker threads (serial /
 * inline mode); run_batch then executes tasks on the calling thread.
 * Returns true on success. On a spawn failure every already-started worker
 * is joined and the pool is left un-inited (false). */
bool thread_pool_start(struct thread_pool *p, int workers);

/* Number of worker threads spawned (0 in serial mode). */
int thread_pool_worker_count(const struct thread_pool *p);

/* Run one batch and block until every task's verdict is written into its
 * result_out slot. Safe to call repeatedly; tasks[] / args must outlive
 * the call. With zero worker threads (serial mode) the tasks run inline on
 * the caller. Returns false (and writes nothing) only on a NULL-arg guard. */
bool thread_pool_run_batch(struct thread_pool *p,
                           const struct thread_pool_task *tasks, int n);

/* Stop + join every worker; idempotent; no leak. After this the pool may
 * be re-started. */
void thread_pool_stop(struct thread_pool *p);

#endif /* ZCL_VALIDATION_THREAD_POOL_H */
