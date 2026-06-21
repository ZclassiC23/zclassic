/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * gap_fill_service — sequential block-gap filler for IBD and catch-up.
 *
 * Problem: when a peer announces a tip far ahead of ours (gap > 512),
 * reducer activation defers and queues only the one far block via
 * `dl_queue_priority`. The 2,500 intermediate blocks `[tip+1, far-1]`
 * are not requested by that path — they depend on the header-reception path
 * in `msg_headers.c` queueing them, which only fires when new
 * headers arrive. If headers have already arrived and the chain still
 * has missing block data, the gap never closes.
 *
 * Solution: a background pthread that, while `tip_h < best_header_h`:
 *   1. Walks pprev from `pindex_best_header` down to `tip+1`
 *      (capped at GAPFILL_WINDOW) collecting candidate block_index ptrs
 *   2. For each that lacks `BLOCK_HAVE_DATA` and is not already
 *      in-flight, batches the hash/height pair
 *   3. Calls `dl_queue_blocks` once per pass
 *   4. Sleeps GAPFILL_TICK_SECS or until woken by `gap_fill_kick`
 *
 * Crash-safe: takes `ms->cs_main` briefly per pass; never holds the
 * lock across a download-manager mutation. Walks use the same
 * monotonicity + step-cap pattern as `pprev_walk_safe` (canonical
 * helper landing in Part 2). */

#ifndef ZCL_GAP_FILL_SERVICE_H
#define ZCL_GAP_FILL_SERVICE_H

#include "util/result.h"
#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct download_manager;

#define GAPFILL_TICK_SECS    5       /* periodic refill cadence */
#define GAPFILL_WINDOW       65536   /* max blocks per pass — needs to be
                                      * large enough to cover any plausible
                                      * mining-rate gap. 65k = ~6 weeks of
                                      * ZCL blocks (~75s/block); single
                                      * pass can reach tip+1 even after a
                                      * long disconnection. */
#define GAPFILL_WALK_CAP     131072  /* hard pprev step cap, 2x window for
                                      * defense against pprev cycles */
#define GAPFILL_PRIORITY_BOTTOM_N 16 /* front-insert the lowest N missing
                                      * blocks of the window (the
                                      * connectable bottom: tip+1..) so the
                                      * tip-advancing body can never be
                                      * tail-starved behind far-ahead live
                                      * blocks. See gap_fill_pass(). */

struct gap_fill_stats {
    uint64_t passes;
    uint64_t blocks_enqueued;
    uint64_t passes_idle;
    uint64_t passes_corrupt_walk;
    int      last_tip_h;
    int      last_best_h;
    int      last_window_lo;
    int      last_window_hi;
};

/* Start the service. Spawns one pthread. ms and dm must outlive the
 * service. Returns ZCL_OK on success (including the already-running
 * no-op); a non-ok result on bad args or spawn failure. Idempotent —
 * safe to call twice. */
struct zcl_result gap_fill_start(struct main_state *ms, struct download_manager *dm);

/* Request shutdown and join the thread. Safe if never started. */
void gap_fill_stop(void);

/* Wake the service immediately (instead of waiting for the next tick).
 * Called from reducer activation when a far-ahead block is deferred and from
 * header-reception when new headers expand pindex_best_header. No-op if
 * service not running. */
void gap_fill_kick(void);

#endif /* ZCL_GAP_FILL_SERVICE_H */
