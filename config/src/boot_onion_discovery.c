/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The config/-side implementation of lib/net's signed discovery port
 * (net/onion_discovery.h onion_signed_peer_source_fn). lib/net is
 * ranked below lib/vcs in config/lib_module_order.def and may not
 * include it, so the dependency is inverted here — the same shape
 * net_runtime_port.h and node_db_runtime.h use.
 *
 * Each hostname comes from a descriptor whose signature, validity
 * window, and monotonic seq were checked when it was accepted
 * (vcs/zdesc_swarm.h) — against a caller-supplied master key, NOT
 * against a chain anchor. That binding is a separate, open seam
 * (lib/vcs/src/zdesc_swarm.c, CHAIN-BINDING SEAM); nothing here claims
 * these peers are chain-verified.
 *
 * `height` is 0 because a descriptor carries no height; both consumers
 * treat it as display/storage only.
 *
 * On a node where nothing has published a descriptor, the directory is
 * empty and this returns 0 — zero behaviour change. */

#include "config/boot_onion_discovery.h"

#include "platform/time_compat.h"
#include "vcs/zdesc_swarm.h"

#include <stdio.h>

#define BOOT_SIGNED_PEERS_MAX 32u

static int boot_signed_onion_peers(void *unused_ctx, struct onion_peer *out,
                                   size_t max)
{
    (void)unused_ctx;
    if (!out || max == 0)
        return 0;
    if (max > BOOT_SIGNED_PEERS_MAX)
        max = BOOT_SIGNED_PEERS_MAX;

    char hosts[BOOT_SIGNED_PEERS_MAX][ZDESC_ONION_LEN + 1];
    size_t n = zdesc_global_onions((uint64_t)platform_time_wall_unix(), hosts,
                                   max);
    for (size_t i = 0; i < n; i++) {
        snprintf(out[i].hostname, sizeof(out[i].hostname), "%s", hosts[i]);
        out[i].height = 0;
    }
    return (int)n;
}

void boot_onion_discovery_register(onion_blog_serve_fn blog_serve,
                                   onion_peer_discover_fn peer_discover)
{
    onion_service_set_app_handlers(blog_serve, peer_discover);
    /* Additive: signed descriptors are asked first, the unsigned scrape
     * still fills the remaining capacity. */
    onion_service_set_signed_peer_source(boot_signed_onion_peers, NULL);
}
