/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_SYNC_MONITOR_H
#define ZCL_SERVICES_SYNC_MONITOR_H

#include "net/connman.h"
#include "net/download.h"
#include "sync/sync_state.h"
#include "util/result.h"
#include "validation/main_state.h"

#include <stdbool.h>
#include <stdint.h>

enum watchdog_recovery_type {
    WATCHDOG_NONE = 0,
    WATCHDOG_HEADER_STALL,
    WATCHDOG_HEADER_LAG,
    WATCHDOG_BLOCK_STALL,
    WATCHDOG_STATE_STUCK,
    WATCHDOG_REPEATED_RESTART,
    WATCHDOG_PEER_FLOOR,
    WATCHDOG_SYNC_VIOLATION,
    WATCHDOG_QUEUE_STARVED,
    WATCHDOG_LOCAL_HEADER_REFILL,
    WATCHDOG_BODY_FRONTIER_MISSING,
    WATCHDOG_SNAPSHOT_RESNAPSHOT,
};

struct watchdog_stats {
    int      checks_run;
    int      recoveries_total;
    int      escalation_level;
    double   blocks_per_sec;
    int64_t  last_recovery_time;
    enum watchdog_recovery_type last_recovery;
    int      last_recovery_local_height;
    int      last_recovery_peer_height;
    int      last_recovery_peer_count;
    int      last_recovery_target_height;
    int      last_recovery_manifest_height;
    char     last_recovery_reason[96];
    char     last_recovery_trigger[64];
};

struct watchdog_local_recovery_stats {
    bool    active;
    bool    mirror_repair_gated;
    bool    retries_exhausted;
    int     missing_height;
    int     retry_count;
    int     distinct_peer_count;
    int     peer_rotation_count;
    char    mode[32];
    char    last_reason[64];
};

void sync_monitor_init(void);
void sync_monitor_set_context(struct connman *cm,
                              struct download_manager *dm,
                              struct main_state *ms);
struct connman *sync_monitor_connman(void);
struct download_manager *sync_monitor_download_manager(void);
struct main_state *sync_monitor_main_state(void);

void sync_monitor_kick_local_sync(const char *reason);
struct zcl_result sync_monitor_queue_active_frontier_body(
    int target_height,
    const char *reason);
int sync_monitor_local_header_refill(struct connman *cm,
                                     int next_h,
                                     const char *reason);
bool sync_monitor_active_next_child_exists(struct main_state *ms,
                                           struct block_index *tip,
                                           int next_h);
void sync_monitor_get_local_recovery_stats(
    struct watchdog_local_recovery_stats *out);

void sync_monitor_on_block_connected(int height);
int64_t sync_monitor_tip_advance_age(void);
void sync_monitor_record_recovery(enum watchdog_recovery_type type,
                                  int local_height,
                                  int peer_height,
                                  int peer_count,
                                  const char *reason);
void sync_monitor_record_snapshot_resnapshot(int local_height,
                                             int peer_height,
                                             int peer_count,
                                             int target_height,
                                             int manifest_height,
                                             const char *trigger,
                                             const char *reason);
void sync_monitor_get_stats(struct watchdog_stats *out);

const char *watchdog_recovery_type_name(enum watchdog_recovery_type type);

#ifdef ZCL_TESTING
void sync_monitor_test_set_local_recovery(bool active,
                                          bool retries_exhausted,
                                          int missing_height,
                                          int retry_count,
                                          const char *mode);
/* Override the last-block-connected timestamp so tip_advance_age() returns a
 * deterministic value. Pass 0 to restore the "never connected" sentinel. */
void sync_monitor_test_set_tip_advance_ts(int64_t ts);
#endif

#endif /* ZCL_SERVICES_SYNC_MONITOR_H */
