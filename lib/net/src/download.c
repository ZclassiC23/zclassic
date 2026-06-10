/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block download manager — coordinates parallel block downloads.
 * Lock-free reads where possible, mutex for writes. */

#include "platform/time_compat.h"
#include "net/download.h"
#include "event/event.h"
#include "sync/sync_state.h"
#include "util/log_macros.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "util/safe_alloc.h"

#define INITIAL_SLOTS 2048
#define INITIAL_QUEUE 4096

/* ── Dynamic IBD-aware limits ─────────────────────────────────── */

static bool dl_is_ibd(void)
{
    enum sync_state st = sync_get_state();
    return st == SYNC_HEADERS_DOWNLOAD ||
           st == SYNC_BLOCKS_DOWNLOAD ||
           st == SYNC_CONNECTING_BLOCKS;
}

size_t dl_get_max_in_flight_total(void)
{
    return dl_is_ibd() ? DL_MAX_IN_FLIGHT_TOTAL_IBD
                       : DL_MAX_IN_FLIGHT_TOTAL;
}

int dl_get_request_timeout_secs(void)
{
    return dl_is_ibd() ? DL_REQUEST_TIMEOUT_SECS_IBD
                       : DL_REQUEST_TIMEOUT_SECS;
}

/* FNV-1a hash for uint256 → slot index */
static size_t hash_slot(const struct uint256 *h, size_t mask)
{
    uint64_t fnv = 14695981039346656037ULL;
    for (int i = 0; i < 32; i++) {
        fnv ^= h->data[i];
        fnv *= 1099511628211ULL;
    }
    return (size_t)(fnv & mask);
}

void dl_init(struct download_manager *dm)
{
    memset(dm, 0, sizeof(*dm));
    zcl_mutex_init(&dm->cs);
    dm->num_slots = INITIAL_SLOTS;
    dm->slots = zcl_calloc(dm->num_slots, sizeof(struct dl_in_flight), "dl_slots");
    dm->queue_cap = INITIAL_QUEUE;
    dm->queue = zcl_malloc(dm->queue_cap * sizeof(struct uint256), "dl_queue");
    dm->queue_heights = zcl_malloc(dm->queue_cap * sizeof(int32_t), "dl_queue_heights");
    if (!dm->slots || !dm->queue || !dm->queue_heights) {
        free(dm->slots); free(dm->queue); free(dm->queue_heights);
        dm->slots = NULL; dm->queue = NULL; dm->queue_heights = NULL;
        dm->num_slots = 0; dm->queue_cap = 0;
    }
}

void dl_free(struct download_manager *dm)
{
    free(dm->slots);
    free(dm->queue);
    free(dm->queue_heights);
    dm->slots = NULL;
    dm->queue = NULL;
    dm->queue_heights = NULL;
}

/* Expand queue capacity. Returns true on success. Caller holds mutex. */
static bool dl_queue_grow(struct download_manager *dm)
{
    if (dm->queue_cap >= 65536) LOG_FAIL("net", "dl_queue_grow: queue at max capacity (65536)");
    size_t new_cap = dm->queue_cap * 2;
    struct uint256 *nq = zcl_realloc(dm->queue, new_cap * sizeof(struct uint256), "dl_queue");
    int32_t *nh = zcl_realloc(dm->queue_heights, new_cap * sizeof(int32_t), "dl_queue_heights");
    if (!nq || !nh) {
        /* If one succeeded, keep the old pointer valid */
        if (nq) dm->queue = nq;
        if (nh) dm->queue_heights = nh;
        LOG_FAIL("net", "dl_queue_grow: realloc failed for new_cap=%zu", new_cap);
    }
    dm->queue = nq;
    dm->queue_heights = nh;
    dm->queue_cap = new_cap;
    return true;
}

/* Sort key for queue ordering: lowest height first (= closest to the
 * tip = the block that actually advances the active chain). A height of
 * -1 means "unknown" — such blocks must NOT preempt known tip-adjacent
 * blocks, so they sort to the BACK (treated as +infinity). */
static inline int64_t dl_sort_key(int32_t height)
{
    return height < 0 ? (int64_t)INT64_MAX : (int64_t)height;
}

/* Insert a block into the download queue, keeping it sorted by height
 * ascending (lowest height = front). This is the STRUCTURAL guarantee
 * that tip-advancing blocks can never be tail-starved: dl_assign_to_peer
 * pops from the front, so the block closest to the tip is always fetched
 * first regardless of any caller's enqueue order. Caller holds mutex.
 *
 * Cost: O(log n) to find the slot + O(n) memmove to open it. Pushes are
 * one-at-a-time and the queue is bounded (<= 65536), so this is cheap
 * relative to the per-item O(n) dedup scan callers already perform. */
static bool dl_queue_push(struct download_manager *dm,
                           const struct uint256 *hash, int32_t height)
{
    if (dm->queue_len >= dm->queue_cap && !dl_queue_grow(dm))
        LOG_FAIL("net", "dl_queue_push: queue full and grow failed (len=%zu, cap=%zu)", dm->queue_len, dm->queue_cap);

    /* Binary search for the first entry whose key is strictly greater
     * than ours; insert there (stable for equal heights). */
    int64_t key = dl_sort_key(height);
    size_t lo = 0, hi = dm->queue_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (dl_sort_key(dm->queue_heights[mid]) <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    size_t pos = lo;

    if (pos < dm->queue_len) {
        memmove(&dm->queue[pos + 1], &dm->queue[pos],
                (dm->queue_len - pos) * sizeof(struct uint256));
        memmove(&dm->queue_heights[pos + 1], &dm->queue_heights[pos],
                (dm->queue_len - pos) * sizeof(int32_t));
    }
    dm->queue[pos] = *hash;
    dm->queue_heights[pos] = height;
    dm->queue_len++;
    return true;
}

/* Find or update peer stats. Caller holds mutex. */
static struct dl_peer_stats *dl_find_peer(struct download_manager *dm,
                                           uint32_t peer_id, bool create)
{
    for (size_t i = 0; i < dm->num_peers; i++) {
        if (dm->peers[i].peer_id == peer_id)
            return &dm->peers[i];
    }
    if (!create || dm->num_peers >= 256)
        return NULL;
    struct dl_peer_stats *p = &dm->peers[dm->num_peers++];
    memset(p, 0, sizeof(*p));
    p->peer_id = peer_id;
    p->active = true;
    return p;
}

/* Find slot for hash (open addressing with linear probe).
 * Handles gaps from deletions: inactive slots are NOT probe-chain
 * terminators because dl_mark_received clears slots without
 * rehashing. We must scan through inactive slots to find matches. */
static struct dl_in_flight *find_slot(struct download_manager *dm,
                                       const struct uint256 *hash,
                                       bool find_empty)
{
    if (!dm->slots || dm->num_slots == 0) return NULL;
    size_t mask = dm->num_slots - 1;
    size_t idx = hash_slot(hash, mask);
    struct dl_in_flight *first_empty = NULL;

    for (size_t i = 0; i < dm->num_slots; i++) {
        struct dl_in_flight *s = &dm->slots[(idx + i) & mask];
        if (!s->active) {
            if (!first_empty) first_empty = s;
            /* Check if this slot was NEVER used (zero hash = virgin) */
            if (uint256_is_null(&s->hash))
                break; /* end of probe chain */
            continue; /* skip gap from deletion, keep probing */
        }
        if (uint256_eq(&s->hash, hash))
            return s;
    }
    return find_empty ? first_empty : NULL;
}

/* Rehash into a table of given size (must be power of 2). */
static void dl_rehash(struct download_manager *dm, size_t new_size)
{
    struct dl_in_flight *new_slots = zcl_calloc(new_size, sizeof(struct dl_in_flight), "dl_slots");
    if (!new_slots) return;

    size_t new_mask = new_size - 1;
    for (size_t i = 0; i < dm->num_slots; i++) {
        if (!dm->slots[i].active) continue;
        size_t idx = hash_slot(&dm->slots[i].hash, new_mask);
        for (size_t j = 0; j < new_size; j++) {
            struct dl_in_flight *s = &new_slots[(idx + j) & new_mask];
            if (!s->active) {
                *s = dm->slots[i];
                break;
            }
        }
    }
    free(dm->slots);
    dm->slots = new_slots;
    dm->num_slots = new_size;
}

/* Grow or compact hash table.
 * Grows when load factor > 50%.
 * Compacts (rehash in place) when active entries < 25% of slots
 * and total insertions have left many dead gaps. */
static void maybe_grow(struct download_manager *dm)
{
    if (dm->num_active * 2 >= dm->num_slots) {
        dl_rehash(dm, dm->num_slots * 2);
    } else if (dm->num_slots > INITIAL_SLOTS &&
               dm->num_active * 4 < dm->num_slots) {
        /* Compact: rehash at same size to eliminate dead gaps */
        dl_rehash(dm, dm->num_slots);
    }
}

bool dl_is_in_flight(struct download_manager *dm, const struct uint256 *hash)
{
    zcl_mutex_lock(&dm->cs);
    struct dl_in_flight *s = find_slot(dm, hash, false);
    bool found = (s != NULL && s->active);
    zcl_mutex_unlock(&dm->cs);
    return found;
}

bool dl_mark_requested(struct download_manager *dm,
                       const struct uint256 *hash, int32_t height,
                       uint32_t peer_id)
{
    zcl_mutex_lock(&dm->cs);

    /* Check if already in-flight */
    struct dl_in_flight *existing = find_slot(dm, hash, false);
    if (existing && existing->active) {
        dm->total_duplicate++;
        zcl_mutex_unlock(&dm->cs);
        return false;
    }

    /* Check global limit (dynamic: aggressive during IBD) */
    if (dm->num_active >= dl_get_max_in_flight_total()) {
        zcl_mutex_unlock(&dm->cs);
        return false;
    }

    maybe_grow(dm);

    /* Remove from queue if present (block is moving to in-flight) */
    for (size_t j = 0; j < dm->queue_len; j++) {
        if (uint256_eq(&dm->queue[j], hash)) {
            dm->queue_len--;
            if (j < dm->queue_len) {
                memmove(&dm->queue[j], &dm->queue[j + 1],
                        (dm->queue_len - j) * sizeof(struct uint256));
                memmove(&dm->queue_heights[j], &dm->queue_heights[j + 1],
                        (dm->queue_len - j) * sizeof(int32_t));
            }
            break;
        }
    }

    /* Find empty slot */
    struct dl_in_flight *slot = find_slot(dm, hash, true);
    if (!slot) {
        zcl_mutex_unlock(&dm->cs);
        LOG_FAIL("net", "dl_mark_requested: no empty slot in hash table (active=%zu, slots=%zu)",
                 dm->num_active, dm->num_slots);
    }

    slot->hash = *hash;
    slot->height = height;
    slot->peer_id = peer_id;
    slot->request_time = (int64_t)platform_time_wall_time_t();
    slot->active = true;
    dm->num_active++;
    dm->total_requested++;

    /* Update peer stats */
    {
        struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, true);
        if (ps) ps->blocks_requested++;
    }

    zcl_mutex_unlock(&dm->cs);
    return true;
}

uint32_t dl_mark_received(struct download_manager *dm,
                          const struct uint256 *hash)
{
    zcl_mutex_lock(&dm->cs);

    struct dl_in_flight *s = find_slot(dm, hash, false);
    if (!s || !s->active) {
        zcl_mutex_unlock(&dm->cs);
        return 0;
    }

    uint32_t peer_id = s->peer_id;
    int64_t delivery = (int64_t)platform_time_wall_time_t() - s->request_time;

    s->active = false;
    /* Don't zero the hash — find_slot needs it to detect "was used" vs "never used"
     * for proper probe chain handling after deletions. */
    dm->num_active--;
    dm->total_received++;

    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, false);
    if (ps) {
        ps->blocks_received++;
        ps->last_block_time = (int64_t)platform_time_wall_time_t();
        int64_t delivery_us = delivery * 1000000;
        if (ps->avg_delivery_us == 0)
            ps->avg_delivery_us = delivery_us;
        else
            ps->avg_delivery_us = (ps->avg_delivery_us * 7 + delivery_us) / 8;

        /* Update bandwidth score: inverse of delivery time.
         * Score 128 = baseline (1s delivery). Faster = higher score.
         * Range: 1 (very slow) to 255 (very fast, <50ms). */
        if (ps->avg_delivery_us > 0) {
            /* 128M / avg_us gives ~128 at 1s, ~256 at 0.5s, ~64 at 2s */
            uint64_t score = 128000000ULL / (uint64_t)ps->avg_delivery_us;
            if (score < 1) score = 1;
            if (score > 255) score = 255;
            ps->bandwidth_score = (uint32_t)score;
        }
    }

    zcl_mutex_unlock(&dm->cs);
    return peer_id;
}

size_t dl_check_timeouts(struct download_manager *dm, int64_t now)
{
    zcl_mutex_lock(&dm->cs);

    size_t reassigned = 0;
    for (size_t i = 0; i < dm->num_slots; i++) {
        struct dl_in_flight *s = &dm->slots[i];
        if (!s->active) continue;

        int64_t age = now - s->request_time;
        if (age < dl_get_request_timeout_secs()) continue;

        /* Timed out — move back to queue for reassignment */
        event_emitf(EV_BLOCK_REQUESTED, s->peer_id,
                    "TIMEOUT h=%d age=%llds", s->height, (long long)age);

        struct dl_peer_stats *ps = dl_find_peer(dm, s->peer_id, false);
        if (ps) ps->blocks_timed_out++;

        dl_queue_push(dm, &s->hash, s->height);
        s->active = false;
        dm->num_active--;
        dm->total_timed_out++;
        reassigned++;
    }

    /* Compact hash table if it has many dead gaps */
    maybe_grow(dm);

    zcl_mutex_unlock(&dm->cs);
    return reassigned;
}

size_t dl_peer_in_flight(struct download_manager *dm, uint32_t peer_id)
{
    zcl_mutex_lock(&dm->cs);
    size_t count = 0;
    for (size_t i = 0; i < dm->num_slots; i++) {
        if (dm->slots[i].active && dm->slots[i].peer_id == peer_id)
            count++;
    }
    zcl_mutex_unlock(&dm->cs);
    return count;
}

size_t dl_peer_disconnected(struct download_manager *dm, uint32_t peer_id)
{
    zcl_mutex_lock(&dm->cs);
    size_t requeued = 0;

    for (size_t i = 0; i < dm->num_slots; i++) {
        struct dl_in_flight *s = &dm->slots[i];
        if (!s->active || s->peer_id != peer_id) continue;

        dl_queue_push(dm, &s->hash, s->height);
        s->active = false;
        dm->num_active--;
        requeued++;
    }

    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, false);
    if (ps) ps->active = false;

    if (requeued > 0)
        event_emitf(EV_BLOCK_REQUESTED, peer_id,
                    "peer disconnect: %zu blocks requeued", requeued);

    zcl_mutex_unlock(&dm->cs);
    return requeued;
}

size_t dl_queue_blocks(struct download_manager *dm,
                       const struct uint256 *hashes,
                       const int32_t *heights,
                       size_t count)
{
    zcl_mutex_lock(&dm->cs);

    size_t added = 0;
    for (size_t i = 0; i < count; i++) {
        /* Skip if already in-flight */
        struct dl_in_flight *s = find_slot(dm, &hashes[i], false);
        if (s && s->active) continue;

        /* Skip if already in queue (linear scan — acceptable for queue < 65K) */
        bool dup = false;
        for (size_t j = 0; j < dm->queue_len; j++) {
            if (uint256_eq(&dm->queue[j], &hashes[i])) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        if (dl_queue_push(dm, &hashes[i], heights ? heights[i] : -1))
            added++;
    }

    zcl_mutex_unlock(&dm->cs);
    return added;
}

void dl_queue_priority(struct download_manager *dm,
                       const struct uint256 *hash, int32_t height)
{
    zcl_mutex_lock(&dm->cs);

    /* Skip if already in-flight */
    struct dl_in_flight *s = find_slot(dm, hash, false);
    if (s && s->active) {
        zcl_mutex_unlock(&dm->cs);
        return;
    }

    /* Remove from queue if already present (we'll re-insert sorted) */
    for (size_t j = 0; j < dm->queue_len; j++) {
        if (uint256_eq(&dm->queue[j], hash)) {
            memmove(&dm->queue[j], &dm->queue[j+1],
                    (dm->queue_len - j - 1) * sizeof(dm->queue[0]));
            memmove(&dm->queue_heights[j], &dm->queue_heights[j+1],
                    (dm->queue_len - j - 1) * sizeof(dm->queue_heights[0]));
            dm->queue_len--;
            break;
        }
    }

    /* Insert keeping the queue sorted by height ascending. Because the
     * queue is height-ordered and dl_assign_to_peer pops from the front,
     * a priority block (always tip-adjacent / lowest-height in practice)
     * lands at the front and is fetched first — without breaking the
     * single sorted invariant that makes tail-starvation impossible. */
    dl_queue_push(dm, hash, height);

    zcl_mutex_unlock(&dm->cs);
}

size_t dl_assign_to_peer(struct download_manager *dm,
                         uint32_t peer_id,
                         struct uint256 *out_hashes,
                         size_t max_assign)
{
    zcl_mutex_lock(&dm->cs);

    /* Adaptive per-peer limit: fast peers get larger windows.
     * bandwidth_score 0-63 → 16 slots, 64-127 → 32-64, 128+ → 64-128.
     * This naturally gives ~4x more work to 4x-faster peers. */
    size_t peer_count = 0;
    for (size_t i = 0; i < dm->num_slots; i++) {
        if (dm->slots[i].active && dm->slots[i].peer_id == peer_id)
            peer_count++;
    }
    struct dl_peer_stats *ps_assign = dl_find_peer(dm, peer_id, false);
    size_t peer_limit = DL_MAX_IN_FLIGHT_PER_PEER; /* default for new peers */
    if (ps_assign && ps_assign->is_loopback) {
        /* K2: loopback has no WAN-fairness constraint and effectively
         * unlimited bandwidth. Use the elevated cap; the global limit
         * (DL_MAX_IN_FLIGHT_TOTAL_IBD=4096) and the consumer-side reducer
         * pipeline are the real backpressure. */
        peer_limit = DL_MAX_IN_FLIGHT_PER_LOOPBACK;
    } else if (ps_assign && ps_assign->bandwidth_score > 0) {
        /* Scale: score/128 * MAX, clamped to [16, MAX] */
        peer_limit = (size_t)ps_assign->bandwidth_score
                     * DL_MAX_IN_FLIGHT_PER_PEER / 128;
        if (peer_limit < 16) peer_limit = 16;
        if (peer_limit > DL_MAX_IN_FLIGHT_PER_PEER)
            peer_limit = DL_MAX_IN_FLIGHT_PER_PEER;
    }
    size_t available = 0;
    if (peer_count < peer_limit)
        available = peer_limit - peer_count;
    if (available > max_assign) available = max_assign;
    if (available > dm->queue_len) available = dm->queue_len;

    /* Also respect global limit (dynamic: aggressive during IBD) */
    size_t global_limit = dl_get_max_in_flight_total();
    if (dm->num_active + available > global_limit)
        available = global_limit - dm->num_active;

    /* Pop from front — the queue is kept height-sorted ascending by
     * dl_queue_push, so the front is always the LOWEST-height queued
     * block (the one closest to the tip). Popping front therefore hands
     * out tip-advancing blocks first, making it structurally impossible
     * for any caller's enqueue order to tail-starve the connectable
     * bottom. Batch the memmove after the loop (O(1) amortized per pop). */
    size_t pop_count = 0;
    size_t assigned = 0;
    while (assigned < available && pop_count < dm->queue_len) {
        struct uint256 hash = dm->queue[pop_count];
        int32_t height = dm->queue_heights[pop_count];
        pop_count++;

        maybe_grow(dm);
        struct dl_in_flight *slot = find_slot(dm, &hash, true);
        if (!slot) continue;

        slot->hash = hash;
        slot->height = height;
        slot->peer_id = peer_id;
        slot->request_time = (int64_t)platform_time_wall_time_t();
        slot->active = true;
        dm->num_active++;
        dm->total_requested++;

        out_hashes[assigned++] = hash;
    }

    /* Batch shift: remove all popped entries in one memmove */
    if (pop_count > 0) {
        dm->queue_len -= pop_count;
        if (dm->queue_len > 0) {
            memmove(&dm->queue[0], &dm->queue[pop_count],
                    dm->queue_len * sizeof(struct uint256));
            memmove(&dm->queue_heights[0], &dm->queue_heights[pop_count],
                    dm->queue_len * sizeof(int32_t));
        }
    }

    if (assigned > 0) {
        struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, true);
        if (ps) ps->blocks_requested += (uint32_t)assigned;
    }

    zcl_mutex_unlock(&dm->cs);
    return assigned;
}

void dl_peer_block_received(struct download_manager *dm,
                            uint32_t peer_id, int64_t delivery_us)
{
    zcl_mutex_lock(&dm->cs);
    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, false);
    if (ps) {
        ps->blocks_received++;
        ps->last_block_time = (int64_t)platform_time_wall_time_t();
        if (ps->avg_delivery_us == 0)
            ps->avg_delivery_us = delivery_us;
        else
            ps->avg_delivery_us = (ps->avg_delivery_us * 7 + delivery_us) / 8;

        /* Update bandwidth score */
        if (ps->avg_delivery_us > 0) {
            uint64_t score = 128000000ULL / (uint64_t)ps->avg_delivery_us;
            if (score < 1) score = 1;
            if (score > 255) score = 255;
            ps->bandwidth_score = (uint32_t)score;
        }
    }
    zcl_mutex_unlock(&dm->cs);
}

size_t dl_peer_adaptive_window(struct download_manager *dm, uint32_t peer_id)
{
    zcl_mutex_lock(&dm->cs);
    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, false);
    size_t window = DL_MAX_IN_FLIGHT_PER_PEER;
    if (ps && ps->is_loopback) {
        window = DL_MAX_IN_FLIGHT_PER_LOOPBACK;
    } else if (ps && ps->bandwidth_score > 0) {
        window = (size_t)ps->bandwidth_score * DL_MAX_IN_FLIGHT_PER_PEER / 128;
        if (window < 16) window = 16;
        if (window > DL_MAX_IN_FLIGHT_PER_PEER) window = DL_MAX_IN_FLIGHT_PER_PEER;
    }
    zcl_mutex_unlock(&dm->cs);
    return window;
}

void dl_set_peer_loopback(struct download_manager *dm,
                          uint32_t peer_id, bool is_loopback)
{
    if (!dm) return;
    zcl_mutex_lock(&dm->cs);
    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, true);
    if (ps) ps->is_loopback = is_loopback;
    zcl_mutex_unlock(&dm->cs);
}

void dl_add_bytes_received(struct download_manager *dm, uint64_t bytes)
{
    zcl_mutex_lock(&dm->cs);
    if (dm->sync_start_time == 0)
        dm->sync_start_time = (int64_t)platform_time_wall_time_t();
    dm->total_bytes_received += bytes;
    zcl_mutex_unlock(&dm->cs);
}

void dl_get_throughput(struct download_manager *dm,
                       uint64_t *total_bytes, double *mbps_avg)
{
    zcl_mutex_lock(&dm->cs);
    if (total_bytes) *total_bytes = dm->total_bytes_received;
    if (mbps_avg) {
        if (dm->sync_start_time > 0 && dm->total_bytes_received > 0) {
            int64_t elapsed = (int64_t)platform_time_wall_time_t() - dm->sync_start_time;
            if (elapsed < 1) elapsed = 1;
            *mbps_avg = (double)dm->total_bytes_received / (1048576.0 * elapsed);
        } else {
            *mbps_avg = 0.0;
        }
    }
    zcl_mutex_unlock(&dm->cs);
}

void dl_get_stats(struct download_manager *dm,
                  uint64_t *requested, uint64_t *received,
                  uint64_t *timed_out, uint64_t *in_flight,
                  uint64_t *queued)
{
    zcl_mutex_lock(&dm->cs);
    if (requested)  *requested  = dm->total_requested;
    if (received)   *received   = dm->total_received;
    if (timed_out)  *timed_out  = dm->total_timed_out;
    if (in_flight)  *in_flight  = dm->num_active;
    if (queued)     *queued     = dm->queue_len;
    zcl_mutex_unlock(&dm->cs);
}

size_t dl_drain_for_backpressure(struct download_manager *dm)
{
    if (!dm) return 0;
    zcl_mutex_lock(&dm->cs);
    size_t drained = dm->queue_len + dm->num_active;

    /* Drop pending hashes outright — peers won't be re-asked until
     * something else (header sync, reducer activation) re-queues. */
    dm->queue_len = 0;

    /* Mark every in-flight slot inactive WITHOUT zeroing its hash:
     * find_slot relies on the hash bits to distinguish a virgin slot
     * from a deletion gap during open-addressing probes. The block
     * body that arrives later finds no active slot, dl_mark_received
     * returns 0, and net_message_free reclaims the buffer as usual. */
    for (size_t i = 0; i < dm->num_slots; i++)
        dm->slots[i].active = false;
    dm->num_active = 0;

    zcl_mutex_unlock(&dm->cs);
    return drained;
}
