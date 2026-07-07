/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* msg_version.c — Version/verack handshake processing.
 * Split from msgprocessor.c for maintainability. */

#include "platform/time_compat.h"
#include "net/msg_internal.h"
#include "net/addrman.h"
#include "net/version.h"
#include "net/peer_identity.h"
#include "net/p2p_message.h"
#include "net/file_service.h"
#include "net/peer_lifecycle.h"
#include "net/port_policy.h"
#include "net/peer_scoring.h"
#include "net/fast_sync.h"
#include "core/serialize.h"
#include "util/timedata.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "jobs/reducer_frontier.h"  // lib-layer-ok:provable-tip-served-to-peers
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

/* External IP for addr advertisement — set by boot from -externalip= */
static uint8_t g_external_ip[4];
static uint16_t g_external_port;
static bool g_has_external_ip = false;

static bool msg_version_parse_external_ip(const char *ip_str,
                                          uint16_t default_port,
                                          uint8_t out_ip[4],
                                          uint16_t *out_port)
{
    char host[INET_ADDRSTRLEN];
    const char *port_part = NULL;
    const char *colon;
    struct in_addr addr;

    if (!ip_str || ip_str[0] == '\0')
        LOG_RETURN(false, "net", "externalip is empty");

    colon = strchr(ip_str, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - ip_str);
        if (host_len == 0 || host_len >= sizeof(host))
            LOG_RETURN(false, "net", "invalid externalip host: %s", ip_str);
        memcpy(host, ip_str, host_len);
        host[host_len] = '\0';
        port_part = colon + 1;
        if (port_part[0] == '\0')
            LOG_RETURN(false, "net", "externalip port is empty: %s", ip_str);
    } else {
        size_t host_len = strlen(ip_str);
        if (host_len >= sizeof(host))
            LOG_RETURN(false, "net", "invalid externalip host: %s", ip_str);
        memcpy(host, ip_str, host_len + 1);
    }

    if (inet_pton(AF_INET, host, &addr) != 1)
        LOG_RETURN(false, "net", "externalip is not IPv4: %s", ip_str);

    if (port_part) {
        char *end = NULL;
        unsigned long parsed;

        errno = 0;
        parsed = strtoul(port_part, &end, 10);
        if (errno != 0 || end == port_part || *end != '\0' ||
            parsed == 0 || parsed > 65535)
            LOG_RETURN(false, "net", "invalid externalip port: %s", ip_str);
        default_port = (uint16_t)parsed;
    }

    memcpy(out_ip, &addr, 4);
    *out_port = default_port;
    return true;
}

static void msg_version_save_peer(struct msg_processor *mp,
                                  const struct p2p_node *node)
{
    if (!mp || !node)
        return;
    if (!msg_version_should_save_peer(node)) {
        peer_lifecycle_note_cache_skipped(node, "inbound_ephemeral_port");
        return;
    }
    if (mp->peer_save)
        mp->peer_save(node, mp->peer_save_ctx);
}

void msg_version_set_external_ip(const char *ip_str, uint16_t port)
{
    uint8_t ip[4];
    uint16_t parsed_port = port;
    char printable_ip[INET_ADDRSTRLEN];
    struct in_addr addr;

    if (!msg_version_parse_external_ip(ip_str, port, ip, &parsed_port))
        return;

    memcpy(g_external_ip, ip, 4);
    g_external_port = parsed_port;
    g_has_external_ip = true;

    memcpy(&addr, g_external_ip, 4);
    if (inet_ntop(AF_INET, &addr, printable_ip, sizeof(printable_ip)))
        printf("External IP configured: %s:%u\n", printable_ip,
               g_external_port);
}

bool msg_version_get_external_ip(char *buf, size_t buflen, uint16_t *port)
{
    if (!g_has_external_ip) return false;
    struct in_addr addr;
    memcpy(&addr, g_external_ip, 4);
    if (!inet_ntop(AF_INET, &addr, buf, (socklen_t)buflen)) return false;
    if (port) *port = g_external_port;
    return true;
}

bool msg_version_should_save_peer(const struct p2p_node *node)
{
    if (!node)
        return false;
    if (!node->inbound)
        return true;

    /* Inbound sockets usually carry the remote node's ephemeral source port.
     * Persisting that tuple turns every reconnect into a separate peer-cache
     * row. The reachable listen address is learned from version.addr_from and
     * stored in addrman instead. */
    return zcl_net_port_is_reachable_candidate(node->addr.svc.port);
}

#ifdef ZCL_TESTING
void msg_version_clear_external_ip_for_test(void)
{
    memset(g_external_ip, 0, sizeof(g_external_ip));
    g_external_port = 0;
    g_has_external_ip = false;
}
#endif

const char *msg_version_user_agent(void)
{
    return "/ZClassic23:0.1.0/";
}

static bool msg_version_subver_is_zcl23(const char *subver)
{
    return subver &&
           (strstr(subver, "ZClassic23") != NULL ||
            strstr(subver, "ZClassic-C23") != NULL);
}

bool msg_version_classify_peer(const char *subver, uint64_t services,
                               bool *is_magicbean, bool *is_zcl23)
{
    bool mb = subver && strstr(subver, "MagicBean") != NULL;
    bool z23 = peer_supports_fast_sync(services) ||
               msg_version_subver_is_zcl23(subver);
    if (is_magicbean) *is_magicbean = mb;
    if (is_zcl23) *is_zcl23 = z23;
    return mb || z23;
}

static bool addr_name_host_is_external(const char *addr_name)
{
    char ext[INET_ADDRSTRLEN] = {0};
    uint16_t port = 0;
    size_t host_len;

    if (!addr_name || !msg_version_get_external_ip(ext, sizeof(ext), &port))
        return false;

    host_len = strcspn(addr_name, ":");
    return host_len == strlen(ext) &&
           strncmp(addr_name, ext, host_len) == 0;
}

bool msg_version_peer_uses_external_host(const struct p2p_node *node)
{
    char host[ZCL_PEER_HOST_KEY_MAX];

    if (!node)
        return false;
    if (zcl_peer_host_key(node, host, sizeof(host)) &&
        addr_name_host_is_external(host))
        return true;
    return addr_name_host_is_external(node->addr_name);
}

static bool msg_version_addr_is_external_self(const struct net_address *addr)
{
    if (!addr || !g_has_external_ip || !net_addr_is_ipv4(&addr->svc.addr))
        return false;
    if (memcmp(addr->svc.addr.ip + 12, g_external_ip, 4) != 0)
        return false;
    return g_external_port == 0 || addr->svc.port == g_external_port;
}

bool msg_version_learn_advertised_addr(struct net_manager *nm,
                                       const struct p2p_node *node,
                                       const struct version_message *ver)
{
    if (!nm || !node || !ver || !node->inbound)
        return false;
    if (ver->addr_from.svc.port == 0)
        return false;
    if (!net_addr_is_routable(&ver->addr_from.svc.addr))
        return false;
    if (msg_version_addr_is_external_self(&ver->addr_from))
        return false;

    struct net_address learned = ver->addr_from;
    if (learned.nTime == 0)
        learned.nTime = (uint32_t)platform_time_wall_time_t();
    if (learned.nServices == 0)
        learned.nServices = ver->services;

    struct net_addr source = node->addr.svc.addr;
    return addrman_add(&nm->addrman, &learned, &source, 0);
}

void msg_version_build(struct version_message *ver,
                       const struct msg_processor *mp,
                       const struct p2p_node *node,
                       int start_height)
{
    if (!ver || !mp || !node)
        return;
    version_message_init(ver);
    ver->protocol_version = PROTOCOL_VERSION;
    ver->services = NODE_NETWORK | NODE_ZCL23;
    if (bip37_enabled())
        ver->services |= NODE_BLOOM;
    ver->timestamp = (int64_t)platform_time_wall_time_t();
    ver->addr_recv = node->addr;
    if (g_has_external_ip) {
        ver->addr_from.nServices = ver->services;
        ver->addr_from.nTime = (uint32_t)platform_time_wall_time_t();
        net_addr_set_ipv4(&ver->addr_from.svc.addr, g_external_ip);
        ver->addr_from.svc.port = g_external_port;
    }
    ver->nonce = mp->net_mgr->local_host_nonce;
    /* User-agent: advertise the native ZClassic23 identity. Legacy
     * /MagicBean:.../ peers remain accepted by the classifier; this string is
     * only our public product identity, not consensus or network magic. */
    snprintf(ver->sub_version, sizeof(ver->sub_version), "%s",
             msg_version_user_agent());
    ver->start_height = start_height;
    ver->relay = true;
}

void push_version(struct msg_processor *mp, struct p2p_node *node)
{
    struct version_message ver;
    /* Advertise the PROVABLE tip (H*), not the sync-window/lookahead tip:
     * start_height is an external claim to peers about where our chain ends,
     * so it must be the height we can prove, never one that can rewind under a
     * reorg. Lock-free cached atomic — see reducer_frontier_provable_tip_cached.
     * (void) the window arg's owner intentionally: internal window readers stay
     * on active_chain_height; only this OUTWARD claim switches to H*.) */
    int start_height = reducer_frontier_external_tip_height();
    msg_version_build(&ver, mp, node, start_height);

    struct byte_stream s;
    stream_init(&s, 256);
    version_message_serialize(&ver, &s);

    p2p_node_begin_message(node, "version", mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);

    stream_free(&s);
    peer_lifecycle_note_version_sent(node, ver.services, ver.start_height,
                                     ver.sub_version);
}

void push_verack(struct msg_processor *mp, struct p2p_node *node)
{
    p2p_node_begin_message(node, "verack", mp->params->pchMessageStart);
    p2p_node_end_message(node);
}

bool process_version(struct msg_processor *mp, struct p2p_node *node,
                     struct byte_stream *s)
{
    if (node->version != 0) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "duplicate version from %s", node->addr_name);
        LOG_FAIL("net", "duplicate version from peer %s", node->addr_name);
    }

    struct version_message ver;
    version_message_init(&ver);
    if (!version_message_deserialize(&ver, s))
        LOG_FAIL("net", "failed to deserialize version message from %s",
                 node->addr_name);

    if (ver.protocol_version < MIN_PEER_PROTO_VERSION) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "proto %d too old (min %d) %s",
                    ver.protocol_version, MIN_PEER_PROTO_VERSION,
                    node->addr_name);
        node->disconnect = true;
        peer_lifecycle_note_reject(node, "protocol-too-old");
        LOG_FAIL("net", "proto version %d too old (min %d) from %s",
                 ver.protocol_version, MIN_PEER_PROTO_VERSION,
                 node->addr_name);
    }

    /* Self-connection detection: if the peer's nonce matches our own
     * local_host_nonce, we are connecting to ourselves. Disconnect. */
    if (ver.nonce == mp->net_mgr->local_host_nonce &&
        mp->net_mgr->local_host_nonce != 0) {
        event_emitf(EV_TCP_DISCONNECTED, (uint32_t)node->id,
                    "self-connection %s", node->addr_name);
        node->disconnect = true;
        peer_lifecycle_note_reject(node, "self-connection");
        LOG_FAIL("net", "self-connection detected for %s", node->addr_name);
    }

    node->version = ver.protocol_version;
    node->services = ver.services;
    strncpy(node->sub_ver, ver.sub_version, MAX_SUBVERSION_LENGTH - 1);
    node->sub_ver[MAX_SUBVERSION_LENGTH - 1] = '\0';
    strncpy(node->clean_sub_ver, ver.sub_version, MAX_SUBVERSION_LENGTH - 1);
    node->clean_sub_ver[MAX_SUBVERSION_LENGTH - 1] = '\0';
    node->starting_height = ver.start_height;
    node->time_offset = ver.timestamp - (int64_t)platform_time_wall_time_t();
    node->relay_txes = ver.relay;

    event_emitf(EV_PEER_VERSION, (uint32_t)node->id,
                "proto=%d h=%d %s", ver.protocol_version,
                ver.start_height, ver.sub_version);
    peer_lifecycle_note_version_received(node, ver.services,
                                         ver.start_height, ver.sub_version);
    if (msg_version_learn_advertised_addr(mp->net_mgr, node, &ver)) {
        char learned[72];
        net_service_to_string(&ver.addr_from.svc, learned, sizeof(learned));
        printf("Peer %s: learned advertised address %s from inbound version\n",
               node->addr_name, learned);
    }

    /* Ignore duplicate version messages from peers already past handshake */
    if (node->state >= PEER_HANDSHAKE_COMPLETE) {
        printf("Peer %s: ignoring duplicate version (already %s)\n",
               node->addr_name, peer_state_name(node->state));
        return true;
    }
    peer_set_state_checked((uint32_t)node->id, &node->state,
                           PEER_VERSION_RECEIVED, "version msg received");

    if (!node->inbound) {
        AddTimeData((const unsigned char *)node->addr_name,
                    (int)strlen(node->addr_name), node->time_offset);
    }

    push_verack(mp, node);

    if (node->inbound)
        push_version(mp, node);

    /* For outbound connections, we already sent version; now we received
     * their version and sent verack. Mark connected once we also get their
     * verack (handled in process_verack). For inbound, the peer initiated,
     * so mark connected after we send our version+verack. */
    if (node->inbound) {
        peer_set_state_checked((uint32_t)node->id, &node->state,
                               PEER_HANDSHAKE_COMPLETE, "inbound version+verack");
        peer_lifecycle_note_handshake_complete(node);
    }

    /* Ask outbound peers for their address list */
    if (!node->inbound && !node->get_addr) {
        p2p_node_begin_message(node, "getaddr", mp->params->pchMessageStart);
        p2p_node_end_message(node);
        node->get_addr = true;
    }

    /* Send sendheaders — tells peer we prefer headers announcements
     * over inv. Critical for headers-first sync with legacy zclassicd. */
    p2p_node_begin_message(node, "sendheaders", mp->params->pchMessageStart);
    p2p_node_end_message(node);

    /* Advertise our external address to this peer so it can relay us */
    if (g_has_external_ip) {
        struct net_address self;
        memset(&self, 0, sizeof(self));
        self.nServices = NODE_NETWORK;
        self.nTime = (uint32_t)platform_time_wall_time_t();
        net_addr_set_ipv4(&self.svc.addr, g_external_ip);
        self.svc.port = g_external_port;
        p2p_node_push_address(node, &self);
    }

    event_emitf(EV_PEER_VERSION, (uint32_t)node->id,
                "%s v=%d h=%d %s%s",
                node->addr_name, node->version, node->starting_height,
                node->sub_ver,
                peer_supports_fast_sync(node->services) ? " [ZCL23]" : "");

    /* Detect zclassic23 peers via subversion string.
     * Service bit detection is secondary — some peers filter unknown bits. */
    bool is_magicbean = false;
    bool is_zcl23 = false;
    msg_version_classify_peer(node->sub_ver, node->services,
                              &is_magicbean, &is_zcl23);
    (void)is_magicbean;
    if (is_zcl23) {
        node->services |= NODE_ZCL23; /* mark for fast sync */
        node->swarm_inflight_chunk = -1;
        for (int pi = 0; pi < 4; pi++)
            node->blk_pipeline[pi].piece_index = -1;
        printf("Peer %s: supports zclassic23 fast sync [ZCL23]\n",
               node->addr_name);

        /* Exchange UTXO manifests — both peers announce what they have. */
        push_manifest(mp, node);

        /* Exchange block piece manifests for parallel block sync. */
        push_block_manifest(mp, node);

        /* Advertise file service port. The peer knows our IP from the
         * TCP connection — we just tell them which port to connect to
         * for the fast file service. They cache it in SQLite for
         * sticky reconnection across restarts.
         * Message: "zfileaddr" with [2-byte port]. */
        {
            uint8_t faddr[2];
            uint16_t fport = fs_server_get_port();
            memcpy(faddr, &fport, 2);

            struct byte_stream fs_msg;
            stream_init(&fs_msg, 4);
            stream_write_bytes(&fs_msg, faddr, 2);
            p2p_node_begin_message(node, "zfileaddr",
                                    mp->params->pchMessageStart);
            p2p_node_write_message_data(node, fs_msg.data, fs_msg.size);
            p2p_node_end_message(node);
            stream_free(&fs_msg);
        }
    }

    return true;
}

bool process_verack(struct msg_processor *mp, struct p2p_node *node)
{
    node->recv_version = PROTOCOL_VERSION;
    peer_lifecycle_note_verack_received(node);

    /* Outbound: handshake complete (we sent version, got version+verack).
     * Inbound: already marked in process_version. */
    if (!node->inbound && node->state < PEER_HANDSHAKE_COMPLETE) {
        peer_set_state_checked((uint32_t)node->id, &node->state,
                               PEER_HANDSHAKE_COMPLETE, "verack received");
        peer_lifecycle_note_handshake_complete(node);
        printf("Peer %s: handshake complete (outbound)\n", node->addr_name);
    } else if (node->state < PEER_HANDSHAKE_COMPLETE) {
        peer_set_state_checked((uint32_t)node->id, &node->state,
                               PEER_HANDSHAKE_COMPLETE, "verack received");
        peer_lifecycle_note_handshake_complete(node);
        printf("Peer %s: verack received\n", node->addr_name);
    }

    /* Mark peer as good in addrman — increases selection priority */
    if (mp->net_mgr) {
        addrman_good(&mp->net_mgr->addrman, &node->addr.svc,
                      (int64_t)platform_time_wall_time_t());
        addrman_connected(&mp->net_mgr->addrman, &node->addr.svc,
                           (int64_t)platform_time_wall_time_t());
    }

    /* Aggressive peer exchange with ZCL23 nodes — don't wait for getaddr.
     * Push all known addresses immediately so both nodes build their
     * address books fast. This is the key to low-friction peer discovery:
     * every ZCL23 handshake floods addresses in both directions. */
    if (peer_supports_fast_sync(node->services) && mp->net_mgr) {
        struct net_address addrs[2500];
        size_t num = addrman_get_addr(&mp->net_mgr->addrman, addrs, 2500);
        if (num > 0) {
            struct byte_stream addr_msg;
            stream_init(&addr_msg, num * 30 + 8);
            stream_write_compact_size(&addr_msg, num);
            for (size_t i = 0; i < num; i++)
                net_address_serialize(&addrs[i], &addr_msg, true);
            p2p_node_begin_message(node, "addr",
                                    mp->params->pchMessageStart);
            p2p_node_write_message_data(node, addr_msg.data, addr_msg.size);
            p2p_node_end_message(node);
            stream_free(&addr_msg);
            printf("Peer %s: pushed %zu addresses (ZCL23 peer exchange)\n",
                   node->addr_name, num);
        }
    }

    /* Peer persistence is advisory and caller-owned; the app shell decides
     * whether to enqueue a model write so handshake processing stays a net
     * protocol concern. */
    msg_version_save_peer(mp, node);
    return true;
}
