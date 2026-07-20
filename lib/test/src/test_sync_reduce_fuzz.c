/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure sync reducer — fuzz determinism (sync/sync_reduce.h). Step-0 contract
 * test: a seeded event sequence folded twice must produce byte-identical
 * states. WF1 lane 1C replaces this with the lib/sim/seed_tape.h harness that
 * prints the seed + minimal trace on any invariant break. */

#include "test/test_helpers.h"
#include "sync/sync_reduce.h"
#include <string.h>

/* Tiny deterministic LCG — self-contained so the step-0 group has no external
 * dependency (lane 1C swaps in seed_tape). */
static uint64_t lcg(uint64_t *st)
{
    *st = *st * 6364136223846793005ULL + 1442695040888963407ULL;
    return *st >> 33;
}

static struct sync_kernel_state fold_sequence(uint64_t seed, int steps)
{
    uint64_t st = seed;
    struct sync_kernel_state s;
    memset(&s, 0, sizeof(s));
    s.session_id = 1;
    s.phase = SYNC_PHASE_IDLE;
    for (int i = 0; i < steps; i++) {
        struct sync_event e;
        memset(&e, 0, sizeof(e));
        e.session_id = 1;
        e.kind = (enum sync_event_kind)(lcg(&st) % SYNC_EVENT_COUNT);
        struct sync_decision d = sync_reduce(s, e);
        s.phase = d.next;
    }
    return s;
}

static int test_sync_reduce_fuzz_deterministic(void)
{
    int failures = 0;
    TEST("sync_reduce: seeded event sequences fold deterministically") {
        for (uint64_t seed = 1; seed <= 64; seed++) {
            struct sync_kernel_state a = fold_sequence(seed, 200);
            struct sync_kernel_state b = fold_sequence(seed, 200);
            if (memcmp(&a, &b, sizeof(a)) != 0) {
                printf("FUZZ DIVERGENCE at seed=%llu\n",
                       (unsigned long long)seed);
                ASSERT(0);
            }
            ASSERT(a.phase >= 0 && a.phase < SYNC_PHASE_COUNT);
        }
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_reduce_fuzz(void)
{
    return test_sync_reduce_fuzz_deterministic();
}
