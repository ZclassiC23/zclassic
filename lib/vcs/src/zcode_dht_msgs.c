/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Pure wire codec for the ZCODE DHT FIND_NODE / NODES messages. */

#include "vcs/zcode_dht_msgs.h"

#include "base/serialize_le.h"

#include <string.h>

static const uint8_t msgs_magic[8] =
    {'Z','C','D','H','T','M',0x0D,0x0A};

static bool dht_msg_nonzero16(const uint8_t id[16])
{
    uint8_t any = 0;
    if (!id) return false;
    for (size_t i = 0; i < 16; i++) any |= id[i];
    return any != 0;
}

static bool dht_msg_nonzero32(const uint8_t id[32])
{
    uint8_t any = 0;
    if (!id) return false;
    for (size_t i = 0; i < 32; i++) any |= id[i];
    return any != 0;
}

static size_t dht_msg_write_header(uint8_t *wire,
                                   enum vcs_zcode_dht_msg_kind kind)
{
    size_t off = 0;
    memcpy(wire + off, msgs_magic, sizeof(msgs_magic));
    off += sizeof(msgs_magic);
    zcl_write_u16_le(wire + off, VCS_ZCODE_DHT_MSGS_WIRE_VERSION);
    off += 2;
    wire[off++] = (uint8_t)kind;
    return off;
}

enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_find_node(
    const struct vcs_zcode_dht_msg_find_node *msg,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out)
{
    if (!wire_len_out) return VCS_ZCODE_DHT_ERR_NULL;
    *wire_len_out = 0;
    if (!msg || !wire) return VCS_ZCODE_DHT_ERR_NULL;
    if (!dht_msg_nonzero32(msg->sender_node_id))
        return VCS_ZCODE_DHT_ERR_ID_ZERO;
    if (!dht_msg_nonzero16(msg->query_id))
        return VCS_ZCODE_DHT_ERR_QUERY_ID;
    if (!dht_msg_nonzero32(msg->target_node_id))
        return VCS_ZCODE_DHT_ERR_ID_ZERO;
    if (wire_capacity < VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES)
        return VCS_ZCODE_DHT_ERR_WIRE_SIZE;

    size_t off = dht_msg_write_header(wire, VCS_ZCODE_DHT_MSG_FIND_NODE);
    memcpy(wire + off, msg->sender_node_id, VCS_ZCODE_DHT_ID_BYTES);
    off += VCS_ZCODE_DHT_ID_BYTES;
    memcpy(wire + off, msg->query_id, VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES);
    off += VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES;
    memcpy(wire + off, msg->target_node_id, VCS_ZCODE_DHT_ID_BYTES);
    off += VCS_ZCODE_DHT_ID_BYTES;
    if (off != VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES)
        return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    *wire_len_out = off;
    return VCS_ZCODE_DHT_OK;
}

enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_nodes(
    const struct vcs_zcode_dht_msg_nodes *msg,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out)
{
    if (!wire_len_out) return VCS_ZCODE_DHT_ERR_NULL;
    *wire_len_out = 0;
    if (!msg || !wire) return VCS_ZCODE_DHT_ERR_NULL;
    if (!dht_msg_nonzero32(msg->sender_node_id))
        return VCS_ZCODE_DHT_ERR_ID_ZERO;
    if (!dht_msg_nonzero16(msg->query_id))
        return VCS_ZCODE_DHT_ERR_QUERY_ID;
    if (msg->contact_count > VCS_ZCODE_DHT_K)
        return VCS_ZCODE_DHT_ERR_LIMIT;

    size_t need = VCS_ZCODE_DHT_MSGS_HEADER_BYTES +
                  VCS_ZCODE_DHT_ID_BYTES +
                  VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES + 1 +
                  (size_t)msg->contact_count * VCS_ZCODE_DHT_ID_BYTES;
    if (wire_capacity < need) return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    for (uint32_t i = 0; i < msg->contact_count; i++) {
        if (!dht_msg_nonzero32(msg->node_ids[i]))
            return VCS_ZCODE_DHT_ERR_ID_ZERO;
        if (i && memcmp(msg->node_ids[i - 1], msg->node_ids[i], 32) >= 0)
            return VCS_ZCODE_DHT_ERR_WIRE_ORDER;
    }

    size_t off = dht_msg_write_header(wire, VCS_ZCODE_DHT_MSG_NODES);
    memcpy(wire + off, msg->sender_node_id, VCS_ZCODE_DHT_ID_BYTES);
    off += VCS_ZCODE_DHT_ID_BYTES;
    memcpy(wire + off, msg->query_id, VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES);
    off += VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES;
    wire[off++] = (uint8_t)msg->contact_count;
    for (uint32_t i = 0; i < msg->contact_count; i++) {
        memcpy(wire + off, msg->node_ids[i], VCS_ZCODE_DHT_ID_BYTES);
        off += VCS_ZCODE_DHT_ID_BYTES;
    }
    if (off != need) return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    *wire_len_out = off;
    return VCS_ZCODE_DHT_OK;
}

enum vcs_zcode_dht_error vcs_zcode_dht_msg_parse(
    const uint8_t *wire, size_t wire_len, struct vcs_zcode_dht_msg *out)
{
    if (!wire || !out) return VCS_ZCODE_DHT_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (wire_len < VCS_ZCODE_DHT_MSGS_HEADER_BYTES)
        return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    if (memcmp(wire, msgs_magic, sizeof(msgs_magic)) != 0)
        return VCS_ZCODE_DHT_ERR_WIRE_MAGIC;
    size_t off = sizeof(msgs_magic);
    uint16_t version = zcl_read_u16_le(wire + off);
    off += 2;
    if (version != VCS_ZCODE_DHT_MSGS_WIRE_VERSION)
        return VCS_ZCODE_DHT_ERR_VERSION;
    uint8_t kind = wire[off++];

    if (kind == VCS_ZCODE_DHT_MSG_FIND_NODE) {
        if (wire_len != VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES)
            return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
        memcpy(out->find_node.sender_node_id, wire + off,
               VCS_ZCODE_DHT_ID_BYTES);
        off += VCS_ZCODE_DHT_ID_BYTES;
        memcpy(out->find_node.query_id, wire + off,
               VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES);
        off += VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES;
        memcpy(out->find_node.target_node_id, wire + off,
               VCS_ZCODE_DHT_ID_BYTES);
        if (!dht_msg_nonzero32(out->find_node.sender_node_id))
            return VCS_ZCODE_DHT_ERR_ID_ZERO;
        if (!dht_msg_nonzero16(out->find_node.query_id))
            return VCS_ZCODE_DHT_ERR_QUERY_ID;
        if (!dht_msg_nonzero32(out->find_node.target_node_id))
            return VCS_ZCODE_DHT_ERR_ID_ZERO;
        out->kind = VCS_ZCODE_DHT_MSG_FIND_NODE;
        return VCS_ZCODE_DHT_OK;
    }

    if (kind == VCS_ZCODE_DHT_MSG_NODES) {
        if (wire_len < VCS_ZCODE_DHT_MSGS_HEADER_BYTES +
                       VCS_ZCODE_DHT_ID_BYTES +
                       VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES + 1)
            return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
        memcpy(out->nodes.sender_node_id, wire + off,
               VCS_ZCODE_DHT_ID_BYTES);
        off += VCS_ZCODE_DHT_ID_BYTES;
        memcpy(out->nodes.query_id, wire + off,
               VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES);
        off += VCS_ZCODE_DHT_MSG_QUERY_ID_BYTES;
        uint32_t count = wire[off++];
        if (count > VCS_ZCODE_DHT_K) return VCS_ZCODE_DHT_ERR_LIMIT;
        if (wire_len - off != (size_t)count * VCS_ZCODE_DHT_ID_BYTES)
            return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
        if (!dht_msg_nonzero32(out->nodes.sender_node_id))
            return VCS_ZCODE_DHT_ERR_ID_ZERO;
        if (!dht_msg_nonzero16(out->nodes.query_id))
            return VCS_ZCODE_DHT_ERR_QUERY_ID;
        for (uint32_t i = 0; i < count; i++) {
            memcpy(out->nodes.node_ids[i], wire + off, 32); off += 32;
            if (!dht_msg_nonzero32(out->nodes.node_ids[i]))
                return VCS_ZCODE_DHT_ERR_ID_ZERO;
            if (i && memcmp(out->nodes.node_ids[i - 1],
                            out->nodes.node_ids[i], 32) >= 0)
                return VCS_ZCODE_DHT_ERR_WIRE_ORDER;
        }
        out->kind = VCS_ZCODE_DHT_MSG_NODES;
        out->nodes.contact_count = count;
        return VCS_ZCODE_DHT_OK;
    }

    return VCS_ZCODE_DHT_ERR_WIRE_KIND;
}
