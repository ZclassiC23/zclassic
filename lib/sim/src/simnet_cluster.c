/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Deterministic in-memory multi-node simnet cluster.
 *
 * Delivery scheduling is event-driven. A pending delivery is "ready" when the
 * receiver already holds the block (a duplicate arrived by another path) OR
 * holds the block's parent. Because a chain only ever GAINS blocks (blocks are
 * retained, never removed), readiness is monotonic: once ready, always ready.
 *
 * Instead of re-scanning the whole queue for the min-ready entry on every
 * delivery (O(N^2) per drain), the scheduler keeps:
 *   - a min-heap of currently-ready deliveries keyed by (deliver_us, seq) — the
 *     EXACT tie-break the old linear scan used, so the delivery order is
 *     byte-for-byte identical;
 *   - a "waiting" hash map: every not-yet-ready delivery is registered under
 *     the specific receiver-block hash(es) it is missing (its own hash and its
 *     parent hash). When a node gains a block, only the deliveries waiting on
 *     (that node, that hash) are re-checked and promoted into the heap.
 *
 * This is exactly "recompute readiness for everyone whenever chain state
 * changes", done incrementally rather than by full rescan, so it schedules the
 * identical sequence the linear scan did — which the recorded delivery
 * fingerprint / seed-tape capsules depend on.
 */

#define ZCL_SIMNET_CLUSTER_INTERNAL
#include "sim/simnet_cluster.h"

#include "platform/clock.h"
#include "platform/rng.h"
#include "sim/seed_tape.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SIM_CLUSTER_EVENT_BLOCK_ENQUEUED 128u
#define SIM_CLUSTER_MIN_LATENCY_US 50u
#define SIM_CLUSTER_LATENCY_SPAN_US 5000u

/* Power-of-two bucket count for the waiting map. Sized so the per-drain live
 * key set (bounded by the pending count of one drain batch) keeps chains
 * short; cleared per drain, so this is not a hard capacity. */
#define SIM_CLUSTER_WAIT_BUCKETS 16384u

struct simnet_delivery {
    size_t from_node;
    size_t to_node;
    struct uint256 block_hash;
    uint64_t first_seen;
    uint64_t deliver_us;
    uint64_t seq;
};

/* Lifecycle of a pending delivery inside the scheduler. */
enum simnet_pending_state {
    SIM_PEND_WAITING = 0,  /* not ready; registered in the waiting map */
    SIM_PEND_READY = 1,    /* in the ready heap, awaiting delivery */
    SIM_PEND_DONE = 2,     /* delivered */
};

struct simnet_pending {
    struct simnet_delivery d;
    uint8_t state;
};

/* One registration of a waiting pending under a single (to_node, hash) trigger
 * key. A waiting pending is registered under both hashes it is missing, so it
 * appears in up to two bucket chains. */
struct simnet_wait_reg {
    uint32_t pool_idx;
    int32_t next;  /* next registration in the same bucket chain, or -1 */
};

struct simnet_cluster {
    size_t node_count;
    struct simnet_chain **nodes;
    seed_tape_t *tape;

    /* Pending-delivery pool. Entries are referenced by index everywhere
     * (heap, waiting map) so a realloc-grow never invalidates a reference.
     * Fully reset to empty after each successful drain. */
    struct simnet_pending *pool;
    size_t pool_count;
    size_t pool_cap;

    /* Min-heap of ready pool indices, ordered by (deliver_us, seq). */
    uint32_t *heap;
    size_t heap_count;
    size_t heap_cap;

    /* Waiting map: bucket_head[b] indexes the first registration in bucket b
     * (or -1). Registrations live in the flat regs pool. */
    struct simnet_wait_reg *regs;
    size_t reg_count;
    size_t reg_cap;
    int32_t bucket_head[SIM_CLUSTER_WAIT_BUCKETS];

    size_t waiting_count;  /* pool entries currently in SIM_PEND_WAITING */

    uint64_t next_first_seen;
    uint64_t next_delivery_seq;
    uint64_t delivery_fingerprint;
};

struct simnet_delivery_record {
    uint64_t from_node;
    uint64_t to_node;
    uint64_t deliver_us;
    uint8_t block_hash[32];
};

static uint64_t simnet_cluster_now_us(void)
{
    int64_t ns = clock_now_monotonic_ns();
    if (ns <= 0)
        return 0;
    return (uint64_t)ns / 1000u;
}

static void simnet_cluster_fp_mix(struct simnet_cluster *cluster,
                                  const struct simnet_delivery *delivery)
{
    uint64_t h = cluster->delivery_fingerprint ?
        cluster->delivery_fingerprint : 1469598103934665603ULL;
    h ^= delivery->from_node + 0x9e3779b97f4a7c15ULL;
    h *= 1099511628211ULL;
    h ^= delivery->to_node + (delivery->seq << 7);
    h *= 1099511628211ULL;
    h ^= delivery->deliver_us;
    h *= 1099511628211ULL;
    h ^= uint256_get_cheap_hash(&delivery->block_hash);
    h *= 1099511628211ULL;
    cluster->delivery_fingerprint = h;
}

/* ── comparator (unchanged tie-break: deliver_us, then seq) ─────────── */

static bool simnet_cluster_delivery_less(const struct simnet_delivery *a,
                                         const struct simnet_delivery *b)
{
    if (a->deliver_us != b->deliver_us)
        return a->deliver_us < b->deliver_us;
    return a->seq < b->seq;
}

static bool simnet_cluster_delivery_ready(const struct simnet_cluster *cluster,
                                          const struct simnet_delivery *d)
{
    if (!cluster || !d || d->from_node >= cluster->node_count ||
        d->to_node >= cluster->node_count)
        return false;

    const struct block *block =
        simnet_chain_block_by_hash(cluster->nodes[d->from_node],
                                   &d->block_hash);
    if (!block)
        return false;
    if (simnet_chain_has_block(cluster->nodes[d->to_node], &d->block_hash))
        return true;
    return simnet_chain_has_parent_for_block(cluster->nodes[d->to_node],
                                             block);
}

/* ── ready heap (min-heap of pool indices) ─────────────────────────── */

static bool simnet_cluster_heap_less(const struct simnet_cluster *cluster,
                                     uint32_t a, uint32_t b)
{
    return simnet_cluster_delivery_less(&cluster->pool[a].d,
                                        &cluster->pool[b].d);
}

static bool simnet_cluster_heap_push(struct simnet_cluster *cluster,
                                     uint32_t pool_idx)
{
    if (cluster->heap_count >= cluster->heap_cap) {
        size_t new_cap = cluster->heap_cap ? cluster->heap_cap * 2 : 16;
        uint32_t *grown = zcl_realloc(cluster->heap, new_cap * sizeof(*grown),
                                      "simnet_cluster_heap");
        if (!grown)
            LOG_FAIL("simnet.cluster", "OOM growing ready heap to %zu", new_cap);
        cluster->heap = grown;
        cluster->heap_cap = new_cap;
    }

    size_t i = cluster->heap_count++;
    cluster->heap[i] = pool_idx;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!simnet_cluster_heap_less(cluster, cluster->heap[i],
                                      cluster->heap[parent]))
            break;
        uint32_t tmp = cluster->heap[i];
        cluster->heap[i] = cluster->heap[parent];
        cluster->heap[parent] = tmp;
        i = parent;
    }
    return true;
}

static uint32_t simnet_cluster_heap_pop_min(struct simnet_cluster *cluster)
{
    uint32_t top = cluster->heap[0];
    cluster->heap_count--;
    if (cluster->heap_count > 0) {
        cluster->heap[0] = cluster->heap[cluster->heap_count];
        size_t i = 0;
        size_t n = cluster->heap_count;
        for (;;) {
            size_t l = 2 * i + 1;
            size_t r = 2 * i + 2;
            size_t m = i;
            if (l < n && simnet_cluster_heap_less(cluster, cluster->heap[l],
                                                  cluster->heap[m]))
                m = l;
            if (r < n && simnet_cluster_heap_less(cluster, cluster->heap[r],
                                                  cluster->heap[m]))
                m = r;
            if (m == i)
                break;
            uint32_t tmp = cluster->heap[i];
            cluster->heap[i] = cluster->heap[m];
            cluster->heap[m] = tmp;
            i = m;
        }
    }
    return top;
}

/* ── waiting map ───────────────────────────────────────────────────── */

static uint32_t simnet_cluster_wait_bucket(size_t to_node,
                                           const struct uint256 *hash)
{
    uint64_t x = (uint64_t)to_node * 0x9e3779b97f4a7c15ULL;
    x ^= uint256_get_cheap_hash(hash) + ((uint64_t)to_node << 6) +
         0x9e3779b97f4a7c15ULL;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 29;
    return (uint32_t)(x & (SIM_CLUSTER_WAIT_BUCKETS - 1u));
}

static bool simnet_cluster_wait_register(struct simnet_cluster *cluster,
                                         uint32_t pool_idx, size_t to_node,
                                         const struct uint256 *hash)
{
    if (cluster->reg_count >= cluster->reg_cap) {
        size_t new_cap = cluster->reg_cap ? cluster->reg_cap * 2 : 64;
        struct simnet_wait_reg *grown =
            zcl_realloc(cluster->regs, new_cap * sizeof(*grown),
                        "simnet_cluster_wait_regs");
        if (!grown)
            LOG_FAIL("simnet.cluster", "OOM growing wait regs to %zu", new_cap);
        cluster->regs = grown;
        cluster->reg_cap = new_cap;
    }

    uint32_t b = simnet_cluster_wait_bucket(to_node, hash);
    uint32_t ri = (uint32_t)cluster->reg_count++;
    cluster->regs[ri].pool_idx = pool_idx;
    cluster->regs[ri].next = cluster->bucket_head[b];
    cluster->bucket_head[b] = (int32_t)ri;
    return true;
}

/* A block just became present on `node`. Re-check every pending delivery
 * registered as waiting on (node, hash) and promote the now-ready ones into
 * the ready heap. Registrations are left in place (lazy); an already-promoted
 * entry is skipped by its state. */
static bool simnet_cluster_promote_on_add(struct simnet_cluster *cluster,
                                          size_t node,
                                          const struct uint256 *hash)
{
    uint32_t b = simnet_cluster_wait_bucket(node, hash);
    int32_t ri = cluster->bucket_head[b];
    while (ri >= 0) {
        uint32_t pi = cluster->regs[ri].pool_idx;
        int32_t next = cluster->regs[ri].next;
        if (cluster->pool[pi].state == SIM_PEND_WAITING &&
            simnet_cluster_delivery_ready(cluster, &cluster->pool[pi].d)) {
            cluster->pool[pi].state = SIM_PEND_READY;
            cluster->waiting_count--;
            if (!simnet_cluster_heap_push(cluster, pi))
                return false;
        }
        ri = next;
    }
    return true;
}

/* ── scheduling ────────────────────────────────────────────────────── */

static bool simnet_cluster_pool_reserve(struct simnet_cluster *cluster)
{
    if (cluster->pool_count < cluster->pool_cap)
        return true;
    size_t new_cap = cluster->pool_cap ? cluster->pool_cap * 2 : 32;
    struct simnet_pending *grown =
        zcl_realloc(cluster->pool, new_cap * sizeof(*grown),
                    "simnet_cluster_pool");
    if (!grown)
        LOG_FAIL("simnet.cluster", "OOM growing pending pool to %zu", new_cap);
    cluster->pool = grown;
    cluster->pool_cap = new_cap;
    return true;
}

/* Insert one delivery: evaluate readiness now (the receiver's state is
 * known), pushing straight to the ready heap if ready, else registering it in
 * the waiting map under the hash(es) it is missing. */
static bool simnet_cluster_schedule(struct simnet_cluster *cluster,
                                    const struct simnet_delivery *d)
{
    if (!simnet_cluster_pool_reserve(cluster))
        return false;

    uint32_t pi = (uint32_t)cluster->pool_count++;
    cluster->pool[pi].d = *d;

    if (simnet_cluster_delivery_ready(cluster, &cluster->pool[pi].d)) {
        cluster->pool[pi].state = SIM_PEND_READY;
        return simnet_cluster_heap_push(cluster, pi);
    }

    cluster->pool[pi].state = SIM_PEND_WAITING;
    cluster->waiting_count++;

    const struct block *src =
        simnet_chain_block_by_hash(cluster->nodes[d->from_node],
                                   &d->block_hash);
    if (!src)
        LOG_FAIL("simnet.cluster", "schedule: source block missing");

    /* Not ready ⟹ receiver holds neither the block nor its parent. Register
     * under each missing hash; whichever arrives first fires the promotion. */
    if (!simnet_chain_has_block(cluster->nodes[d->to_node], &d->block_hash)) {
        if (!simnet_cluster_wait_register(cluster, pi, d->to_node,
                                          &d->block_hash))
            return false;
    }
    if (!simnet_chain_has_block(cluster->nodes[d->to_node],
                                &src->header.hashPrevBlock)) {
        if (!simnet_cluster_wait_register(cluster, pi, d->to_node,
                                          &src->header.hashPrevBlock))
            return false;
    }
    return true;
}

static void simnet_cluster_reset_scheduler(struct simnet_cluster *cluster)
{
    cluster->pool_count = 0;
    cluster->heap_count = 0;
    cluster->reg_count = 0;
    cluster->waiting_count = 0;
    memset(cluster->bucket_head, 0xFF, sizeof(cluster->bucket_head));
}

/* ── enqueue ───────────────────────────────────────────────────────── */

static bool simnet_cluster_enqueue(struct simnet_cluster *cluster,
                                   size_t from_node, size_t to_node,
                                   const struct uint256 *block_hash,
                                   uint64_t first_seen)
{
    if (!cluster || !block_hash)
        LOG_FAIL("simnet.cluster", "invalid enqueue request");

    uint64_t latency =
        SIM_CLUSTER_MIN_LATENCY_US +
        (rng_u64() % SIM_CLUSTER_LATENCY_SPAN_US);
    uint64_t reorder = rng_u64() % 17u;
    uint64_t deliver_us = simnet_cluster_now_us() + latency + reorder;

    struct simnet_delivery d;
    memset(&d, 0, sizeof(d));
    d.from_node = from_node;
    d.to_node = to_node;
    d.block_hash = *block_hash;
    d.first_seen = first_seen;
    d.deliver_us = deliver_us;
    d.seq = cluster->next_delivery_seq++;

    if (!simnet_cluster_schedule(cluster, &d))
        return false;

    struct simnet_delivery_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.from_node = from_node;
    rec.to_node = to_node;
    rec.deliver_us = deliver_us;
    memcpy(rec.block_hash, block_hash->data, sizeof(rec.block_hash));
    int rc = seed_tape_inject(cluster->tape, SIM_CLUSTER_EVENT_BLOCK_ENQUEUED,
                              &rec, sizeof(rec));
    if (rc != 0)
        LOG_FAIL("simnet.cluster", "seed_tape_inject failed rc=%d", rc);
    return true;
}

/* ── delivery ──────────────────────────────────────────────────────── */

static bool simnet_cluster_deliver_pool(struct simnet_cluster *cluster,
                                        uint32_t pi)
{
    struct simnet_delivery d = cluster->pool[pi].d;
    const struct block *block =
        simnet_chain_block_by_hash(cluster->nodes[d.from_node],
                                   &d.block_hash);
    if (!block)
        LOG_FAIL("simnet.cluster", "source block missing during delivery");

    uint64_t now = simnet_cluster_now_us();
    if (d.deliver_us > now) {
        uint64_t delta = d.deliver_us - now;
        if (delta > (uint64_t)INT64_MAX)
            LOG_FAIL("simnet.cluster", "delivery delta too large");
        int rc = seed_tape_advance(cluster->tape, (int64_t)delta);
        if (rc != 0)
            LOG_FAIL("simnet.cluster", "seed_tape_advance failed rc=%d", rc);
    }

    bool added = false;
    if (!simnet_chain_has_block(cluster->nodes[d.to_node], &d.block_hash)) {
        if (!simnet_chain_accept_block(cluster->nodes[d.to_node], block,
                                       d.first_seen)) {
            LOG_FAIL("simnet.cluster", "receiver rejected delivered block");
        }
        added = true;
    }
    simnet_cluster_fp_mix(cluster, &d);
    cluster->pool[pi].state = SIM_PEND_DONE;

    /* The receiver now holds d.block_hash; wake any deliveries that were
     * waiting on it (as their own block or as a parent). A duplicate delivery
     * adds nothing, so nothing to promote. */
    if (added && !simnet_cluster_promote_on_add(cluster, d.to_node,
                                                &d.block_hash))
        return false;
    return true;
}

struct simnet_cluster *simnet_cluster_init(size_t node_count, uint64_t seed)
{
    if (node_count == 0)
        LOG_NULL("simnet.cluster", "node_count must be positive");

    struct simnet_cluster *cluster =
        zcl_calloc(1, sizeof(*cluster), "simnet_cluster");
    if (!cluster)
        LOG_NULL("simnet.cluster", "OOM allocating cluster");

    cluster->tape = seed_tape_open(seed, 0);
    if (!cluster->tape) {
        free(cluster);
        LOG_NULL("simnet.cluster", "seed_tape_open failed");
    }
    seed_tape_install(cluster->tape);

    cluster->node_count = node_count;
    cluster->nodes = zcl_calloc(node_count, sizeof(*cluster->nodes),
                                "simnet_cluster_nodes");
    if (!cluster->nodes) {
        simnet_cluster_free(cluster);
        LOG_NULL("simnet.cluster", "OOM allocating %zu nodes", node_count);
    }

    for (size_t i = 0; i < node_count; i++) {
        uint64_t tag = rng_u64() ^ ((uint64_t)i * 0x9e3779b97f4a7c15ULL);
        cluster->nodes[i] = simnet_chain_create(tag);
        if (!cluster->nodes[i]) {
            simnet_cluster_free(cluster);
            LOG_NULL("simnet.cluster", "node create failed index=%zu", i);
        }
    }

    memset(cluster->bucket_head, 0xFF, sizeof(cluster->bucket_head));
    cluster->next_first_seen = 1;
    cluster->next_delivery_seq = 1;
    cluster->delivery_fingerprint = 1469598103934665603ULL;
    return cluster;
}

void simnet_cluster_free(struct simnet_cluster *cluster)
{
    if (!cluster)
        return;
    if (cluster->nodes) {
        for (size_t i = 0; i < cluster->node_count; i++)
            simnet_chain_free(cluster->nodes[i]);
        free(cluster->nodes);
    }
    free(cluster->pool);
    free(cluster->heap);
    free(cluster->regs);
    if (cluster->tape) {
        seed_tape_uninstall();
        seed_tape_close(cluster->tape);
    }
    free(cluster);
}

bool simnet_cluster_mint_on(struct simnet_cluster *cluster, size_t node_id,
                            struct uint256 *out_block_hash)
{
    if (!cluster || node_id >= cluster->node_count)
        LOG_FAIL("simnet.cluster", "invalid mint node=%zu", node_id);
    uint64_t first_seen = cluster->next_first_seen++;
    struct uint256 minted;
    if (!simnet_chain_mint(cluster->nodes[node_id], first_seen, &minted))
        LOG_FAIL("simnet.cluster", "mint failed node=%zu", node_id);
    if (out_block_hash)
        *out_block_hash = minted;

    /* A fresh local block can satisfy a delivery already queued TO this node
     * (as its own block or as a needed parent), exactly as the old rescan
     * would observe on the next deliver_pending. Promote incrementally. */
    if (!simnet_cluster_promote_on_add(cluster, node_id, &minted))
        return false;
    return true;
}

bool simnet_cluster_broadcast(struct simnet_cluster *cluster,
                              size_t from_node,
                              const struct uint256 *block_hash)
{
    return simnet_cluster_broadcast_except(cluster, from_node, block_hash,
                                           NULL);
}

bool simnet_cluster_broadcast_except(struct simnet_cluster *cluster,
                                     size_t from_node,
                                     const struct uint256 *block_hash,
                                     const bool *exclude_to)
{
    if (!cluster || from_node >= cluster->node_count || !block_hash)
        LOG_FAIL("simnet.cluster", "invalid broadcast request");
    if (!simnet_chain_has_block(cluster->nodes[from_node], block_hash))
        LOG_FAIL("simnet.cluster", "broadcast block unknown");

    uint64_t first_seen = 0;
    if (!simnet_chain_block_first_seen(cluster->nodes[from_node], block_hash,
                                       &first_seen)) {
        return false;
    }

    for (size_t to = 0; to < cluster->node_count; to++) {
        if (to == from_node)
            continue;
        if (exclude_to && exclude_to[to])
            continue;
        if (!simnet_cluster_enqueue(cluster, from_node, to, block_hash,
                                    first_seen))
            return false;
    }
    return true;
}

bool simnet_cluster_relay_subset(struct simnet_cluster *cluster,
                                 size_t from_node,
                                 const struct uint256 *block_hash,
                                 const size_t *targets, size_t target_count)
{
    if (!cluster || from_node >= cluster->node_count || !block_hash ||
        (target_count > 0 && !targets))
        LOG_FAIL("simnet.cluster", "invalid relay-subset request");
    if (!simnet_chain_has_block(cluster->nodes[from_node], block_hash))
        LOG_FAIL("simnet.cluster", "relay-subset block unknown");

    uint64_t first_seen = 0;
    if (!simnet_chain_block_first_seen(cluster->nodes[from_node], block_hash,
                                       &first_seen)) {
        return false;
    }

    for (size_t i = 0; i < target_count; i++) {
        size_t to = targets[i];
        if (to == from_node)
            continue;
        if (to >= cluster->node_count)
            LOG_FAIL("simnet.cluster", "relay-subset target %zu out of range",
                     to);
        if (!simnet_cluster_enqueue(cluster, from_node, to, block_hash,
                                    first_seen))
            return false;
    }
    return true;
}

bool simnet_cluster_deliver_pending(struct simnet_cluster *cluster)
{
    if (!cluster)
        LOG_FAIL("simnet.cluster", "NULL cluster deliver");

    while (cluster->heap_count > 0) {
        uint32_t pi = simnet_cluster_heap_pop_min(cluster);
        if (cluster->pool[pi].state != SIM_PEND_READY)
            continue;  /* defensive: never expected (each entry pushed once) */
        if (!simnet_cluster_deliver_pool(cluster, pi))
            return false;
    }

    if (cluster->waiting_count > 0)
        LOG_FAIL("simnet.cluster", "delivery queue stuck waiting=%zu",
                 cluster->waiting_count);

    /* Fully drained — reclaim the scheduler for the next batch. The global
     * delivery seq is preserved so FIFO tie-break stays monotonic. */
    simnet_cluster_reset_scheduler(cluster);
    return true;
}

bool simnet_cluster_tip_hash(const struct simnet_cluster *cluster,
                             size_t node_id, struct uint256 *out)
{
    if (!cluster || node_id >= cluster->node_count || !out)
        LOG_FAIL("simnet.cluster", "invalid tip hash node=%zu", node_id);
    return simnet_chain_tip_hash(cluster->nodes[node_id], out);
}

bool simnet_cluster_coins_digest(struct simnet_cluster *cluster,
                                 size_t node_id,
                                 struct utxo_commitment *out)
{
    if (!cluster || node_id >= cluster->node_count || !out)
        LOG_FAIL("simnet.cluster", "invalid digest node=%zu", node_id);
    return simnet_chain_coins_digest(cluster->nodes[node_id], out);
}

bool simnet_cluster_tip_height(const struct simnet_cluster *cluster,
                               size_t node_id, int32_t *out_height)
{
    if (!cluster || node_id >= cluster->node_count || !out_height)
        LOG_FAIL("simnet.cluster", "invalid tip height node=%zu", node_id);
    return simnet_chain_tip_height(cluster->nodes[node_id], out_height);
}

uint64_t simnet_cluster_delivery_fingerprint(
    const struct simnet_cluster *cluster)
{
    return cluster ? cluster->delivery_fingerprint : 0;
}
