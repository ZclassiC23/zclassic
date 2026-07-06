/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "platform/time_compat.h"
#include "test/test_helpers.h"

#include "event/event.h"
#include "net/fast_sync.h"
#include "net/msg_internal.h"
#include "net/peer_lifecycle.h"
#include "net/port_policy.h"
#include "net/version.h"

#include <time.h>
#include <unistd.h>

static int g_lifecycle_disconnect_events;
static char g_lifecycle_disconnect_payload[EVENT_PAYLOAD_SIZE];

static void lifecycle_disconnect_observer(enum event_type type,
                                          uint32_t peer_id,
                                          const void *payload,
                                          uint32_t payload_len,
                                          void *ctx)
{
    (void)ctx;
    if (type != EV_TCP_DISCONNECTED || peer_id != 77)
        return;
    g_lifecycle_disconnect_events++;
    size_t n = payload_len < sizeof(g_lifecycle_disconnect_payload) - 1
             ? payload_len : sizeof(g_lifecycle_disconnect_payload) - 1;
    if (payload && n > 0)
        memcpy(g_lifecycle_disconnect_payload, payload, n);
    g_lifecycle_disconnect_payload[n] = '\0';
}

static void test_addr_ipv4(struct net_address *addr,
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

static const struct json_value *find_lifecycle_source(
    const struct json_value *arr, const char *source)
{
    if (!arr || arr->type != JSON_ARR || !source)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *entry = json_at(arr, i);
        const struct json_value *name = json_get(entry, "source");
        if (name && strcmp(json_get_str(name), source) == 0)
            return entry;
    }
    return NULL;
}

static const struct json_value *find_lifecycle_obj_str(
    const struct json_value *arr, const char *key, const char *value)
{
    if (!arr || arr->type != JSON_ARR || !key || !value)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *entry = json_at(arr, i);
        const struct json_value *field = json_get(entry, key);
        if (field && strcmp(json_get_str(field), value) == 0)
            return entry;
    }
    return NULL;
}

static int test_peer_lifecycle_user_agent(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: version user agent advertises ZClassic23")
    {
        const char *ua = msg_version_user_agent();
        ASSERT(strcmp(ua, "/ZClassic23:0.1.0/") == 0);
        ASSERT(strstr(ua, "MagicBean") == NULL);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_classify(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: subver/service classification")
    {
        bool mb = false, z23 = false;
        ASSERT(msg_version_classify_peer("/MagicBean:2.1.2/",
                                         NODE_NETWORK, &mb, &z23));
        ASSERT(mb);
        ASSERT(!z23);
        ASSERT(msg_version_classify_peer("/Satoshi:0.11.2/",
                                         NODE_NETWORK | NODE_ZCL23,
                                         &mb, &z23));
        ASSERT(!mb);
        ASSERT(z23);
        ASSERT(msg_version_classify_peer("/ZClassic23:0.1.0/",
                                         NODE_NETWORK, &mb, &z23));
        ASSERT(!mb);
        ASSERT(z23);
        ASSERT(msg_version_classify_peer("/ZClassic-C23:1.0.0/",
                                         NODE_NETWORK, &mb, &z23));
        ASSERT(!mb);
        ASSERT(z23);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_version_build(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: version construction includes NODE_ZCL23")
    {
        struct net_manager nm;
        struct msg_processor mp;
        struct p2p_node node;
        struct version_message ver;

        memset(&mp, 0, sizeof(mp));
        memset(&node, 0, sizeof(node));
        net_manager_init(&nm);
        nm.local_host_nonce = 12345;
        mp.net_mgr = &nm;
        test_addr_ipv4(&node.addr, 203, 0, 113, 44, 8033);

        msg_version_build(&ver, &mp, &node, 3117073);
        ASSERT(ver.protocol_version == PROTOCOL_VERSION);
        ASSERT((ver.services & NODE_NETWORK) != 0);
        ASSERT((ver.services & NODE_ZCL23) != 0);
        ASSERT(ver.start_height == 3117073);
        ASSERT(strcmp(ver.sub_version, msg_version_user_agent()) == 0);
        ASSERT(ver.nonce == 12345);
        net_manager_free(&nm);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_learns_inbound_advertised_addr(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: inbound version advertised address feeds addrman")
    {
        struct net_manager nm;
        struct p2p_node node;
        struct version_message ver;
        struct addr_info selected;

        net_manager_init(&nm);
        memset(&node, 0, sizeof(node));
        version_message_init(&ver);

        node.inbound = true;
        test_addr_ipv4(&node.addr, 81, 214, 132, 20, 52000);
        snprintf(node.addr_name, sizeof(node.addr_name), "81.214.132.20:52000");

        ver.services = NODE_NETWORK;
        test_addr_ipv4(&ver.addr_from, 66, 70, 182, 44, 8033);
        ver.addr_from.nServices = NODE_NETWORK;
        ver.addr_from.nTime = (uint32_t)platform_time_wall_time_t();

        ASSERT(msg_version_learn_advertised_addr(&nm, &node, &ver));
        ASSERT(addrman_size(&nm.addrman) == 1);
        ASSERT(addrman_select(&nm.addrman, true, &selected));
        ASSERT(net_service_eq(&selected.addr.svc, &ver.addr_from.svc));

        net_manager_free(&nm);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_skips_inbound_ephemeral_cache(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: inbound ephemeral source ports are not cached")
    {
        struct p2p_node inbound;
        struct p2p_node outbound;

        memset(&inbound, 0, sizeof(inbound));
        test_addr_ipv4(&inbound.addr, 40, 160, 53, 56, 53100);
        inbound.inbound = true;
        snprintf(inbound.addr_name, sizeof(inbound.addr_name),
                 "40.160.53.56:53100");

        memset(&outbound, 0, sizeof(outbound));
        test_addr_ipv4(&outbound.addr, 40, 160, 53, 56, 8033);
        outbound.inbound = false;
        snprintf(outbound.addr_name, sizeof(outbound.addr_name),
                 "40.160.53.56:8033");

        ASSERT(!msg_version_should_save_peer(&inbound));
        ASSERT(msg_version_should_save_peer(&outbound));
        ASSERT(!zcl_net_port_is_reachable_candidate(53100));
        ASSERT(zcl_net_port_is_reachable_candidate(8033));
        ASSERT(zcl_net_port_is_reachable_candidate(20022));

        inbound.addr.svc.port = 8033;
        ASSERT(msg_version_should_save_peer(&inbound));
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_counters(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: counters track handshake lifecycle")
    {
        struct net_address addr;
        struct p2p_node node;
        struct peer_lifecycle_summary s;
        struct json_value dump;

        peer_lifecycle_reset_for_test();
        memset(&node, 0, sizeof(node));
        test_addr_ipv4(&addr, 198, 51, 100, 7, 8033);
        node.addr = addr;
        node.id = 77;
        node.state = PEER_HANDSHAKE_COMPLETE;
        snprintf(node.addr_name, sizeof(node.addr_name), "198.51.100.7:8033");
        snprintf(node.sub_ver, sizeof(node.sub_ver), "%s",
                 msg_version_user_agent());
        node.services = NODE_NETWORK | NODE_ZCL23;

        peer_lifecycle_note_attempt(&addr, PEER_LIFECYCLE_SOURCE_ADDNODE);
        peer_lifecycle_note_connected(&node, PEER_LIFECYCLE_SOURCE_ADDNODE);
        peer_lifecycle_note_version_sent(&node, node.services, 10,
                                         msg_version_user_agent());
        peer_lifecycle_note_version_received(&node, node.services, 12,
                                             msg_version_user_agent());
        peer_lifecycle_note_verack_received(&node);
        peer_lifecycle_note_handshake_complete(&node);
        peer_lifecycle_note_active(&node);
        peer_lifecycle_note_cache_skipped(&node, "save_advisory");
        event_log_init();
        g_lifecycle_disconnect_events = 0;
        g_lifecycle_disconnect_payload[0] = '\0';
        ASSERT(event_observe(EV_TCP_DISCONNECTED,
                             lifecycle_disconnect_observer, NULL));
        peer_lifecycle_note_disconnected(&node, "cleanup");
        peer_lifecycle_get_summary(&s);

        ASSERT(s.attempted == 1);
        ASSERT(s.connected == 1);
        ASSERT(s.version_sent == 1);
        ASSERT(s.version_received == 1);
        ASSERT(s.verack_received == 1);
        ASSERT(s.handshake_complete == 1);
        ASSERT(s.active == 1);
        ASSERT(s.disconnected == 1);
        ASSERT(s.cache_skipped == 1);
        ASSERT(s.magicbean_handshakes == 0);
        ASSERT(s.zcl23_handshakes == 1);
        ASSERT(g_lifecycle_disconnect_events == 1);
        ASSERT(strstr(g_lifecycle_disconnect_payload,
                      "disconnect addr=198.51.100.7:8033") != NULL);
        ASSERT(strstr(g_lifecycle_disconnect_payload,
                      "reason=cleanup") != NULL);
        event_clear_observers(EV_TCP_DISCONNECTED);

        json_init(&dump);
        json_set_object(&dump);
        ASSERT(peer_lifecycle_dump_state_json(&dump, NULL));
        const struct json_value *sources = json_get(&dump, "sources");
        const struct json_value *addnode =
            find_lifecycle_source(sources, "addnode");
        ASSERT(sources && sources->type == JSON_ARR);
        ASSERT(addnode && addnode->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(addnode, "attempted")) == 1);
        ASSERT(json_get_int(json_get(addnode, "connected")) == 1);
        ASSERT(json_get_int(json_get(addnode, "handshake_complete")) == 1);
        ASSERT(json_get_int(json_get(addnode, "active")) == 1);
        ASSERT(json_get_int(json_get(addnode, "disconnected")) == 1);
        ASSERT(json_get_int(json_get(addnode, "cache_skipped")) == 1);
        ASSERT(json_get_int(json_get(addnode, "magicbean_handshakes")) == 0);
        ASSERT(json_get_int(json_get(addnode,
                                      "legacy_compatible_handshakes")) == 0);
        ASSERT(json_get_int(json_get(addnode,
                                      "legacy_magicbean_handshakes")) == 0);
        ASSERT(json_get_int(json_get(addnode, "zclassic23_handshakes")) == 1);
        ASSERT(json_get_int(json_get(addnode, "zclassic_c23_handshakes")) == 1);
        json_free(&dump);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_inbound_source_bucket(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: inbound source bucket tracks public reachability")
    {
        struct net_address addr;
        struct p2p_node node;
        struct json_value summary;

        peer_lifecycle_reset_for_test();
        memset(&node, 0, sizeof(node));
        test_addr_ipv4(&addr, 203, 0, 113, 9, 8033);
        node.addr = addr;
        node.id = 88;
        node.inbound = true;
        node.state = PEER_HANDSHAKE_COMPLETE;
        snprintf(node.addr_name, sizeof(node.addr_name), "203.0.113.9:8033");
        snprintf(node.sub_ver, sizeof(node.sub_ver), "%s",
                 msg_version_user_agent());
        node.services = NODE_NETWORK | NODE_ZCL23;

        peer_lifecycle_note_connected(&node, PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_sent(&node, node.services, 20,
                                         msg_version_user_agent());
        peer_lifecycle_note_version_received(&node, node.services, 21,
                                             msg_version_user_agent());
        peer_lifecycle_note_handshake_complete(&node);

        json_init(&summary);
        ASSERT(peer_lifecycle_summary_json(&summary));
        const struct json_value *sources = json_get(&summary, "sources");
        const struct json_value *inbound =
            find_lifecycle_source(sources, "inbound");
        ASSERT(sources && sources->type == JSON_ARR);
        ASSERT(inbound && inbound->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(inbound, "attempted")) == 0);
        ASSERT(json_get_int(json_get(inbound, "connected")) == 1);
        ASSERT(json_get_int(json_get(inbound, "version_sent")) == 1);
        ASSERT(json_get_int(json_get(inbound, "version_received")) == 1);
        ASSERT(json_get_int(json_get(inbound, "handshake_complete")) == 1);
        ASSERT(json_get_int(json_get(inbound, "magicbean_handshakes")) == 0);
        ASSERT(json_get_int(json_get(inbound,
                                      "legacy_compatible_handshakes")) == 0);
        ASSERT(json_get_int(json_get(inbound,
                                      "legacy_magicbean_handshakes")) == 0);
        ASSERT(json_get_int(json_get(inbound, "zclassic_c23_handshakes")) == 1);
        json_free(&summary);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_duplicate_connect_duration(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: duplicate connect does not make duration negative")
    {
        struct net_address addr;
        struct p2p_node node;
        struct json_value peer;

        peer_lifecycle_reset_for_test();
        memset(&node, 0, sizeof(node));
        test_addr_ipv4(&addr, 198, 51, 100, 8, 8033);
        node.addr = addr;
        node.id = 99;
        node.state = PEER_HANDSHAKE_COMPLETE;
        snprintf(node.addr_name, sizeof(node.addr_name), "198.51.100.8:8033");
        snprintf(node.sub_ver, sizeof(node.sub_ver), "%s",
                 msg_version_user_agent());
        node.services = NODE_NETWORK | NODE_ZCL23;

        peer_lifecycle_note_connected(&node, PEER_LIFECYCLE_SOURCE_MANUAL);
        peer_lifecycle_note_version_received(&node, node.services, 12,
                                             msg_version_user_agent());
        peer_lifecycle_note_handshake_complete(&node);
        sleep(1);
        peer_lifecycle_note_connected(&node, PEER_LIFECYCLE_SOURCE_MANUAL);

        json_init(&peer);
        json_set_object(&peer);
        ASSERT(peer_lifecycle_peer_json(&node, &peer));
        ASSERT(json_get_int(json_get(&peer, "handshake_duration_secs")) >= 0);
        ASSERT(!json_get_bool(json_get(&peer, "legacy_compatible")));
        ASSERT(json_get_int(json_get(&peer, "handshake_complete_at")) >=
               json_get_int(json_get(&peer, "connected_at")));
        json_free(&peer);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_inbound_classifies_remote_version(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: inbound C23 count uses remote version")
    {
        struct net_address addr;
        struct p2p_node node;
        struct peer_lifecycle_summary s;

        peer_lifecycle_reset_for_test();
        memset(&node, 0, sizeof(node));
        test_addr_ipv4(&addr, 203, 0, 113, 10, 8033);
        node.addr = addr;
        node.id = 100;
        node.inbound = true;
        node.state = PEER_HANDSHAKE_COMPLETE;
        snprintf(node.addr_name, sizeof(node.addr_name), "203.0.113.10:8033");
        snprintf(node.sub_ver, sizeof(node.sub_ver),
                 "%s", "/MagicBean:2.1.2-beta6/");
        node.services = NODE_NETWORK;

        peer_lifecycle_note_connected(&node, PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_received(&node, node.services, 21,
                                             node.sub_ver);
        peer_lifecycle_note_version_sent(&node, NODE_NETWORK | NODE_ZCL23, 20,
                                         msg_version_user_agent());
        peer_lifecycle_note_handshake_complete(&node);
        peer_lifecycle_get_summary(&s);

        ASSERT(s.handshake_complete == 1);
        ASSERT(s.magicbean_handshakes == 1);
        ASSERT(s.legacy_compatible_handshakes == 1);
        ASSERT(s.zcl23_handshakes == 0);
    } TEST_END
    return failures;
}

static int test_peer_lifecycle_incident_view(void)
{
    int failures = 0;
    TEST_CASE("peer_lifecycle: compact incident view groups reconnecting hosts")
    {
        struct p2p_node zigma_a;
        struct p2p_node zigma_b;
        struct p2p_node stable;
        struct json_value incidents;
        struct json_value keyed;

        peer_lifecycle_reset_for_test();
        memset(&zigma_a, 0, sizeof(zigma_a));
        test_addr_ipv4(&zigma_a.addr, 40, 160, 53, 56, 45474);
        zigma_a.id = 201;
        zigma_a.inbound = true;
        zigma_a.state = PEER_HANDSHAKE_COMPLETE;
        zigma_a.services = NODE_NETWORK;
        snprintf(zigma_a.addr_name, sizeof(zigma_a.addr_name),
                 "40.160.53.56:45474");
        snprintf(zigma_a.sub_ver, sizeof(zigma_a.sub_ver),
                 "%s", "/Zigma:0.1.0/");

        peer_lifecycle_note_connected(&zigma_a,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_received(&zigma_a, zigma_a.services,
                                             3170406, zigma_a.sub_ver);
        peer_lifecycle_note_handshake_complete(&zigma_a);
        peer_lifecycle_note_active(&zigma_a);
        peer_lifecycle_note_disconnected(&zigma_a, "cleanup");
        peer_lifecycle_note_connected(&zigma_a,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_version_received(&zigma_a, zigma_a.services,
                                             3170407, zigma_a.sub_ver);
        peer_lifecycle_note_handshake_complete(&zigma_a);
        peer_lifecycle_note_active(&zigma_a);

        memset(&zigma_b, 0, sizeof(zigma_b));
        test_addr_ipv4(&zigma_b.addr, 40, 160, 53, 56, 39030);
        zigma_b.id = 202;
        zigma_b.inbound = true;
        zigma_b.state = PEER_CONNECTING;
        zigma_b.services = NODE_NETWORK;
        snprintf(zigma_b.addr_name, sizeof(zigma_b.addr_name),
                 "40.160.53.56:39030");
        snprintf(zigma_b.sub_ver, sizeof(zigma_b.sub_ver),
                 "%s", "/Zigma:0.1.0/");
        peer_lifecycle_note_connected(&zigma_b,
                                      PEER_LIFECYCLE_SOURCE_INBOUND);
        peer_lifecycle_note_timeout(&zigma_b, "handshake_timeout");

        memset(&stable, 0, sizeof(stable));
        test_addr_ipv4(&stable.addr, 198, 51, 100, 10, 8033);
        stable.id = 203;
        stable.state = PEER_HANDSHAKE_COMPLETE;
        stable.services = NODE_NETWORK;
        snprintf(stable.addr_name, sizeof(stable.addr_name),
                 "198.51.100.10:8033");
        snprintf(stable.sub_ver, sizeof(stable.sub_ver),
                 "%s", "/MagicBean:2.1.2-beta6/");
        peer_lifecycle_note_connected(&stable,
                                      PEER_LIFECYCLE_SOURCE_ADDNODE);
        peer_lifecycle_note_version_received(&stable, stable.services,
                                             3171817, stable.sub_ver);
        peer_lifecycle_note_handshake_complete(&stable);
        peer_lifecycle_note_active(&stable);

        json_init(&incidents);
        ASSERT(peer_lifecycle_incidents_json(&incidents));
        ASSERT(strcmp(json_get_str(json_get(&incidents, "schema")),
                      "zcl.peer_incidents.v1") == 0);
        ASSERT(json_get_bool(json_get(&incidents, "bounded")));
        ASSERT(json_get_int(json_get(&incidents, "incident_count")) == 2);
        ASSERT(json_get_int(json_get(&incidents,
                                     "duplicate_host_group_count")) == 1);
        ASSERT(json_get_int(json_get(&incidents,
                                     "bootstrap_useful_count")) == 2);

        const struct json_value *groups =
            json_get(&incidents, "duplicate_host_groups");
        const struct json_value *group =
            find_lifecycle_obj_str(groups, "host", "40.160.53.56");
        ASSERT(group && group->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(group, "entries")) == 2);
        ASSERT(json_get_int(json_get(group, "inbound_entries")) == 2);
        ASSERT(json_get_int(json_get(group, "reconnects")) == 1);
        ASSERT(json_get_int(json_get(group, "timeout")) == 1);
        ASSERT(json_get_int(json_get(group,
                                     "pre_handshake_disconnects")) == 1);
        ASSERT(json_get_bool(json_get(group, "bootstrap_useful")));

        const struct json_value *top =
            json_get(&incidents, "top_incidents");
        const struct json_value *a =
            find_lifecycle_obj_str(top, "addr", "40.160.53.56:45474");
        const struct json_value *b =
            find_lifecycle_obj_str(top, "addr", "40.160.53.56:39030");
        ASSERT(a && a->type == JSON_OBJ);
        ASSERT(b && b->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(a, "duplicate_host_entries")) == 2);
        ASSERT(json_get_int(json_get(a, "reconnects")) == 1);
        ASSERT(strcmp(json_get_str(json_get(a, "direction")), "inbound") == 0);
        ASSERT(json_get_int(json_get(a, "advertised_height")) == 3170407);
        ASSERT(json_get_int(json_get(a, "handshake_age_secs")) >= 0);
        ASSERT(json_get_bool(json_get(a, "bootstrap_useful")));
        ASSERT(!json_get_bool(json_get(a, "fast_sync_useful")));
        ASSERT(json_get_int(json_get(b, "timeout")) == 1);
        ASSERT(json_get_int(json_get(b,
                                     "pre_handshake_disconnects")) == 1);
        ASSERT(!json_get_bool(json_get(b, "bootstrap_useful")));
        json_free(&incidents);

        json_init(&keyed);
        json_set_object(&keyed);
        ASSERT(peer_lifecycle_dump_state_json(&keyed, "incidents"));
        ASSERT(strcmp(json_get_str(json_get(&keyed, "schema")),
                      "zcl.peer_incidents.v1") == 0);
        ASSERT(json_get(&keyed, "peers") == NULL);
        json_free(&keyed);
    } TEST_END
    return failures;
}

int test_peer_lifecycle(void)
{
    int failures = 0;
    failures += test_peer_lifecycle_user_agent();
    failures += test_peer_lifecycle_classify();
    failures += test_peer_lifecycle_version_build();
    failures += test_peer_lifecycle_learns_inbound_advertised_addr();
    failures += test_peer_lifecycle_skips_inbound_ephemeral_cache();
    failures += test_peer_lifecycle_counters();
    failures += test_peer_lifecycle_inbound_source_bucket();
    failures += test_peer_lifecycle_duplicate_connect_duration();
    failures += test_peer_lifecycle_inbound_classifies_remote_version();
    failures += test_peer_lifecycle_incident_view();

    return failures;
}
