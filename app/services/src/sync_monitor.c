/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

// one-result-type-ok:monitor-frontier-repair-result — E2 (one way out): most
// of this file is sync-monitor / recovery-stats recording (void setters,
// pointer accessors, pure predicates, and out-param stats). The one operation
// executor is sync_monitor_queue_active_frontier_body(), and it returns
// struct zcl_result so a Condition remedy keeps contextual failure reasons.

#include "services/sync_monitor.h"
#include "util/log_macros.h"

#include "framework/condition.h"
#include "net/connman.h"
#include "net/netaddr.h"
#include "platform/time_compat.h"
#include "sync/sync_planner.h"
#include "services/chain_activation_service.h"
#include "services/gap_fill_service.h"
#include "sync/sync_state.h"
#include "validation/chainstate.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define LOCAL_HEADER_REFILL_MIN_PEERS 3
#define LOCAL_HEADER_REFILL_MAX_RETRIES 3

static _Atomic int64_t g_last_block_connected_ts;
static _Atomic int g_last_block_connected_height;
static _Atomic int g_recoveries_total;
static _Atomic int64_t g_last_recovery_time;
static _Atomic int g_last_recovery_type;
static _Atomic int g_last_recovery_local_height;
static _Atomic int g_last_recovery_peer_height;
static _Atomic int g_last_recovery_peer_count;
static _Atomic int g_last_recovery_target_height;
static _Atomic int g_last_recovery_manifest_height;
static char g_last_recovery_reason[96];
static char g_last_recovery_trigger[64];

static struct connman *g_condition_cm;
static struct download_manager *g_condition_dm;
static struct main_state *g_condition_ms;

static struct {
    bool active;
    bool retries_exhausted;
    int missing_height;
    int retry_count;
    int distinct_peer_count;
    int peer_rotation_count;
    char mode[32];
    char last_reason[64];
} g_local_recovery;

void sync_monitor_init(void)
{
    memset(&g_local_recovery, 0, sizeof(g_local_recovery));
    memset(g_last_recovery_reason, 0, sizeof(g_last_recovery_reason));
    memset(g_last_recovery_trigger, 0, sizeof(g_last_recovery_trigger));
    atomic_store(&g_recoveries_total, 0);
    atomic_store(&g_last_recovery_time, 0);
    atomic_store(&g_last_recovery_type, WATCHDOG_NONE);
    atomic_store(&g_last_recovery_local_height, -1);
    atomic_store(&g_last_recovery_peer_height, -1);
    atomic_store(&g_last_recovery_peer_count, 0);
    atomic_store(&g_last_recovery_target_height, -1);
    atomic_store(&g_last_recovery_manifest_height, -1);
    sync_state_monitor_init();
}

void sync_monitor_set_context(struct connman *cm,
                              struct download_manager *dm,
                              struct main_state *ms)
{
    g_condition_cm = cm;
    g_condition_dm = dm;
    g_condition_ms = ms;
    condition_engine_set_main_state(ms);

    if (ms && atomic_load(&g_last_block_connected_ts) == 0) {
        int height = active_chain_height(&ms->chain_active);
        if (height >= 0) {
            atomic_store(&g_last_block_connected_ts,
                         (int64_t)platform_time_wall_time_t());
            atomic_store(&g_last_block_connected_height, height);
        }
    }
}

struct connman *sync_monitor_connman(void)
{
    return g_condition_cm;
}

struct download_manager *sync_monitor_download_manager(void)
{
    return g_condition_dm;
}

struct main_state *sync_monitor_main_state(void)
{
    return g_condition_ms ? g_condition_ms : condition_engine_main_state();
}

void sync_monitor_on_block_connected(int height)
{
    atomic_store(&g_last_block_connected_ts,
                 (int64_t)platform_time_wall_time_t());
    atomic_store(&g_last_block_connected_height, height);
}

int64_t sync_monitor_tip_advance_age(void)
{
    int64_t last = atomic_load(&g_last_block_connected_ts);
    if (last == 0)
        return -1; // raw-return-ok:sentinel
    int64_t now = (int64_t)platform_time_wall_time_t();
    return (now > last) ? (now - last) : 0;
}

void sync_monitor_record_recovery(enum watchdog_recovery_type type,
                                  int local_height,
                                  int peer_height,
                                  int peer_count,
                                  const char *reason)
{
    atomic_fetch_add(&g_recoveries_total, 1);
    atomic_store(&g_last_recovery_time,
                 (int64_t)platform_time_wall_time_t());
    atomic_store(&g_last_recovery_type, (int)type);
    atomic_store(&g_last_recovery_local_height, local_height);
    atomic_store(&g_last_recovery_peer_height, peer_height);
    atomic_store(&g_last_recovery_peer_count, peer_count);
    atomic_store(&g_last_recovery_target_height, -1);
    atomic_store(&g_last_recovery_manifest_height, -1);
    snprintf(g_last_recovery_reason, sizeof(g_last_recovery_reason), "%s",
             reason ? reason : "");
    g_last_recovery_trigger[0] = '\0';
}

void sync_monitor_record_snapshot_resnapshot(int local_height,
                                             int peer_height,
                                             int peer_count,
                                             int target_height,
                                             int manifest_height,
                                             const char *trigger,
                                             const char *reason)
{
    sync_monitor_record_recovery(WATCHDOG_SNAPSHOT_RESNAPSHOT,
                                 local_height, peer_height, peer_count,
                                 reason);
    atomic_store(&g_last_recovery_target_height, target_height);
    atomic_store(&g_last_recovery_manifest_height, manifest_height);
    snprintf(g_last_recovery_trigger, sizeof(g_last_recovery_trigger), "%s",
             trigger ? trigger : "");
}

void sync_monitor_kick_local_sync(const char *reason)
{
    gap_fill_kick();

    struct chain_activation_controller *ctl = boot_activation_controller();
    if (!ctl || !ctl->ms || !ctl->coins_tip || !ctl->params || !ctl->datadir)
        return;

    enum activation_state state = activation_get_state(ctl);
    if (state != ACTIVATION_READY && state != ACTIVATION_AT_TIP)
        return;

    struct activation_exec_outcome outcome;
    activation_request_connect(ctl, ACTIVATION_SRC_HEADERS_ALL_DATA,
                               NULL, &outcome);
    if (outcome.result == ACTIVATION_EXEC_FAILED) {
        LOG_WARN("sync_monitor", "[sync_monitor] local activation kick failed (%s): %s", reason ? reason : "unspecified", outcome.reason[0] ? outcome.reason : "unknown");
    }
}

static struct block_index *find_active_frontier_child(
    struct main_state *ms,
    int target_height)
{
    if (!ms || target_height < 0)
        return NULL;

    struct block_index *bi = active_chain_at(&ms->chain_active,
                                             target_height);
    struct block_index *prev = target_height > 0
        ? active_chain_at(&ms->chain_active, target_height - 1)
        : NULL;
    if (bi && bi->nHeight == target_height && bi->phashBlock &&
        !block_has_any_failure(bi) &&
        (target_height == 0 || !prev || bi->pprev == prev))
        return bi;

    size_t iter = 0;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (bi && bi->nHeight == target_height &&
            bi->phashBlock && !block_has_any_failure(bi))
        {
            if (target_height > 0 && prev && bi->pprev != prev)
                continue;
            return bi;
        }
    }
    return NULL;
}

static void reset_local_addnode_backoff(struct connman *cm)
{
    if (!cm)
        return;
    for (int i = 0; i < cm->num_addnodes; i++) {
        if (!net_addr_is_local(&cm->addnodes[i].svc.addr))
            continue;
        cm->addnode_backoff_sec[i] = 0;
        cm->addnode_last_attempt[i] = 0;
    }
}

static bool ensure_blocks_download_state(const char *reason)
{
    enum sync_state st = sync_get_state();
    if (st == SYNC_BLOCKS_DOWNLOAD || st == SYNC_CONNECTING_BLOCKS)
        return true;

    if (st == SYNC_FAILED) {
        if (!sync_set_state(SYNC_IDLE, reason ? reason :
                            "frontier body queue reset")) {
            LOG_WARN("sync_monitor",
                     "[sync_monitor] block-download wake failed: %s -> idle",
                     sync_state_name(st));
            return false;
        }
        st = sync_get_state();
    }

    if (st == SYNC_IDLE || st == SYNC_AT_TIP ||
        st == SYNC_FINDING_PEERS || st == SYNC_SNAPSHOT_RECEIVE) {
        if (!sync_set_state(SYNC_HEADERS_DOWNLOAD, reason ? reason :
                            "frontier body queue headers")) {
            LOG_WARN("sync_monitor",
                     "[sync_monitor] block-download wake failed: %s -> "
                     "headers_download",
                     sync_state_name(st));
            return false;
        }
        st = sync_get_state();
    }

    if (st == SYNC_HEADERS_DOWNLOAD || st == SYNC_REORG_RECOVERY) {
        if (!sync_set_state(SYNC_BLOCKS_DOWNLOAD, reason ? reason :
                            "frontier body queue")) {
            LOG_WARN("sync_monitor",
                     "[sync_monitor] block-download wake failed: %s -> "
                     "blocks_download",
                     sync_state_name(st));
            return false;
        }
        return true;
    }

    return sync_get_state() == SYNC_BLOCKS_DOWNLOAD ||
           sync_get_state() == SYNC_CONNECTING_BLOCKS;
}

struct zcl_result sync_monitor_queue_active_frontier_body(
    int target_height,
    const char *reason)
{
    struct main_state *ms = sync_monitor_main_state();
    struct download_manager *dm = sync_monitor_download_manager();
    if (!ms || !dm)
        return ZCL_ERR(-1, "frontier body queue: missing ms or dm");

    struct uint256 target_hash;
    memset(&target_hash, 0, sizeof(target_hash));
    bool already_have_data = false;
    int local_h = target_height - 1;

    zcl_mutex_lock(&ms->cs_main);
    if (target_height < 0) {
        zcl_mutex_unlock(&ms->cs_main);
        return ZCL_ERR(-2, "frontier body queue: invalid target=%d",
                       target_height);
    }

    struct block_index *child =
        find_active_frontier_child(ms, target_height);
    if (!child) {
        zcl_mutex_unlock(&ms->cs_main);
        return ZCL_ERR(-3,
                       "frontier body queue: no visible block at h=%d",
                       target_height);
    }
    if (child->nStatus & BLOCK_FAILED_ANY_MASK) {
        zcl_mutex_unlock(&ms->cs_main);
        return ZCL_ERR(-4,
                       "frontier body queue: child failed h=%d status=%u",
                       target_height, child->nStatus);
    }
    already_have_data = (child->nStatus & BLOCK_HAVE_DATA) != 0;
    target_hash = *child->phashBlock;
    zcl_mutex_unlock(&ms->cs_main);

    if (!already_have_data)
        dl_queue_priority(dm, &target_hash, target_height);

    struct connman *cm = sync_monitor_connman();
    reset_local_addnode_backoff(cm);
    if (cm)
        connman_kick_seed_discovery(cm);

    if (!ensure_blocks_download_state(reason ? reason :
                                      "frontier body queue"))
        return ZCL_ERR(-5,
                       "frontier body queue: sync state wake failed from %s",
                       sync_state_name(sync_get_state()));

    sync_monitor_kick_local_sync(reason ? reason :
                                 "frontier body queue");
    sync_monitor_record_recovery(WATCHDOG_BODY_FRONTIER_MISSING,
                                 local_h, target_height,
                                 cm ? (int)connman_get_node_count(cm) : 0,
                                 reason ? reason :
                                 "frontier body queue");
    return ZCL_OK;
}

bool sync_monitor_active_next_child_exists(struct main_state *ms,
                                           struct block_index *tip,
                                           int next_h)
{
    if (!ms || !tip)
        return false;

    size_t iter = 0;
    struct block_index *bi = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (bi && bi->nHeight == next_h && bi->pprev == tip &&
            bi->phashBlock && !(bi->nStatus & BLOCK_FAILED_MASK))
            return true;
    }
    return false;
}

int sync_monitor_local_header_refill(struct connman *cm,
                                     int next_h,
                                     const char *reason)
{
    if (!cm)
        return 0;

    int eligible = 0;
    struct p2p_node *worst = NULL;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *n = cm->manager.nodes[i];
        if (!n || n->disconnect || n->inbound)
            continue;
        if (n->starting_height < next_h ||
            n->state < PEER_HANDSHAKE_COMPLETE)
            continue;

        eligible++;
        n->last_getheaders_time = 0;
        n->getheaders_stale_count = 0;
        if (n->state == PEER_HANDSHAKE_COMPLETE ||
            n->state == PEER_ACTIVE ||
            n->state == PEER_SYNCING_BLOCKS ||
            n->state == PEER_STALE) {
            (void)peer_set_state_checked((uint32_t)n->id, &n->state,
                                         PEER_SYNCING_HEADERS,
                                         "condition local header refill");
        }

        if (!worst ||
            n->total_headers_delivered < worst->total_headers_delivered)
            worst = n;
    }

    if (g_local_recovery.retry_count > 0 && worst && eligible >= 2 &&
        eligible < LOCAL_HEADER_REFILL_MIN_PEERS) {
        worst->disconnect = true;
        g_local_recovery.peer_rotation_count++;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    if (g_local_recovery.retry_count > 0 &&
        eligible < LOCAL_HEADER_REFILL_MIN_PEERS) {
        connman_kick_seed_discovery(cm);
    }

    g_local_recovery.active = true;
    g_local_recovery.missing_height = next_h;
    g_local_recovery.retry_count++;
    g_local_recovery.distinct_peer_count = eligible;
    snprintf(g_local_recovery.mode, sizeof(g_local_recovery.mode),
             "%s", "next-child-missing");
    snprintf(g_local_recovery.last_reason,
             sizeof(g_local_recovery.last_reason), "%s",
             reason ? reason : "");
    if (eligible >= LOCAL_HEADER_REFILL_MIN_PEERS ||
        g_local_recovery.retry_count >= LOCAL_HEADER_REFILL_MAX_RETRIES)
        g_local_recovery.retries_exhausted = true;

    return eligible;
}

void sync_monitor_get_local_recovery_stats(
    struct watchdog_local_recovery_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = g_local_recovery.active;
    out->mirror_repair_gated = g_local_recovery.active &&
        !g_local_recovery.retries_exhausted;
    out->retries_exhausted = g_local_recovery.retries_exhausted;
    out->missing_height = g_local_recovery.missing_height;
    out->retry_count = g_local_recovery.retry_count;
    out->distinct_peer_count = g_local_recovery.distinct_peer_count;
    out->peer_rotation_count = g_local_recovery.peer_rotation_count;
    snprintf(out->mode, sizeof(out->mode), "%s", g_local_recovery.mode);
    snprintf(out->last_reason, sizeof(out->last_reason), "%s",
             g_local_recovery.last_reason);
}

void sync_monitor_get_stats(struct watchdog_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->recoveries_total = atomic_load(&g_recoveries_total);
    out->last_recovery_time = atomic_load(&g_last_recovery_time);
    out->last_recovery = atomic_load(&g_last_recovery_type);
    out->last_recovery_local_height =
        atomic_load(&g_last_recovery_local_height);
    out->last_recovery_peer_height =
        atomic_load(&g_last_recovery_peer_height);
    out->last_recovery_peer_count =
        atomic_load(&g_last_recovery_peer_count);
    out->last_recovery_target_height =
        atomic_load(&g_last_recovery_target_height);
    out->last_recovery_manifest_height =
        atomic_load(&g_last_recovery_manifest_height);
    snprintf(out->last_recovery_reason,
             sizeof(out->last_recovery_reason), "%s",
             g_last_recovery_reason);
    snprintf(out->last_recovery_trigger,
             sizeof(out->last_recovery_trigger), "%s",
             g_last_recovery_trigger);
}

const char *watchdog_recovery_type_name(enum watchdog_recovery_type type)
{
    switch (type) {
    case WATCHDOG_NONE: return "NONE";
    case WATCHDOG_HEADER_STALL: return "HEADER_STALL";
    case WATCHDOG_HEADER_LAG: return "HEADER_LAG";
    case WATCHDOG_BLOCK_STALL: return "BLOCK_STALL";
    case WATCHDOG_STATE_STUCK: return "STATE_STUCK";
    case WATCHDOG_REPEATED_RESTART: return "REPEATED_RESTART";
    case WATCHDOG_PEER_FLOOR: return "PEER_FLOOR";
    case WATCHDOG_SYNC_VIOLATION: return "SYNC_VIOLATION";
    case WATCHDOG_QUEUE_STARVED: return "QUEUE_STARVED";
    case WATCHDOG_LOCAL_HEADER_REFILL: return "LOCAL_HEADER_REFILL";
    case WATCHDOG_BODY_FRONTIER_MISSING: return "BODY_FRONTIER_MISSING";
    case WATCHDOG_SNAPSHOT_RESNAPSHOT: return "SNAPSHOT_RESNAPSHOT";
    }
    return "UNKNOWN";
}

#ifdef ZCL_TESTING
void sync_monitor_test_set_local_recovery(bool active,
                                          bool retries_exhausted,
                                          int missing_height,
                                          int retry_count,
                                          const char *mode)
{
    g_local_recovery.active = active;
    g_local_recovery.retries_exhausted = retries_exhausted;
    g_local_recovery.missing_height = missing_height;
    g_local_recovery.retry_count = retry_count;
    g_local_recovery.distinct_peer_count = 0;
    g_local_recovery.peer_rotation_count = 0;
    snprintf(g_local_recovery.mode, sizeof(g_local_recovery.mode), "%s",
             mode ? mode : "");
    g_local_recovery.last_reason[0] = '\0';
}

void sync_monitor_test_set_tip_advance_ts(int64_t ts)
{
    atomic_store(&g_last_block_connected_ts, ts);
}
#endif
