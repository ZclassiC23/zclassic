/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Onion peer discovery contract for net-layer bootstrapping. */

#ifndef ZCL_NET_ONION_DISCOVERY_H
#define ZCL_NET_ONION_DISCOVERY_H

#include <stddef.h>

struct onion_peer {
    char hostname[64];
    int height;
};

typedef int (*onion_peer_discover_fn)(const char *datadir,
                                      struct onion_peer *out,
                                      size_t max);

/* A SIGNED discovery source — a registered port, not a second scraper.
 *
 * The original source (blog_discover_onion_peers) scrapes wallet-local
 * ZSLP OP_RETURNs for a trailing ".onion": wallet-scoped, unsigned, no
 * expiry, no freshness. It stays wired; peer discovery is
 * liveness-critical and its fallbacks are deliberate.
 *
 * This port lets a source that CAN prove freshness contribute
 * alongside it: the zdesc signed-descriptor directory
 * (vcs/zdesc_swarm.h), whose entries carry a signature over a validity
 * window and a monotonic seq. lib/net is ranked below lib/vcs
 * (config/lib_module_order.def) and may not include it, so the
 * implementation is registered from config/ — the same inversion
 * net_runtime_port.h and node_db_runtime.h use.
 *
 * Contract: fill at most `max` entries, return the count, never block
 * on I/O or the network, and return 0 when there is nothing to say.
 * Hostnames are re-validated by the net layer regardless of source. */
typedef int (*onion_signed_peer_source_fn)(void *ctx,
                                           struct onion_peer *out,
                                           size_t max);

#endif
