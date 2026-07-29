// one-result-type-ok:body-backfill-pass-returns-a-count — the single exported
// function returns the NUMBER of bodies it handed to the download manager,
// which is a measurement, not a status. It has no failure return to carry a
// reason in: every path that could not do its job publishes the fail-closed
// BODY_HISTORY_UNKNOWN verdict (storage/body_history.h) and logs, which is a
// louder and longer-lived signal than a zcl_result the caller would discard.
// The fallible surfaces it calls (body_history_census_fold, body_history_save)
// route through LOG_FAIL in lib/storage/src/body_history.c.
//
// repair-rung-ok:test_bh_at_tip_requires_proven_history — this is a backfill
// rung and there is no writer to fix instead. The missing bodies were never
// written by anything: a checkpoint- or snapshot-seeded datadir legitimately
// starts above them, so refetching from peers is the only way they can ever
// exist locally. What WAS a producer defect is the false claim built on top
// of that hole — syncsvc_plan_periodic_tip_state used to publish AT_TIP for a
// node missing 98% of its own chain's bodies. That producer now refuses to
// emit the claim unless coverage is positively COMPLETE, and the cited test
// pins it for a known hole AND for unmeasured coverage.

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_backfill_service — the below-tip half of block-body acquisition.
 *
 * gap_fill_service walks [tip+1, best_header]: strictly ABOVE the tip, by
 * construction. Nothing in the node ever asked for a height below its own
 * tip, which is how a node holding genesis plus the last few thousand
 * blocks — and missing the bodies for 98% of the chain — could sit idle and
 * report itself at tip. This file is the other direction.
 *
 * It is a DRIVER, not a second downloader. The census algebra is
 * storage/body_history.h, the have-data record is body_coverage's one
 * global map, and the download primitives are dl_queue_blocks /
 * dl_is_in_flight — the same ones gap_fill uses. It runs on gap_fill's
 * worker thread (see gap_fill_thread_main) so there is no second cadence,
 * no second supervisor child, and no second cursor discipline.
 */

#include "services/body_backfill_service.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "net/download.h"
#include "storage/body_coverage.h"
#include "storage/body_history.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/sync.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Locking: cs_main is held only across the probe loop (array indexing and
 * flag reads, no allocation, no download-manager call). The coverage locks
 * are taken after it is released, and every dl_* call happens outside both.
 */

/* Do not hand below-tip work to the download manager while the queue is
 * already deep. The queue is height-sorted, so a below-tip height sorts
 * AHEAD of every tip-chasing block; an ungated backfill would push live
 * sync to the back of its own queue. */
#define BODY_HISTORY_QUEUE_HEADROOM 256

struct bb_probe_ctx {
    struct main_state *ms; /* cs_main is held by the caller for the walk */
};

/* Probe one height against the in-memory active chain — the same authority
 * `zclassic23 dumpstate block_index <h>` reports, and the only one that
 * distinguishes a body on disk from a header row.
 *
 * Anything that is not a positive read returns INDETERMINATE. A NULL slot,
 * a height/index disagreement, or a missing block hash is "I could not
 * look", and body_history_census_fold leaves those heights unmeasured. It
 * must never fall through to MISSING (which would invent holes) or to HAVE
 * (which would invent coverage). */
static enum body_history_probe bb_probe(int64_t height,
                                        struct uint256 *out_hash,
                                        void *ctx)
{
    struct bb_probe_ctx *pc = (struct bb_probe_ctx *)ctx;
    if (!pc || !pc->ms || height < 0 || height > INT32_MAX)
        return BODY_HISTORY_PROBE_INDETERMINATE;

    struct block_index *bi =
        active_chain_at(&pc->ms->chain_active, (int)height);
    if (!bi || bi->nHeight != (int)height || !bi->phashBlock)
        return BODY_HISTORY_PROBE_INDETERMINATE;

    if (bi->nStatus & BLOCK_HAVE_DATA)
        return BODY_HISTORY_PROBE_HAVE;

    if (out_hash)
        *out_hash = *bi->phashBlock; /* value copy under cs_main */
    return BODY_HISTORY_PROBE_MISSING;
}

/* One bounded census pass over [0, tip], plus a rate-limited enqueue of the
 * missing bodies it found. `tip_work_pending` is true when the above-tip
 * pass still has work; the census still RUNS (the report must stay fresh)
 * but the backfill holds off so live sync keeps the queue.
 *
 * Returns the number of below-tip bodies handed to the download manager. */
int body_backfill_pass(struct main_state *ms, struct download_manager *dm,
                       bool tip_work_pending,
                       body_backfill_wake_fn wake, void *wake_ctx)
{
    if (!ms || !dm) {
        body_history_publish(NULL); /* not wired == not measured */
        return 0;
    }

    zcl_mutex_lock(&ms->cs_main);
    int tip_h = active_chain_height(&ms->chain_active);
    zcl_mutex_unlock(&ms->cs_main);
    if (tip_h < 0) {
        body_history_publish(NULL);
        return 0;
    }

    int64_t lo = 0, hi = 0;
    body_history_global_lock();
    bool planned = body_history_census_plan(body_history_global_census(),
                                            0, (int64_t)tip_h,
                                            BODY_HISTORY_CENSUS_BUDGET,
                                            &lo, &hi);
    body_history_global_unlock();
    if (!planned) {
        body_history_publish(NULL);
        return 0;
    }

    size_t cap = (size_t)(hi - lo + 1);
    uint8_t *classes = zcl_malloc(cap, "body_history_classes");
    struct uint256 *hashes =
        zcl_malloc(cap * sizeof(*hashes), "body_history_hashes");
    if (!classes || !hashes) {
        free(classes);
        free(hashes);
        /* Out of memory is "could not look", not "nothing missing". */
        body_history_publish(NULL);
        LOG_WARN("body_backfill",
                 "[body-history] census pass [%lld..%lld] alloc failed — "
                 "coverage reported unknown",
                 (long long)lo, (long long)hi);
        return 0;
    }

    struct bb_probe_ctx pc = { .ms = ms };
    zcl_mutex_lock(&ms->cs_main);
    size_t n = body_history_census_probe_window(lo, hi, bb_probe,
                                                &pc, classes, hashes, cap);
    zcl_mutex_unlock(&ms->cs_main);

    struct body_history_pass_result res;
    struct body_history_verdict verdict;
    memset(&res, 0, sizeof(res));
    body_history_global_lock();
    struct body_history_census *census = body_history_global_census();
    bool folded = body_history_census_fold(census,
                                           body_coverage_global_map(),
                                           body_history_global_measured(),
                                           lo, classes, n, &res);
    if (folded)
        body_history_census_advance(census, lo, hi);
    bool evaluated = body_history_evaluate(body_coverage_global_map(),
                                           body_history_global_measured(),
                                           0, (int64_t)tip_h, &verdict);
    body_history_global_unlock();

    /* Publish outside the bracket (body_history_publish takes the lock).
     * A fold or evaluate that failed publishes UNKNOWN, never silence. */
    body_history_publish(evaluated ? &verdict : NULL);

    int enqueued = 0;
    if (folded && res.missing > 0 && !tip_work_pending) {
        uint64_t in_flight = 0, queued = 0;
        dl_get_stats(dm, NULL, NULL, NULL, &in_flight, &queued);
        if (queued <= BODY_HISTORY_QUEUE_HEADROOM &&
            in_flight < dl_get_max_in_flight_total() / 4) {
            struct uint256 *eh = zcl_malloc(
                BODY_HISTORY_ENQUEUE_MAX * sizeof(*eh), "body_history_enq_h");
            int32_t *ehh = zcl_malloc(
                BODY_HISTORY_ENQUEUE_MAX * sizeof(*ehh), "body_history_enq_n");
            if (eh && ehh) {
                size_t want = body_history_census_collect_missing(
                    lo, classes, hashes, n, eh, ehh,
                    BODY_HISTORY_ENQUEUE_MAX);
                /* Drop anything already in flight so a slow peer's
                 * outstanding request is not duplicated. */
                size_t keep = 0;
                for (size_t i = 0; i < want; i++) {
                    if (dl_is_in_flight(dm, &eh[i]))
                        continue;
                    eh[keep] = eh[i];
                    ehh[keep] = ehh[i];
                    keep++;
                }
                if (keep > 0) {
                    size_t added = dl_queue_blocks(dm, eh, ehh, keep);
                    enqueued = (int)added;
                    if (added > 0) {
                        body_history_global_lock();
                        body_history_global_census()->blocks_enqueued +=
                            (uint64_t)added;
                        body_history_global_unlock();
                        LOG_WARN("body_backfill",
                                 "[body-history] backfill queued %zu below-tip "
                                 "bodies (window [%lld..%lld] tip=%d)",
                                 added, (long long)lo, (long long)hi, tip_h);
                        event_emitf(EV_BLOCK_REQUESTED, 0,
                                    "body_history backfill queued=%zu lo=%lld "
                                    "hi=%lld tip=%d",
                                    added, (long long)lo, (long long)hi, tip_h);
                        if (wake)
                            wake(wake_ctx);
                    }
                }
            } else {
                LOG_WARN("body_backfill",
                         "[body-history] backfill alloc failed — census "
                         "verdict still published");
            }
            free(eh);
            free(ehh);
        }
    }

    free(classes);
    free(hashes);
    return enqueued;
}

