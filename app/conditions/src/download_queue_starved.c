/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/download_queue_starved.h"
#include "util/log_macros.h"
#include "framework/condition.h"

#include "net/connman.h"
#include "net/download.h"
#include "platform/time_compat.h"
#include "services/sync_monitor.h"
#include "sync/sync_state.h"

#include <stdatomic.h>
#include <stdio.h>

#define QUEUE_STARVED_TRIGGER_SECS 120
#define QUEUE_STARVED_RATIO_DEN 10

static _Atomic int64_t g_first_seen;
static _Atomic uint64_t g_inflight_at_detect;
static _Atomic uint64_t g_queued_at_detect;
static _Atomic uint64_t g_requested_at_detect;
static _Atomic int64_t g_age_at_detect;

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
#endif

static bool detect_download_queue_starved(void)
{
    struct connman *cm = sync_monitor_connman();
    struct download_manager *dm =
        sync_monitor_download_manager();
    int64_t now = platform_time_wall_unix();
    if (sync_get_state() != SYNC_BLOCKS_DOWNLOAD || !cm || !dm ||
        connman_get_node_count(cm) == 0) {
        atomic_store(&g_first_seen, 0);
        return false;
    }

    uint64_t requested = 0, inflight = 0, queued = 0;
    dl_get_stats(dm, &requested, NULL, NULL, &inflight, &queued);
    uint64_t threshold = DL_MAX_IN_FLIGHT_TOTAL_IBD /
                         QUEUE_STARVED_RATIO_DEN;
    if (inflight >= threshold) {
        atomic_store(&g_first_seen, 0);
        return false;
    }
    int64_t first = atomic_load(&g_first_seen);
    if (first == 0) {
        atomic_store(&g_first_seen, now);
        return false;
    }
    int64_t age = now - first;
    if (age < QUEUE_STARVED_TRIGGER_SECS)
        return false;

    atomic_store(&g_inflight_at_detect, inflight);
    atomic_store(&g_queued_at_detect, queued);
    atomic_store(&g_requested_at_detect, requested);
    atomic_store(&g_age_at_detect, age);
    return true;
}

static enum condition_remedy_result remedy_download_queue_starved(void)
{
    LOG_WARN("condition", "[condition:download_queue_starved] in_flight=%llu queued=%llu " "age=%llds action=kick_refill", (unsigned long long)atomic_load(&g_inflight_at_detect), (unsigned long long)atomic_load(&g_queued_at_detect), (long long)atomic_load(&g_age_at_detect));
    sync_monitor_kick_local_sync("condition:download_queue_starved");
#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    return COND_REMEDY_OK;
}

static bool witness_download_queue_starved(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct download_manager *dm =
        sync_monitor_download_manager();
    if (!dm)
        return false;
    /* HONEST witness (Law 7): the remedy (kick_refill) is async and mutates
     * nothing observable on return, so an inflight-level threshold here is
     * orthogonal to whether the remedy did anything — a transient inflight
     * spike could satisfy it with zero effect. The symptom is "the queue is
     * not refilling" (no new requests going out), so we witness that it MOVED:
     * the cumulative request counter advanced past the value captured at
     * detect, i.e. the queue was actually refilled and new blocks were
     * requested. A frozen download pipeline cannot manufacture that increase. */
    uint64_t requested = 0;
    dl_get_stats(dm, &requested, NULL, NULL, NULL, NULL);
    return requested > atomic_load(&g_requested_at_detect);
}

static struct condition c_download_queue_starved = {
    .name = "download_queue_starved",
    .severity = COND_WARN,
    .poll_secs = 5,
    .backoff_secs = 120,
    /* Page after 5 unwitnessed kick_refill attempts (the request counter never
     * advanced), THEN re-arm on the cooldown below — escalate once without ever
     * permanently giving up. (100000 attempts at 120s backoff made this
     * effectively non-escalating; a hard cap-and-latch made a transient no-peer
     * stall permanent. Re-arm is the robust middle: page + keep trying.) */
    .max_attempts = 5,
    .detect = detect_download_queue_starved,
    .remedy = remedy_download_queue_starved,
    .witness = witness_download_queue_starved,
    .witness_window_secs = 60,
    /* External-resource fault (peers / bandwidth / a momentarily empty fetch
     * window) — NOT a deterministic local fault. Re-arm on a long cooldown so a
     * transient fetch stall can never become a permanent operator_needed latch;
     * the remedy keeps retrying every 5 min, unbounded, until the queue refills.
     * Mirrors peer_floor_violated (the proven external-dependency pattern). */
    .cooldown_secs = 300,
    .cooldown_max_rearms = 0,
};

void register_download_queue_starved(void)
{
    (void)condition_register(&c_download_queue_starved);
}

#ifdef ZCL_TESTING
void download_queue_starved_test_reset(void)
{
    atomic_store(&g_first_seen, 0);
    atomic_store(&g_inflight_at_detect, 0);
    atomic_store(&g_queued_at_detect, 0);
    atomic_store(&g_requested_at_detect, 0);
    atomic_store(&g_age_at_detect, 0);
    atomic_store(&g_test_remedy_calls, 0);
}

int download_queue_starved_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif
