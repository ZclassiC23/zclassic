/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The v3 onion hostname rule and the two-source peer merge — the one
 * home for both, so no caller re-derives either.
 *
 * Peer discovery has two independent sources and one validity rule:
 *   - the SIGNED source (zdesc descriptors, registered from config/):
 *     entries carry a signature over a validity window and a monotonic
 *     seq, so they go first when capacity is short;
 *   - the unsigned wallet scrape (blog_discover_onion_peers): still
 *     wired, because it is the only source on a node that has never
 *     seen a descriptor. Discovery is liveness-critical and its
 *     fallbacks are deliberate.
 * Both are run through the same unchanged hostname rule, and a service
 * advertising through both is merged once. */

#ifndef ZCL_NET_ONION_PEER_MERGE_H
#define ZCL_NET_ONION_PEER_MERGE_H

#include "net/onion_discovery.h"
#include <stdbool.h>
#include <stddef.h>

/* On-chain and peer-supplied hostnames are attacker-controlled. Only
 * the exact Tor v3 shape (56 base32 [a-z2-7] chars + ".onion" = 62)
 * may reach HTML, JSON, or the peer_directory table. */
bool onion_hostname_valid(const char *h);

/* Fill out[0..max) from both sources, signed first, dropping malformed
 * hostnames and duplicates. Either source may be NULL (discover also
 * needs a non-NULL datadir). *rejected_out, when non-NULL, receives the
 * count of MALFORMED hostnames dropped — a duplicate is agreement
 * between sources, not an attack, and is not counted. Returns how many
 * entries were kept. With no signed source registered this is exactly
 * the old single-source behaviour. */
int onion_peers_collect(struct onion_peer *out, size_t max,
                        onion_signed_peer_source_fn signed_source,
                        void *signed_ctx, onion_peer_discover_fn discover,
                        const char *datadir, int *rejected_out);

#endif
