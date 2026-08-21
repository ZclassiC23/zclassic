/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Dual-signed swarm receipts multiplexed on zpkgswm. */

#ifndef ZCL_CONFIG_BOOT_ZCODE_SWARM_RECEIPT_H
#define ZCL_CONFIG_BOOT_ZCODE_SWARM_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct msg_processor;
struct p2p_node;
struct vcs_swarm_engine;
struct vcs_service_book;

/* True if the payload was a ZSID identity or ZSR1 receipt (consumed,
 * never forwarded to the frozen swarm codec). */
bool boot_zcode_swarm_receipt_frame(
    struct msg_processor *mp, struct p2p_node *node,
    struct vcs_swarm_engine *engine, struct vcs_service_book *book,
    const char *zcode_dir, const uint8_t *payload, size_t payload_len,
    int64_t day);

/* Queue identity (once) and a half-signed offer when this session has a
 * verified transfer with the peer. Returns frames sent. */
size_t boot_zcode_swarm_receipt_drain(
    struct msg_processor *mp, struct p2p_node *node,
    struct vcs_swarm_engine *engine, const char *zcode_dir, int64_t day);

void boot_zcode_swarm_receipt_close(void);

#endif /* ZCL_CONFIG_BOOT_ZCODE_SWARM_RECEIPT_H */