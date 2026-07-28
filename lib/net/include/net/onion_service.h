/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Onion service integration - bridges Tor dynhost to zclassic23 app handlers.
 *
 * This is the glue between our Tor fork's dynhost and the injected app layer.
 * When a request arrives over a Tor circuit:
 *   dynhost -> onion_service_handle_request -> app handlers -> response
 *
 * No SOCKS. No ports. No HTTP server. Just C function calls. */

#ifndef ZCL_NET_ONION_SERVICE_H
#define ZCL_NET_ONION_SERVICE_H

#include "net/onion_discovery.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef size_t (*onion_blog_serve_fn)(const char *datadir,
                                      const char *path,
                                      char *out,
                                      size_t out_len);

void onion_service_set_app_handlers(onion_blog_serve_fn blog_serve,
                                    onion_peer_discover_fn peer_discover);

/* Register the SIGNED discovery source (see net/onion_discovery.h).
 * Additive: onion peer discovery asks this source first and fills the
 * remaining capacity from the unsigned scraper, deduplicating by
 * hostname. With no source registered — the default, and the state of
 * any node until something publishes a descriptor — discovery behaves
 * exactly as it did before. Pass NULL to unregister. */
void onion_service_set_signed_peer_source(onion_signed_peer_source_fn source,
                                          void *ctx);

/* ── Directory freshness ────────────────────────────────────────────
 *
 * The peer_directory table used to be written once, at boot, and never
 * touched again: no re-probe, no expiry, and /directory.json handed out
 * up to 500 rows with no way for the reader to tell a row measured a
 * minute ago from one measured a week ago. A node up for a week served a
 * week-old list as if it were current.
 *
 * Three things fix that, and none of them may ever REMOVE a peer from
 * anyone's reach (net/onion_discovery.h: a record is a hint about where
 * to look, never proof of who is there):
 *
 *   1. `last_seen` is now maintained, not just stamped once. The
 *      refresh round re-runs the SAME discovery sources the boot path
 *      uses (onion_peers_collect) and re-registers self.
 *   2. Rows age out. A row nothing has confirmed for
 *      ONION_DIR_EXPIRE_SECS is dropped and is not served. Our own row
 *      never expires.
 *   3. Every served row carries its OWN age plus the policy constants,
 *      so the consumer judges freshness itself instead of trusting us.
 *      Nothing is silently filtered on a consumer's behalf beyond the
 *      expired rows, which are gone from the table anyway.
 *
 * Freshness is a pure function of (last_seen, now, self) so it is one
 * rule, testable without a database. */
enum onion_dir_freshness {
    ONION_DIR_FRESH = 0, /* confirmed within ONION_DIR_STALE_SECS */
    ONION_DIR_STALE,     /* served, flagged: old but not yet dropped */
    ONION_DIR_EXPIRED,   /* dropped from the table and never served */
};

/* Confirmed more recently than this ⇒ FRESH. */
#define ONION_DIR_STALE_SECS   ((int64_t)6 * 3600)
/* Unconfirmed for longer than this ⇒ EXPIRED (deleted, never served). */
#define ONION_DIR_EXPIRE_SECS  ((int64_t)7 * 24 * 3600)
/* Supervised refresh cadence. */
#define ONION_DIR_REFRESH_SECS 900
/* Hard cap on rows one /directory.json response may carry. ONION_DIR_STR
 * pastes it into a SQL LIMIT literal so the cap is declared once here
 * rather than repeated as a magic 500 in each query. */
#define ONION_DIR_SERVE_MAX    500
#define ONION_DIR_STR_(x) #x
#define ONION_DIR_STR(x)  ONION_DIR_STR_(x)

/* `self` rows are always FRESH: this node's own presence is not hearsay.
 * A non-self row with last_seen <= 0 has no provenance at all and is
 * EXPIRED. A last_seen in the future (peer clock skew, or a hearsay
 * stamp) is clamped to "now", never trusted forward. */
enum onion_dir_freshness onion_directory_freshness(int64_t last_seen,
                                                   int64_t now, bool self);

/* Age in seconds, clamped at 0 (never negative, whatever the stamp says). */
int64_t onion_directory_age_secs(int64_t last_seen, int64_t now);

/* ── The census bridge (freshness INPUT) ────────────────────────────
 *
 * This is deliberately not a second crawler. The node already measures
 * peers on bounded, short-lived sockets outside connman
 * (services/network_crawler.h) and already keeps a durable per-peer
 * uptime ledger (storage/peers_projection.h). Neither can be joined back
 * to a hostname by SQL — the census keys onions on the 16-byte torv3
 * head, from which the 56-char name is not recoverable — so the census
 * OWNER hands the observation over by hostname through this port.
 *
 * What an observation may do is bounded on purpose: it can refresh an
 * EXISTING row or let one age, and nothing else. It never inserts, never
 * deletes, and an unreachable probe never moves last_seen. A lying census
 * therefore cannot add a peer, cannot remove one, and can at most make a
 * row look staler than it is — which costs one skipped hint. */
struct onion_directory_observation {
    char    hostname[64];
    bool    reachable;
    int64_t observed_unix;  /* when the probe ran */
    int64_t best_height;    /* < 0 when the probe learned no height */
};

struct onion_directory_refresh_stats {
    int observed;    /* observations applied to a known row */
    int unknown;     /* observations naming a row we do not have */
    int refreshed;   /* rows whose last_seen moved forward */
    int failed;      /* rows whose fail_count was bumped */
    int discovered;  /* rows the discovery sources (re-)announced */
    int expired;     /* rows dropped for being past ONION_DIR_EXPIRE_SECS */
    int rows_after;  /* directory size once the round finished */
};

/* Apply `n` census observations. Returns the number applied, or -1 when
 * the directory is not open (no datadir).
 *
 * SEAM (stated, not hidden): this port is implemented and tested, and the
 * PRODUCER is not wired yet. The refresh round below keeps the directory
 * current from the discovery sources on its own, so nothing depends on
 * this to work; wiring it adds measured reachability on top. The caller
 * it wants is the crawler's per-round result set
 * (app/services/src/network_crawler.c, which already produces
 * {reachable, latency_us, last_probe_us, best_height} per peer) — it owns
 * the hostname, which is the one thing SQL cannot recover from the
 * census's 16-byte torv3 key. */
int onion_service_directory_observe(const struct onion_directory_observation *obs,
                                    size_t n,
                                    struct onion_directory_refresh_stats *out);

/* Run one refresh round: re-ask the discovery sources, re-register self,
 * expire what has aged out. True when the round completed against a
 * writable directory (whether or not it changed anything). */
bool onion_service_directory_refresh(struct onion_directory_refresh_stats *out);

/* Register the supervised refresh child. Called by onion_service_start()
 * once a datadir exists; idempotent. */
void onion_service_directory_register_refresh(void);

/* Retire it (onion_service_stop). Idempotent; re-registering after this
 * works, so a stop/start cycle is not a one-way door. */
void onion_service_directory_unregister_refresh(void);

/* The boot round: seed the table from the discovery sources, register
 * self, and expire what aged out while this node was down. */
void onion_directory_boot_round(void);

/* Re-publish our own row after Tor hands us an address. */
void onion_directory_register_self(void);

/* ── Our own clearnet endpoint: published, never probed on a tick ────
 *
 * The refresh round runs on the SHARED supervisor tick runner (30 s
 * liveness deadline). The public-IP probe it used to call
 * (peer_strategy_discover_self) does NAT-PMP + UPnP SSDP/SOAP and its
 * own comment records that it blocks for tens of seconds on a gateway
 * that ignores it. Blocking there freezes every other supervised child,
 * so the tick now READS what this setter published and never dials.
 *
 * THE PRODUCER PUBLISHES: peer_strategy_discover_self() calls this at the
 * end of every probe it runs, so there is no caller who has to remember
 * to. A NULL/all-zero ip or port 0 clears the cache, and an unpublished
 * cache simply means our served row carries no clearnet endpoint (exactly
 * what a failed probe produced before). Thread-safe. */
void onion_directory_set_self_clearnet(const uint8_t ip[4], uint16_t port);

/* Drop the cached endpoint (tests, and address changes that invalidate
 * it). Idempotent. */
void onion_directory_reset_self_clearnet(void);

/* ── The onion graph (transitive discovery) ─────────────────────────
 *
 * A /directory.json response carries an "onion" field per node, and the
 * seed-fetch path used to throw it away and keep only clearnet_ip — so an
 * onion peer could never teach this node about another onion peer, which
 * is both the transitive discovery that makes a directory worth having
 * and the censorship-resistant path (it needs no fixed clearnet address).
 *
 * This is the parser for that field. Pure: no I/O, no allocation, no
 * globals. Every hostname goes through onion_hostname_valid(); duplicates
 * and `self_host` are skipped; at most `max` hints are written, which is
 * how the caller caps a single response's contribution. */
struct onion_relay_hint {
    char    hostname[64];
    int     port;
    int     height;
    int64_t last_seen;   /* the ADVERTISING node's stamp; 0 when absent */
};

int onion_directory_parse_relay_hints(const char *body, const char *self_host,
                                      struct onion_relay_hint *out, size_t max);

/* Bounds on what a relayed hostname may cost us. Every one is a hard cap,
 * and together they mean a poisoned directory can only ever waste a
 * bounded number of connection attempts. */
#define ONION_RELAY_PER_RESPONSE  8    /* hostnames one response may add */
#define ONION_RELAY_MAX_DEPTH     1    /* seed -> its neighbours. No further. */
#define ONION_RELAY_VISIT_CAP     64   /* dedupe ring for the current window */
#define ONION_RELAY_FOLLOW_BUDGET 8    /* transitive fetches per window */
#define ONION_RELAY_WINDOW_SECS   600  /* budget + dedupe reset cadence */

/* Claim one transitive fetch for `hostname`. True only when the hostname
 * is new in the current window AND follow budget remains — so a repeated
 * or over-budget hint is simply not followed, never queued and never
 * retried in a loop. Rolling the window clears the dedupe ring with the
 * budget, so a host that also appears as a configured seed becomes
 * reachable again next window rather than being locked out for the life
 * of the process. Thread-safe. */
bool onion_directory_claim_relay_follow(const char *hostname, int64_t now);

/* Drop the window state (budget + dedupe ring). */
void onion_directory_reset_relay_follow(void);

/* Record a hostname learned from another node's directory. ADD-only by
 * construction: INSERT OR IGNORE, so a first-hand row is never
 * overwritten by hearsay, and nothing is ever deleted here. `peer_last_seen`
 * is the advertising node's own stamp and is clamped into
 * [now - ONION_DIR_EXPIRE_SECS, now] — hearsay can make a row look older
 * than we would, never newer. False when the hostname fails the v3 rule
 * or the directory is not open. */
bool onion_service_directory_learn(const char *hostname, int port, int height,
                                   int64_t peer_last_seen);

/* Initialize the onion service layer.
 * Called from app_init() after Tor is linked in.
 * datadir: path for persistent state
 * Returns the .onion address or NULL on failure. */
const char *onion_service_start(const char *datadir);

/* Stop the onion service. */
void onion_service_stop(void);

/* Get the .onion address (NULL if not started). */
const char *onion_service_get_address(void);

/* The datadir handed to onion_service_start (NULL when not started).
 * onion_directory.c reads it rather than keeping a second copy — one
 * owner of the string, one lifetime. */
const char *onion_service_datadir(void);

/* Run the merged discovery sources (signed descriptors first, then the
 * unsigned scrape — onion_peers_collect). Exposed so the directory
 * refresh round uses the SAME source set the boot path does instead of
 * growing a second scraper. */
int onion_service_discover_peers(struct onion_peer *out, size_t max);

/* Set the .onion address (called by tor_integration after reading key). */
void onion_service_set_address(const char *address);

/* Handle an incoming request from dynhost.
 * This is the callback registered with Tor's dynhost webserver.
 *
 * method: "GET" or "POST"
 * path: URL path (e.g., "/", "/blog", "/search")
 * body: request body (NULL for GET)
 * body_len: body length
 * response: output buffer
 * response_max: max response size
 * Returns bytes written to response. */
size_t onion_service_handle_request(const char *method,
                                     const char *path,
                                     const uint8_t *body,
                                     size_t body_len,
                                     uint8_t *response,
                                     size_t response_max);

#endif
