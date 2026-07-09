/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * simnet_wire — deterministic in-memory P2P wire harness.
 */

#ifndef ZCL_SIM_SIMNET_WIRE_H
#define ZCL_SIM_SIMNET_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct p2p_node;
struct uint256;
struct utxo_commitment;
struct simnet_wire;

enum simnet_wire_peer_kind {
    SIMNET_WIRE_PEER_HONEST = 1,
    SIMNET_WIRE_PEER_MALFORMED_FRAME = 2,
    SIMNET_WIRE_PEER_BAD_HANDSHAKE = 3,
    SIMNET_WIRE_PEER_FLOOD = 4,
    SIMNET_WIRE_PEER_SLOWLORIS = 5,
    SIMNET_WIRE_PEER_INVALID_BLOCK = 6,
    SIMNET_WIRE_PEER_INVALID_HEADER = 7,
    SIMNET_WIRE_PEER_REPLAY = 8,
    SIMNET_WIRE_PEER_ECLIPSE = 9,
    SIMNET_WIRE_PEER_FUZZ = 10,
};

enum simnet_wire_malformed_case {
    SIMNET_WIRE_MALFORMED_RANDOM = 0,
    SIMNET_WIRE_MALFORMED_BAD_CHECKSUM = 1,
    SIMNET_WIRE_MALFORMED_OVERSIZED = 2,
    SIMNET_WIRE_MALFORMED_BAD_MAGIC = 3,
};

enum simnet_wire_bad_handshake_case {
    SIMNET_WIRE_BAD_HANDSHAKE_RANDOM = 0,
    SIMNET_WIRE_BAD_HANDSHAKE_DATA_BEFORE_VERSION = 1,
    SIMNET_WIRE_BAD_HANDSHAKE_VERACK_FIRST = 2,
    SIMNET_WIRE_BAD_HANDSHAKE_GARBAGE_AFTER_VERACK = 3,
};

struct wire_scenario_peer {
    enum simnet_wire_peer_kind kind;
    size_t count;
};

/* Tick-keyed (NOT wall-clock) per-link partition timeline entry. peer_id
 * is closed once simnet_wire's tick counter reaches at_tick, and reopened
 * once it reaches at_tick + duration_ticks. duration_ticks == 0 means the
 * link stays closed for the rest of the run (no scripted reopen — a test
 * can still call simnet_wire_partition_peer() directly to reopen it). */
struct wire_scenario_partition {
    size_t peer_id;
    uint64_t at_tick;
    uint64_t duration_ticks;
};

struct wire_scenario {
    uint64_t master_seed;
    const struct wire_scenario_peer *peers;
    size_t peer_kind_count;
    size_t honest_peer_count;
    uint64_t duration_us;
    const struct wire_scenario_partition *partitions;
    size_t partition_count;
};

struct simnet_wire_stats {
    uint64_t ticks;
    uint64_t delivered_to_nut_bytes;
    uint64_t delivered_to_peer_bytes;
    uint64_t fingerprint;
    uint64_t rng_count;
    uint64_t checksum_fail_events;
    uint64_t peer_misbehave_events;
    uint64_t backpressure_reject_events;
    uint64_t peer_banned_events;
    uint64_t not_implemented_peers;
    size_t max_recv_msg_count;
    size_t max_send_size;
    size_t max_inventory_to_send;
    size_t max_addr_to_send;
    size_t pending_events;
    size_t to_nut_bytes;
    size_t to_peer_bytes;
    size_t peers_open;
    bool nut_disconnected;
    bool nut_banned;
    bool handshake_complete;
    bool pong_received;
    bool recv_queue_bounded;
    bool no_unexpected_permanent_blocker;
    bool memory_plateau_ok;
    bool consensus_unchanged;
    bool monitor_failed;
};

struct simnet_wire *simnet_wire_create(size_t peer_count, uint64_t seed);
void simnet_wire_free(struct simnet_wire *wire);

struct p2p_node *simnet_wire_node(struct simnet_wire *wire);

bool simnet_wire_start_honest_peer(struct simnet_wire *wire, size_t peer_id);
bool simnet_wire_start_peer_kind(struct simnet_wire *wire, size_t peer_id,
                                 enum simnet_wire_peer_kind kind);
bool simnet_wire_start_malformed_peer(
    struct simnet_wire *wire, size_t peer_id,
    enum simnet_wire_malformed_case malformed_case);
bool simnet_wire_start_bad_handshake_peer(
    struct simnet_wire *wire, size_t peer_id,
    enum simnet_wire_bad_handshake_case handshake_case);
struct simnet_wire *simnet_wire_create_scenario(
    const struct wire_scenario *scenario);
bool simnet_wire_peer_send_ping(struct simnet_wire *wire, size_t peer_id,
                                uint64_t nonce);
bool simnet_wire_peer_stop_adversary(struct simnet_wire *wire,
                                     size_t peer_id);

/* Per-link partition/recovery (Step D1). Enqueues WIRE_EVENT_CLOSE
 * (closed=true) or WIRE_EVENT_OPEN (closed=false) for peer_id — models
 * "the NUT loses its connection to one of its peers" without touching
 * any other link. Egress is pinned to peer slot 0 (see simnet_wire.c),
 * so closing peer 0 cuts the only egress-visible connection; closing any
 * other slot cuts an ingress-only byte source while peer 0 stays live. */
bool simnet_wire_partition_peer(struct simnet_wire *wire, size_t peer_id,
                                bool closed);

bool simnet_wire_run(struct simnet_wire *wire, uint64_t max_ticks,
                     uint64_t stuck_guard);

bool simnet_wire_peer_handshake_complete(const struct simnet_wire *wire,
                                         size_t peer_id);
bool simnet_wire_peer_pong_received(const struct simnet_wire *wire,
                                    size_t peer_id, uint64_t nonce);
uint64_t simnet_wire_fingerprint(const struct simnet_wire *wire);
bool simnet_wire_get_stats(const struct simnet_wire *wire,
                           struct simnet_wire_stats *out);
bool simnet_wire_tip_hash(const struct simnet_wire *wire,
                          struct uint256 *out);
bool simnet_wire_coins_digest(const struct simnet_wire *wire,
                              struct utxo_commitment *out);
bool simnet_wire_save_capsule(const struct simnet_wire *wire,
                              const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_SIM_SIMNET_WIRE_H */
