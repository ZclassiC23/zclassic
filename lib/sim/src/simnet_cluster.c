/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Deterministic in-memory multi-node simnet cluster.
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

struct simnet_delivery {
    size_t from_node;
    size_t to_node;
    struct uint256 block_hash;
    uint64_t first_seen;
    uint64_t deliver_us;
    uint64_t seq;
};

struct simnet_cluster {
    size_t node_count;
    struct simnet_chain **nodes;
    seed_tape_t *tape;
    struct simnet_delivery *queue;
    size_t queue_count;
    size_t queue_cap;
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

static bool simnet_cluster_ensure_queue(struct simnet_cluster *cluster)
{
    if (!cluster)
        LOG_FAIL("simnet.cluster", "NULL cluster queue");
    if (cluster->queue_count < cluster->queue_cap)
        return true;

    size_t new_cap = cluster->queue_cap ? cluster->queue_cap * 2 : 16;
    struct simnet_delivery *grown =
        zcl_realloc(cluster->queue, new_cap * sizeof(*grown),
                    "simnet_cluster_queue");
    if (!grown)
        LOG_FAIL("simnet.cluster", "OOM growing delivery queue to %zu",
                 new_cap);
    cluster->queue = grown;
    cluster->queue_cap = new_cap;
    return true;
}

static bool simnet_cluster_enqueue(struct simnet_cluster *cluster,
                                   size_t from_node, size_t to_node,
                                   const struct uint256 *block_hash,
                                   uint64_t first_seen)
{
    if (!cluster || !block_hash)
        LOG_FAIL("simnet.cluster", "invalid enqueue request");
    if (!simnet_cluster_ensure_queue(cluster))
        return false;

    uint64_t latency =
        SIM_CLUSTER_MIN_LATENCY_US +
        (rng_u64() % SIM_CLUSTER_LATENCY_SPAN_US);
    uint64_t reorder = rng_u64() % 17u;
    uint64_t deliver_us = simnet_cluster_now_us() + latency + reorder;

    struct simnet_delivery *d = &cluster->queue[cluster->queue_count++];
    memset(d, 0, sizeof(*d));
    d->from_node = from_node;
    d->to_node = to_node;
    d->block_hash = *block_hash;
    d->first_seen = first_seen;
    d->deliver_us = deliver_us;
    d->seq = cluster->next_delivery_seq++;

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

static bool simnet_cluster_find_next_ready(
    const struct simnet_cluster *cluster, size_t *out_idx)
{
    if (!cluster || !out_idx)
        LOG_FAIL("simnet.cluster", "invalid ready scan");

    bool found = false;
    size_t best = 0;
    for (size_t i = 0; i < cluster->queue_count; i++) {
        if (!simnet_cluster_delivery_ready(cluster, &cluster->queue[i]))
            continue;
        if (!found ||
            simnet_cluster_delivery_less(&cluster->queue[i],
                                         &cluster->queue[best])) {
            best = i;
            found = true;
        }
    }
    if (!found)
        return false;
    *out_idx = best;
    return true;
}

static void simnet_cluster_remove_delivery(struct simnet_cluster *cluster,
                                           size_t idx)
{
    if (!cluster || idx >= cluster->queue_count)
        return;
    cluster->queue[idx] = cluster->queue[cluster->queue_count - 1];
    cluster->queue_count--;
}

static bool simnet_cluster_deliver_one(struct simnet_cluster *cluster,
                                       size_t idx)
{
    if (!cluster || idx >= cluster->queue_count)
        LOG_FAIL("simnet.cluster", "invalid delivery index=%zu", idx);

    struct simnet_delivery d = cluster->queue[idx];
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

    if (!simnet_chain_has_block(cluster->nodes[d.to_node], &d.block_hash)) {
        if (!simnet_chain_accept_block(cluster->nodes[d.to_node], block,
                                       d.first_seen)) {
            LOG_FAIL("simnet.cluster", "receiver rejected delivered block");
        }
    }
    simnet_cluster_fp_mix(cluster, &d);
    simnet_cluster_remove_delivery(cluster, idx);
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
    free(cluster->queue);
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
    if (!simnet_chain_mint(cluster->nodes[node_id], first_seen,
                           out_block_hash)) {
        LOG_FAIL("simnet.cluster", "mint failed node=%zu", node_id);
    }
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

bool simnet_cluster_deliver_pending(struct simnet_cluster *cluster)
{
    if (!cluster)
        LOG_FAIL("simnet.cluster", "NULL cluster deliver");

    while (cluster->queue_count > 0) {
        size_t idx = 0;
        if (!simnet_cluster_find_next_ready(cluster, &idx)) {
            LOG_FAIL("simnet.cluster", "delivery queue stuck count=%zu",
                     cluster->queue_count);
        }
        if (!simnet_cluster_deliver_one(cluster, idx))
            return false;
    }
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
