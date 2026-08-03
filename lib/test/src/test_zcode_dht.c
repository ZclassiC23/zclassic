/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Golden vectors and table semantics for the pure ZCODE DHT core. */

#include "test/test_core.h"

#include "base/hex.h"
#include "vcs/zcode_dht.h"

#include <stdio.h>
#include <string.h>

static void zdht_id(uint8_t id[32], uint8_t value)
{
    memset(id, value, 32);
}

/* Contact id with its most significant set bit at absolute position top_pos
 * (0 = MSB of byte 0) and the counter r encoded in the bits right below it.
 * Against a self id whose only set bit is at position 255, the XOR distance
 * keeps its MSB at top_pos, so top_pos selects the routing bucket exactly. */
static void zdht_cap_id(uint8_t id[32], uint32_t top_pos, uint32_t r)
{
    memset(id, 0, 32);
    id[top_pos / 8] |= (uint8_t)(1u << (7 - (top_pos % 8)));
    for (uint32_t k = 0; k < 8; k++) {
        uint32_t pos = top_pos + 1 + k;
        if (pos >= 256) break;
        if (r & (1u << k)) id[pos / 8] |= (uint8_t)(1u << (7 - (pos % 8)));
    }
}

static void zdht_contact(struct vcs_zcode_dht_contact *contact,
                         const uint8_t node_id[32], uint8_t zid_byte,
                         int64_t last_seen, uint32_t failures)
{
    memset(contact, 0, sizeof(*contact));
    memcpy(contact->node_id, node_id, 32);
    zdht_id(contact->zid_root, zid_byte);
    contact->last_seen_unix = last_seen;
    contact->consecutive_failures = failures;
}

static void zdht_self_lsb(uint8_t self[32])
{
    memset(self, 0, 32);
    self[31] = 0x01;
}

static int test_zdht_node_id_golden(void)
{
    int failures = 0;
    TEST("zcode dht: node id golden vectors") {
        /* Pinned against python3 hashlib.sha3_256 over
         * b"zcl.zcode.dht.nodeid.v1\x00" || genesis || zid || block. */
        uint8_t genesis[32], zid[32], block[32], out[32];
        char hex[65];
        zdht_id(genesis, 0x01);
        zdht_id(zid, 0x11);
        zdht_id(block, 0x22);
        ASSERT(vcs_zcode_dht_node_id(out, genesis, zid, block));
        zcl_hex_encode(out, 32, hex);
        ASSERT_STR_EQ(hex,
            "7a14deaa1ca7ffaaf30e0359aa89c8cc5b95b92e35800d00d6e2141032f7d3a7");

        zdht_id(genesis, 0xaa);
        zdht_id(zid, 0xbb);
        zdht_id(block, 0xcc);
        ASSERT(vcs_zcode_dht_node_id(out, genesis, zid, block));
        zcl_hex_encode(out, 32, hex);
        ASSERT_STR_EQ(hex,
            "4441cb032a69d0e23363e7ae1f3740c5d1be88a59ef972bdf0d5b7968f0c3715");
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_node_id_rejection(void)
{
    int failures = 0;
    TEST("zcode dht: node id rejects NULL and zero inputs") {
        uint8_t genesis[32], zid[32], block[32], out[32];
        zdht_id(genesis, 0x01);
        zdht_id(zid, 0x11);
        zdht_id(block, 0x22);
        ASSERT(!vcs_zcode_dht_node_id(NULL, genesis, zid, block));
        ASSERT(!vcs_zcode_dht_node_id(out, NULL, zid, block));
        ASSERT(!vcs_zcode_dht_node_id(out, genesis, NULL, block));
        ASSERT(!vcs_zcode_dht_node_id(out, genesis, zid, NULL));
        uint8_t zero[32] = {0};
        ASSERT(!vcs_zcode_dht_node_id(out, zero, zid, block));
        ASSERT(!vcs_zcode_dht_node_id(out, genesis, zero, block));
        ASSERT(!vcs_zcode_dht_node_id(out, genesis, zid, zero));
        /* Rejection zeroes the output. */
        ASSERT(memcmp(out, zero, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_distance_and_bucket(void)
{
    int failures = 0;
    TEST("zcode dht: xor distance and bucket index") {
        uint8_t a[32], b[32], distance[32];
        zdht_id(a, 0x0f);
        zdht_id(b, 0xf0);
        vcs_zcode_dht_xor_distance(a, b, distance);
        uint8_t expected[32];
        zdht_id(expected, 0xff);
        ASSERT(memcmp(distance, expected, 32) == 0);
        vcs_zcode_dht_xor_distance(a, a, distance);
        ASSERT_EQ(vcs_zcode_dht_bucket_index(distance), -1);

        memset(distance, 0, 32);
        distance[0] = 0x80;
        ASSERT_EQ(vcs_zcode_dht_bucket_index(distance), 255);
        distance[0] = 0x01;
        ASSERT_EQ(vcs_zcode_dht_bucket_index(distance), 248);
        memset(distance, 0, 32);
        distance[31] = 0x01;
        ASSERT_EQ(vcs_zcode_dht_bucket_index(distance), 0);
        distance[31] = 0xff;
        ASSERT_EQ(vcs_zcode_dht_bucket_index(distance), 7);
        ASSERT_EQ(vcs_zcode_dht_bucket_index(NULL), -1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_table_add_and_reject(void)
{
    int failures = 0;
    TEST("zcode dht: table add, refresh, and rejection") {
        struct vcs_zcode_dht_table table;
        uint8_t self[32], id_a[32], zero[32] = {0};
        zdht_self_lsb(self);
        zdht_id(id_a, 0x42);
        ASSERT(!vcs_zcode_dht_table_init(NULL, self));
        ASSERT(!vcs_zcode_dht_table_init(&table, zero));
        ASSERT(vcs_zcode_dht_table_init(&table, self));
        ASSERT_EQ(vcs_zcode_dht_table_count(&table), 0);

        struct vcs_zcode_dht_contact contact;
        zdht_contact(&contact, id_a, 0x11, 1000, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_ADDED);
        ASSERT_EQ(vcs_zcode_dht_table_count(&table), 1);

        /* Re-add refreshes in place: no duplicate, fields updated. */
        zdht_contact(&contact, id_a, 0x22, 2000, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_REFRESHED);
        ASSERT_EQ(vcs_zcode_dht_table_count(&table), 1);
        struct vcs_zcode_dht_contact found;
        ASSERT(vcs_zcode_dht_table_find(&table, id_a, &found));
        ASSERT_EQ(found.last_seen_unix, 2000);
        zdht_id(zero, 0x22);
        ASSERT(memcmp(found.zid_root, zero, 32) == 0);
        memset(zero, 0, 32);

        zdht_contact(&contact, self, 0x11, 3000, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_REJECTED_SELF);
        zdht_contact(&contact, zero, 0x11, 3000, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_REJECTED_ZERO_ID);
        ASSERT_EQ(vcs_zcode_dht_table_count(&table), 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_table_bucket_eviction(void)
{
    int failures = 0;
    TEST("zcode dht: full bucket evicts least-recently-seen, refresh protects") {
        struct vcs_zcode_dht_table table;
        uint8_t self[32];
        zdht_id(self, 0x01);
        ASSERT(vcs_zcode_dht_table_init(&table, self));

        /* self starts 0x01, so an id starting 0x80 puts the distance MSB at
         * byte 0 bit 7: every contact below lands in bucket 255. */
        uint8_t ids[18][32];
        for (size_t i = 0; i < 18; i++) {
            memset(ids[i], 0, 32);
            ids[i][0] = 0x80;
            ids[i][31] = (uint8_t)i;
        }
        for (size_t i = 0; i < VCS_ZCODE_DHT_K; i++) {
            struct vcs_zcode_dht_contact contact;
            zdht_contact(&contact, ids[i], 0x11, 1000 + (int64_t)i, 0);
            ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                      VCS_ZCODE_DHT_ADD_ADDED);
        }
        ASSERT_EQ(vcs_zcode_dht_table_count(&table), VCS_ZCODE_DHT_K);

        /* 17th add to the full bucket evicts contact 0 (oldest seen). */
        struct vcs_zcode_dht_contact contact, found;
        zdht_contact(&contact, ids[16], 0x11, 1000 + 16, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_EVICTED_TO_ADD);
        ASSERT_EQ(vcs_zcode_dht_table_count(&table), VCS_ZCODE_DHT_K);
        ASSERT(!vcs_zcode_dht_table_find(&table, ids[0], &found));
        ASSERT(vcs_zcode_dht_table_find(&table, ids[16], &found));

        /* Refreshing contact 1 makes contact 2 the eviction victim. */
        ASSERT(vcs_zcode_dht_table_touch(&table, ids[1], 9000));
        zdht_contact(&contact, ids[17], 0x11, 1000 + 17, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_EVICTED_TO_ADD);
        ASSERT(vcs_zcode_dht_table_find(&table, ids[1], &found));
        ASSERT(!vcs_zcode_dht_table_find(&table, ids[2], &found));
        ASSERT(vcs_zcode_dht_table_find(&table, ids[17], &found));
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_table_cap(void)
{
    int failures = 0;
    TEST("zcode dht: 1024-contact cap evicts table-wide lru") {
        struct vcs_zcode_dht_table table;
        uint8_t self[32], id[32], first_id[32];
        zdht_self_lsb(self);
        ASSERT(vcs_zcode_dht_table_init(&table, self));

        /* 1024 unique contacts spread so no bucket exceeds 10: 4 each at
         * top-bit positions 0..253, 2 at 254, and 6 extras at position 0.
         * last_seen matches insertion order, so the first add is the lru. */
        uint32_t added = 0;
        for (uint32_t p = 0; p <= 253 && !failures; p++) {
            for (uint32_t r = 0; r < 4; r++) {
                struct vcs_zcode_dht_contact contact;
                zdht_cap_id(id, p, r);
                if (added == 0) memcpy(first_id, id, 32);
                zdht_contact(&contact, id, 0x11, (int64_t)added, 0);
                ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                          VCS_ZCODE_DHT_ADD_ADDED);
                added++;
            }
        }
        for (uint32_t r = 0; r < 2 && !failures; r++) {
            struct vcs_zcode_dht_contact contact;
            zdht_cap_id(id, 254, r);
            zdht_contact(&contact, id, 0x11, (int64_t)added, 0);
            ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                      VCS_ZCODE_DHT_ADD_ADDED);
            added++;
        }
        for (uint32_t r = 4; r < 10 && !failures; r++) {
            struct vcs_zcode_dht_contact contact;
            zdht_cap_id(id, 0, r);
            zdht_contact(&contact, id, 0x11, (int64_t)added, 0);
            ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                      VCS_ZCODE_DHT_ADD_ADDED);
            added++;
        }
        ASSERT_EQ(added, VCS_ZCODE_DHT_MAX_CONTACTS);
        ASSERT_EQ(vcs_zcode_dht_table_count(&table),
                  VCS_ZCODE_DHT_MAX_CONTACTS);

        /* One more: the table-wide lru (the first contact added) is evicted
         * and the cap holds. */
        struct vcs_zcode_dht_contact contact, found;
        zdht_cap_id(id, 0, 10);
        zdht_contact(&contact, id, 0x11, 10000, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_EVICTED_TO_ADD);
        ASSERT_EQ(vcs_zcode_dht_table_count(&table),
                  VCS_ZCODE_DHT_MAX_CONTACTS);
        ASSERT(!vcs_zcode_dht_table_find(&table, first_id, &found));
        zdht_cap_id(id, 0, 10);
        ASSERT(vcs_zcode_dht_table_find(&table, id, &found));
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_table_closest(void)
{
    int failures = 0;
    TEST("zcode dht: closest ordering and max bound") {
        struct vcs_zcode_dht_table table;
        uint8_t self[32], id_a[32], id_b[32], id_c[32], target[32];
        zdht_id(self, 0xaa);
        ASSERT(vcs_zcode_dht_table_init(&table, self));
        memset(id_a, 0, 32);
        id_a[31] = 0x05; /* distance 5 to the zero target */
        memset(id_b, 0, 32);
        id_b[31] = 0x03; /* distance 3 */
        memset(id_c, 0, 32);
        id_c[0] = 0x01; /* distance 2^248 */
        memset(target, 0, 32);
        struct vcs_zcode_dht_contact contact;
        zdht_contact(&contact, id_c, 0x11, 100, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_ADDED);
        zdht_contact(&contact, id_a, 0x11, 100, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_ADDED);
        zdht_contact(&contact, id_b, 0x11, 100, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_ADDED);

        struct vcs_zcode_dht_contact out[3];
        ASSERT_EQ(vcs_zcode_dht_table_closest(&table, target, out, 3), 3);
        ASSERT(memcmp(out[0].node_id, id_b, 32) == 0);
        ASSERT(memcmp(out[1].node_id, id_a, 32) == 0);
        ASSERT(memcmp(out[2].node_id, id_c, 32) == 0);

        ASSERT_EQ(vcs_zcode_dht_table_closest(&table, target, out, 2), 2);
        ASSERT(memcmp(out[0].node_id, id_b, 32) == 0);
        ASSERT(memcmp(out[1].node_id, id_a, 32) == 0);

        /* A target equal to a known id finds that id at distance zero. */
        ASSERT_EQ(vcs_zcode_dht_table_closest(&table, id_c, out, 1), 1);
        ASSERT(memcmp(out[0].node_id, id_c, 32) == 0);
        ASSERT_EQ(vcs_zcode_dht_table_closest(&table, target, out, 0), 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_table_touch_failure_remove_find(void)
{
    int failures = 0;
    TEST("zcode dht: touch, note_failure, remove, find") {
        struct vcs_zcode_dht_table table;
        uint8_t self[32], id_a[32], id_b[32];
        zdht_id(self, 0x01);
        zdht_id(id_a, 0x80);
        zdht_id(id_b, 0x81);
        ASSERT(vcs_zcode_dht_table_init(&table, self));
        struct vcs_zcode_dht_contact contact, found;
        zdht_contact(&contact, id_a, 0x11, 100, 0);
        ASSERT_EQ(vcs_zcode_dht_table_add_contact(&table, &contact),
                  VCS_ZCODE_DHT_ADD_ADDED);

        ASSERT(vcs_zcode_dht_table_note_failure(&table, id_a));
        ASSERT(vcs_zcode_dht_table_note_failure(&table, id_a));
        ASSERT(vcs_zcode_dht_table_find(&table, id_a, &found));
        ASSERT_EQ(found.consecutive_failures, 2);
        ASSERT(!vcs_zcode_dht_table_note_failure(&table, id_b));

        ASSERT(vcs_zcode_dht_table_touch(&table, id_a, 500));
        ASSERT(vcs_zcode_dht_table_find(&table, id_a, &found));
        ASSERT_EQ(found.last_seen_unix, 500);
        ASSERT_EQ(found.consecutive_failures, 0);
        ASSERT(!vcs_zcode_dht_table_touch(&table, id_b, 500));

        ASSERT(vcs_zcode_dht_table_remove(&table, id_a));
        ASSERT_EQ(vcs_zcode_dht_table_count(&table), 0);
        ASSERT(!vcs_zcode_dht_table_find(&table, id_a, &found));
        ASSERT(!vcs_zcode_dht_table_remove(&table, id_a));
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_codec_roundtrip(void)
{
    int failures = 0;
    TEST("zcode dht: contact codec round trip is canonical ascending") {
        struct vcs_zcode_dht_contact contacts[4];
        const uint8_t id_bytes[4] = {0x04, 0x02, 0x03, 0x01};
        for (size_t i = 0; i < 4; i++) {
            uint8_t id[32];
            zdht_id(id, id_bytes[i]);
            zdht_contact(&contacts[i], id, (uint8_t)(0x10 + id_bytes[i]),
                         100 + (int64_t)i, (uint32_t)i);
        }
        uint8_t wire[VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES +
                     4 * VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_contacts_wire_bytes(4), sizeof(wire));
        ASSERT_EQ(vcs_zcode_dht_contacts_serialize(
                      contacts, 4, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire_len, sizeof(wire));
        /* Unsorted input must serialize in ascending node_id order. */
        for (size_t i = 0; i < 4; i++) {
            uint8_t expected[32];
            zdht_id(expected, (uint8_t)(i + 1));
            ASSERT(memcmp(wire + VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES +
                              i * VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES,
                          expected, 32) == 0);
        }

        struct vcs_zcode_dht_contact parsed[4];
        uint32_t count = 0;
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(
                      wire, wire_len, parsed, 4, &count), VCS_ZCODE_DHT_OK);
        ASSERT_EQ(count, 4);
        for (size_t i = 0; i < 4; i++) {
            uint8_t expected_id[32], expected_zid[32];
            zdht_id(expected_id, (uint8_t)(i + 1));
            zdht_id(expected_zid, (uint8_t)(0x11 + i));
            ASSERT(memcmp(parsed[i].node_id, expected_id, 32) == 0);
            ASSERT(memcmp(parsed[i].zid_root, expected_zid, 32) == 0);
        }
        /* Re-serializing the parsed set reproduces identical bytes. */
        uint8_t wire2[sizeof(wire)];
        size_t wire2_len = 0;
        ASSERT_EQ(vcs_zcode_dht_contacts_serialize(
                      parsed, count, wire2, sizeof(wire2), &wire2_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire2_len, wire_len);
        ASSERT(memcmp(wire, wire2, wire_len) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_codec_golden(void)
{
    int failures = 0;
    TEST("zcode dht: two-contact wire golden") {
        struct vcs_zcode_dht_contact contacts[2];
        uint8_t id[32];
        zdht_id(id, 0x01);
        zdht_contact(&contacts[0], id, 0x11, 1700000000, 0);
        zdht_id(id, 0x02);
        zdht_contact(&contacts[1], id, 0x22, 1700000100, 7);
        /* Feed unsorted: the golden bytes are the canonical ascending form. */
        struct vcs_zcode_dht_contact swapped[2] = {contacts[1], contacts[0]};
        uint8_t wire[166];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_contacts_serialize(
                      swapped, 2, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(wire_len, sizeof(wire));
        static const char golden_hex[] =
            "5a43444854430d0a010002000000"
            "0101010101010101010101010101010101010101010101010101010101010101"
            "1111111111111111111111111111111111111111111111111111111111111111"
            "00f1536500000000"
            "00000000"
            "0202020202020202020202020202020202020202020202020202020202020202"
            "2222222222222222222222222222222222222222222222222222222222222222"
            "64f1536500000000"
            "07000000";
        char hex[2 * sizeof(wire) + 1];
        zcl_hex_encode(wire, wire_len, hex);
        ASSERT_STR_EQ(hex, golden_hex);

        uint8_t golden_wire[sizeof(wire)];
        test_hex_to_bytes(golden_hex, golden_wire, (int)sizeof(golden_wire));
        struct vcs_zcode_dht_contact parsed[2];
        memset(parsed, 0, sizeof(parsed));
        uint32_t count = 0;
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(
                      golden_wire, sizeof(golden_wire), parsed, 2, &count),
                  VCS_ZCODE_DHT_OK);
        ASSERT_EQ(count, 2);
        ASSERT_EQ(parsed[0].last_seen_unix, 1700000000);
        ASSERT_EQ(parsed[0].consecutive_failures, 0);
        ASSERT_EQ(parsed[1].last_seen_unix, 1700000100);
        ASSERT_EQ(parsed[1].consecutive_failures, 7);
        ASSERT(memcmp(parsed, contacts, sizeof(contacts)) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zdht_codec_reject(void)
{
    int failures = 0;
    TEST("zcode dht: codec rejects malformed wire") {
        struct vcs_zcode_dht_contact contacts[2];
        uint8_t id[32];
        zdht_id(id, 0x01);
        zdht_contact(&contacts[0], id, 0x11, 100, 0);
        zdht_id(id, 0x02);
        zdht_contact(&contacts[1], id, 0x22, 200, 3);
        uint8_t wire[VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES +
                     2 * VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES + 1];
        size_t wire_len = 0;
        ASSERT_EQ(vcs_zcode_dht_contacts_serialize(
                      contacts, 2, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);

        struct vcs_zcode_dht_contact parsed[2];
        uint32_t count = 0;
        uint8_t saved = wire[0];
        wire[0] ^= 0xff;
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(wire, wire_len, parsed, 2,
                                               &count),
                  VCS_ZCODE_DHT_ERR_WIRE_MAGIC);
        wire[0] = saved;
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(wire, wire_len - 1, parsed, 2,
                                               &count),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(wire, wire_len + 1, parsed, 2,
                                               &count),
                  VCS_ZCODE_DHT_ERR_WIRE_SIZE);
        saved = wire[8];
        wire[8] = 2; /* schema version */
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(wire, wire_len, parsed, 2,
                                               &count),
                  VCS_ZCODE_DHT_ERR_VERSION);
        wire[8] = saved;
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(wire, wire_len, parsed, 1,
                                               &count),
                  VCS_ZCODE_DHT_ERR_LIMIT);

        /* Count over the 1024 cap fails before any entry is read. */
        uint8_t header[VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES];
        memcpy(header, wire, sizeof(header));
        header[10] = 0x01;
        header[11] = 0x04; /* count = 1025 LE at bytes 10..13 */
        header[12] = 0;
        header[13] = 0;
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(header, sizeof(header), parsed,
                                               2, &count),
                  VCS_ZCODE_DHT_ERR_LIMIT);

        memset(wire + VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES, 0, 32);
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(wire, wire_len, parsed, 2,
                                               &count),
                  VCS_ZCODE_DHT_ERR_ID_ZERO);
        ASSERT_EQ(vcs_zcode_dht_contacts_serialize(
                      contacts, 2, wire, sizeof(wire), &wire_len),
                  VCS_ZCODE_DHT_OK);
        memset(wire + VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES + 64, 0xff, 8);
        ASSERT_EQ(vcs_zcode_dht_contacts_parse(wire, wire_len, parsed, 2,
                                               &count),
                  VCS_ZCODE_DHT_ERR_LAST_SEEN);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_dht(void)
{
    int failures = 0;
    failures += test_zdht_node_id_golden();
    failures += test_zdht_node_id_rejection();
    failures += test_zdht_distance_and_bucket();
    failures += test_zdht_table_add_and_reject();
    failures += test_zdht_table_bucket_eviction();
    failures += test_zdht_table_cap();
    failures += test_zdht_table_closest();
    failures += test_zdht_table_touch_failure_remove_find();
    failures += test_zdht_codec_roundtrip();
    failures += test_zdht_codec_golden();
    failures += test_zdht_codec_reject();
    printf("=== zcode_dht: %d failures ===\n", failures);
    return failures;
}
