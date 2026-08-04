/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Composition-root adapter between Noise peers and ZCODE DHT. */

#ifndef ZCL_CONFIG_BOOT_ZCODE_DHT_H
#define ZCL_CONFIG_BOOT_ZCODE_DHT_H

#include "vcs/zcode_dht_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct boot_svc_ctx;
struct json_value;
struct msg_processor;
struct p2p_node;
struct rpc_table;
struct block_index;

/* Lock order (outermost to innermost): public lookup lifecycle -> DHT service
 * -> chain-authorization/reachability caches.  No database, ancestry walk,
 * endpoint scan, connman/socket operation, or disk I/O may run while the DHT
 * service lock is held.  Detached persistence snapshots own their bytes and
 * are committed only after service-pointer + generation revalidation. */

/* True only after Noise and the P2P version/verack handshake are both ready;
 * DHT bootstrap frames must not cross the message layer earlier. */
bool boot_zcode_dht_peer_ready(const struct p2p_node *node);

/* Returns true only when this is the ZCDHTM namespace and is consumed. */
bool boot_zcode_dht_frame(struct msg_processor *mp, struct p2p_node *node,
                          const uint8_t *payload, size_t payload_len,
                          struct boot_svc_ctx *svc);
void boot_zcode_dht_periodic(struct msg_processor *mp,
                             struct boot_svc_ctx *svc);
void boot_zcode_dht_shutdown(void);

/* Short critical-section adapters used by the nonblocking public lifecycle.
 * `generation` binds an opaque public admission to one service instance. */
bool boot_zcode_dht_lookup_begin(
    const uint8_t target[32], struct vcs_zcode_dht_time now,
    uint64_t *lookup_id, uint64_t *generation);
bool boot_zcode_dht_lookup_poll(
    uint64_t lookup_id, uint64_t generation, struct vcs_zcode_dht_time now,
    struct vcs_zcode_dht_lookup_result *out);
bool boot_zcode_dht_lookup_cancel(uint64_t lookup_id, uint64_t generation);
bool boot_zcode_dht_peers(uint64_t wall_now,
                          struct vcs_zcode_dht_peer_view *out, size_t max,
                          size_t offset, size_t *count_out);
void boot_zcode_dht_public_tick(uint64_t monotonic_s);
void boot_zcode_dht_public_reset(void);

/* O(log n) ancestry proof used by the chain-binding adapter and its deep-tip
 * regression test. `height_span_out` is diagnostic context only. */
bool boot_zcode_dht_beacon_matches(const struct block_index *header_tip,
                                   uint32_t beacon_height,
                                   const uint8_t beacon_hash[32],
                                   uint64_t *height_span_out);

/* Invoked by the existing ZID status-generation worker, never a new poller. */
bool boot_zcode_dht_revalidate(void);

/* See AGENTS.md "Adding state introspection". Reentrant-safe. */
bool boot_zcode_dht_dump_state_json(struct json_value *out, const char *key);
void boot_zcode_dht_register_rpc(struct rpc_table *table);

#endif /* ZCL_CONFIG_BOOT_ZCODE_DHT_H */
