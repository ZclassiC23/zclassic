/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Deterministic in-memory P2P wire transport for a real p2p_node.
 */

#include "sim/simnet_wire.h"

#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "consensus/validation.h"
#include "core/hash.h"
#include "core/serialize.h"
#include "net/fast_sync.h"
#include "net/msgprocessor.h"
#include "net/net.h"
#include "net/p2p_message.h"
#include "net/protocol.h"
#include "net/version.h"
#include "platform/clock.h"
#include "platform/rng.h"
#include "platform/time_compat.h"
#include "primitives/block.h"
#include "sim/seed_tape.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define SIMNET_WIRE_EVENT_ENQUEUED 129u
#define SIMNET_WIRE_MIN_LATENCY_US 50u
#define SIMNET_WIRE_LATENCY_SPAN_US 5000u
#define SIMNET_WIRE_REORDER_SPAN_US 17u
#define SIMNET_WIRE_DEFAULT_RING_CAP 4096u
#define SIMNET_WIRE_MAX_PEERS 1024u
#define SIMNET_WIRE_IO_CHUNK_BASE 0x10000u
#define SIMNET_WIRE_RECV_LOW_WATER_SLOTS 16u
#define SIMNET_WIRE_FNV_OFFSET 1469598103934665603ULL
#define SIMNET_WIRE_FNV_PRIME 1099511628211ULL

enum wire_event_kind {
    WIRE_EVENT_DELIVER_TO_NUT = 1,
    WIRE_EVENT_OPEN = 2,
    WIRE_EVENT_CLOSE = 3,
    WIRE_EVENT_PARTITION = 4,
};

enum wire_peer_kind {
    WIRE_PEER_HONEST = 1,
};

struct wire_byte_ring {
    uint8_t *data;
    size_t cap;
    size_t head;
    size_t len;
};

struct wire_link {
    struct wire_byte_ring to_nut;
    struct wire_byte_ring to_peer;
    bool open;
    size_t down_tokens;
    size_t up_tokens;
};

struct wire_event {
    size_t peer_id;
    uint64_t deliver_us;
    uint64_t seq;
    enum wire_event_kind kind;
    uint8_t *bytes;
    size_t len;
};

struct wire_peer {
    enum wire_peer_kind kind;
    struct wire_link link;
    struct net_address addr;
    bool version_sent;
    bool verack_sent;
    bool saw_nut_version;
    bool saw_nut_verack;
    bool saw_nut_sendheaders;
    bool saw_nut_pong;
    uint64_t ping_nonce_sent;
    uint64_t last_pong_nonce;
};

struct simnet_wire {
    size_t peer_count;
    struct wire_peer *peers;
    seed_tape_t *tape;

    struct net_manager nm;
    struct msg_processor mp;
    struct main_state ms;
    struct tx_mempool mempool;
    struct coins_view null_view;
    struct coins_view_cache coins_tip;
    const struct chain_params *params;
    struct p2p_node *nut;
    struct send_segment *send_sentinel;
    bool main_ready;
    bool mempool_ready;
    bool coins_ready;
    bool net_ready;

    struct wire_event *queue;
    size_t queue_count;
    size_t queue_cap;
    uint64_t next_seq;
    uint64_t fingerprint;
    uint64_t ticks;
    uint64_t delivered_to_nut_bytes;
    uint64_t delivered_to_peer_bytes;
};

struct wire_event_record {
    uint64_t peer_id;
    uint64_t deliver_us;
    uint64_t seq;
    uint64_t len;
    uint64_t bytes_hash;
    uint8_t kind;
};

static uint64_t simnet_wire_now_us(void)
{
    int64_t ns = clock_now_monotonic_ns();
    if (ns <= 0)
        return 0;
    return (uint64_t)ns / 1000u;
}

static uint64_t simnet_wire_fnv_bytes(const uint8_t *bytes, size_t len)
{
    uint64_t h = SIMNET_WIRE_FNV_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= SIMNET_WIRE_FNV_PRIME;
    }
    return h;
}

static void simnet_wire_fp_mix(struct simnet_wire *wire, size_t peer_id,
                               uint8_t direction, const uint8_t *bytes,
                               size_t len)
{
    uint64_t h = wire->fingerprint ? wire->fingerprint
                                   : SIMNET_WIRE_FNV_OFFSET;
    h ^= (uint64_t)peer_id + 0x9e3779b97f4a7c15ULL;
    h *= SIMNET_WIRE_FNV_PRIME;
    h ^= direction;
    h *= SIMNET_WIRE_FNV_PRIME;
    h ^= (uint64_t)len;
    h *= SIMNET_WIRE_FNV_PRIME;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= SIMNET_WIRE_FNV_PRIME;
    }
    wire->fingerprint = h;
}

static bool ring_init(struct wire_byte_ring *ring, size_t cap)
{
    if (!ring)
        LOG_FAIL("simnet.wire", "NULL ring init");
    memset(ring, 0, sizeof(*ring));
    if (cap == 0)
        cap = SIMNET_WIRE_DEFAULT_RING_CAP;
    ring->data = zcl_malloc(cap, "simnet_wire_ring");
    if (!ring->data)
        LOG_FAIL("simnet.wire", "OOM allocating ring cap=%zu", cap);
    ring->cap = cap;
    return true;
}

static void ring_free(struct wire_byte_ring *ring)
{
    if (!ring)
        return;
    free(ring->data);
    memset(ring, 0, sizeof(*ring));
}

static size_t ring_available(const struct wire_byte_ring *ring)
{
    return ring ? ring->len : 0;
}

static bool ring_copy_out(const struct wire_byte_ring *ring, size_t off,
                          uint8_t *out, size_t len)
{
    if (!ring || (!out && len > 0) || off > ring->len ||
        len > ring->len - off)
        LOG_FAIL("simnet.wire", "invalid ring copy off=%zu len=%zu", off,
                 len);
    for (size_t i = 0; i < len; i++)
        out[i] = ring->data[(ring->head + off + i) % ring->cap];
    return true;
}

static bool ring_grow(struct wire_byte_ring *ring, size_t need)
{
    if (!ring)
        LOG_FAIL("simnet.wire", "NULL ring grow");
    if (need <= ring->cap)
        return true;

    size_t new_cap = ring->cap ? ring->cap : SIMNET_WIRE_DEFAULT_RING_CAP;
    while (new_cap < need)
        new_cap *= 2u;

    uint8_t *grown = zcl_malloc(new_cap, "simnet_wire_ring_grow");
    if (!grown)
        LOG_FAIL("simnet.wire", "OOM growing ring to %zu", new_cap);
    if (ring->len > 0 && !ring_copy_out(ring, 0, grown, ring->len)) {
        free(grown);
        return false;
    }
    free(ring->data);
    ring->data = grown;
    ring->cap = new_cap;
    ring->head = 0;
    return true;
}

static bool ring_write(struct wire_byte_ring *ring, const uint8_t *bytes,
                       size_t len)
{
    if (!ring || (!bytes && len > 0))
        LOG_FAIL("simnet.wire", "invalid ring write len=%zu", len);
    if (!ring_grow(ring, ring->len + len))
        return false;
    size_t tail = (ring->head + ring->len) % ring->cap;
    for (size_t i = 0; i < len; i++)
        ring->data[(tail + i) % ring->cap] = bytes[i];
    ring->len += len;
    return true;
}

static const uint8_t *ring_linear_ptr(const struct wire_byte_ring *ring,
                                      size_t *out_len)
{
    if (!ring || !out_len)
        return NULL;
    if (ring->len == 0) {
        *out_len = 0;
        return NULL;
    }
    size_t until_end = ring->cap - ring->head;
    *out_len = ring->len < until_end ? ring->len : until_end;
    return ring->data + ring->head;
}

static bool ring_drop(struct wire_byte_ring *ring, size_t len)
{
    if (!ring || len > ring->len)
        LOG_FAIL("simnet.wire", "invalid ring drop len=%zu", len);
    ring->head = (ring->head + len) % ring->cap;
    ring->len -= len;
    if (ring->len == 0)
        ring->head = 0;
    return true;
}

static bool ring_read(struct wire_byte_ring *ring, uint8_t *out, size_t len)
{
    if (!ring_copy_out(ring, 0, out, len))
        return false;
    return ring_drop(ring, len);
}

static void simnet_wire_init_peer_addr(struct net_address *addr,
                                       size_t peer_id,
                                       const struct chain_params *params)
{
    static const unsigned char base_ip[4] = {198, 51, 100, 1};
    unsigned char ip[4];

    net_address_init(addr);
    addr->nServices = NODE_NETWORK;
    memcpy(ip, base_ip, sizeof(ip));
    ip[3] = (unsigned char)(10u + (peer_id % 200u));
    net_addr_set_ipv4(&addr->svc.addr, ip);
    addr->svc.port = params ? (uint16_t)params->nDefaultPort : 8033u;
}

static bool simnet_wire_stub_submit_block(struct block *block,
                                          struct validation_state *out,
                                          void *ctx)
{
    (void)block;
    (void)ctx;
    if (out)
        validation_state_error(out, "simnet-wire-threadless-block-submit");
    LOG_FAIL("simnet.wire", "unexpected block submit in wire harness");
}

static bool simnet_wire_install_send_sentinel(struct simnet_wire *wire)
{
    if (!wire || !wire->nut)
        LOG_FAIL("simnet.wire", "invalid sentinel install");
    struct send_segment *sentinel =
        zcl_calloc(1, sizeof(*sentinel), "simnet_wire_send_sentinel");
    if (!sentinel)
        LOG_FAIL("simnet.wire", "OOM allocating send sentinel");

    zcl_mutex_lock(&wire->nut->cs_send);
    wire->nut->send_head = sentinel;
    wire->nut->send_tail = sentinel;
    wire->nut->send_offset = 0;
    zcl_mutex_unlock(&wire->nut->cs_send);

    wire->send_sentinel = sentinel;
    return true;
}

static bool simnet_wire_init_runtime(struct simnet_wire *wire, uint64_t seed)
{
    wire->tape = seed_tape_open(seed, 1700000000);
    if (!wire->tape)
        LOG_FAIL("simnet.wire", "seed_tape_open failed");
    seed_tape_install(wire->tape);

    wire->params = chain_params_get();

    main_state_init(&wire->ms);
    wire->main_ready = true;
    tx_mempool_init(&wire->mempool, 0);
    wire->mempool_ready = true;
    memset(&wire->null_view, 0, sizeof(wire->null_view));
    coins_view_cache_init(&wire->coins_tip, &wire->null_view);
    wire->coins_ready = true;
    net_manager_init(&wire->nm);
    wire->net_ready = true;
    memcpy(wire->nm.message_start, wire->params->pchMessageStart,
           MESSAGE_START_SIZE);
    wire->nm.default_port = (uint16_t)wire->params->nDefaultPort;
    wire->nm.local_services = NODE_NETWORK;
    wire->nm.local_host_nonce = rng_u64();
    if (wire->nm.local_host_nonce == 0)
        wire->nm.local_host_nonce = 1;

    memset(&wire->mp, 0, sizeof(wire->mp));
    wire->mp.main_state = &wire->ms;
    wire->mp.mempool = &wire->mempool;
    wire->mp.coins_tip = &wire->coins_tip;
    wire->mp.params = wire->params;
    wire->mp.datadir = ".";
    wire->mp.net_mgr = &wire->nm;
    wire->mp.block_submit = simnet_wire_stub_submit_block;
    wire->mp.compact_block_submit = simnet_wire_stub_submit_block;

    struct net_address addr;
    simnet_wire_init_peer_addr(&addr, 0, wire->params);
    wire->nut = p2p_node_create(&wire->nm, ZCL_INVALID_SOCKET, &addr,
                                "simnet-wire-peer0", true);
    if (!wire->nut)
        LOG_FAIL("simnet.wire", "p2p_node_create failed");
    wire->nut->services = NODE_NETWORK;
    wire->nut->network_node = true;
    wire->nut->relay_txes = true;

    return simnet_wire_install_send_sentinel(wire);
}

static bool simnet_wire_event_less(const struct wire_event *a,
                                   const struct wire_event *b)
{
    if (a->deliver_us != b->deliver_us)
        return a->deliver_us < b->deliver_us;
    return a->seq < b->seq;
}

static bool simnet_wire_find_next_event(const struct simnet_wire *wire,
                                        size_t *out_idx)
{
    if (!wire || !out_idx)
        LOG_FAIL("simnet.wire", "invalid event scan");
    if (wire->queue_count == 0)
        return false;

    size_t best = 0;
    for (size_t i = 1; i < wire->queue_count; i++) {
        if (simnet_wire_event_less(&wire->queue[i], &wire->queue[best]))
            best = i;
    }
    *out_idx = best;
    return true;
}

static void simnet_wire_remove_event(struct simnet_wire *wire, size_t idx)
{
    if (!wire || idx >= wire->queue_count)
        return;
    free(wire->queue[idx].bytes);
    wire->queue[idx] = wire->queue[wire->queue_count - 1];
    wire->queue_count--;
}

static bool simnet_wire_ensure_queue(struct simnet_wire *wire)
{
    if (!wire)
        LOG_FAIL("simnet.wire", "NULL queue owner");
    if (wire->queue_count < wire->queue_cap)
        return true;
    size_t new_cap = wire->queue_cap ? wire->queue_cap * 2u : 16u;
    struct wire_event *grown =
        zcl_realloc(wire->queue, new_cap * sizeof(*grown),
                    "simnet_wire_events");
    if (!grown)
        LOG_FAIL("simnet.wire", "OOM growing event queue to %zu", new_cap);
    wire->queue = grown;
    wire->queue_cap = new_cap;
    return true;
}

static bool simnet_wire_enqueue(struct simnet_wire *wire, size_t peer_id,
                                enum wire_event_kind kind,
                                const uint8_t *bytes, size_t len)
{
    if (!wire || peer_id >= wire->peer_count || (!bytes && len > 0))
        LOG_FAIL("simnet.wire", "invalid enqueue peer=%zu len=%zu",
                 peer_id, len);
    if (!simnet_wire_ensure_queue(wire))
        return false;

    uint64_t latency =
        SIMNET_WIRE_MIN_LATENCY_US +
        (rng_u64() % SIMNET_WIRE_LATENCY_SPAN_US);
    uint64_t reorder = rng_u64() % SIMNET_WIRE_REORDER_SPAN_US;
    uint64_t deliver_us = simnet_wire_now_us() + latency + reorder;

    uint8_t *copy = NULL;
    if (len > 0) {
        copy = zcl_malloc(len, "simnet_wire_event_bytes");
        if (!copy)
            LOG_FAIL("simnet.wire", "OOM copying event bytes len=%zu", len);
        memcpy(copy, bytes, len);
    }

    struct wire_event *ev = &wire->queue[wire->queue_count++];
    memset(ev, 0, sizeof(*ev));
    ev->peer_id = peer_id;
    ev->deliver_us = deliver_us;
    ev->seq = wire->next_seq++;
    ev->kind = kind;
    ev->bytes = copy;
    ev->len = len;

    struct wire_event_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = peer_id;
    rec.deliver_us = deliver_us;
    rec.seq = ev->seq;
    rec.len = len;
    rec.bytes_hash = simnet_wire_fnv_bytes(bytes, len);
    rec.kind = (uint8_t)kind;
    int rc = seed_tape_inject(wire->tape, SIMNET_WIRE_EVENT_ENQUEUED,
                              &rec, sizeof(rec));
    if (rc != 0)
        LOG_FAIL("simnet.wire", "seed_tape_inject failed rc=%d", rc);
    return true;
}

static bool simnet_wire_frame(struct simnet_wire *wire, const char *command,
                              const uint8_t *payload, size_t payload_len,
                              uint8_t **out, size_t *out_len)
{
    if (!wire || !command || !out || !out_len ||
        (!payload && payload_len > 0))
        LOG_FAIL("simnet.wire", "invalid frame request");
    if (payload_len > MAX_PROTOCOL_MESSAGE_LENGTH)
        LOG_FAIL("simnet.wire", "payload too large len=%zu", payload_len);

    size_t total = MSG_HEADER_SIZE + payload_len;
    uint8_t *frame = zcl_malloc(total, "simnet_wire_frame");
    if (!frame)
        LOG_FAIL("simnet.wire", "OOM allocating frame len=%zu", total);

    struct msg_header hdr;
    msg_header_init_full(&hdr, wire->params->pchMessageStart, command,
                         (unsigned int)payload_len);
    struct uint256 checksum;
    hash256(payload_len ? payload : (const uint8_t *)"",
            payload_len, checksum.data);
    memcpy(&hdr.nChecksum, checksum.data, sizeof(hdr.nChecksum));

    memcpy(frame, &hdr, MSG_HEADER_SIZE);
    if (payload_len > 0)
        memcpy(frame + MSG_HEADER_SIZE, payload, payload_len);

    *out = frame;
    *out_len = total;
    return true;
}

static bool simnet_wire_enqueue_frame(struct simnet_wire *wire, size_t peer_id,
                                      const char *command,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (!simnet_wire_frame(wire, command, payload, payload_len,
                           &frame, &frame_len))
        return false;
    bool ok = simnet_wire_enqueue(wire, peer_id,
                                  WIRE_EVENT_DELIVER_TO_NUT,
                                  frame, frame_len);
    free(frame);
    return ok;
}

static bool simnet_wire_enqueue_verack(struct simnet_wire *wire,
                                       size_t peer_id)
{
    return simnet_wire_enqueue_frame(wire, peer_id, "verack", NULL, 0);
}

static bool simnet_wire_enqueue_pong(struct simnet_wire *wire, size_t peer_id,
                                     uint64_t nonce)
{
    struct byte_stream s;
    stream_init(&s, 8);
    bool ok = stream_write_u64_le(&s, nonce) &&
              simnet_wire_enqueue_frame(wire, peer_id, "pong", s.data,
                                        s.size);
    stream_free(&s);
    if (!ok)
        LOG_FAIL("simnet.wire", "failed to enqueue pong");
    return true;
}

static bool simnet_wire_enqueue_version(struct simnet_wire *wire,
                                        size_t peer_id)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire", "invalid version peer=%zu", peer_id);

    struct wire_peer *peer = &wire->peers[peer_id];
    struct version_message ver;
    version_message_init(&ver);
    ver.protocol_version = PROTOCOL_VERSION;
    ver.services = NODE_NETWORK;
    ver.timestamp = (int64_t)platform_time_wall_time_t();
    ver.addr_recv = peer->addr;
    ver.addr_from = peer->addr;
    ver.nonce = rng_u64();
    if (ver.nonce == 0 || ver.nonce == wire->nm.local_host_nonce)
        ver.nonce ^= 0xa5a5a5a55a5a5a5aULL;
    if (ver.nonce == 0 || ver.nonce == wire->nm.local_host_nonce)
        ver.nonce++;
    snprintf(ver.sub_version, sizeof(ver.sub_version), "/simnet-wire:0.1/");
    ver.start_height = 0;
    ver.relay = true;

    struct byte_stream s;
    stream_init(&s, 128);
    bool ok = version_message_serialize(&ver, &s) &&
              simnet_wire_enqueue_frame(wire, peer_id, "version", s.data,
                                        s.size);
    stream_free(&s);
    if (!ok)
        LOG_FAIL("simnet.wire", "failed to enqueue version");
    peer->version_sent = true;
    return true;
}

static bool simnet_wire_deliver_one_event(struct simnet_wire *wire,
                                          bool *progress)
{
    if (!wire || !progress)
        LOG_FAIL("simnet.wire", "invalid delivery args");
    size_t idx = 0;
    if (!simnet_wire_find_next_event(wire, &idx))
        return true;

    struct wire_event ev = wire->queue[idx];
    uint64_t now = simnet_wire_now_us();
    if (ev.deliver_us > now) {
        uint64_t delta = ev.deliver_us - now;
        if (delta > (uint64_t)INT64_MAX)
            LOG_FAIL("simnet.wire", "delivery delta too large");
        int rc = seed_tape_advance(wire->tape, (int64_t)delta);
        if (rc != 0)
            LOG_FAIL("simnet.wire", "seed_tape_advance failed rc=%d", rc);
        *progress = true;
    }

    struct wire_peer *peer = &wire->peers[ev.peer_id];
    if (ev.kind == WIRE_EVENT_DELIVER_TO_NUT && peer->link.open) {
        if (!ring_write(&peer->link.to_nut, ev.bytes, ev.len))
            return false;
        *progress = true;
    } else if (ev.kind == WIRE_EVENT_OPEN) {
        peer->link.open = true;
        *progress = true;
    } else if (ev.kind == WIRE_EVENT_CLOSE) {
        peer->link.open = false;
        *progress = true;
    }

    simnet_wire_remove_event(wire, idx);
    return true;
}

static size_t simnet_wire_recv_cap_for_queue(size_t queued, size_t base_cap)
{
    if (queued >= MAX_RECV_MESSAGES)
        return 0;

    size_t free_slots = MAX_RECV_MESSAGES - queued;
    if (free_slots < SIMNET_WIRE_RECV_LOW_WATER_SLOTS) {
        size_t cap = free_slots * (size_t)MSG_HEADER_SIZE;
        if (cap < base_cap)
            return cap;
    }
    return base_cap;
}

static bool simnet_wire_pump_to_nut(struct simnet_wire *wire, size_t peer_id,
                                    bool *progress)
{
    if (!wire || peer_id >= wire->peer_count || !progress)
        LOG_FAIL("simnet.wire", "invalid ingress pump peer=%zu", peer_id);
    struct wire_peer *peer = &wire->peers[peer_id];
    struct p2p_node *node = wire->nut;

    while (peer->link.open && !node->disconnect &&
           ring_available(&peer->link.to_nut) > 0) {
        zcl_mutex_lock(&node->cs_recv);
        size_t queued = node->recv_msg_count;
        if (queued >= MAX_RECV_MESSAGES) {
            zcl_mutex_unlock(&node->cs_recv);
            break;
        }

        size_t linear_len = 0;
        const uint8_t *ptr = ring_linear_ptr(&peer->link.to_nut, &linear_len);
        if (!ptr || linear_len == 0) {
            zcl_mutex_unlock(&node->cs_recv);
            break;
        }

        size_t cap = simnet_wire_recv_cap_for_queue(
            queued, SIMNET_WIRE_IO_CHUNK_BASE);
        if (peer->link.down_tokens < cap)
            cap = peer->link.down_tokens;
        if (cap == 0) {
            zcl_mutex_unlock(&node->cs_recv);
            break;
        }
        if (cap > linear_len)
            cap = linear_len;
        size_t chunk = 1u + (rng_u64() % cap);

        bool ok = p2p_node_receive_bytes(node, (const char *)ptr,
                                         (unsigned int)chunk,
                                         wire->params->pchMessageStart);
        if (!ok) {
            node->disconnect = true;
            for (size_t i = 0; i < node->recv_msg_count; i++)
                net_message_free(&node->recv_msgs[i]);
            node->recv_msg_count = 0;
            zcl_mutex_unlock(&node->cs_recv);
            LOG_FAIL("simnet.wire", "NUT rejected inbound bytes");
        }
        node->last_recv = platform_time_wall_time_t();
        node->recv_bytes += (uint64_t)chunk;
        if (peer->link.down_tokens != SIZE_MAX)
            peer->link.down_tokens -= chunk;
        zcl_mutex_unlock(&node->cs_recv);

        simnet_wire_fp_mix(wire, peer_id, 1, ptr, chunk);
        wire->delivered_to_nut_bytes += chunk;
        if (!ring_drop(&peer->link.to_nut, chunk))
            return false;
        *progress = true;
    }
    return true;
}

static bool simnet_wire_peer_handle_frame(struct simnet_wire *wire,
                                          size_t peer_id,
                                          const uint8_t *frame,
                                          size_t frame_len)
{
    if (!wire || peer_id >= wire->peer_count || !frame ||
        frame_len < MSG_HEADER_SIZE)
        LOG_FAIL("simnet.wire", "invalid peer frame");

    struct msg_header hdr;
    memcpy(&hdr, frame, sizeof(hdr));
    if (!msg_header_is_valid(&hdr, wire->params->pchMessageStart))
        return false;
    size_t payload_len = hdr.nMessageSize;
    if (frame_len != MSG_HEADER_SIZE + payload_len)
        LOG_FAIL("simnet.wire", "frame length mismatch");

    const uint8_t *payload = frame + MSG_HEADER_SIZE;
    struct uint256 checksum;
    hash256(payload_len ? payload : (const uint8_t *)"",
            payload_len, checksum.data);
    unsigned int expected = 0;
    memcpy(&expected, checksum.data, sizeof(expected));
    if (expected != hdr.nChecksum)
        LOG_FAIL("simnet.wire", "peer saw checksum mismatch");

    char cmd[COMMAND_SIZE + 1];
    msg_header_get_command(&hdr, cmd, sizeof(cmd));
    struct wire_peer *peer = &wire->peers[peer_id];

    if (strcmp(cmd, "version") == 0) {
        struct byte_stream s;
        struct version_message ver;
        version_message_init(&ver);
        stream_init_from_data(&s, payload, payload_len);
        bool ok = version_message_deserialize(&ver, &s);
        stream_free(&s);
        if (!ok)
            LOG_FAIL("simnet.wire", "peer failed to parse NUT version");
        peer->saw_nut_version = true;
        if (!peer->verack_sent) {
            if (!simnet_wire_enqueue_verack(wire, peer_id))
                return false;
            peer->verack_sent = true;
        }
    } else if (strcmp(cmd, "verack") == 0) {
        peer->saw_nut_verack = true;
    } else if (strcmp(cmd, "sendheaders") == 0) {
        peer->saw_nut_sendheaders = true;
    } else if (strcmp(cmd, "ping") == 0) {
        struct byte_stream s;
        uint64_t nonce = 0;
        stream_init_from_data(&s, payload, payload_len);
        bool ok = stream_read_u64_le(&s, &nonce);
        stream_free(&s);
        if (ok && !simnet_wire_enqueue_pong(wire, peer_id, nonce))
            return false;
    } else if (strcmp(cmd, "pong") == 0) {
        struct byte_stream s;
        uint64_t nonce = 0;
        stream_init_from_data(&s, payload, payload_len);
        bool ok = stream_read_u64_le(&s, &nonce);
        stream_free(&s);
        if (!ok)
            LOG_FAIL("simnet.wire", "peer failed to parse pong");
        peer->saw_nut_pong = true;
        peer->last_pong_nonce = nonce;
    }

    return true;
}

static bool simnet_wire_peer_process(struct simnet_wire *wire, size_t peer_id,
                                     bool *progress)
{
    if (!wire || peer_id >= wire->peer_count || !progress)
        LOG_FAIL("simnet.wire", "invalid peer process peer=%zu", peer_id);
    struct wire_peer *peer = &wire->peers[peer_id];

    while (ring_available(&peer->link.to_peer) >= MSG_HEADER_SIZE) {
        struct msg_header hdr;
        if (!ring_copy_out(&peer->link.to_peer, 0, (uint8_t *)&hdr,
                           sizeof(hdr)))
            return false;
        if (hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH)
            LOG_FAIL("simnet.wire", "peer frame too large: %u",
                     hdr.nMessageSize);
        size_t total = MSG_HEADER_SIZE + (size_t)hdr.nMessageSize;
        if (ring_available(&peer->link.to_peer) < total)
            break;

        uint8_t *frame = zcl_malloc(total, "simnet_wire_peer_frame");
        if (!frame)
            LOG_FAIL("simnet.wire", "OOM allocating peer frame len=%zu",
                     total);
        bool ok = ring_read(&peer->link.to_peer, frame, total) &&
                  simnet_wire_peer_handle_frame(wire, peer_id, frame, total);
        free(frame);
        if (!ok)
            return false;
        *progress = true;
    }

    return true;
}

static bool simnet_wire_drain_nut_send(struct simnet_wire *wire,
                                       bool *progress)
{
    if (!wire || !wire->nut || !wire->send_sentinel || !progress)
        LOG_FAIL("simnet.wire", "invalid egress drain");
    struct p2p_node *node = wire->nut;

    zcl_mutex_lock(&node->cs_send);
    while (wire->send_sentinel->next) {
        struct send_segment *seg = wire->send_sentinel->next;
        wire->send_sentinel->next = seg->next;
        if (node->send_tail == seg)
            node->send_tail = wire->send_sentinel;
        if (node->send_size >= seg->size)
            node->send_size -= seg->size;
        else
            node->send_size = 0;
        node->send_offset = 0;

        size_t peer_id = 0;
        if (wire->peer_count > 0 && wire->peers[peer_id].link.open) {
            struct wire_peer *peer = &wire->peers[peer_id];
            if (!ring_write(&peer->link.to_peer, seg->data, seg->size)) {
                zcl_mutex_unlock(&node->cs_send);
                return false;
            }
            simnet_wire_fp_mix(wire, peer_id, 2, seg->data, seg->size);
            wire->delivered_to_peer_bytes += seg->size;
            if (peer->link.up_tokens != SIZE_MAX &&
                peer->link.up_tokens >= seg->size)
                peer->link.up_tokens -= seg->size;
            *progress = true;
        }
        send_segment_free(seg);
    }
    node->send_head = wire->send_sentinel;
    node->send_tail = wire->send_sentinel;
    zcl_mutex_unlock(&node->cs_send);
    return true;
}

static uint64_t simnet_wire_progress_signature(const struct simnet_wire *wire)
{
    if (!wire)
        return 0;
    uint64_t h = wire->fingerprint ^ wire->queue_count;
    h ^= wire->delivered_to_nut_bytes << 7;
    h ^= wire->delivered_to_peer_bytes << 13;
    if (wire->nut) {
        h ^= (uint64_t)wire->nut->recv_msg_count << 21;
        h ^= (uint64_t)wire->nut->send_size << 29;
        h ^= wire->nut->disconnect ? 0xfeed0001ULL : 0;
    }
    for (size_t i = 0; i < wire->peer_count; i++) {
        h ^= ring_available(&wire->peers[i].link.to_nut) << (i % 17u);
        h ^= ring_available(&wire->peers[i].link.to_peer) << ((i + 5u) % 23u);
        h ^= wire->peers[i].saw_nut_pong ? (0xbeefULL + i) : 0;
    }
    return h;
}

static bool simnet_wire_idle(const struct simnet_wire *wire)
{
    if (!wire || wire->queue_count > 0)
        return false;
    if (wire->nut &&
        (wire->nut->recv_msg_count > 0 || wire->nut->send_size > 0))
        return false;
    for (size_t i = 0; i < wire->peer_count; i++) {
        if (ring_available(&wire->peers[i].link.to_nut) > 0 ||
            ring_available(&wire->peers[i].link.to_peer) > 0)
            return false;
    }
    return true;
}

static bool simnet_wire_tick(struct simnet_wire *wire, bool *progress)
{
    if (!wire || !progress)
        LOG_FAIL("simnet.wire", "invalid tick args");
    *progress = false;

    if (!simnet_wire_deliver_one_event(wire, progress))
        return false;
    for (size_t i = 0; i < wire->peer_count; i++) {
        if (!simnet_wire_pump_to_nut(wire, i, progress))
            return false;
    }
    if (!msg_process_messages(&wire->mp, wire->nut))
        LOG_FAIL("simnet.wire", "msg_process_messages failed");
    if (!simnet_wire_drain_nut_send(wire, progress))
        return false;
    for (size_t i = 0; i < wire->peer_count; i++) {
        if (!simnet_wire_peer_process(wire, i, progress))
            return false;
    }
    wire->ticks++;
    return true;
}

struct simnet_wire *simnet_wire_create(size_t peer_count, uint64_t seed)
{
    if (peer_count == 0 || peer_count > SIMNET_WIRE_MAX_PEERS)
        LOG_NULL("simnet.wire", "invalid peer_count=%zu", peer_count);

    struct simnet_wire *wire =
        zcl_calloc(1, sizeof(*wire), "simnet_wire");
    if (!wire)
        LOG_NULL("simnet.wire", "OOM allocating simnet_wire");

    wire->peer_count = peer_count;
    wire->fingerprint = SIMNET_WIRE_FNV_OFFSET;
    wire->next_seq = 1;
    wire->peers = zcl_calloc(peer_count, sizeof(*wire->peers),
                             "simnet_wire_peers");
    if (!wire->peers) {
        simnet_wire_free(wire);
        LOG_NULL("simnet.wire", "OOM allocating peers count=%zu",
                 peer_count);
    }

    if (!simnet_wire_init_runtime(wire, seed)) {
        simnet_wire_free(wire);
        LOG_NULL("simnet.wire", "runtime init failed");
    }

    for (size_t i = 0; i < peer_count; i++) {
        struct wire_peer *peer = &wire->peers[i];
        peer->kind = WIRE_PEER_HONEST;
        peer->link.open = false;
        peer->link.down_tokens = SIZE_MAX;
        peer->link.up_tokens = SIZE_MAX;
        simnet_wire_init_peer_addr(&peer->addr, i, wire->params);
        if (!ring_init(&peer->link.to_nut, SIMNET_WIRE_DEFAULT_RING_CAP) ||
            !ring_init(&peer->link.to_peer, SIMNET_WIRE_DEFAULT_RING_CAP)) {
            simnet_wire_free(wire);
            LOG_NULL("simnet.wire", "peer ring init failed index=%zu", i);
        }
    }

    return wire;
}

void simnet_wire_free(struct simnet_wire *wire)
{
    if (!wire)
        return;

    if (wire->nut)
        p2p_node_free(wire->nut);
    if (wire->net_ready)
        net_manager_free(&wire->nm);
    if (wire->coins_ready)
        coins_view_cache_free(&wire->coins_tip);
    if (wire->mempool_ready)
        tx_mempool_free(&wire->mempool);
    if (wire->main_ready)
        main_state_free(&wire->ms);

    if (wire->peers) {
        for (size_t i = 0; i < wire->peer_count; i++) {
            ring_free(&wire->peers[i].link.to_nut);
            ring_free(&wire->peers[i].link.to_peer);
        }
        free(wire->peers);
    }
    if (wire->queue) {
        for (size_t i = 0; i < wire->queue_count; i++)
            free(wire->queue[i].bytes);
        free(wire->queue);
    }
    if (wire->tape) {
        seed_tape_uninstall();
        seed_tape_close(wire->tape);
    }
    free(wire);
}

struct p2p_node *simnet_wire_node(struct simnet_wire *wire)
{
    return wire ? wire->nut : NULL;
}

bool simnet_wire_start_honest_peer(struct simnet_wire *wire, size_t peer_id)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire", "invalid start peer=%zu", peer_id);
    struct wire_peer *peer = &wire->peers[peer_id];
    peer->link.open = true;
    return simnet_wire_enqueue_version(wire, peer_id);
}

bool simnet_wire_peer_send_ping(struct simnet_wire *wire, size_t peer_id,
                                uint64_t nonce)
{
    if (!wire || peer_id >= wire->peer_count)
        LOG_FAIL("simnet.wire", "invalid ping peer=%zu", peer_id);
    struct byte_stream s;
    stream_init(&s, 8);
    bool ok = stream_write_u64_le(&s, nonce) &&
              simnet_wire_enqueue_frame(wire, peer_id, "ping", s.data,
                                        s.size);
    stream_free(&s);
    if (!ok)
        LOG_FAIL("simnet.wire", "failed to enqueue ping");
    wire->peers[peer_id].ping_nonce_sent = nonce;
    return true;
}

bool simnet_wire_run(struct simnet_wire *wire, uint64_t max_ticks,
                     uint64_t stuck_guard)
{
    if (!wire || max_ticks == 0)
        LOG_FAIL("simnet.wire", "invalid run max_ticks=%llu",
                 (unsigned long long)max_ticks);

    uint64_t stuck = 0;
    for (uint64_t i = 0; i < max_ticks; i++) {
        if (simnet_wire_idle(wire))
            return true;
        uint64_t before = simnet_wire_progress_signature(wire);
        bool tick_progress = false;
        if (!simnet_wire_tick(wire, &tick_progress))
            return false;
        uint64_t after = simnet_wire_progress_signature(wire);
        if (tick_progress || after != before) {
            stuck = 0;
        } else {
            stuck++;
            if (stuck_guard > 0 && stuck >= stuck_guard)
                LOG_FAIL("simnet.wire", "stuck after %llu ticks",
                         (unsigned long long)stuck);
        }
    }

    LOG_FAIL("simnet.wire", "max_ticks exhausted ticks=%llu pending=%zu",
             (unsigned long long)max_ticks, wire->queue_count);
}

bool simnet_wire_peer_handshake_complete(const struct simnet_wire *wire,
                                         size_t peer_id)
{
    if (!wire || peer_id >= wire->peer_count || !wire->nut)
        return false;
    const struct wire_peer *peer = &wire->peers[peer_id];
    return peer->version_sent &&
           peer->verack_sent &&
           peer->saw_nut_version &&
           peer->saw_nut_verack &&
           atomic_load(&wire->nut->state) >= PEER_HANDSHAKE_COMPLETE &&
           wire->nut->version == PROTOCOL_VERSION &&
           wire->nut->recv_version == PROTOCOL_VERSION &&
           !wire->nut->disconnect;
}

bool simnet_wire_peer_pong_received(const struct simnet_wire *wire,
                                    size_t peer_id, uint64_t nonce)
{
    if (!wire || peer_id >= wire->peer_count)
        return false;
    const struct wire_peer *peer = &wire->peers[peer_id];
    return peer->saw_nut_pong && peer->last_pong_nonce == nonce;
}

uint64_t simnet_wire_fingerprint(const struct simnet_wire *wire)
{
    return wire ? wire->fingerprint : 0;
}

bool simnet_wire_get_stats(const struct simnet_wire *wire,
                           struct simnet_wire_stats *out)
{
    if (!wire || !out)
        LOG_FAIL("simnet.wire", "invalid stats request");
    memset(out, 0, sizeof(*out));
    out->ticks = wire->ticks;
    out->delivered_to_nut_bytes = wire->delivered_to_nut_bytes;
    out->delivered_to_peer_bytes = wire->delivered_to_peer_bytes;
    out->fingerprint = wire->fingerprint;
    out->rng_count = seed_tape_rng_count(wire->tape);
    out->pending_events = wire->queue_count;
    out->nut_disconnected = wire->nut ? wire->nut->disconnect : true;
    if (wire->peer_count > 0) {
        out->to_nut_bytes = ring_available(&wire->peers[0].link.to_nut);
        out->to_peer_bytes = ring_available(&wire->peers[0].link.to_peer);
        out->handshake_complete =
            simnet_wire_peer_handshake_complete(wire, 0);
        out->pong_received = wire->peers[0].saw_nut_pong;
    }
    return true;
}
