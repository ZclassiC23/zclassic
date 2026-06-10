/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/peer_floor_violated.h"
#include "util/log_macros.h"
#include "framework/condition.h"

#include "event/event.h"
#include "net/connman.h"
#include "platform/time_compat.h"
#include "services/block_source_policy.h"
#include "services/sync_monitor.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define PEER_FLOOR_MIN_HEALTHY 3
#define PEER_FLOOR_TRIGGER_SECS 60

static _Atomic int64_t g_first_violation_unix;
static _Atomic int g_outbound_at_detect;
static _Atomic int g_inbound_at_detect;
static _Atomic int g_local_height_at_detect;
static _Atomic int g_peer_max_at_detect;

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
#endif

static bool peerless_ok(void)
{
    const char *v = getenv("ZCL_PEERLESS_OK");
    return v && v[0] == '1' && v[1] == '\0';
}

static bool detect_peer_floor_violated(void)
{
    if (peerless_ok()) {
        atomic_store(&g_first_violation_unix, 0);
        return false;
    }

    struct connman *cm = sync_monitor_connman();
    if (!cm)
        return false;

    struct connman_outbound_health h;
    connman_get_outbound_health(cm, &h);
    if (h.healthy >= PEER_FLOOR_MIN_HEALTHY) {
        atomic_store(&g_first_violation_unix, 0);
        return false;
    }

    int64_t now = platform_time_wall_unix();
    int64_t first = atomic_load(&g_first_violation_unix);
    if (first == 0) {
        atomic_store(&g_first_violation_unix, now);
        return false;
    }
    if (now - first < PEER_FLOOR_TRIGGER_SECS)
        return false;

    struct main_state *ms = sync_monitor_main_state();
    atomic_store(&g_outbound_at_detect, (int)h.healthy);
    atomic_store(&g_inbound_at_detect, (int)h.inbound_total);
    atomic_store(&g_peer_max_at_detect, connman_max_peer_height(cm));
    atomic_store(&g_local_height_at_detect,
                 ms ? active_chain_height(&ms->chain_active) : -1);
    return true;
}

static enum condition_remedy_result remedy_peer_floor_violated(void)
{
    struct connman *cm = sync_monitor_connman();
    if (!cm)
        return COND_REMEDY_SKIP;

    struct cac_decision decision;
    bool recover = block_source_policy_peer_floor_recovery_needed(
        atomic_load(&g_outbound_at_detect),
        PEER_FLOOR_MIN_HEALTHY,
        atomic_load(&g_local_height_at_detect),
        atomic_load(&g_peer_max_at_detect),
        &decision);
    LOG_INFO("condition", "[condition:peer_floor_violated] outbound=%d inbound=%d " "peer_max=%d decision=%s reason=%s", atomic_load(&g_outbound_at_detect), atomic_load(&g_inbound_at_detect), atomic_load(&g_peer_max_at_detect), recover ? "recover" : "wait", decision.reason);
    if (!recover)
        return COND_REMEDY_SKIP;

    size_t inbound_seen = 0;
    size_t inbound_dropped = 0;
    size_t outbound_dropped = 0;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *n = cm->manager.nodes[i];
        if (n && !n->inbound && !n->disconnect &&
            n->state < PEER_HANDSHAKE_COMPLETE) {
            n->disconnect = true;
            outbound_dropped++;
        }
        if (n && n->inbound && !n->disconnect) {
            inbound_seen++;
            if (inbound_seen > 2) {
                n->disconnect = true;
                inbound_dropped++;
            }
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    for (int ai = 0; ai < cm->num_addnodes; ai++) {
        if (cm->addnode_protocol_failures[ai] == 0 &&
            cm->addnode_tcp_failures[ai] > 0) {
            cm->addnode_backoff_sec[ai] = 0;
            cm->addnode_last_attempt[ai] = 0;
        }
    }
    connman_kick_seed_discovery(cm);
    sync_monitor_kick_local_sync("condition:peer_floor_violated");
    sync_monitor_record_recovery(WATCHDOG_PEER_FLOOR,
                                 atomic_load(&g_local_height_at_detect),
                                 atomic_load(&g_peer_max_at_detect),
                                 atomic_load(&g_outbound_at_detect),
                                 "condition:peer_floor_violated");
    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "condition PEER_FLOOR drop_outbound=%zu drop_inbound=%zu",
                outbound_dropped, inbound_dropped);

#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    return COND_REMEDY_OK;
}

static bool witness_peer_floor_violated(int64_t target_at_detect)
{
    (void)target_at_detect;
    if (peerless_ok())
        return true;

    struct connman *cm = sync_monitor_connman();
    if (!cm)
        return false;

    /* Peers must be restored above the floor — necessary but NOT sufficient. */
    if (connman_outbound_healthy_count(cm) < PEER_FLOOR_MIN_HEALTHY)
        return false;

    /* DOCTRINE: a peer_floor remedy only resolved the symptom if the chain
     * actually resumed making progress. Counting peers alone is the original
     * false-ok bug: during a header-admit wedge we HAD >=3 healthy
     * peers, so the old witness reported `ok` 46x while the tip never moved.
     *
     * Resolution = either (a) the local height advanced past the height
     * recorded at detect (chain is moving again), OR (b) there is genuinely
     * nothing to fetch — we are already at/above the best height any peer
     * advertises, so peers were the only deficit and it is now cured. If
     * peers advertise blocks beyond our tip and our tip has NOT advanced, the
     * symptom persists (something other than a peer shortage is wedging us)
     * and the witness must FAIL so attempts accrue toward operator_needed. */
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        return false;
    int height_now = active_chain_height(&ms->chain_active);
    int height_at_detect = atomic_load(&g_local_height_at_detect);
    if (height_at_detect >= 0 && height_now > height_at_detect)
        return true; /* chain advanced — genuinely recovered */

    int peer_max = connman_max_peer_height(cm);
    /* Nothing to sync: peers offer nothing beyond what we already have. */
    return peer_max >= 0 && height_now >= peer_max;
}

static struct condition c_peer_floor_violated = {
    .name = "peer_floor_violated",
    .severity = COND_WARN,
    .poll_secs = 5,
    .backoff_secs = 60,
    /* Finite: after this many un-witnessed remedies (peers present but the
     * tip is not advancing), the engine escalates to operator_needed. The old
     * value (100000) meant a wedged-but-peered chain could never page anyone
     * — the engine just looped result=ok forever. */
    .max_attempts = 5,
    .detect = detect_peer_floor_violated,
    .remedy = remedy_peer_floor_violated,
    .witness = witness_peer_floor_violated,
    .witness_window_secs = 60,
};

void register_peer_floor_violated(void)
{
    (void)condition_register(&c_peer_floor_violated);
}

#ifdef ZCL_TESTING
void peer_floor_violated_test_reset(void)
{
    atomic_store(&g_first_violation_unix, 0);
    atomic_store(&g_outbound_at_detect, 0);
    atomic_store(&g_inbound_at_detect, 0);
    atomic_store(&g_local_height_at_detect, -1);
    atomic_store(&g_peer_max_at_detect, -1);
    atomic_store(&g_test_remedy_calls, 0);
}

int peer_floor_violated_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif
