/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * network_crawler — the whole-network OBSERVATORY.
 *
 * The own-peer network_monitor only ever sees THIS node's handful of
 * connections; it structurally cannot answer "what does the ENTIRE reachable
 * ZClassic P2P network look like?" This service closes that gap Bitnodes-style:
 * a supervised worker walks the full local address table (addrman_get_addr),
 * opens SHORT-LIVED measurement sockets to a bounded, rate-limited batch of
 * addresses per round OUTSIDE the node's connman, performs only a version/verack
 * handshake, records {addr, onion-vs-clearnet, version, subver, services,
 * best_height, latency_us, reachable, last_probe_us}, then disconnects
 * immediately. It NEVER relays or syncs on these sockets.
 *
 * From that census it folds network-wide facts (reachable count, version
 * histogram, height distribution, onion/clearnet split) and a WHOLE-NETWORK
 * eclipse signal: if our connected peers cluster on a height that is a small
 * minority in the wider crawled network, that is an eclipse indicator the
 * own-peer monitor cannot see.
 *
 * ON BY DEFAULT (omniscience directive: the node obsesses about knowing the
 * whole network). It is opt-OUT: `-netcrawl=0` / `-nonetcrawl` or
 * ZCL_NETWORK_CRAWLER=0 fully disables the dialer. The rate limits below
 * (bounded probe batch on short-lived measurement sockets outside connman)
 * keep always-on cost tiny. When off, the supervised worker still registers
 * and idles (named degradation, never a boot failure).
 *
 * The per-address dialer is behind an injectable probe_fn seam so the census
 * fold is unit-tested hermetically with synthetic results — no real sockets.
 */

#ifndef ZCL_SERVICES_NETWORK_CRAWLER_H
#define ZCL_SERVICES_NETWORK_CRAWLER_H

#include "net/netaddr.h"
#include "util/result.h"

#include <stdbool.h>
#include <stdint.h>

struct addr_man;
struct json_value;

/* ── Pedantic bounds (every one hard-capped) ─────────────────────────── */
enum {
    NCRAWL_MAX_CENSUS     = 1024, /* total bounded census-table cap */
    NCRAWL_MAX_PER_ROUND  = 64,   /* max addresses dialed per round */
    NCRAWL_MAX_CONCURRENT = 8,    /* HARD cap on concurrent in-flight dials */
    NCRAWL_MAX_VERSIONS   = 16,   /* distinct subver histogram buckets */
    NCRAWL_TOPN_VERSIONS  = 8,    /* reported top-N subver buckets */
    NCRAWL_ADDR_MAX       = 96,   /* addr string cap */
    NCRAWL_SUBVER_MAX     = 128,  /* trimmed subver cap */
    NCRAWL_ECLIPSE_MIN    = 4,    /* reachable nodes before eclipse can fire */
};

#define NCRAWL_CONNECT_TIMEOUT_MS_DEFAULT   3000
#define NCRAWL_HANDSHAKE_TIMEOUT_MS_DEFAULT 4000
#define NCRAWL_ROUND_INTERVAL_SECS_DEFAULT  60

/* One measured node in the census. reachable=false rows are retained too:
 * "we know this address, the last probe did not complete". */
struct ncrawl_probe_result {
    char     addr[NCRAWL_ADDR_MAX];
    bool     is_onion;
    bool     reachable;
    int32_t  version;
    char     subver[NCRAWL_SUBVER_MAX];
    uint64_t services;
    int64_t  best_height;
    int64_t  latency_us;
    int64_t  last_probe_us;   /* wall-unix secs of this probe */
};

struct ncrawl_version_bucket {
    char    subver[NCRAWL_SUBVER_MAX];
    int32_t count;
};

/* A folded snapshot of the whole reachable network. */
struct network_census_view {
    bool    ready;
    int64_t computed_at;
    int32_t probed;              /* census rows in the sample */
    int32_t reachable_count;     /* rows with reachable=true */
    int32_t onion_count;         /* reachable onion nodes */
    int32_t clearnet_count;      /* reachable clearnet nodes */

    /* height distribution over reachable nodes advertising a best_height */
    int32_t heights_known;
    int64_t modal_height;        /* -1 if none */
    int32_t modal_height_count;
    int64_t max_height;          /* -1 if none */
    int64_t min_height;          /* -1 if none */
    int64_t height_spread;       /* max-min, 0 if <1 height known */

    /* version histogram (top-N, bounded) */
    int32_t num_versions;
    struct ncrawl_version_bucket versions[NCRAWL_MAX_VERSIONS];

    /* whole-network eclipse signal */
    bool    eclipse_suspected;
    int64_t own_modal_height;        /* our connected-peer modal (network_monitor) */
    int64_t network_modal_height;    /* == modal_height */
    int32_t network_count_at_own_modal;
};

/* ── Pure census fold (unit-testable with synthetic probe results) ──────
 * Deterministic; no clock/IO except stamping computed_at from the
 * caller-supplied now_unix. own_modal_height is our connected-peer modal
 * height (from network_monitor), or <0 if unknown. Feeding n > NCRAWL_MAX_CENSUS
 * clamps to the cap. */
void network_census_compute(const struct ncrawl_probe_result *r, int n,
                            int64_t own_modal_height, int64_t now_unix,
                            struct network_census_view *out);

/* ── Injectable dialer seam ─────────────────────────────────────────────
 * Fill *out for a single address (always set addr/is_onion/last_probe_us;
 * reachable + version/subver/... on a completed handshake). Return true if a
 * recordable result was produced, false only on invalid args/address. */
typedef bool (*ncrawl_probe_fn)(const struct net_address *addr,
                                int connect_timeout_ms,
                                int handshake_timeout_ms,
                                struct ncrawl_probe_result *out);

/* Default REAL dialer (network_crawler_probe.c): a short-lived clearnet socket
 * + version/verack handshake, then immediate disconnect. */
bool network_crawler_default_probe(const struct net_address *addr,
                                   int connect_timeout_ms,
                                   int handshake_timeout_ms,
                                   struct ncrawl_probe_result *out);

/* ── Runtime lifecycle ──────────────────────────────────────────────── */
struct network_crawler_config {
    bool enabled;               /* ON by default; -netcrawl=0 / ZCL_NETWORK_CRAWLER=0 opts out */
    int  round_interval_secs;
    int  max_per_round;
    int  max_concurrent;
    int  connect_timeout_ms;
    int  handshake_timeout_ms;
};
void network_crawler_config_defaults(struct network_crawler_config *cfg);

/* Start/stop the supervised crawler worker. addrman is the crawl seed (the full
 * local address table); it is only ever READ. When disabled the worker still
 * registers + idles. */
struct zcl_result network_crawler_start(const struct network_crawler_config *cfg,
                                        struct addr_man *addrman);
void network_crawler_stop(void);

/* Copy the latest folded census view. false until the first fold. */
bool network_crawler_get_view(struct network_census_view *out);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
bool network_crawler_dump_state_json(struct json_value *out, const char *key);

#ifdef ZCL_TESTING
void network_crawler_test_reset(void);
void network_crawler_test_set_probe_fn(ncrawl_probe_fn fn);
void network_crawler_test_set_own_modal(int64_t h);
/* Drive one synchronous probe round over addrs[0..n) using the (injected)
 * probe_fn, honoring the per-round + concurrency caps; ingest into the bounded
 * census and refold. Returns the number of addresses actually probed. */
int  network_crawler_test_probe_round(const struct net_address *addrs, int n);
int  network_crawler_test_census_count(void);
/* Inject a folded census view (marks it ready) so the eclipse condition can be
 * unit-tested without a live crawl. */
void network_crawler_test_set_view(const struct network_census_view *v);
#endif

#endif /* ZCL_SERVICES_NETWORK_CRAWLER_H */
