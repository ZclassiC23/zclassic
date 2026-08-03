/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Composition-root adapter between Noise peers and ZCODE DHT. */

#ifndef ZCL_CONFIG_BOOT_ZCODE_DHT_H
#define ZCL_CONFIG_BOOT_ZCODE_DHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct boot_svc_ctx;
struct json_value;
struct msg_processor;
struct p2p_node;
struct rpc_table;

/* Returns true only when this is the ZCDHTM namespace and is consumed. */
bool boot_zcode_dht_frame(struct msg_processor *mp, struct p2p_node *node,
                          const uint8_t *payload, size_t payload_len,
                          struct boot_svc_ctx *svc);
void boot_zcode_dht_periodic(struct msg_processor *mp,
                             struct boot_svc_ctx *svc);
void boot_zcode_dht_shutdown(void);

/* Invoked by the existing ZID status-generation worker, never a new poller. */
bool boot_zcode_dht_revalidate(void);

/* See AGENTS.md "Adding state introspection". Reentrant-safe. */
bool boot_zcode_dht_dump_state_json(struct json_value *out, const char *key);
void boot_zcode_dht_register_rpc(struct rpc_table *table);

#endif /* ZCL_CONFIG_BOOT_ZCODE_DHT_H */
