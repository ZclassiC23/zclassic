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

#include <stddef.h>

bool verify_queue_submit_batch(struct thread_pool *pool,
                               struct verify_job *jobs, int n)
{
    if (n < 0)
        LOG_FAIL("verify_queue", "negative job count %d", n);
    if (n > 0 && !jobs)
        LOG_FAIL("verify_queue", "NULL jobs with n=%d", n);
    if (n == 0)
        return true;   /* vacuous: empty batch passes */

    /* Serial path: no pool, or a pool with zero worker threads (-par=1).
     * Run every job inline on the caller; this is the reference order the
     * parallel path must match bit-for-bit. */
    bool serial = (pool == NULL) || (thread_pool_worker_count(pool) == 0);

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
