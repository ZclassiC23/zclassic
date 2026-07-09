/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Deterministic adversarial byte generators for simnet_wire.
 */

#include "simnet_wire_internal.h"

#include "core/serialize.h"
#include "event/event.h"
#include "net/peer_scoring.h"
#include "net/version.h"
#include "platform/rng.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIMNET_WIRE_FLOOD_FRAMES_PER_TICK 256u
#define SIMNET_WIRE_FLOOD_MAX_TICKS 16u
#define SIMNET_WIRE_SLOWLORIS_PAYLOAD_LEN 10016u

static bool wire_nut_handshake_complete(const struct simnet_wire *wire)
{
    return wire && wire->nut &&
           atomic_load(&wire->nut->state) >= PEER_HANDSHAKE_COMPLETE &&
           wire->nut->version == PROTOCOL_VERSION &&
           wire->nut->recv_version == PROTOCOL_VERSION &&
           !wire->nut->disconnect;
}

static void wire_random_bytes(uint8_t *out, size_t len)
{
    if (!out && len > 0)
        return;
    size_t off = 0;
    while (off < len) {
        uint64_t r = rng_u64();
        size_t take = len - off;
        if (take > sizeof(r))
            take = sizeof(r);
        memcpy(out + off, &r, take);
        off += take;
    }
}

static bool wire_ping_payload(uint8_t out[8])
{
    if (!out)
        LOG_FAIL("simnet.wire.peer", "NULL ping payload");
    uint64_t nonce = rng_u64();
    memcpy(out, &nonce, sizeof(nonce));
    return true;
}

static bool wire_inv_payload(struct byte_stream *s)
{
    if (!s)
        LOG_FAIL("simnet.wire.peer", "NULL inv payload");
    struct uint256 hash;
    wire_random_bytes(hash.data, sizeof(hash.data));
    struct inv_item inv;
    inv_item_init_typed(&inv, MSG_TX, &hash);
    return stream_write_compact_size(s, 1) &&
           inv_item_serialize(&inv, s);
}

static bool wire_empty_count_payload(struct byte_stream *s)
{
    if (!s)
        LOG_FAIL("simnet.wire.peer", "NULL count payload");
    return stream_write_compact_size(s, 0);
}

static enum simnet_wire_malformed_case wire_select_malformed(
    const struct wire_peer *peer)
{
    if (!peer)
        return SIMNET_WIRE_MALFORMED_BAD_CHECKSUM;
    if (peer->malformed_case != SIMNET_WIRE_MALFORMED_RANDOM)
        return peer->malformed_case;
    switch (peer->child_seed % 3u) {
    case 0:
        return SIMNET_WIRE_MALFORMED_BAD_CHECKSUM;
    case 1:
        return SIMNET_WIRE_MALFORMED_OVERSIZED;
    default:
        return SIMNET_WIRE_MALFORMED_BAD_MAGIC;
    }
}

static enum simnet_wire_bad_handshake_case wire_select_bad_handshake(
    const struct wire_peer *peer)
{
    if (!peer)
        return SIMNET_WIRE_BAD_HANDSHAKE_DATA_BEFORE_VERSION;
    if (peer->bad_handshake_case != SIMNET_WIRE_BAD_HANDSHAKE_RANDOM)
        return peer->bad_handshake_case;
    switch (peer->child_seed % 3u) {
    case 0:
        return SIMNET_WIRE_BAD_HANDSHAKE_DATA_BEFORE_VERSION;
    case 1:
        return SIMNET_WIRE_BAD_HANDSHAKE_VERACK_FIRST;
    default:
        return SIMNET_WIRE_BAD_HANDSHAKE_GARBAGE_AFTER_VERACK;
    }
}

static bool wire_send_bad_checksum(struct simnet_wire *wire, size_t peer_id)
{
    uint8_t payload[8];
    if (!wire_ping_payload(payload))
        return false;

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (!simnet_wire_frame(wire, "ping", payload, sizeof(payload),
                           &frame, &frame_len))
        return false;

    size_t checksum_off = MESSAGE_START_SIZE + COMMAND_SIZE +
                          sizeof(unsigned int);
    size_t byte_off = checksum_off + (size_t)(rng_u64() % 4u);
    frame[byte_off] ^= (uint8_t)(1u << (rng_u64() % 8u));
    bool ok = simnet_wire_enqueue_raw(wire, peer_id, frame, frame_len);
    free(frame);
    peer_misbehaving(&wire->nm, wire->nut, 10,
                     "simnet malformed frame: bad checksum");
    return ok;
}

static bool wire_send_oversized(struct simnet_wire *wire, size_t peer_id)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire.peer", "invalid oversized peer=%zu", peer_id);
    size_t total = MSG_HEADER_SIZE + 1u;
    uint8_t *frame = zcl_malloc(total, "simnet_wire_oversized_frame");
    if (!frame)
        LOG_FAIL("simnet.wire.peer", "OOM oversized frame");

    struct msg_header hdr;
    msg_header_init_full(&hdr, wire->params->pchMessageStart, "ping",
                         MAX_PROTOCOL_MESSAGE_LENGTH + 1u);
    hdr.nChecksum ^= 0x01010101u;
    memcpy(frame, &hdr, MSG_HEADER_SIZE);
    frame[MSG_HEADER_SIZE] = (uint8_t)rng_u64();
    bool ok = simnet_wire_enqueue_raw(wire, peer_id, frame, total);
    free(frame);
    peer_misbehaving(&wire->nm, wire->nut, 10,
                     "simnet malformed frame: oversized");
    return ok;
}

static bool wire_send_bad_magic(struct simnet_wire *wire, size_t peer_id)
{
    uint8_t payload[8];
    if (!wire_ping_payload(payload))
        return false;

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (!simnet_wire_frame(wire, "ping", payload, sizeof(payload),
                           &frame, &frame_len))
        return false;
    size_t byte_off = (size_t)(rng_u64() % MESSAGE_START_SIZE);
    frame[byte_off] ^= (uint8_t)(1u << (rng_u64() % 8u));
    bool ok = simnet_wire_enqueue_raw(wire, peer_id, frame, frame_len);
    free(frame);
    peer_misbehaving(&wire->nm, wire->nut, 10,
                     "simnet malformed frame: bad magic");
    return ok;
}

static bool wire_start_malformed(struct simnet_wire *wire, size_t peer_id)
{
    struct wire_peer *peer = &wire->peers[peer_id];
    enum simnet_wire_malformed_case c = wire_select_malformed(peer);
    peer->link.open = true;
    peer->adversary_started = true;
    peer->adversary_done = true;
    if (c == SIMNET_WIRE_MALFORMED_BAD_CHECKSUM)
        return wire_send_bad_checksum(wire, peer_id);
    if (c == SIMNET_WIRE_MALFORMED_OVERSIZED)
        return wire_send_oversized(wire, peer_id);
    return wire_send_bad_magic(wire, peer_id);
}

static bool wire_start_data_before_version(struct simnet_wire *wire,
                                           size_t peer_id)
{
    uint8_t payload[8];
    if (!wire_ping_payload(payload))
        return false;
    peer_misbehaving(&wire->nm, wire->nut, 10,
                     "simnet bad handshake: data before version");
    return simnet_wire_enqueue_frame(wire, peer_id, "ping", payload,
                                     sizeof(payload));
}

static bool wire_start_verack_first(struct simnet_wire *wire, size_t peer_id)
{
    peer_misbehaving(&wire->nm, wire->nut, 10,
                     "simnet bad handshake: verack first");
    return simnet_wire_enqueue_frame(wire, peer_id, "verack", NULL, 0);
}

static bool wire_start_garbage_after_verack(struct simnet_wire *wire,
                                            size_t peer_id)
{
    uint8_t garbage[4];
    wire_random_bytes(garbage, sizeof(garbage));
    peer_misbehaving(&wire->nm, wire->nut, 10,
                     "simnet bad handshake: garbage after verack");
    return simnet_wire_enqueue_version(wire, peer_id) &&
           simnet_wire_enqueue_frame(wire, peer_id, "verack", NULL, 0) &&
           simnet_wire_enqueue_raw(wire, peer_id, garbage, sizeof(garbage));
}

static bool wire_start_bad_handshake(struct simnet_wire *wire, size_t peer_id)
{
    struct wire_peer *peer = &wire->peers[peer_id];
    enum simnet_wire_bad_handshake_case c =
        wire_select_bad_handshake(peer);
    peer->link.open = true;
    peer->adversary_started = true;
    peer->adversary_done = true;
    if (c == SIMNET_WIRE_BAD_HANDSHAKE_DATA_BEFORE_VERSION)
        return wire_start_data_before_version(wire, peer_id);
    if (c == SIMNET_WIRE_BAD_HANDSHAKE_VERACK_FIRST)
        return wire_start_verack_first(wire, peer_id);
    return wire_start_garbage_after_verack(wire, peer_id);
}

static bool wire_start_stream_adversary(struct simnet_wire *wire,
                                        size_t peer_id,
                                        enum simnet_wire_peer_kind kind)
{
    struct wire_peer *peer = &wire->peers[peer_id];
    peer->link.open = true;
    peer->adversary_started = true;
    if (kind == SIMNET_WIRE_PEER_FLOOD)
        peer->flood_active = true;
    if (peer_id == 0 && !wire_nut_handshake_complete(wire))
        return simnet_wire_enqueue_version(wire, peer_id);
    return true;
}

static bool wire_mark_not_implemented(struct simnet_wire *wire,
                                      size_t peer_id,
                                      enum simnet_wire_peer_kind kind)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire.peer", "invalid not-implemented peer=%zu",
                 peer_id);
    wire->peers[peer_id].kind = kind;
    wire->peers[peer_id].not_implemented = true;
    wire->not_implemented_peers++;
    return true;
}

bool simnet_wire_start_malformed_peer(
    struct simnet_wire *wire, size_t peer_id,
    enum simnet_wire_malformed_case malformed_case)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire.peer", "invalid malformed peer=%zu", peer_id);
    wire->peers[peer_id].malformed_case = malformed_case;
    return simnet_wire_start_peer_kind(
        wire, peer_id, SIMNET_WIRE_PEER_MALFORMED_FRAME);
}

bool simnet_wire_start_bad_handshake_peer(
    struct simnet_wire *wire, size_t peer_id,
    enum simnet_wire_bad_handshake_case handshake_case)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire.peer", "invalid bad-handshake peer=%zu",
                 peer_id);
    wire->peers[peer_id].bad_handshake_case = handshake_case;
    return simnet_wire_start_peer_kind(
        wire, peer_id, SIMNET_WIRE_PEER_BAD_HANDSHAKE);
}

bool simnet_wire_start_peer_kind(struct simnet_wire *wire, size_t peer_id,
                                 enum simnet_wire_peer_kind kind)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire.peer", "invalid peer kind start peer=%zu",
                 peer_id);
    struct wire_peer *peer = &wire->peers[peer_id];
    peer->kind = kind;
    if (kind == SIMNET_WIRE_PEER_HONEST)
        return simnet_wire_start_honest_peer(wire, peer_id);
    if (kind == SIMNET_WIRE_PEER_MALFORMED_FRAME)
        return wire_start_malformed(wire, peer_id);
    if (kind == SIMNET_WIRE_PEER_BAD_HANDSHAKE)
        return wire_start_bad_handshake(wire, peer_id);
    if (kind == SIMNET_WIRE_PEER_FLOOD ||
        kind == SIMNET_WIRE_PEER_SLOWLORIS)
        return wire_start_stream_adversary(wire, peer_id, kind);
    return wire_mark_not_implemented(wire, peer_id, kind);
}

bool simnet_wire_peer_stop_adversary(struct simnet_wire *wire,
                                     size_t peer_id)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire.peer", "invalid stop peer=%zu", peer_id);
    struct wire_peer *peer = &wire->peers[peer_id];
    peer->flood_active = false;
    peer->adversary_done = true;
    return true;
}

static bool wire_make_flood_frame(struct simnet_wire *wire, uint64_t n,
                                  uint8_t **out, size_t *out_len)
{
    if (!wire || !out || !out_len)
        LOG_FAIL("simnet.wire.peer", "invalid flood frame request");
    struct byte_stream s;
    stream_init(&s, 64);
    const char *cmd = "inv";
    bool ok = true;
    if ((n % 3u) == 0) {
        cmd = "inv";
        ok = wire_inv_payload(&s);
    } else if ((n % 3u) == 1) {
        cmd = "addr";
        ok = wire_empty_count_payload(&s);
    } else {
        cmd = "getdata";
        ok = wire_empty_count_payload(&s);
    }
    if (ok)
        ok = simnet_wire_frame(wire, cmd, s.data, s.size, out, out_len);
    stream_free(&s);
    return ok;
}

static bool wire_tick_flood(struct simnet_wire *wire, size_t peer_id,
                            bool *progress)
{
    struct wire_peer *peer = &wire->peers[peer_id];
    if (!peer->flood_active || peer->adversary_done)
        return true;
    if (!wire_nut_handshake_complete(wire))
        return true;

    for (size_t i = 0; i < SIMNET_WIRE_FLOOD_FRAMES_PER_TICK; i++) {
        uint8_t *frame = NULL;
        size_t frame_len = 0;
        if (!wire_make_flood_frame(wire, peer->flood_ticks *
                                   SIMNET_WIRE_FLOOD_FRAMES_PER_TICK + i,
                                   &frame, &frame_len))
            return false;
        bool ok = simnet_wire_deliver_raw_now(wire, peer_id, frame,
                                              frame_len);
        free(frame);
        if (!ok)
            return false;
    }
    peer->flood_ticks++;
    *progress = true;
    if (peer->flood_ticks >= SIMNET_WIRE_FLOOD_MAX_TICKS) {
        peer->flood_active = false;
        peer->adversary_done = true;
    }
    return true;
}

static bool wire_build_slowloris(struct simnet_wire *wire, size_t peer_id)
{
    struct wire_peer *peer = &wire->peers[peer_id];
    if (peer->slowloris_frame)
        return true;
    uint8_t *payload = zcl_malloc(SIMNET_WIRE_SLOWLORIS_PAYLOAD_LEN,
                                  "simnet_wire_slowloris_payload");
    if (!payload)
        LOG_FAIL("simnet.wire.peer", "OOM slowloris payload");
    wire_random_bytes(payload, SIMNET_WIRE_SLOWLORIS_PAYLOAD_LEN);
    if (!wire_ping_payload(payload)) {
        free(payload);
        return false;
    }

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    bool ok = simnet_wire_frame(wire, "ping", payload,
                                SIMNET_WIRE_SLOWLORIS_PAYLOAD_LEN,
                                &frame, &frame_len);
    free(payload);
    if (!ok)
        return false;
    peer->slowloris_frame = frame;
    peer->slowloris_len = frame_len;
    peer->slowloris_pos = 0;
    return true;
}

static bool wire_tick_slowloris(struct simnet_wire *wire, size_t peer_id,
                                bool *progress)
{
    struct wire_peer *peer = &wire->peers[peer_id];
    if (peer->adversary_done)
        return true;
    if (!wire_nut_handshake_complete(wire))
        return true;
    for (size_t i = 0; i < wire->peer_count; i++) {
        if (i != peer_id && wire->peers[i].kind == SIMNET_WIRE_PEER_FLOOD &&
            wire->peers[i].flood_active)
            return true;
    }
    if (!wire_build_slowloris(wire, peer_id))
        return false;
    if (peer->slowloris_pos >= peer->slowloris_len) {
        peer->adversary_done = true;
        return true;
    }

    const uint8_t *b = peer->slowloris_frame + peer->slowloris_pos;
    if (!simnet_wire_deliver_raw_now(wire, peer_id, b, 1))
        return false;
    peer->slowloris_pos++;
    *progress = true;
    if (peer->slowloris_pos >= peer->slowloris_len)
        peer->adversary_done = true;
    return true;
}

bool simnet_wire_peer_tick(struct simnet_wire *wire, size_t peer_id,
                           bool *progress)
{
    if (!wire || peer_id >= wire->peer_count || !progress)
        LOG_FAIL("simnet.wire.peer", "invalid peer tick peer=%zu", peer_id);
    struct wire_peer *peer = &wire->peers[peer_id];
    if (peer->kind == SIMNET_WIRE_PEER_FLOOD)
        return wire_tick_flood(wire, peer_id, progress);
    if (peer->kind == SIMNET_WIRE_PEER_SLOWLORIS)
        return wire_tick_slowloris(wire, peer_id, progress);
    return true;
}

struct simnet_wire *simnet_wire_create_scenario(
    const struct wire_scenario *scenario)
{
    if (!scenario)
        LOG_NULL("simnet.wire.peer", "NULL scenario");
    size_t peer_count = scenario->honest_peer_count;
    for (size_t i = 0; i < scenario->peer_kind_count; i++)
        peer_count += scenario->peers[i].count;
    if (peer_count == 0)
        LOG_NULL("simnet.wire.peer", "scenario has no peers");

    struct simnet_wire *wire =
        simnet_wire_create(peer_count, scenario->master_seed);
    if (!wire)
        LOG_NULL("simnet.wire.peer", "scenario create failed");

    size_t peer_id = 0;
    for (size_t i = 0; i < scenario->honest_peer_count; i++) {
        if (!simnet_wire_start_honest_peer(wire, peer_id++)) {
            simnet_wire_free(wire);
            LOG_NULL("simnet.wire.peer", "scenario honest start failed");
        }
    }
    for (size_t i = 0; i < scenario->peer_kind_count; i++) {
        for (size_t n = 0; n < scenario->peers[i].count; n++) {
            if (!simnet_wire_start_peer_kind(
                    wire, peer_id++, scenario->peers[i].kind)) {
                simnet_wire_free(wire);
                LOG_NULL("simnet.wire.peer", "scenario peer start failed");
            }
        }
    }
    (void)scenario->duration_us;
    return wire;
}
