/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE  /* pthread_timedjoin_np */

#include "platform/time_compat.h"
#include "services/gap_fill_service.h"

#include "supervisors/domains.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "net/download.h"
#include "jobs/body_fetch_stage.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"
#include "event/event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

struct gap_fill_state {
    pthread_t              thread;
    pthread_mutex_t        mu;
    pthread_cond_t         cv;
    _Atomic bool           running;
    _Atomic bool           stop_requested;
    bool                   thread_started;

    struct main_state      *ms;
    struct download_manager *dm;
    gap_fill_dispatch_wake_fn wake_dispatch;
    void                   *wake_dispatch_ctx;

    struct gap_fill_stats  stats;
    _Atomic supervisor_child_id supervisor_id;
};

static struct gap_fill_state g_gf = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .supervisor_id = SUPERVISOR_INVALID_ID,
};

static struct liveness_contract g_gap_fill_contract;

static int64_t gap_fill_supervisor_deadline_secs(void)
{
    return (int64_t)GAPFILL_TICK_SECS * 3 + 30;
}

static int64_t gap_fill_progress_marker(void)
{
    struct gap_fill_stats st;
    gap_fill_get_stats(&st);
    return (int64_t)st.passes;
}

static void gap_fill_supervisor_heartbeat(void)
{
    supervisor_child_id id = atomic_load(&g_gf.supervisor_id);
    if (id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_tick(id);
    supervisor_progress(id, gap_fill_progress_marker());
}

static void gap_fill_on_stall(struct liveness_contract *c)
{
    struct gap_fill_stats st;
    gap_fill_get_stats(&st);
    const char *reason = c
        ? supervisor_stall_reason_name(
              (enum supervisor_stall_reason)atomic_load(&c->stall_reason))
        : "unknown";
    LOG_WARN("gap_fill",
             "[gap-fill] supervisor stall reason=%s passes=%llu "
             "enqueued=%llu idle=%llu corrupt=%llu timeout_sweeps=%llu "
             "timeouts_requeued=%llu dispatch_wakes=%llu",
             reason,
             (unsigned long long)st.passes,
             (unsigned long long)st.blocks_enqueued,
             (unsigned long long)st.passes_idle,
             (unsigned long long)st.passes_corrupt_walk,
             (unsigned long long)st.timeout_sweeps,
             (unsigned long long)st.timeouts_requeued,
             (unsigned long long)st.dispatch_wakes);
    event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
                "source=chain.gap_fill decision=worker_stall "
                "reason=%s passes=%llu enqueued=%llu idle=%llu corrupt=%llu "
                "timeout_sweeps=%llu timeouts_requeued=%llu "
                "dispatch_wakes=%llu",
                reason,
                (unsigned long long)st.passes,
                (unsigned long long)st.blocks_enqueued,
                (unsigned long long)st.passes_idle,
                (unsigned long long)st.passes_corrupt_walk,
                (unsigned long long)st.timeout_sweeps,
                (unsigned long long)st.timeouts_requeued,
                (unsigned long long)st.dispatch_wakes);
    gap_fill_kick();
}

static struct zcl_result gap_fill_register_supervisor(void)
{
    if (!supervisor_start())
        return ZCL_ERR(-3, "gap_fill: supervisor_start failed");

    int64_t deadline = gap_fill_supervisor_deadline_secs();
    supervisor_child_id id = atomic_load(&g_gf.supervisor_id);
    if (id != SUPERVISOR_INVALID_ID) {
        supervisor_set_deadline(id, deadline);
        supervisor_progress(id, gap_fill_progress_marker());
        supervisor_tick(id);
        return ZCL_OK;
    }

    liveness_contract_init(&g_gap_fill_contract, "chain.gap_fill");
    atomic_store(&g_gap_fill_contract.period_secs, 0);
    atomic_store(&g_gap_fill_contract.deadline_secs, deadline);
    atomic_store(&g_gap_fill_contract.progress_max_quiet_us, 0);
    g_gap_fill_contract.on_stall = gap_fill_on_stall;

    supervisor_domains_init();
    id = supervisor_register_in_domain(g_chain_sup, &g_gap_fill_contract);
    if (id == SUPERVISOR_INVALID_ID)
        return ZCL_ERR(-4, "gap_fill: supervisor_register failed");
    atomic_store(&g_gf.supervisor_id, id);
    supervisor_progress(id, gap_fill_progress_marker());
    supervisor_tick(id);
    return ZCL_OK;
}

/* Walk pprev from `start` collecting block_index pointers until we
 * reach a node whose height == stop_height_exclusive (i.e. stop_h+1
 * is the lowest included height). Returns count collected. Returns
 * -1 if a corrupt walk is detected (non-monotonic height or step
 * cap hit) — caller logs and skips this pass. */
static int collect_pprev_window(struct block_index *start,
                                int stop_height_exclusive,
                                struct block_index **out,
                                int out_cap)
{
    if (!start || out_cap <= 0) return 0;
    int count = 0;
    struct block_index *cur = start;
    int last_h = cur->nHeight + 1; /* sentinel: any valid prev_h < this */
    int steps = 0;
    while (cur && count < out_cap &&
           cur->nHeight > stop_height_exclusive) {
        if (steps++ > GAPFILL_WALK_CAP) return -1; // raw-return-ok:sentinel
        if (cur->nHeight >= last_h) return -1; // raw-return-ok:sentinel
        last_h = cur->nHeight;
        out[count++] = cur;
        cur = cur->pprev;
    }
    return count;
}

bool gap_fill_compute_window(int active_tip_h, int best_header_h,
                             uint64_t body_fetch_cursor,
                             struct gap_fill_window *out)
{
    if (!out)
        return false;

    memset(out, 0, sizeof(*out));
    out->effective_tip_h = active_tip_h;
    out->best_h = best_header_h;
    out->lo = active_tip_h + 1;
    out->hi = active_tip_h;

    if (body_fetch_cursor > 0 &&
        (int64_t)body_fetch_cursor - 1 < (int64_t)out->effective_tip_h) {
        out->effective_tip_h = (int)body_fetch_cursor - 1;
    }

    out->lo = out->effective_tip_h + 1;
    out->hi = out->effective_tip_h;
    if (best_header_h <= out->effective_tip_h)
        return false;

    int gap = best_header_h - out->effective_tip_h;
    int count = gap < GAPFILL_WINDOW ? gap : GAPFILL_WINDOW;
    out->count = count;
    out->hi = out->effective_tip_h + count;
    out->has_work = count > 0;
    return out->has_work;
}

struct block_index *gap_fill_window_walk_start(
    struct block_index *best, const struct gap_fill_window *window)
{
    if (!best || !window || !window->has_work)
        return NULL;
    if (best->nHeight <= window->hi)
        return best;

    struct block_index *walk_start = best;
    int steps = 0;
    while (walk_start && walk_start->nHeight > window->hi &&
           steps++ < GAPFILL_WALK_CAP) {
        walk_start = walk_start->pprev;
    }
    if (!walk_start || walk_start->nHeight != window->hi)
        return best;
    return walk_start;
}

bool gap_fill_block_needs_queue(const struct block_index *bi)
{
    return bi && bi->phashBlock && !(bi->nStatus & BLOCK_HAVE_DATA);
}

void gap_fill_get_stats(struct gap_fill_stats *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&g_gf.mu);
    *out = g_gf.stats;
    pthread_mutex_unlock(&g_gf.mu);
}

size_t gap_fill_sweep_download_timeouts(struct download_manager *dm,
                                        int64_t now_seconds)
{
    if (!dm)
        return 0;
    if (now_seconds <= 0)
        now_seconds = (int64_t)platform_time_wall_time_t();

    size_t timed_out = dl_check_timeouts(dm, now_seconds);
    if (timed_out > 0) {
        LOG_WARN("gap_fill",
                 "[gap-fill] download timeout sweep requeued=%zu",
                 timed_out);
        event_emitf(EV_BLOCK_REQUESTED, 0,
                    "gap_fill timeout_sweep requeued=%zu", timed_out);
    }
    return timed_out;
}

static void gap_fill_wake_dispatcher(const char *reason)
{
    uint64_t total = 0;
    gap_fill_dispatch_wake_fn fn = NULL;
    void *ctx = NULL;
    pthread_mutex_lock(&g_gf.mu);
    fn = g_gf.wake_dispatch;
    ctx = g_gf.wake_dispatch_ctx;
    pthread_mutex_unlock(&g_gf.mu);
    if (!fn)
        return;
    fn(ctx);
    pthread_mutex_lock(&g_gf.mu);
    g_gf.stats.dispatch_wakes++;
    total = g_gf.stats.dispatch_wakes;
    pthread_mutex_unlock(&g_gf.mu);
    event_emitf(EV_BLOCK_REQUESTED, 0,
                "gap_fill dispatch_wake reason=%s total=%llu",
                reason ? reason : "unknown",
                (unsigned long long)total);
}

bool gap_fill_wake_dispatch_if_idle(struct download_manager *dm,
                                    const char *reason)
{
    if (!dm)
        return false;

    uint64_t in_flight = 0;
    uint64_t queued = 0;
    dl_get_stats(dm, NULL, NULL, NULL, &in_flight, &queued);
    if (queued == 0 || in_flight > 0)
        return false;

    gap_fill_wake_dispatcher(reason ? reason : "dispatch_idle");
    return true;
}

/* One pass: scan [tip+1, best_header] for missing data, queue
 * downloads. Returns number of blocks enqueued (0 = idle, -1 =
 * corrupt walk detected). */
static int gap_fill_pass(void)
{
    struct main_state *ms = g_gf.ms;
    struct download_manager *dm = g_gf.dm;
    if (!ms || !dm) return 0;

    size_t timed_out = gap_fill_sweep_download_timeouts(
        dm, (int64_t)platform_time_wall_time_t());
    pthread_mutex_lock(&g_gf.mu);
    g_gf.stats.timeout_sweeps++;
    g_gf.stats.timeouts_requeued += (uint64_t)timed_out;
    pthread_mutex_unlock(&g_gf.mu);
    if (timed_out > 0)
        gap_fill_wake_dispatcher("timeout_sweep");

    /* Snapshot tip and best_header under cs_main. We hold the lock
     * only for the pointer reads + pprev walk; the dl_queue_blocks
     * call is done outside the lock. */
    zcl_mutex_lock(&ms->cs_main);
    int tip_h = active_chain_height(&ms->chain_active);
    struct block_index *best = ms->pindex_best_header;
    int best_h = best ? best->nHeight : 0;
    struct gap_fill_window gf_window;
    bool has_window =
        gap_fill_compute_window(tip_h, best_h, body_fetch_stage_cursor(),
                                &gf_window) && best;
    tip_h = gf_window.effective_tip_h;

    if (!has_window) {
        zcl_mutex_unlock(&ms->cs_main);
        pthread_mutex_lock(&g_gf.mu);
        g_gf.stats.last_tip_h  = tip_h;
        g_gf.stats.last_best_h = best_h;
        pthread_mutex_unlock(&g_gf.mu);
        return 0;
    }

    /* Window: collect at most GAPFILL_WINDOW indices from pprev,
     * stopping at tip_h (exclusive).
     *
     * When the gap (best_h - tip_h) exceeds GAPFILL_WINDOW, walking
     * back from `best` for GAPFILL_WINDOW steps gives us the TOP of
     * the gap (e.g. h=76094..141629 with tip=8911) — but
     * reducer activation needs the BOTTOM of the gap (h=8912..) to extend the
     * active chain. The far-ahead blocks are useless until
     * intermediates connect, and they saturate dl_queue (capacity
     * 65536), preventing the immediate successors from even being
     * queued. Result: chain wedges at the current tip.
     *
     * Walk pprev from `best` down to height `tip_h + window` FIRST
     * (discarding those entries), then collect the next `window`
     * entries which end at tip_h+1. That gives the bottom window
     * of the gap, which is what the active chain needs next. */
    int window = gf_window.count;
    struct block_index *walk_start =
        gap_fill_window_walk_start(best, &gf_window);

    struct block_index **bis = zcl_malloc((size_t)window * sizeof(*bis),
                                          "gap_fill_window");
    if (!bis) {
        zcl_mutex_unlock(&ms->cs_main);
        return 0;
    }
    int collected = collect_pprev_window(walk_start, tip_h, bis, window);
    if (collected < 0) {
        zcl_mutex_unlock(&ms->cs_main);
        free(bis);
        return -1; // raw-return-ok:sentinel
    }
    if (collected == 0) {
        zcl_mutex_unlock(&ms->cs_main);
        free(bis);
        pthread_mutex_lock(&g_gf.mu);
        g_gf.stats.last_tip_h = tip_h;
        g_gf.stats.last_best_h = best_h;
        g_gf.stats.last_window_lo = tip_h + 1;
        g_gf.stats.last_window_hi = tip_h;
        pthread_mutex_unlock(&g_gf.mu);
        return 0;
    }

    /* Filter: needs data AND not in-flight. Build parallel arrays for
     * dl_queue_blocks. We allocate up to `collected` slots. */
    struct uint256 *hashes = zcl_malloc((size_t)collected * sizeof(*hashes),
                                        "gap_fill_hashes");
    int32_t *heights = zcl_malloc((size_t)collected * sizeof(*heights),
                                  "gap_fill_heights");
    if (!hashes || !heights) {
        zcl_mutex_unlock(&ms->cs_main);
        free(bis); free(hashes); free(heights);
        return 0;
    }

    int n_need = 0;
    int lo = bis[collected - 1] ? bis[collected - 1]->nHeight : tip_h + 1;
    int hi = bis[0] ? bis[0]->nHeight : tip_h;
    for (int i = 0; i < collected; i++) {
        struct block_index *bi = bis[i];
        if (!gap_fill_block_needs_queue(bi)) continue;
        if (dl_is_in_flight(dm, bi->phashBlock)) continue;
        hashes[n_need]  = *bi->phashBlock; /* value copy */
        heights[n_need] = bi->nHeight;
        n_need++;
    }
    zcl_mutex_unlock(&ms->cs_main);
    free(bis);

    int enqueued = 0;
    if (n_need > 0) {
        size_t added = dl_queue_blocks(dm, hashes, heights,
                                       (size_t)n_need);
        enqueued = (int)added;
        if (added > 0) {
            printf("[gap-fill] queued %zu blocks (window [%d..%d] "
                   "tip=%d best=%d)\n",
                   added, lo, hi, tip_h, best_h);
            event_emitf(EV_BLOCK_REQUESTED, 0,
                        "gap_fill queued=%zu lo=%d hi=%d tip=%d best=%d",
                        added, lo, hi, tip_h, best_h);
            gap_fill_wake_dispatcher("queued_blocks");
        } else {
            gap_fill_wake_dispatch_if_idle(dm, "queued_idle");
        }

        /* Explicitly front-insert (priority) the LOWEST N missing blocks
         * of the window — the connectable bottom (tip+1, tip+2, ...).
         * hashes[]/heights[] were built from bis[] which is highest-first
         * (collect_pprev_window walks pprev from the top), so the lowest
         * heights are at the END of the parallel arrays. Even though
         * dl_queue_push now keeps the queue height-sorted, this guarantees
         * the connectable bottom is queued first regardless of any later
         * far-ahead enqueue, belt-and-suspenders against starvation. */
        int prio = n_need < GAPFILL_PRIORITY_BOTTOM_N
                       ? n_need : GAPFILL_PRIORITY_BOTTOM_N;
        for (int i = 0; i < prio; i++) {
            int idx = n_need - 1 - i; /* lowest heights live at the tail */
            dl_queue_priority(dm, &hashes[idx], heights[idx]);
        }
    }

    free(hashes);
    free(heights);

    pthread_mutex_lock(&g_gf.mu);
    g_gf.stats.last_tip_h     = tip_h;
    g_gf.stats.last_best_h    = best_h;
    g_gf.stats.last_window_lo = lo;
    g_gf.stats.last_window_hi = hi;
    pthread_mutex_unlock(&g_gf.mu);
    return enqueued;
}

static void *gap_fill_thread_main(void *arg)
{
    (void)arg;
    printf("[gap-fill] service started\n");
    gap_fill_supervisor_heartbeat();
    while (!atomic_load(&g_gf.stop_requested)) {
        int n = gap_fill_pass();
        pthread_mutex_lock(&g_gf.mu);
        g_gf.stats.passes++;
        if (n > 0) {
            g_gf.stats.blocks_enqueued += (uint64_t)n;
        } else if (n == 0) {
            g_gf.stats.passes_idle++;
        } else {
            g_gf.stats.passes_corrupt_walk++;
        }
        pthread_mutex_unlock(&g_gf.mu);
        if (n < 0) {
            LOG_WARN("gap", "[gap-fill] %s:%d %s(): corrupt pprev walk detected, " "skipping pass", __FILE__, __LINE__, __func__);
        }
        gap_fill_supervisor_heartbeat();

        /* Sleep until kicked or GAPFILL_TICK_SECS elapsed. */
        pthread_mutex_lock(&g_gf.mu);
        if (!atomic_load(&g_gf.stop_requested)) {
            struct timespec until;
            platform_time_realtime_timespec(&until);
            until.tv_sec += GAPFILL_TICK_SECS;
            pthread_cond_timedwait(&g_gf.cv, &g_gf.mu, &until);
        }
        pthread_mutex_unlock(&g_gf.mu);
    }
    struct gap_fill_stats st;
    gap_fill_get_stats(&st);
    printf("[gap-fill] service stopped (passes=%llu enqueued=%llu "
           "idle=%llu corrupt=%llu timeout_sweeps=%llu "
           "timeouts_requeued=%llu dispatch_wakes=%llu)\n",
           (unsigned long long)st.passes,
           (unsigned long long)st.blocks_enqueued,
           (unsigned long long)st.passes_idle,
           (unsigned long long)st.passes_corrupt_walk,
           (unsigned long long)st.timeout_sweeps,
           (unsigned long long)st.timeouts_requeued,
           (unsigned long long)st.dispatch_wakes);
    return NULL;
}

struct zcl_result gap_fill_start(struct main_state *ms, struct download_manager *dm)
{
    if (!ms || !dm)
        return ZCL_ERR(-1, "start: null ms or dm");
    if (atomic_load(&g_gf.running)) return ZCL_OK;
    g_gf.ms = ms;
    g_gf.dm = dm;
    memset(&g_gf.stats, 0, sizeof(g_gf.stats));
    atomic_store(&g_gf.stop_requested, false);
    if (thread_registry_spawn_ex("zcl_gap_fill", gap_fill_thread_main, NULL,
                                  &g_gf.thread) != 0) {
        return ZCL_ERR(-2, "thread_registry_spawn_ex failed: errno=%d", errno);
    }
    g_gf.thread_started = true;
    atomic_store(&g_gf.running, true);
    struct zcl_result sup_r = gap_fill_register_supervisor();
    if (!sup_r.ok) {
        atomic_store(&g_gf.stop_requested, true);
        pthread_mutex_lock(&g_gf.mu);
        pthread_cond_broadcast(&g_gf.cv);
        pthread_mutex_unlock(&g_gf.mu);
        pthread_join(g_gf.thread, NULL);
        g_gf.thread_started = false;
        atomic_store(&g_gf.running, false);
        return sup_r;
    }
    return ZCL_OK;
}

void gap_fill_stop(void)
{
    if (!atomic_load(&g_gf.running)) return;
    supervisor_child_id id = atomic_load(&g_gf.supervisor_id);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_set_deadline(id, 0);
    atomic_store(&g_gf.stop_requested, true);
    pthread_mutex_lock(&g_gf.mu);
    pthread_cond_broadcast(&g_gf.cv);
    pthread_mutex_unlock(&g_gf.mu);
    if (g_gf.thread_started) {
        /* cap join at 5 s. If the worker is stuck
         * (eg holding cs_main on a long pprev walk), detach rather
         * than block systemd shutdown past TimeoutStopSec. */
        struct timespec ts;
        if (platform_time_realtime_timespec(&ts) == 0) {
            ts.tv_sec += 5;
            int rc = pthread_timedjoin_np(g_gf.thread, NULL, &ts);
            if (rc != 0) {
                LOG_WARN("gap_fill_stop", "gap_fill_stop: thread join timed out (rc=%d) — " "detaching", rc);
                pthread_detach(g_gf.thread);
            }
        } else {
            pthread_join(g_gf.thread, NULL);
        }
        g_gf.thread_started = false;
    }
    atomic_store(&g_gf.running, false);
    pthread_mutex_lock(&g_gf.mu);
    g_gf.wake_dispatch = NULL;
    g_gf.wake_dispatch_ctx = NULL;
    pthread_mutex_unlock(&g_gf.mu);
#ifdef ZCL_TESTING
    id = atomic_exchange(&g_gf.supervisor_id, SUPERVISOR_INVALID_ID);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_unregister(id);
#endif
}

void gap_fill_kick(void)
{
    if (!atomic_load(&g_gf.running)) return;
    pthread_mutex_lock(&g_gf.mu);
    pthread_cond_broadcast(&g_gf.cv);
    pthread_mutex_unlock(&g_gf.mu);
}

void gap_fill_set_dispatch_wake(gap_fill_dispatch_wake_fn fn, void *ctx)
{
    pthread_mutex_lock(&g_gf.mu);
    g_gf.wake_dispatch = fn;
    g_gf.wake_dispatch_ctx = ctx;
    pthread_mutex_unlock(&g_gf.mu);
}
