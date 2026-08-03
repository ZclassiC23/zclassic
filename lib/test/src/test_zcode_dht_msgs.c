/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Golden vectors and codec semantics for the ZCODE DHT messages. */

#include "test/test_core.h"

#include "base/hex.h"
#include "vcs/zcode_dht_msgs.h"

#include <stdio.h>
#include <string.h>

static void zdhtm_qid(uint8_t query_id[16], uint8_t base)
{
    for (size_t i = 0; i < 16; i++) query_id[i] = (uint8_t)(base + i);
}

static void zdhtm_id(uint8_t id[32], uint8_t value)
{
    memset(id, value, 32);
}

static void zdhtm_contact(struct vcs_zcode_dht_contact *contact,
                          uint8_t id_byte, uint8_t zid_byte,
                          int64_t last_seen, uint32_t failures)
{
    memset(contact, 0, sizeof(*contact));
    zdhtm_id(contact->node_id, id_byte);
    zdhtm_id(contact->zid_root, zid_byte);
    contact->last_seen_unix = last_seen;
    contact->consecutive_failures = failures;
}

static int test_zdhtm_find_node_roundtrip(void)
{
    int failures = 0;
    TEST("zcode dht msgs: find_node round trip is canonical") {
        struct vcs_zcode_dht_msg_find_node msg;
        zdhtm_qid(msg.query_id, 0x00);
        zdhtm_id(msg.target_node_id, 0xa5);
        uint8_t wire[VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire_len, VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES);

        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.kind, VCS_ZCODE_DHT_MSG_FIND_NODE);
        ASSERT(memcmp(parsed.find_node.query_id, msg.query_id, 16) == 0);
        ASSERT(memcmp(parsed.find_node.target_node_id,
                      msg.target_node_id, 32) == 0);

        /* serialize(parse(wire)) reproduces the input wire byte-for-byte. */
        uint8_t wire2[sizeof(wire)];
        size_t wire2_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &parsed.find_node, wire2, sizeof(wire2), &wire2_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire2_len, wire_len);
        ASSERT(memcmp(wire, wire2, wire_len) == 0);

        /* Exact capacity passes; one byte short fails. */
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &msg, wire, VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES,
                      &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &msg, wire, VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES - 1,
                      &wire_len),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        ASSERT_EQ(wire_len, 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdhtm_find_node_golden(void)
{
    int failures = 0;
    TEST("zcode dht msgs: find_node wire golden") {
        /* Pinned against a python3 struct/bytes recompute of the spec. */
        static const char golden_hex[] =
            "5a434448544d0d0a0100010001020304"
            "05060708090a0b0c0d0e0f2021222324"
            "25262728292a2b2c2d2e2f3031323334"
            "35363738393a3b3c3d3e3f";
        struct vcs_zcode_dht_msg_find_node msg;
        zdhtm_qid(msg.query_id, 0x00);
        for (size_t i = 0; i < 32; i++)
            msg.target_node_id[i] = (uint8_t)(0x20 + i);
        uint8_t wire[VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        char hex[2 * sizeof(wire) + 1];
        zcl_hex_encode(wire, wire_len, hex);
        ASSERT_STR_EQ(hex, golden_hex);

        uint8_t golden_wire[sizeof(wire)];
        test_hex_to_bytes(golden_hex, golden_wire, (int)sizeof(golden_wire));
        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(
                      golden_wire, sizeof(golden_wire), &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.kind, VCS_ZCODE_DHT_MSG_FIND_NODE);
        ASSERT(memcmp(&parsed.find_node, &msg, sizeof(msg)) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdhtm_nodes_roundtrip(void)
{
    int failures = 0;
    TEST("zcode dht msgs: nodes round trip with 0, 1, and 16 contacts") {
        struct vcs_zcode_dht_msg_nodes msg;
        zdhtm_qid(msg.query_id, 0x40);

        /* Zero contacts: contacts_wire_len 0, no embedded blob. */
        msg.contact_count = 0;
        uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire_len, VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16 + 4);
        ASSERT_EQ(wire[VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16], 0);
        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.kind, VCS_ZCODE_DHT_MSG_NODES);
        ASSERT_EQ(parsed.nodes.contact_count, 0);
        ASSERT(memcmp(parsed.nodes.query_id, msg.query_id, 16) == 0);
        size_t wire2_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &parsed.nodes, wire, sizeof(wire), &wire2_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire2_len, wire_len);

        /* One contact. */
        msg.contact_count = 1;
        zdhtm_contact(&msg.contacts[0], 0x01, 0x11, 100, 0);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire_len, VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16 + 4 +
                  VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES +
                  VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.nodes.contact_count, 1);
        ASSERT(memcmp(&parsed.nodes.contacts[0], &msg.contacts[0],
                      sizeof(msg.contacts[0])) == 0);

        /* Sixteen contacts fed in reverse order: the embedded contacts codec
         * emits them canonically ascending, so the round trip still holds. */
        msg.contact_count = VCS_ZCODE_DHT_K;
        for (uint32_t i = 0; i < VCS_ZCODE_DHT_K; i++) {
            zdhtm_contact(&msg.contacts[i],
                          (uint8_t)(VCS_ZCODE_DHT_K - i),
                          (uint8_t)(0x10 + VCS_ZCODE_DHT_K - i),
                          1000 + (int64_t)i, i);
        }
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire_len, VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.nodes.contact_count, VCS_ZCODE_DHT_K);
        for (uint32_t i = 0; i < VCS_ZCODE_DHT_K; i++) {
            uint8_t expected_id[32];
            zdhtm_id(expected_id, (uint8_t)(i + 1));
            ASSERT(memcmp(parsed.nodes.contacts[i].node_id,
                          expected_id, 32) == 0);
        }
        uint8_t wire2[sizeof(wire)];
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &parsed.nodes, wire2, sizeof(wire2), &wire2_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire2_len, wire_len);
        ASSERT(memcmp(wire, wire2, wire_len) == 0);

        /* Exact capacity passes; one byte short fails. */
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, wire_len, &wire2_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, wire_len - 1, &wire2_len),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdhtm_nodes_golden(void)
{
    int failures = 0;
    TEST("zcode dht msgs: two-contact nodes wire golden") {
        /* Pinned against a python3 struct/bytes recompute of the spec: the
         * contacts are fed unsorted and the golden is the canonical form. */
        static const char golden_hex[] =
            "5a434448544d0d0a010002f0f1f2f3f4"
            "f5f6f7f8f9fafbfcfdfeffa60000005a"
            "43444854430d0a010002000000010101"
            "01010101010101010101010101010101"
            "01010101010101010101010101111111"
            "11111111111111111111111111111111"
            "1111111111111111111111111100f153"
            "65000000000000000002020202020202"
            "02020202020202020202020202020202"
            "02020202020202020222222222222222"
            "22222222222222222222222222222222"
            "22222222222222222264f15365000000"
            "0007000000";
        struct vcs_zcode_dht_msg_nodes msg;
        zdhtm_qid(msg.query_id, 0xf0);
        msg.contact_count = 2;
        zdhtm_contact(&msg.contacts[0], 0x02, 0x22, 1700000100, 7);
        zdhtm_contact(&msg.contacts[1], 0x01, 0x11, 1700000000, 0);
        uint8_t wire[197];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire_len, sizeof(wire));
        char hex[2 * sizeof(wire) + 1];
        zcl_hex_encode(wire, wire_len, hex);
        ASSERT_STR_EQ(hex, golden_hex);

        uint8_t golden_wire[sizeof(wire)];
        test_hex_to_bytes(golden_hex, golden_wire, (int)sizeof(golden_wire));
        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(
                      golden_wire, sizeof(golden_wire), &parsed),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(parsed.kind, VCS_ZCODE_DHT_MSG_NODES);
        ASSERT_EQ(parsed.nodes.contact_count, 2);
        ASSERT(memcmp(parsed.nodes.query_id, msg.query_id, 16) == 0);
        /* Parsed order is the canonical ascending order. */
        ASSERT_EQ(parsed.nodes.contacts[0].last_seen_unix, 1700000000);
        ASSERT_EQ(parsed.nodes.contacts[1].last_seen_unix, 1700000100);
        ASSERT_EQ(parsed.nodes.contacts[1].consecutive_failures, 7);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdhtm_parse_reject(void)
{
    int failures = 0;
    TEST("zcode dht msgs: parse rejects malformed wire") {
        struct vcs_zcode_dht_msg_find_node fn;
        zdhtm_qid(fn.query_id, 0x00);
        zdhtm_id(fn.target_node_id, 0xa5);
        uint8_t wire[VCS_ZCODE_DHT_FIND_NODE_WIRE_BYTES + 1];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &fn, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);

        struct vcs_zcode_dht_msg parsed;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(NULL, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_NULL);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, NULL),
                  VCS_ZCODE_DHT_ERR_NULL);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(
                      wire, VCS_ZCODE_DHT_MSGS_HEADER_BYTES - 1, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        uint8_t saved = wire[0];
        wire[0] ^= 0xff;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_MAGIC);
        wire[0] = saved;
        saved = wire[8];
        wire[8] = 2; /* schema version */
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_VERSION);
        wire[8] = saved;
        saved = wire[10];
        wire[10] = 3; /* unknown kind */
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_KIND);
        wire[10] = saved;
        /* Trailing bytes and truncation are both rejected. */
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len + 1, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len - 1, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        /* All-zero query_id, then all-zero target. */
        memset(wire + VCS_ZCODE_DHT_MSGS_HEADER_BYTES, 0, 16);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_QUERY_ID);
        zdhtm_qid(wire + VCS_ZCODE_DHT_MSGS_HEADER_BYTES, 0x00);
        memset(wire + VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16, 0, 32);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_ID_ZERO);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdhtm_nodes_parse_reject(void)
{
    int failures = 0;
    TEST("zcode dht msgs: nodes parse rejects malformed frames") {
        struct vcs_zcode_dht_msg_nodes msg;
        zdhtm_qid(msg.query_id, 0xf0);
        msg.contact_count = 1;
        zdhtm_contact(&msg.contacts[0], 0x01, 0x11, 100, 0);
        uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES + 1];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        const size_t body = VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16;

        struct vcs_zcode_dht_msg parsed;
        /* contacts_wire_len larger than the remaining bytes. */
        wire[body] = 0xff;
        wire[body + 1] = 0xff;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        /* Trailing bytes after a complete frame. */
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len + 1, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        /* Header too short to even hold query_id + contacts_wire_len. */
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, body + 3, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        /* All-zero query_id. */
        uint8_t saved[16];
        memcpy(saved, wire + VCS_ZCODE_DHT_MSGS_HEADER_BYTES, 16);
        memset(wire + VCS_ZCODE_DHT_MSGS_HEADER_BYTES, 0, 16);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_QUERY_ID);
        memcpy(wire + VCS_ZCODE_DHT_MSGS_HEADER_BYTES, saved, 16);

        /* Embedded blob with a bad magic propagates the codec error. */
        const size_t blob = body + 4;
        uint8_t saved_byte = wire[blob];
        wire[blob] ^= 0xff;
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_MAGIC);
        wire[blob] = saved_byte;
        /* Embedded blob with a trailing byte: contacts_wire_len consumes
         * the frame exactly, but the blob itself is oversized. */
        wire[body] = (uint8_t)(wire[body] + 1);
        ASSERT_EQ(vcs_zcode_dht_msg_parse(wire, wire_len + 1, &parsed),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &msg, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);

        /* A blob header declaring 17 contacts exceeds the K cap. */
        uint8_t over[VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16 + 4 +
                     VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES];
        memcpy(over, wire, VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16);
        size_t off = VCS_ZCODE_DHT_MSGS_HEADER_BYTES + 16;
        memset(over + off, 0, 4);
        over[off] = VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES;
        off += 4;
        memcpy(over + off, "ZCDHTC\x0d\x0a", 8);
        off += 8;
        over[off++] = 1;
        over[off++] = 0; /* contacts schema version */
        over[off++] = 17;
        over[off++] = 0;
        over[off++] = 0;
        over[off++] = 0; /* contact count 17 */
        ASSERT_EQ(off, sizeof(over));
        ASSERT_EQ(vcs_zcode_dht_msg_parse(over, sizeof(over), &parsed),
                  VCS_ZCODE_DHT_ERR_LIMIT);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdhtm_serialize_reject(void)
{
    int failures = 0;
    TEST("zcode dht msgs: serialize rejects invalid input") {
        struct vcs_zcode_dht_msg_find_node fn;
        zdhtm_qid(fn.query_id, 0x00);
        zdhtm_id(fn.target_node_id, 0xa5);
        uint8_t wire[VCS_ZCODE_DHT_NODES_MAX_WIRE_BYTES];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      NULL, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_NULL);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &fn, NULL, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_NULL);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &fn, wire, sizeof(wire), NULL),
                  VCS_ZCODE_DHT_ERR_NULL);
        struct vcs_zcode_dht_msg_find_node bad = fn;
        memset(bad.query_id, 0, 16);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &bad, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_QUERY_ID);
        bad = fn;
        memset(bad.target_node_id, 0, 32);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_find_node(
                      &bad, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_ID_ZERO);
        ASSERT_EQ(wire_len, 0);

        struct vcs_zcode_dht_msg_nodes nodes;
        zdhtm_qid(nodes.query_id, 0xf0);
        nodes.contact_count = 1;
        zdhtm_contact(&nodes.contacts[0], 0x01, 0x11, 100, 0);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      NULL, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_NULL);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &nodes, NULL, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_NULL);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &nodes, wire, sizeof(wire), NULL),
                  VCS_ZCODE_DHT_ERR_NULL);
        struct vcs_zcode_dht_msg_nodes bad_nodes = nodes;
        memset(bad_nodes.query_id, 0, 16);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &bad_nodes, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_QUERY_ID);
        /* Seventeen contacts exceed the K cap. */
        bad_nodes = nodes;
        bad_nodes.contact_count = VCS_ZCODE_DHT_K + 1;
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &bad_nodes, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_LIMIT);
        /* Zero and duplicate contact node ids propagate the codec error. */
        bad_nodes = nodes;
        memset(bad_nodes.contacts[0].node_id, 0, 32);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &bad_nodes, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_ID_ZERO);
        bad_nodes = nodes;
        bad_nodes.contact_count = 2;
        zdhtm_contact(&bad_nodes.contacts[1], 0x01, 0x22, 200, 0);
        ASSERT_EQ(vcs_zcode_dht_msg_serialize_nodes(
                      &bad_nodes, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_ERR_WIRE_ORDER);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_dht_msgs(void)
{
    int failures = 0;
    failures += test_zdhtm_find_node_roundtrip();
    failures += test_zdhtm_find_node_golden();
    failures += test_zdhtm_nodes_roundtrip();
    failures += test_zdhtm_nodes_golden();
    failures += test_zdhtm_parse_reject();
    failures += test_zdhtm_nodes_parse_reject();
    failures += test_zdhtm_serialize_reject();
    printf("=== zcode_dht_msgs: %d failures ===\n", failures);
    return failures;
}
