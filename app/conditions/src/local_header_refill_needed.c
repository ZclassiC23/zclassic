/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/local_header_refill_needed.h"
#include "core/uint256.h"
#include "util/log_macros.h"
#include "framework/condition.h"

#include "net/connman.h"
#include "services/block_source_policy.h"
#include "services/sync_monitor.h"
#include "sync/sync_state.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>

static _Atomic int g_tip_at_detect = -1;
static _Atomic int g_missing_height = -1;
static _Atomic int g_peer_max_at_detect = -1;

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
#endif

static int best_header_same_height_body_target(struct main_state *ms)
{
    int target = -1;
    if (!ms) {
        LOG_WARN("condition",
                 "[condition:local_header_refill_needed] best-header body "
                 "target unavailable: missing main_state");
        return target;
    }

    zcl_mutex_lock(&ms->cs_main);
    struct block_index *tip = active_chain_tip(&ms->chain_active);
    struct block_index *best = ms->pindex_best_header;
    if (tip && tip->phashBlock && best && best->nHeight > tip->nHeight) {
        struct block_index *best_at_tip =
            block_index_get_ancestor(best, tip->nHeight);
        if (best_at_tip && best_at_tip->phashBlock &&
            !uint256_eq(best_at_tip->phashBlock, tip->phashBlock) &&
            !block_has_any_failure(best_at_tip))
            target = tip->nHeight;
    }
    zcl_mutex_unlock(&ms->cs_main);
    return target;
}

static bool detect_local_header_refill_needed(void)
{
    struct connman *cm = sync_monitor_connman();
    struct main_state *ms = sync_monitor_main_state();
    if (!cm || !ms)
        return false;

    int peer_max = connman_max_peer_height(cm);
    int tip_h = -1;
    int next_h = -1;
    bool missing = false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *tip = active_chain_tip(&ms->chain_active);
    if (tip) {
        tip_h = tip->nHeight;
        next_h = tip_h + 1;
        missing = peer_max >= next_h &&
            !sync_monitor_active_next_child_exists(ms, tip, next_h);
    }
    zcl_mutex_unlock(&ms->cs_main);
    if (!missing)
        return false;

    atomic_store(&g_tip_at_detect, tip_h);
    atomic_store(&g_missing_height, next_h);
    atomic_store(&g_peer_max_at_detect, peer_max);
    return true;
}

static enum condition_remedy_result remedy_local_header_refill_needed(void)
{
    struct connman *cm = sync_monitor_connman();
    int next_h = atomic_load(&g_missing_height);
    int peers = sync_monitor_local_header_refill(
        cm, next_h, "condition:local_header_refill_needed");
    struct watchdog_local_recovery_stats lr;
    sync_monitor_get_local_recovery_stats(&lr);
    struct cac_decision decision;
    bool proceed = block_source_policy_local_header_refill_needed(
        atomic_load(&g_tip_at_detect),
        next_h,
        atomic_load(&g_peer_max_at_detect),
        peers,
        lr.retry_count,
        lr.retries_exhausted,
        &decision);
    LOG_WARN("condition", "[condition:local_header_refill_needed] missing=%d peer_max=%d " "eligible=%d decision=%s reason=%s", next_h, atomic_load(&g_peer_max_at_detect), peers, proceed ? "proceed" : "wait", decision.reason);
    if (proceed) {
        bool queued_body = false;
        struct main_state *ms = sync_monitor_main_state();
        int body_h = best_header_same_height_body_target(ms);
        if (body_h >= 0) {
            struct zcl_result qr = sync_monitor_queue_best_header_body(
                body_h,
                "condition:local_header_refill_needed same-height fork body");
            if (!qr.ok) {
                LOG_WARN("condition",
                         "[condition:local_header_refill_needed] best-header "
                         "body queue failed h=%d code=%d msg=%s",
                         body_h, qr.code, qr.message);
            } else {
                LOG_WARN("condition",
                         "[condition:local_header_refill_needed] queued "
                         "best-header same-height body h=%d",
                         body_h);
                queued_body = true;
            }
        }
        if (!queued_body) {
            if (!sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                "condition local_header_refill_needed")) {
                sync_set_state(SYNC_IDLE, "condition local refill via idle");
                sync_set_state(SYNC_HEADERS_DOWNLOAD,
                               "condition local_header_refill_needed");
            }
            sync_monitor_kick_local_sync(
                "condition:local_header_refill_needed");
        }
    }
#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    return COND_REMEDY_OK;
}

static bool witness_local_header_refill_needed(int64_t target_at_detect)
{
    (void)target_at_detect;
    /* Witness the symptom MOVED, not that detect flipped. The remedy's job
     * was to supply the missing header at g_missing_height. Verify forward
     * progress directly: either that header child now exists (it arrived), or
     * peers no longer reach that height (the gap closed from the other end).
     * Re-running detect would only confirm "the poison I named is gone" — a
     * tautology forbidden by Law 7. */
    int missing_h = atomic_load(&g_missing_height);
    if (missing_h < 0)
        return false;

    struct connman *cm = sync_monitor_connman();
    struct main_state *ms = sync_monitor_main_state();
    if (!cm || !ms)
        return false;

    int peer_max = connman_max_peer_height(cm);
    if (peer_max < missing_h)
        return true; /* peers retreated below the needed height */

    bool arrived = false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *tip = active_chain_tip(&ms->chain_active);
    if (tip && tip->nHeight >= missing_h)
        arrived = true;
    else if (tip)
        arrived = sync_monitor_active_next_child_exists(ms, tip, missing_h);
    zcl_mutex_unlock(&ms->cs_main);
    return arrived; /* the missing header child actually showed up */
}

static struct condition c_local_header_refill_needed = {
    .name = "local_header_refill_needed",
    .severity = COND_WARN,
    .poll_secs = 5,
    .backoff_secs = 300,
    .max_attempts = 3,
    .detect = detect_local_header_refill_needed,
    .remedy = remedy_local_header_refill_needed,
    .witness = witness_local_header_refill_needed,
    .witness_window_secs = 60,
};

void register_local_header_refill_needed(void)
{
    (void)condition_register(&c_local_header_refill_needed);
}

#ifdef ZCL_TESTING
void local_header_refill_needed_test_reset(void)
{
    atomic_store(&g_tip_at_detect, -1);
    atomic_store(&g_missing_height, -1);
    atomic_store(&g_peer_max_at_detect, -1);
    atomic_store(&g_test_remedy_calls, 0);
}

int local_header_refill_needed_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif
