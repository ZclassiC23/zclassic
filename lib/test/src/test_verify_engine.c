/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Determinism tests for the parallel verification engine
 * (lib/validation/src/thread_pool.c + verify_queue.c).
 *
 * The engine is an ADDITIVE foundation — not wired into the staged reducer
 * or any consensus path. These tests pin its CONTRACT:
 *
 *   1. A mixed batch of jobs (all three kinds, KNOWN per-index verdicts) runs
 *      to identical per-job verdicts AND identical batch verdict whether run
 *      (a) serially (no pool) and (b) fanned across a multi-worker pool.
 *   2. -par=1 takes the serial path: thread_pool_start(.,1) spawns ZERO worker
 *      threads, and a batch run on it produces the same verdicts as the
 *      reference serial run.
 *   3. The default sizing (-par=0 => GetNumCores()-1, clamped >= 1) spawns the
 *      expected worker count.
 *   4. An all-pass batch reduces to true; any single fail reduces to false.
 *   5. Empty batch is vacuously true.
 *
 * Determinism is by construction: each job's verifier is a pure function of
 * its self-contained payload (a job index), with the verdict derived from the
 * index alone — no rand, no clock — so result is independent of which worker
 * picks up which index.
 */

#include "test/test_helpers.h"

#include "validation/verify_queue.h"
#include "validation/thread_pool.h"
#include "util/util.h"   /* GetNumCores */

#include <stdio.h>
#include <string.h>

#define VE_CHECK(name, expr) do {                       \
    printf("verify_engine: %s... ", (name));            \
    if (expr) printf("OK\n");                            \
    else { printf("FAIL\n"); failures++; }              \
} while (0)

/* Self-contained payload: an index. The KNOWN verdict is a pure function of
 * that index and the job kind, so it is identical on any worker. */
struct ve_payload {
    int index;
    enum verify_job_kind kind;
};

/* Deterministic expected verdict: PASS iff (index % 7) != 0. Crafted so a
 * batch of, say, 50 jobs has a deterministic mix of passes and a handful of
 * fails at indices 0,7,14,... (no rand, no clock). */
static bool ve_expected(int index)
{
    return (index % 7) != 0;
}

/* The pure verifier the engine runs. Touches only its own arg. We make the
 * three kinds compute the verdict by genuinely different (but still pure)
 * arithmetic to exercise the tagged dispatch, then assert all three agree
 * with ve_expected(). */
static bool ve_verify(void *arg)
{
    const struct ve_payload *p = arg;
    switch (p->kind) {
    case VERIFY_JOB_SCRIPT_CHECK:
        /* fail when divisible by 7 */
        return (p->index % 7) != 0;
    case VERIFY_JOB_GROTH16_PROOF:
        /* same predicate, expressed differently */
        return p->index - (p->index / 7) * 7 != 0;
    case VERIFY_JOB_EQUIHASH_POW: {
        int r = p->index;
        while (r >= 7) r -= 7;
        return r != 0;
    }
    }
    return false;
}

/* Build N jobs cycling through the three kinds by index. Verdicts known. */
static void ve_build(struct verify_job *jobs, struct ve_payload *pay, int n)
{
    static const enum verify_job_kind kinds[3] = {
        VERIFY_JOB_SCRIPT_CHECK,
        VERIFY_JOB_GROTH16_PROOF,
        VERIFY_JOB_EQUIHASH_POW,
    };
    for (int i = 0; i < n; i++) {
        pay[i].index = i;
        pay[i].kind = kinds[i % 3];
        jobs[i].kind = pay[i].kind;
        jobs[i].fn = ve_verify;
        jobs[i].arg = &pay[i];
        jobs[i].result = false;
    }
}

int test_verify_engine(void)
{
    printf("\n=== parallel verify engine (thread_pool + verify_queue) ===\n");
    int failures = 0;

    /* ── 1. Known-verdict batch, several sizes, serial vs parallel ─────── */
    const int sizes[] = { 1, 4, 5, 17, 50, 64 };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int n = sizes[s];
        char label[64];

        struct verify_job sj[64], pj[64];
        struct ve_payload sp[64], pp[64];
        ve_build(sj, sp, n);
        ve_build(pj, pp, n);

        /* (a) serial reference: NULL pool forces inline execution. */
        bool serial_batch = verify_queue_submit_batch(NULL, sj, n);

        /* Expected batch verdict from the known per-index predicate. */
        bool exp_batch = true;
        for (int i = 0; i < n; i++)
            exp_batch = exp_batch && ve_expected(i);

        snprintf(label, sizeof(label),
                 "n=%d serial batch matches expected", n);
        VE_CHECK(label, serial_batch == exp_batch);

        snprintf(label, sizeof(label),
                 "n=%d serial per-job verdicts match expected", n);
        bool serial_perjob_ok = true;
        for (int i = 0; i < n; i++)
            if (sj[i].result != ve_expected(i)) serial_perjob_ok = false;
        VE_CHECK(label, serial_perjob_ok);

        /* (b) parallel: 4 worker threads (force >1 so the pool actually
         * fans out regardless of host core count). */
        struct thread_pool pool;
        bool started = thread_pool_start(&pool, 4);
        snprintf(label, sizeof(label), "n=%d pool start (4 workers)", n);
        VE_CHECK(label, started && thread_pool_worker_count(&pool) == 4);

        bool par_batch = verify_queue_submit_batch(&pool, pj, n);

        snprintf(label, sizeof(label),
                 "n=%d parallel batch verdict == serial", n);
        VE_CHECK(label, par_batch == serial_batch);

        snprintf(label, sizeof(label),
                 "n=%d parallel per-job verdicts == serial per-job", n);
        bool perjob_match = true;
        for (int i = 0; i < n; i++)
            if (pj[i].result != sj[i].result) perjob_match = false;
        VE_CHECK(label, perjob_match);

        thread_pool_stop(&pool);
    }

    /* ── 2. Re-running the SAME pool on a second batch is stable ───────── */
    {
        struct thread_pool pool;
        bool started = thread_pool_start(&pool, 4);
        VE_CHECK("reuse: pool start", started);

        int n = 33;
        struct verify_job a[33], b[33];
        struct ve_payload pa[33], pb[33];
        ve_build(a, pa, n);
        ve_build(b, pb, n);
        bool r1 = verify_queue_submit_batch(&pool, a, n);
        bool r2 = verify_queue_submit_batch(&pool, b, n);
        bool same = (r1 == r2);
        for (int i = 0; i < n; i++)
            if (a[i].result != b[i].result) same = false;
        VE_CHECK("reuse: two batches on one pool give identical verdicts", same);
        thread_pool_stop(&pool);
    }

    /* ── 3. -par=1 is the serial path: ZERO worker threads ─────────────── */
    {
        struct thread_pool serial_pool;
        bool started = thread_pool_start(&serial_pool, 1);
        VE_CHECK("-par=1 starts with zero worker threads",
                 started && thread_pool_worker_count(&serial_pool) == 0);

        int n = 21;
        struct verify_job jobs[21];
        struct ve_payload pay[21];
        ve_build(jobs, pay, n);
        bool batch = verify_queue_submit_batch(&serial_pool, jobs, n);

        bool exp_batch = true, perjob_ok = true;
        for (int i = 0; i < n; i++) {
            exp_batch = exp_batch && ve_expected(i);
            if (jobs[i].result != ve_expected(i)) perjob_ok = false;
        }
        VE_CHECK("-par=1 serial-pool batch matches expected", batch == exp_batch);
        VE_CHECK("-par=1 serial-pool per-job verdicts match expected", perjob_ok);
        thread_pool_stop(&serial_pool);
    }

    /* ── 4. Default sizing: -par=0 => GetNumCores()-1, clamped >= 1 ─────── */
    {
        int cores = GetNumCores();
        int expect_workers = cores - 1;
        if (expect_workers < 1) expect_workers = 1;
        /* expect_workers==1 means serial mode => 0 spawned threads. */
        int expect_spawned = (expect_workers == 1) ? 0 : expect_workers;

        struct thread_pool pool;
        bool started = thread_pool_start(&pool, 0);
        VE_CHECK("-par=0 default sizing spawns GetNumCores()-1 workers",
                 started && thread_pool_worker_count(&pool) == expect_spawned);
        thread_pool_stop(&pool);
    }

    /* ── 5. All-pass reduces true; one fail reduces false; empty vacuous ── */
    {
        struct thread_pool pool;
        thread_pool_start(&pool, 4);

        /* indices 1,2,3,4,5,6 are all PASS (none divisible by 7). */
        int n = 6;
        struct verify_job jobs[6];
        struct ve_payload pay[6];
        for (int i = 0; i < n; i++) {
            pay[i].index = i + 1;
            pay[i].kind = VERIFY_JOB_SCRIPT_CHECK;
            jobs[i].kind = VERIFY_JOB_SCRIPT_CHECK;
            jobs[i].fn = ve_verify;
            jobs[i].arg = &pay[i];
            jobs[i].result = false;
        }
        VE_CHECK("all-pass batch reduces to true (parallel)",
                 verify_queue_submit_batch(&pool, jobs, n) == true);

        /* Flip index 3 to a known FAIL (divisible by 7 => 7). */
        pay[2].index = 7;
        VE_CHECK("one fail reduces batch to false (parallel)",
                 verify_queue_submit_batch(&pool, jobs, n) == false);

        /* Same on the serial path. */
        VE_CHECK("one fail reduces batch to false (serial)",
                 verify_queue_submit_batch(NULL, jobs, n) == false);

        VE_CHECK("empty batch is vacuously true (parallel)",
                 verify_queue_submit_batch(&pool, jobs, 0) == true);
        VE_CHECK("empty batch is vacuously true (serial)",
                 verify_queue_submit_batch(NULL, jobs, 0) == true);
        thread_pool_stop(&pool);
    }

    /* ── 6. NULL / negative-n guards return false, do not crash ────────── */
    {
        struct verify_job one = { .kind = VERIFY_JOB_SCRIPT_CHECK,
                                  .fn = ve_verify, .arg = NULL,
                                  .result = false };
        (void)one;
        VE_CHECK("NULL jobs with n>0 -> false",
                 verify_queue_submit_batch(NULL, NULL, 3) == false);
        VE_CHECK("negative n -> false",
                 verify_queue_submit_batch(NULL, NULL, -1) == false);
    }

    return failures;
}
