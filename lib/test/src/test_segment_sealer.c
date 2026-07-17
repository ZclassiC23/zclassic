/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the segment sealer's pure range selector — the logic that decides
 * WHICH 10k-aligned, fully-finalized, not-yet-sealed segment to seal next. The
 * threaded service + supervision are exercised at the chain_segment /
 * block_parse_cache level; here we prove the selection invariants directly.
 */

#include "test/test_helpers.h"

#include "services/segment_sealer_service.h"
#include "storage/chain_segment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SS_CHECK(name, expr) do {                                         \
    if (expr) { printf("  segment_sealer: %s... OK\n", (name)); }          \
    else { printf("  segment_sealer: %s... FAIL\n", (name)); failures++; } \
} while (0)

static bool tiny_body(void *user, uint32_t h, uint8_t **bytes, size_t *len)
{
    (void)user;
    size_t n = 8;
    uint8_t *b = malloc(n); // raw-alloc-ok:test
    if (!b) return false;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(h + i);
    *bytes = b; *len = n;
    return true;
}

int test_segment_sealer(void);
int test_segment_sealer(void)
{
    printf("\n=== segment_sealer (range selector) ===\n");
    int failures = 0;
    const uint32_t SEG = CHAIN_SEGMENT_BLOCKS_PER_SEG; /* 10000 */
    uint32_t first = 999, count = 999;

    /* Empty store: the first aligned segment fully below the frontier. */
    SS_CHECK("empty store, frontier deep -> segment 0",
             segment_sealer_next_range(SEG * 2 + 5, NULL, &first, &count) &&
             first == 0 && count == SEG);

    /* Frontier exactly at the top of segment 0 -> segment 0 is eligible. */
    first = count = 999;
    SS_CHECK("frontier == top of seg0 -> segment 0",
             segment_sealer_next_range(SEG - 1, NULL, &first, &count) &&
             first == 0 && count == SEG);

    /* Frontier one short of the top of segment 0 -> nothing to seal. */
    SS_CHECK("frontier below seg0 top -> none",
             !segment_sealer_next_range(SEG - 2, NULL, &first, &count));

    /* A store that already covers segment 0 -> next is segment 1. */
    {
        char err[256];
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "segment_sealer", "store");
        /* Seal a real, full 10k-aligned segment [0, SEG). */
        enum cseg_status st = chain_segment_seal_range(dir, tiny_body, NULL,
                                                       0, SEG, err, sizeof(err));
        SS_CHECK("seal segment 0 ok", st == CSEG_OK);

        struct chain_segment_store *store = NULL;
        st = chain_segment_store_open(dir, &store, err, sizeof(err));
        SS_CHECK("store open ok", st == CSEG_OK && store != NULL);

        first = count = 999;
        SS_CHECK("seg0 sealed -> next is segment 1",
                 segment_sealer_next_range(SEG * 3, store, &first, &count) &&
                 first == SEG && count == SEG);

        /* Frontier only reaches into segment 1 but not its top -> still none
         * beyond the already-sealed segment 0. */
        SS_CHECK("seg0 sealed, seg1 not finalized -> none",
                 !segment_sealer_next_range(SEG + 3, store, &first, &count));

        chain_segment_store_close(store);
        test_rm_rf_recursive(dir);
    }

    /* ── Bounded backfill catch-up + never-seal-above-frontier ─────────────
     * segment_sealer_seal_next is the primitive the background catch-up loops
     * (run_catchup calls it up to catchup_batch times per tick). Prove it seals
     * exactly ONE segment per call, oldest-first (so looping walks the backlog
     * forward), and NEVER writes a segment whose top exceeds the finalized
     * frontier. Driven with a synthetic body source — no node fixture needed. */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "segment_sealer", "sealnext");
        char err[256] = {0};
        /* Heights 0 .. 3*SEG-1 are finalized; the top of segment 2 == frontier. */
        const uint32_t frontier = SEG * 3 - 1;

        /* Call 1: seals the oldest unsealed segment (segment 0). */
        uint32_t f0 = 999;
        int r0 = segment_sealer_seal_next(dir, frontier, tiny_body, NULL,
                                          &f0, err, sizeof(err));
        SS_CHECK("seal_next #1 seals segment 0", r0 == 1 && f0 == 0);

        /* After call 1 exactly ONE segment is sealed (bounded: not two). */
        {
            struct chain_segment_store *st1 = NULL;
            enum cseg_status o1 = chain_segment_store_open(dir, &st1, err, sizeof(err));
            SS_CHECK("one segment sealed per call",
                     o1 == CSEG_OK && st1 &&
                     chain_segment_store_segment_count(st1) == 1 &&
                     !chain_segment_store_covers(st1, SEG));
            if (st1) chain_segment_store_close(st1);
        }

        /* Call 2: segment 0 sealed -> seals segment 1 (oldest-first advance). */
        uint32_t f1 = 999;
        int r1 = segment_sealer_seal_next(dir, frontier, tiny_body, NULL,
                                          &f1, err, sizeof(err));
        SS_CHECK("seal_next #2 seals segment 1", r1 == 1 && f1 == SEG);

        /* Call 3: seals segment 2 whose top == frontier (inclusive, eligible). */
        uint32_t f2 = 999;
        int r2 = segment_sealer_seal_next(dir, frontier, tiny_body, NULL,
                                          &f2, err, sizeof(err));
        SS_CHECK("seal_next #3 seals segment 2 (top == frontier)",
                 r2 == 1 && f2 == SEG * 2);

        /* Call 4: segment 3 is entirely ABOVE the frontier -> nothing sealed. */
        int r3 = segment_sealer_seal_next(dir, frontier, tiny_body, NULL,
                                          NULL, err, sizeof(err));
        SS_CHECK("seal_next #4 refuses to seal above frontier", r3 == 0);

        /* The store covers exactly [0, frontier]; nothing above was written. */
        struct chain_segment_store *store = NULL;
        enum cseg_status ost = chain_segment_store_open(dir, &store, err, sizeof(err));
        SS_CHECK("sealed_max == frontier, nothing above",
                 ost == CSEG_OK && store &&
                 chain_segment_store_covers(store, frontier) &&
                 !chain_segment_store_covers(store, frontier + 1) &&
                 chain_segment_store_sealed_max(store) == frontier);
        if (store) chain_segment_store_close(store);

        /* And no seg file for the above-frontier segment exists on disk. */
        char above[512];
        snprintf(above, sizeof(above), "%s/seg-%u-%u.dat", dir, SEG * 3, SEG);
        struct stat sb;
        SS_CHECK("no segment file above frontier on disk", stat(above, &sb) != 0);

        test_rm_rf_recursive(dir);
    }

    printf("segment_sealer: %d failures\n", failures);
    return failures;
}
