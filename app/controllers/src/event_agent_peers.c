/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "event_agent_peers.h"

#include "net/protocol.h"
#include "net/version.h"
#include "platform/time_compat.h"

#include <limits.h>
#include <stdatomic.h>

static _Atomic int g_agent_peer_cache_valid;
static _Atomic int g_agent_peer_cache_total;
static _Atomic int g_agent_peer_cache_best_height;
static _Atomic int g_agent_peer_cache_magicbean;
static _Atomic int g_agent_peer_cache_zclassic23;
static _Atomic int64_t g_agent_peer_cache_sampled_at;

static int agent_peer_clamp_size_to_int(size_t value)
{
    if (value > INT_MAX)
        return INT_MAX;
    return (int)value;
}

static void agent_peer_store_cache(int peer_count,
                                   int peer_best_height,
                                   int magicbean_peer_count,
                                   int zclassic23_peer_count,
                                   int64_t sampled_at)
{
    atomic_store(&g_agent_peer_cache_total, peer_count);
    atomic_store(&g_agent_peer_cache_best_height, peer_best_height);
    atomic_store(&g_agent_peer_cache_magicbean, magicbean_peer_count);
    atomic_store(&g_agent_peer_cache_zclassic23, zclassic23_peer_count);
    atomic_store(&g_agent_peer_cache_sampled_at, sampled_at);
    atomic_store(&g_agent_peer_cache_valid, 1);
}

static bool agent_peer_load_cache(int *peer_count,
                                  int *peer_best_height,
                                  int *magicbean_peer_count,
                                  int *zclassic23_peer_count,
                                  int64_t *sampled_at)
{
    if (!atomic_load(&g_agent_peer_cache_valid))
        return false; // raw-return-ok:cache-miss
    *peer_count = atomic_load(&g_agent_peer_cache_total);
    *peer_best_height = atomic_load(&g_agent_peer_cache_best_height);
    *magicbean_peer_count = atomic_load(&g_agent_peer_cache_magicbean);
    *zclassic23_peer_count = atomic_load(&g_agent_peer_cache_zclassic23);
    *sampled_at = atomic_load(&g_agent_peer_cache_sampled_at);
    return true;
}

void agent_peer_snapshot_collect(struct agent_peer_snapshot *out,
                                 struct connman *cm)
{
    struct agent_peer_snapshot empty = {
        .peer_best_height = -1,
        .age_seconds = -1,
    };
    if (!out)
        return;
    *out = empty;
    if (!cm)
        return;

    int peer_count = 0;
    int peer_best_height = -1;
    int magicbean_peer_count = 0;
    int zclassic23_peer_count = 0;
    int64_t sampled_at = 0;

    if (zcl_mutex_trylock(&cm->manager.cs_nodes)) {
        peer_count = agent_peer_clamp_size_to_int(cm->manager.num_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            if (!node || node->disconnect)
                continue;
            if (node->state < PEER_HANDSHAKE_COMPLETE)
                continue;
            if ((node->services & NODE_NETWORK) != 0 &&
                node->starting_height > peer_best_height)
                peer_best_height = node->starting_height;
            bool is_mb = false, is_z23 = false;
            msg_version_classify_peer(node->sub_ver, node->services,
                                      &is_mb, &is_z23);
            if (is_mb)
                magicbean_peer_count++;
            if (is_z23)
                zclassic23_peer_count++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);
        sampled_at = (int64_t)platform_time_wall_time_t();
        agent_peer_store_cache(peer_count, peer_best_height,
                               magicbean_peer_count,
                               zclassic23_peer_count,
                               sampled_at);
        out->available = true;
        out->stale = false;
        out->age_seconds = 0;
    } else if (agent_peer_load_cache(&peer_count, &peer_best_height,
                                     &magicbean_peer_count,
                                     &zclassic23_peer_count,
                                     &sampled_at)) {
        int64_t now = (int64_t)platform_time_wall_time_t();
        out->available = true;
        out->stale = true;
        out->age_seconds = now >= sampled_at ? now - sampled_at : -1;
        out->warning_reason = "peer_snapshot_busy";
    } else {
        out->available = false;
        out->stale = true;
        out->age_seconds = -1;
        out->warning_reason = "peer_snapshot_unavailable";
        return;
    }

    out->peer_count = peer_count > 0 ? (size_t)peer_count : 0;
    out->peer_best_height = peer_best_height;
    out->magicbean_peer_count =
        magicbean_peer_count > 0 ? (size_t)magicbean_peer_count : 0;
    out->zclassic_c23_peer_count =
        zclassic23_peer_count > 0 ? (size_t)zclassic23_peer_count : 0;
}
