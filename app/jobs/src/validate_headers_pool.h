/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_pool — sibling-private worker-pool helper for the
 * validate_headers reducer stage. */

#ifndef ZCL_VALIDATE_HEADERS_POOL_H
#define ZCL_VALIDATE_HEADERS_POOL_H

#include "jobs/validate_headers_stage.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*vh_pool_job_fn)(void *job, void *user);

struct vh_pool {
    pthread_t       threads[VH_POOL_SIZE];
    bool            thread_started[VH_POOL_SIZE];

    unsigned char  *jobs;
    size_t          job_size;
    int             n_jobs;
    int             next_to_take;
    int             n_done;

    bool            stop;
    bool            inited;

    vh_pool_job_fn  run_job;
    void           *run_user;

    pthread_mutex_t mu;
    pthread_cond_t  cv_take;
    pthread_cond_t  cv_done;
};

bool vh_pool_start(struct vh_pool *pool, vh_pool_job_fn run_job,
                   void *run_user);
void vh_pool_run_batch(struct vh_pool *pool, void *jobs, size_t job_size,
                       int n_jobs);
void vh_pool_stop(struct vh_pool *pool);

#endif /* ZCL_VALIDATE_HEADERS_POOL_H */
