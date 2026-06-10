/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

/* Regression test for the connect_node TOCTOU use-after-free fix
 * (find_node_by_service_locked): the find-and-ref of an existing peer now
 * happens atomically under cs_nodes, and disconnect-flagged nodes are skipped
 * so connect_node never re-refs a peer the socket sweep is about to free. */

#include "test/test_helpers.h"

#include "net/net.h"
#include "net/netaddr.h"
#include "net/netbase.h"

#include <stdio.h>
#include <string.h>

/* Manually splice a pre-built node into nm->nodes[] the way nm_add_node does
 * internally (nm_add_node is static). net_manager_free() will free both the
 * array and every node still parked in it, so the caller must NOT free the
 * node separately. */
static void manager_insert_node(struct net_manager *nm, struct p2p_node *node)
{
    struct p2p_node **grown =
        zcl_realloc(nm->nodes, (nm->num_nodes + 1) * sizeof(*grown),
                    "test_node_list");
    nm->nodes = grown;
    nm->nodes_cap = nm->num_nodes + 1;
    nm->nodes[nm->num_nodes++] = node;
}

int test_connect_node_locked(void)
{
    printf("\n=== connect_node find_node_by_service_locked tests ===\n");
    int failures = 0;

    TEST_CASE("connect_node dedupes (ref+1) and skips disconnect-flagged nodes")
    {
        /* Case 1: an existing live node for service S is deduped — connect_node
         * returns the SAME pointer with exactly one extra ref and appends no
         * new node (the find-and-ref happens atomically under cs_nodes). */
        struct net_manager nm;
        net_manager_init(&nm);
        memcpy(nm.message_start, "\xfa\x1a\xf9\xbf", 4);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8233;

        struct p2p_node *node =
            p2p_node_create(&nm, ZCL_INVALID_SOCKET, &addr, "dedupe-peer",
                            false);
        ASSERT(node != NULL);
        manager_insert_node(&nm, node);

        int ref_before = p2p_node_get_ref(node);
        size_t nodes_before = nm.num_nodes;

        /* dest=NULL: dedupe must short-circuit before any socket connect. */
        struct p2p_node *got = connect_node(&nm, &addr, NULL);
        ASSERT(got == node);
        ASSERT(p2p_node_get_ref(node) == ref_before + 1);
        ASSERT(nm.num_nodes == nodes_before);

        net_manager_free(&nm);

        /* Case 2: the node for service S is flagged disconnect before the call.
         * The lookup must skip it, so connect_node never returns it and never
         * re-refs it — closing the use-after-free window. Loopback port 1 makes
         * the fall-through socket connect refuse immediately, so connect_node
         * returns NULL fast and deterministically without touching node. */
        struct net_manager nm2;
        net_manager_init(&nm2);
        memcpy(nm2.message_start, "\xfa\x1a\xf9\xbf", 4);

        struct net_address addr2;
        net_address_init(&addr2);
        unsigned char lo4[4] = {127, 0, 0, 1};
        net_addr_set_ipv4(&addr2.svc.addr, lo4);
        addr2.svc.port = 1;

        struct p2p_node *dnode =
            p2p_node_create(&nm2, ZCL_INVALID_SOCKET, &addr2, "disc-peer",
                            false);
        ASSERT(dnode != NULL);
        dnode->disconnect = true;
        manager_insert_node(&nm2, dnode);

        int dref_before = p2p_node_get_ref(dnode);

        struct p2p_node *dgot = connect_node(&nm2, &addr2, NULL);
        ASSERT(dgot != dnode);
        ASSERT(p2p_node_get_ref(dnode) == dref_before);

        net_manager_free(&nm2);
    }
    TEST_END

    return failures;
}
