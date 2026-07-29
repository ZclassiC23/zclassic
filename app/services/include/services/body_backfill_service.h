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
 * goes stale; `tip_work_pending` only holds back the enqueue so live sync
 * keeps the (height-sorted) download queue.
 *
 * Every exit path publishes a verdict — a pass that could not measure
 * publishes BODY_HISTORY_UNKNOWN rather than staying silent, because
 * silence is what a caller would read as "fine". */
int body_backfill_pass(struct main_state *ms, struct download_manager *dm,
                       bool tip_work_pending,
                       body_backfill_wake_fn wake, void *wake_ctx);

#endif /* ZCL_BODY_BACKFILL_SERVICE_H */
