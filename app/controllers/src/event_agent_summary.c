/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Compact AI/operator first-call telemetry. This path must stay cheap even
 * while the full healthcheck path is doing repair-era evidence work. */

#include "event_agent_summary.h"

#include "api_controller_internal.h"
#include "controllers/agent_controller.h"
#include "controllers/network_controller.h"
#include "controllers/strong_params.h"
#include "event/event.h"
#include "jobs/tip_finalize_stage.h"
#include "json/json.h"
#include "net/connman.h"
#include "net/download.h"
#include "net/msgprocessor.h"
#include "net/onion_service.h"
#include "net/tor_integration.h"
#include "net/version.h"
#include "platform/time_compat.h"
#include "rpc/server.h"
#include "services/invariant_sentinel.h"
#include "services/gap_fill_service.h"
#include "services/sync_monitor.h"
#include "sync/sync_state.h"
#include "util/alerts.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AGENT_CATCHUP_STALL_SECS 120
#define AGENT_DISPATCH_IDLE_SECS 30

struct agent_fast_snapshot {
    enum sync_state sync_state;
    int served_height;
    int tip_height;
    int header_height;
    int peer_best_height;
    int target_height;
    int gap;
    int index_gap;
    int log_head;
    int log_head_gap;
    size_t peer_count;
    size_t magicbean_peer_count;
    size_t zclassic_c23_peer_count;
    bool has_peers;
    bool healthy;
    bool serving;
    bool operator_needed;
    bool operator_latch_recovered;
    bool validation_pack_ok;
    bool tor_enabled;
    bool tor_ready;
    bool onion_service_ready;
    bool catchup_active;
    bool catchup_stalled;
    bool download_dispatch_idle;
    bool download_dispatch_stalled;
    uint64_t blocks_requested;
    uint64_t blocks_received;
    uint64_t blocks_timed_out;
    uint64_t in_flight;
    uint64_t queued;
    uint64_t overdue_in_flight;
    uint64_t in_flight_peer_count;
    uint64_t assign_attempts;
    uint64_t assign_successes;
    uint64_t assign_zero_results;
    uint64_t dispatch_wakes;
    uint64_t message_cycles;
    uint64_t message_nodes_snapshotted;
    uint64_t message_send_calls;
    uint64_t message_process_calls;
    uint64_t message_recv_ready;
    uint64_t message_idle_waits;
    uint64_t message_wakes;
    uint64_t block_intake_enqueued;
    uint64_t block_intake_processed;
    uint64_t block_intake_accepted;
    uint64_t block_intake_retryable;
    uint64_t block_intake_rejected;
    uint64_t block_intake_dropped;
    uint64_t block_intake_clone_failed;
    uint64_t block_intake_spawn_failed;
    uint64_t block_intake_current_depth;
    uint64_t block_intake_capacity;
    uint64_t block_intake_max_depth;
    bool block_intake_running;
    bool block_intake_stopping;
    uint32_t last_assign_peer_id;
    uint64_t last_assign_max_requested;
    uint64_t last_assign_available;
    uint64_t last_assign_assigned;
    uint64_t last_assign_queue_len;
    uint64_t last_assign_active;
    uint64_t last_assign_peer_in_flight;
    uint64_t last_assign_peer_limit;
    uint64_t last_assign_global_limit;
    uint64_t download_bytes_received;
    double download_mbps_avg;
    int request_timeout_seconds;
    int last_assign_result;
    int32_t oldest_in_flight_height;
    uint32_t oldest_in_flight_peer_id;
    int64_t tip_advance_age_seconds;
    int64_t catchup_stall_seconds;
    int64_t download_dispatch_idle_seconds;
    int64_t oldest_in_flight_age_seconds;
    int64_t last_error_age_seconds;
    int warning_count;
    char warning_reasons[256];
    char blocking_reason[128];
    char last_error_type[64];
    char validation_pack_detail[64];
};

static int agent_fast_served_height(void)
{
    int64_t served = api_served_tip_height();
    if (served < 0 || served > INT_MAX)
        return 0;
    return (int)served;
}

static void agent_fast_add_warning(struct agent_fast_snapshot *s,
                                   const char *reason)
{
    if (!s || !reason || !reason[0])
        return;
    if (strstr(s->warning_reasons, reason))
        return;
    size_t used = strlen(s->warning_reasons);
    size_t cap = sizeof(s->warning_reasons);
    if (used + 1 < cap) {
        int n = snprintf(s->warning_reasons + used, cap - used,
                         "%s%s", used ? "," : "", reason);
        if (n > 0)
            s->warning_count++;
    } else {
        s->warning_count++;
    }
}

static int agent_fast_tip_finalize_log_head(void)
{
    uint64_t live = tip_finalize_stage_cursor();
    if (live > 0 && live <= INT_MAX)
        return (int)live;
    return -1; // raw-return-ok:sentinel
}

static void agent_fast_collect_peer_counts(struct agent_fast_snapshot *s,
                                           struct connman *cm)
{
    if (!s || !cm)
        return;

    s->peer_count = connman_get_node_count(cm);
    s->has_peers = s->peer_count > 0;
    s->peer_best_height = connman_max_peer_height(cm);

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *node = cm->manager.nodes[i];
        if (!node || node->disconnect)
            continue;
        if (node->state < PEER_HANDSHAKE_COMPLETE)
            continue;
        bool is_mb = false, is_z23 = false;
        msg_version_classify_peer(node->sub_ver, node->services,
                                  &is_mb, &is_z23);
        if (is_mb)
            s->magicbean_peer_count++;
        if (is_z23)
            s->zclassic_c23_peer_count++;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
}

static void agent_fast_collect_errors(struct agent_fast_snapshot *s)
{
    if (!s)
        return;
    s->last_error_age_seconds = -1;
    struct error_ring *er = error_ring_global();
    const struct error_entry *last_err = error_ring_last(er);
    if (!last_err || !last_err->message[0])
        return;

    int64_t now_us = (int64_t)platform_time_wall_time_t() * 1000000;
    if (last_err->timestamp_us > 0 && now_us >= last_err->timestamp_us)
        s->last_error_age_seconds =
            (now_us - last_err->timestamp_us) / 1000000;
    snprintf(s->last_error_type, sizeof(s->last_error_type),
             "%s", event_type_name(last_err->type));
    if (s->last_error_age_seconds >= 0 &&
        s->last_error_age_seconds <= 300)
        agent_fast_add_warning(s, "recent_error");
}

static void agent_fast_collect(struct agent_fast_snapshot *s)
{
    struct agent_fast_snapshot empty = {0};
    if (!s)
        return;
    *s = empty;
    s->tip_height = -1;
    s->header_height = -1;
    s->peer_best_height = -1;
    s->log_head = -1;
    s->log_head_gap = -1;
    s->last_error_age_seconds = -1;
    s->oldest_in_flight_age_seconds = -1;
    s->oldest_in_flight_height = -1;
    s->validation_pack_ok = true;

    s->sync_state = sync_get_state();
    s->served_height = agent_fast_served_height();

    struct main_state *ms = sync_monitor_main_state();
    if (ms) {
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip)
            s->tip_height = tip->nHeight;
        if (ms->pindex_best_header)
            s->header_height = ms->pindex_best_header->nHeight;
    }
    if (s->tip_height < 0)
        s->tip_height = s->served_height;
    if (s->header_height < 0)
        s->header_height = s->tip_height;

    agent_fast_collect_peer_counts(s, rpc_net_get_connman());

    s->log_head = agent_fast_tip_finalize_log_head();
    if (s->peer_best_height >= 0 && s->log_head >= 0)
        s->log_head_gap = s->peer_best_height - s->log_head;

    struct download_manager *dm = msg_get_download_mgr();
    if (dm) {
        struct dl_diagnostics diag;
        dl_get_stats(dm, &s->blocks_requested, &s->blocks_received,
                     &s->blocks_timed_out, &s->in_flight, &s->queued);
        dl_get_diagnostics(dm, &diag);
        s->request_timeout_seconds = diag.request_timeout_seconds;
        s->oldest_in_flight_age_seconds =
            diag.oldest_in_flight_age_seconds;
        s->oldest_in_flight_height = diag.oldest_in_flight_height;
        s->oldest_in_flight_peer_id = diag.oldest_in_flight_peer_id;
        s->overdue_in_flight = diag.overdue_in_flight;
        s->in_flight_peer_count = diag.in_flight_peer_count;
        s->assign_attempts = diag.assign_attempts;
        s->assign_successes = diag.assign_successes;
        s->assign_zero_results = diag.assign_zero_results;
        s->last_assign_peer_id = diag.last_assign_peer_id;
        s->last_assign_max_requested = diag.last_assign_max_requested;
        s->last_assign_available = diag.last_assign_available;
        s->last_assign_assigned = diag.last_assign_assigned;
        s->last_assign_queue_len = diag.last_assign_queue_len;
        s->last_assign_active = diag.last_assign_active;
        s->last_assign_peer_in_flight = diag.last_assign_peer_in_flight;
        s->last_assign_peer_limit = diag.last_assign_peer_limit;
        s->last_assign_global_limit = diag.last_assign_global_limit;
        s->last_assign_result = diag.last_assign_result;
        dl_get_throughput(dm, &s->download_bytes_received,
                          &s->download_mbps_avg);
    }
    {
        struct gap_fill_stats gf_stats;
        gap_fill_get_stats(&gf_stats);
        s->dispatch_wakes = gf_stats.dispatch_wakes;
    }
    {
        struct connman_message_cycle_stats msg_stats;
        connman_get_message_cycle_stats(rpc_net_get_connman(), &msg_stats);
        s->message_cycles = msg_stats.cycles;
        s->message_nodes_snapshotted = msg_stats.nodes_snapshotted;
        s->message_send_calls = msg_stats.send_calls;
        s->message_process_calls = msg_stats.process_calls;
        s->message_recv_ready = msg_stats.recv_ready;
        s->message_idle_waits = msg_stats.idle_waits;
        s->message_wakes = msg_stats.wakes;
    }
    {
        struct msg_block_intake_stats intake;
        msg_processor_get_block_intake_stats(rpc_net_get_msg_processor(),
                                             &intake);
        s->block_intake_running = intake.running;
        s->block_intake_stopping = intake.stopping;
        s->block_intake_current_depth = intake.current_depth;
        s->block_intake_capacity = intake.capacity;
        s->block_intake_max_depth = intake.max_depth;
        s->block_intake_enqueued = intake.enqueued;
        s->block_intake_processed = intake.processed;
        s->block_intake_accepted = intake.accepted;
        s->block_intake_retryable = intake.retryable;
        s->block_intake_rejected = intake.rejected;
        s->block_intake_dropped = intake.dropped;
        s->block_intake_clone_failed = intake.clone_failed;
        s->block_intake_spawn_failed = intake.spawn_failed;
        if (intake.running && !intake.stopping && intake.capacity > 0 &&
            intake.current_depth >= intake.capacity)
            agent_fast_add_warning(s, "block_intake_saturated");
        if (intake.clone_failed > 0 || intake.spawn_failed > 0)
            agent_fast_add_warning(s, "block_intake_fault");
    }

    s->tip_advance_age_seconds = sync_monitor_tip_advance_age();
    if (s->tip_advance_age_seconds > 600 &&
        s->sync_state != SYNC_AT_TIP)
        agent_fast_add_warning(s, "tip_advance_stale");

    agent_fast_collect_errors(s);

    s->validation_pack_ok =
        invariant_sentinel_healthy(s->validation_pack_detail,
                                   (int)sizeof(s->validation_pack_detail));
    if (!s->validation_pack_ok)
        agent_fast_add_warning(s, s->validation_pack_detail[0]
                                  ? s->validation_pack_detail
                                  : "validation_pack");

    s->target_height = s->tip_height > s->served_height
        ? s->tip_height : s->served_height;
    if (s->header_height > s->target_height)
        s->target_height = s->header_height;
    if (s->peer_best_height > s->target_height)
        s->target_height = s->peer_best_height;
    s->gap = s->target_height > s->served_height
        ? s->target_height - s->served_height : 0;
    s->index_gap = s->tip_height > s->served_height
        ? s->tip_height - s->served_height : 0;
    s->serving = s->served_height > 0;

    char operator_detail[96] = {0};
    bool frontier_recovered =
        s->serving && s->has_peers && s->gap <= 1 &&
        (s->log_head_gap < 0 || s->log_head_gap <= 1);
    s->operator_latch_recovered =
        alerts_operator_needed_clear_if_chain_advance_recovered(
            frontier_recovered, operator_detail, sizeof(operator_detail),
            NULL);
    if (s->operator_latch_recovered)
        agent_fast_add_warning(s, "operator_latch_recovered");
    s->operator_needed =
        alerts_operator_needed(operator_detail, sizeof(operator_detail), NULL);

    s->catchup_active = s->in_flight > 0 || s->queued > 0;
    s->catchup_stalled =
        s->gap > 1 && s->catchup_active &&
        s->tip_advance_age_seconds >= AGENT_CATCHUP_STALL_SECS;
    s->download_dispatch_idle =
        s->gap > 1 && s->queued > 0 && s->in_flight == 0;
    s->download_dispatch_stalled =
        s->download_dispatch_idle &&
        s->tip_advance_age_seconds >= AGENT_DISPATCH_IDLE_SECS;
    if (s->catchup_stalled) {
        s->catchup_stall_seconds = s->tip_advance_age_seconds;
        agent_fast_add_warning(s, "catchup_stalled");
    }
    if (s->download_dispatch_idle) {
        s->download_dispatch_idle_seconds =
            s->tip_advance_age_seconds >= 0 ? s->tip_advance_age_seconds : 0;
        agent_fast_add_warning(s, "download_dispatch_idle");
    }
    if (s->overdue_in_flight > 0)
        agent_fast_add_warning(s, "download_timeouts_overdue");

    s->healthy = s->serving && s->has_peers && !s->operator_needed &&
                 s->gap <= 1 &&
                 (s->log_head_gap < 0 || s->log_head_gap <= 1);

    if (s->operator_needed) {
        snprintf(s->blocking_reason, sizeof(s->blocking_reason),
                 "operator_needed:%s",
                 operator_detail[0] ? operator_detail : "unspecified");
    } else if (!s->has_peers) {
        snprintf(s->blocking_reason, sizeof(s->blocking_reason),
                 "no_peers");
    } else if (s->catchup_stalled) {
        snprintf(s->blocking_reason, sizeof(s->blocking_reason),
                 "catchup_stalled");
    } else if (s->download_dispatch_stalled) {
        snprintf(s->blocking_reason, sizeof(s->blocking_reason),
                 "download_dispatch_idle");
    } else if (s->gap > 1 && s->in_flight == 0 && s->queued == 0) {
        snprintf(s->blocking_reason, sizeof(s->blocking_reason),
                 "download_queue_idle");
    } else if (s->log_head < 0) {
        snprintf(s->blocking_reason, sizeof(s->blocking_reason),
                 "log_head_unknown");
    }

    s->tor_enabled = tor_integration_is_enabled();
    s->tor_ready = tor_integration_is_ready();
    s->onion_service_ready = onion_service_get_address() != NULL;
}

bool rpc_agent_summary(const struct json_value *params, bool help,
                       struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agent\n"
        "\nReturn the compact first-check node summary used by REST "
        "/api/v1/agent and MCP zcl_agent.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.public_status.v1\", \"status\":\"healthy\", "
        "\"height\":N, \"gap\":0, \"primary_blocker\":\"none\" }\n");

    struct agent_fast_snapshot health;
    agent_fast_collect(&health);
    bool material_gap = health.gap > 1;

    const char *status = "healthy";
    const char *primary = "none";
    const char *next = "none";
    const char *summary = "node healthy at served frontier";
    bool operator_needed = false;

    if (health.operator_needed) {
        status = "blocked";
        primary = health.blocking_reason[0] ? health.blocking_reason
                                            : "operator_needed";
        next = "zclassic23 healthcheck";
        summary = "node has an active health blocker";
        operator_needed = true;
    } else if (!health.has_peers) {
        status = "blocked";
        primary = "no_peers";
        next = "zclassic23 getpeerinfo";
        summary = "node has no connected peers";
        operator_needed = true;
    } else if (material_gap && health.catchup_stalled) {
        status = "degraded";
        primary = "catchup_stalled";
        next = "zclassic23 getsyncdiag";
        summary = "node is behind and catch-up has not advanced recently";
        operator_needed = true;
    } else if (material_gap && health.download_dispatch_stalled) {
        status = "degraded";
        primary = "download_dispatch_idle";
        next = "zclassic23 getsyncdiag";
        summary = "node has queued block downloads but no in-flight requests";
        operator_needed = true;
    } else if (material_gap &&
               (health.in_flight > 0 || health.queued > 0)) {
        status = "catching_up";
        primary = "chain_gap";
        next = "zclassic23 downloadstats";
        summary = "node is downloading blocks toward the best known tip";
    } else if (material_gap) {
        status = "degraded";
        primary = "download_queue_idle";
        next = "zclassic23 getsyncdiag";
        summary = "node is behind the best known tip without active downloads";
        operator_needed = true;
    } else if (!health.healthy) {
        status = "degraded";
        primary = health.blocking_reason[0] ? health.blocking_reason
                                            : "healthcheck_unhealthy";
        next = "zclassic23 healthcheck";
        summary = "node health checks are degraded";
        operator_needed = health.warning_count > 0;
    }

    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.public_status.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", status);
    json_push_kv_bool(result, "healthy", strcmp(status, "healthy") == 0);
    json_push_kv_bool(result, "serving", health.serving);
    json_push_kv_bool(result, "operator_needed", operator_needed);
    json_push_kv_str(result, "summary", summary);
    json_push_kv_str(result, "primary_blocker", primary);
    json_push_kv_str(result, "next", next);
    agent_push_operator_lane_json(result, "operator_lane");
    json_push_kv_int(result, "height", health.served_height);
    json_push_kv_int(result, "served_height", health.served_height);
    json_push_kv_int(result, "indexed_height", health.tip_height);
    json_push_kv_int(result, "header_height", health.header_height);
    json_push_kv_int(result, "peer_best_height", health.peer_best_height);
    json_push_kv_int(result, "target_height", health.target_height);
    json_push_kv_int(result, "gap", health.gap);
    json_push_kv_int(result, "index_gap", health.index_gap);
    json_push_kv_str(result, "sync_state", sync_state_name(health.sync_state));

    struct json_value reducer = {0};
    json_set_object(&reducer);
    json_push_kv_int(&reducer, "log_head", health.log_head);
    json_push_kv_int(&reducer, "log_head_gap", health.log_head_gap);
    json_push_kv_int(&reducer, "tip_advance_age_seconds",
                     health.tip_advance_age_seconds);
    json_push_kv_bool(&reducer, "validation_pack_ok",
                      health.validation_pack_ok);
    json_push_kv_str(&reducer, "validation_pack_detail",
                     health.validation_pack_detail);
    json_push_kv(result, "reducer", &reducer);
    json_free(&reducer);

    struct json_value health_obj = {0};
    json_set_object(&health_obj);
    json_push_kv_int(&health_obj, "warning_count",
                     (int64_t)health.warning_count);
    json_push_kv_str(&health_obj, "warning_reasons",
                     health.warning_reasons);
    json_push_kv_int(&health_obj, "last_error_age_seconds",
                     health.last_error_age_seconds);
    json_push_kv_str(&health_obj, "last_error_type",
                     health.last_error_type);
    json_push_kv_str(&health_obj, "blocking_reason",
                     health.blocking_reason);
    json_push_kv_bool(&health_obj, "operator_latch_recovered",
                      health.operator_latch_recovered);
    json_push_kv(result, "health", &health_obj);
    json_free(&health_obj);

    struct json_value peers = {0};
    json_set_object(&peers);
    json_push_kv_int(&peers, "total", (int64_t)health.peer_count);
    json_push_kv_bool(&peers, "has_peers", health.has_peers);
    json_push_kv_int(&peers, "magicbean",
                     (int64_t)health.magicbean_peer_count);
    json_push_kv_int(&peers, "zclassic23",
                     (int64_t)health.zclassic_c23_peer_count);
    json_push_kv(result, "peers", &peers);
    json_free(&peers);

    struct json_value download = {0};
    json_set_object(&download);
    json_push_kv_int(&download, "requested",
                     (int64_t)health.blocks_requested);
    json_push_kv_int(&download, "received",
                     (int64_t)health.blocks_received);
    json_push_kv_int(&download, "timed_out",
                     (int64_t)health.blocks_timed_out);
    json_push_kv_int(&download, "in_flight", (int64_t)health.in_flight);
    json_push_kv_int(&download, "queued", (int64_t)health.queued);
    json_push_kv_bool(&download, "active", health.catchup_active);
    json_push_kv_bool(&download, "catchup_stalled",
                      health.catchup_stalled);
    json_push_kv_int(&download, "catchup_stall_seconds",
                     health.catchup_stall_seconds);
    json_push_kv_bool(&download, "dispatch_idle",
                      health.download_dispatch_idle);
    json_push_kv_bool(&download, "dispatch_stalled",
                      health.download_dispatch_stalled);
    json_push_kv_int(&download, "dispatch_idle_seconds",
                     health.download_dispatch_idle_seconds);
    json_push_kv_int(&download, "request_timeout_seconds",
                     (int64_t)health.request_timeout_seconds);
    json_push_kv_int(&download, "oldest_in_flight_age_seconds",
                     health.oldest_in_flight_age_seconds);
    json_push_kv_int(&download, "oldest_in_flight_height",
                     health.oldest_in_flight_height);
    json_push_kv_int(&download, "oldest_in_flight_peer_id",
                     (int64_t)health.oldest_in_flight_peer_id);
    json_push_kv_int(&download, "overdue_in_flight",
                     (int64_t)health.overdue_in_flight);
    json_push_kv_int(&download, "in_flight_peer_count",
                     (int64_t)health.in_flight_peer_count);
    json_push_kv_int(&download, "assign_attempts",
                     (int64_t)health.assign_attempts);
    json_push_kv_int(&download, "assign_successes",
                     (int64_t)health.assign_successes);
    json_push_kv_int(&download, "assign_zero_results",
                     (int64_t)health.assign_zero_results);
    json_push_kv_int(&download, "dispatch_wakes",
                     (int64_t)health.dispatch_wakes);
    json_push_kv_int(&download, "message_cycles",
                     (int64_t)health.message_cycles);
    json_push_kv_int(&download, "message_nodes_snapshotted",
                     (int64_t)health.message_nodes_snapshotted);
    json_push_kv_int(&download, "message_send_calls",
                     (int64_t)health.message_send_calls);
    json_push_kv_int(&download, "message_process_calls",
                     (int64_t)health.message_process_calls);
    json_push_kv_int(&download, "message_recv_ready",
                     (int64_t)health.message_recv_ready);
    json_push_kv_int(&download, "message_idle_waits",
                     (int64_t)health.message_idle_waits);
    json_push_kv_int(&download, "message_wakes",
                     (int64_t)health.message_wakes);
    {
        struct json_value intake = {0};
        json_set_object(&intake);
        json_push_kv_bool(&intake, "running",
                          health.block_intake_running);
        json_push_kv_bool(&intake, "stopping",
                          health.block_intake_stopping);
        json_push_kv_int(&intake, "current_depth",
                         (int64_t)health.block_intake_current_depth);
        json_push_kv_int(&intake, "capacity",
                         (int64_t)health.block_intake_capacity);
        json_push_kv_bool(&intake, "saturated",
                          health.block_intake_running &&
                          !health.block_intake_stopping &&
                          health.block_intake_capacity > 0 &&
                          health.block_intake_current_depth >=
                              health.block_intake_capacity);
        json_push_kv_int(&intake, "max_depth",
                         (int64_t)health.block_intake_max_depth);
        json_push_kv_int(&intake, "enqueued",
                         (int64_t)health.block_intake_enqueued);
        json_push_kv_int(&intake, "processed",
                         (int64_t)health.block_intake_processed);
        json_push_kv_int(&intake, "accepted",
                         (int64_t)health.block_intake_accepted);
        json_push_kv_int(&intake, "retryable",
                         (int64_t)health.block_intake_retryable);
        json_push_kv_int(&intake, "rejected",
                         (int64_t)health.block_intake_rejected);
        json_push_kv_int(&intake, "dropped",
                         (int64_t)health.block_intake_dropped);
        json_push_kv_int(&intake, "clone_failed",
                         (int64_t)health.block_intake_clone_failed);
        json_push_kv_int(&intake, "spawn_failed",
                         (int64_t)health.block_intake_spawn_failed);
        json_push_kv(&download, "block_intake", &intake);
        json_free(&intake);
    }
    json_push_kv_int(&download, "last_assign_peer_id",
                     (int64_t)health.last_assign_peer_id);
    json_push_kv_int(&download, "last_assign_max_requested",
                     (int64_t)health.last_assign_max_requested);
    json_push_kv_int(&download, "last_assign_available",
                     (int64_t)health.last_assign_available);
    json_push_kv_int(&download, "last_assign_assigned",
                     (int64_t)health.last_assign_assigned);
    json_push_kv_int(&download, "last_assign_queue_len",
                     (int64_t)health.last_assign_queue_len);
    json_push_kv_int(&download, "last_assign_active",
                     (int64_t)health.last_assign_active);
    json_push_kv_int(&download, "last_assign_peer_in_flight",
                     (int64_t)health.last_assign_peer_in_flight);
    json_push_kv_int(&download, "last_assign_peer_limit",
                     (int64_t)health.last_assign_peer_limit);
    json_push_kv_int(&download, "last_assign_global_limit",
                     (int64_t)health.last_assign_global_limit);
    json_push_kv_str(&download, "last_assign_result",
                     dl_assign_result_name(health.last_assign_result));
    json_push_kv_int(&download, "bytes_received",
                     (int64_t)health.download_bytes_received);
    json_push_kv_real(&download, "mbps_avg", health.download_mbps_avg);
    json_push_kv(result, "download", &download);
    json_free(&download);

    struct json_value services = {0};
    json_set_object(&services);
    json_push_kv_bool(&services, "tor_enabled", health.tor_enabled);
    json_push_kv_bool(&services, "tor_ready", health.tor_ready);
    json_push_kv_bool(&services, "onion_service_ready",
                      health.onion_service_ready);
    json_push_kv(result, "services", &services);
    json_free(&services);

    return true;
}
