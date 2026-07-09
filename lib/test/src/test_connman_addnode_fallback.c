/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include <unistd.h>

static void test_set_ipv4(struct net_address *addr,
                          uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                          uint16_t port)
{
    net_address_init(addr);
    addr->svc.addr.ip[10] = 0xff;
    addr->svc.addr.ip[11] = 0xff;
    addr->svc.addr.ip[12] = a;
    addr->svc.addr.ip[13] = b;
    addr->svc.addr.ip[14] = c;
    addr->svc.addr.ip[15] = d;
    addr->svc.port = port;
}

static struct p2p_node *add_test_peer(struct connman *cm,
                                      uint8_t a, uint8_t b,
                                      uint8_t c, uint8_t d,
                                      enum peer_state state,
                                      bool inbound,
                                      bool disconnect)
{
    if (!cm)
        return NULL;
    if (!cm->manager.nodes) {
        cm->manager.nodes = zcl_calloc(8, sizeof(*cm->manager.nodes),
                                       "connman_test_nodes");
        cm->manager.nodes_cap = 8;
    }
    struct net_address addr;
    test_set_ipv4(&addr, a, b, c, d, 8033);
    struct p2p_node *node = p2p_node_create(
        &cm->manager, ZCL_INVALID_SOCKET, &addr, "connman-test", inbound);
    if (!node)
        return NULL;
    node->state = state;
    node->disconnect = disconnect;
    node->starting_height = 3117074;
    node->services = NODE_NETWORK;
    cm->manager.nodes[cm->manager.num_nodes++] = node;
    return node;
}

int test_connman_addnode_fallback(void)
{
    int failures = 0;

    printf("connman_addnode_fallback: addnodes drain before addrman... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);

        for (int i = 0; ok && i < 10; i++) {
            struct net_address addr;
            test_set_ipv4(&addr, 203, 0, 113, (uint8_t)(10 + i), 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
        }

        for (int i = 0; ok && i < 10; i++) {
            struct net_address want = cm.addnodes[i];
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            size_t addnode_index = SIZE_MAX;
            memset(&pick, 0, sizeof(pick));
            ok = connman_pick_next_outbound_target(&cm,
                                                   &cm.next_addnode_cursor,
                                                   &pick,
                                                   &source,
                                                   &addnode_index);
            ok = ok && source == CONNMAN_TARGET_ADDNODE;
            ok = ok && addnode_index == (size_t)i;
            ok = ok && net_addr_eq(&pick.addr.svc.addr, &want.svc.addr);
            ok = ok && pick.addr.svc.port == want.svc.port;

            /* Simulate a failed dial so the next pick advances instead of
             * returning the same addnode again within the cooldown window. */
            connman_record_addnode_attempt(&cm, addnode_index, false);
        }

        if (ok) {
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            struct connman_outbound_health health;
            memset(&pick, 0, sizeof(pick));
            ok = !connman_pick_next_outbound_target(&cm,
                                                    &cm.next_addnode_cursor,
                                                    &pick,
                                                    &source,
                                                    NULL);
            memset(&health, 0, sizeof(health));
            connman_get_outbound_health(&cm, &health);
            ok = ok && health.addnode_count == 10;
            ok = ok && health.addnode_backoff_active == 10;
            ok = ok && health.addnode_tcp_failures == 10;
            ok = ok && health.addnode_protocol_failures == 0;
            connman_record_addnode_failure(&cm, 0,
                                           CONNMAN_ADDNODE_FAILURE_PROTOCOL);
            memset(&health, 0, sizeof(health));
            connman_get_outbound_health(&cm, &health);
            ok = ok && health.addnode_tcp_failures == 10;
            ok = ok && health.addnode_protocol_failures == 1;
        }

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: addnode remove compacts state... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        struct net_address a;
        struct net_address b;
        struct net_address c;
        struct net_address missing;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);

        test_set_ipv4(&a, 203, 0, 113, 10, 8033);
        test_set_ipv4(&b, 203, 0, 113, 11, 20022);
        test_set_ipv4(&c, 203, 0, 113, 12, 8033);
        test_set_ipv4(&missing, 203, 0, 113, 99, 8033);

        cm.addnodes[cm.num_addnodes++] = a;
        cm.addnodes[cm.num_addnodes++] = b;
        cm.addnodes[cm.num_addnodes++] = c;
        cm.addnode_last_attempt[0] = 10;
        cm.addnode_last_attempt[1] = 20;
        cm.addnode_last_attempt[2] = 30;
        cm.addnode_backoff_sec[0] = 40;
        cm.addnode_backoff_sec[1] = 50;
        cm.addnode_backoff_sec[2] = 60;
        cm.addnode_tcp_failures[0] = 1;
        cm.addnode_tcp_failures[1] = 2;
        cm.addnode_tcp_failures[2] = 3;
        cm.addnode_protocol_failures[0] = 4;
        cm.addnode_protocol_failures[1] = 5;
        cm.addnode_protocol_failures[2] = 6;
        cm.next_addnode_cursor = 2;

        ok = ok && connman_remove_addnode(&cm, &b);
        ok = ok && cm.num_addnodes == 2;
        ok = ok && net_addr_eq(&cm.addnodes[0].svc.addr, &a.svc.addr);
        ok = ok && cm.addnodes[0].svc.port == a.svc.port;
        ok = ok && net_addr_eq(&cm.addnodes[1].svc.addr, &c.svc.addr);
        ok = ok && cm.addnodes[1].svc.port == c.svc.port;
        ok = ok && cm.addnode_last_attempt[1] == 30;
        ok = ok && cm.addnode_backoff_sec[1] == 60;
        ok = ok && cm.addnode_tcp_failures[1] == 3;
        ok = ok && cm.addnode_protocol_failures[1] == 6;
        ok = ok && cm.next_addnode_cursor == 1;
        ok = ok && !connman_remove_addnode(&cm, &missing);

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: outbound health tracks diversity... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        struct connman_outbound_health health;
        memset(&sigs, 0, sizeof(sigs));
        memset(&health, 0, sizeof(health));
        bool ok = connman_init(&cm, params, &sigs);

        ok = ok && add_test_peer(&cm, 10, 1, 0, 1,
                                 PEER_HANDSHAKE_COMPLETE,
                                 false, false) != NULL;
        ok = ok && add_test_peer(&cm, 10, 1, 0, 2,
                                 PEER_ACTIVE,
                                 false, false) != NULL;
        ok = ok && add_test_peer(&cm, 172, 16, 0, 1,
                                 PEER_SYNCING_BLOCKS,
                                 false, false) != NULL;
        ok = ok && add_test_peer(&cm, 198, 51, 100, 5,
                                 PEER_CONNECTING,
                                 false, false) != NULL;
        ok = ok && add_test_peer(&cm, 203, 0, 113, 1,
                                 PEER_ACTIVE,
                                 true, false) != NULL;
        ok = ok && add_test_peer(&cm, 203, 0, 113, 2,
                                 PEER_ACTIVE,
                                 false, true) != NULL;

        connman_get_outbound_health(&cm, &health);
        ok = ok && health.outbound_total == 4;
        ok = ok && health.inbound_total == 1;
        ok = ok && health.healthy == 3;
        ok = ok && health.inbound_healthy == 1;
        ok = ok && health.connecting == 1;
        ok = ok && health.handshake_incomplete == 1;
        ok = ok && health.inbound_handshake_incomplete == 0;
        ok = ok && health.ipv4_group_count == 3;
        ok = ok && health.ipv4_max_group_size == 2;
        ok = ok && connman_outbound_healthy_count(&cm) == 3;

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: non-network peers do not satisfy "
           "outbound floor... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        struct connman_outbound_health health;
        struct p2p_node *no_network = NULL;
        memset(&sigs, 0, sizeof(sigs));
        memset(&health, 0, sizeof(health));
        bool ok = connman_init(&cm, params, &sigs);

        ok = ok && add_test_peer(&cm, 10, 9, 0, 1,
                                 PEER_HANDSHAKE_COMPLETE,
                                 false, false) != NULL;
        ok = ok && add_test_peer(&cm, 172, 20, 0, 1,
                                 PEER_ACTIVE,
                                 false, false) != NULL;
        ok = ok && (no_network = add_test_peer(&cm, 198, 51, 100, 9,
                                               PEER_ACTIVE,
                                               false, false)) != NULL;
        if (ok)
            no_network->services = 0;

        connman_get_outbound_health(&cm, &health);
        ok = ok && health.outbound_total == 3;
        ok = ok && health.healthy == 2;
        ok = ok && connman_outbound_healthy_count(&cm) == 2;

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: max peer height ignores unusable slots... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);

        struct p2p_node *usable = NULL;
        struct p2p_node *connecting = NULL;
        struct p2p_node *disconnecting = NULL;
        struct p2p_node *no_network = NULL;
        ok = ok && (usable = add_test_peer(&cm, 10, 2, 0, 1,
                                           PEER_HANDSHAKE_COMPLETE,
                                           false, false)) != NULL;
        ok = ok && (connecting = add_test_peer(&cm, 10, 2, 0, 2,
                                               PEER_CONNECTING,
                                               false, false)) != NULL;
        ok = ok && (disconnecting = add_test_peer(&cm, 10, 2, 0, 3,
                                                  PEER_ACTIVE,
                                                  false, true)) != NULL;
        ok = ok && (no_network = add_test_peer(&cm, 10, 2, 0, 4,
                                               PEER_ACTIVE,
                                               false, false)) != NULL;
        if (ok) {
            usable->starting_height = 120;
            connecting->starting_height = 900;
            disconnecting->starting_height = 800;
            no_network->starting_height = 700;
            no_network->services = 0;
            ok = connman_max_peer_height(&cm) == 120;
        }

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: peer-floor addnodes prefer diverse "
           "subnets... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);

        ok = ok && add_test_peer(&cm, 10, 1, 0, 1,
                                 PEER_HANDSHAKE_COMPLETE,
                                 false, false) != NULL;
        ok = ok && add_test_peer(&cm, 10, 1, 0, 2,
                                 PEER_HANDSHAKE_COMPLETE,
                                 false, false) != NULL;

        if (ok) {
            struct net_address addr;
            test_set_ipv4(&addr, 10, 1, 0, 1, 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
            test_set_ipv4(&addr, 10, 1, 0, 2, 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
            test_set_ipv4(&addr, 10, 1, 0, 3, 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
            test_set_ipv4(&addr, 172, 16, 0, 3, 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
        }

        if (ok) {
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            size_t addnode_index = SIZE_MAX;
            memset(&pick, 0, sizeof(pick));
            ok = connman_pick_next_outbound_target(&cm,
                                                   &cm.next_addnode_cursor,
                                                   &pick,
                                                   &source,
                                                   &addnode_index);
            ok = ok && source == CONNMAN_TARGET_ADDNODE;
            ok = ok && addnode_index == 3;
            ok = ok && net_addr_is_ipv4(&pick.addr.svc.addr);
            ok = ok && pick.addr.svc.addr.ip[12] == 172;
            ok = ok && pick.addr.svc.addr.ip[13] == 16;
        }

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: addrman skips saturated subnets... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        struct net_addr src;
        memset(&sigs, 0, sizeof(sigs));
        net_addr_init(&src);
        bool ok = connman_init(&cm, params, &sigs);

        ok = ok && add_test_peer(&cm, 51, 178, 0, 1,
                                 PEER_HANDSHAKE_COMPLETE,
                                 false, false) != NULL;
        ok = ok && add_test_peer(&cm, 51, 178, 0, 2,
                                 PEER_HANDSHAKE_COMPLETE,
                                 false, false) != NULL;

        if (ok) {
            struct net_address addr;
            test_set_ipv4(&addr, 51, 178, 0, 3, 8033);
            ok = ok && addrman_add(&cm.manager.addrman, &addr, &src, 0);
            test_set_ipv4(&addr, 51, 178, 0, 4, 8033);
            ok = ok && addrman_add(&cm.manager.addrman, &addr, &src, 0);
            test_set_ipv4(&addr, 47, 88, 87, 154, 8033);
            ok = ok && addrman_add(&cm.manager.addrman, &addr, &src, 0);
        }

        if (ok) {
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            memset(&pick, 0, sizeof(pick));
            ok = connman_pick_next_outbound_target(&cm,
                                                   &cm.next_addnode_cursor,
                                                   &pick,
                                                   &source,
                                                   NULL);
            ok = ok && source == CONNMAN_TARGET_ADDRMAN;
            ok = ok && net_addr_is_ipv4(&pick.addr.svc.addr);
            ok = ok && pick.addr.svc.addr.ip[12] == 47;
            ok = ok && pick.addr.svc.addr.ip[13] == 88;

            bool attempt_recorded = false;
            for (int i = 0; i < cm.manager.addrman.id_count; i++) {
                struct addr_info *info = &cm.manager.addrman.entries[i];
                if (!info->used)
                    continue;
                if (info->addr.svc.addr.ip[12] == 47 &&
                    info->addr.svc.addr.ip[13] == 88) {
                    attempt_recorded =
                        info->attempts == 1 && info->last_try > 0;
                    break;
                }
            }
            ok = ok && attempt_recorded;

            memset(&pick, 0, sizeof(pick));
            source = CONNMAN_TARGET_NONE;
            ok = ok && !connman_pick_next_outbound_target(
                &cm, &cm.next_addnode_cursor, &pick, &source, NULL);
        }

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: inbound ephemeral does not block "
           "advertised addrman dial... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        struct net_addr src;
        memset(&sigs, 0, sizeof(sigs));
        net_addr_init(&src);
        bool ok = connman_init(&cm, params, &sigs);

        struct p2p_node *inbound = NULL;
        ok = ok && (inbound = add_test_peer(&cm, 66, 70, 182, 44,
                                            PEER_ACTIVE, true, false)) != NULL;
        if (ok)
            inbound->addr.svc.port = 44554;

        if (ok) {
            struct net_address addr;
            test_set_ipv4(&addr, 66, 70, 182, 44, 8033);
            ok = ok && addrman_add(&cm.manager.addrman, &addr, &src, 0);
        }

        if (ok) {
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            memset(&pick, 0, sizeof(pick));
            ok = connman_pick_next_outbound_target(&cm,
                                                   &cm.next_addnode_cursor,
                                                   &pick,
                                                   &source,
                                                   NULL);
            ok = ok && source == CONNMAN_TARGET_ADDRMAN;
            ok = ok && net_addr_is_ipv4(&pick.addr.svc.addr);
            ok = ok && pick.addr.svc.addr.ip[12] == 66;
            ok = ok && pick.addr.svc.addr.ip[13] == 70;
            ok = ok && pick.addr.svc.port == 8033;
        }

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: pre-handshake addnode disconnect "
           "backs off as protocol failure... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);

        if (ok) {
            struct net_address addr;
            test_set_ipv4(&addr, 51, 178, 179, 75, 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
            struct p2p_node *node = add_test_peer(
                &cm, 51, 178, 179, 75, PEER_VERSION_SENT, false, false);
            ok = ok && node != NULL;
            if (node)
                connman_note_addnode_prehandshake_disconnect(
                    &cm, node, "unit-test");
        }

        ok = ok && cm.addnode_protocol_failures[0] == 1;
        ok = ok && cm.addnode_tcp_failures[0] == 0;
        /* First PROTOCOL failure now backs off via the gentle ramp
         * (step 1 = 60s), NOT an instant 900s lockout — a transient drop
         * is re-dialed in time to fill the outbound floor. */
        ok = ok && cm.addnode_backoff_sec[0] == 60;
        ok = ok && cm.addnode_last_attempt[0] > 0;

        /* A second consecutive prehandshake (PROTOCOL) failure ramps but is
         * still well below the 1800s ceiling — proves it is no longer an
         * instant lockout. */
        struct p2p_node *node2 = add_test_peer(
            &cm, 51, 178, 179, 75, PEER_VERSION_SENT, false, false);
        if (node2)
            connman_note_addnode_prehandshake_disconnect(
                &cm, node2, "unit-test-2");
        ok = ok && cm.addnode_protocol_failures[0] == 2;
        ok = ok && cm.addnode_backoff_sec[0] == 120;   /* step 2 */
        ok = ok && cm.addnode_backoff_sec[0] < 1800;

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: protocol failures back off longer "
           "than TCP failures... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, params, &sigs);

        if (ok) {
            struct net_address addr;
            test_set_ipv4(&addr, 203, 0, 113, 21, 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
            test_set_ipv4(&addr, 203, 0, 113, 22, 8033);
            cm.addnodes[cm.num_addnodes++] = addr;
        }

        connman_record_addnode_failure(&cm, 0,
                                       CONNMAN_ADDNODE_FAILURE_TCP);
        connman_record_addnode_failure(&cm, 1,
                                       CONNMAN_ADDNODE_FAILURE_PROTOCOL);
        /* After ONE failure each: TCP=20 (ramp step 0), PROTOCOL=60 (ramp
         * step 1, one ahead). PROTOCOL still backs off longer than TCP —
         * the invariant this test guards — but neither is an instant
         * 900s/120s lockout. */
        ok = ok && cm.addnode_backoff_sec[0] == 20;
        ok = ok && cm.addnode_backoff_sec[1] == 60;
        ok = ok && cm.addnode_backoff_sec[1] > cm.addnode_backoff_sec[0];

        /* A genuinely dead host still reaches the 1800s ceiling: ramp a TCP
         * addnode to 7 consecutive failures (step 6 = the last ramp entry). */
        for (int f = 0; f < 6; f++)
            connman_record_addnode_failure(&cm, 0,
                                           CONNMAN_ADDNODE_FAILURE_TCP);
        ok = ok && cm.addnode_tcp_failures[0] == 7;
        ok = ok && cm.addnode_backoff_sec[0] == 1800;

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("connman_addnode_fallback: addrman repeated failures cool down... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *params = chain_params_get();
        struct connman cm;
        struct node_signals sigs;
        struct net_addr src;
        memset(&sigs, 0, sizeof(sigs));
        net_addr_init(&src);
        bool ok = connman_init(&cm, params, &sigs);

        if (ok) {
            struct net_address addr;
            test_set_ipv4(&addr, 66, 70, 182, 44, 8033);
            ok = ok && addrman_add(&cm.manager.addrman, &addr, &src, 0);
            test_set_ipv4(&addr, 81, 214, 132, 20, 8033);
            ok = ok && addrman_add(&cm.manager.addrman, &addr, &src, 0);
        }

        if (ok) {
            int64_t now = (int64_t)platform_time_wall_time_t();
            for (int i = 0; i < cm.manager.addrman.id_count; i++) {
                struct addr_info *info = &cm.manager.addrman.entries[i];
                if (!info->used)
                    continue;
                if (info->addr.svc.addr.ip[12] == 66 &&
                    info->addr.svc.addr.ip[13] == 70) {
                    info->attempts = 3;
                    info->last_try = now - 120;
                }
            }
        }

        if (ok) {
            struct addr_info pick;
            enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
            memset(&pick, 0, sizeof(pick));
            ok = connman_pick_next_outbound_target(&cm,
                                                   &cm.next_addnode_cursor,
                                                   &pick,
                                                   &source,
                                                   NULL);
            ok = ok && source == CONNMAN_TARGET_ADDRMAN;
            ok = ok && net_addr_is_ipv4(&pick.addr.svc.addr);
            ok = ok && pick.addr.svc.addr.ip[12] == 81;
            ok = ok && pick.addr.svc.addr.ip[13] == 214;
        }

        connman_free(&cm);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* S6 (docs/work/sovereign-service-roadmap.md #S6): bootstrap
     * fallback-of-last-resort. With DNS seeds AND the operator addrman
     * file BOTH unavailable, the hardcoded chainparams onion-seed tier
     * (connman.c:281-283 run_onion_seed_pass, addrman.c addrman_add) is
     * the only remaining peer source and must still be reachable/bounded.
     *
     * The embedded Tor stub in this test binary never reports
     * tor_integration_is_ready()==true, so the live onion-directory HTTP
     * fetch (try_onion_seed_fetch -> tor_integration_fetch_onion_blocking)
     * cannot be driven end-to-end without a real bootstrapped Tor circuit
     * (see test_onion_bootstrap.c). This test instead deterministically
     * proves the two halves that make the tier a genuine last resort:
     * (1) with a chainparams copy whose DNS (nSeeds) and hardcoded IP
     *     fixed-seed (nFixedSeeds) tiers are both zeroed, and no
     *     peers.dat on disk (operator addrman file "unavailable"),
     *     connman_kick_seed_discovery + connman_load_addrman leave the
     *     node with zero outbound candidates — proving those tiers are
     *     truly exhausted, not silently still supplying peers;
     * (2) the surviving onionSeeds[] tier is non-empty, and once it
     *     contributes addrman entries (the exact effect
     *     try_onion_seed_fetch has on a successful /directory.json
     *     fetch), a single bounded connman_pick_next_outbound_target
     *     call yields a first peer from CONNMAN_TARGET_ADDRMAN — i.e.
     *     the fallback-of-last-resort tier alone is sufficient. */
    printf("connman_addnode_fallback: onion-seed tier is fallback of last "
           "resort when DNS + addrman file are both unavailable... ");
    {
        chain_params_select(CHAIN_MAIN);
        struct chain_params no_dns_no_fixed = *chain_params_get();
        no_dns_no_fixed.nSeeds = 0;
        no_dns_no_fixed.nFixedSeeds = 0;
        bool ok = no_dns_no_fixed.nOnionSeeds > 0;

        char tmpdir[] = "/tmp/zcl_connman_lastresort_XXXXXX";
        ok = ok && mkdtemp(tmpdir) != NULL;

        struct connman cm;
        struct node_signals sigs;
        memset(&sigs, 0, sizeof(sigs));
        ok = ok && connman_init(&cm, &no_dns_no_fixed, &sigs);
        if (ok)
            cm.datadir = tmpdir;

        /* DNS (0 seeds) + hardcoded fixed seeds (0) contribute nothing;
         * the operator addrman file does not exist in tmpdir either. */
        if (ok)
            connman_kick_seed_discovery(&cm);
        if (ok)
            connman_load_addrman(&cm);

        struct addr_info pick;
        enum connman_outbound_target_source source = CONNMAN_TARGET_NONE;
        memset(&pick, 0, sizeof(pick));
        bool exhausted_before_onion_tier =
            ok && !connman_pick_next_outbound_target(&cm,
                                                     &cm.next_addnode_cursor,
                                                     &pick, &source, NULL);
        ok = ok && exhausted_before_onion_tier;

        /* Simulate the onion-seed tier's real effect: a successful
         * /directory.json fetch from a hardcoded .onion seed adds the
         * discovered clearnet peer via addrman_add (connman.c:232),
         * bounded by nOnionSeeds attempts (the same bound
         * run_onion_seed_pass iterates under, connman.c:282-283). */
        if (ok) {
            struct net_addr src;
            net_addr_init(&src);
            for (size_t i = 0; i < no_dns_no_fixed.nOnionSeeds; i++) {
                struct net_address addr;
                test_set_ipv4(&addr, 45, 33, (uint8_t)(i + 1), 1, 8033);
                ok = ok && addrman_add(&cm.manager.addrman, &addr, &src, 0);
            }
        }

        if (ok) {
            memset(&pick, 0, sizeof(pick));
            source = CONNMAN_TARGET_NONE;
            ok = connman_pick_next_outbound_target(&cm,
                                                    &cm.next_addnode_cursor,
                                                    &pick, &source, NULL);
            ok = ok && source == CONNMAN_TARGET_ADDRMAN;
        }

        connman_free(&cm);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
        (void)system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
