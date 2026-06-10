/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_NET_CONNMAN_H
#define ZCL_NET_CONNMAN_H

#include "net/net.h"
#include "net/onion_discovery.h"
#include "chain/chainparams.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_ADDNODES 16

/* Initial capacity of the deferred-free list: starts at 256, grows
 * dynamically on overflow up to CONNMAN_DEFERRED_FREE_HARD_CAP. The fixed
 * cap-256 used to overflow under Tor-driven churn while the message
 * handler held snapshot refs, triggering the deliberate-leak fallback in
 * thread_socket_handler. grow the array instead. The hard
 * ceiling is 8× DEFAULT_MAX_PEER_CONNECTIONS = 1000, large enough that
 * hitting it indicates a genuine leak (and tripping the SIGABRT handler
 * is the right outcome). */
#define CONNMAN_DEFERRED_FREE_INIT_CAP 256
#define CONNMAN_DEFERRED_FREE_HARD_CAP 1000

enum connman_outbound_target_source {
    CONNMAN_TARGET_NONE = 0,
    CONNMAN_TARGET_ADDNODE,
    CONNMAN_TARGET_ADDRMAN,
};

enum connman_addnode_failure_kind {
    CONNMAN_ADDNODE_FAILURE_TCP = 0,
    CONNMAN_ADDNODE_FAILURE_PROTOCOL,
};

struct connman_known_peer {
    uint8_t ip[16];
    uint16_t port;
    uint64_t services;
};

typedef int (*connman_known_zcl23_peers_fn)(
    void *ctx,
    struct connman_known_peer *out,
    size_t max);

struct connman_outbound_health {
    size_t outbound_total;
    size_t inbound_total;
    size_t healthy;
    size_t inbound_healthy;
    size_t connecting;
    size_t handshake_incomplete;
    size_t inbound_handshake_incomplete;
    size_t ipv4_group_count;
    size_t ipv4_max_group_size;
    size_t healthy_ipv4_group_count;
    size_t healthy_ipv4_max_group_size;
    size_t addnode_count;
    size_t addnode_backoff_active;
    int addnode_backoff_max_sec;
    int64_t addnode_tcp_failures;
    int64_t addnode_protocol_failures;
};

struct connman {
    struct net_manager manager;
    const struct chain_params *params;
    bool started;
    bool dns_seed_thread_started;
    bool socket_thread_started;
    bool open_thread_started;
    bool message_thread_started;
    struct p2p_node **deferred_free;
    size_t num_deferred_free;
    size_t deferred_free_cap;
    /* Persistent addnode list — reconnected automatically on disconnect */
    struct net_address addnodes[MAX_ADDNODES];
    int num_addnodes;
    size_t next_addnode_cursor;
    int64_t addnode_last_attempt[MAX_ADDNODES];
    int addnode_backoff_sec[MAX_ADDNODES];
    int64_t addnode_tcp_failures[MAX_ADDNODES];
    int64_t addnode_protocol_failures[MAX_ADDNODES];
    /* Data directory for persisting addrman (peers.dat) */
    const char *datadir;
    const char *onion_peer_datadir;
    onion_peer_discover_fn onion_peer_discover;
    connman_known_zcl23_peers_fn known_zcl23_peers;
    void *known_zcl23_peers_ctx;
};

bool connman_init(struct connman *cm, const struct chain_params *params,
                   struct node_signals *signals);
bool connman_start(struct connman *cm);
void connman_signal_stop(struct connman *cm);
void connman_join(struct connman *cm);
void connman_stop(struct connman *cm);
void connman_free(struct connman *cm);

/* Persist addrman to {datadir}/peers.dat. Call on shutdown. */
void connman_save_addrman(struct connman *cm);

/* Load addrman from {datadir}/peers.dat. Call before connman_start. */
void connman_load_addrman(struct connman *cm);

void connman_add_seed_node(struct connman *cm, const char *host,
                            uint16_t port);
void connman_open_connection(struct connman *cm,
                              const struct net_address *addr);

/* Kick the seed-discovery loop: re-add fixed seeds + retry DNS resolve.
 * Safe to call from any thread; idempotent. In -connect mode this is a no-op:
 * explicit peers are the entire outbound universe. Used by the sync watchdog
 * when it detects a peer-floor breach or single-peer recovery state to widen
 * the addrman selection without waiting for the adaptive timer. */
void connman_kick_seed_discovery(struct connman *cm);

void connman_set_onion_peer_discovery(struct connman *cm,
                                      const char *datadir,
                                      onion_peer_discover_fn discover);

void connman_set_known_zcl23_peer_source(
    struct connman *cm,
    connman_known_zcl23_peers_fn peers,
    void *ctx);

size_t connman_get_node_count(const struct connman *cm);

/* Count of outbound peers in PEER_HANDSHAKE_COMPLETE or later. Used by
 * the sync watchdog to distinguish slot-burning peers stuck in
 * PEER_CONNECTING from peers actually able to serve us blocks. */
size_t connman_outbound_healthy_count(struct connman *cm);

/* Return the highest starting_height among all connected peers, or -1. */
int connman_max_peer_height(struct connman *cm);
void connman_get_outbound_health(struct connman *cm,
                                 struct connman_outbound_health *out);
int connman_force_outbound_rotation(struct connman *cm, const char *reason);

void connman_relay_transaction(struct connman *cm,
                                const struct uint256 *txid);

/* BIP 156 private relay for a locally originated tx. Holds the
 * serialized tx in the Dandelion stempool and announces it (inv type
 * MSG_DANDELION_TX) to this node's wallet-edge stem destination; the
 * tx enters the mempool when it fluffs (peer fluff, embargo expiry,
 * or destination loss — it cannot be lost). Returns false when
 * Dandelion is disabled, uninitialized, or no Dandelion-capable
 * outbound peer exists — caller falls back to mempool insert +
 * connman_relay_transaction. */
bool connman_relay_transaction_private(struct connman *cm,
                                       const struct uint256 *txid,
                                       const uint8_t *tx_bytes,
                                       size_t tx_size);

/* Stem-first relay for a locally originated tx that is ALREADY in the
 * wallet/mempool (wallet send paths commit to the mempool for
 * bookkeeping). Tries the private stem announcement; falls back to
 * normal inv diffusion. While under stem embargo the tx is hidden
 * from the P2P `mempool` reply and from MSG_TX getdata. */
struct transaction;
void connman_relay_transaction_local(struct connman *cm,
                                     const struct transaction *tx);

/* one pass of the message-handler loop body.
 *
 * Snapshots cm->manager.nodes[] under cs_nodes + bumps ref_count on each
 * non-disconnected entry, releases cs_nodes, calls the process_messages
 * and send_messages signals against the local copy, then re-acquires
 * cs_nodes to decrement refs. Returns true if any peer saw work.
 *
 * Exposed outside the message thread so the stress test can drive
 * the cycle directly without needing to stand up a full connman_start(). */
bool connman_run_message_cycle(struct connman *cm);

/* one pass of the socket-handler deferred-free sweep.
 *
 * Walks cm->deferred_free[], freeing entries whose ref_count has reached
 * zero and re-parking any that are still held by an in-flight snapshot.
 * Caller must hold cm->manager.cs_nodes. Exposed for the stress test. */
void connman_run_deferred_free_sweep(struct connman *cm);

bool connman_pick_next_outbound_target(
    struct connman *cm,
    size_t *addnode_cursor,
    struct addr_info *result,
    enum connman_outbound_target_source *source,
    size_t *addnode_index);

/* addnode reconnection backoff trio. All three stamp
 * addnode_last_attempt[i] with wall time so the dialer's cooldown can
 * pace retries.
 *
 * record_addnode_attempt(success=true) clears addnode_backoff_sec[i] to
 * 0; success=false forwards to record_addnode_failure with a TCP kind. */
void connman_record_addnode_attempt(struct connman *cm,
                                    size_t addnode_index,
                                    bool success);
/* Bumps the per-kind failure counter (addnode_tcp_failures[i] or
 * addnode_protocol_failures[i]) and grows addnode_backoff_sec[i]: from 0
 * it seeds 120s (TCP) or 900s (PROTOCOL), otherwise doubles, capped at
 * 1800s. */
void connman_record_addnode_failure(struct connman *cm,
                                    size_t addnode_index,
                                    enum connman_addnode_failure_kind kind);
/* Charges a PROTOCOL failure (the 900s seed / doubling-to-1800s backoff)
 * when an addnode peer drops before the handshake completes. No-op for
 * inbound, already-handshaked, or non-addnode nodes. */
void connman_note_addnode_prehandshake_disconnect(
    struct connman *cm,
    const struct p2p_node *node,
    const char *reason);

#endif
