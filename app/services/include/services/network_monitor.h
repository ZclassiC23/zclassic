/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * network_monitor — the node's "eyes on the game". The node is always trying
 * to be on the best (highest-work valid) zclassic chain; it cannot play a game
 * it cannot see, so a supervised sampler continuously observes every reachable
 * peer's advertised chain (best height, learnable tip hash, version, latency),
 * retains that history (peer_chain_observations model), and folds the latest
 * per-peer sample into a CONSENSUS VIEW: the modal tip, the max advertised
 * height, fork clusters (peers grouped by tip hash at one height), and OUR
 * delta from the best advertised chain.
 *
 * OBSERVATIONAL ONLY. This never changes chain selection — find_most_work_chain
 * stays authoritative. It adds no P2P message types (peer heights arrive on
 * existing handshake/header paths). Its value is redundant, always-loud
 * detection of "are we falling behind / is there a fork / are we partitioned".
 */

#ifndef ZCL_SERVICES_NETWORK_MONITOR_H
#define ZCL_SERVICES_NETWORK_MONITOR_H

#include "models/peer_chain_observation.h"
#include "util/result.h"
#include <stdbool.h>
#include <stdint.h>

struct node_db;
struct json_value;

enum {
    NM_MAX_PEERS = 256,          /* bounded per-sample peer cap */
    NM_MAX_FORK_CLUSTERS = 16,   /* bounded distinct (height,tip_hash) clusters */
    NM_MAX_HEADER_VOTES = 256    /* bounded peer_id -> latest (height,hash) map */
};

/* Minimum peers agreeing on each of two distinct tip hashes at the same height
 * before it is called a fork (not one lagging/lying peer). */
#define NM_FORK_MIN_CLUSTER 2

struct network_fork_cluster {
    int64_t height;
    char tip_hash[PEER_OBS_TIP_HEX + 1];
    int peer_count;
};

/* A folded snapshot of what the reachable network says the chain is. */
struct network_consensus_view {
    bool ready;                 /* false until the first sample completes */
    int64_t computed_at;        /* unix secs of this fold */
    int num_peers;              /* peers in the sample */
    int peers_with_height;      /* peers advertising a valid best height */
    int64_t modal_height;       /* most common advertised best height (-1 none) */
    int modal_height_count;     /* peers at modal_height */
    int64_t max_height;         /* max advertised best height (-1 none) */
    int64_t our_height;         /* our active-chain height */
    int64_t delta;              /* max_height - our_height (0 if unknown) */

    int num_clusters;           /* distinct (height,tip_hash) clusters observed */
    struct network_fork_cluster clusters[NM_MAX_FORK_CLUSTERS];

    bool fork_detected;         /* two clusters at one height, each >= min size */
    int64_t fork_height;        /* height of the detected fork (-1 none) */
    char fork_hash_a[PEER_OBS_TIP_HEX + 1];
    char fork_hash_b[PEER_OBS_TIP_HEX + 1];
    int fork_count_a;
    int fork_count_b;
};

/* ── Pure fold (unit-testable with synthetic observations) ──────────────
 * Compute the consensus view from an array of per-peer observations (one per
 * peer — the latest sample). our_height is our active-chain height (-1 if
 * unknown). Deterministic; no clock/IO except stamping computed_at from the
 * caller-independent `now_unix`. */
void network_monitor_compute_view(const struct db_peer_chain_observation *obs,
                                  int n, int64_t our_height, int64_t now_unix,
                                  struct network_consensus_view *out);

/* ── Runtime lifecycle ──────────────────────────────────────────────── */
struct network_monitor_config {
    int sample_interval_secs;   /* default 30 */
    int retain_rows;            /* default 10000 */
};
void network_monitor_config_defaults(struct network_monitor_config *cfg);

/* Start/stop the supervised sampler thread. db owns the retained history. */
struct zcl_result network_monitor_start(const struct network_monitor_config *cfg,
                                        struct node_db *db);
void network_monitor_stop(void);

/* Copy the latest folded view. Returns false if no sample has completed yet. */
bool network_monitor_get_view(struct network_consensus_view *out);

/* Feed a per-peer (height, tip hash) learned from an EXISTING message path
 * (an accepted-headers batch — see config/src/boot_msg_callbacks.c). Bounded
 * map; no new wire message. hash_hex is 64 hex chars + NUL. */
void network_monitor_note_peer_header(uint32_t peer_id, int height,
                                      const char hash_hex[65]);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
bool network_monitor_dump_state_json(struct json_value *out, const char *key);

#ifdef ZCL_TESTING
/* Force a single sample+fold now (uses the live connman), for tests that have
 * a running connman. Returns false if the monitor is not started. */
bool network_monitor_test_sample_once(void);
/* Reset the in-RAM header-vote map and last view. */
void network_monitor_test_reset(void);
/* Inject a folded view (marks it ready) so condition detectors can be unit
 * tested without a live connman. */
void network_monitor_test_set_view(const struct network_consensus_view *v);
#endif

#endif /* ZCL_SERVICES_NETWORK_MONITOR_H */
