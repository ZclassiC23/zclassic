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

#endif
