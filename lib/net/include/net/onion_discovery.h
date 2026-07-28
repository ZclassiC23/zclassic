/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Onion peer discovery contract for net-layer bootstrapping. */

#ifndef ZCL_NET_ONION_DISCOVERY_H
#define ZCL_NET_ONION_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* ── the rich endpoint, alongside the narrow peer ───────────────────
 *
 * `struct onion_peer` is a hostname and a height: no port, no
 * services, no expiry, no provenance. It stays exactly as it is —
 * every existing producer and consumer keeps compiling and keeps
 * behaving identically — and this richer type sits BESIDE it for the
 * sources that actually know more.
 *
 * The one that does today is the signed endpoint record (zid/zendp.h,
 * body tag "ZIDE"): a document the peer SIGNED, whose signing key is
 * resolved against the on-chain identity projection
 * (vcs/zendp_swarm.h). Such a record carries onion + clearnet
 * addresses with real ports, a services bitmask, a claimed height, its
 * own signed expiry, and the identity + anchor height that vouch for
 * it.
 *
 * ── WHAT A RECORD IS, AND IS NOT ──────────────────────────────────
 * A directory record is a HINT ABOUT WHERE TO LOOK, never proof of who
 * is there (docs/work/NAT_AND_ONION_TRANSPORT.md). Even a fully
 * verified, chain-anchored record does NOT prove that whoever answers
 * at that address holds that key: binding the SESSION to a key needs
 * the Noise v2 transport (-v2transport, net/connman.h), which is
 * default OFF because every peer on the live network speaks v1 today.
 *
 * So the discipline, structurally: endpoints from records may only
 * ever ADD a place to try. They must never remove, rank below, or
 * exclude any peer from any other source. A poisoned record then costs
 * at most one wasted connection attempt. The only sanctioned way for a
 * directory to influence selection at all is
 * addrman_set_reputation_weight (lib/net/src/addrman.c), which is
 * bounded to a [1.0, 4.0] dial-chance multiplier and structurally
 * cannot exclude. */

enum onion_peer_provenance {
    /* Wallet OP_RETURN scrape: unsigned, no expiry, no freshness. */
    ONION_PROV_UNSIGNED = 0,
    /* Signed document, verified against a key the CALLER supplied —
     * fresh and authentic, but not bound to the chain (zdesc). */
    ONION_PROV_SIGNED,
    /* Signed document whose key resolved to an ACTIVE on-chain anchor
     * (zendp). The strongest provenance that exists today, and still
     * only a hint about where to look. */
    ONION_PROV_ANCHORED,
};

const char *onion_peer_provenance_string(enum onion_peer_provenance p);

struct onion_endpoint {
    char     hostname[64];   /* "" when the record is clearnet-only */
    uint16_t onion_port;
    uint8_t  ipv4[4];        /* all-zero when absent */
    uint16_t ipv4_port;
    uint8_t  ipv6[16];       /* all-zero when absent */
    uint16_t ipv6_port;
    uint64_t services;       /* the P2P nServices bitmask the peer claims */
    int      height;         /* best height the peer claims */
    uint64_t expiry;         /* unix seconds; the record's OWN signed end */
    uint64_t seq;            /* monotonic per identity; supersede authority */
    uint8_t  master_pubkey[32]; /* all-zero when the source is unsigned */
    int32_t  anchor_height;  /* height that anchored the key; 0 if none */
    enum onion_peer_provenance provenance;
};

/* True when the endpoint is inside its own validity window and names at
 * least one address. An endpoint with expiry == 0 has no signed window
 * (an unsigned source) and is live by definition — freshness is
 * per-record, and there is deliberately no global refresh clock. */
bool onion_endpoint_live(const struct onion_endpoint *ep, uint64_t now_unix);

/* Narrowing adapter: the rich endpoint as the old two-field peer, so
 * every existing consumer keeps working unchanged. False when the
 * endpoint names no valid v3 onion hostname — the clearnet half of a
 * record cannot be expressed as an onion_peer and is dropped from this
 * path (see the seam note in config/src/boot_endpoint_records.c). */
bool onion_endpoint_to_peer(const struct onion_endpoint *ep,
                            struct onion_peer *out);

/* Bulk form: project `n` endpoints into out[0..max), skipping any that
 * are not live and any whose hostname fails the v3 rule. *rejected_out,
 * when non-NULL, receives the count dropped for being malformed or
 * expired — an endpoint that is simply clearnet-only is not an error
 * and is not counted. Returns how many were written. */
int onion_endpoints_to_peers(const struct onion_endpoint *eps, int n,
                             struct onion_peer *out, size_t max,
                             uint64_t now_unix, int *rejected_out);

#endif
