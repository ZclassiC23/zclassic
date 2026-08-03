/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Pure wire codec for the ZCODE DHT FIND_NODE / NODES messages. */

#ifndef ZCL_VCS_ZCODE_DHT_MSGS_H
#define ZCL_VCS_ZCODE_DHT_MSGS_H

#include "vcs/zcode_dht.h"

#include <stddef.h>
#include <stdint.h>

/* Message envelope zcode_dht_msgs.v1, all integers little-endian:
 * 8-byte magic {'Z','C','D','H','T','M',0x0D,0x0A}, u16 LE schema version
 * (=1), u8 kind, then the kind's body. Every body opens with the sender's
 * node id (32) so the receiver can add the sender to its routing table.
 * FIND_NODE carries sender_node_id(32) || query_id(16) ||
 * target_node_id(32); NODES carries sender_node_id(32) || query_id(16) ||
 * count:u8 || count node-ID hints. Hints contain no address, delegation or
 * local observation and can never be promoted without a direct authenticated
 * Noise session. This v1 codec is replaced by the signed/bound v2 envelope
 * at the transport layer; it remains a pure payload parser. */
#define VCS_ZCODE_DHT_MSGS_WIRE_VERSION 1u
#define VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES 16u
#define VCS_ZCODE_DHT_MSGS_HEADER_BYTES 11u
#define VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES \
    (VCS_ZCODE_DHT_MSGS_HEADER_BYTES + VCS_ZCODE_DHT_ID_BYTES + 48u)
#define VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES \
    (VCS_ZCODE_DHT_MSGS_HEADER_BYTES + VCS_ZCODE_DHT_ID_BYTES + \
     VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES + 1u + \
     VCS_ZCODE_DHT_K * VCS_ZCODE_DHT_ID_BYTES)

enum vcs_zcode_dht_msg_kind {
    VCS_ZCODE_DHT_MSG_FIND_NODE = 1,
    VCS_ZCODE_DHT_MSG_NODES = 2,
};

struct vcs_zcode_dht_msg_find_node {
    uint8_t sender_node_id[VCS_ZCODE_DHT_ID_BYTES];
    uint8_t query_id[VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES];
    uint8_t target_node_id[VCS_ZCODE_DHT_ID_BYTES];
};

struct vcs_zcode_dht_msg_nodes {
    uint8_t sender_node_id[VCS_ZCODE_DHT_ID_BYTES];
    uint8_t query_id[VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES];
    uint32_t contact_count;
    uint8_t node_ids[VCS_ZCODE_DHT_K][VCS_ZCODE_DHT_ID_BYTES];
};

struct vcs_zcode_dht_msg {
    enum vcs_zcode_dht_msg_kind kind;
    union {
        struct vcs_zcode_dht_msg_find_node find_node;
        struct vcs_zcode_dht_msg_nodes nodes;
    };
};

/* Canonical serialization: equal inputs produce byte-identical wire, so
 * parse(serialize(x)) round-trips exactly and serialize(parse(wire))
 * reproduces the input wire byte-for-byte. sender_node_id, query_id and
 * (for FIND_NODE) target_node_id must be non-zero; NODES hints must be
 * strictly ascending, unique and non-zero. */
enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_find_node(
    const struct vcs_zcode_dht_msg_find_node *msg,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out);

/* contact_count over VCS_ZCODE_DHT_K is rejected. */
enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_nodes(
    const struct vcs_zcode_dht_msg_nodes *msg,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out);

/* Rejects wrong magic, wrong version, unknown kind, any length other than
 * header + body exactly (trailing bytes rejected), an all-zero
 * sender_node_id, an all-zero query_id, an all-zero FIND_NODE target, a
 * duplicate/out-of-order/zero NODES hints. */
enum vcs_zcode_dht_error vcs_zcode_dht_msg_parse(
    const uint8_t *wire, size_t wire_len, struct vcs_zcode_dht_msg *out);

#endif /* ZCL_VCS_ZCODE_DHT_MSGS_H */
