/* Copyright (c) 2012 Pieter Wuille
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_ADDRMAN_H
#define ZCL_ADDRMAN_H

#include "net/netaddr.h"
#include "core/uint256.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stdint.h>

#define ADDRMAN_TRIED_BUCKET_COUNT 256
#define ADDRMAN_NEW_BUCKET_COUNT 1024
#define ADDRMAN_BUCKET_SIZE 64
#define ADDRMAN_TRIED_BUCKETS_PER_GROUP 8
#define ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP 64
#define ADDRMAN_NEW_BUCKETS_PER_ADDRESS 8
#define ADDRMAN_HORIZON_DAYS 30
#define ADDRMAN_RETRIES 3
#define ADDRMAN_MAX_FAILURES 10
#define ADDRMAN_MIN_FAIL_DAYS 7
#define ADDRMAN_GETADDR_MAX_PCT 23
#define ADDRMAN_GETADDR_MAX 2500

#define ADDRMAN_MAX_ENTRIES (ADDRMAN_NEW_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE)

struct addr_info {
    struct net_address addr;
    struct net_addr source;
    int64_t last_success;
    int64_t last_try;
    int attempts;
    int ref_count;
    bool in_tried;
    int random_pos;
    bool used;
};

/* O(1) address→entry-id index slot. Defined privately in addrman.c;
 * struct addr_man holds only a pointer, so a forward declaration is
 * enough here. See "address index" in addrman.c. */
struct addr_index_slot;

struct addr_man {
    zcl_mutex_t cs;
    struct uint256 nKey;

    struct addr_info *entries;
    size_t entries_cap;
    int id_count;

    /* In-memory O(1) net_addr→id index (open addressing, tombstoned).
     * NOT serialized — rebuilt from `entries` on load. Keeps addrman_add's
     * per-address dedup off the old O(id_count) linear scan. */
    struct addr_index_slot *idx;
    size_t idx_slots;   /* capacity, power of 2 */
    size_t idx_live;    /* live slots == number of used entries */
    size_t idx_tombs;   /* tombstoned slots */

    int *random_order;
    size_t random_size;
    size_t random_cap;

    int tried_count;
    int vvTried[ADDRMAN_TRIED_BUCKET_COUNT][ADDRMAN_BUCKET_SIZE];

    int new_count;
    int vvNew[ADDRMAN_NEW_BUCKET_COUNT][ADDRMAN_BUCKET_SIZE];
};

void addrman_init(struct addr_man *am);
void addrman_free(struct addr_man *am);
void addrman_clear(struct addr_man *am);
size_t addrman_size(const struct addr_man *am);

bool addrman_add(struct addr_man *am, const struct net_address *addr,
                 const struct net_addr *source, int64_t time_penalty);

void addrman_good(struct addr_man *am, const struct net_service *addr,
                  int64_t nTime);

void addrman_attempt(struct addr_man *am, const struct net_service *addr,
                     int64_t nTime);

bool addrman_select(struct addr_man *am, bool new_only,
                    struct addr_info *result);

void addrman_connected(struct addr_man *am, const struct net_service *addr,
                       int64_t nTime);

size_t addrman_get_addr(struct addr_man *am, struct net_address *out,
                        size_t max_out);

int addr_info_get_tried_bucket(const struct addr_info *info,
                               const struct uint256 *nKey);

int addr_info_get_new_bucket(const struct addr_info *info,
                             const struct uint256 *nKey,
                             const struct net_addr *src);

int addr_info_get_bucket_position(const struct addr_info *info,
                                  const struct uint256 *nKey,
                                  bool fNew, int nBucket);

bool addr_info_is_terrible(const struct addr_info *info, int64_t nNow);
double addr_info_get_chance(const struct addr_info *info, int64_t nNow);

/* Verify internal consistency of bucket tables.
 * Returns 0 on success, negative on error.
 * err_buf (if non-NULL) receives a description of the first error found. */
int addrman_consistency_check(const struct addr_man *am,
                              char *err_buf, size_t err_cap);

/* Verify the in-memory address index agrees with a brute-force scan of
 * `entries` (find-by-index == find-by-scan for every used entry, live-slot
 * count matches, every live slot points at a matching used entry).
 * Returns 0 on success, -1 on the first discrepancy (described in err_buf).
 * A NULL index (OOM fallback to linear scan) verifies trivially. */
int addrman_index_verify(const struct addr_man *am,
                         char *err_buf, size_t err_cap);

/* Bucket distribution stats for monitoring/debugging. */
struct addrman_bucket_stats {
    int new_occupied;           /* total occupied slots in new table */
    int tried_occupied;         /* total occupied slots in tried table */
    int new_buckets_nonempty;   /* buckets with >= 1 entry */
    int tried_buckets_nonempty;
    int max_new_bucket_fill;    /* most-full new bucket */
    int max_tried_bucket_fill;  /* most-full tried bucket */
};

void addrman_get_bucket_stats(const struct addr_man *am,
                              struct addrman_bucket_stats *stats);

struct byte_stream;
bool addrman_serialize(const struct addr_man *am, struct byte_stream *s);
bool addrman_deserialize(struct addr_man *am, struct byte_stream *s);

#endif
