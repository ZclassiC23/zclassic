/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_core.h"
#include "vcs/zcode_dht_msgs.h"

#include <stdio.h>
#include <string.h>

static void id32(uint8_t out[32], uint8_t v) { memset(out, v, 32); }
static void qid(uint8_t out[16], uint8_t v)
{
    for (size_t i = 0; i < 16; i++) out[i] = (uint8_t)(v + i);
}

static int test_find_node(void)
{
    int failures = 0;
    TEST("zcode dht msgs: FIND_NODE is exact and bounded") {
        struct vcs_zcode_dht_msg_find_node m;
        id32(m.sender_node_id, 0x11); qid(m.query_id, 1);
        id32(m.target_node_id, 0x22);
        uint8_t wire[VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES + 1]; size_t len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &m, wire, sizeof(wire), &len), VCS_ZCODE_DHT_OK);
        ASSERT_EQ(len, VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES);
        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, len, &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.kind, VCS_ZCODE_DHT_MSG_FIND_NODE);
        ASSERT(memcmp(parsed.find_node.target_node_id, m.target_node_id, 32) == 0);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, len + 1, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        memset(wire + VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 32, 0, 16);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, len, &parsed),
                  VCS_ZCODE_DHT_ERR_QUERY_ID);
        PASS();
    } _test_next:;
    return failures;
}

static int test_nodes_hints(void)
{
    int failures = 0;
    TEST("zcode dht msgs: NODES carries ordered identity hints only") {
        struct vcs_zcode_dht_msg_nodes m; memset(&m, 0, sizeof(m));
        id32(m.sender_node_id, 0x11); qid(m.query_id, 1); m.contact_count = 3;
        id32(m.node_ids[0], 1); id32(m.node_ids[1], 2); id32(m.node_ids[2], 3);
        uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES + 1]; size_t len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &m, wire, sizeof(wire), &len), VCS_ZCODE_DHT_OK);
        ASSERT_EQ(len, VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 32 + 16 + 1 + 96);
        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, len, &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.nodes.contact_count, 3);
        ASSERT(memcmp(parsed.nodes.node_ids[2], m.node_ids[2], 32) == 0);
        struct vcs_zcode_dht_msg_nodes bad = m;
        memcpy(bad.node_ids[1], bad.node_ids[0], 32);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &bad, wire, sizeof(wire), &len),
                  VCS_ZCODE_DHT_ERR_WIRE_ORDER);
        bad = m; memset(bad.node_ids[1], 0, 32);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &bad, wire, sizeof(wire), &len),
                  VCS_ZCODE_DHT_ERR_ID_ZERO);
        bad = m; bad.contact_count = VCS_ZCODE_DHT_K + 1;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &bad, wire, sizeof(wire), &len),
                  VCS_ZCODE_DHT_ERR_LIMIT);
        PASS();
    } _test_next:;
    return failures;
}

static int test_nodes_parse_poison(void)
{
    int failures = 0;
    TEST("zcode dht msgs: poisoned hint sets and trailing bytes reject") {
        struct vcs_zcode_dht_msg_nodes m; memset(&m, 0, sizeof(m));
        id32(m.sender_node_id, 0x11); qid(m.query_id, 1); m.contact_count = 2;
        id32(m.node_ids[0], 1); id32(m.node_ids[1], 2);
        uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES + 1]; size_t len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &m, wire, sizeof(wire), &len), VCS_ZCODE_DHT_OK);
        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, len + 1, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        size_t ids = VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 32 + 16 + 1;
        memcpy(wire + ids + 32, wire + ids, 32);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, len, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_ORDER);
        memset(wire + ids, 0, 32);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, len, &parsed),
                  VCS_ZCODE_DHT_ERR_ID_ZERO);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_dht_msgs(void)
{
    int failures = 0;
    failures += test_find_node();
    failures += test_nodes_hints();
    failures += test_nodes_parse_poison();
    printf("=== zcode_dht_msgs: %d failures ===\n", failures);
    return failures;
}
