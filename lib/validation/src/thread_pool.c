/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/*
 * thread_pool — persistent worker pool for parallel verification.
 * See validation/thread_pool.h for the contract. ADDITIVE; not wired into
 * the staged reducer or any consensus path.
 *
 * Pool lifecycle:
 *   start  -> spawn n_workers ONCE (thread_registry_spawn_ex); workers loop
 *             waiting on cv_take.
 *   run    -> submitter publishes tasks under mu, broadcasts cv_take, then
 *             waits on cv_done until n_done == n_tasks. Each worker pulls the
 *             next index off next_to_take (greedy work-steal) and writes its
 *             verdict into tasks[i].result_out.
 *   stop   -> set stop, broadcast cv_take, join every started worker.
 */

#include "validation/thread_pool.h"

#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "util/thread_registry.h"
#include "util/util.h"   /* GetNumCores */

#include <stdio.h>
#include <string.h>

/* The worker entry receives its pool as the spawn arg (no global state). */
static void *thread_pool_worker_entry(void *arg)
{
    struct thread_pool *p = arg;

    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->stop && p->next_to_take >= p->n_tasks)
            pthread_cond_wait(&p->cv_take, &p->mu);
        if (p->stop) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        int my = p->next_to_take++;
        const struct thread_pool_task *task = &p->tasks[my];
        pthread_mutex_unlock(&p->mu);

        /* Run the verifier outside the lock — it is pure and self-contained
         * (touches only its own arg), so the verdict is independent of which
         * worker happened to take this index. */
        bool verdict = task->fn ? task->fn(task->arg) : false;
        if (task->result_out)
            *task->result_out = verdict;

        pthread_mutex_lock(&p->mu);
        p->n_done++;
        if (p->n_done >= p->n_tasks)
            pthread_cond_broadcast(&p->cv_done);
        pthread_mutex_unlock(&p->mu);
    }

    thread_registry_unregister_self();
    return NULL;
}

bool thread_pool_start(struct thread_pool *p, int workers)
{
    if (!p)
        LOG_FAIL("thread_pool", "NULL pool");

    memset(p, 0, sizeof(*p));

    if (workers <= 0) {
        int cores = GetNumCores();   /* clamped >= 1 by util.c */
        workers = cores - 1;
    }
    if (workers < 1)
        workers = 1;

    /* workers == 1 => serial mode: zero worker threads, run_batch inline. */
    int n_workers = (workers == 1) ? 0 : workers;

    p->n_workers = 0;
    p->tasks = NULL;
    p->n_tasks = 0;
    p->next_to_take = 0;
    p->n_done = 0;
    p->stop = false;

    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv_take, NULL);
    pthread_cond_init(&p->cv_done, NULL);
    p->inited = true;

    if (n_workers == 0)
        return true;   /* serial pool — nothing to spawn */

    p->threads = zcl_calloc((size_t)n_workers, sizeof(pthread_t),
                            "thread_pool threads");
    p->thread_started = zcl_calloc((size_t)n_workers, sizeof(bool),
                                   "thread_pool started");
    if (!p->threads || !p->thread_started) {
        free(p->threads);
        free(p->thread_started);
        pthread_mutex_destroy(&p->mu);
        pthread_cond_destroy(&p->cv_take);
        pthread_cond_destroy(&p->cv_done);
        p->inited = false;
        LOG_FAIL("thread_pool", "alloc failed for %d workers", n_workers);
    }

    for (int i = 0; i < n_workers; i++) {
        char name[32];
        snprintf(name, sizeof(name), "vq.worker.%d", i);
        int rc = thread_registry_spawn_ex(name, thread_pool_worker_entry, p,
                                           &p->threads[i]);
        if (rc != 0) {
            LOG_WARN("thread_pool", "worker %d spawn failed: rc=%d", i, rc);
            /* Tear down what we started. */
            pthread_mutex_lock(&p->mu);
            p->stop = true;
            pthread_cond_broadcast(&p->cv_take);
            pthread_mutex_unlock(&p->mu);
            for (int j = 0; j < i; j++)
                pthread_join(p->threads[j], NULL);
            free(p->threads);
            free(p->thread_started);
            p->threads = NULL;
            p->thread_started = NULL;
            pthread_mutex_destroy(&p->mu);
            pthread_cond_destroy(&p->cv_take);
            pthread_cond_destroy(&p->cv_done);
            p->inited = false;
            LOG_FAIL("thread_pool", "spawn aborted after %d/%d workers",
                     i, n_workers);
        }
        p->thread_started[i] = true;
        p->n_workers++;
    }

    return true;
}

int thread_pool_worker_count(const struct thread_pool *p)
{
    return p ? p->n_workers : 0;
}

bool thread_pool_run_batch(struct thread_pool *p,
                           const struct thread_pool_task *tasks, int n)
{
    if (!p || !p->inited)
        LOG_FAIL("thread_pool", "run_batch on un-inited pool");
    if (n < 0)
        LOG_FAIL("thread_pool", "negative task count %d", n);
    if (n > 0 && !tasks)
        LOG_FAIL("thread_pool", "NULL tasks with n=%d", n);
    if (n == 0)
        return true;

    /* Serial / inline path: zero worker threads. Run on the caller. This is
     * the -par=1 path; it never touches cv_take/cv_done. */
    if (p->n_workers == 0) {
        for (int i = 0; i < n; i++) {
            bool verdict = tasks[i].fn ? tasks[i].fn(tasks[i].arg) : false;
            if (tasks[i].result_out)
                *tasks[i].result_out = verdict;
        }
        return true;
    }

    /* Parallel path: publish the batch, wake workers, wait for completion. */
    pthread_mutex_lock(&p->mu);
    p->tasks        = tasks;
    p->n_tasks      = n;
    p->next_to_take = 0;
    p->n_done       = 0;
    pthread_cond_broadcast(&p->cv_take);
    while (p->n_done < p->n_tasks)
        pthread_cond_wait(&p->cv_done, &p->mu);
    p->tasks   = NULL;
    p->n_tasks = 0;
    pthread_mutex_unlock(&p->mu);
    return true;
}

void thread_pool_stop(struct thread_pool *p)
{
    if (!p || !p->inited)
        return;

    if (p->n_workers > 0) {
        pthread_mutex_lock(&p->mu);
        p->stop = true;
        pthread_cond_broadcast(&p->cv_take);
        pthread_mutex_unlock(&p->mu);
        for (int i = 0; i < p->n_workers; i++) {
            if (p->thread_started && p->thread_started[i]) {
                pthread_join(p->threads[i], NULL);
                p->thread_started[i] = false;
            }
        }
    }

    free(p->threads);
    free(p->thread_started);
    p->threads = NULL;
    p->thread_started = NULL;
    p->n_workers = 0;

    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv_take);
    pthread_cond_destroy(&p->cv_done);
    p->inited = false;
}
