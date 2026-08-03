/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Signed Noise-bound ZCODE DHT FIND_NODE/NODES frames. */

#ifndef ZCL_VCS_ZCODE_DHT_MSGS_H
#define ZCL_VCS_ZCODE_DHT_MSGS_H

#include "vcs/zcode_dht.h"

#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_DHT_MSGS_WIRE_VERSION 2u
#define VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES 16u
#define VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES 64u
#define VCS_ZCODE_DHT_MSGS_HEADER_BYTES 11u
#define VCS_ZCODE_DHT_MSGS_AUTH_BYTES \
    (8u + 32u + VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES + \
     VCS_ZCODE_DHT_DELEGATION_WIRE_BYTES)
#define VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES \
    (VCS_ZCODE_DHT_MSGS_HEADER_BYTES + VCS_ZCODE_DHT_MSGS_AUTH_BYTES + \
     32u + VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES)
#define VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES \
    (VCS_ZCODE_DHT_MSGS_HEADER_BYTES + VCS_ZCODE_DHT_MSGS_AUTH_BYTES + \
     1u + VCS_ZCODE_DHT_K * 32u + VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES)
#define VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN "zcl.zcode.dht.frame.v2"

enum vcs_zcode_dht_msg_kind {
    VCS_ZCODE_DHT_MSG_FIND_NODE = 1,
    VCS_ZCODE_DHT_MSG_NODES = 2,
};

struct vcs_zcode_dht_msg_find_node {
    uint64_t session_generation;
    uint8_t sender_node_id[32];
    uint8_t query_id[16];
    struct vcs_zcode_dht_delegation delegation;
    uint8_t target_node_id[32];
};

struct vcs_zcode_dht_msg_nodes {
    uint64_t session_generation;
    uint8_t sender_node_id[32];
    uint8_t query_id[16];
    struct vcs_zcode_dht_delegation delegation;
    uint32_t contact_count;
    uint8_t node_ids[VCS_ZCODE_DHT_K][32];
};

struct vcs_zcode_dht_msg {
    enum vcs_zcode_dht_msg_kind kind;
    union {
        struct vcs_zcode_dht_msg_find_node find_node;
        struct vcs_zcode_dht_msg_nodes nodes;
    };
};

struct vcs_zcode_dht_msg_verify_context {
    bool noise_established;
    uint8_t noise_transcript_hash[32];
    uint8_t remote_noise_static[32];
    uint8_t network_genesis[32];
    uint64_t session_generation;
    uint64_t now_unix;
    vcs_zcode_dht_chain_verify_fn chain_verify;
    void *chain_ctx;
};

enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_find_node(
    const struct vcs_zcode_dht_msg_find_node *msg,
    const uint8_t noise_transcript_hash[32], const uint8_t online_seed[32],
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out);
enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_nodes(
    const struct vcs_zcode_dht_msg_nodes *msg,
    const uint8_t noise_transcript_hash[32], const uint8_t online_seed[32],
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out);

/* Exact bounds first, then established Noise/delegation/chain/static binding,
 * derived sender ID, online signature and session generation. Query-state,
 * replay and deadline checks belong to the bounded service above this codec. */
enum vcs_zcode_dht_error vcs_zcode_dht_msg_parse(
    const uint8_t *wire, size_t wire_len,
    const struct vcs_zcode_dht_msg_verify_context *verify,
    struct vcs_zcode_dht_msg *out);

#endif /* ZCL_VCS_ZCODE_DHT_MSGS_H */
