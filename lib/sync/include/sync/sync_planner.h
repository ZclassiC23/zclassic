/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SYNC_PLANNER_H
#define ZCL_SYNC_PLANNER_H

#include "core/uint256.h"
#include "event/event.h"
#include "primitives/block.h"
#include "sync/sync_state.h"
#include "util/result.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct active_chain;
struct block_index;
struct download_manager;
struct main_state;
struct p2p_node;

enum sync_header_request_anchor {
    SYNC_HEADER_REQUEST_TIP = 0,
    SYNC_HEADER_REQUEST_TIP_PARENT = 1,
    SYNC_HEADER_REQUEST_EXPLICIT = 2,
};

enum sync_header_log_mode {
    SYNC_HEADER_LOG_NONE = 0,
    SYNC_HEADER_LOG_IBD = 1,
    SYNC_HEADER_LOG_TIP = 2,
};

struct sync_needed_blocks {
    bool chains_from_tip;
    bool should_activate_chain;
    size_t count;
};

struct sync_header_batch {
    bool should_warn_all_rejected;
    bool should_emit_received;
    bool should_request_more_headers;
};

struct sync_header_download_plan {
    bool has_candidate;
    bool should_begin_blocks_download;
    struct sync_needed_blocks needed_blocks;
};

struct sync_header_processing_plan {
    struct sync_header_batch batch;
    bool should_scan_block_files;
    struct sync_header_download_plan download;
    bool should_set_sync_state;
    enum sync_state next_sync_state;
    bool should_queue_needed_blocks;
    size_t queue_count;
    bool should_activate_chain;
};

struct sync_chain_activation {
    bool should_activate;
};

struct sync_getheaders_action {
    bool should_send;
    enum sync_header_request_anchor anchor;
    bool should_log;
};

struct sync_progress_snapshot {
    enum sync_state sync_state;
    int chain_height;
    int header_height;
    uint64_t requested;
    uint64_t received;
    uint64_t timed_out;
    uint64_t in_flight;
    uint64_t queued;
    uint64_t total_bytes;
    double mbps_avg;
    double gib_received;
    bool should_log_progress;
    bool tip_stale;
    int64_t tip_stale_seconds;
};

struct sync_stall_recovery {
    bool should_recover;
    bool should_log;
    bool should_reset_tip_next;
    bool should_request_tip_parent;
    int chain_height;
    int next_height;
    size_t entries_at_next;
    size_t entries_with_data;
    size_t entries_failed;
    struct uint256 *alt_hashes;
    int32_t *alt_heights;
    size_t alt_count;
};

struct sync_next_block_download {
    bool attempted;
    bool queued;
    int height;
    unsigned int status_before;
    unsigned int status_after;
    char reason[64];
    struct uint256 hash;
};

struct sync_block_assignment {
    bool should_assign;
    size_t max_assign;
};

struct sync_block_batch {
    bool should_assign;
    size_t in_flight_before;
    size_t assigned;
};

struct sync_block_acceptance {
    bool should_request_headers_retry;
    bool reached_peer_tip;
    bool should_emit_tip_updated;
    bool should_set_sync_state;
    enum sync_state next_sync_state;
    bool should_set_flush_policy;
    bool should_update_peer_state;
    enum peer_state next_peer_state;
};

bool syncsvc_should_begin_peer_sync(const struct p2p_node *node,
                                    int our_height,
                                    int best_header_height,
                                    enum sync_state sync_state);
bool syncsvc_should_mark_peer_caught_up(const struct p2p_node *node,
                                        int our_height,
                                        int best_header_height);
bool syncsvc_begin_peer_sync(struct p2p_node *node,
                             int our_height,
                             int best_header_height);
void syncsvc_collect_needed_blocks(struct sync_needed_blocks *result,
                                   const struct block_index *candidate,
                                   const struct block_index *tip,
                                   int our_height,
                                   struct uint256 *hashes,
                                   int32_t *heights,
                                   size_t max_collect);
void syncsvc_evaluate_header_batch(struct sync_header_batch *result,
                                   size_t accepted,
                                   uint64_t total_count,
                                   const struct block_index *last_header);
void syncsvc_plan_header_download(struct sync_header_download_plan *plan,
                                  enum sync_state sync_state,
                                  const struct block_index *candidate,
                                  const struct block_index *tip,
                                  int our_height,
                                  struct uint256 *hashes,
                                  int32_t *heights,
                                  size_t max_collect);
void syncsvc_plan_header_processing(struct sync_header_processing_plan *plan,
                                    size_t accepted,
                                    uint64_t total_count,
                                    const struct block_index *last_header,
                                    enum sync_state sync_state,
                                    const struct block_index *candidate,
                                    const struct block_index *tip,
                                    int our_height,
                                    struct uint256 *hashes,
                                    int32_t *heights,
                                    size_t max_collect);
bool syncsvc_should_restart_headers_from_tip(size_t accepted,
                                             const struct block_index *last_header,
                                             int our_height,
                                             int peer_height);
void syncsvc_build_block_file_scan_activation(
    struct sync_chain_activation *result,
    int scanned_blocks);
void syncsvc_build_header_processing_activation(
    struct sync_chain_activation *result,
    const struct sync_header_processing_plan *plan);
bool syncsvc_should_log_accepted_headers(const struct p2p_node *node,
                                         const struct block_index *header_tip);
bool syncsvc_is_initial_block_download(const struct p2p_node *node,
                                       int our_height);
bool syncsvc_should_request_headers(const struct p2p_node *node,
                                    int our_height,
                                    int64_t now_seconds);
void syncsvc_plan_periodic_getheaders(struct sync_getheaders_action *action,
                                      const struct p2p_node *node,
                                      int our_height,
                                      int64_t now_seconds);
void syncsvc_note_headers_requested(struct p2p_node *node,
                                    int64_t now_seconds);
/* Pass the count of NEW-to-index headers (newly_added), never the raw
 * accepted count — accepted includes already-known headers a peer
 * could replay to stay "useful" forever. */
void syncsvc_note_headers_received(struct p2p_node *node,
                                   size_t newly_added);
bool syncsvc_should_scan_block_files_after_headers(size_t accepted,
                                                   const struct block_index *header_tip);
enum sync_header_log_mode syncsvc_header_log_mode(
    const struct p2p_node *node,
    const struct block_index *tip,
    bool in_ibd);
bool syncsvc_should_activate_after_block_file_scan(int scanned_blocks);
bool syncsvc_should_activate_after_header_processing(
    const struct sync_header_processing_plan *plan);
bool syncsvc_should_release_snapshot_anchor(
    const struct block_index *anchor,
    const struct block_index *header_tip);
bool syncsvc_should_begin_blocks_download(enum sync_state sync_state,
                                          const struct block_index *candidate,
                                          int our_height);
bool syncsvc_headers_chain_from_tip(const struct block_index *candidate,
                                    const struct block_index *tip,
                                    int our_height);
struct zcl_result syncsvc_build_getheaders_locator(struct block_locator *loc,
                                      const struct active_chain *chain,
                                      const struct block_index *from,
                                      const struct uint256 *genesis_hash);

#define HEADER_STALL_TIMEOUT_SECS 120

/* Returns false (keep the peer) when best_header_height has reached
 * the peer's handshake-claimed tip minus 144 — at frontier parity no
 * peer can deliver "useful" headers faster than block cadence, so the
 * stale-disconnect would be pure churn. */
bool syncsvc_should_disconnect_stale_header_peer(const struct p2p_node *node,
                                                  int our_height,
                                                  int best_header_height,
                                                  int64_t now_seconds);
bool syncsvc_is_header_sync_stalled(enum sync_state state,
                                    int best_header_height,
                                    int64_t last_advance_time,
                                    int64_t now_seconds);
bool syncsvc_should_request_headers_with_fallback(const struct p2p_node *node,
                                                   int our_height,
                                                   int64_t now_seconds,
                                                   bool header_stall_active);

void syncsvc_plan_invalid_block_getheaders(struct sync_getheaders_action *action,
                                           enum sync_state sync_state);
void syncsvc_plan_block_assignment(struct sync_block_assignment *plan,
                                   const struct p2p_node *node,
                                   size_t in_flight);
void syncsvc_assign_peer_blocks(struct sync_block_batch *batch,
                                struct download_manager *dm,
                                const struct p2p_node *node,
                                struct uint256 *out_hashes,
                                size_t out_cap);
void syncsvc_note_valid_block(struct sync_block_acceptance *result,
                              const struct p2p_node *node,
                              enum sync_state sync_state,
                              int new_tip_height,
                              int best_header_height,
                              uint32_t new_tip_time,
                              int max_peer_height);
void syncsvc_collect_progress(struct sync_progress_snapshot *snapshot,
                              struct download_manager *dm,
                              enum sync_state sync_state,
                              int chain_height,
                              int header_height,
                              int64_t peer_last_block_time,
                              int64_t now_seconds);
bool syncsvc_build_stall_recovery(struct sync_stall_recovery *recovery,
                                  const struct main_state *ms,
                                  const struct p2p_node *node,
                                  uint64_t queued,
                                  uint64_t in_flight,
                                  int64_t now_seconds);
enum sync_header_request_anchor syncsvc_recovery_header_anchor(
    const struct sync_stall_recovery *recovery,
    const struct block_index *tip);
void syncsvc_plan_recovery_getheaders(struct sync_getheaders_action *action,
                                      const struct sync_stall_recovery *recovery,
                                      const struct block_index *tip);
void syncsvc_apply_stall_recovery(const struct sync_stall_recovery *recovery,
                                  struct main_state *ms,
                                  struct download_manager *dm,
                                  int *cleared_blocks);
bool syncsvc_queue_next_block_download(struct sync_next_block_download *download,
                                       struct main_state *ms,
                                       struct download_manager *dm);
bool syncsvc_should_warn_tip_stale(
    const struct sync_progress_snapshot *snapshot,
    const struct p2p_node *node,
    int64_t now_seconds);
void syncsvc_plan_tip_stale_getheaders(struct sync_getheaders_action *action,
                                       const struct sync_progress_snapshot *snapshot,
                                       const struct p2p_node *node,
                                       int64_t now_seconds);
void syncsvc_free_stall_recovery(struct sync_stall_recovery *recovery);

#endif /* ZCL_SYNC_PLANNER_H */
