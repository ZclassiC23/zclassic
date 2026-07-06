/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_EVENT_AGENT_SUMMARY_INTERNAL_H
#define ZCL_CONTROLLERS_EVENT_AGENT_SUMMARY_INTERNAL_H

#include "controllers/agent_resources.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/node_health_service.h"
#include "sync/sync_state.h"
#include "util/alerts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct json_value;

struct agent_fast_snapshot {
    enum sync_state sync_state;
    int served_height;
    int tip_height;
    int indexed_height;
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
    int64_t peer_snapshot_age_seconds;
    bool peer_snapshot_available;
    bool peer_snapshot_stale;
    bool has_peers;
    bool healthy;
    bool serving;
    bool operator_needed;
    bool operator_action_required;
    bool operator_latch_recovered;
    bool operator_latch_suppressed_by_mirror;
    bool validation_pack_ok;
    bool provable_tip_published;
    bool tor_enabled;
    bool tor_ready;
    bool onion_service_ready;
    bool block_source_status_cached;
    bool catchup_active;
    bool catchup_stalled;
    bool projection_deferred;
    bool projection_catchup_active;
    bool download_dispatch_idle;
    bool download_dispatch_stalled;
    bool resources_collected;
    uint64_t blocks_requested;
    uint64_t blocks_received;
    uint64_t blocks_timed_out;
    uint64_t in_flight;
    uint64_t queued;
    uint64_t overdue_in_flight;
    uint64_t in_flight_peer_count;
    uint64_t queue_peer_avoid_count;
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
    int projection_height;
    int projection_catchup_height;
    int projection_catchup_target_height;
    int last_projection_deferred_height;
    int32_t oldest_in_flight_height;
    uint32_t oldest_in_flight_peer_id;
    int64_t projection_lag;
    int64_t projection_deferred_total;
    int64_t last_projection_deferred_time;
    int64_t projection_catchup_progress_age_seconds;
    int64_t projection_catchup_uptime_seconds;
    int64_t tip_advance_age_seconds;
    int64_t catchup_stall_seconds;
    int64_t download_dispatch_idle_seconds;
    int64_t queue_peer_avoid_max_seconds;
    int64_t oldest_in_flight_age_seconds;
    int64_t last_error_age_seconds;
    struct agent_resource_snapshot resources;
    struct legacy_mirror_sync_stats mirror;
    int active_condition_count;
    int unresolved_condition_count;
    int warning_count;
    char warning_reasons[256];
    char blocking_reason[ZCL_NODE_HEALTH_REASON_LEN];
    char operator_needed_detail[ALERT_OPERATOR_NEEDED_DETAIL_LEN];
    char last_error_type[64];
    char projection_state[32];
    char last_projection_deferred_reason[64];
    char validation_pack_detail[64];
    int64_t operator_needed_since_unix;
};

void agent_summary_push_detail_json(struct json_value *result,
                                    const struct agent_fast_snapshot *health,
                                    bool partial_result);

#endif
