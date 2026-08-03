/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Pure Kademlia DHT routing core for the ZCODE metaverse overlay. */

#ifndef ZCL_VCS_ZCODE_DHT_H
#define ZCL_VCS_ZCODE_DHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed Kademlia parameters. VCS_ZCODE_DHT_LOOKUP_CEILING_S bounds the
 * wall-clock a transport-layer lookup may run; the pure core never reads a
 * clock, so the constant lives here for callers to enforce. */
#define VCS_ZCODE_DHT_K 16u
#define VCS_ZCODE_DHT_ALPHA 3u
#define VCS_ZCODE_DHT_MAX_CONTACTS 1024u
#define VCS_ZCODE_DHT_LOOKUP_CEILING_S 30u

#define VCS_ZCODE_DHT_ID_BYTES 32u
#define VCS_ZCODE_DHT_BUCKET_COUNT 256u

#define VCS_ZCODE_DHT_NODE_ID_DOMAIN "zcl.zcode.dht.nodeid.v1"

#define VCS_ZCODE_DHT_CONTACTS_WIRE_VERSION 1u
#define VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES 14u
#define VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES 76u
#define VCS_ZCODE_DHT_CONTACTS_MAX_WIRE_BYTES \
    (VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES + \
     VCS_ZCODE_DHT_MAX_CONTACTS * VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES)

enum vcs_zcode_dht_error {
    VCS_ZCODE_DHT_OK = 0,
    VCS_ZCODE_DHT_ERR_NULL,
    VCS_ZCODE_DHT_ERR_VERSION,
    VCS_ZCODE_DHT_ERR_LIMIT,
    VCS_ZCODE_DHT_ERR_ID_ZERO,
    VCS_ZCODE_DHT_ERR_LAST_SEEN,
    VCS_ZCODE_DHT_ERR_WIRE_SIZE,
    VCS_ZCODE_DHT_ERR_WIRE_MAGIC,
    VCS_ZCODE_DHT_ERR_WIRE_ORDER,
};

const char *vcs_zcode_dht_error_string(enum vcs_zcode_dht_error error);

enum vcs_zcode_dht_add_result {
    VCS_ZCODE_DHT_ADD_ADDED = 0,
    VCS_ZCODE_DHT_ADD_REFRESHED,
    VCS_ZCODE_DHT_ADD_EVICTED_TO_ADD,
    VCS_ZCODE_DHT_ADD_REJECTED_SELF,
    VCS_ZCODE_DHT_ADD_REJECTED_ZERO_ID,
};

const char *vcs_zcode_dht_add_result_string(
    enum vcs_zcode_dht_add_result result);

/* A contact is deliberately address-free: this pure core routes on node
 * identity only, and address resolution belongs to the transport layer
 * above it. last_seen_unix is caller-supplied; the module never reads a
 * clock, so all ordering decisions stay deterministic. */
struct vcs_zcode_dht_contact {
    uint8_t node_id[VCS_ZCODE_DHT_ID_BYTES];
    uint8_t zid_root[VCS_ZCODE_DHT_ID_BYTES];
    int64_t last_seen_unix;
    uint32_t consecutive_failures;
};

/* Caller-allocated fixed-size routing table: 256 buckets of K contacts,
 * addressed by the XOR-distance bucket index against self_id. No heap, no
 * globals, no I/O. */
struct vcs_zcode_dht_table {
    uint8_t self_id[VCS_ZCODE_DHT_ID_BYTES];
    uint32_t contact_count;
    uint8_t bucket_sizes[VCS_ZCODE_DHT_BUCKET_COUNT];
    struct vcs_zcode_dht_contact buckets[VCS_ZCODE_DHT_BUCKET_COUNT]
                                        [VCS_ZCODE_DHT_K];
};

/* node_id = SHA3-256(domain || genesis || zid_root || delayed_block_hash),
 * the domain string hashed including its NUL terminator (sibling-module
 * convention). Any NULL or all-zero input is rejected. */
bool vcs_zcode_dht_node_id(uint8_t out[32],
                           const uint8_t network_genesis_root[32],
                           const uint8_t zid_root[32],
                           const uint8_t delayed_block_hash[32]);

void vcs_zcode_dht_xor_distance(const uint8_t a[32], const uint8_t b[32],
                                uint8_t out[32]);

/* Bucket index 0..255 by the position of the most significant set bit of the
 * distance: 255 is the MSB of byte 0, 0 is the LSB of byte 31. A zero
 * distance (self) yields -1. */
int vcs_zcode_dht_bucket_index(const uint8_t distance[32]);

/* self_id must be non-zero. */
bool vcs_zcode_dht_table_init(struct vcs_zcode_dht_table *table,
                              const uint8_t self_id[32]);

/* Insert or refresh. A known node_id refreshes in place (zid_root and
 * last_seen updated, failures reset) and never duplicates. Self and zero
 * node ids are rejected. A new contact lands in its distance bucket; when
 * the bucket is full the least-recently-seen contact in that bucket is
 * evicted to make room, and when the table is at VCS_ZCODE_DHT_MAX_CONTACTS
 * the table-wide least-recently-seen contact is evicted instead (ties
 * broken by smaller node_id bytes), keeping the 1024-contact cap absolute. */
enum vcs_zcode_dht_add_result vcs_zcode_dht_table_add_contact(
    struct vcs_zcode_dht_table *table,
    const struct vcs_zcode_dht_contact *contact);

/* Refresh last_seen and reset consecutive_failures. */
bool vcs_zcode_dht_table_touch(struct vcs_zcode_dht_table *table,
                               const uint8_t node_id[32],
                               int64_t last_seen_unix);

bool vcs_zcode_dht_table_note_failure(struct vcs_zcode_dht_table *table,
                                      const uint8_t node_id[32]);

bool vcs_zcode_dht_table_remove(struct vcs_zcode_dht_table *table,
                                const uint8_t node_id[32]);

bool vcs_zcode_dht_table_find(const struct vcs_zcode_dht_table *table,
                              const uint8_t node_id[32],
                              struct vcs_zcode_dht_contact *out);

/* Copy the up-to-max closest contacts to target_id into out_contacts,
 * sorted ascending by XOR distance with a node_id tie-break. Returns the
 * number of contacts written. */
size_t vcs_zcode_dht_table_closest(
    const struct vcs_zcode_dht_table *table,
    const uint8_t target_id[32],
    struct vcs_zcode_dht_contact *out_contacts, size_t max);

uint32_t vcs_zcode_dht_table_count(const struct vcs_zcode_dht_table *table);

/* Canonical contact-set persistence codec, wire name zcode_dht_contacts.v1:
 * 8-byte magic {'Z','C','D','H','T','C',0x0D,0x0A}, u16 LE schema version
 * (=1), u32 LE contact count, then count entries of the fixed layout
 * node_id(32) || zid_root(32) || last_seen_unix(i64 LE) ||
 * consecutive_failures(u32 LE). Serialize emits contacts sorted by node_id
 * ascending, so equal sets produce byte-identical wire. */
size_t vcs_zcode_dht_contacts_wire_bytes(uint32_t count);

/* contacts must hold unique node ids; input order never affects output. */
enum vcs_zcode_dht_error vcs_zcode_dht_contacts_serialize(
    const struct vcs_zcode_dht_contact *contacts, uint32_t count,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out);

/* Rejects wrong magic, wrong version, any length other than
 * header + count*entry exactly (trailing bytes rejected), count over
 * VCS_ZCODE_DHT_MAX_CONTACTS, zero node ids, and negative last_seen. */
enum vcs_zcode_dht_error vcs_zcode_dht_contacts_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_dht_contact *contacts_out, uint32_t contact_capacity,
    uint32_t *count_out);

#endif /* ZCL_VCS_ZCODE_DHT_H */
