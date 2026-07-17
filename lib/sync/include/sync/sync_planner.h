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

/* Periodic catch-up -> AT_TIP decision.  The asynchronous reducer intake is
 * the normal BLOCKS_DOWNLOAD path, so the per-message synchronous acceptance
 * callback cannot be the sole owner of this transition (that callback is
 * bypassed until the FSM is already AT_TIP). */
struct sync_tip_state_evaluation {
    bool should_set_at_tip;
    int target_height;
    int served_gap;
    int local_gap;
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
/* `band_fill_in_progress` (from syncsvc_header_band_continue) vetoes the
 * restart unconditionally: a below-tip batch that extends the trust-rooted
 * frontier toward an installed-above-frontier island is PROGRESS — the
 * restart-from-tip policy was what kept the 2026-06-11 band hole
 * (3,140,573..3,143,301) permanently unrequested. */
bool syncsvc_should_restart_headers_from_tip(size_t accepted,
                                             const struct block_index *last_header,
                                             int our_height,
                                             int peer_height,
                                             bool band_fill_in_progress);

/* ── Header band backfill (installed-above-frontier hole) ───────────
 * Implemented app-side by header_band_service.c (same ownership split
 * as every other syncsvc_* planner in this contract). All facts are
 * derived from pprev contiguity to the compiled SHA3 anchor / genesis;
 * the band typed blocker is a loud cache, not an authority. */

/* O(1) predicate: true while the band hole is open (recorded via the
 * HEADER_BAND_BLOCKER_ID typed blocker — the same fact backfill_anchor
 * gates on). Consumed by the request-side getheaders interval planner
 * (header_sync_service.c, S8) to stay at the IBD cadence during backfill
 * even when active_chain_height already reads at the island-anchored tip
 * and every peer-comparison heuristic concludes "at tip". Reverts
 * automatically when syncsvc_header_band_after_batch clears the fact on
 * closure. */
bool syncsvc_header_band_hole_open(void);

/* True iff the active tip is a detached island AND `last_header` extends
 * the trust-rooted frontier below the island root — i.e. the batch is
 * band-fill progress that must suppress restart-from-tip and the
 * best-header skip. Records the band fact if absent, and advances the
 * band index-frontier cursor — even for an all-known (newly_added==0)
 * batch, whose tail still proves how far the index extends (defect #7:
 * without that advance the next anchor re-derives from stale slots and
 * the peer re-serves the same range forever). */
bool syncsvc_header_band_continue(const struct active_chain *chain,
                                  const struct block_index *last_header);

/* While the band fact is recorded: the contiguous-frontier block to
 * anchor periodic getheaders at (the peer forks there and serves the
 * band). The anchor is the HIGHER of the highest populated active-chain
 * slot and the index-frontier cursor, both re-verified trust-rooted +
 * below the island root at use (defect #7: slots are not populated by
 * header acceptance, so a slot-only anchor pins one batch behind the
 * index and livelocks). NULL when no band fact exists (O(1)), the band
 * has closed, or no servable frontier resolves. */
struct block_index *syncsvc_header_band_backfill_anchor(
    const struct active_chain *chain);

/* Test seam: clears the process-global band index-frontier cursor (unit
 * fixtures build chains on the stack — a cursor surviving a test would
 * dangle into a dead frame). Pairs with blocker_reset_for_testing(). */
void syncsvc_header_band_reset_for_testing(void);

/* Closure probe — call after every accepted header batch (outside the
 * full-batch gate: the final band batch can be <160). No-op without the
 * band fact. On closure: non-destructive in-memory chain[] slot-fill +
 * pskip + chainwork repropagation, then re-derive the band fact and only
 * then blocker clear + chain-evidence reconcile re-arm. Runs on the net
 * message thread, so it NEVER touches disk and never mutates shared
 * block_index ancestry (no chain_restore_finalize here — the boot disk
 * ladder rewrites pprev across millions of nodes that reducer/RPC read
 * lock-free, and band headers are bodyless so its disk walk always
 * degrades to a full blk*.dat scan). */
void syncsvc_header_band_after_batch(struct main_state *ms,
                                     const struct block_index *last_header);
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
/* A peer is treated as "behind" (and skipped for getheaders + block
 * getdata) only when its handshake-claimed tip is more than this many
 * blocks below ours.
 *
 * Why a large (wedge-scale) tolerance, not a tight one: node->starting_height
 * is HANDSHAKE-STATIC (msg_version.c:221; never updated). A peer that
 * connected when the chain was lower keeps a stale-low value even though it
 * has since followed the tip, so a tight gate would (a) suppress the 120s
 * at-tip keepalive getheaders and (b) demote a healthy long-lived peer.
 * New blocks from a healthy peer still reach us via BIP130 push
 * (sendheaders, msg_blocks.c:409-477), so the keepalive poll is only a
 * backstop — but we still keep it for any peer within this band.
 *
 * The threshold is set far above any plausible "connected-long-ago-at-tip"
 * gap so it NEVER trips on a healthy peer, yet is dwarfed by the live wedge
 * (peer at 3056758 while we are at 3150488 — a ~94k gap, ~94x this bound),
 * which is correctly excluded. Net policy only. */
#define SYNC_PEER_BEHIND_TOLERANCE 1000

/* True ONLY when we can PROVE this peer is substantially behind our height
 * (see SYNC_PEER_BEHIND_TOLERANCE). A peer with unknown advertised height
 * (starting_height < 0, e.g. mid-handshake or the co-located zclassicd
 * oracle before it reports its tip) is NEVER considered behind — header
 * sync is how we learn its real tip, so we keep it eligible. Mirrors
 * zclassicd FindNextBlocksToDownload (main.cpp:501) refusing peers whose
 * best-known work is below our tip. Net policy only; touches no block/tx/
 * header VALIDITY predicate. Used to skip getheaders rounds and block-
 * getdata slots spent on a wedged/behind peer (the live 3056758-vs-3150488
 * stall) so they go to a strictly-ahead peer instead. */
bool syncsvc_peer_is_behind(const struct p2p_node *node, int our_height);
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
                                   size_t in_flight,
                                   int our_height);
void syncsvc_assign_peer_blocks(struct sync_block_batch *batch,
                                struct download_manager *dm,
                                const struct p2p_node *node,
                                struct uint256 *out_hashes,
                                size_t out_cap,
                                int our_height);
void syncsvc_note_valid_block(struct sync_block_acceptance *result,
                              const struct p2p_node *node,
                              enum sync_state sync_state,
                              int new_tip_height,
                              int best_header_height,
                              uint32_t new_tip_time,
                              int max_peer_height);
void syncsvc_plan_periodic_tip_state(
    struct sync_tip_state_evaluation *result,
    enum sync_state sync_state,
    bool served_tip_published,
    int served_height,
    int local_height,
    int header_height,
    int peer_height,
    size_t peer_count,
    uint64_t queued,
    uint64_t in_flight,
    uint64_t intake_pending);
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
