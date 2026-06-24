/* Copyright (c) 2012 Pieter Wuille
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/addrman.h"
#include "core/hash.h"
#include "core/random.h"
#include "core/serialize.h"
#include "util/timedata.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "util/safe_alloc.h"
#include "util/log_macros.h"

int addr_info_get_tried_bucket(const struct addr_info *info,
                               const struct uint256 *nKey)
{
    unsigned char key[NET_SERVICE_KEY_SIZE];
    net_service_get_key(&info->addr.svc, key);

    unsigned char buf[32 + NET_SERVICE_KEY_SIZE];
    memcpy(buf, nKey->data, 32);
    memcpy(buf + 32, key, NET_SERVICE_KEY_SIZE);
    struct uint256 hash1;
    hash256(buf, sizeof(buf), hash1.data);
    uint64_t h1;
    memcpy(&h1, hash1.data, sizeof(h1));

    unsigned char group[NET_ADDR_GROUP_MAX];
    size_t glen = net_addr_get_group(&info->addr.svc.addr, group, sizeof(group));

    unsigned char buf2[32 + NET_ADDR_GROUP_MAX + 8];
    memcpy(buf2, nKey->data, 32);
    memcpy(buf2 + 32, group, glen);
    uint64_t bucket_seed = h1 % ADDRMAN_TRIED_BUCKETS_PER_GROUP;
    memcpy(buf2 + 32 + glen, &bucket_seed, sizeof(bucket_seed));
    struct uint256 hash2;
    hash256(buf2, 32 + glen + sizeof(bucket_seed), hash2.data);
    uint64_t h2;
    memcpy(&h2, hash2.data, sizeof(h2));

    return (int)(h2 % ADDRMAN_TRIED_BUCKET_COUNT);
}

int addr_info_get_new_bucket(const struct addr_info *info,
                             const struct uint256 *nKey,
                             const struct net_addr *src)
{
    unsigned char src_group[NET_ADDR_GROUP_MAX];
    size_t sglen = net_addr_get_group(src, src_group, sizeof(src_group));

    unsigned char my_group[NET_ADDR_GROUP_MAX];
    size_t mglen = net_addr_get_group(&info->addr.svc.addr, my_group,
                                       sizeof(my_group));

    unsigned char buf1[32 + NET_ADDR_GROUP_MAX + NET_ADDR_GROUP_MAX];
    memcpy(buf1, nKey->data, 32);
    memcpy(buf1 + 32, my_group, mglen);
    memcpy(buf1 + 32 + mglen, src_group, sglen);
    struct uint256 hash1;
    hash256(buf1, 32 + mglen + sglen, hash1.data);
    uint64_t h1;
    memcpy(&h1, hash1.data, sizeof(h1));

    unsigned char buf2[32 + NET_ADDR_GROUP_MAX + 8];
    memcpy(buf2, nKey->data, 32);
    memcpy(buf2 + 32, src_group, sglen);
    uint64_t bucket_seed = h1 % ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP;
    memcpy(buf2 + 32 + sglen, &bucket_seed, sizeof(bucket_seed));
    struct uint256 hash2;
    hash256(buf2, 32 + sglen + sizeof(bucket_seed), hash2.data);
    uint64_t h2;
    memcpy(&h2, hash2.data, sizeof(h2));

    return (int)(h2 % ADDRMAN_NEW_BUCKET_COUNT);
}

int addr_info_get_bucket_position(const struct addr_info *info,
                                  const struct uint256 *nKey,
                                  bool fNew, int nBucket)
{
    unsigned char key[NET_SERVICE_KEY_SIZE];
    net_service_get_key(&info->addr.svc, key);

    unsigned char buf[32 + 1 + 4 + NET_SERVICE_KEY_SIZE];
    memcpy(buf, nKey->data, 32);
    buf[32] = fNew ? 'N' : 'K';
    memcpy(buf + 33, &nBucket, 4);
    memcpy(buf + 37, key, NET_SERVICE_KEY_SIZE);
    struct uint256 h;
    hash256(buf, sizeof(buf), h.data);
    uint64_t r;
    memcpy(&r, h.data, sizeof(r));
    return (int)(r % ADDRMAN_BUCKET_SIZE);
}

bool addr_info_is_terrible(const struct addr_info *info, int64_t nNow)
{
    if (info->last_try && info->last_try >= nNow - 60)
        return false;
    if ((int64_t)info->addr.nTime > nNow + 10 * 60)
        return true;
    if (info->addr.nTime == 0 ||
        nNow - (int64_t)info->addr.nTime > ADDRMAN_HORIZON_DAYS * 24 * 60 * 60)
        return true;
    if (info->last_success == 0 && info->attempts >= ADDRMAN_RETRIES)
        return true;
    if (nNow - info->last_success > ADDRMAN_MIN_FAIL_DAYS * 24 * 60 * 60 &&
        info->attempts >= ADDRMAN_MAX_FAILURES)
        return true;
    return false;
}

double addr_info_get_chance(const struct addr_info *info, int64_t nNow)
{
    double fChance = 1.0;
    int64_t nSinceLastTry = nNow - info->last_try;
    if (nSinceLastTry < 0) nSinceLastTry = 0;
    if (nSinceLastTry < 60 * 10)
        fChance *= 0.01;
    int n = info->attempts < 8 ? info->attempts : 8;
    fChance *= pow(0.66, n);
    return fChance;
}

static bool addrman_find_occupied_slot(const int *table,
                                       int bucket_count,
                                       int start_bucket,
                                       int start_pos,
                                       int *bucket_out,
                                       int *pos_out)
{
    if (!table || bucket_count <= 0 || !bucket_out || !pos_out)
        return false;

    for (int bucket_offset = 0; bucket_offset < bucket_count; bucket_offset++) {
        int bucket = (start_bucket + bucket_offset) % bucket_count;
        for (int pos_offset = 0; pos_offset < ADDRMAN_BUCKET_SIZE; pos_offset++) {
            int pos = (start_pos + pos_offset) % ADDRMAN_BUCKET_SIZE;
            if (table[bucket * ADDRMAN_BUCKET_SIZE + pos] != -1) {
                *bucket_out = bucket;
                *pos_out = pos;
                return true;
            }
        }
    }

    return false;
}

void addrman_init(struct addr_man *am)
{
    zcl_mutex_init(&am->cs);
    GetRandBytes(am->nKey.data, 32);
    am->id_count = 0;
    am->tried_count = 0;
    am->new_count = 0;
    am->random_order = NULL;
    am->random_size = 0;
    am->random_cap = 0;
    am->entries_cap = 4096;
    am->entries = zcl_calloc(am->entries_cap, sizeof(struct addr_info), "addr_entries");
    for (int i = 0; i < ADDRMAN_NEW_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvNew[i][j] = -1;
    for (int i = 0; i < ADDRMAN_TRIED_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvTried[i][j] = -1;
}

void addrman_free(struct addr_man *am)
{
    free(am->random_order);
    free(am->entries);
    am->random_order = NULL;
    am->entries = NULL;
    zcl_mutex_destroy(&am->cs);
}

void addrman_clear(struct addr_man *am)
{
    GetRandBytes(am->nKey.data, 32);
    am->id_count = 0;
    am->tried_count = 0;
    am->new_count = 0;
    free(am->random_order);
    am->random_order = NULL;
    am->random_size = 0;
    am->random_cap = 0;
    if (am->entries)
        memset(am->entries, 0, am->entries_cap * sizeof(struct addr_info));
    for (int i = 0; i < ADDRMAN_NEW_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvNew[i][j] = -1;
    for (int i = 0; i < ADDRMAN_TRIED_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvTried[i][j] = -1;
}

size_t addrman_size(const struct addr_man *am)
{
    return am->random_size;
}

static void random_push(struct addr_man *am, int id)
{
    if (am->random_size >= am->random_cap) {
        size_t new_cap = am->random_cap ? am->random_cap * 2 : 256;
        int *p = zcl_realloc(am->random_order, new_cap * sizeof(int), "addr_random_order");
        if (!p) return;
        am->random_order = p;
        am->random_cap = new_cap;
    }
    am->random_order[am->random_size++] = id;
}

static void swap_random(struct addr_man *am, unsigned int p1, unsigned int p2)
{
    if (p1 == p2) return;
    if (p1 >= am->random_size || p2 >= am->random_size) return;
    int id1 = am->random_order[p1];
    int id2 = am->random_order[p2];
    if (id1 < 0 || (size_t)id1 >= am->entries_cap) return;
    if (id2 < 0 || (size_t)id2 >= am->entries_cap) return;
    am->entries[id1].random_pos = (int)p2;
    am->entries[id2].random_pos = (int)p1;
    am->random_order[p1] = id2;
    am->random_order[p2] = id1;
}

static struct addr_info *find_addr(struct addr_man *am,
                                    const struct net_addr *addr, int *pnId)
{
    if (!am || !addr || !am->entries)
        LOG_NULL("addrman", "find_addr: bad args");

    for (int i = 0; i < am->id_count; i++) {
        if (am->entries[i].used &&
            net_addr_eq(&am->entries[i].addr.svc.addr, addr)) {
            if (pnId) *pnId = i;
            return &am->entries[i];
        }
    }
    return NULL;
}

static struct addr_info *create_entry(struct addr_man *am,
                                       const struct net_address *addr,
                                       const struct net_addr *source,
                                       int *pnId)
{
    int id = am->id_count;
    if ((size_t)id >= am->entries_cap) {
        size_t new_cap = am->entries_cap * 2;
        if (new_cap > ADDRMAN_MAX_ENTRIES) new_cap = ADDRMAN_MAX_ENTRIES;
        if ((size_t)id >= new_cap) LOG_NULL("addrman", "entry id=%d exceeds max capacity=%zu", id, new_cap);
        struct addr_info *p = zcl_realloc(am->entries,
                                       new_cap * sizeof(struct addr_info), "addr_entries");
        if (!p) LOG_NULL("addrman", "realloc failed for entries new_cap=%zu", new_cap);
        memset(p + am->entries_cap, 0,
               (new_cap - am->entries_cap) * sizeof(struct addr_info));
        am->entries = p;
        am->entries_cap = new_cap;
    }
    am->id_count++;

    struct addr_info *info = &am->entries[id];
    memset(info, 0, sizeof(*info));
    info->addr = *addr;
    info->source = *source;
    info->last_success = 0;
    info->last_try = 0;
    info->attempts = 0;
    info->ref_count = 0;
    info->in_tried = false;
    info->random_pos = (int)am->random_size;
    info->used = true;
    random_push(am, id);

    if (pnId) *pnId = id;
    return info;
}

static void delete_entry(struct addr_man *am, int nId)
{
    struct addr_info *info = &am->entries[nId];
    swap_random(am, (unsigned int)info->random_pos,
                (unsigned int)(am->random_size - 1));
    am->random_size--;
    info->used = false;
    am->new_count--;
}

static void clear_new(struct addr_man *am, int nUBucket, int nUBucketPos)
{
    if (am->vvNew[nUBucket][nUBucketPos] != -1) {
        int nIdDelete = am->vvNew[nUBucket][nUBucketPos];
        if (nIdDelete < 0 || (size_t)nIdDelete >= am->entries_cap) {
            am->vvNew[nUBucket][nUBucketPos] = -1;
            return;
        }
        struct addr_info *info = &am->entries[nIdDelete];
        info->ref_count--;
        am->vvNew[nUBucket][nUBucketPos] = -1;
        if (info->ref_count == 0)
            delete_entry(am, nIdDelete);
    }
}

static void make_tried(struct addr_man *am, struct addr_info *info, int nId)
{
    for (int bucket = 0; bucket < ADDRMAN_NEW_BUCKET_COUNT; bucket++) {
        int pos = addr_info_get_bucket_position(info, &am->nKey, true, bucket);
        if (am->vvNew[bucket][pos] == nId) {
            am->vvNew[bucket][pos] = -1;
            info->ref_count--;
        }
    }
    am->new_count--;

    int nKBucket = addr_info_get_tried_bucket(info, &am->nKey);
    int nKBucketPos = addr_info_get_bucket_position(info, &am->nKey, false,
                                                     nKBucket);

    if (am->vvTried[nKBucket][nKBucketPos] != -1) {
        int nIdEvict = am->vvTried[nKBucket][nKBucketPos];
        if (nIdEvict < 0 || (size_t)nIdEvict >= am->entries_cap) {
            am->vvTried[nKBucket][nKBucketPos] = -1;
        } else {
            struct addr_info *old = &am->entries[nIdEvict];
            old->in_tried = false;
            am->vvTried[nKBucket][nKBucketPos] = -1;
            am->tried_count--;

            int nUBucket = addr_info_get_new_bucket(old, &am->nKey, &old->source);
            int nUBucketPos = addr_info_get_bucket_position(old, &am->nKey, true,
                                                             nUBucket);
            /* Eclipse attack mitigation: only evict the occupant of the
             * new bucket slot if it's "terrible" (stale/failed) or if
             * the evicted tried entry has fewer references. This prevents
             * an attacker from cascading evictions through tried→new to
             * purge legitimate addresses from the new table. */
            int existing_new_id = am->vvNew[nUBucket][nUBucketPos];
            if (existing_new_id != -1 &&
                existing_new_id >= 0 &&
                (size_t)existing_new_id < am->entries_cap) {
                struct addr_info *occupant = &am->entries[existing_new_id];
                if (occupant->used &&
                    !addr_info_is_terrible(occupant, GetAdjustedTime()) &&
                    occupant->ref_count <= 1) {
                    /* Occupant is still good — don't evict. Place the
                     * tried-evicted entry in a different new bucket. */
                    bool placed = false;
                    for (int attempt = 1; attempt < 8 && !placed; attempt++) {
                        unsigned char salt[4];
                        memcpy(salt, &attempt, 4);
                        /* Try alternative buckets derived from source */
                        int alt_bucket = (nUBucket + attempt * 97) %
                                         ADDRMAN_NEW_BUCKET_COUNT;
                        int alt_pos = addr_info_get_bucket_position(
                            old, &am->nKey, true, alt_bucket);
                        if (am->vvNew[alt_bucket][alt_pos] == -1) {
                            old->ref_count = 1;
                            am->vvNew[alt_bucket][alt_pos] = nIdEvict;
                            am->new_count++;
                            placed = true;
                        }
                    }
                    if (!placed) {
                        /* Last resort: evict the occupant anyway */
                        clear_new(am, nUBucket, nUBucketPos);
                        old->ref_count = 1;
                        am->vvNew[nUBucket][nUBucketPos] = nIdEvict;
                        am->new_count++;
                    }
                    goto place_tried;
                }
            }
            clear_new(am, nUBucket, nUBucketPos);
            old->ref_count = 1;
            am->vvNew[nUBucket][nUBucketPos] = nIdEvict;
            am->new_count++;
        }
    }

place_tried:
    am->vvTried[nKBucket][nKBucketPos] = nId;
    am->tried_count++;
    info->in_tried = true;
}

bool addrman_add(struct addr_man *am, const struct net_address *addr,
                 const struct net_addr *source, int64_t time_penalty)
{
    if (!net_addr_is_routable(&addr->svc.addr))
        return false;

    zcl_mutex_lock(&am->cs);

    bool fNew = false;
    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->svc.addr, &nId);

    if (pinfo) {
        int64_t nUpdateInterval = 24 * 60 * 60;
        bool fCurrentlyOnline = (GetAdjustedTime() - (int64_t)addr->nTime < 24 * 60 * 60);
        if (fCurrentlyOnline)
            nUpdateInterval = 60 * 60;

        if (addr->nTime && (!pinfo->addr.nTime ||
            (int64_t)pinfo->addr.nTime < (int64_t)addr->nTime - nUpdateInterval - time_penalty)) {
            int64_t t = (int64_t)addr->nTime - time_penalty;
            pinfo->addr.nTime = (uint32_t)(t > 0 ? t : 0);
        }

        pinfo->addr.nServices |= addr->nServices;

        if (!addr->nTime || (pinfo->addr.nTime && addr->nTime <= pinfo->addr.nTime)) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }
        if (pinfo->in_tried) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }
        if (pinfo->ref_count == ADDRMAN_NEW_BUCKETS_PER_ADDRESS) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }

        int nFactor = 1;
        for (int n = 0; n < pinfo->ref_count; n++)
            nFactor *= 2;
        if (nFactor > 1 && (GetRandInt(nFactor) != 0)) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }
    } else {
        pinfo = create_entry(am, addr, source, &nId);
        if (!pinfo) {
            zcl_mutex_unlock(&am->cs);
            LOG_FAIL("addrman", "create_entry failed for new address");
        }
        int64_t t = (int64_t)pinfo->addr.nTime - time_penalty;
        pinfo->addr.nTime = (uint32_t)(t > 0 ? t : 0);
        am->new_count++;
        fNew = true;
    }

    bool inserted = false;
    int nUBucket = addr_info_get_new_bucket(pinfo, &am->nKey, source);
    for (int attempt = 0; attempt < ADDRMAN_NEW_BUCKETS_PER_ADDRESS; attempt++) {
        int bucket = (nUBucket + attempt * 97) % ADDRMAN_NEW_BUCKET_COUNT;
        int pos = addr_info_get_bucket_position(pinfo, &am->nKey, true, bucket);
        if (am->vvNew[bucket][pos] == nId) {
            inserted = true;
            break;
        }

        bool fInsert = am->vvNew[bucket][pos] == -1;
        if (!fInsert) {
            int eId = am->vvNew[bucket][pos];
            if (eId < 0 || (size_t)eId >= am->entries_cap) {
                am->vvNew[bucket][pos] = -1;
                fInsert = true;
            } else {
                struct addr_info *existing = &am->entries[eId];
                if (!existing->used ||
                    addr_info_is_terrible(existing, GetAdjustedTime()) ||
                    (existing->ref_count > 1 && pinfo->ref_count == 0))
                    fInsert = true;
            }
        }
        if (!fInsert)
            continue;

        clear_new(am, bucket, pos);
        pinfo->ref_count++;
        am->vvNew[bucket][pos] = nId;
        inserted = true;
        break;
    }
    if (!inserted && pinfo->ref_count == 0)
        delete_entry(am, nId);

    zcl_mutex_unlock(&am->cs);
    return fNew;
}

void addrman_good(struct addr_man *am, const struct net_service *addr,
                  int64_t nTime)
{
    zcl_mutex_lock(&am->cs);

    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->addr, &nId);
    if (!pinfo || !net_service_eq(&pinfo->addr.svc, addr)) {
        zcl_mutex_unlock(&am->cs);
        return;
    }

    pinfo->last_success = nTime;
    pinfo->last_try = nTime;
    pinfo->attempts = 0;

    if (pinfo->in_tried) {
        zcl_mutex_unlock(&am->cs);
        return;
    }

    int nRnd = GetRandInt(ADDRMAN_NEW_BUCKET_COUNT);
    int nUBucket = -1;
    for (int n = 0; n < ADDRMAN_NEW_BUCKET_COUNT; n++) {
        int nB = (n + nRnd) % ADDRMAN_NEW_BUCKET_COUNT;
        int nBpos = addr_info_get_bucket_position(pinfo, &am->nKey, true, nB);
        if (am->vvNew[nB][nBpos] == nId) {
            nUBucket = nB;
            break;
        }
    }

    if (nUBucket == -1) {
        zcl_mutex_unlock(&am->cs);
        return;
    }

    make_tried(am, pinfo, nId);
    zcl_mutex_unlock(&am->cs);
}

void addrman_attempt(struct addr_man *am, const struct net_service *addr,
                     int64_t nTime)
{
    zcl_mutex_lock(&am->cs);
    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->addr, &nId);
    if (!pinfo || !net_service_eq(&pinfo->addr.svc, addr)) {
        zcl_mutex_unlock(&am->cs);
        return;
    }
    pinfo->last_try = nTime;
    pinfo->attempts++;
    zcl_mutex_unlock(&am->cs);
}

bool addrman_select(struct addr_man *am, bool new_only,
                    struct addr_info *result)
{
    zcl_mutex_lock(&am->cs);

    if (am->random_size == 0) {
        zcl_mutex_unlock(&am->cs);
        LOG_FAIL("addrman", "select failed: address table is empty");
    }
    if (new_only && am->new_count == 0) {
        zcl_mutex_unlock(&am->cs);
        LOG_FAIL("addrman", "select failed: no new addresses available");
    }

    int64_t nNow = GetAdjustedTime();

    if (!new_only && am->tried_count > 0 &&
        (am->new_count == 0 || GetRandInt(2) == 0)) {
        double fChanceFactor = 1.0;
        for (int i = 0; i < 200000; i++) {
            int nKBucket = GetRandInt(ADDRMAN_TRIED_BUCKET_COUNT);
            int nKBucketPos = GetRandInt(ADDRMAN_BUCKET_SIZE);
            if (am->vvTried[nKBucket][nKBucketPos] == -1 &&
                !addrman_find_occupied_slot(&am->vvTried[0][0],
                                            ADDRMAN_TRIED_BUCKET_COUNT,
                                            nKBucket,
                                            nKBucketPos,
                                            &nKBucket,
                                            &nKBucketPos)) {
                zcl_mutex_unlock(&am->cs);
                LOG_FAIL("addrman", "select exhausted tried bucket search after full table scan");
                return false;
            }
            int nId = am->vvTried[nKBucket][nKBucketPos];
            if (nId < 0 || (size_t)nId >= am->entries_cap) {
                am->vvTried[nKBucket][nKBucketPos] = -1; /* repair */
                fChanceFactor *= 1.2;
                continue;
            }
            struct addr_info *info = &am->entries[nId];
            double chance = fChanceFactor * addr_info_get_chance(info, nNow);
            if (GetRandInt(1 << 30) < chance * (double)(1 << 30)) {
                *result = *info;
                zcl_mutex_unlock(&am->cs);
                return true;
            }
            fChanceFactor *= 1.2;
        }
    } else {
        double fChanceFactor = 1.0;
        for (int i = 0; i < 200000; i++) {
            int nUBucket = GetRandInt(ADDRMAN_NEW_BUCKET_COUNT);
            int nUBucketPos = GetRandInt(ADDRMAN_BUCKET_SIZE);
            if (am->vvNew[nUBucket][nUBucketPos] == -1 &&
                !addrman_find_occupied_slot(&am->vvNew[0][0],
                                            ADDRMAN_NEW_BUCKET_COUNT,
                                            nUBucket,
                                            nUBucketPos,
                                            &nUBucket,
                                            &nUBucketPos)) {
                zcl_mutex_unlock(&am->cs);
                LOG_FAIL("addrman", "select exhausted new bucket search after full table scan");
                return false;
            }
            int nId = am->vvNew[nUBucket][nUBucketPos];
            if (nId < 0 || (size_t)nId >= am->entries_cap) {
                am->vvNew[nUBucket][nUBucketPos] = -1; /* repair */
                fChanceFactor *= 1.2;
                continue;
            }
            struct addr_info *info = &am->entries[nId];
            double chance = fChanceFactor * addr_info_get_chance(info, nNow);
            if (GetRandInt(1 << 30) < chance * (double)(1 << 30)) {
                *result = *info;
                zcl_mutex_unlock(&am->cs);
                return true;
            }
            fChanceFactor *= 1.2;
        }
    }

    /* Fallback for sparse bucket layouts: if randomized bucket probing
     * failed, return the first eligible address from the random-order set
     * instead of spuriously reporting an empty addrman. */
    if (am->random_size > 0) {
        size_t start = (size_t)GetRandInt((int)am->random_size);
        for (size_t n = 0; n < am->random_size; n++) {
            int nId = am->random_order[(start + n) % am->random_size];
            if (nId < 0 || (size_t)nId >= am->entries_cap)
                continue;
            struct addr_info *info = &am->entries[nId];
            if (!info->used)
                continue;
            if (new_only && info->in_tried)
                continue;
            *result = *info;
            zcl_mutex_unlock(&am->cs);
            return true;
        }
    }

    zcl_mutex_unlock(&am->cs);
    LOG_FAIL("addrman", "select failed: no eligible address found after full scan");
}

void addrman_connected(struct addr_man *am, const struct net_service *addr,
                       int64_t nTime)
{
    zcl_mutex_lock(&am->cs);
    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->addr, &nId);
    if (!pinfo || !net_service_eq(&pinfo->addr.svc, addr)) {
        zcl_mutex_unlock(&am->cs);
        return;
    }
    int64_t nUpdateInterval = 20 * 60;
    if (nTime - (int64_t)pinfo->addr.nTime > nUpdateInterval)
        pinfo->addr.nTime = (uint32_t)nTime;
    zcl_mutex_unlock(&am->cs);
}

size_t addrman_get_addr(struct addr_man *am, struct net_address *out,
                        size_t max_out)
{
    zcl_mutex_lock(&am->cs);
    size_t nNodes = ADDRMAN_GETADDR_MAX_PCT * am->random_size / 100;
    if (nNodes > ADDRMAN_GETADDR_MAX) nNodes = ADDRMAN_GETADDR_MAX;
    if (nNodes > max_out) nNodes = max_out;

    size_t count = 0;
    int64_t nNow = GetAdjustedTime();
    for (size_t n = 0; n < am->random_size && count < nNodes; n++) {
        int nRndPos = GetRandInt((int)(am->random_size - n)) + (int)n;
        swap_random(am, (unsigned int)n, (unsigned int)nRndPos);
        int rid = am->random_order[n];
        if (rid < 0 || (size_t)rid >= am->entries_cap) continue;
        struct addr_info *ai = &am->entries[rid];
        if (!addr_info_is_terrible(ai, nNow))
            out[count++] = ai->addr;
    }

    zcl_mutex_unlock(&am->cs);
    return count;
}

int addrman_consistency_check(const struct addr_man *am,
                              char *err_buf, size_t err_cap)
{
#define CC_ERR(fmt, ...) do { \
    if (err_buf && err_cap > 0) \
        snprintf(err_buf, err_cap, fmt, ##__VA_ARGS__); \
    return -1; \
} while (0)

    /* 1. Verify new table: every non-(-1) slot points to a valid, used,
     *    non-tried entry with ref_count > 0. */
    int new_refs = 0;
    for (int b = 0; b < ADDRMAN_NEW_BUCKET_COUNT; b++) {
        for (int p = 0; p < ADDRMAN_BUCKET_SIZE; p++) {
            int id = am->vvNew[b][p];
            if (id == -1) continue;
            if (id < 0 || (size_t)id >= am->entries_cap)
                CC_ERR("new[%d][%d]: id=%d out of range (cap=%zu)",
                       b, p, id, am->entries_cap);
            const struct addr_info *info = &am->entries[id];
            if (!info->used)
                CC_ERR("new[%d][%d]: id=%d not used", b, p, id);
            if (info->in_tried)
                CC_ERR("new[%d][%d]: id=%d is in tried table", b, p, id);
            if (info->ref_count <= 0)
                CC_ERR("new[%d][%d]: id=%d ref_count=%d <= 0",
                       b, p, id, info->ref_count);
            new_refs++;
        }
    }

    /* 2. Verify tried table: every non-(-1) slot points to a valid, used,
     *    in_tried entry. */
    int tried_refs = 0;
    for (int b = 0; b < ADDRMAN_TRIED_BUCKET_COUNT; b++) {
        for (int p = 0; p < ADDRMAN_BUCKET_SIZE; p++) {
            int id = am->vvTried[b][p];
            if (id == -1) continue;
            if (id < 0 || (size_t)id >= am->entries_cap)
                CC_ERR("tried[%d][%d]: id=%d out of range", b, p, id);
            const struct addr_info *info = &am->entries[id];
            if (!info->used)
                CC_ERR("tried[%d][%d]: id=%d not used", b, p, id);
            if (!info->in_tried)
                CC_ERR("tried[%d][%d]: id=%d not marked in_tried",
                       b, p, id);
            tried_refs++;
        }
    }

    /* 3. Verify counts match. */
    if (tried_refs != am->tried_count)
        CC_ERR("tried_count mismatch: table has %d, tracked %d",
               tried_refs, am->tried_count);

    /* 4. Verify ref_count sums: each new-table entry's ref_count should
     *    equal the number of new bucket slots pointing to it. */
    for (int i = 0; i < am->id_count; i++) {
        const struct addr_info *info = &am->entries[i];
        if (!info->used || info->in_tried) continue;
        int actual_refs = 0;
        for (int b = 0; b < ADDRMAN_NEW_BUCKET_COUNT; b++)
            for (int p = 0; p < ADDRMAN_BUCKET_SIZE; p++)
                if (am->vvNew[b][p] == i)
                    actual_refs++;
        if (actual_refs != info->ref_count)
            CC_ERR("entry %d: ref_count=%d but %d bucket refs",
                   i, info->ref_count, actual_refs);
    }

    /* 5. Verify no duplicate entries in buckets. */
    for (int b = 0; b < ADDRMAN_TRIED_BUCKET_COUNT; b++) {
        for (int p1 = 0; p1 < ADDRMAN_BUCKET_SIZE; p1++) {
            if (am->vvTried[b][p1] == -1) continue;
            for (int p2 = p1 + 1; p2 < ADDRMAN_BUCKET_SIZE; p2++) {
                if (am->vvTried[b][p1] == am->vvTried[b][p2])
                    CC_ERR("tried[%d]: duplicate id=%d at pos %d and %d",
                           b, am->vvTried[b][p1], p1, p2);
            }
        }
    }

#undef CC_ERR
    return 0;
}

void addrman_get_bucket_stats(const struct addr_man *am,
                              struct addrman_bucket_stats *stats)
{
    memset(stats, 0, sizeof(*stats));

    for (int b = 0; b < ADDRMAN_NEW_BUCKET_COUNT; b++) {
        int fill = 0;
        for (int p = 0; p < ADDRMAN_BUCKET_SIZE; p++)
            if (am->vvNew[b][p] != -1) fill++;
        stats->new_occupied += fill;
        if (fill > 0) stats->new_buckets_nonempty++;
        if (fill > stats->max_new_bucket_fill)
            stats->max_new_bucket_fill = fill;
    }

    for (int b = 0; b < ADDRMAN_TRIED_BUCKET_COUNT; b++) {
        int fill = 0;
        for (int p = 0; p < ADDRMAN_BUCKET_SIZE; p++)
            if (am->vvTried[b][p] != -1) fill++;
        stats->tried_occupied += fill;
        if (fill > 0) stats->tried_buckets_nonempty++;
        if (fill > stats->max_tried_bucket_fill)
            stats->max_tried_bucket_fill = fill;
    }
}

bool addrman_serialize(const struct addr_man *am, struct byte_stream *s)
{
    if (!stream_write_u8(s, 1)) LOG_FAIL("addrman", "serialize: failed to write version");
    if (!stream_write_u8(s, 32)) LOG_FAIL("addrman", "serialize: failed to write key size");
    if (!stream_write_bytes(s, am->nKey.data, 32)) LOG_FAIL("addrman", "serialize: failed to write nKey");
    if (!stream_write_i32_le(s, am->new_count)) LOG_FAIL("addrman", "serialize: failed to write new_count");
    if (!stream_write_i32_le(s, am->tried_count)) LOG_FAIL("addrman", "serialize: failed to write tried_count");

    int nUBuckets = ADDRMAN_NEW_BUCKET_COUNT ^ (1 << 30);
    if (!stream_write_i32_le(s, nUBuckets)) LOG_FAIL("addrman", "serialize: failed to write bucket count");

    int *mapUnkIds = zcl_calloc((size_t)am->id_count > 0 ? (size_t)am->id_count : 1, sizeof(int), "addr_unk_ids");
    if (!mapUnkIds) LOG_FAIL("addrman", "serialize: alloc failed for mapUnkIds");

    int nIds = 0;
    for (int i = 0; i < am->id_count; i++) {
        if (am->entries[i].used && am->entries[i].ref_count > 0) {
            mapUnkIds[i] = nIds;
            if (!stream_write_bytes(s, am->entries[i].addr.svc.addr.ip, 16)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write new entry ip i=%d", i); }
            if (!stream_write_u16_le(s, am->entries[i].addr.svc.port)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write new entry port i=%d", i); }
            if (!stream_write_u64_le(s, am->entries[i].addr.nServices)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write new entry services i=%d", i); }
            if (!stream_write_u32_le(s, am->entries[i].addr.nTime)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write new entry time i=%d", i); }
            if (!stream_write_bytes(s, am->entries[i].source.ip, 16)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write new entry source i=%d", i); }
            if (!stream_write_i64_le(s, am->entries[i].last_success)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write new entry last_success i=%d", i); }
            if (!stream_write_i32_le(s, am->entries[i].attempts)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write new entry attempts i=%d", i); }
            nIds++;
        }
    }
    for (int i = 0; i < am->id_count; i++) {
        if (am->entries[i].used && am->entries[i].in_tried) {
            if (!stream_write_bytes(s, am->entries[i].addr.svc.addr.ip, 16)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write tried entry ip i=%d", i); }
            if (!stream_write_u16_le(s, am->entries[i].addr.svc.port)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write tried entry port i=%d", i); }
            if (!stream_write_u64_le(s, am->entries[i].addr.nServices)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write tried entry services i=%d", i); }
            if (!stream_write_u32_le(s, am->entries[i].addr.nTime)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write tried entry time i=%d", i); }
            if (!stream_write_bytes(s, am->entries[i].source.ip, 16)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write tried entry source i=%d", i); }
            if (!stream_write_i64_le(s, am->entries[i].last_success)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write tried entry last_success i=%d", i); }
            if (!stream_write_i32_le(s, am->entries[i].attempts)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write tried entry attempts i=%d", i); }
        }
    }

    for (int bucket = 0; bucket < ADDRMAN_NEW_BUCKET_COUNT; bucket++) {
        int nSize = 0;
        for (int i = 0; i < ADDRMAN_BUCKET_SIZE; i++)
            if (am->vvNew[bucket][i] != -1) nSize++;
        if (!stream_write_i32_le(s, nSize)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write bucket size bucket=%d", bucket); }
        for (int i = 0; i < ADDRMAN_BUCKET_SIZE; i++) {
            if (am->vvNew[bucket][i] != -1) {
                int nIndex = mapUnkIds[am->vvNew[bucket][i]];
                if (!stream_write_i32_le(s, nIndex)) { free(mapUnkIds); LOG_FAIL("addrman", "serialize: failed to write bucket index bucket=%d i=%d", bucket, i); }
            }
        }
    }

    free(mapUnkIds);
    return true;
}

bool addrman_deserialize(struct addr_man *am, struct byte_stream *s)
{
    addrman_clear(am);

    uint8_t nVersion;
    if (!stream_read_u8(s, &nVersion)) LOG_FAIL("addrman", "deserialize: failed to read version");
    uint8_t nKeySize;
    if (!stream_read_u8(s, &nKeySize)) LOG_FAIL("addrman", "deserialize: failed to read key size");
    if (nKeySize != 32) LOG_FAIL("addrman", "deserialize: invalid key size=%u expected 32", nKeySize);
    if (!stream_read_bytes(s, am->nKey.data, 32)) LOG_FAIL("addrman", "deserialize: failed to read nKey");

    int32_t nNew, nTried;
    if (!stream_read_i32_le(s, &nNew)) LOG_FAIL("addrman", "deserialize: failed to read new count");
    if (!stream_read_i32_le(s, &nTried)) LOG_FAIL("addrman", "deserialize: failed to read tried count");

    int32_t nUBuckets;
    if (!stream_read_i32_le(s, &nUBuckets)) LOG_FAIL("addrman", "deserialize: failed to read bucket count");
    if (nVersion != 0) nUBuckets ^= (1 << 30);

    /* Reject out-of-range counts BEFORE the (size_t) cast: a negative nNew/
     * nTried (corrupt/hostile peers.dat) would wrap to a huge need and force a
     * multi-GB zcl_realloc. LOG_FAIL alone (the prior form) logged but continued
     * into the overflow, so this must return false. A valid peers.dat never has
     * negative or over-cap counts, so this rejects only corrupt input. */
    if (nNew < 0 || nNew > ADDRMAN_NEW_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE) {
        LOG_FAIL("addrman", "deserialize: nNew=%d out of range", nNew);
        return false;
    }
    if (nTried < 0 || nTried > ADDRMAN_TRIED_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE) {
        LOG_FAIL("addrman", "deserialize: nTried=%d out of range", nTried);
        return false;
    }

    size_t need = (size_t)(nNew + nTried);
    if (need > am->entries_cap) {
        struct addr_info *p = zcl_realloc(am->entries,
                                       need * sizeof(struct addr_info), "addr_entries");
        if (!p) LOG_FAIL("addrman", "deserialize: realloc failed for entries need=%zu", need);
        memset(p + am->entries_cap, 0,
               (need - am->entries_cap) * sizeof(struct addr_info));
        am->entries = p;
        am->entries_cap = need;
    }

    for (int n = 0; n < nNew; n++) {
        struct addr_info *info = &am->entries[n];
        memset(info, 0, sizeof(*info));
        info->used = true;

        if (!stream_read_bytes(s, info->addr.svc.addr.ip, 16)) LOG_FAIL("addrman", "deserialize: failed to read new entry ip n=%d", n);
        if (!stream_read_u16_le(s, &info->addr.svc.port)) LOG_FAIL("addrman", "deserialize: failed to read new entry port n=%d", n);
        if (!stream_read_u64_le(s, &info->addr.nServices)) LOG_FAIL("addrman", "deserialize: failed to read new entry services n=%d", n);
        if (!stream_read_u32_le(s, &info->addr.nTime)) LOG_FAIL("addrman", "deserialize: failed to read new entry time n=%d", n);
        if (!stream_read_bytes(s, info->source.ip, 16)) LOG_FAIL("addrman", "deserialize: failed to read new entry source n=%d", n);
        if (!stream_read_i64_le(s, &info->last_success)) LOG_FAIL("addrman", "deserialize: failed to read new entry last_success n=%d", n);
        int32_t attempts;
        if (!stream_read_i32_le(s, &attempts)) LOG_FAIL("addrman", "deserialize: failed to read new entry attempts n=%d", n);
        info->attempts = attempts;

        info->random_pos = (int)am->random_size;
        random_push(am, n);

        if (nVersion != 1 || nUBuckets != ADDRMAN_NEW_BUCKET_COUNT) {
            int nUBucket = addr_info_get_new_bucket(info, &am->nKey,
                                                     &info->source);
            int nUBucketPos = addr_info_get_bucket_position(info, &am->nKey,
                                                             true, nUBucket);
            if (am->vvNew[nUBucket][nUBucketPos] == -1) {
                am->vvNew[nUBucket][nUBucketPos] = n;
                info->ref_count++;
            }
        }
    }
    am->id_count = nNew;
    am->new_count = nNew;

    int nLost = 0;
    for (int n = 0; n < nTried; n++) {
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        info.used = true;

        if (!stream_read_bytes(s, info.addr.svc.addr.ip, 16)) LOG_FAIL("addrman", "deserialize: failed to read tried entry ip n=%d", n);
        if (!stream_read_u16_le(s, &info.addr.svc.port)) LOG_FAIL("addrman", "deserialize: failed to read tried entry port n=%d", n);
        if (!stream_read_u64_le(s, &info.addr.nServices)) LOG_FAIL("addrman", "deserialize: failed to read tried entry services n=%d", n);
        if (!stream_read_u32_le(s, &info.addr.nTime)) LOG_FAIL("addrman", "deserialize: failed to read tried entry time n=%d", n);
        if (!stream_read_bytes(s, info.source.ip, 16)) LOG_FAIL("addrman", "deserialize: failed to read tried entry source n=%d", n);
        if (!stream_read_i64_le(s, &info.last_success)) LOG_FAIL("addrman", "deserialize: failed to read tried entry last_success n=%d", n);
        int32_t attempts;
        if (!stream_read_i32_le(s, &attempts)) LOG_FAIL("addrman", "deserialize: failed to read tried entry attempts n=%d", n);
        info.attempts = attempts;

        int nKBucket = addr_info_get_tried_bucket(&info, &am->nKey);
        int nKBucketPos = addr_info_get_bucket_position(&info, &am->nKey,
                                                         false, nKBucket);
        if (am->vvTried[nKBucket][nKBucketPos] == -1) {
            info.random_pos = (int)am->random_size;
            info.in_tried = true;
            int id = am->id_count++;
            if ((size_t)id >= am->entries_cap) {
                size_t new_cap = am->entries_cap * 2;
                while (new_cap <= (size_t)id) new_cap *= 2;
                struct addr_info *p = zcl_realloc(am->entries,
                    new_cap * sizeof(struct addr_info), "addr_entries");
                if (!p) continue;
                memset(p + am->entries_cap, 0,
                    (new_cap - am->entries_cap) * sizeof(struct addr_info));
                am->entries = p;
                am->entries_cap = new_cap;
            }
            am->entries[id] = info;
            random_push(am, id);
            am->vvTried[nKBucket][nKBucketPos] = id;
        } else {
            nLost++;
        }
    }
    am->tried_count = nTried - nLost;

    for (int bucket = 0; bucket < nUBuckets; bucket++) {
        int32_t nSize;
        if (!stream_read_i32_le(s, &nSize)) LOG_FAIL("addrman", "deserialize: failed to read bucket size bucket=%d", bucket);
        for (int n = 0; n < nSize; n++) {
            int32_t nIndex;
            if (!stream_read_i32_le(s, &nIndex)) LOG_FAIL("addrman", "deserialize: failed to read bucket index bucket=%d n=%d", bucket, n);
            if (nIndex >= 0 && nIndex < nNew && bucket < ADDRMAN_NEW_BUCKET_COUNT
                && (size_t)nIndex < am->entries_cap) {
                struct addr_info *info = &am->entries[nIndex];
                int nUBucketPos = addr_info_get_bucket_position(
                    info, &am->nKey, true, bucket);
                if (nVersion == 1 && nUBuckets == ADDRMAN_NEW_BUCKET_COUNT &&
                    am->vvNew[bucket][nUBucketPos] == -1 &&
                    info->ref_count < ADDRMAN_NEW_BUCKETS_PER_ADDRESS) {
                    info->ref_count++;
                    am->vvNew[bucket][nUBucketPos] = nIndex;
                }
            }
        }
    }

    return true;
}
