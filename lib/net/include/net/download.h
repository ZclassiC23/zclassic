/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block download manager — coordinates parallel block downloads across
 * multiple peers during IBD. Prevents duplicate requests, enforces
 * timeouts, reassigns stalled blocks, and tracks per-peer performance.
 *
 * Architecture:
 *   - Single global instance (g_download_mgr), mutex-protected
 *   - Hash table of in-flight blocks: hash → (peer_id, request_time)
 *   - Sliding window: max N blocks in-flight per peer
 *   - Timeout: blocks not received within T seconds get reassigned
 *   - Stats: per-peer blocks received, latency, failures
 */

#ifndef ZCL_DOWNLOAD_H
#define ZCL_DOWNLOAD_H

#include "core/uint256.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Tuning constants — conservative (at-tip) defaults.
 * During IBD, use dl_get_*() functions which return aggressive values. */
#define DL_MAX_IN_FLIGHT_PER_PEER 128    /* max concurrent block requests per peer */
#define DL_MAX_IN_FLIGHT_PER_LOOPBACK 512 /* loopback bypasses WAN-fairness ceiling */
#define DL_MAX_IN_FLIGHT_TOTAL    1024   /* max total in-flight blocks (at tip) */
#define DL_MAX_IN_FLIGHT_TOTAL_IBD 4096  /* max total in-flight blocks (during IBD) */
#define DL_REQUEST_TIMEOUT_SECS   30     /* reassign after this many seconds (at tip) */
#define DL_REQUEST_TIMEOUT_SECS_IBD 15   /* reassign after this many seconds (during IBD) */
#define DL_STALL_TIMEOUT_SECS     120    /* disconnect peer after this */
#define DL_WINDOW_SIZE            512    /* blocks to request per batch */

/* Dynamic limits — return aggressive values during IBD, conservative at tip.
 * These check sync_get_state() internally. Thread-safe. */
size_t dl_get_max_in_flight_total(void);
int    dl_get_request_timeout_secs(void);

/* Per-block in-flight entry */
struct dl_in_flight {
    struct uint256 hash;
    int32_t        height;          /* -1 if unknown */
    uint32_t       peer_id;
    int64_t        request_time;    /* seconds since epoch */
    bool           active;          /* true if slot in use */
};

/* Queued-hash membership entry (open addressing). state: 0 = virgin
 * (probe-chain terminator), 1 = live, 2 = tombstone (deleted; probe
 * continues through it). */
struct dl_queued_key {
    struct uint256 hash;
    uint8_t        state;
};

/* Per-peer download stats */
struct dl_peer_stats {
    uint32_t peer_id;
    uint32_t blocks_requested;
    uint32_t blocks_received;
    uint32_t blocks_timed_out;
    int64_t  last_block_time;       /* when last block arrived */
    int64_t  avg_delivery_us;       /* rolling average delivery time (EWMA) */
    uint32_t bandwidth_score;       /* adaptive score: higher = faster peer */
    bool     active;
    bool     is_loopback;           /* K2: peer at 127.0.0.0/8 or ::1; gets
                                     * DL_MAX_IN_FLIGHT_PER_LOOPBACK window
                                     * and bypasses bandwidth-score scaling */
};

/* Download manager — global singleton */
struct download_manager {
    zcl_mutex_t cs;

    /* In-flight hash table (open addressing, power-of-2 size) */
    struct dl_in_flight *slots;
    size_t               num_slots;     /* capacity (power of 2) */
    size_t               num_active;    /* current in-flight count */

    /* Per-peer stats */
    struct dl_peer_stats peers[256];
    size_t               num_peers;

    /* Download window: blocks we need but haven't requested yet */
    struct uint256      *queue;         /* block hashes to download */
    int32_t             *queue_heights; /* corresponding heights */
    size_t               queue_len;
    size_t               queue_cap;

    /* Membership set over `queue` (open addressing, power-of-2 size,
     * load < 50%). Makes the "already queued?" check O(1); the old
     * per-item linear scan was O(queue_len) and, with the queue pinned
     * at its 65536 cap during deep IBD, turned every bulk enqueue into
     * an O(n^2) grind that held `cs` for minutes and starved every
     * other thread touching the manager (2026-06-09 tracka wedge). */
    struct dl_queued_key *qset;
    size_t                qset_slots;   /* capacity (power of 2) */
    size_t                qset_live;    /* live entries == queue_len */
    size_t                qset_tombs;   /* tombstoned entries */

    /* Global stats */
    uint64_t total_requested;
    uint64_t total_received;
    uint64_t total_timed_out;
    uint64_t total_duplicate;
    uint64_t total_queue_evicted;   /* high-height entries displaced at cap */
    uint64_t total_queue_rejected;  /* pushes refused at cap (not lower
                                     * than the current tail) */

    /* Byte throughput tracking */
    uint64_t total_bytes_received;   /* total block bytes downloaded */
    int64_t  sync_start_time;        /* epoch seconds when first block received */
};

/* Initialize the download manager. Call once at startup. */
void dl_init(struct download_manager *dm);

/* Free all resources. */
void dl_free(struct download_manager *dm);

/* Check if a block is already in-flight (requested from any peer). */
bool dl_is_in_flight(struct download_manager *dm, const struct uint256 *hash);

/* Mark a block as requested from a specific peer.
 * Returns false if already in-flight or table full. */
bool dl_mark_requested(struct download_manager *dm,
                       const struct uint256 *hash, int32_t height,
                       uint32_t peer_id);

/* Mark a block as received (remove from in-flight).
 * Returns the peer_id that requested it, or 0 if not found. */
uint32_t dl_mark_received(struct download_manager *dm,
                          const struct uint256 *hash);

/* Check for timed-out requests. Returns number of blocks reassigned.
 * Timed-out blocks are moved back to the download queue.
 * Call periodically from send_messages. */
size_t dl_check_timeouts(struct download_manager *dm, int64_t now);

/* Get number of in-flight blocks for a specific peer. */
size_t dl_peer_in_flight(struct download_manager *dm, uint32_t peer_id);

/* Handle peer disconnect — re-queue all in-flight blocks from this peer.
 * Call from connman when a peer is disconnected. Returns count re-queued. */
size_t dl_peer_disconnected(struct download_manager *dm, uint32_t peer_id);

/* Add blocks to the download queue (blocks we need but haven't requested).
 * Deduplicates against already-in-flight and already-queued blocks. */
size_t dl_queue_blocks(struct download_manager *dm,
                       const struct uint256 *hashes,
                       const int32_t *heights,
                       size_t count);

/* Push a block to the FRONT of the queue (highest priority).
 * Used by reducer activation when it needs the next sequential block. */
void dl_queue_priority(struct download_manager *dm,
                       const struct uint256 *hash, int32_t height);

/* Assign queued blocks to a peer. Returns number assigned.
 * Respects DL_MAX_IN_FLIGHT_PER_PEER (or DL_MAX_IN_FLIGHT_PER_LOOPBACK
 * for peers flagged via dl_set_peer_loopback) and DL_MAX_IN_FLIGHT_TOTAL.
 * Fills `out_hashes` with the assigned block hashes. */
size_t dl_assign_to_peer(struct download_manager *dm,
                         uint32_t peer_id,
                         struct uint256 *out_hashes,
                         size_t max_assign);

/* K2: mark a peer as loopback so dl_assign_to_peer uses
 * DL_MAX_IN_FLIGHT_PER_LOOPBACK and bypasses bandwidth-score scaling.
 * Caller-set (the download manager doesn't see net addresses). Idempotent. */
void dl_set_peer_loopback(struct download_manager *dm,
                          uint32_t peer_id, bool is_loopback);

/* Update peer stats when a block is received from them. */
void dl_peer_block_received(struct download_manager *dm,
                            uint32_t peer_id, int64_t delivery_us);

/* Get download stats for RPC/diagnostics. */
void dl_get_stats(struct download_manager *dm,
                  uint64_t *requested, uint64_t *received,
                  uint64_t *timed_out, uint64_t *in_flight,
                  uint64_t *queued);

/* Record block bytes received (call from process_block_msg). */
void dl_add_bytes_received(struct download_manager *dm, uint64_t bytes);

/* Get byte throughput stats. */
void dl_get_throughput(struct download_manager *dm,
                       uint64_t *total_bytes, double *mbps_avg);

/* Get the adaptive per-peer window size based on bandwidth score.
 * Fast peers get up to DL_MAX_IN_FLIGHT_PER_PEER; slow peers get fewer.
 * Returns 0 if peer not found. */
size_t dl_peer_adaptive_window(struct download_manager *dm, uint32_t peer_id);

/* Global download manager accessor (initialized by msg_processor_init). */
struct download_manager *msg_get_download_mgr(void);

/* drain the queue + in-flight tracking under tip-stall
 * backpressure. Returns the number of (queued + in-flight) entries
 * dropped. Block bodies that arrive after the drain are no longer
 * tracked — dl_mark_received returns 0 — and are freed by the
 * normal net_message_free / block_already_seen paths. The header
 * chain in main_state is untouched. Safe to call from any thread. */
size_t dl_drain_for_backpressure(struct download_manager *dm);

#endif /* ZCL_DOWNLOAD_H */
