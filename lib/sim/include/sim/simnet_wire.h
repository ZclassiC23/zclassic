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
struct simnet_wire;

struct simnet_wire_stats {
    uint64_t ticks;
    uint64_t delivered_to_nut_bytes;
    uint64_t delivered_to_peer_bytes;
    uint64_t fingerprint;
    uint64_t rng_count;
    size_t pending_events;
    size_t to_nut_bytes;
    size_t to_peer_bytes;
    bool nut_disconnected;
    bool handshake_complete;
    bool pong_received;
};

struct simnet_wire *simnet_wire_create(size_t peer_count, uint64_t seed);
void simnet_wire_free(struct simnet_wire *wire);

struct p2p_node *simnet_wire_node(struct simnet_wire *wire);

bool simnet_wire_start_honest_peer(struct simnet_wire *wire, size_t peer_id);
bool simnet_wire_peer_send_ping(struct simnet_wire *wire, size_t peer_id,
                                uint64_t nonce);

bool simnet_wire_run(struct simnet_wire *wire, uint64_t max_ticks,
                     uint64_t stuck_guard);

bool simnet_wire_peer_handshake_complete(const struct simnet_wire *wire,
                                         size_t peer_id);
bool simnet_wire_peer_pong_received(const struct simnet_wire *wire,
                                    size_t peer_id, uint64_t nonce);
uint64_t simnet_wire_fingerprint(const struct simnet_wire *wire);
bool simnet_wire_get_stats(const struct simnet_wire *wire,
                           struct simnet_wire_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_SIM_SIMNET_WIRE_H */
