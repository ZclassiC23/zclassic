/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

// one-result-type-ok:monitor-frontier-repair-result — E2 (one way out): most
// of this file is sync-monitor / recovery-stats recording (void setters,
// pointer accessors, pure predicates, and out-param stats). The one operation
// executor is sync_monitor_queue_active_frontier_body(), and it returns
// struct zcl_result so a Condition remedy keeps contextual failure reasons.

#include "services/sync_monitor.h"
#include "util/log_macros.h"

#include "json/json.h"
#include "framework/condition.h"
#include "net/connman.h"
#include "net/msgprocessor.h"
#include "net/netaddr.h"
#include "platform/time_compat.h"
#include "sync/sync_planner.h"
#include "services/chain_activation_service.h"
#include "services/gap_fill_service.h"
#include "sync/sync_state.h"
#include "validation/chainstate.h"

#include <pthread.h>
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

/* Guards the multi-word g_local_recovery diagnostic struct: the condition
 * engine writes it from sync_monitor_local_header_refill() while the legacy
 * mirror, health, and MCP threads read it via
 * sync_monitor_get_local_recovery_stats(). This lock must NEVER be held while
 * taking cm->manager.cs_nodes (or cs_main/coins_kv): the refill path snapshots
 * cs_nodes-derived state to locals and only touches g_local_recovery after
 * releasing cs_nodes, so the two never nest. */
static pthread_mutex_t g_local_recovery_lock = PTHREAD_MUTEX_INITIALIZER;

void sync_monitor_init(void)
{
    pthread_mutex_lock(&g_local_recovery_lock);
    memset(&g_local_recovery, 0, sizeof(g_local_recovery));
    pthread_mutex_unlock(&g_local_recovery_lock);
    memset(g_last_recovery_reason, 0, sizeof(g_last_recovery_reason));
    memset(g_last_recovery_trigger, 0, sizeof(g_last_recovery_trigger));
    atomic_store(&g_last_block_connected_ts, 0);
    atomic_store(&g_last_block_connected_height, -1);
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

static struct block_index *find_best_header_ancestor(
    struct main_state *ms,
    int target_height)
{
    if (!ms || target_height < 0)
        return NULL;

    struct block_index *best = ms->pindex_best_header;
    if (!best || target_height > best->nHeight)
        return NULL;

    struct block_index *bi = block_index_get_ancestor(best, target_height);
    if (!bi || bi->nHeight != target_height || !bi->phashBlock ||
        block_has_any_failure(bi))
        return NULL;
    return bi;
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

enum body_queue_selector {
    BODY_QUEUE_ACTIVE_FRONTIER = 0,
    BODY_QUEUE_BEST_HEADER_ANCESTOR = 1,
};

static const char *body_queue_selector_name(enum body_queue_selector selector)
{
    switch (selector) {
    case BODY_QUEUE_ACTIVE_FRONTIER: return "active frontier";
    case BODY_QUEUE_BEST_HEADER_ANCESTOR: return "best-header ancestor";
    }
    return "unknown";
}

static struct block_index *resolve_body_queue_target(
    struct main_state *ms,
    int target_height,
    enum body_queue_selector selector)
{
    switch (selector) {
    case BODY_QUEUE_ACTIVE_FRONTIER:
        return find_active_frontier_child(ms, target_height);
    case BODY_QUEUE_BEST_HEADER_ANCESTOR:
        return find_best_header_ancestor(ms, target_height);
    }
    return NULL;
}

static struct zcl_result queue_body_target(
    int target_height,
    const char *reason,
    enum body_queue_selector selector)
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

    struct block_index *target =
        resolve_body_queue_target(ms, target_height, selector);
    if (!target) {
        zcl_mutex_unlock(&ms->cs_main);
        return ZCL_ERR(-3,
                       "frontier body queue: no %s block at h=%d",
                       body_queue_selector_name(selector), target_height);
    }
    if (target->nStatus & BLOCK_FAILED_ANY_MASK) {
        zcl_mutex_unlock(&ms->cs_main);
        return ZCL_ERR(-4,
                       "frontier body queue: %s block failed h=%d status=%u",
                       body_queue_selector_name(selector), target_height,
                       target->nStatus);
    }
    already_have_data = (target->nStatus & BLOCK_HAVE_DATA) != 0;
    target_hash = *target->phashBlock;
    if (selector == BODY_QUEUE_BEST_HEADER_ANCESTOR)
        local_h = active_chain_height(&ms->chain_active);
    zcl_mutex_unlock(&ms->cs_main);

    if (!already_have_data) {
        msg_processor_clear_seen_block(&target_hash);
        dl_queue_priority(dm, &target_hash, target_height);
    }

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

struct zcl_result sync_monitor_queue_active_frontier_body(
    int target_height,
    const char *reason)
{
    return queue_body_target(target_height, reason,
                             BODY_QUEUE_ACTIVE_FRONTIER);
}

struct zcl_result sync_monitor_queue_best_header_body(
    int target_height,
    const char *reason)
{
    return queue_body_target(target_height, reason,
                             BODY_QUEUE_BEST_HEADER_ANCESTOR);
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
    bool rotated_peer = false;

    /* Snapshot the prior retry count before taking cs_nodes so the peer scan
     * never touches g_local_recovery while cs_nodes is held (lock-order: the
     * recovery lock must not nest under cs_nodes). */
    pthread_mutex_lock(&g_local_recovery_lock);
    int prev_retry_count = g_local_recovery.retry_count;
    pthread_mutex_unlock(&g_local_recovery_lock);

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *n = cm->manager.nodes[i];
        if (!n || n->disconnect || n->inbound)
            continue;
        if (n->starting_height < next_h ||
            n->state < PEER_HANDSHAKE_COMPLETE)
            continue;

        eligible++;
        atomic_store_explicit(&n->last_getheaders_time, 0, memory_order_relaxed);
        atomic_store_explicit(&n->getheaders_stale_count, 0, memory_order_relaxed);
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

    if (prev_retry_count > 0 && worst && eligible >= 2 &&
        eligible < LOCAL_HEADER_REFILL_MIN_PEERS) {
        worst->disconnect = true;
        rotated_peer = true;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    if (prev_retry_count > 0 &&
        eligible < LOCAL_HEADER_REFILL_MIN_PEERS) {
        connman_kick_seed_discovery(cm);
    }

    pthread_mutex_lock(&g_local_recovery_lock);
    if (rotated_peer)
        g_local_recovery.peer_rotation_count++;
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
    pthread_mutex_unlock(&g_local_recovery_lock);

    return eligible;
}

void sync_monitor_get_local_recovery_stats(
    struct watchdog_local_recovery_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_local_recovery_lock);
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
    pthread_mutex_unlock(&g_local_recovery_lock);
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

/* zcl_state subsystem=sync_monitor — the sync watchdog's recovery counters
 * plus the local-recovery (header-refill) sub-state. See CLAUDE.md "Adding
 * state introspection". Reentrant-safe (both accessors snapshot internally). */
bool sync_monitor_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    struct watchdog_stats wd;
    sync_monitor_get_stats(&wd);
    json_push_kv_int (out, "last_block_connected_height",
                      atomic_load(&g_last_block_connected_height));
    json_push_kv_int (out, "last_block_connected_time",
                      atomic_load(&g_last_block_connected_ts));
    json_push_kv_int (out, "tip_advance_age_seconds",
                      sync_monitor_tip_advance_age());
    json_push_kv_int (out, "recoveries_total", wd.recoveries_total);
    json_push_kv_str (out, "last_recovery",
                      watchdog_recovery_type_name(wd.last_recovery));
    json_push_kv_int (out, "last_recovery_time", wd.last_recovery_time);
    json_push_kv_int (out, "last_recovery_local_height",
                      wd.last_recovery_local_height);
    json_push_kv_int (out, "last_recovery_peer_height",
                      wd.last_recovery_peer_height);
    json_push_kv_int (out, "last_recovery_peer_count",
                      wd.last_recovery_peer_count);
    json_push_kv_int (out, "last_recovery_target_height",
                      wd.last_recovery_target_height);
    json_push_kv_int (out, "last_recovery_manifest_height",
                      wd.last_recovery_manifest_height);
    json_push_kv_str (out, "last_recovery_reason", wd.last_recovery_reason);
    json_push_kv_str (out, "last_recovery_trigger", wd.last_recovery_trigger);

    uint64_t dl_requested = 0;
    uint64_t dl_received = 0;
    uint64_t dl_timed_out = 0;
    uint64_t dl_in_flight = 0;
    uint64_t dl_queued = 0;
    uint64_t dl_bytes = 0;
    double dl_mbps_avg = 0.0;
    struct download_manager *dm = sync_monitor_download_manager();
    if (dm) {
        dl_get_stats(dm, &dl_requested, &dl_received, &dl_timed_out,
                     &dl_in_flight, &dl_queued);
        dl_get_throughput(dm, &dl_bytes, &dl_mbps_avg);
    }
    json_push_kv_int(out, "download_requested", (int64_t)dl_requested);
    json_push_kv_int(out, "download_received", (int64_t)dl_received);
    json_push_kv_int(out, "download_timed_out", (int64_t)dl_timed_out);
    json_push_kv_int(out, "download_in_flight", (int64_t)dl_in_flight);
    json_push_kv_int(out, "download_queued", (int64_t)dl_queued);
    json_push_kv_int(out, "download_bytes_received", (int64_t)dl_bytes);
    json_push_kv_real(out, "download_mbps_avg", dl_mbps_avg);

    struct watchdog_local_recovery_stats lr;
    sync_monitor_get_local_recovery_stats(&lr);
    json_push_kv_bool(out, "local_recovery_active", lr.active);
    json_push_kv_bool(out, "mirror_repair_gated", lr.mirror_repair_gated);
    json_push_kv_bool(out, "retries_exhausted", lr.retries_exhausted);
    json_push_kv_int (out, "missing_height", lr.missing_height);
    json_push_kv_int (out, "retry_count", lr.retry_count);
    json_push_kv_int (out, "distinct_peer_count", lr.distinct_peer_count);
    json_push_kv_int (out, "peer_rotation_count", lr.peer_rotation_count);
    json_push_kv_str (out, "local_recovery_mode", lr.mode);
    json_push_kv_str (out, "local_recovery_last_reason", lr.last_reason);
    return true;
}

#ifdef ZCL_TESTING
void sync_monitor_test_set_local_recovery(bool active,
                                          bool retries_exhausted,
                                          int missing_height,
                                          int retry_count,
                                          const char *mode)
{
    pthread_mutex_lock(&g_local_recovery_lock);
    g_local_recovery.active = active;
    g_local_recovery.retries_exhausted = retries_exhausted;
    g_local_recovery.missing_height = missing_height;
    g_local_recovery.retry_count = retry_count;
    g_local_recovery.distinct_peer_count = 0;
    g_local_recovery.peer_rotation_count = 0;
    snprintf(g_local_recovery.mode, sizeof(g_local_recovery.mode), "%s",
             mode ? mode : "");
    g_local_recovery.last_reason[0] = '\0';
    pthread_mutex_unlock(&g_local_recovery_lock);
}

void sync_monitor_test_set_tip_advance_ts(int64_t ts)
{
    atomic_store(&g_last_block_connected_ts, ts);
}
#endif
