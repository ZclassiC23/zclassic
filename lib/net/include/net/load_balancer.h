/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * On-chain load balancer — multiple nodes serve one site via ZSLP.
 *
 * Each replica announces itself on-chain:
 *   ZSLP SEND to token_id with .onion + capacity + version in OP_RETURN
 *
 * Clients discover replicas by scanning the chain for that token_id,
 * probe latency, and connect to the best one with failover. */

#ifndef ZCL_NET_LOAD_BALANCER_H
#define ZCL_NET_LOAD_BALANCER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct site_replica {
    char onion[68];        /* .onion address */
    uint16_t port;         /* service port (usually 80) */
    uint32_t capacity;     /* max concurrent requests advertised */
    uint32_t version;      /* content version (for consistency) */
    int32_t height;        /* block height of announcement */
    int64_t latency_us;    /* measured round-trip (0 = not probed) */
    bool reachable;        /* true after successful probe */
};

/* Discover all replicas for a site from the blockchain.
 * token_id: hex string of the ZSLP token that identifies this site.
 * Returns count of discovered replicas. */
int site_discover_replicas(const char *datadir, const char *token_id,
                            struct site_replica *out, size_t max);

/* Select the best replica from a list.
 * Prefers: reachable > unreachable, low latency, high capacity, fresh height.
 * Returns index into replicas array, or -1 if none available. */
int site_select_replica(struct site_replica *replicas, int count);

/* Announce this node as a replica for a site.
 * Builds and broadcasts a ZSLP SEND tx with .onion + metadata in OP_RETURN.
 * Returns true if the tx was broadcast. */
bool site_announce_replica(const char *datadir,
                            const char *token_id,
                            const char *onion_addr,
                            uint32_t capacity,
                            uint32_t content_version);

/* Probe a replica's reachability and measure latency.
 * Connects via Tor circuit, sends a ping, measures RTT.
 * Updates replica->latency_us and replica->reachable. */
bool site_probe_replica(struct site_replica *replica);

/* Connect to the best available replica for a site.
 * Discovers replicas, probes top candidates, returns the best .onion.
 * out_onion: buffer for selected .onion address (min 68 bytes).
 * Returns true if a reachable replica was found. */
bool site_connect_best(const char *datadir, const char *token_id,
                        char *out_onion, size_t out_max);

#endif
