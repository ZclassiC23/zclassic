/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Signed Noise-bound ZCODE DHT FIND_NODE/NODES frames. */

#include "vcs/zcode_dht_msgs.h"

#include "base/serialize_le.h"
#include "crypto/ed25519.h"
#include "support/cleanse.h"

#include <string.h>

static const uint8_t msgs_magic[8] =
    {'Z','C','D','H','T','M',0x0D,0x0A};

static bool nonzero(const uint8_t *p, size_t n)
{
    uint8_t any = 0;
    if (!p) return false;
    for (size_t i = 0; i < n; i++) any |= p[i];
    return any != 0;
}

static size_t write_header(uint8_t *wire, enum vcs_zcode_dht_msg_kind kind)
{
    memcpy(wire, msgs_magic, 8);
    zcl_write_u16_le(wire + 8, VCS_ZCODE_DHT_MSGS_WIRE_VERSION);
    wire[10] = (uint8_t)kind;
    return VCS_ZCODE_DHT_MSGS_HEADER_BYTES;
}

static enum vcs_zcode_dht_error write_auth(
    uint8_t *wire, size_t *off, uint64_t generation,
    const uint8_t sender[32], const uint8_t query[16],
    const struct vcs_zcode_dht_delegation *delegation)
{
    if (!generation || !nonzero(sender, 32)) return VCS_ZCODE_DHT_ERR_ID_ZERO;
    if (!nonzero(query, 16)) return VCS_ZCODE_DHT_ERR_QUERY_ID;
    zcl_write_u64_le(wire + *off, generation); *off += 8;
    memcpy(wire + *off, sender, 32); *off += 32;
    memcpy(wire + *off, query, 16); *off += 16;
    if (vcs_zcode_dht_delegation_encode(delegation, wire + *off) !=
        VCS_ZCODE_DHT_DELEGATION_OK) return VCS_ZCODE_DHT_ERR_DELEGATION;
    *off += VCS_ZCODE_DHT_DELEGATION_WIRE_BYTES;
    uint8_t derived[32];
    if (!vcs_zcode_dht_delegation_node_id(derived, delegation) ||
        memcmp(derived, sender, 32) != 0)
        return VCS_ZCODE_DHT_ERR_DELEGATION;
    return VCS_ZCODE_DHT_OK;
}

static enum vcs_zcode_dht_error sign_frame(
    uint8_t *wire, size_t unsigned_len, const uint8_t transcript[32],
    const uint8_t online_seed[32],
    const struct vcs_zcode_dht_delegation *delegation)
{
    if (!nonzero(transcript, 32) || !online_seed)
        return VCS_ZCODE_DHT_ERR_SESSION;
    uint8_t pub[32], secret[32];
    ed25519_keypair(pub, secret, online_seed);
    if (memcmp(pub, delegation->online_pubkey, 32) != 0) {
        memory_cleanse(secret, sizeof(secret));
        return VCS_ZCODE_DHT_ERR_IDENTITY;
    }
    uint8_t preimage[sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN) + 32 +
                     VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    size_t off = 0;
    memcpy(preimage + off, VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN,
           sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN));
    off += sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN);
    memcpy(preimage + off, transcript, 32); off += 32;
    memcpy(preimage + off, wire, unsigned_len); off += unsigned_len;
    ed25519_sign(wire + unsigned_len, preimage, off, secret, pub);
    memory_cleanse(secret, sizeof(secret));
    memory_cleanse(preimage, off);
    return VCS_ZCODE_DHT_OK;
}

enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_find_node(
    const struct vcs_zcode_dht_msg_find_node *m,
    const uint8_t transcript[32], const uint8_t online_seed[32],
    uint8_t *wire, size_t cap, size_t *len_out)
{
    if (!len_out) return VCS_ZCODE_DHT_ERR_NULL;
    *len_out = 0;
    if (!m || !wire || !transcript || !online_seed)
        return VCS_ZCODE_DHT_ERR_NULL;
    if (!nonzero(m->target_node_id, 32)) return VCS_ZCODE_DHT_ERR_ID_ZERO;
    if (cap < VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES)
        return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    size_t off = write_header(wire, VCS_ZCODE_DHT_MSG_FIND_NODE);
    enum vcs_zcode_dht_error e = write_auth(
        wire, &off, m->session_generation, m->sender_node_id, m->query_id,
        &m->delegation);
    if (e != VCS_ZCODE_DHT_OK) return e;
    memcpy(wire + off, m->target_node_id, 32); off += 32;
    e = sign_frame(wire, off, transcript, online_seed, &m->delegation);
    if (e != VCS_ZCODE_DHT_OK) return e;
    off += VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES;
    *len_out = off; return off == VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES
        ? VCS_ZCODE_DHT_OK : VCS_ZCODE_DHT_ERR_WIRE_SIZE;
}

enum vcs_zcode_dht_error vcs_zcode_dht_msg_serialize_nodes(
    const struct vcs_zcode_dht_msg_nodes *m,
    const uint8_t transcript[32], const uint8_t online_seed[32],
    uint8_t *wire, size_t cap, size_t *len_out)
{
    if (!len_out) return VCS_ZCODE_DHT_ERR_NULL;
    *len_out = 0;
    if (!m || !wire || !transcript || !online_seed)
        return VCS_ZCODE_DHT_ERR_NULL;
    if (m->contact_count > VCS_ZCODE_DHT_K) return VCS_ZCODE_DHT_ERR_LIMIT;
    for (uint32_t i = 0; i < m->contact_count; i++) {
        if (!nonzero(m->node_ids[i], 32)) return VCS_ZCODE_DHT_ERR_ID_ZERO;
        if (i && memcmp(m->node_ids[i - 1], m->node_ids[i], 32) >= 0)
            return VCS_ZCODE_DHT_ERR_WIRE_ORDER;
    }
    size_t need = VCS_ZCODE_DHT_MSGS_HEADER_BYTES +
        VCS_ZCODE_DHT_MSGS_AUTH_BYTES + 1 + (size_t)m->contact_count * 32 +
        VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES;
    if (cap < need) return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    size_t off = write_header(wire, VCS_ZCODE_DHT_MSG_NODES);
    enum vcs_zcode_dht_error e = write_auth(
        wire, &off, m->session_generation, m->sender_node_id, m->query_id,
        &m->delegation);
    if (e != VCS_ZCODE_DHT_OK) return e;
    wire[off++] = (uint8_t)m->contact_count;
    for (uint32_t i = 0; i < m->contact_count; i++) {
        memcpy(wire + off, m->node_ids[i], 32); off += 32;
    }
    e = sign_frame(wire, off, transcript, online_seed, &m->delegation);
    if (e != VCS_ZCODE_DHT_OK) return e;
    off += VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES;
    *len_out = off; return off == need ? VCS_ZCODE_DHT_OK
                                       : VCS_ZCODE_DHT_ERR_WIRE_SIZE;
}

static enum vcs_zcode_dht_error verify_signature(
    const uint8_t *wire, size_t unsigned_len, const uint8_t signature[64],
    const uint8_t transcript[32], const uint8_t online_pubkey[32])
{
    uint8_t preimage[sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN) + 32 +
                     VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
    size_t off = 0;
    memcpy(preimage + off, VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN,
           sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN));
    off += sizeof(VCS_ZCODE_DHT_MSG_SIGNATURE_DOMAIN);
    memcpy(preimage + off, transcript, 32); off += 32;
    memcpy(preimage + off, wire, unsigned_len); off += unsigned_len;
    bool ok = ed25519_verify(signature, preimage, off, online_pubkey);
    memory_cleanse(preimage, off);
    return ok ? VCS_ZCODE_DHT_OK : VCS_ZCODE_DHT_ERR_SIGNATURE;
}

static enum vcs_zcode_dht_error read_auth(
    const uint8_t *wire, size_t *off,
    const struct vcs_zcode_dht_msg_verify_context *v, uint64_t *generation,
    uint8_t sender[32], uint8_t query[16],
    struct vcs_zcode_dht_delegation *delegation)
{
    *generation = zcl_read_u64_le(wire + *off); *off += 8;
    memcpy(sender, wire + *off, 32); *off += 32;
    memcpy(query, wire + *off, 16); *off += 16;
    if (!*generation || !nonzero(sender, 32)) return VCS_ZCODE_DHT_ERR_ID_ZERO;
    if (!nonzero(query, 16)) return VCS_ZCODE_DHT_ERR_QUERY_ID;
    if (vcs_zcode_dht_delegation_decode(delegation, wire + *off,
            VCS_ZCODE_DHT_DELEGATION_WIRE_BYTES) !=
        VCS_ZCODE_DHT_DELEGATION_OK) return VCS_ZCODE_DHT_ERR_DELEGATION;
    *off += VCS_ZCODE_DHT_DELEGATION_WIRE_BYTES;
    if (!v->noise_established || !nonzero(v->noise_transcript_hash, 32))
        return VCS_ZCODE_DHT_ERR_SESSION;
    if (vcs_zcode_dht_delegation_verify(
            delegation, v->network_genesis, v->remote_noise_static, 0, NULL,
            v->now_unix) != VCS_ZCODE_DHT_DELEGATION_OK ||
        (v->chain_verify && !v->chain_verify(v->chain_ctx, delegation)))
        return VCS_ZCODE_DHT_ERR_DELEGATION;
    uint8_t derived[32];
    if (!vcs_zcode_dht_delegation_node_id(derived, delegation) ||
        memcmp(derived, sender, 32) != 0)
        return VCS_ZCODE_DHT_ERR_IDENTITY;
    return VCS_ZCODE_DHT_OK;
}

enum vcs_zcode_dht_error vcs_zcode_dht_msg_parse(
    const uint8_t *wire, size_t len,
    const struct vcs_zcode_dht_msg_verify_context *v,
    struct vcs_zcode_dht_msg *out)
{
    if (!wire || !v || !out) return VCS_ZCODE_DHT_ERR_NULL;
    memset(out, 0, sizeof(*out));
    /* Parse into a private scratch object. No authenticated-looking prefix is
     * ever observable after a late signature/session/order rejection. */
    struct vcs_zcode_dht_msg parsed;
    memset(&parsed, 0, sizeof(parsed));
    struct vcs_zcode_dht_msg *dst = &parsed;
    if (len < VCS_ZCODE_DHT_MSGS_HEADER_BYTES + VCS_ZCODE_DHT_MSGS_AUTH_BYTES +
              VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES)
        return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    if (memcmp(wire, msgs_magic, 8) != 0) return VCS_ZCODE_DHT_ERR_WIRE_MAGIC;
    if (zcl_read_u16_le(wire + 8) != VCS_ZCODE_DHT_MSGS_WIRE_VERSION)
        return VCS_ZCODE_DHT_ERR_VERSION;
    uint8_t kind = wire[10];
    if (kind != VCS_ZCODE_DHT_MSG_FIND_NODE && kind != VCS_ZCODE_DHT_MSG_NODES)
        return VCS_ZCODE_DHT_ERR_WIRE_KIND;
    const size_t payload_off = VCS_ZCODE_DHT_MSGS_HEADER_BYTES +
                               VCS_ZCODE_DHT_MSGS_AUTH_BYTES;
    uint32_t count = 0;
    if (kind == VCS_ZCODE_DHT_MSG_FIND_NODE) {
        if (len != VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES)
            return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    } else {
        if (payload_off >= len - VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES)
            return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
        count = wire[payload_off];
        if (count > VCS_ZCODE_DHT_K)
            return VCS_ZCODE_DHT_ERR_LIMIT;
        size_t expected = payload_off + 1 + (size_t)count * 32 +
                          VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES;
        if (len != expected)
            return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    }
    size_t off = VCS_ZCODE_DHT_MSGS_HEADER_BYTES;
    uint64_t *generation = kind == VCS_ZCODE_DHT_MSG_FIND_NODE
        ? &dst->find_node.session_generation : &dst->nodes.session_generation;
    uint8_t *sender = kind == VCS_ZCODE_DHT_MSG_FIND_NODE
        ? dst->find_node.sender_node_id : dst->nodes.sender_node_id;
    uint8_t *query = kind == VCS_ZCODE_DHT_MSG_FIND_NODE
        ? dst->find_node.query_id : dst->nodes.query_id;
    struct vcs_zcode_dht_delegation *delegation =
        kind == VCS_ZCODE_DHT_MSG_FIND_NODE ? &dst->find_node.delegation
                                            : &dst->nodes.delegation;
    enum vcs_zcode_dht_error e = read_auth(
        wire, &off, v, generation, sender, query, delegation);
    if (e != VCS_ZCODE_DHT_OK) return e;
    size_t unsigned_len = len - VCS_ZCODE_DHT_MSG_SIGNATURE_BYTES;
    e = verify_signature(wire, unsigned_len, wire + unsigned_len,
                         v->noise_transcript_hash,
                         delegation->online_pubkey);
    if (e != VCS_ZCODE_DHT_OK) return e;
    if (*generation != v->session_generation) return VCS_ZCODE_DHT_ERR_SESSION;
    if (kind == VCS_ZCODE_DHT_MSG_FIND_NODE) {
        memcpy(dst->find_node.target_node_id, wire + off, 32); off += 32;
        if (!nonzero(dst->find_node.target_node_id, 32))
            return VCS_ZCODE_DHT_ERR_ID_ZERO;
    } else {
        off++;
        for (uint32_t i = 0; i < count; i++) {
            memcpy(dst->nodes.node_ids[i], wire + off, 32); off += 32;
            if (!nonzero(dst->nodes.node_ids[i], 32))
                return VCS_ZCODE_DHT_ERR_ID_ZERO;
            if (i && memcmp(dst->nodes.node_ids[i - 1],
                            dst->nodes.node_ids[i], 32) >= 0)
                return VCS_ZCODE_DHT_ERR_WIRE_ORDER;
        }
        dst->nodes.contact_count = count;
    }
    dst->kind = (enum vcs_zcode_dht_msg_kind)kind;
    *out = parsed;
    return VCS_ZCODE_DHT_OK;
}
