/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_NODE_HEALTH_SERVICE_H
#define ZCL_NODE_HEALTH_SERVICE_H

#include "event/event.h"
#include "sync/sync_state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct node_db;
struct main_state;
struct cac_decision;

struct node_health_snapshot {
    enum sync_state sync_state;
    bool healthy;
    bool synced;
    bool has_peers;
    bool tor_enabled;
    bool tor_ready;
    bool onion_service_ready;
    bool tip_stale;
    bool queue_backed_up;
    size_t peer_count;
    int tip_height;
    int header_height;
    int peer_best_height;
    int tip_lag;
    /* Prime Directive health = network_tip − log_head, expressed as one
     * real number. `log_head` is the tip_finalize stage cursor (the
     * height the reducer has finalized through the event log);
     * `log_head_gap` = peer_best_height − log_head. The no-forward-progress
     * canary reads this. -1 when unknown. */
    int log_head;
    int log_head_gap;
    int64_t tip_stale_seconds;
    int64_t utxo_count;
    int64_t wal_size_bytes;
    int64_t uptime_seconds;
    int64_t db_last_activity_age_seconds;
    int error_total;
    int db_last_sqlite_rc;
    int64_t last_error_age_seconds;
    bool last_error_recent;
    char last_error[EVENT_PAYLOAD_SIZE + 1];
    char last_error_type[64];
    char degraded_reason[128];
    char onion_address[128];
    char db_last_op[64];
    bool db_open;
    bool db_tx_open;
    bool db_turbo_mode;
    bool db_service_started;
    bool db_service_worker_started;
    bool db_service_stop_requested;
    size_t db_service_queue_depth;
    int64_t db_service_uptime_seconds;
    bool catchup_active;
    int catchup_height;
    int catchup_target_height;
    int64_t catchup_uptime_seconds;
    int64_t catchup_progress_age_seconds;
    bool import_active;
    int import_rows_written;
    int64_t import_uptime_seconds;
    int64_t import_progress_age_seconds;
    uint64_t blocks_requested;
    uint64_t blocks_received;
    uint64_t blocks_timed_out;
    uint64_t in_flight;
    uint64_t queued;

    /* Memory */
    int64_t memory_rss_mb;

    /* Watchdog stats */
    int      wd_checks_run;
    int      wd_recoveries;
    double   wd_blocks_per_sec;
    int      wd_escalation_level;
    int64_t  wd_last_recovery_time;
    int      wd_last_recovery_type;       /* enum watchdog_recovery_type */
    int      wd_last_recovery_target_height;
    int      wd_last_recovery_manifest_height;
    char     wd_last_recovery_name[32];
    char     wd_last_recovery_reason[96];
    char     wd_last_recovery_trigger[64];

    /* Seconds since the most recent block-connect, or -1 if we have
     * not seen one yet (cold boot). Decoupled from sync_state so a
     * stall in HEADERS_DOWNLOAD with header_gap=0 surfaces as a real
     * staleness signal. Healthy gate flips false when this exceeds
     * TIP_ADVANCE_AGE_DEGRADED_SECS (600) AND peers>0 AND
     * sync_state!=SYNC_AT_TIP. */
    int64_t  tip_advance_age_seconds;

    /* Mirror lag SLO breach severity (none|warn|critical|fatal). When
     * "fatal" the snapshot.healthy gate flips false, which causes the
     * sd_notify heartbeat thread to skip pinging systemd's WatchdogSec
     * timer and triggers a service restart. Surfaced via zcl_health,
     * zcl_status, and Prometheus zcl_mirror_lag_breach_seconds. */
    int64_t  mirror_lag_blocks;
    int64_t  mirror_lag_breach_seconds;
    int64_t  mirror_lag_critical_seconds;
    char     mirror_lag_breach_severity[16];

    /* Magic Bean / zclassic23 peer classification: counts of currently
     * connected peers whose advertised subver matches the legacy
     * /MagicBean:.../ or the new /ZClassic-C23:.../ identifier. Goal 3
     * of the redundancy plan ("magic bean reporting"): operators can
     * see at a glance how many zclassicd-era peers are connecting to
     * us and how many native zclassic23 peers we have. */
    size_t   magicbean_peer_count;
    size_t   zclassic_c23_peer_count;

    /* Operator-needed latch (from lib/util/alerts.c). True once the
     * auto-healing condition engine exhausts remedies for a CRITICAL
     * problem and emits EV_OPERATOR_NEEDED — the "a halt can never be
     * silent" signal. Flips healthy=false and sets degraded_reason so
     * zcl_status shows it and the sd_notify heartbeat stops. Cleared
     * automatically when the underlying condition clears. */
    bool     operator_needed;
    char     operator_needed_detail[128];
};

void node_health_collect(struct node_health_snapshot *snapshot,
                         struct node_db *ndb,
                         const struct main_state *ms);
bool node_health_chain_advance_synced(const struct cac_decision *decision);

#endif
