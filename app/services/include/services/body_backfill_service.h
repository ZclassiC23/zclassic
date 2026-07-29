/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_backfill_service — census and refetch of block bodies BELOW the tip.
 *
 * gap_fill_service's window is [tip+1, best_header] by construction, so the
 * node never asked for a height below its own tip. A node holding genesis
 * plus a recent tail therefore went idle, called itself at tip, and never
 * mentioned the millions of block bodies it did not have. This is the other
 * direction: measure the hole, say so, and drip-fill it through the
 * download machinery that already exists.
 *
 * One entry point, driven from gap_fill's existing worker loop. There is no
 * second thread, no second cadence, no second supervisor child and no
 * second downloader.
 */

#ifndef ZCL_BODY_BACKFILL_SERVICE_H
#define ZCL_BODY_BACKFILL_SERVICE_H

#include <stdbool.h>

struct main_state;
struct download_manager;

/* Wake the network dispatcher after new work is queued. Same hook shape
 * gap_fill uses; NULL is legal (the next tick picks the work up). */
typedef void (*body_backfill_wake_fn)(void *ctx);

/* Run ONE bounded census pass over [0, tip] and, if the pipeline has room,
 * hand up to BODY_HISTORY_ENQUEUE_MAX missing bodies to the download
 * manager. Returns the number of bodies enqueued.
 *
 * The census runs on EVERY call so the published coverage verdict never
 * goes stale. Two separate brakes hold back the ENQUEUE half:
 *
 *   tip_work_pending  the above-tip pass still has work, so live sync keeps
 *                     the (height-sorted) download queue to itself.
 *   census_only       this call is part of the boot catch-up burst, which
 *                     runs slices back-to-back. A burst is a measurement;
 *                     passing true makes "it cannot become a download surge"
 *                     true by construction rather than by arithmetic over
 *                     the queue-headroom gate.
 *
 * Every exit path publishes a verdict — a pass that could not measure
 * publishes BODY_HISTORY_UNKNOWN rather than staying silent, because
 * silence is what a caller would read as "fine". */
int body_backfill_pass(struct main_state *ms, struct download_manager *dm,
                       bool tip_work_pending, bool census_only,
                       body_backfill_wake_fn wake, void *wake_ctx);

/* Asked between catch-up slices; true means "shut down now". NULL is legal
 * (the burst then ends only on its own bounds). */
typedef bool (*body_backfill_abort_fn)(void *ctx);

/* Boot catch-up burst: run census-only slices back-to-back until every
 * height in the window has been looked at once, then return and let the
 * caller go back to its slow tick.
 *
 * Why it exists: one slice per GAPFILL_TICK_SECS takes ~65 minutes to walk a
 * 3.2M-height window, and until the walk finishes the node can only publish
 * "could not determine" — so it would refuse to say at tip for an hour after
 * every restart. That hour was a CADENCE artifact, not a cost: the whole
 * sweep is tens of milliseconds of CPU (measured; see
 * BODY_HISTORY_CENSUS_BUDGET).
 *
 * It cannot be why block download waits. cs_main is still taken and released
 * per slice (~0.08 ms each, measured); the burst is capped at
 * BODY_HISTORY_CENSUS_BURST_MS, 5% of one tick; the caller's above-tip pass
 * is scheduled first; and every slice is census-only, so a burst measures
 * and never queues. Returns the number of slices it ran. */
int body_backfill_catch_up(struct main_state *ms, struct download_manager *dm,
                           bool tip_work_pending,
                           body_backfill_abort_fn should_abort, void *abort_ctx,
                           body_backfill_wake_fn heartbeat, void *hb_ctx);

#endif /* ZCL_BODY_BACKFILL_SERVICE_H */
