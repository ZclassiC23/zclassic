/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_NET_H
#define ZCL_NET_H

#include "net/netaddr.h"
#include "net/netbase.h"
#include "net/protocol.h"
#include "net/fast_sync.h"
#include "net/addrman.h"
#include "bloom/bloom.h"
#include "core/uint256.h"
#include "event/event.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#define PING_INTERVAL (2 * 60)
#define TIMEOUT_INTERVAL (20 * 60)
#define MAX_INV_SZ 50000
#define MAX_ADDR_TO_SEND 1000
#define MAX_PROTOCOL_MESSAGE_LENGTH (2 * 1024 * 1024)
#define MAX_SUBVERSION_LENGTH 256
#define DEFAULT_MAX_PEER_CONNECTIONS 125
#define MAX_OUTBOUND_CONNECTIONS 8
#define NETWORK_UPGRADE_PEER_PREFERENCE_BLOCK_PERIOD (24 * 24 * 3)
#define DUMP_ADDRESSES_INTERVAL 900
#define MAPASKFOR_MAX_SZ MAX_INV_SZ
#define SETASKFOR_MAX_SZ (2 * MAX_INV_SZ)
#define BIP0031_VERSION 60000
#define INIT_PROTO_VERSION 209

typedef int32_t node_id_t;

struct block;  /* forward decl for BIP152 pending compact block state */

enum local_addr_score {
    LOCAL_NONE = 0,
    LOCAL_IF,
    LOCAL_BIND,
    LOCAL_UPNP,
    LOCAL_MANUAL,
    LOCAL_MAX
};

struct local_service_info {
    int score;
    uint16_t port;
};

struct node_stats {
    node_id_t nodeid;
    uint64_t services;
    int64_t last_send;
    int64_t last_recv;
    int64_t time_connected;
    int64_t time_offset;
    char addr_name[256];
    int version;
    char clean_sub_ver[MAX_SUBVERSION_LENGTH];
    bool inbound;
    int starting_height;
    uint64_t send_bytes;
    uint64_t recv_bytes;
    bool whitelisted;
    double ping_time;
    double ping_wait;
    char addr_local[72];
};

struct net_message {
    bool in_data;
    uint8_t hdr_buf[MSG_HEADER_SIZE];
    struct msg_header hdr;
    unsigned char expected_msgstart[MESSAGE_START_SIZE];
    unsigned int hdr_pos;
    uint8_t *recv_data;
    size_t recv_alloc;
    unsigned int data_pos;
    int64_t time_usec;
};

void net_message_init(struct net_message *msg,
                      const unsigned char msgstart[MESSAGE_START_SIZE]);
void net_message_free(struct net_message *msg);
bool net_message_complete(const struct net_message *msg);
int net_message_read_header(struct net_message *msg,
                            const char *pch, unsigned int nbytes);
int net_message_read_data(struct net_message *msg,
                          const char *pch, unsigned int nbytes);

/* process-wide recv queue byte budget. net_recv_total_bytes
 * returns the current sum of outstanding msg->recv_alloc across every
 * net_message. net_recv_total_bytes_cap() returns the configured
 * ceiling (env ZCL_MAX_RECVBUFFER_TOTAL_BYTES, default 256 MiB). When
 * adding a new message's allocation would exceed the cap,
 * net_message_read_data fails with -1 instead of triggering the
 * allocation. */
size_t net_recv_total_bytes(void);
size_t net_recv_total_bytes_cap(void);

struct send_segment {
    uint8_t *data;
    size_t size;
    struct send_segment *next;
};

/* Free a send_segment AND release its bytes from the process-wide send
 * budget. Every drain/disconnect path must use this rather than a raw
 * free(), or g_send_total_bytes leaks. */
void send_segment_free(struct send_segment *seg);

struct p2p_node;

/* Process-wide send-queue byte budget — the symmetric mirror of the
 * recv budget above. net_send_total_bytes() is the current sum of every
 * live send_segment->size across all peers; net_send_total_bytes_cap()
 * is the configured process ceiling (env ZCL_MAX_SENDBUFFER_TOTAL_BYTES,
 * default 512 MiB) and net_send_peer_bytes_cap() the per-peer ceiling
 * (env ZCL_MAX_SENDBUFFER_PEER_BYTES, default 32 MiB). net_send_over_budget()
 * is true when this peer's buffered send bytes exceed the per-peer cap OR
 * the process-wide total is at/over the cap; whitelisted peers are exempt.
 * Serving loops (e.g. process_getdata) consult it to pause serving — they
 * must NOT disconnect, the peer is within protocol and re-requests later. */
size_t net_send_total_bytes(void);
size_t net_send_total_bytes_cap(void);
size_t net_send_peer_bytes_cap(void);
bool net_send_over_budget(const struct p2p_node *node);

/* reason is a short human-readable label ("threshold reached: <offence>"
 * etc, truncated) captured at ban time purely for operator diagnosis
 * (zcl_peer_incidents / node.log) — it is NEVER matched against on
 * reload, so a future rename of an offence string cannot break restore. */
#define BAN_ENTRY_REASON_MAX 32
struct ban_entry {
    struct net_addr addr;
    uint8_t prefix_len;
    int64_t ban_until;
    int32_t score_at_ban;
    char reason[BAN_ENTRY_REASON_MAX];
};

#define MAX_BAN_ENTRIES 4096
#define MAX_WHITELIST_ENTRIES 256
#define MAX_RECV_MESSAGES 1024
#define MAX_ASKFOR_ENTRIES 50000
#define MAX_INVENTORY_KNOWN 50000

struct askfor_entry {
    int64_t request_time;
    struct inv_item inv;
};

struct p2p_node {
    /* Cross-thread publication barrier: writers release-store state after
     * filling handshake metadata; diagnostic readers acquire-load it before
     * copying services/version/height/subversion.  Transition validation is
     * serialized in peer_set_state_checked(). */
    _Atomic enum peer_state state; /* explicit state machine — use peer_set_state_checked() */
    uint64_t services;
    zcl_socket_t socket;

    struct send_segment *send_head;
    struct send_segment *send_tail;
    size_t send_size;
    size_t send_offset;
    uint64_t send_bytes;
    zcl_mutex_t cs_send;

    struct net_message *recv_msgs;
    size_t recv_msg_count;
    size_t recv_msg_cap;
    uint64_t recv_bytes;
    int recv_version;
    zcl_mutex_t cs_recv;

    int64_t last_send;
    int64_t last_recv;
    int64_t time_connected;
    int64_t time_offset;
    struct net_address addr;
    char addr_name[256];
    struct net_service addr_local;
    int version;
    char sub_ver[MAX_SUBVERSION_LENGTH];
    char clean_sub_ver[MAX_SUBVERSION_LENGTH];
    bool whitelisted;
    bool one_shot;
    bool client;
    bool inbound;
    bool network_node;
    _Atomic bool disconnect;
    bool relay_txes;
    bool sent_addr;
    int ref_count;
    node_id_t id;

    struct uint256 hash_continue;
    int starting_height;

    struct net_address *addr_to_send;
    size_t addr_to_send_count;
    size_t addr_to_send_cap;
    struct rolling_bloom_filter addr_known;
    bool get_addr;

    /* Mempool sync-on-connect (msg_tx.c::msg_tx_maybe_request_mempool):
     * set the instant we queue our one outbound "mempool" pull for this
     * peer so a later duplicate verack (or any other call site) never
     * sends a second request. */
    bool mempool_requested;

    /* Per-peer addr-message rate limit (msgprocessor_inv.c::process_addr).
     * A single "addr" message is already capped at MAX_ADDR_TO_SEND
     * entries (oversized -> PEER_OFFENCE_FLOOD + disconnect), but nothing
     * stopped a peer from repeating max-legal-size batches back-to-back
     * for free. Fixed window: addr_rate_window_count accumulates entries
     * received since addr_rate_window_start; once the window rolls over
     * (ADDR_RATE_WINDOW_SECS) it resets. Zero-initialised (memset in
     * p2p_node_create) so window_start==0 correctly reads as "no window
     * yet" on the very first addr message. */
    int64_t addr_rate_window_start;
    uint32_t addr_rate_window_count;

    struct inv_item *inventory_to_send;
    size_t inventory_to_send_count;
    size_t inventory_to_send_cap;
    struct uint256 *inventory_known_hashes;
    size_t inventory_known_count;
    size_t inventory_known_cap;
    zcl_mutex_t cs_inventory;

    struct uint256 *askfor_set;
    size_t askfor_set_count;
    size_t askfor_set_cap;
    struct askfor_entry *askfor_map;
    size_t askfor_map_count;
    size_t askfor_map_cap;

    struct bloom_filter *pfilter;
    zcl_mutex_t cs_filter;

    uint64_t ping_nonce_sent;
    int64_t ping_usec_start;
    int64_t ping_usec_time;
    int64_t min_ping_usec_time;
    bool ping_queued;
    bool prefer_headers;
    bool send_compact;

    /* BIP152: pending compact block reconstruction (at most one per peer) */
    struct block *compact_pending_block;       /* partial block from cmpctblock */
    struct uint256 compact_pending_hash;       /* block hash we're waiting for */
    uint64_t *compact_missing_indices;         /* which tx slots are empty */
    size_t compact_num_missing;                /* count of missing indices */
    int64_t compact_request_time;              /* when getblocktxn was sent (timeout) */

    /* Cross-thread: the msg-handler thread writes these (no cs_nodes) while
     * the self_heal thread reads/resets them (under cs_nodes). cs_nodes gives
     * zero mutual exclusion because one side never takes it, so make them
     * _Atomic and use relaxed atomics at every site. This removes torn reads
     * / lost stall-counter updates without changing any net policy (these are
     * heuristic stall counters; relaxed ordering is sufficient). */
    _Atomic int64_t last_getheaders_time;
    _Atomic int     getheaders_stale_count;   /* consecutive empty header batches */

    /* Per-peer header delivery tracking (stall detection). Same cross-thread
     * rationale as above — _Atomic + relaxed. */
    _Atomic int64_t  last_useful_headers_time;  /* last time peer delivered accepted headers */
    uint64_t total_headers_delivered;   /* lifetime count of accepted headers from peer */

    _Atomic int misbehavior;  /* cumulative misbehavior score; banned at 100 */
    /* Monotonic timestamp (ms since UNIX epoch) of last accepted / valid
     * message from this peer. Used by peer_scoring.c to decay `misbehavior`
     * when a peer has been behaving. 0 means "never" — treated as "now"
     * on first decay call so freshly-connected peers don't get a free
     * score drop. */
    _Atomic int_least64_t peer_score_last_good_ms;

    /* Framing-layer offence tag (holds an enum peer_offence value; 0 =
     * PEER_OFFENCE_NONE). The message framing parse functions
     * (net_message_read_header / net_message_read_data) and
     * p2p_node_receive_bytes run WITHOUT a net_manager back-pointer, so they
     * cannot call peer_scoring_record() directly. Instead p2p_node_receive_bytes
     * tags the offence here; the connman receive caller (which holds nm) drains
     * it exactly once via p2p_node_score_framing_offence() — the single scoring
     * point, disjoint from the pure parse code. Atomic because the tag is set
     * on the recv path and drained by the same thread but must never tear. */
    _Atomic int framing_offence;

    /* connection quality metrics */
    int64_t last_block_time;  /* timestamp of last valid block received */
    int64_t last_tx_time;     /* timestamp of last novel tx accepted from this peer */
    int64_t avg_latency_us;   /* rolling average ping latency in microseconds */
    int blocks_received;      /* count of valid blocks from this peer */

    /* zclassic23 fast sync state (tracked via enum peer_state) */
    uint64_t zsync_offset;    /* total UTXOs received/sent (progress) */
    uint64_t zsync_total;     /* total UTXOs expected */
    uint64_t zsync_sent;      /* chunks sent so far */
    uint8_t zsync_cursor_txid[32]; /* keyset cursor: last txid sent (legacy) */
    int32_t zsync_cursor_vout;     /* keyset cursor: last vout sent (legacy) */
    bool zsync_cursor_valid;       /* true after first batch (legacy) */
    int64_t zsync_file_offset;     /* byte offset into pre-serialized snapshot */
    int64_t zsync_file_size;       /* total bytes in snapshot file */
    uint8_t zsync_offered_root[32]; /* SHA3 root from offer (verify on end) */
    uint8_t zsync_offered_mmr[32];  /* MMR root from offer (PoW chain proof) */
    int32_t zsync_offered_height;   /* height of offered snapshot */
    uint8_t zsync_offered_block[32]; /* block hash of offered snapshot */
    uint64_t zsync_offered_count;   /* UTXO count in offered snapshot */
    uint64_t zsync_offer_version;   /* offer cache generation sent to peer */
    uint64_t zsync_snapshot_version; /* snapshot buffer generation sent to peer */

    /* Swarm parallel chunk sync state (UTXO) */
    bool swarm_manifest_sent;     /* true if we sent our manifest to this peer */
    bool swarm_manifest_received; /* true if we received manifest from peer */
    int32_t swarm_inflight_chunk; /* chunk index assigned to this peer, -1 = none */
    int64_t swarm_chunk_req_time; /* when chunk was requested (for timeout) */

    /* Block swarm state (parallel block download) */
    bool blk_manifest_sent;       /* true if we sent block manifest to this peer */
    bool blk_manifest_received;   /* true if we received block manifest from peer */
    struct {
        int32_t piece_index;      /* -1 = empty slot */
        int64_t request_time;
    } blk_pipeline[PIECE_PIPELINE_DEPTH];
    uint8_t *blk_bitmap;          /* peer's piece availability bitmap (heap) */
    uint32_t blk_bitmap_len;      /* bytes in bitmap */
    int32_t blk_peer_height;      /* peer's manifest end_height */
};

struct node_signals {
    int (*get_height)(void *ctx);
    bool (*process_messages)(void *ctx, struct p2p_node *node);
    bool (*send_messages)(void *ctx, struct p2p_node *node, bool send_trickle);
    void (*initialize_node)(void *ctx, node_id_t id, struct p2p_node *node);
    void (*finalize_node)(void *ctx, node_id_t id);
    void *ctx;
};

struct listen_socket {
    zcl_socket_t socket;
    bool whitelisted;
};

struct net_manager {
    bool discover;
    bool listen;
    uint64_t local_services;
    uint64_t local_host_nonce;
    char sub_version[MAX_SUBVERSION_LENGTH];
    int max_connections;
    bool addresses_initialized;

    struct addr_man addrman;

    zcl_mutex_t cs_nodes;
    struct p2p_node **nodes;
    size_t num_nodes;
    size_t nodes_cap;

    struct p2p_node **nodes_disconnected;
    size_t num_disconnected;
    size_t disconnected_cap;

    zcl_mutex_t cs_local_host;
    struct net_addr *local_hosts;
    struct local_service_info *local_host_info;
    size_t num_local_hosts;
    size_t local_hosts_cap;
    bool limited[NET_MAX];

    zcl_mutex_t cs_banned;
    struct ban_entry *banned;
    size_t num_banned;
    size_t banned_cap;
    /* Borrowed pointer (owned by boot/connman context), NULL until the
     * owner wires it — see connman_load_addrman()/connman_save_addrman().
     * When set, ban_addr()/unban_addr()/clear_banned() persist the ban
     * table to <datadir>/banlist.dat on every mutation so a restart does
     * not amnesty banned attackers. NULL is the safe default: every
     * existing net_manager_init() caller (tests included) simply gets
     * no persistence, unchanged from before this field existed. */
    const char *datadir;

    struct net_addr *whitelisted;
    uint8_t *whitelist_prefix;
    size_t num_whitelisted;
    size_t whitelist_cap;

    struct listen_socket *listen_sockets;
    size_t num_listen_sockets;
    size_t listen_sockets_cap;

    node_id_t last_node_id;
    zcl_mutex_t cs_last_node_id;

    struct node_signals signals;
    zcl_cond_t msg_handler_cond;
    zcl_mutex_t msg_handler_mutex;

    uint64_t total_bytes_recv;
    uint64_t total_bytes_sent;
    zcl_mutex_t cs_total_bytes_recv;
    zcl_mutex_t cs_total_bytes_sent;

    volatile bool stop_requested;

    unsigned char message_start[MESSAGE_START_SIZE];
    uint16_t default_port;
};

void net_manager_init(struct net_manager *nm);
void net_manager_free(struct net_manager *nm);

struct p2p_node *p2p_node_create(struct net_manager *nm, zcl_socket_t sock,
                                  const struct net_address *addr,
                                  const char *addr_name, bool inbound);
void p2p_node_free(struct p2p_node *node);

void p2p_node_add_ref(struct p2p_node *node);
void p2p_node_release(struct p2p_node *node);
int p2p_node_get_ref(struct p2p_node *node);

bool p2p_node_receive_bytes(struct p2p_node *node, const char *data,
                             unsigned int nbytes,
                             const unsigned char msgstart[MESSAGE_START_SIZE]);

/* Drain the framing-layer offence tag set by p2p_node_receive_bytes and, if
 * non-zero, score the peer once via peer_scoring_record(). Resets the tag to
 * PEER_OFFENCE_NONE so a reconnecting peer that repeats the abuse keeps
 * accruing toward the ban threshold. This is the SINGLE scoring point for the
 * framing layer — the pure parse functions stay free of any net_manager
 * dependency. Safe to call after every receive (no-op when the tag is zero). */
void p2p_node_score_framing_offence(struct net_manager *nm,
                                    struct p2p_node *node);

void p2p_node_close_socket(struct p2p_node *node);

void p2p_node_copy_stats(const struct p2p_node *node, struct node_stats *stats);

void p2p_node_push_address(struct p2p_node *node, const struct net_address *addr);
void p2p_node_add_inventory_known(struct p2p_node *node, const struct inv_item *inv);
void p2p_node_push_inventory(struct p2p_node *node, const struct inv_item *inv);

bool p2p_node_begin_message(struct p2p_node *node, const char *command,
                             const unsigned char msgstart[MESSAGE_START_SIZE]);
void p2p_node_write_message_data(struct p2p_node *node,
                                  const uint8_t *data, size_t len);
bool p2p_node_end_message(struct p2p_node *node);

void socket_send_data(struct p2p_node *node);

/* Returns NULL or a node with a +1 CALLER-owned ref. The caller MUST release
 * that ref under cs_nodes after it is done deref'ing the node (symmetric-ref
 * contract — see the connect_node body in net.c). Both the new-node and the
 * dedupe path return a releasable +1 ref. */
struct p2p_node *connect_node(struct net_manager *nm,
                               struct net_address *addr_connect,
                               const char *dest);

bool accept_connection(struct net_manager *nm, const struct listen_socket *ls);
bool is_banned(struct net_manager *nm, const struct net_addr *addr);
void ban_addr(struct net_manager *nm, const struct net_addr *addr,
              int64_t ban_offset, bool since_epoch);

/* Increase misbehavior score. Auto-bans at threshold (100). */
void peer_misbehaving(struct net_manager *nm, struct p2p_node *node,
                      int howmuch, const char *reason);
bool unban_addr(struct net_manager *nm, const struct net_addr *addr);
void clear_banned(struct net_manager *nm);

/* Ban persistence — <datadir>/banlist.dat, one self-verifying file (a
 * 48-byte embedded SHA3 integrity header shared with peers.dat /
 * block_index.bin via storage/sha3_sidecar_io.h prefixes the serialized
 * ban entries; one fsync, one rename, no separate sidecar file so a
 * crash mid-write can never strand a body under a stale commitment).
 *
 * ban_db_write() serializes every currently-live (non-expired) entry in
 * nm->banned[]. Called automatically by ban_addr()/unban_addr()/
 * clear_banned() whenever nm->datadir is set (see connman_load_addrman())
 * — callers do not need to invoke it directly in normal operation.
 *
 * ban_db_read() loads the persisted table into nm->banned[], skipping
 * (lazily pruning) any entry whose ban_until has already passed. Call
 * once at boot BEFORE the first inbound/outbound connection is accepted
 * (see connman_load_addrman()). A missing file is a clean cold-start
 * miss (returns false, not an error); a corrupt file is quarantined
 * (renamed aside) and treated the same as missing — bans are advisory
 * hardening, never fatal to boot. */
bool ban_db_write(struct net_manager *nm, const char *datadir);
bool ban_db_read(struct net_manager *nm, const char *datadir);

bool add_local(struct net_manager *nm, const struct net_service *addr,
               int score);
bool remove_local(struct net_manager *nm, const struct net_service *addr);
bool is_local(struct net_manager *nm, const struct net_service *addr);
bool is_reachable_net(struct net_manager *nm, enum zcl_network net);
void set_limited(struct net_manager *nm, enum zcl_network net, bool limited);

bool bind_listen_port(struct net_manager *nm, const struct net_service *addr,
                      bool whitelisted);

bool addr_db_write(const struct net_manager *nm, const char *datadir);
bool addr_db_read(struct net_manager *nm, const char *datadir);

#endif
