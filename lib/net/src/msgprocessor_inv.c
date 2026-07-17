/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Inventory / peer-discovery message family:
 *   inv, getdata, notfound — block/tx availability advertising,
 *   addr, getaddr           — peer address gossip.
 *
 * The actual block/tx accept logic lives in msg_blocks.c and
 * msg_tx.c; the address-manager lives in addrman.c. These handlers
 * are thin adapters / forwarders for the dispatch table. */

#include "msgprocessor_internal.h"
#include "net/msg_bounds_guard.h"
#include "net/addrman.h"
#include "net/download.h"
#include "net/peer_scoring.h"
#include "core/uint256.h"
#include "core/utiltime.h"
#include "storage/topology_store.h"
#include "util/log_macros.h"
#include <stdio.h>
#include <string.h>

bool mp_handle_inv(struct msg_processor *mp, struct p2p_node *node,
                   struct byte_stream *s)
{
    return process_inv(mp, node, s);
}

bool mp_handle_getdata(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s)
{
    return process_getdata(mp, node, s);
}

static bool process_notfound(struct msg_processor *mp, struct p2p_node *node,
                             struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        LOG_FAIL("net", "failed to read notfound count from %s",
                 node->addr_name);

    /* notfound answers a getdata, so bound it by the same cap as inv/getdata
     * (the 2 MB frame cap already limits this, but cap at the call site so the
     * discipline is auditable and uniform across inv-bearing handlers). */
    if (msg_count_exceeds("net", "notfound", count, MAX_INV_SZ,
                          node->addr_name)) {
        /* Score like every other oversized-count flood (inv/getdata/addr/
         * headers) — see msg_tx.c::process_inv for why disconnect alone
         * lets a hostile peer repeat this forever across reconnects. */
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_FLOOD,
                            "notfound count exceeds MAX_INV_SZ");
        node->disconnect = true;
        return false;
    }

    struct download_manager *dm = get_download_mgr();
    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s))
            LOG_FAIL("net", "failed to deserialize notfound inv[%llu] from %s",
                     (unsigned long long)i, node->addr_name);
        if (inv.type == MSG_BLOCK) {
            char hex[65];
            uint256_get_hex(&inv.hash, hex);
            printf("Peer %s: notfound block %s\n", node->addr_name, hex);
            /* Re-queue so another peer can try */
            dl_peer_disconnected(dm, (uint32_t)node->id);
        }
    }
    return true;
}

bool mp_handle_notfound(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    return process_notfound(mp, node, s);
}

/* A single addr message is already bounded at MAX_ADDR_TO_SEND (1000)
 * entries by the check above, but nothing previously stopped a peer from
 * repeating max-legal-size batches back-to-back forever for free — cheap
 * CPU/addrman-churn amplification with zero score cost. Fixed 60s window,
 * cap generous enough for legitimate bursts (an unsolicited self-announce
 * plus a getaddr response, each up to MAX_ADDR_TO_SEND) while still
 * catching sustained repetition. */
#define ADDR_RATE_WINDOW_SECS 60
#define ADDR_RATE_MAX_PER_WINDOW (MAX_ADDR_TO_SEND * 3)

static bool process_addr(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        LOG_FAIL("net", "failed to read addr count from %s",
                 node->addr_name);

    if (msg_count_exceeds("net", "addr", count, MAX_ADDR_TO_SEND,
                          node->addr_name)) {
        /* Score like every other oversized-count flood (inv/getdata/
         * notfound/headers) — see msg_tx.c::process_inv for why
         * disconnect alone lets a hostile peer repeat this forever
         * across reconnects. */
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_FLOOD,
                            "addr count exceeds MAX_ADDR_TO_SEND");
        printf("Peer %s: addr message too large (%llu)\n",
               node->addr_name, (unsigned long long)count);
        node->disconnect = true;
        return false;
    }

    /* Per-peer addr rate limit: a fixed window over TOTAL addresses
     * received (not message count), so one giant batch and many small
     * batches are both bounded the same way. */
    {
        int64_t now = GetTime();
        if (node->addr_rate_window_start == 0 ||
            now - node->addr_rate_window_start >= ADDR_RATE_WINDOW_SECS) {
            node->addr_rate_window_start = now;
            node->addr_rate_window_count = 0;
        }
        node->addr_rate_window_count += (uint32_t)count;
        if (node->addr_rate_window_count > ADDR_RATE_MAX_PER_WINDOW) {
            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_FLOOD,
                                "addr rate limit exceeded");
            printf("Peer %s: addr rate limit exceeded (%u in %ds)\n",
                   node->addr_name, node->addr_rate_window_count,
                   ADDR_RATE_WINDOW_SECS);
            node->disconnect = true;
            return false;
        }
    }

    struct net_addr source;
    net_addr_init(&source);
    memcpy(source.ip, node->addr.svc.addr.ip, 16);

    /* Topology graph edge: node (the observer — our already-connected,
     * already-handshaked peer) told us about each address below. now stamped
     * once per message (not per address) — plenty precise for a
     * first-seen/last-seen graph edge. topology_store_record_edge()
     * independently PEDANTIC-validates (net_addr_is_routable) and renders
     * BOTH endpoints before storage; a not-open store or a rejected address
     * is a silent no-op here (observational only, never gates addrman). */
    int64_t topo_now = GetTime();

    for (uint64_t i = 0; i < count; i++) {
        struct net_address addr;
        net_address_init(&addr);
        if (!net_address_deserialize(&addr, s, true))
            LOG_FAIL("net", "failed to deserialize addr[%llu] from %s",
                     (unsigned long long)i, node->addr_name);

        if (mp->net_mgr)
            addrman_add(&mp->net_mgr->addrman, &addr, &source, 0);
        (void)topology_store_record_edge(&node->addr.svc.addr,
                                         node->addr.svc.port, &addr.svc.addr,
                                         addr.svc.port, topo_now, NULL);
    }
    return true;
}

bool mp_handle_addr(struct msg_processor *mp, struct p2p_node *node,
                    struct byte_stream *s)
{
    return process_addr(mp, node, s);
}

static bool process_getaddr(struct msg_processor *mp, struct p2p_node *node)
{
    if (node->sent_addr)
        return true;
    node->sent_addr = true;

    if (!mp->net_mgr)
        return true;

    struct net_address addrs[MAX_ADDR_TO_SEND];
    size_t num = addrman_get_addr(&mp->net_mgr->addrman, addrs,
                                   MAX_ADDR_TO_SEND);

    if (num > 0) {
        struct byte_stream addr_msg;
        stream_init(&addr_msg, num * 30 + 8);
        if (!stream_write_compact_size(&addr_msg, (uint64_t)num)) {
            /* allocation failed (addr_msg.data NULL); skip sending the
             * addr message rather than emit a malformed one. */
            stream_free(&addr_msg);
            return true;
        }
        for (size_t i = 0; i < num; i++) {
            if (!net_address_serialize(&addrs[i], &addr_msg, true)) {
                stream_free(&addr_msg);
                return true;
            }
        }

        p2p_node_begin_message(node, "addr", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, addr_msg.data, addr_msg.size);
        p2p_node_end_message(node);
        stream_free(&addr_msg);
    }
    return true;
}

bool mp_handle_getaddr(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s)
{
    (void)s;
    return process_getaddr(mp, node);
}
