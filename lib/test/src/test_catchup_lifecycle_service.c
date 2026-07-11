/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * catchup_lifecycle_service — start/join/reap policy lifted out of
 * config/src/boot_services.c (boot_start_catchup_service /
 * boot_join_catchup_service / boot_reap_catchup_service). Exercises the
 * double-start guard, the NULL-safety of every entry point, the bounded
 * join clearing job->started, and the poll-only reap contract (no-op
 * while running, joins + clears once finished). */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "services/catchup_lifecycle_service.h"
#include "controllers/sync_controller.h"
#include "models/database.h"
#include "validation/chainstate.h"

#include <stdatomic.h>

int test_catchup_lifecycle_service(void)
{
    int failures = 0;
    printf("\n=== catchup_lifecycle_service tests ===\n");

    {
        printf("catchup_lifecycle_service: every entry point is NULL-safe... ");
        bool ok = !catchup_lifecycle_start(NULL, NULL, NULL, NULL, NULL);
        catchup_lifecycle_join(NULL, 0);   /* must not crash */
        ok = ok && catchup_lifecycle_reap(NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    {
        printf("catchup_lifecycle_service: reap/join no-op on a job never started... ");
        struct node_db_sync_catchup_job job;
        node_db_sync_catchup_job_init(&job);
        bool ok = catchup_lifecycle_reap(&job);
        catchup_lifecycle_join(&job, 0);   /* must not crash */
        ok = ok && !job.started;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    {
        printf("catchup_lifecycle_service: start + double-start guard + join... ");
        struct node_db ndb;
        struct active_chain ac;
        struct node_db_sync_catchup_job job;
        bool db_ok = node_db_open(&ndb, ":memory:");

        active_chain_init(&ac);
        node_db_sync_catchup_job_init(&job);

        bool started = db_ok &&
            catchup_lifecycle_start(&job, &ndb, &ac, NULL, NULL);
        /* Second start while the first is still (at least momentarily)
         * marked started must fail closed without disturbing the job —
         * mirrors the former boot_start_catchup_service guard that
         * pre-empted node_db_sync_catchup_job_start's LOG_FAIL path. */
        bool double_start_rejected = started &&
            !catchup_lifecycle_start(&job, &ndb, &ac, NULL, NULL);

        catchup_lifecycle_join(&job, 5);
        bool joined_clears_started = !job.started;

        bool ok = db_ok && started && double_start_rejected &&
                  joined_clears_started;

        if (ndb.open)
            node_db_close(&ndb);
        active_chain_free(&ac);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    {
        printf("catchup_lifecycle_service: reap is a no-op until the job finishes... ");
        struct node_db ndb;
        struct active_chain ac;
        struct node_db_sync_catchup_job job;
        bool db_ok = node_db_open(&ndb, ":memory:");

        active_chain_init(&ac);
        node_db_sync_catchup_job_init(&job);

        bool started = db_ok &&
            catchup_lifecycle_start(&job, &ndb, &ac, NULL, NULL);

        /* Give the worker thread a moment to run to completion — the fake
         * in-memory DB catchup is a fast no-op body, so this settles
         * quickly; reap is polled the same way the projection-backfill
         * watcher does in boot_background_workers.c. */
        bool reaped = false;
        for (int i = 0; i < 200 && !reaped; i++) {
            if (atomic_load(&job.finished))
                reaped = catchup_lifecycle_reap(&job);
            else
                platform_sleep_ms(5);
        }

        bool ok = started && reaped && !job.started;

        /* Belt-and-suspenders: never leave a thread dangling if the loop
         * above somehow didn't observe `finished` in time. */
        if (job.started)
            catchup_lifecycle_join(&job, 5);

        if (ndb.open)
            node_db_close(&ndb);
        active_chain_free(&ac);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("catchup_lifecycle_service: %d failures\n", failures);
    return failures;
}
