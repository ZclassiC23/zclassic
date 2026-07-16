/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * network_crawler_probe — the DEFAULT real dialer behind the network_crawler
 * probe_fn seam. It opens a SHORT-LIVED clearnet socket to one address OUTSIDE
 * the node's connman, performs a minimal version/verack handshake (Bitnodes
 * pattern), records {version, subver, services, best_height, latency}, then
 * disconnects immediately. It NEVER relays, syncs, or requests blocks; it
 * advertises no services and relay=false. Onion addresses need SOCKS/Tor this
 * direct dialer does not use, so they are recorded as known-but-unmeasured.
 *
 * Kept in its own TU (no sockets in the census fold, no fold logic here) so the
 * fold is unit-testable hermetically and this untested, public-network-dialing
 * code stays isolated. Every read/write is bounded (connect/recv/send timeouts,
 * payload cap, handshake-message cap).
 */

// one-result-type-ok:network-crawler-probe-dialer — this TU is the default
// probe_fn seam; every export/helper returns bool per the ncrawl_probe_fn
// typedef (a recordable-result predicate), not a fallible zcl_result service
// surface. The crawler's fallible lifecycle (network_crawler_start) lives in
// network_crawler.c and returns struct zcl_result.

#include "services/network_crawler.h"

#include "chain/chainparams.h"
#include "core/hash.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "net/netaddr.h"
#include "net/netbase.h"
#include "net/p2p_message.h"
#include "net/protocol.h"
#include "net/version.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define NCRAWL_MAX_MSG_PAYLOAD  (1u << 20) /* 1 MiB cap on any handshake frame */
#define NCRAWL_MAX_HANDSHAKE_MSGS 4        /* frames read while awaiting version */
#define NCRAWL_USER_AGENT "/zclassic23-observatory:1/"

static bool ncrawl_send_all(zcl_socket_t sock, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static bool ncrawl_recv_all(zcl_socket_t sock, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sock, buf + got, len - got, 0);
        if (n <= 0)
            return false; /* 0 = peer closed, <0 = error/timeout */
        got += (size_t)n;
    }
    return true;
}

/* Frame `payload` as a P2P message with the given command and send it. Mirrors
 * p2p_node_end_message: 24-byte header (magic, command, LE length, checksum)
 * then payload; checksum = first 4 bytes of hash256(payload). */
static bool ncrawl_send_framed(zcl_socket_t sock,
                               const struct chain_params *params,
                               const char *command,
                               const struct byte_stream *payload)
{
    struct byte_stream msg;
    stream_init(&msg, MSG_HEADER_SIZE + (payload ? payload->size : 0) + 8);

    struct msg_header hdr;
    msg_header_init_full(&hdr, params->pchMessageStart, command,
                         payload ? (unsigned int)payload->size : 0);
    stream_write(&msg, (const uint8_t *)&hdr, MSG_HEADER_SIZE);
    if (payload && payload->size)
        stream_write(&msg, payload->data, payload->size);
    if (msg.error || msg.size < MSG_HEADER_SIZE) {
        stream_free(&msg);
        return false;
    }

    uint8_t *buf = msg.data;
    unsigned int plen = (unsigned int)(msg.size - MSG_HEADER_SIZE);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE + 0] = (uint8_t)(plen & 0xff);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE + 1] = (uint8_t)((plen >> 8) & 0xff);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE + 2] = (uint8_t)((plen >> 16) & 0xff);
    buf[MESSAGE_START_SIZE + COMMAND_SIZE + 3] = (uint8_t)((plen >> 24) & 0xff);
    struct uint256 h;
    hash256(buf + MSG_HEADER_SIZE, msg.size - MSG_HEADER_SIZE, h.data);
    memcpy(buf + MESSAGE_START_SIZE + COMMAND_SIZE + 4, h.data, 4);

    bool ok = ncrawl_send_all(sock, buf, msg.size);
    stream_free(&msg);
    return ok;
}

static bool ncrawl_send_version(zcl_socket_t sock,
                                const struct chain_params *params,
                                const struct net_address *peer)
{
    struct version_message ver;
    version_message_init(&ver);
    ver.protocol_version = PROTOCOL_VERSION;
    ver.services = 0;                   /* pure observer: advertise nothing */
    ver.timestamp = platform_time_wall_unix();
    ver.addr_recv = *peer;
    ver.addr_recv.nServices = 0;
    ver.nonce = (uint64_t)platform_time_monotonic_us() ^
                ((uint64_t)peer->svc.port << 48);
    snprintf(ver.sub_version, sizeof(ver.sub_version), "%s", NCRAWL_USER_AGENT);
    ver.start_height = 0;
    ver.relay = false;                  /* do not want relayed txs */

    struct byte_stream s;
    stream_init(&s, 256);
    if (!version_message_serialize(&ver, &s)) {
        stream_free(&s);
        return false;
    }
    bool ok = ncrawl_send_framed(sock, params, "version", &s);
    stream_free(&s);
    return ok;
}

/* Read framed messages (bounded) until the peer's `version`, parsed into out. */
static bool ncrawl_read_version(zcl_socket_t sock,
                                const struct chain_params *params,
                                struct ncrawl_probe_result *out)
{
    for (int attempt = 0; attempt < NCRAWL_MAX_HANDSHAKE_MSGS; attempt++) {
        uint8_t hdr[MSG_HEADER_SIZE];
        if (!ncrawl_recv_all(sock, hdr, MSG_HEADER_SIZE))
            return false; // raw-return-ok:crawler probe handshake IO failure is expected per-address, not logged
        if (memcmp(hdr, params->pchMessageStart, MESSAGE_START_SIZE) != 0)
            return false;

        char cmd[COMMAND_SIZE + 1];
        memcpy(cmd, hdr + MESSAGE_START_SIZE, COMMAND_SIZE);
        cmd[COMMAND_SIZE] = '\0';

        unsigned int plen =
            (unsigned int)hdr[MESSAGE_START_SIZE + COMMAND_SIZE] |
            ((unsigned int)hdr[MESSAGE_START_SIZE + COMMAND_SIZE + 1] << 8) |
            ((unsigned int)hdr[MESSAGE_START_SIZE + COMMAND_SIZE + 2] << 16) |
            ((unsigned int)hdr[MESSAGE_START_SIZE + COMMAND_SIZE + 3] << 24);
        if (plen > NCRAWL_MAX_MSG_PAYLOAD)
            return false;

        uint8_t *payload = NULL;
        if (plen) {
            payload = zcl_malloc(plen, "ncrawl_payload");
            if (!payload)
                return false;
            if (!ncrawl_recv_all(sock, payload, plen)) {
                free(payload);
                return false;
            }
        }

        if (strcmp(cmd, "version") == 0) {
            struct byte_stream s;
            stream_init_from_data(&s, payload ? payload : (const uint8_t *)"",
                                  plen);
            struct version_message ver;
            version_message_init(&ver);
            bool ok = version_message_deserialize(&ver, &s);
            free(payload);
            if (!ok)
                return false;
            out->version = ver.protocol_version;
            out->services = ver.services;
            out->best_height = ver.start_height >= 0 ? ver.start_height : -1;
            snprintf(out->subver, sizeof(out->subver), "%s", ver.sub_version);
            return true;
        }
        free(payload);
        /* not the version frame yet — skip and keep reading (bounded) */
    }
    return false;
}

bool network_crawler_default_probe(const struct net_address *addr,
                                   int connect_timeout_ms,
                                   int handshake_timeout_ms,
                                   struct ncrawl_probe_result *out)
{
    if (!addr || !out)
        return false;

    memset(out, 0, sizeof(*out));
    net_service_to_string(&addr->svc, out->addr, sizeof(out->addr));
    out->is_onion = net_addr_is_tor(&addr->svc.addr);
    out->reachable = false;
    out->best_height = -1;
    out->last_probe_us = platform_time_wall_unix();
    if (!out->addr[0])
        return false; /* could not render address → not recordable */

    /* Onion needs SOCKS/Tor this direct dialer does not use: record it as a
     * known node we could not measure over clearnet. */
    if (out->is_onion)
        return true;

    const struct chain_params *params = chain_params_get();
    if (!params)
        return true; /* recorded unreachable */

    zcl_socket_t sock = ZCL_INVALID_SOCKET;
    int64_t t0 = platform_time_monotonic_us();
    if (!connect_socket_directly(&addr->svc, &sock, connect_timeout_ms) ||
        sock == ZCL_INVALID_SOCKET)
        return true; /* recorded unreachable */

    struct timeval tv = millis_to_timeval(handshake_timeout_ms);
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (ncrawl_send_version(sock, params, addr) &&
        ncrawl_read_version(sock, params, out)) {
        (void)ncrawl_send_framed(sock, params, "verack", NULL); /* best-effort */
        out->reachable = true;
        out->latency_us = platform_time_monotonic_us() - t0;
    }

    close_socket(&sock);
    return true;
}
