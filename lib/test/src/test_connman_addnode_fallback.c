/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "platform/time_compat.h"
#include "test/test_helpers.h"

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

    return failures;
}
