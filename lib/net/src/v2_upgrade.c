/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * v2_upgrade.c — one-shot legacy-to-Noise reconnect policy. */

#include "net/connman.h"
#include "net/v2_transport.h"
#include "util/log_macros.h"

bool connman_request_v2_upgrade(struct connman *cm, struct p2p_node *node)
{
    if (!cm || !node || node->inbound || node->transport ||
        !cm->manager.v2_enabled ||
        (node->services & NODE_V2TRANSPORT) == 0)
        return false;

    /* node->addr is the dial-time capability snapshot.  If it already knew
     * v2, net.c would have armed Noise before sending any version bytes; do
     * not create a reconnect loop if a caller reaches this path anyway. */
    if ((node->addr.nServices & NODE_V2TRANSPORT) != 0)
        return false;

    node->addr.nServices |= NODE_V2TRANSPORT;

    /* Pinned addnodes bypass addrman selection, so update their durable
     * in-process dial entry as well. */
    for (int i = 0; i < cm->num_addnodes; i++) {
        if (net_addr_eq(&cm->addnodes[i].svc.addr, &node->addr.svc.addr) &&
            cm->addnodes[i].svc.port == node->addr.svc.port)
            cm->addnodes[i].nServices |= NODE_V2TRANSPORT;
    }

    /* addrman_add merges service bits for an existing entry even if it does
     * not add another bucket reference.  Loopback addnodes are covered above. */
    if (net_addr_is_routable(&node->addr.svc.addr)) {
        struct net_addr source = node->addr.svc.addr;
        (void)addrman_add(&cm->manager.addrman, &node->addr, &source, 0);
    }

    node->disconnect = true;
    LOG_INFO("net", "peer %s advertised v2 transport; capability persisted "
             "and controlled Noise reconnect requested", node->addr_name);
    return true;
}
