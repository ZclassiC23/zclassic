/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Pure Kademlia DHT routing core for the ZCODE metaverse overlay. */

#include "vcs/zcode_dht.h"

#include "base/serialize_le.h"
#include "crypto/sha3.h"

#include <string.h>

static const uint8_t contacts_magic[8] =
    {'Z','C','D','H','T','C',0x0D,0x0A};

static bool dht_nonzero(const uint8_t id[32])
{
    uint8_t any = 0;
    if (!id) return false;
    for (size_t i = 0; i < 32; i++) any |= id[i];
    return any != 0;
}

const char *vcs_zcode_dht_error_string(enum vcs_zcode_dht_error error)
{
    switch (error) {
    case VCS_ZCODE_DHT_OK: return "ok";
    case VCS_ZCODE_DHT_ERR_NULL: return "null-argument";
    case VCS_ZCODE_DHT_ERR_VERSION: return "schema-version";
    case VCS_ZCODE_DHT_ERR_LIMIT: return "limit-invalid";
    case VCS_ZCODE_DHT_ERR_ID_ZERO: return "id-zero";
    case VCS_ZCODE_DHT_ERR_LAST_SEEN: return "last-seen-negative";
    case VCS_ZCODE_DHT_ERR_WIRE_SIZE: return "wire-size";
    case VCS_ZCODE_DHT_ERR_WIRE_MAGIC: return "wire-magic";
    case VCS_ZCODE_DHT_ERR_WIRE_ORDER: return "entry-order";
    }
    return "unknown";
}

const char *vcs_zcode_dht_add_result_string(
    enum vcs_zcode_dht_add_result result)
{
    switch (result) {
    case VCS_ZCODE_DHT_ADD_ADDED: return "added";
    case VCS_ZCODE_DHT_ADD_REFRESHED: return "refreshed";
    case VCS_ZCODE_DHT_ADD_EVICTED_TO_ADD: return "evicted-to-add";
    case VCS_ZCODE_DHT_ADD_REJECTED_SELF: return "rejected-self";
    case VCS_ZCODE_DHT_ADD_REJECTED_ZERO_ID: return "rejected-zero-id";
    }
    return "unknown";
}

bool vcs_zcode_dht_node_id(uint8_t out[32],
                           const uint8_t network_genesis_root[32],
                           const uint8_t zid_root[32],
                           const uint8_t delayed_block_hash[32])
{
    if (!out) return false;
    memset(out, 0, 32);
    if (!dht_nonzero(network_genesis_root) || !dht_nonzero(zid_root) ||
        !dht_nonzero(delayed_block_hash))
        return false;
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)VCS_ZCODE_DHT_NODE_ID_DOMAIN,
                   sizeof(VCS_ZCODE_DHT_NODE_ID_DOMAIN));
    sha3_256_write(&sha, network_genesis_root, 32);
    sha3_256_write(&sha, zid_root, 32);
    sha3_256_write(&sha, delayed_block_hash, 32);
    sha3_256_finalize(&sha, out);
    return true;
}

void vcs_zcode_dht_xor_distance(const uint8_t a[32], const uint8_t b[32],
                                uint8_t out[32])
{
    if (!a || !b || !out) return;
    for (size_t i = 0; i < 32; i++) out[i] = a[i] ^ b[i];
}

int vcs_zcode_dht_bucket_index(const uint8_t distance[32])
{
    if (!distance) return -1;
    for (size_t i = 0; i < 32; i++) {
        if (distance[i] == 0) continue;
        for (int bit = 7; bit >= 0; bit--) {
            if (distance[i] & (uint8_t)(1u << bit))
                return 255 - (int)(8 * i + (size_t)(7 - bit));
        }
    }
    return -1;
}

bool vcs_zcode_dht_table_init(struct vcs_zcode_dht_table *table,
                              const uint8_t self_id[32])
{
    if (!table) return false;
    memset(table, 0, sizeof(*table));
    if (!dht_nonzero(self_id)) return false;
    memcpy(table->self_id, self_id, 32);
    return true;
}

/* Locate a contact by node_id; returns the bucket index or -1. */
static int dht_table_slot(const struct vcs_zcode_dht_table *table,
                          const uint8_t node_id[32], size_t *slot_out)
{
    uint8_t distance[32];
    vcs_zcode_dht_xor_distance(table->self_id, node_id, distance);
    int bucket = vcs_zcode_dht_bucket_index(distance);
    if (bucket < 0) return -1;
    for (size_t i = 0; i < table->bucket_sizes[bucket]; i++) {
        if (memcmp(table->buckets[bucket][i].node_id, node_id, 32) == 0) {
            *slot_out = i;
            return bucket;
        }
    }
    return -1;
}

/* Index of the eviction victim in a non-empty bucket: the least-recently-seen
 * contact, ties broken by smaller node_id bytes. */
static size_t dht_bucket_lru(const struct vcs_zcode_dht_table *table,
                             size_t bucket)
{
    size_t victim = 0;
    for (size_t i = 1; i < table->bucket_sizes[bucket]; i++) {
        const struct vcs_zcode_dht_contact *candidate =
            &table->buckets[bucket][i];
        const struct vcs_zcode_dht_contact *current =
            &table->buckets[bucket][victim];
        if (candidate->last_seen_unix < current->last_seen_unix ||
            (candidate->last_seen_unix == current->last_seen_unix &&
             memcmp(candidate->node_id, current->node_id, 32) < 0))
            victim = i;
    }
    return victim;
}

/* Locate the table-wide eviction victim (used only when the table is at the
 * contact cap but the target bucket has no candidate of its own): the
 * least-recently-seen contact anywhere, ties broken by smaller node_id
 * bytes. Caller guarantees contact_count > 0. */
static void dht_table_lru(const struct vcs_zcode_dht_table *table,
                          size_t *bucket_out, size_t *slot_out)
{
    size_t best_bucket = 0;
    size_t best_slot = 0;
    bool have = false;
    for (size_t bucket = 0; bucket < VCS_ZCODE_DHT_BUCKET_COUNT; bucket++) {
        for (size_t slot = 0; slot < table->bucket_sizes[bucket]; slot++) {
            const struct vcs_zcode_dht_contact *candidate =
                &table->buckets[bucket][slot];
            const struct vcs_zcode_dht_contact *current =
                &table->buckets[best_bucket][best_slot];
            if (!have ||
                candidate->last_seen_unix < current->last_seen_unix ||
                (candidate->last_seen_unix == current->last_seen_unix &&
                 memcmp(candidate->node_id, current->node_id, 32) < 0)) {
                best_bucket = bucket;
                best_slot = slot;
                have = true;
            }
        }
    }
    *bucket_out = best_bucket;
    *slot_out = best_slot;
}

static void dht_bucket_remove_at(struct vcs_zcode_dht_table *table,
                                 size_t bucket, size_t slot)
{
    size_t size = table->bucket_sizes[bucket];
    if (slot + 1 < size) {
        memmove(&table->buckets[bucket][slot],
                &table->buckets[bucket][slot + 1],
                (size - slot - 1) * sizeof(struct vcs_zcode_dht_contact));
    }
    table->bucket_sizes[bucket]--;
    table->contact_count--;
}

enum vcs_zcode_dht_add_result vcs_zcode_dht_table_add_contact(
    struct vcs_zcode_dht_table *table,
    const struct vcs_zcode_dht_contact *contact)
{
    if (!table || !contact) return VCS_ZCODE_DHT_ADD_REJECTED_ZERO_ID;
    if (!dht_nonzero(contact->node_id))
        return VCS_ZCODE_DHT_ADD_REJECTED_ZERO_ID;
    if (memcmp(contact->node_id, table->self_id, 32) == 0)
        return VCS_ZCODE_DHT_ADD_REJECTED_SELF;

    size_t slot = 0;
    int bucket = dht_table_slot(table, contact->node_id, &slot);
    if (bucket >= 0) {
        struct vcs_zcode_dht_contact *existing =
            &table->buckets[bucket][slot];
        memcpy(existing->zid_root, contact->zid_root, 32);
        existing->last_seen_unix = contact->last_seen_unix;
        existing->consecutive_failures = 0;
        return VCS_ZCODE_DHT_ADD_REFRESHED;
    }

    uint8_t distance[32];
    vcs_zcode_dht_xor_distance(table->self_id, contact->node_id, distance);
    bucket = vcs_zcode_dht_bucket_index(distance);
    bool evicted = false;
    if (table->bucket_sizes[bucket] == VCS_ZCODE_DHT_K) {
        dht_bucket_remove_at(table, (size_t)bucket,
                             dht_bucket_lru(table, (size_t)bucket));
        evicted = true;
    } else if (table->contact_count == VCS_ZCODE_DHT_MAX_CONTACTS) {
        /* Cap reached with room left in this bucket: drop the table-wide
         * LRU contact so the cap stays absolute. */
        size_t victim_bucket = 0;
        size_t victim_slot = 0;
        dht_table_lru(table, &victim_bucket, &victim_slot);
        dht_bucket_remove_at(table, victim_bucket, victim_slot);
        evicted = true;
    }
    size_t size = table->bucket_sizes[bucket];
    table->buckets[bucket][size] = *contact;
    table->bucket_sizes[bucket]++;
    table->contact_count++;
    return evicted ? VCS_ZCODE_DHT_ADD_EVICTED_TO_ADD
                   : VCS_ZCODE_DHT_ADD_ADDED;
}

bool vcs_zcode_dht_table_touch(struct vcs_zcode_dht_table *table,
                               const uint8_t node_id[32],
                               int64_t last_seen_unix)
{
    if (!table || !node_id) return false;
    size_t slot = 0;
    int bucket = dht_table_slot(table, node_id, &slot);
    if (bucket < 0) return false;
    table->buckets[bucket][slot].last_seen_unix = last_seen_unix;
    table->buckets[bucket][slot].consecutive_failures = 0;
    return true;
}

bool vcs_zcode_dht_table_note_failure(struct vcs_zcode_dht_table *table,
                                      const uint8_t node_id[32])
{
    if (!table || !node_id) return false;
    size_t slot = 0;
    int bucket = dht_table_slot(table, node_id, &slot);
    if (bucket < 0) return false;
    table->buckets[bucket][slot].consecutive_failures++;
    return true;
}

bool vcs_zcode_dht_table_remove(struct vcs_zcode_dht_table *table,
                                const uint8_t node_id[32])
{
    if (!table || !node_id) return false;
    size_t slot = 0;
    int bucket = dht_table_slot(table, node_id, &slot);
    if (bucket < 0) return false;
    dht_bucket_remove_at(table, (size_t)bucket, slot);
    return true;
}

bool vcs_zcode_dht_table_find(const struct vcs_zcode_dht_table *table,
                              const uint8_t node_id[32],
                              struct vcs_zcode_dht_contact *out)
{
    if (!table || !node_id || !out) return false;
    memset(out, 0, sizeof(*out));
    size_t slot = 0;
    int bucket = dht_table_slot(table, node_id, &slot);
    if (bucket < 0) return false;
    *out = table->buckets[bucket][slot];
    return true;
}

/* Candidate ordering for closest(): ascending XOR distance to the target,
 * node_id ascending as the tie-break. Returns true when the contact keyed by
 * candidate_distance/candidate_id sorts strictly before the incumbent. */
static bool dht_closer(const uint8_t candidate_distance[32],
                       const uint8_t candidate_id[32],
                       const uint8_t incumbent_distance[32],
                       const uint8_t incumbent_id[32])
{
    int cmp = memcmp(candidate_distance, incumbent_distance, 32);
    if (cmp != 0) return cmp < 0;
    return memcmp(candidate_id, incumbent_id, 32) < 0;
}

size_t vcs_zcode_dht_table_closest(
    const struct vcs_zcode_dht_table *table,
    const uint8_t target_id[32],
    struct vcs_zcode_dht_contact *out_contacts, size_t max)
{
    if (!table || !target_id || !out_contacts || max == 0) return 0;
    /* Distances for the current out set, kept parallel to out_contacts. */
    uint8_t distances[VCS_ZCODE_DHT_MAX_CONTACTS][32];
    if (max > VCS_ZCODE_DHT_MAX_CONTACTS) max = VCS_ZCODE_DHT_MAX_CONTACTS;
    size_t count = 0;
    for (size_t bucket = 0; bucket < VCS_ZCODE_DHT_BUCKET_COUNT; bucket++) {
        for (size_t slot = 0; slot < table->bucket_sizes[bucket]; slot++) {
            const struct vcs_zcode_dht_contact *contact =
                &table->buckets[bucket][slot];
            uint8_t distance[32];
            vcs_zcode_dht_xor_distance(contact->node_id, target_id,
                                       distance);
            if (count == max &&
                !dht_closer(distance, contact->node_id,
                            distances[count - 1],
                            out_contacts[count - 1].node_id))
                continue;
            size_t at = count;
            while (at > 0 &&
                   dht_closer(distance, contact->node_id,
                              distances[at - 1],
                              out_contacts[at - 1].node_id))
                at--;
            if (count < max) count++;
            memmove(&out_contacts[at + 1], &out_contacts[at],
                    (count - at - 1) * sizeof(*out_contacts));
            memmove(&distances[at + 1][0], &distances[at][0],
                    (count - at - 1) * 32);
            out_contacts[at] = *contact;
            memcpy(distances[at], distance, 32);
        }
    }
    return count;
}

uint32_t vcs_zcode_dht_table_count(const struct vcs_zcode_dht_table *table)
{
    return table ? table->contact_count : 0;
}

size_t vcs_zcode_dht_contacts_wire_bytes(uint32_t count)
{
    if (count > VCS_ZCODE_DHT_MAX_CONTACTS) return 0;
    return VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES +
           (size_t)count * VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES;
}

static void dht_contact_write(uint8_t *wire,
                              const struct vcs_zcode_dht_contact *contact)
{
    memcpy(wire, contact->node_id, 32);
    memcpy(wire + 32, contact->zid_root, 32);
    zcl_write_i64_le(wire + 64, contact->last_seen_unix);
    zcl_write_u32_le(wire + 72, contact->consecutive_failures);
}

enum vcs_zcode_dht_error vcs_zcode_dht_contacts_serialize(
    const struct vcs_zcode_dht_contact *contacts, uint32_t count,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len_out)
{
    if (!wire_len_out) return VCS_ZCODE_DHT_ERR_NULL;
    *wire_len_out = 0;
    if (!contacts || !wire) return VCS_ZCODE_DHT_ERR_NULL;
    if (count == 0 || count > VCS_ZCODE_DHT_MAX_CONTACTS)
        return VCS_ZCODE_DHT_ERR_LIMIT;
    for (uint32_t i = 0; i < count; i++) {
        if (!dht_nonzero(contacts[i].node_id))
            return VCS_ZCODE_DHT_ERR_ID_ZERO;
    }
    size_t need = vcs_zcode_dht_contacts_wire_bytes(count);
    if (wire_capacity < need) return VCS_ZCODE_DHT_ERR_WIRE_SIZE;

    size_t off = 0;
    memcpy(wire + off, contacts_magic, sizeof(contacts_magic));
    off += sizeof(contacts_magic);
    zcl_write_u16_le(wire + off, VCS_ZCODE_DHT_CONTACTS_WIRE_VERSION);
    off += 2;
    zcl_write_u32_le(wire + off, count);
    off += 4;

    /* Canonical emission: repeatedly select the smallest node_id strictly
     * greater than the previously emitted one. Finding no candidate before
     * all contacts are emitted means the input held a duplicate id. */
    const uint8_t *previous = NULL;
    for (uint32_t emitted = 0; emitted < count; emitted++) {
        uint32_t best = count;
        for (uint32_t i = 0; i < count; i++) {
            if (previous &&
                memcmp(contacts[i].node_id, previous, 32) <= 0)
                continue;
            if (best == count ||
                memcmp(contacts[i].node_id, contacts[best].node_id, 32) < 0)
                best = i;
        }
        if (best == count) return VCS_ZCODE_DHT_ERR_WIRE_ORDER;
        dht_contact_write(wire + off, &contacts[best]);
        off += VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES;
        previous = contacts[best].node_id;
    }
    if (off != need) return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    *wire_len_out = need;
    return VCS_ZCODE_DHT_OK;
}

enum vcs_zcode_dht_error vcs_zcode_dht_contacts_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_dht_contact *contacts_out, uint32_t contact_capacity,
    uint32_t *count_out)
{
    if (!wire || !contacts_out || !count_out) return VCS_ZCODE_DHT_ERR_NULL;
    *count_out = 0;
    if (wire_len < VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES)
        return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    if (memcmp(wire, contacts_magic, sizeof(contacts_magic)) != 0)
        return VCS_ZCODE_DHT_ERR_WIRE_MAGIC;
    size_t off = sizeof(contacts_magic);
    uint16_t version = zcl_read_u16_le(wire + off);
    off += 2;
    if (version != VCS_ZCODE_DHT_CONTACTS_WIRE_VERSION)
        return VCS_ZCODE_DHT_ERR_VERSION;
    uint32_t count = zcl_read_u32_le(wire + off);
    off += 4;
    if (count == 0 || count > VCS_ZCODE_DHT_MAX_CONTACTS ||
        count > contact_capacity)
        return VCS_ZCODE_DHT_ERR_LIMIT;
    if (wire_len != vcs_zcode_dht_contacts_wire_bytes(count))
        return VCS_ZCODE_DHT_ERR_WIRE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        struct vcs_zcode_dht_contact *contact = &contacts_out[i];
        memcpy(contact->node_id, wire + off, 32);
        memcpy(contact->zid_root, wire + off + 32, 32);
        contact->last_seen_unix = zcl_read_i64_le(wire + off + 64);
        contact->consecutive_failures = zcl_read_u32_le(wire + off + 72);
        off += VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES;
        if (!dht_nonzero(contact->node_id))
            return VCS_ZCODE_DHT_ERR_ID_ZERO;
        if (contact->last_seen_unix < 0)
            return VCS_ZCODE_DHT_ERR_LAST_SEEN;
    }
    *count_out = count;
    return VCS_ZCODE_DHT_OK;
}
