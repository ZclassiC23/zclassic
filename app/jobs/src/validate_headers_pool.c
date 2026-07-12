/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_pool — fixed worker pool used by validate_headers_stage.c. */

#include "validate_headers_pool.h"

#include "util/log_macros.h"
#include "util/thread_registry.h"

#include <stdio.h>
#include <string.h>

static void *vh_pool_worker_entry(void *arg)
{
    struct vh_pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mu);
        while (!pool->stop && pool->next_to_take >= pool->n_jobs)
            pthread_cond_wait(&pool->cv_take, &pool->mu);
        if (pool->stop) {
            pthread_mutex_unlock(&pool->mu);
            break;
        }
        int my = pool->next_to_take++;
        void *job = pool->jobs + (pool->job_size * (size_t)my);
        vh_pool_job_fn run_job = pool->run_job;
        void *run_user = pool->run_user;
        pthread_mutex_unlock(&pool->mu);

        run_job(job, run_user);

        pthread_mutex_lock(&pool->mu);
        pool->n_done++;
        if (pool->n_done >= pool->n_jobs)
            pthread_cond_broadcast(&pool->cv_done);
        pthread_mutex_unlock(&pool->mu);
    }
    thread_registry_unregister_self();
    return NULL;
}

bool vh_pool_start(struct vh_pool *pool, vh_pool_job_fn run_job,
                   void *run_user)
{
    if (!pool || !run_job)
        return false;

    memset(pool, 0, sizeof(*pool));
    pool->run_job = run_job;
    pool->run_user = run_user;
    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->cv_take, NULL);
    pthread_cond_init(&pool->cv_done, NULL);

    for (int i = 0; i < VH_POOL_SIZE; i++) {
        char name[32];
        snprintf(name, sizeof(name), "vh.worker.%d", i);
        int rc = thread_registry_spawn(name, vh_pool_worker_entry, pool,
                                          &pool->threads[i]);
        if (rc != 0) {
            LOG_WARN("validate_headers",
                     "[validate_headers] worker %d spawn failed: rc=%d",
                     i, rc);
            pthread_mutex_lock(&pool->mu);
            pool->stop = true;
            pthread_cond_broadcast(&pool->cv_take);
            pthread_mutex_unlock(&pool->mu);
            for (int j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);
            pthread_mutex_destroy(&pool->mu);
            pthread_cond_destroy(&pool->cv_take);
            pthread_cond_destroy(&pool->cv_done);
            memset(pool, 0, sizeof(*pool));
            return false;
        }
        pool->thread_started[i] = true;
    }
    pool->inited = true;
    return true;
}

void vh_pool_run_batch(struct vh_pool *pool, void *jobs, size_t job_size,
                       int n_jobs)
{
    if (!pool || !pool->inited || !jobs || job_size == 0 || n_jobs <= 0)
        return;

    pthread_mutex_lock(&pool->mu);
    pool->jobs = jobs;
    pool->job_size = job_size;
    pool->n_jobs = n_jobs;
    pool->next_to_take = 0;
    pool->n_done = 0;
    pthread_cond_broadcast(&pool->cv_take);
    while (pool->n_done < pool->n_jobs)
        pthread_cond_wait(&pool->cv_done, &pool->mu);
    pool->jobs = NULL;
    pool->job_size = 0;
    pool->n_jobs = 0;
    pthread_mutex_unlock(&pool->mu);
}

void vh_pool_stop(struct vh_pool *pool)
{
    if (!pool || !pool->inited)
        return;

    pthread_mutex_lock(&pool->mu);
    pool->stop = true;
    pthread_cond_broadcast(&pool->cv_take);
    pthread_mutex_unlock(&pool->mu);
    for (int i = 0; i < VH_POOL_SIZE; i++) {
        if (pool->thread_started[i]) {
            pthread_join(pool->threads[i], NULL);
            pool->thread_started[i] = false;
        }
    }
    pthread_mutex_destroy(&pool->mu);
    pthread_cond_destroy(&pool->cv_take);
    pthread_cond_destroy(&pool->cv_done);
    memset(pool, 0, sizeof(*pool));
}
