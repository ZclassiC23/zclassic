/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#define _GNU_SOURCE  /* pthread_timedjoin_np */

#define _DEFAULT_SOURCE
#include "platform/time_compat.h"
#include "net/connman.h"
#include "net/v2_transport.h"
#include "net/addrman.h"
#include "event/event.h"
#include "net/peer_bandwidth.h"
#include "net/peer_lifecycle.h"
#include "net/port_policy.h"
#include "net/peer_scoring.h"
#include "net/addrman_integrity.h"
#include "net/download.h"
#include "net/fast_sync.h"
#include "net/tor_integration.h"
#include "core/random.h"
#include "core/serialize.h"
#include "net/netbase.h"
#include "net/version.h"
#include "bloom/bloom.h"
#include "storage/census_read.h"
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/utiltime.h"
#include "util/safe_alloc.h"
#include "util/util.h"
#include "util/log_macros.h"
#include "crypto/random_secret.h"
#include "crypto/curve25519.h"
#include "support/cleanse.h"
#include "util/thread_registry.h"
#include "util/thread_liveness.h"
#include "util/blocker.h"

/* -connect mode: only connect to specified peers, no seeds */
bool g_connect_only = false;

/* Per-peer bandwidth quotas. */
static struct peer_bandwidth g_peer_bw;
static bool g_peer_bw_active = false;

struct peer_bandwidth *peer_bandwidth_get_global(void)
{
    return g_peer_bw_active ? &g_peer_bw : NULL;
}

static pthread_t g_thread_dns_seed;
static pthread_t g_thread_socket;
static pthread_t g_thread_open;
static pthread_t g_thread_message;
static volatile bool g_stop = false;

/* Supervisor liveness for the 4 P2P threads. Root children (not
 * supervisor_register_in_domain(net,...)): lib/net cannot include the
 * app-side supervisors/domains.h without a lib-layering violation — see
 * util/thread_liveness.h. zcl_connman_sock wakes on a deterministic 50ms
 * poll() timeout, so it gets a REAL deadline; the other three legitimately
 * sit idle (DNS-seed sleep cadence, outbound-dialer backoff sleep, message
 * condvar wait) so they are liveness-only (no deadline, no progress gate) —
 * present on the tree, heartbeat when they do work, never falsely flagged
 * for a quiet cycle. */
static struct thread_liveness_child g_dns_seed_liveness = { .id = SUPERVISOR_INVALID_ID };
static struct thread_liveness_child g_sock_liveness     = { .id = SUPERVISOR_INVALID_ID };
static struct thread_liveness_child g_open_liveness     = { .id = SUPERVISOR_INVALID_ID };
static struct thread_liveness_child g_msg_liveness      = { .id = SUPERVISOR_INVALID_ID };
static _Atomic uint64_t g_sock_poll_iterations = 0;

/* Reactor fd-array stats (Task 2 — see REACTOR_MAX_FDS in connman.h).
 * npfds_high_water is written only by thread_socket_handler (single
 * writer); the configured_* fields are snapshotted once in connman_start(). */
static _Atomic size_t g_reactor_npfds_high_water        = 0;
static _Atomic size_t g_reactor_configured_listen_sockets = 0;
static _Atomic int    g_reactor_configured_max_connections = 0;

/* Time-to-first-handshaked-peer telemetry (omniscience surface). connman_start()
 * stamps g_connman_start_us with a monotonic epoch; the FIRST fully-handshaked
 * peer records now-epoch into g_first_peer_us (delta stored directly, 0 = "none
 * yet"). Both are process-wide (a single connman per process, matching the
 * g_reactor_* globals above). */
static _Atomic int64_t g_connman_start_us = 0;   /* 0 = connman not started */
static _Atomic int64_t g_first_peer_us    = 0;   /* delta us; 0 = no peer yet */

void connman_note_first_handshaked_peer(void)
{
    int64_t start = atomic_load(&g_connman_start_us);
    if (start == 0)
        return;                  /* connman not started (e.g. a bare unit test) */
    int64_t expected = 0;
    if (atomic_load(&g_first_peer_us) != 0)
        return;                  /* already recorded — first peer only */
    int64_t delta = platform_time_monotonic_us() - start;
    if (delta <= 0)
        delta = 1;               /* keep 0 reserved for "none yet" */
    (void)atomic_compare_exchange_strong(&g_first_peer_us, &expected, delta);
}

int64_t connman_time_to_first_peer_us(void)
{
    return atomic_load(&g_first_peer_us);
}

#ifdef ZCL_TESTING
void connman_ttfp_reset_for_testing(void)
{
    atomic_store(&g_connman_start_us, 0);
    atomic_store(&g_first_peer_us, 0);
}
#endif

void connman_get_reactor_stats(struct connman_reactor_stats *out)
{
    if (!out) return;
    out->npfds_high_water = atomic_load(&g_reactor_npfds_high_water);
    out->reactor_max_fds = REACTOR_MAX_FDS;
    out->configured_max_connections =
        atomic_load(&g_reactor_configured_max_connections);
    out->configured_listen_sockets =
        atomic_load(&g_reactor_configured_listen_sockets);
}

#define CONNMAN_RECV_LOW_WATER_SLOTS 16

static size_t connman_recv_cap_for_queue(size_t queued, size_t base_cap)
{
    if (queued >= MAX_RECV_MESSAGES)
        return 0;

    size_t free_slots = MAX_RECV_MESSAGES - queued;
    if (free_slots < CONNMAN_RECV_LOW_WATER_SLOTS) {
        size_t cap = free_slots * (size_t)MSG_HEADER_SIZE;
        if (cap < base_cap)
            return cap;
    }
    return base_cap;
}

static void dns_seed_resolve(struct connman *cm)
{
    for (size_t i = 0; i < cm->params->nSeeds; i++) {
        const char *host = cm->params->vSeeds[i].host;
        if (host[0] == '\0') continue;

        printf("Resolving DNS seed: %s\n", host);

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *res = NULL;
        int gai_rc = getaddrinfo(host, NULL, &hints, &res);
        if (gai_rc != 0) {
            LOG_WARN("connman", "DNS seed %s resolution failed: %s",
                     host, gai_strerror(gai_rc));
            continue;
        }

        int count = 0;
        for (struct addrinfo *p = res; p && count < 256; p = p->ai_next) {
            struct net_address addr;
            net_address_init(&addr);
            addr.svc.port = (uint16_t)cm->params->nDefaultPort;

            if (p->ai_family == AF_INET) {
                struct sockaddr_in *s4 = (struct sockaddr_in *)p->ai_addr;
                net_addr_set_ipv4(&addr.svc.addr,
                                  (const unsigned char *)&s4->sin_addr);
                addr.nServices = NODE_NETWORK;
            } else if (p->ai_family == AF_INET6) {
                struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)p->ai_addr;
                memcpy(addr.svc.addr.ip,
                       &s6->sin6_addr, 16);
                addr.nServices = NODE_NETWORK;
            } else {
                continue;
            }

            { struct net_addr src; net_addr_init(&src);
            addrman_add(&cm->manager.addrman, &addr, &src, 0); }
            count++;
        }
        freeaddrinfo(res);
        printf("DNS seed %s: %d addresses\n", host, count);
    }
}

static void seed_from_fixed(struct connman *cm)
{
    const struct chain_params *p = cm->params;
    for (size_t i = 0; i < p->nFixedSeeds && !g_stop; i++) {
        struct net_address addr;
        net_address_init(&addr);
        memcpy(addr.svc.addr.ip, p->vFixedSeeds[i].addr, 16);
        addr.svc.port = p->vFixedSeeds[i].port;
        addr.nServices = NODE_NETWORK;
        struct net_addr src;
        net_addr_init(&src);
        addrman_add(&cm->manager.addrman, &addr, &src, 0);
    }
    if (p->nFixedSeeds > 0)
        printf("Added %zu hardcoded seed nodes\n", p->nFixedSeeds);
}

/* Public: kick seed discovery from outside the connman thread. Used by
 * the sync watchdog when the peer set is too narrow to risk rotation —
 * widens addrman selection so the next outbound cycle picks fresh hosts.
 * Re-adds fixed seeds (cheap, in-memory) and re-runs DNS resolution
 * (one-shot, blocking on the caller's thread for the duration of the
 * resolves). Safe to call concurrently with the discovery thread. */
void connman_kick_seed_discovery(struct connman *cm)
{
    if (!cm || !cm->params || g_stop) return;
    if (g_connect_only) return;
    seed_from_fixed(cm);
    dns_seed_resolve(cm);
}

void connman_set_onion_peer_discovery(struct connman *cm,
                                      const char *datadir,
                                      onion_peer_discover_fn discover)
{
    if (!cm)
        return;
    cm->onion_peer_datadir = datadir;
    cm->onion_peer_discover = discover;
}

void connman_set_known_zcl23_peer_source(
    struct connman *cm,
    connman_known_zcl23_peers_fn peers,
    void *ctx)
{
    if (!cm)
        return;
    cm->known_zcl23_peers = peers;
    cm->known_zcl23_peers_ctx = ctx;
}

/* Fetch /directory.json from a .onion seed and add clearnet IPs */
static void try_onion_seed_fetch(struct connman *cm, const char *onion)
{
    printf("Onion seed: fetching /directory.json from %s...\n", onion);
    fflush(stdout);

    struct onion_fetch_result result = {0};
    int rc = tor_integration_fetch_onion_blocking(onion, "/directory.json",
                                                    &result, 60);
    if (rc < 0 || result.status != 200 || !result.body) {
        printf("Onion seed: fetch failed (rc=%d status=%d)\n",
               rc, result.status);
        if (result.body) free(result.body);
        return;
    }

    /* Parse minimal JSON: extract clearnet_ip and clearnet_port fields */
    const char *p = (const char *)result.body;
    int added = 0;
    while ((p = strstr(p, "\"clearnet_ip\":\"")) != NULL) {
        p += 15; /* skip "clearnet_ip":" */
        const char *end = strchr(p, '"');
        if (!end || end == p) { p++; continue; }

        char ip[64];
        size_t iplen = (size_t)(end - p);
        if (iplen >= sizeof(ip)) { p = end; continue; }
        memcpy(ip, p, iplen);
        ip[iplen] = '\0';
        p = end + 1;

        /* Find clearnet_port */
        uint16_t port = 8033;
        const char *pp = strstr(p, "\"clearnet_port\":");
        if (pp && pp - p < 50) {
            port = (uint16_t)atoi(pp + 16);
            if (port == 0) port = 8033;
        }

        /* Add to address manager */
        if (ip[0] && strcmp(ip, "0.0.0.0") != 0) {
            struct net_address addr;
            memset(&addr, 0, sizeof(addr));
            /* Parse IPv4 */
            unsigned a, b, c, d;
            if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                /* IPv4-mapped IPv6 */
                unsigned char ip4[4] = {(unsigned char)a, (unsigned char)b,
                                        (unsigned char)c, (unsigned char)d};
                net_addr_set_ipv4(&addr.svc.addr, ip4);
                addr.svc.port = port;
                addr.nServices = NODE_NETWORK;
                struct net_addr src;
                net_addr_init(&src);
                addrman_add(&cm->manager.addrman, &addr, &src, 0);
                added++;
                printf("Onion seed: discovered clearnet peer %s:%d\n",
                       ip, port);
            }
        }
    }

    printf("Onion seed: added %d clearnet peers from %s\n", added, onion);
    free(result.body);
}

/* Run the onion-directory bootstrap pass: operator-curated seeds first
 * (~/.config/zclassic23/onion-seeds), then the chainparams onionSeeds, then
 * any known zcl23 .onion peers. Shared by the discovery thread's below-floor
 * branch and the public connman_kick_onion_seeds() peer-of-last-resort entry
 * so both reach an identical supplier set. Requires Tor ready; otherwise a
 * no-op (the clearnet fixed/DNS paths remain the fallback). */
static void run_onion_seed_pass(struct connman *cm)
{
    if (!cm || !cm->params || g_stop) return;
    if (g_connect_only) return;
    if (!tor_integration_is_ready()) return;

    /* Operator-curated onion seeds (one .onion per line, # comments). */
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path),
                 "%s/.config/zclassic23/onion-seeds", home);
        FILE *fp = fopen(path, "re");
        if (fp) {
            char line[256];
            int n = 0;
            while (n < 32 && !g_stop && fgets(line, sizeof(line), fp)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\n' || *p == '\0') continue;
                char *end = strpbrk(p, " \t\r\n#");
                if (end) *end = '\0';
                if (strstr(p, ".onion")) {
                    try_onion_seed_fetch(cm, p);
                    n++;
                }
            }
            fclose(fp);
        }
    }

    /* Hardcoded chainparams onion seeds. */
    for (size_t i = 0; i < cm->params->nOnionSeeds && !g_stop; i++)
        try_onion_seed_fetch(cm, cm->params->onionSeeds[i]);

    /* .onion peers discovered on-chain (ZSLP scan) — same Tor-native
     * source the boot-time discovery pass uses. */
    if (cm->onion_peer_discover && cm->onion_peer_datadir) {
        struct onion_peer peers[16];
        int found = cm->onion_peer_discover(cm->onion_peer_datadir,
                                            peers, 16);
        for (int i = 0; i < found && i < 8 && !g_stop; i++) {
            if (peers[i].hostname[0] && strstr(peers[i].hostname, ".onion"))
                try_onion_seed_fetch(cm, peers[i].hostname);
        }
    }
}

void connman_kick_onion_seeds(struct connman *cm)
{
    if (!cm || g_stop || g_connect_only) return;
    printf("[connman] peer-of-last-resort: querying onion-directory seeds\n");
    fflush(stdout);
    run_onion_seed_pass(cm);
    /* Persist whatever clearnet hosts we just harvested so a subsequent
     * crash/restart before the periodic flush does not lose them. */
    connman_save_addrman(cm);
}

static void *thread_dns_seed(void *arg)
{
    struct connman *cm = (struct connman *)arg;
    thread_liveness_beat(&g_dns_seed_liveness, -1);

    if (g_connect_only) {
        printf("Connect-only mode: skipping seeds, connecting to addnodes only\n");
        return NULL;
    }

    /* Add fixed seeds immediately — don't wait */
    seed_from_fixed(cm);

    /* DNS seeds after 3 seconds (not 11) */
    sleep(3);
    if (!g_stop)
        dns_seed_resolve(cm);

    /* ZSLP chain scan — discover .onion peers from on-chain token data.
     * This is the Tor-native peer discovery: no DNS, no clearnet. */
    if (!g_stop && cm->onion_peer_discover) {
        const char *datadir = cm->onion_peer_datadir;
        if (datadir) {
            struct onion_peer peers[64];
            int found = cm->onion_peer_discover(datadir, peers, 64);
            if (found > 0) {
                printf("ZSLP chain scan: discovered %d .onion peers\n", found);
                for (int i = 0; i < found; i++)
                    printf("  .onion peer: %s (h=%d)\n",
                           peers[i].hostname, peers[i].height);
            }
            /* Try fetching clearnet IPs from discovered .onion peers */
            if (tor_integration_is_ready()) {
                for (int i = 0; i < found && i < 3 && !g_stop; i++) {
                    try_onion_seed_fetch(cm, peers[i].hostname);
                }
            }
        }
    }

    /* Also try operator-curated + hardcoded onion seeds if Tor is
     * ready and few peers. Operator file at ~/.config/zclassic23/
     * onion-seeds (one .onion per line, # comments allowed) is loaded
     * first, then the chainparams fallback seeds. */
    if (!g_stop && cm->manager.num_nodes < 3) {
        if (tor_integration_is_ready()) {
            char buf[32][96];
            int n_seeds = 0;
            const char *home = getenv("HOME");
            if (home) {
                char path[512];
                snprintf(path, sizeof(path),
                         "%s/.config/zclassic23/onion-seeds", home);
                FILE *fp = fopen(path, "re");
                if (fp) {
                    char line[256];
                    while (n_seeds < 32 && fgets(line, sizeof(line), fp)) {
                        char *p = line;
                        while (*p == ' ' || *p == '\t') p++;
                        if (*p == '#' || *p == '\n' || *p == '\0') continue;
                        char *end = strpbrk(p, " \t\r\n#");
                        if (end) *end = '\0';
                        if (strstr(p, ".onion")) {
                            snprintf(buf[n_seeds], sizeof(buf[0]), "%s", p);
                            n_seeds++;
                        }
                    }
                    fclose(fp);
                }
            }
            for (int i = 0; i < n_seeds && !g_stop; i++)
                try_onion_seed_fetch(cm, buf[i]);
            for (size_t i = 0; i < cm->params->nOnionSeeds && !g_stop; i++)
                try_onion_seed_fetch(cm, cm->params->onionSeeds[i]);
        }
    }

    /* If still no peers after 15s, retry everything */
    sleep(12);
    if (!g_stop && cm->manager.num_nodes == 0) {
        printf("No peers found, retrying all discovery methods...\n");
        seed_from_fixed(cm);
        dns_seed_resolve(cm);
    }

    /* Adaptive peer discovery:
     * - 0 peers: retry every 30s (urgent)
     * - 1-2 peers: retry every 60s (degraded)
     * - 3+ peers: check every 5 minutes (healthy)
     *
     * Floor-breach loudness: once we've been below the floor (3 peers)
     * for more than PEER_FLOOR_LOUD_SECS continuously past startup
     * grace, emit EV_PEER_FLOOR_BREACH on every iteration. Independent
     * from the watchdog's recovery path — this is the "loud" half of
     * the redundancy guarantee, so the absence of strong P2P is itself
     * a first-class observable signal. */
    const int PEER_FLOOR_MIN = ZCL_PEER_FLOOR_HEALTHY;
    const int PEER_FLOOR_GRACE_SECS = 120;
    /* Persist addrman on a periodic cadence (~12 min) AND after every seed
     * round, not only on clean shutdown. A kill-9 / OOM before connman_free
     * otherwise discards every host learned this session, forcing the next
     * boot back to fixed/DNS seeds — anti-sticky for a recovering node. */
    const int64_t ADDRMAN_FLUSH_SECS = 12 * 60;
    int64_t last_addrman_flush = (int64_t)platform_time_wall_time_t();
    int64_t start_ts = (int64_t)platform_time_wall_time_t();
    int64_t floor_below_since = 0;
    uint64_t seed_rounds = 0;
    while (!g_stop) {
        size_t n = cm->manager.num_nodes;
        int interval = (n == 0) ? 30 : (n < 3) ? 60 : 300;
        sleep(interval);
        if (g_stop) break;
        thread_liveness_beat(&g_dns_seed_liveness, (int64_t)++seed_rounds);
        size_t cur = cm->manager.num_nodes;
        int64_t now = (int64_t)platform_time_wall_time_t();
        if ((int)cur < PEER_FLOOR_MIN) {
            if (floor_below_since == 0) floor_below_since = now;
            int64_t below_for = now - floor_below_since;
            int64_t since_start = now - start_ts;
            if (since_start > PEER_FLOOR_GRACE_SECS) {
                event_emitf(EV_PEER_FLOOR_BREACH, 0,
                            "healthy=%zu min=%d since=%llds",
                            cur, PEER_FLOOR_MIN, (long long)below_for);
            }
            printf("Peer discovery: %zu peers (need %d+)\n",
                   cur, PEER_FLOOR_MIN);
            seed_from_fixed(cm);
            dns_seed_resolve(cm);
            /* Onion-directory fallback: Tor may not have been ready
             * during the boot-time pass, so retry the chainparams
             * + operator + known-peer seeds while below the floor.
             * Blocking fetches are fine on this dedicated discovery
             * thread. */
            run_onion_seed_pass(cm);
            /* Persist immediately after a seed round: we just learned a
             * fresh host set and the node is degraded — protect it. */
            connman_save_addrman(cm);
            last_addrman_flush = (int64_t)platform_time_wall_time_t();
        } else {
            floor_below_since = 0;
        }
        /* Periodic flush regardless of floor state. */
        if (now - last_addrman_flush >= ADDRMAN_FLUSH_SECS && !g_stop) {
            connman_save_addrman(cm);
            last_addrman_flush = now;
        }
    }

    return NULL;
}

/* Extract /16 subnet group from IPv4-mapped address (bytes 12-13).
 * Returns a 16-bit value representing the first two octets. */
static uint16_t ipv4_group16(const unsigned char ip[16])
{
    return (uint16_t)((ip[12] << 8) | ip[13]);
}

/* Count outbound peers in the same /16 subnet group. Caller holds cs_nodes. */
static int count_outbound_in_group(const struct net_manager *nm, uint16_t group)
{
    int count = 0;
    for (size_t i = 0; i < nm->num_nodes; i++) {
        const struct p2p_node *n = nm->nodes[i];
        if (n->inbound || n->disconnect) continue;
        if (!net_addr_is_ipv4(&n->addr.svc.addr)) continue;
        if (ipv4_group16(n->addr.svc.addr.ip) == group)
            count++;
    }
    return count;
}

#define MAX_OUTBOUND_PER_GROUP16 2

static int connman_outbound_group_count(struct connman *cm, uint16_t group)
{
    int count = 0;

    if (!cm)
        return 0;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    count = count_outbound_in_group(&cm->manager, group);
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return count;
}

/* ── Sybil diversity: IPv6 and onion outbound caps ───────────────────
 *
 * count_outbound_in_group() above only ever inspects IPv4-mapped peers
 * (net_addr_is_ipv4() guard) — a real IPv6 or .onion outbound peer was
 * invisible to the /16 diversity cap entirely, so an attacker with many
 * distinct IPv6 addresses or (far cheaper) many distinct ephemeral
 * .onion identities could fill every outbound slot with sock-puppets
 * and eclipse the node, bypassing MAX_OUTBOUND_PER_GROUP16 completely.
 *
 * IPv6: group by the /32 prefix (first 4 bytes of the real, non-mapped
 * 16-byte address — net_addr_is_ipv6() already excludes the IPv4-mapped
 * ::ffff:a.b.c.d form, so `ip[0..3]` here is a genuine IPv6 prefix, not
 * an IPv4 octet pair). /32 is a coarser allocation unit than IPv4's /16
 * convention but mirrors it: cheap for an attacker to acquire ONE real
 * /32, expensive to acquire MANY.
 *
 * Onion: a v3 onion address is a self-generated Ed25519 public key —
 * free and unlimited to mint, with no ISP/RIR allocation cost at all.
 * There is no "group" to diversify within; the only meaningful bound is
 * a flat cap on TOTAL outbound onion connections. MAX_OUTBOUND_ONION=2
 * matches MAX_OUTBOUND_PER_GROUP16 (the existing IPv4 per-subnet cap) —
 * enough that a normal mixed clearnet+onion peer set keeps some onion
 * diversity, but a wall of attacker-minted .onion identities cannot
 * consume more than 2 of the (default 8) outbound slots.
 *
 * connman_kick_onion_seeds() / run_onion_seed_pass() is UNAFFECTED by
 * this cap: it fetches /directory.json directly over Tor via
 * tor_integration_fetch_onion_blocking() to discover CLEARNET peers —
 * it never dials an outbound P2P connection through
 * connman_pick_next_outbound_target(), so eclipse-recovery via onion
 * seeds still works even when the onion outbound bucket is full. */
#define MAX_OUTBOUND_IPV6_GROUP32 2
#define MAX_OUTBOUND_ONION 2

static uint32_t ipv6_group32(const unsigned char ip[16])
{
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

/* Count outbound peers in the same IPv6 /32 group. Caller holds cs_nodes. */
static int count_outbound_in_ipv6_group(const struct net_manager *nm,
                                        uint32_t group)
{
    int count = 0;
    for (size_t i = 0; i < nm->num_nodes; i++) {
        const struct p2p_node *n = nm->nodes[i];
        if (n->inbound || n->disconnect) continue;
        if (!net_addr_is_ipv6(&n->addr.svc.addr)) continue;
        if (ipv6_group32(n->addr.svc.addr.ip) == group)
            count++;
    }
    return count;
}

/* Count ALL outbound onion peers (no sub-grouping — see the cap doc
 * comment above). Caller holds cs_nodes. */
static int count_outbound_onion(const struct net_manager *nm)
{
    int count = 0;
    for (size_t i = 0; i < nm->num_nodes; i++) {
        const struct p2p_node *n = nm->nodes[i];
        if (n->inbound || n->disconnect) continue;
        if (net_addr_is_tor(&n->addr.svc.addr))
            count++;
    }
    return count;
}

static int connman_outbound_ipv6_group_count(struct connman *cm, uint32_t group)
{
    int count = 0;
    if (!cm) return 0;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    count = count_outbound_in_ipv6_group(&cm->manager, group);
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return count;
}

static int connman_outbound_onion_count(struct connman *cm)
{
    int count = 0;
    if (!cm) return 0;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    count = count_outbound_onion(&cm->manager);
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return count;
}

/* True if `addr` would push its (IPv4 /16, IPv6 /32, or onion-flat)
 * outbound diversity bucket at/over its cap. Shared by the addrman
 * candidate filter and the final pre-connect gate so both enforce the
 * identical rule for all three network kinds. */
static bool connman_outbound_diversity_capped(struct connman *cm,
                                              const struct net_addr *addr)
{
    if (net_addr_is_ipv4(addr)) {
        uint16_t group = ipv4_group16(addr->ip);
        return connman_outbound_group_count(cm, group) >= MAX_OUTBOUND_PER_GROUP16;
    }
    if (net_addr_is_tor(addr))
        return connman_outbound_onion_count(cm) >= MAX_OUTBOUND_ONION;
    if (net_addr_is_ipv6(addr)) {
        uint32_t group = ipv6_group32(addr->ip);
        return connman_outbound_ipv6_group_count(cm, group) >=
               MAX_OUTBOUND_IPV6_GROUP32;
    }
    return false;
}

static bool connman_addnode_is_connected(struct connman *cm, size_t addnode_index)
{
    bool connected = false;

    if (!cm || addnode_index >= (size_t)cm->num_addnodes)
        return false;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
        struct p2p_node *n = cm->manager.nodes[ni];
        if (!n || n->disconnect)
            continue;
        if (net_addr_eq(&n->addr.svc.addr, &cm->addnodes[addnode_index].svc.addr)) {
            connected = true;
            break;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    return connected;
}

static bool connman_node_conflicts_with_target(
    const struct p2p_node *node,
    const struct net_address *addr)
{
    if (!node || !addr || node->disconnect)
        return false;
    if (!net_addr_eq(&node->addr.svc.addr, &addr->svc.addr))
        return false;
    if (node->addr.svc.port == addr->svc.port)
        return true;

    /* Inbound peers usually arrive from ephemeral source ports. Do not let
     * that socket suppress an outbound dial to the peer's advertised listen
     * port; that learned address is useful for reaching the outbound floor. */
    return !node->inbound;
}

static bool connman_addr_is_connected(struct connman *cm,
                                      const struct net_address *addr)
{
    bool connected = false;

    if (!cm || !addr)
        return false;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
        struct p2p_node *n = cm->manager.nodes[ni];
        if (connman_node_conflicts_with_target(n, addr)) {
            connected = true;
            break;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    return connected;
}

/* Release the +1 caller-owned ref that connect_node ALWAYS returns (the
 * symmetric-ref contract: dedupe path returns find_node_by_service_locked's
 * ref; new-node path takes a dedicated CALLER ref alongside the MANAGER ref).
 * Call this AFTER the caller has finished deref'ing the returned node
 * (peer_lifecycle_note_connected, addnode bookkeeping, etc).
 *
 * All ref mutations happen under cs_nodes (p2p_node_add_ref/release are plain
 * non-atomic ++/--), so this acquires it. Two reap orderings are possible:
 *   A) caller releases before the socket sweep reaps the manager ref: ref
 *      2->1, node still in nodes[], reap later drops it to 0 and frees.
 *   B) the socket sweep reaps the manager ref first (POLLHUP -> disconnect ->
 *      reap): the node has left nodes[] and is parked in deferred_free at
 *      ref 1; this release takes it to 0. We must NOT p2p_node_free() it here
 *      — it is still in the deferred_free list, so a direct free would be a
 *      double free on the next sweep. Instead run the deferred_free sweep,
 *      which is the single owner that knows the node is parked and reclaims
 *      every ref<=0 entry. */
static void connman_release_connect_node_ref(struct connman *cm,
                                             struct p2p_node *node)
{
    if (!cm || !node)
        return;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    p2p_node_release(node);
    if (p2p_node_get_ref(node) <= 0) {
        /* Manager ref already reaped concurrently; node is parked in
         * deferred_free. Reclaim it (and any other now-zero entries) here. */
        connman_run_deferred_free_sweep(cm);
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
}

static bool connman_addr_is_advertised_external(
    const struct net_address *addr)
{
    if (!addr || !net_addr_is_ipv4(&addr->svc.addr))
        return false;

    char ip[64];
    uint16_t port = 0;
    if (!msg_version_get_external_ip(ip, sizeof(ip), &port))
        return false;

    unsigned a, b, c, d;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;

    if (addr->svc.addr.ip[12] != (uint8_t)a ||
        addr->svc.addr.ip[13] != (uint8_t)b ||
        addr->svc.addr.ip[14] != (uint8_t)c ||
        addr->svc.addr.ip[15] != (uint8_t)d)
        return false;

    return port == 0 || addr->svc.port == port;
}

static int connman_addrman_retry_cooldown(const struct addr_info *info)
{
    if (!info || info->attempts <= 0)
        return 0;
    if (info->attempts >= 6)
        return 3600;
    if (info->attempts >= 3)
        return 900;
    return 60;
}

static bool connman_addrman_candidate_usable(struct connman *cm,
                                             const struct addr_info *info)
{
    if (!cm || !info)
        return false;

    if (!zcl_net_port_is_reachable_candidate(info->addr.svc.port))
        return false;

    if (connman_addr_is_advertised_external(&info->addr))
        return false;

    if (info->last_try > 0) {
        int64_t now = (int64_t)platform_time_wall_time_t();
        int cooldown = connman_addrman_retry_cooldown(info);
        if (now - info->last_try < cooldown)
            return false;
    }

    if (connman_addr_is_connected(cm, &info->addr))
        return false;

    /* Eclipse/sybil diversity cap — IPv4 /16, IPv6 /32, and the flat
     * onion bucket are all enforced here so no network kind bypasses
     * the diversity gate that IPv4 alone used to get. */
    if (connman_outbound_diversity_capped(cm, &info->addr.svc.addr))
        return false;

    return true;
}

static bool connman_pick_addrman_target(struct connman *cm,
                                        struct addr_info *result)
{
    if (!cm || !result)
        return false;

    for (int i = 0; i < 64; i++) {
        struct addr_info pick;
        memset(&pick, 0, sizeof(pick));
        if (!addrman_select(&cm->manager.addrman, false, &pick))
            return false;
        if (connman_addrman_candidate_usable(cm, &pick)) {
            *result = pick;
            return true;
        }
    }

    /* If random addrman selection keeps landing on saturated/already-used
     * groups, deterministically scan a bounded candidate set. This keeps
     * peer-floor recovery from repeatedly spending slots on the same bad
     * subnet while still preserving addrman's randomized first choice. */
    struct addr_info candidates[128];
    size_t n_candidates = 0;
    struct addr_man *am = &cm->manager.addrman;

    zcl_mutex_lock(&am->cs);
    for (size_t n = 0; n < am->random_size && n_candidates < 128; n++) {
        int nId = am->random_order[n];
        if (nId < 0 || (size_t)nId >= am->entries_cap)
            continue;
        struct addr_info *info = &am->entries[nId];
        if (!info->used)
            continue;
        candidates[n_candidates++] = *info;
    }
    zcl_mutex_unlock(&am->cs);

    for (size_t i = 0; i < n_candidates; i++) {
        if (connman_addrman_candidate_usable(cm, &candidates[i])) {
            *result = candidates[i];
            return true;
        }
    }

    return false;
}

static bool connman_find_addnode_index(struct connman *cm,
                                       const struct net_address *addr,
                                       size_t *out)
{
    if (out)
        *out = SIZE_MAX;
    if (!cm || !addr)
        return false;

    for (int ai = 0; ai < cm->num_addnodes; ai++) {
        if (net_addr_eq(&addr->svc.addr, &cm->addnodes[ai].svc.addr) &&
            addr->svc.port == cm->addnodes[ai].svc.port) {
            if (out)
                *out = (size_t)ai;
            return true;
        }
    }
    return false;
}

void connman_note_addnode_prehandshake_disconnect(
    struct connman *cm,
    const struct p2p_node *node,
    const char *reason)
{
    size_t addnode_index = SIZE_MAX;

    if (!cm || !node || node->inbound ||
        node->state >= PEER_HANDSHAKE_COMPLETE)
        return;
    if (!connman_find_addnode_index(cm, &node->addr, &addnode_index))
        return;

    connman_record_addnode_failure(cm, addnode_index,
                                   CONNMAN_ADDNODE_FAILURE_PROTOCOL);
    printf("Addnode %s: protocol failure before handshake (%s, state=%s)\n",
           node->addr_name,
           reason ? reason : "disconnect",
           peer_state_name(node->state));
}

static bool connman_ready_addnode_from_other_group(struct connman *cm,
                                                   uint16_t saturated_group,
                                                   int64_t now)
{
    if (!cm)
        return false;

    for (int ai = 0; ai < cm->num_addnodes; ai++) {
        if (cm->addnode_retired[ai])
            continue;
        const int cooldown = cm->addnode_backoff_sec[ai] > 0
                           ? cm->addnode_backoff_sec[ai] : 30;
        if (now - cm->addnode_last_attempt[ai] < cooldown)
            continue;
        if (connman_addnode_is_connected(cm, (size_t)ai))
            continue;
        if (!net_addr_is_ipv4(&cm->addnodes[ai].svc.addr))
            return true;

        uint16_t group = ipv4_group16(cm->addnodes[ai].svc.addr.ip);
        if (group != saturated_group &&
            connman_outbound_group_count(cm, group) <
                MAX_OUTBOUND_PER_GROUP16)
            return true;
    }
    return false;
}

bool connman_pick_next_outbound_target(
    struct connman *cm,
    size_t *addnode_cursor,
    struct addr_info *result,
    enum connman_outbound_target_source *source,
    size_t *addnode_index)
{
    if (!cm || !addnode_cursor || !result || !source)
        return false;

    *source = CONNMAN_TARGET_NONE;
    if (addnode_index)
        *addnode_index = SIZE_MAX;
    memset(result, 0, sizeof(*result));

    if (cm->num_addnodes > 0) {
        const int64_t now = (int64_t)platform_time_wall_time_t();
        const size_t start = *addnode_cursor % (size_t)cm->num_addnodes;

        for (size_t offset = 0; offset < (size_t)cm->num_addnodes; offset++) {
            const size_t ai = (start + offset) % (size_t)cm->num_addnodes;
            if (cm->addnode_retired[ai])
                continue;
            const int cooldown = cm->addnode_backoff_sec[ai] > 0
                               ? cm->addnode_backoff_sec[ai] : 30;

            if (connman_addnode_is_connected(cm, ai)) {
                cm->addnode_backoff_sec[ai] = 0;
                continue;
            }
            if (now - cm->addnode_last_attempt[ai] < cooldown)
                continue;

            if (net_addr_is_ipv4(&cm->addnodes[ai].svc.addr)) {
                uint16_t group = ipv4_group16(cm->addnodes[ai].svc.addr.ip);
                if (connman_outbound_group_count(cm, group) >=
                        MAX_OUTBOUND_PER_GROUP16 &&
                    connman_ready_addnode_from_other_group(cm, group, now))
                    continue;
            }

            result->addr = cm->addnodes[ai];
            *source = CONNMAN_TARGET_ADDNODE;
            *addnode_cursor = (ai + 1) % (size_t)cm->num_addnodes;
            if (addnode_index)
                *addnode_index = ai;
            return true;
        }
    }

    if (g_connect_only)
        return false;

    if (!connman_pick_addrman_target(cm, result))
        return false;

    addrman_attempt(&cm->manager.addrman, &result->addr.svc,
                    (int64_t)platform_time_wall_time_t());
    *source = CONNMAN_TARGET_ADDRMAN;
    return true;
}

void connman_record_addnode_attempt(struct connman *cm,
                                    size_t addnode_index,
                                    bool success)
{
    if (!cm || addnode_index >= (size_t)cm->num_addnodes)
        return;

    cm->addnode_last_attempt[addnode_index] = (int64_t)platform_time_wall_time_t();
    if (success) {
        cm->addnode_backoff_sec[addnode_index] = 0;
        /* A live connection clears the failure history. Monotonic
         * counters meant one transient handshake timeout charged a
         * protocol failure forever, permanently disqualifying the
         * addnode from the zero-peer emergency backoff reset. */
        cm->addnode_tcp_failures[addnode_index] = 0;
        cm->addnode_protocol_failures[addnode_index] = 0;
        cm->addnode_first_failure_ts[addnode_index] = 0;
        /* REVIVE: one successful dial is the cheapest possible escape hatch
         * out of retirement — the addnode has just proven it is NOT
         * permanently dead, whatever the historical failure streak said. */
        if (cm->addnode_retired[addnode_index]) {
            cm->addnode_retired[addnode_index] = false;
            char addr[64];
            net_service_to_string(&cm->addnodes[addnode_index].svc, addr,
                                  sizeof(addr));
            LOG_INFO("connman",
                     "addnode revived by successful dial: addr=%s", addr);
        }
        return;
    }

    connman_record_addnode_failure(cm, addnode_index,
                                   CONNMAN_ADDNODE_FAILURE_TCP);
}

void connman_record_addnode_failure(struct connman *cm,
                                    size_t addnode_index,
                                    enum connman_addnode_failure_kind kind)
{
    if (!cm || addnode_index >= (size_t)cm->num_addnodes)
        return;

    cm->addnode_last_attempt[addnode_index] = (int64_t)platform_time_wall_time_t();

    /* Count this failure first so the ramp below can read the running
     * total for this kind. */
    int64_t n;
    if (kind == CONNMAN_ADDNODE_FAILURE_PROTOCOL) {
        n = ++cm->addnode_protocol_failures[addnode_index];
    } else {
        n = ++cm->addnode_tcp_failures[addnode_index];
        /* First TCP failure of a new streak (n==1 means the counter was
         * just reset by a success, or the addnode was just added): stamp
         * the streak start so connman_retire_dead_addnodes can measure the
         * wall-clock window independently of how many attempts happened
         * to land in it. */
        if (n == 1)
            cm->addnode_first_failure_ts[addnode_index] =
                cm->addnode_last_attempt[addnode_index];
    }

    /* Gentle early ramp, then exponential to the 1800s ceiling, driven by
     * the per-kind failure count (not by doubling the previous value).
     *
     * The old logic jumped a FIRST protocol failure straight to 900s, so a
     * single transient drop (one remote-close before the handshake, or one
     * slow first handshake) made a real-but-momentarily-flaky peer invisible
     * to the outbound dialer for 15-30 min. With only 2 healthy outbound and
     * a floor of 3, that starves the floor and "Peer discovery: 2 peers
     * (need 3+)" persists. Now a transient failure costs ~20s, so the peer is
     * re-dialed within seconds; a genuinely dead host still reaches the 1800s
     * cap after ~6-7 consecutive misses, so we do not hammer it.
     *
     * PROTOCOL ramps one step ahead of a bare TCP refusal (a handshake-stage
     * failure is treated slightly more cautiously), preserving the
     * PROTOCOL > TCP ordering the addnode-fallback test asserts. */
    static const int ramp[] = { 20, 60, 120, 300, 600, 1200, 1800 };
    const int ramp_len = (int)(sizeof(ramp) / sizeof(ramp[0]));
    int step = (int)n - 1;
    if (kind == CONNMAN_ADDNODE_FAILURE_PROTOCOL)
        step += 1;            /* one step ahead of a TCP failure */
    if (step < 0) step = 0;
    if (step >= ramp_len) step = ramp_len - 1;
    cm->addnode_backoff_sec[addnode_index] = ramp[step];
}

/* ── addnode self-healing: RETIRE the permanently dead, HARVEST from the
 * census ─────────────────────────────────────────────────────────────
 *
 * See the doc block in net/connman.h for the thresholds and rationale. */

void connman_retire_dead_addnodes(struct connman *cm, size_t outbound_healthy)
{
    if (!cm)
        return;
    /* Floor guard: never retire a dial-of-last-resort while the node is
     * already starved for healthy outbound peers. ZCL_PEER_FLOOR_HEALTHY
     * (net/net.h) is the single source of truth for the floor value. */
    if (outbound_healthy < (size_t)ZCL_PEER_FLOOR_HEALTHY)
        return;

    const int64_t now = (int64_t)platform_time_wall_time_t();
    for (int i = 0; i < cm->num_addnodes; i++) {
        if (cm->addnode_retired[i])
            continue;
        if (cm->addnode_tcp_failures[i] < ZCL_ADDNODE_RETIRE_MIN_TCP_FAILURES)
            continue;
        const int64_t streak_start = cm->addnode_first_failure_ts[i];
        if (streak_start <= 0)
            continue;   /* no failure streak in progress (defensive) */
        if (now - streak_start < ZCL_ADDNODE_RETIRE_MIN_WINDOW_SECS)
            continue;

        cm->addnode_retired[i] = true;
        cm->addnode_retired_at[i] = now;
        cm->addnode_retirements_total++;

        char addr[64];
        net_service_to_string(&cm->addnodes[i].svc, addr, sizeof(addr));
        LOG_WARN("connman",
                 "addnode retired: addr=%s tcp_failures=%lld "
                 "failing_secs=%lld (revivable by one manual dial success "
                 "or an operator addnode re-add)",
                 addr, (long long)cm->addnode_tcp_failures[i],
                 (long long)(now - streak_start));
    }
}

/* Best-effort IPv4 dotted-quad parse. census_node.ip is rendered by
 * net_addr_to_string (census_read.c render_ip16), which for IPv6/onion rows
 * yields a colon-separated or non-numeric form; a harvested row that is not
 * a plain "a.b.c.d" is skipped rather than mis-parsed. */
static bool connman_parse_census_ipv4(const char *ip, unsigned char out[4])
{
    if (!ip || !ip[0])
        return false;
    unsigned a, b, c, d;
    char extra;
    if (sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    out[0] = (unsigned char)a;
    out[1] = (unsigned char)b;
    out[2] = (unsigned char)c;
    out[3] = (unsigned char)d;
    return true;
}

size_t connman_harvest_census_candidates(struct connman *cm,
                                         int64_t min_height)
{
    if (!cm)
        return 0;

    census_reader *r = NULL;
    if (census_read_open(cm->datadir, &r) != CENSUS_READ_OK || !r)
        return 0;   /* absent/unpopulated census: silent no-op, not an error */

    const int64_t now = (int64_t)platform_time_wall_time_t();
    struct census_filter filt = {
        .ua_contains = NULL,
        .min_height = min_height,
        /* Coarse recency pre-filter on last_seen; the exact
         * ZCL_ADDNODE_HARVEST_RECENT_SUCCESS_SECS check below is on
         * last_success, a different column census_filter cannot express. */
        .seen_within_secs = ZCL_ADDNODE_HARVEST_RECENT_SUCCESS_SECS,
        .now_unix = now,
    };
    struct census_node rows[CENSUS_LIST_HARD_CAP];
    int64_t matched = 0;
    int n = census_read_list(r, &filt, 0, CENSUS_LIST_HARD_CAP, rows,
                             CENSUS_LIST_HARD_CAP, &matched);
    census_read_close(r);

    struct net_addr src;
    net_addr_init(&src);
    size_t harvested = 0;
    for (int i = 0; i < n && harvested < ZCL_ADDNODE_HARVEST_MAX_CANDIDATES;
         i++) {
        const struct census_node *node = &rows[i];
        /* dial_success_count>0 AND a recent last_success: only feed the
         * dialer already-proven-reachable candidates, not every host the
         * crawler has merely heard advertised. */
        if (node->dial_success_count <= 0)
            continue;
        if (node->last_success <= 0 ||
            now - node->last_success > ZCL_ADDNODE_HARVEST_RECENT_SUCCESS_SECS)
            continue;

        unsigned char ip4[4];
        if (!connman_parse_census_ipv4(node->ip, ip4))
            continue;   /* IPv6/onion census rows: no parser here yet */

        struct net_address addr;
        net_address_init(&addr);
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = (uint16_t)node->port;
        addr.nServices = (uint64_t)node->services;
        addr.nTime = (uint32_t)now;

        /* addrman_add() is the NEW-table discovery path — never a pinned
         * addnode — and applies addrman's own bucket/group diversity caps
         * exactly as it does for DNS/fixed/onion-seed candidates. */
        if (addrman_add(&cm->manager.addrman, &addr, &src, 0))
            harvested++;
    }

    if (harvested > 0)
        LOG_INFO("connman",
                 "census harvest: added %zu discovery candidate(s) to "
                 "addrman (census_matched=%lld scanned=%d)",
                 harvested, (long long)matched, n);
    return harvested;
}

/* ── Parallel dialer: anchors-first candidate gathering + batch completion ──
 *
 * The old dialer dialed serially: connect_socket_directly() blocked in
 * select() for up to DEFAULT_CONNECT_TIMEOUT (5 s) PER address, up to 3
 * attempts a loop — so a cold boot could spend ~15 s of wall time before it
 * had even 3 peers if the first candidates were slow/dead. The dialer now
 * gathers a batch of distinct candidates (persisted anchors FIRST, then
 * addnode/addrman), fires up to ZCL_DIAL_BATCH_MAX non-blocking connect()s at
 * once, and poll()s them against ONE shared deadline — so N slow peers cost
 * one 5 s window total, not N×5 s, and the fastest live peers win the slots.
 * All fan-out stays inside this single thread (no new threads). */

#define ZCL_DIAL_BATCH_MAX 8   /* <= MAX_OUTBOUND_CONNECTIONS */
#define ZCL_FEELER_INTERVAL_DEFAULT_SECS 120
#define ZCL_FEELER_HANDSHAKE_BUDGET_SECS 40

void connman_collect_healthy_anchors(struct connman *cm,
                                     struct anchor_peer_set *set)
{
    if (!set) return;
    memset(set, 0, sizeof(*set));
    if (!cm) return;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes &&
                       set->count < ANCHOR_PEERS_MAX; i++) {
        const struct p2p_node *n = cm->manager.nodes[i];
        if (!n || n->inbound || n->disconnect || n->is_feeler)
            continue;
        if (n->state < PEER_HANDSHAKE_COMPLETE)
            continue;
        if ((n->services & NODE_NETWORK) == 0)
            continue;
        struct anchor_peer *a = &set->peers[set->count++];
        a->addr = n->addr.svc.addr;
        a->port = n->addr.svc.port;
        a->services = n->services;
        a->last_height = n->starting_height;
        a->last_success = n->time_connected;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
}

/* Dialable right now: reachable ZClassic port, not one of OUR own addresses,
 * not already connected. Diversity is enforced by the batch tally below (the
 * connected-count check alone can't see in-flight dials). */
static bool connman_candidate_addr_dialable(struct connman *cm,
                                            const struct net_address *addr)
{
    if (!cm || !addr)
        return false;
    if (!zcl_net_port_is_reachable_candidate(addr->svc.port))
        return false;
    if (is_local(&cm->manager, &addr->svc))
        return false;
    if (connman_addr_is_connected(cm, addr))
        return false;
    return true;
}

/* Next un-tried anchor as a net_address, marking it tried. Returns false once
 * every loaded anchor has had its single priority attempt. */
static bool connman_anchor_take_next(struct connman *cm, struct net_address *out)
{
    if (!cm || !out)
        return false;
    for (size_t i = 0; i < cm->anchors.count && i < ANCHOR_PEERS_MAX; i++) {
        if (cm->anchors_tried[i])
            continue;
        cm->anchors_tried[i] = true;   /* one attempt each, gate-pass or not */
        const struct anchor_peer *a = &cm->anchors.peers[i];
        net_address_init(out);
        out->svc.addr = a->addr;
        out->svc.port = a->port;
        out->nServices = a->services;
        return true;
    }
    return false;
}

/* In-batch diversity/dup tally: the connected-count caps don't see the OTHER
 * in-flight dials in this same batch, so a batch of 8 could all land in one
 * /16. Track what the batch already targets and refuse a candidate that would
 * push connected+in-batch over the cap. */
struct dial_batch_tally {
    struct net_service svcs[ZCL_DIAL_BATCH_MAX];
    size_t n;
};

static bool tally_has_service(const struct dial_batch_tally *t,
                              const struct net_service *s)
{
    for (size_t i = 0; i < t->n; i++)
        if (net_service_eq(&t->svcs[i], s))
            return true;
    return false;
}

static bool batch_diversity_ok(struct connman *cm,
                               const struct dial_batch_tally *t,
                               const struct net_addr *a)
{
    if (net_addr_is_ipv4(a)) {
        uint16_t g = ipv4_group16(a->ip);
        int inb = 0;
        for (size_t i = 0; i < t->n; i++)
            if (net_addr_is_ipv4(&t->svcs[i].addr) &&
                ipv4_group16(t->svcs[i].addr.ip) == g)
                inb++;
        return connman_outbound_group_count(cm, g) + inb <
               MAX_OUTBOUND_PER_GROUP16;
    }
    if (net_addr_is_tor(a)) {
        int inb = 0;
        for (size_t i = 0; i < t->n; i++)
            if (net_addr_is_tor(&t->svcs[i].addr))
                inb++;
        return connman_outbound_onion_count(cm) + inb < MAX_OUTBOUND_ONION;
    }
    if (net_addr_is_ipv6(a)) {
        uint32_t g = ipv6_group32(a->ip);
        int inb = 0;
        for (size_t i = 0; i < t->n; i++)
            if (net_addr_is_ipv6(&t->svcs[i].addr) &&
                ipv6_group32(t->svcs[i].addr.ip) == g)
                inb++;
        return connman_outbound_ipv6_group_count(cm, g) + inb <
               MAX_OUTBOUND_IPV6_GROUP32;
    }
    return true;
}

size_t connman_gather_dial_candidates(struct connman *cm,
                                      struct connman_dial_candidate *out,
                                      size_t max)
{
    if (!cm || !out || max == 0)
        return 0;
    if (max > ZCL_DIAL_BATCH_MAX)
        max = ZCL_DIAL_BATCH_MAX;

    struct dial_batch_tally tally;
    tally.n = 0;
    size_t n = 0;

    /* (1) Anchors first — but NOT in -connect-only mode, where the explicit
     * -connect peers are the entire outbound universe. Each anchor gets one
     * attempt (marked tried even when it fails a gate, so a dead/saturated
     * anchor can't make us spin). */
    if (!g_connect_only) {
        while (n < max) {
            struct net_address a;
            if (!connman_anchor_take_next(cm, &a))
                break;
            if (!connman_candidate_addr_dialable(cm, &a))
                continue;
            if (tally_has_service(&tally, &a.svc))
                continue;
            if (!batch_diversity_ok(cm, &tally, &a.svc.addr))
                continue;
            out[n].addr = a;
            out[n].source = CONNMAN_TARGET_ANCHOR;
            out[n].addnode_index = SIZE_MAX;
            out[n].is_feeler = false;
            tally.svcs[tally.n++] = a.svc;
            n++;
        }
    }

    /* (2) addnode / addrman via the existing selector. Bound the draws so a
     * pool that keeps returning saturated/duplicate picks can't spin. */
    size_t draws = 0;
    const size_t draw_cap = max * 4 + 8;
    while (n < max && draws < draw_cap) {
        draws++;
        struct addr_info info;
        enum connman_outbound_target_source src = CONNMAN_TARGET_NONE;
        size_t idx = SIZE_MAX;
        memset(&info, 0, sizeof(info));
        if (!connman_pick_next_outbound_target(cm, &cm->next_addnode_cursor,
                                               &info, &src, &idx))
            break;
        if (!connman_candidate_addr_dialable(cm, &info.addr)) {
            /* Already connected → an addnode gets its attempt marked good so
             * its cursor advances, matching the serial dialer. */
            if (src == CONNMAN_TARGET_ADDNODE &&
                connman_addr_is_connected(cm, &info.addr))
                connman_record_addnode_attempt(cm, idx, true);
            continue;
        }
        if (tally_has_service(&tally, &info.addr.svc))
            continue;
        if (!batch_diversity_ok(cm, &tally, &info.addr.svc.addr))
            continue;
        out[n].addr = info.addr;
        out[n].source = src;
        out[n].addnode_index = idx;
        out[n].is_feeler = false;
        tally.svcs[tally.n++] = info.addr.svc;
        n++;
    }

    return n;
}

/* ── Feeler probes (Bitcoin Core pattern) ─────────────────────────────── */

static int64_t connman_feeler_interval_secs(void)
{
    const char *e = getenv("ZCL_FEELER_INTERVAL_SECS");
    if (e && e[0]) {
        long v = strtol(e, NULL, 10);
        if (v > 0 && v < 86400)
            return (int64_t)v;
    }
    return ZCL_FEELER_INTERVAL_DEFAULT_SECS;
}

static bool connman_feeler_in_flight(struct connman *cm)
{
    bool found = false;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        const struct p2p_node *n = cm->manager.nodes[i];
        if (n && n->is_feeler && !n->disconnect) { found = true; break; }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return found;
}

/* Pick an addrman NEW-table address for a feeler probe. */
static bool connman_pick_feeler_target(struct connman *cm,
                                       struct net_address *out)
{
    for (int i = 0; i < 32; i++) {
        struct addr_info pick;
        memset(&pick, 0, sizeof(pick));
        if (!addrman_select(&cm->manager.addrman, /*new_only=*/true, &pick))
            return false;
        if (!connman_candidate_addr_dialable(cm, &pick.addr))
            continue;
        *out = pick.addr;
        return true;
    }
    return false;
}

/* Sweep feeler connections each open-thread iteration: a completed handshake
 * proves the address is real and speaks our protocol → mark it addrman_good
 * and disconnect; a feeler that never handshook inside the budget is dropped.
 * addrman_good is called AFTER releasing cs_nodes to avoid nesting the addrman
 * lock under cs_nodes (lock-order hygiene). */
static void connman_sweep_feelers(struct connman *cm)
{
    int64_t now = (int64_t)platform_time_wall_time_t();
    struct net_service good_svcs[ZCL_DIAL_BATCH_MAX];
    size_t ngood = 0;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *n = cm->manager.nodes[i];
        if (!n || !n->is_feeler || n->disconnect)
            continue;
        if (n->state >= PEER_HANDSHAKE_COMPLETE) {
            if (ngood < ZCL_DIAL_BATCH_MAX)
                good_svcs[ngood++] = n->addr.svc;
            n->disconnect = true;
        } else if (now - n->time_connected > ZCL_FEELER_HANDSHAKE_BUDGET_SECS) {
            n->disconnect = true;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    for (size_t i = 0; i < ngood; i++)
        addrman_good(&cm->manager.addrman, &good_svcs[i], now);
}

/* ── Batch dial: fire non-blocking connects, poll to a shared deadline ──── */

static void connman_record_dial_failure(struct connman *cm,
                                        const struct connman_dial_candidate *c)
{
    char ipbuf[64];
    net_addr_to_string(&c->addr.svc.addr, ipbuf, sizeof(ipbuf));
    if (c->source == CONNMAN_TARGET_ADDNODE)
        connman_record_addnode_attempt(cm, c->addnode_index, false);
    else if (c->source == CONNMAN_TARGET_ADDRMAN)
        addrman_attempt(&cm->manager.addrman, &c->addr.svc,
                        (int64_t)platform_time_wall_time_t());
    /* ANCHOR / feeler carry no durable per-address failure ledger. */
    event_emitf(EV_TCP_CONNECT_FAILED, 0, "%s:%u", ipbuf, c->addr.svc.port);
}

/* Finish a dial whose TCP connect completed successfully: register the node,
 * flag feelers, record addnode success, note lifecycle. */
static void connman_complete_dial(struct connman *cm,
                                  struct connman_dial_candidate *c,
                                  zcl_socket_t sock)
{
    enum peer_lifecycle_source ls =
        c->source == CONNMAN_TARGET_ADDNODE ? PEER_LIFECYCLE_SOURCE_ADDNODE :
        c->source == CONNMAN_TARGET_ANCHOR  ? PEER_LIFECYCLE_SOURCE_ANCHOR  :
                                              PEER_LIFECYCLE_SOURCE_ADDRMAN;
    const char *dest = NULL;
    char destbuf[64];
    if (c->source == CONNMAN_TARGET_ADDNODE) {
        net_service_to_string(&c->addr.svc, destbuf, sizeof(destbuf));
        dest = destbuf;
    }

    bool created = false;
    struct p2p_node *node =
        connect_node_from_socket(&cm->manager, &c->addr, dest, sock, &created);
    if (!node) {
        connman_record_dial_failure(cm, c);
        return;
    }
    /* Only flag a FRESHLY-created node as a feeler; a dedupe race returns an
     * existing real peer that must never be swept/disconnected as a feeler. */
    if (c->is_feeler && created)
        node->is_feeler = true;
    if (c->source == CONNMAN_TARGET_ADDNODE)
        connman_record_addnode_attempt(cm, c->addnode_index, true);
    peer_lifecycle_note_connected(node, ls);
    if (created) {
        char ipbuf[64];
        net_addr_to_string(&c->addr.svc.addr, ipbuf, sizeof(ipbuf));
        printf("Outbound dial: connected to %s:%u%s\n", ipbuf,
               c->addr.svc.port, c->is_feeler ? " (feeler)" : "");
    }
    connman_release_connect_node_ref(cm, node);
}

/* One in-flight non-blocking connect awaiting its writability edge. */
struct dial_inflight {
    zcl_socket_t sock;
    size_t       ci;      /* index into the candidate batch */
};

/* Fire every candidate's non-blocking connect, then poll the in-progress set
 * against ONE shared DEFAULT_CONNECT_TIMEOUT window, completing winners and
 * closing losers. Immediate (localhost) connects complete inline. */
static void connman_dial_batch(struct connman *cm,
                               struct connman_dial_candidate *batch,
                               size_t count)
{
    struct dial_inflight inflight[ZCL_DIAL_BATCH_MAX + 1];
    size_t nin = 0;

    for (size_t i = 0; i < count && !g_stop; i++) {
        struct connman_dial_candidate *c = &batch[i];
        peer_lifecycle_note_attempt(&c->addr,
            c->source == CONNMAN_TARGET_ADDNODE ? PEER_LIFECYCLE_SOURCE_ADDNODE :
            c->source == CONNMAN_TARGET_ANCHOR  ? PEER_LIFECYCLE_SOURCE_ANCHOR  :
                                                  PEER_LIFECYCLE_SOURCE_ADDRMAN);
        zcl_socket_t s = ZCL_INVALID_SOCKET;
        enum zcl_connect_start st = connect_socket_start(&c->addr.svc, &s);
        if (st == ZCL_CONNECT_START_ERROR) {
            connman_record_dial_failure(cm, c);
            continue;
        }
        if (st == ZCL_CONNECT_START_CONNECTED) {
            connman_complete_dial(cm, c, s);
            continue;
        }
        inflight[nin].sock = s;
        inflight[nin].ci = i;
        nin++;
    }

    int64_t deadline_ms = platform_time_monotonic_ms() + DEFAULT_CONNECT_TIMEOUT;
    while (nin > 0 && !g_stop) {
        int64_t remaining = deadline_ms - platform_time_monotonic_ms();
        if (remaining <= 0)
            break;

        struct pollfd pfds[ZCL_DIAL_BATCH_MAX + 1];
        for (size_t i = 0; i < nin; i++) {
            pfds[i].fd = inflight[i].sock;
            pfds[i].events = POLLOUT;
            pfds[i].revents = 0;
        }
        int r = poll(pfds, nin, (int)(remaining > 1000000 ? 1000000 : remaining));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            continue;   /* re-check the deadline */

        /* Resolve every ready fd; swap-remove from the in-flight set. */
        for (size_t i = 0; i < nin; ) {
            short re = pfds[i].revents;
            if (re == 0) { i++; continue; }
            zcl_socket_t s = inflight[i].sock;
            struct connman_dial_candidate *c = &batch[inflight[i].ci];
            bool ok = !(re & (POLLERR | POLLHUP | POLLNVAL)) &&
                      connect_socket_check(s);
            if (ok) {
                connman_complete_dial(cm, c, s);   /* takes ownership of s */
            } else {
                close_socket(&s);
                connman_record_dial_failure(cm, c);
            }
            /* swap-remove i */
            inflight[i] = inflight[nin - 1];
            pfds[i] = pfds[nin - 1];
            nin--;
        }
    }

    /* Deadline / stop: whatever is still connecting lost the race. */
    for (size_t i = 0; i < nin; i++) {
        zcl_socket_t s = inflight[i].sock;
        close_socket(&s);
        connman_record_dial_failure(cm, &batch[inflight[i].ci]);
    }
}

static void *thread_open_connections(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    static int64_t s_last_addrman_attempt = 0;
    uint64_t open_iterations = 0;
    thread_liveness_beat(&g_open_liveness, 0);

    while (!g_stop) {
        /* Count outbound peers in two buckets:
         *   `outbound_slot` — all non-disconnecting outbound peers,
         *     used as the upper bound on connection slots so we don't
         *     overflow MAX_OUTBOUND_CONNECTIONS.
         *   `outbound_healthy` — only outbound peers past handshake,
         *     used to decide if we need aggressive backfill. Without
         *     this distinction a node with 1 working peer + several stuck
         *     in PEER_CONNECTING reads as fully-outbound and never
         *     hunts for replacements. */
        /* Sweep any feeler probes that finished (mark good + disconnect) or
         * timed out, BEFORE counting slots — feelers never count toward the
         * outbound floor/slot budget. */
        connman_sweep_feelers(cm);

        size_t outbound_slot = 0;
        size_t outbound_healthy = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            struct p2p_node *n = cm->manager.nodes[i];
            if (n->inbound || n->disconnect || n->is_feeler) continue;
            outbound_slot++;
            if (n->state >= PEER_HANDSHAKE_COMPLETE)
                outbound_healthy++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        /* RETIRE: cheap (O(MAX_ADDNODES), no I/O) — safe every iteration.
         * Floor-guarded internally against outbound_healthy. */
        connman_retire_dead_addnodes(cm, outbound_healthy);

        size_t outbound = outbound_slot;

        if (outbound >= MAX_OUTBOUND_CONNECTIONS ||
            cm->manager.num_nodes >= (size_t)cm->manager.max_connections) {
            sleep(1);
            continue;
        }

        /* In connect-only mode maintain one outbound connection PER
         * -connect addnode (each explicit target gets its own slot), not a
         * single global connection. The old `outbound >= 1` gate stopped
         * after the FIRST addnode connected, so a second/third -connect peer
         * was never dialed despite the "1 connection per addnode" promise —
         * a single-supplier node could not fan out to its other explicit
         * peers. Multiple connections to the SAME peer still cause snapshot
         * serving to split and stall, but that is prevented by connect_node's
         * per-service dedupe, not by capping the global outbound count.
         * Bound: MAX_OUTBOUND_CONNECTIONS (checked above) still caps the
         * total, and num_addnodes <= MAX_ADDNODES. */
        if (g_connect_only && outbound >= (size_t)cm->num_addnodes) {
            sleep(1);
            continue;
        }

        /* ZCL23 peer preference: 50% of connection attempts go to known
         * ZCL23 peers (fast sync capable, high bandwidth). This creates
         * a tight mesh of power nodes that find each other quickly. */
        bool tried_zcl23 = false;
        if (!g_connect_only && cm->known_zcl23_peers &&
            (GetRand(2) == 0)) {
            struct connman_known_peer zcl_peers[8];
            int nzcl = cm->known_zcl23_peers(
                cm->known_zcl23_peers_ctx, zcl_peers, 8);
            if (nzcl > 0) {
                struct connman_known_peer *pick =
                    &zcl_peers[GetRand((uint64_t)nzcl)];
                /* Check not already connected */
                bool already = false;
                zcl_mutex_lock(&cm->manager.cs_nodes);
                for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
                    struct p2p_node *n = cm->manager.nodes[ni];
                    if (!n->disconnect &&
                        memcmp(n->addr.svc.addr.ip, pick->ip, 16) == 0 &&
                        n->addr.svc.port == pick->port) {
                        already = true;
                        break;
                    }
                }
                zcl_mutex_unlock(&cm->manager.cs_nodes);
                if (!already) {
                    struct net_address addr;
                    memset(&addr, 0, sizeof(addr));
                    memcpy(addr.svc.addr.ip, pick->ip, 16);
                    addr.svc.port = pick->port;
                    addr.nServices = pick->services;
                    addr.nTime = (uint32_t)platform_time_wall_time_t();
                    peer_lifecycle_note_attempt(&addr,
                                                PEER_LIFECYCLE_SOURCE_ZCL23_DB);
                    struct p2p_node *node = connect_node(&cm->manager,
                                                          &addr, NULL);
                    if (node) {
                        peer_lifecycle_note_connected(
                            node, PEER_LIFECYCLE_SOURCE_ZCL23_DB);
                        tried_zcl23 = true;
                        /* Drop the +1 caller ref connect_node returns. */
                        connman_release_connect_node_ref(cm, node);
                    }
                }
            }
        }

        /* Parallel batch dial. Below the healthy-peer floor (3 fully-
         * handshaked outbound) we fill many slots at once (peers stuck in
         * PEER_CONNECTING don't count, so a node with 1 working peer + several
         * stuck sockets still backfills aggressively). At/above the floor we
         * dial gently, rate-limited to one candidate per 10 s. Either way the
         * whole batch's connects run concurrently against ONE shared 5 s
         * window instead of serially, so N slow peers no longer cost N×5 s. */
        int64_t now_oc = (int64_t)platform_time_wall_time_t();
        const size_t OUTBOUND_HEALTHY_FLOOR = ZCL_PEER_FLOOR_HEALTHY;
        bool below_floor = (outbound_healthy < OUTBOUND_HEALTHY_FLOOR);
        bool rate_ok = below_floor ||
                       (now_oc - s_last_addrman_attempt >= 10);

        /* HARVEST: below the healthy floor AND addrman itself is running
         * dry — pull proven-reachable candidates from the durable network
         * census into addrman (NEVER as pinned addnodes). Cadence-gated so
         * a persistently-hungry loop doesn't hammer the census DB every
         * ~1s tail iteration; the outcome feeds the NEXT gather call, not
         * necessarily this one. connman doesn't track chain height, so no
         * min_height filter here (-1). */
        if (below_floor &&
            now_oc - cm->last_census_harvest_ts >=
                ZCL_ADDNODE_HARVEST_INTERVAL_SECS) {
            zcl_mutex_lock(&cm->manager.addrman.cs);
            size_t am_size = addrman_size(&cm->manager.addrman);
            zcl_mutex_unlock(&cm->manager.addrman.cs);
            if (am_size < ZCL_ADDNODE_HARVEST_WEAK_ADDRMAN_THRESHOLD) {
                cm->last_census_harvest_ts = now_oc;
                connman_harvest_census_candidates(cm, -1);
            }
        }

        size_t free_slots = MAX_OUTBOUND_CONNECTIONS - outbound; /* > 0 here */
        size_t want = 0;
        if (rate_ok && !tried_zcl23) {
            if (outbound_healthy == 0)
                want = free_slots;                    /* bootstrap: fill fast */
            else if (below_floor)
                want = free_slots < 4 ? free_slots : 4;
            else
                want = 1;                             /* healthy: gentle */
        }
        if (want > ZCL_DIAL_BATCH_MAX)
            want = ZCL_DIAL_BATCH_MAX;

        struct connman_dial_candidate batch[ZCL_DIAL_BATCH_MAX + 1];
        size_t nbatch = 0;
        if (want > 0)
            nbatch = connman_gather_dial_candidates(cm, batch, want);

        /* Feeler: at most one transient NEW-table validation probe per
         * interval, appended beyond the slot budget (feelers never occupy an
         * outbound slot). Skipped in -connect-only mode. */
        if (!g_connect_only) {
            int64_t feeler_interval = connman_feeler_interval_secs();
            if (now_oc - cm->last_feeler_ts >= feeler_interval &&
                !connman_feeler_in_flight(cm)) {
                struct net_address ftarget;
                if (connman_pick_feeler_target(cm, &ftarget)) {
                    cm->last_feeler_ts = now_oc;
                    batch[nbatch].addr = ftarget;
                    batch[nbatch].source = CONNMAN_TARGET_ADDRMAN;
                    batch[nbatch].addnode_index = SIZE_MAX;
                    batch[nbatch].is_feeler = true;
                    nbatch++;
                } else {
                    /* Nothing probeable right now — still back off the timer
                     * so we don't re-scan the NEW table every loop. */
                    cm->last_feeler_ts = now_oc;
                }
            }
        }

        if (nbatch > 0) {
            if (rate_ok && !tried_zcl23)
                s_last_addrman_attempt = now_oc;
            connman_dial_batch(cm, batch, nbatch);
        }

        /* Liveness-only, not the sleep-tail's 10s cap: the batch dial above
         * can wait on connect() for up to one 5 s window. */
        thread_liveness_beat(&g_open_liveness, (int64_t)++open_iterations);

        /* Sleep based on outbound count:
         * 0 outbound: 200ms (desperate)
         * below target: 1s
         * at target: 10s (just monitoring) */
        if (outbound == 0)
            usleep(200000); /* 200ms */
        else if (outbound < MAX_OUTBOUND_CONNECTIONS)
            sleep(1);
        else
            sleep(10);
    }
    return NULL;
}

static void *thread_socket_handler(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    while (!g_stop) {
        /* Build poll array: listen sockets + connected nodes.
         * Using poll() instead of select() avoids FD_SETSIZE (1024) limit
         * which caused stack corruption with high fd numbers. Array size is
         * REACTOR_MAX_FDS (connman.h) — connman_start()'s admission check
         * guarantees num_listen_sockets + max_connections never exceeds it,
         * so these loop bounds are a defense-in-depth belt, not the
         * enforcement point. */
        struct pollfd pfds[REACTOR_MAX_FDS];
        size_t npfds = 0;
        size_t listen_count = cm->manager.num_listen_sockets;

        /* Add listen sockets */
        for (size_t i = 0; i < listen_count && npfds < REACTOR_MAX_FDS; i++) {
            pfds[npfds].fd = (int)cm->manager.listen_sockets[i].socket;
            pfds[npfds].events = POLLIN;
            pfds[npfds].revents = 0;
            npfds++;
        }

        /* Add connected nodes — snapshot fd + index under lock */
        struct { int fd; size_t node_idx; } node_fds[REACTOR_MAX_FDS];
        size_t n_node_fds = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes && npfds < REACTOR_MAX_FDS; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            int fd = (int)node->socket;
            if (fd < 0) continue;
            short events = POLLIN;
            if (node->send_size > 0)
                events |= POLLOUT;
            pfds[npfds].fd = fd;
            pfds[npfds].events = events;
            pfds[npfds].revents = 0;
            if (n_node_fds < REACTOR_MAX_FDS) {
                node_fds[n_node_fds].fd = fd;
                node_fds[n_node_fds].node_idx = i;
                n_node_fds++;
            }
            npfds++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        /* High-water mark for operator/agent introspection (net/connman
         * dump-state). Single writer (this thread), plain atomic store. */
        if (npfds > atomic_load_explicit(&g_reactor_npfds_high_water,
                                         memory_order_relaxed))
            atomic_store_explicit(&g_reactor_npfds_high_water, npfds,
                                  memory_order_relaxed);

        thread_liveness_beat(&g_sock_liveness,
                             (int64_t)atomic_fetch_add_explicit(
                                 &g_sock_poll_iterations, 1,
                                 memory_order_relaxed) + 1);

        if (npfds == 0) {
            usleep(50000);
            continue;
        }

        int nready = poll(pfds, (nfds_t)npfds, 50 /* ms */);
        if (nready <= 0) continue;

        /* Accept new connections via net.c accept_connection() */
        for (size_t i = 0; i < listen_count; i++) {
            if (pfds[i].revents & POLLIN)
                accept_connection(&cm->manager,
                                  &cm->manager.listen_sockets[i]);
        }

        /* Read/write on connected nodes — re-acquire lock and match by fd.
         * Nodes may have changed since the poll snapshot, so validate. */
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t pi = 0; pi < n_node_fds; pi++) {
            size_t poll_idx = listen_count + pi;
            if (poll_idx >= npfds) break;
            short rev = pfds[poll_idx].revents;
            if (!rev) continue;

            /* Find the node that still has this fd */
            int target_fd = node_fds[pi].fd;
            struct p2p_node *node = NULL;
            for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
                if ((int)cm->manager.nodes[ni]->socket == target_fd) {
                    node = cm->manager.nodes[ni];
                    break;
                }
            }
            if (!node) continue; /* node was removed between poll and now */

            if ((rev & POLLIN) && !node->disconnect) {
                /* Bandwidth quota: check download budget before recv.
                 * If no tokens available, skip this peer until refill. */
                uint32_t bw_id = (uint32_t)node->id;
                size_t bw_avail = g_peer_bw_active
                    ? peer_bandwidth_available(&g_peer_bw, bw_id, PEER_BW_DOWN)
                    : SIZE_MAX;
                if (bw_avail == 0) goto skip_recv;

                zcl_mutex_lock(&node->cs_recv);
                /* Backpressure: if the message queue is full, skip recv
                 * until the processing thread drains it. This prevents
                 * disconnecting fast senders (e.g. snapshot serving over
                 * localhost) just because we can't parse fast enough. */
                size_t queued = node->recv_msg_count;
                if (queued >= MAX_RECV_MESSAGES) {
                    zcl_mutex_unlock(&node->cs_recv);
                    goto skip_recv;
                }
                char buf[0x10000];
                /* Cap recv size to bandwidth budget and remaining queue
                 * space. If a fast peer is close to filling the complete
                 * message queue, avoid reading a large kernel buffer that
                 * would overflow the in-process protocol stream before the
                 * message thread can drain it. */
                size_t recv_cap = connman_recv_cap_for_queue(queued,
                                                             sizeof(buf));
                if (bw_avail < recv_cap) recv_cap = bw_avail;
                if (recv_cap == 0) {
                    zcl_mutex_unlock(&node->cs_recv);
                    goto skip_recv;
                }
                ssize_t n = recv(target_fd, buf, recv_cap, MSG_DONTWAIT);
                if (n > 0) {
                    /* v2 transport seam: decrypt below the message layer. The
                     * plaintext path (transport == NULL) is the UNCHANGED else
                     * — `plain`/`plain_n` remain the raw recv bytes. */
                    const char *plain = buf;
                    unsigned int plain_n = (unsigned int)n;
                    uint8_t *dec = NULL, *wire = NULL;
                    size_t dec_len = 0, wire_len = 0;
                    bool v2_dropped = false;
                    if (node->transport) {
                        if (!v2_transport_feed(node->transport,
                                               (const uint8_t *)buf, (size_t)n,
                                               &wire, &wire_len,
                                               &dec, &dec_len)) {
                            connman_note_addnode_prehandshake_disconnect(
                                cm, node, "v2-transport");
                            node->disconnect = true;
                            v2_dropped = true;
                        } else {
                            /* Handshake replies / flushed sealed pending go out
                             * unmodified; cs_recv held, this takes cs_send. */
                            if (wire_len)
                                p2p_node_queue_raw(node, wire, wire_len);
                            plain = (const char *)dec;
                            plain_n = (unsigned int)dec_len;
                            /* Responder saw v1 magic: drop the transport and
                             * continue plaintext. `dec` carries the buffered
                             * raw bytes to replay through the parser. */
                            if (node->transport->state ==
                                V2_PLAINTEXT_FALLBACK) {
                                v2_transport_free(node->transport);
                                node->transport = NULL;
                            }
                        }
                        free(wire);
                    }
                    if (!v2_dropped && plain_n &&
                        !p2p_node_receive_bytes(node, plain, plain_n,
                                                cm->manager.message_start)) {
                        connman_note_addnode_prehandshake_disconnect(
                            cm, node, "message-parse");
                        node->disconnect = true;
                    }
                    free(dec);
                    /* Single framing-layer scoring point: the parse path has
                     * no net_manager back-pointer, so it only TAGS abuse
                     * (bad start-magic / oversize / parse-fail). Drain the tag
                     * here where nm is in scope, scoring the peer once so a
                     * reconnecting flooder still accrues toward the ban
                     * threshold. No-op when nothing was tagged. */
                    p2p_node_score_framing_offence(&cm->manager, node);
                    node->last_recv = GetTime();
                    node->recv_bytes += (uint64_t)n;
                    /* Consume download tokens post-recv. */
                    if (g_peer_bw_active)
                        peer_bandwidth_consume(&g_peer_bw, bw_id,
                                               PEER_BW_DOWN, (size_t)n);
                } else if (n == 0) {
                    connman_note_addnode_prehandshake_disconnect(
                        cm, node, "remote-close");
                    node->disconnect = true;
                } else {
                    int err = errno;
                    if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
                        connman_note_addnode_prehandshake_disconnect(
                            cm, node, "recv-error");
                        node->disconnect = true;
                    }
                }
                /* If disconnecting, clean up messages while holding lock */
                if (node->disconnect) {
                    for (size_t mi = 0; mi < node->recv_msg_count; mi++)
                        net_message_free(&node->recv_msgs[mi]);
                    node->recv_msg_count = 0;
                }
                zcl_mutex_unlock(&node->cs_recv);
            }
            skip_recv: ;

            if ((rev & POLLOUT) && !node->disconnect) {
                /* Bandwidth quota: check upload budget before send. */
                uint32_t bw_id_s = (uint32_t)node->id;
                size_t up_avail = g_peer_bw_active
                    ? peer_bandwidth_available(&g_peer_bw, bw_id_s, PEER_BW_UP)
                    : SIZE_MAX;
                if (up_avail > 0) {
                    zcl_mutex_lock(&node->cs_send);
                    uint64_t before = node->send_bytes;
                    socket_send_data(node);
                    uint64_t sent = node->send_bytes - before;
                    zcl_mutex_unlock(&node->cs_send);
                    /* Consume upload tokens for actual bytes sent. */
                    if (g_peer_bw_active && sent > 0)
                        peer_bandwidth_consume(&g_peer_bw, bw_id_s,
                                               PEER_BW_UP, (size_t)sent);
                }
                /* If up_avail == 0, skip send — peer is throttled. */
            }

            /* POLLHUP/POLLERR — peer disconnected or socket error */
            if ((rev & (POLLHUP | POLLERR)) && !node->disconnect) {
                connman_note_addnode_prehandshake_disconnect(
                    cm, node, "poll-hup-err");
                node->disconnect = true;
            }
        }
        /* Free deferred nodes (still under cs_nodes lock). refs
         * held by a parallel message-handler snapshot re-park the node
         * in deferred_free for the next cycle instead of being freed. */
        connman_run_deferred_free_sweep(cm);

        /* Periodic peer stats (every 60s) */
        {
            static int64_t last_peer_log = 0;
            static int64_t first_peer_log = 0;
            int64_t now_log = GetTime();
            if (first_peer_log == 0)
                first_peer_log = now_log;
            if (now_log - last_peer_log >= 60) {
                last_peer_log = now_log;
                size_t in = 0, out = 0, connected = 0;
                for (size_t pi = 0; pi < cm->manager.num_nodes; pi++) {
                    struct p2p_node *p = cm->manager.nodes[pi];
                    if (p->disconnect) continue;
                    if (p->inbound) in++; else out++;
                    if (p->state >= PEER_HANDSHAKE_COMPLETE) connected++;
                }
                if (out == 0 && cm->manager.num_nodes > 0 &&
                    now_log - first_peer_log >= 60)
                    printf("WARNING: 0 outbound peers (%zu inbound) "
                           "— cannot sync\n", in);
                else if (cm->manager.num_nodes > 0)
                    printf("Peers: %zu (%zu out, %zu in, %zu active)\n",
                           cm->manager.num_nodes, out, in, connected);
                else if (now_log > 30) /* don't warn during first 30s */
                    printf("WARNING: 0 peers connected\n");
            }
        }

        /* Timeout: disconnect nodes with no activity */
        {
            int64_t now_check = GetTime();
            for (size_t i = 0; i < cm->manager.num_nodes; i++) {
                struct p2p_node *n = cm->manager.nodes[i];
                if (n->disconnect) continue;

                /* Addnode peers get longer timeout (10 min) since they
                 * auto-reconnect anyway and we want them stable */
                size_t addnode_index = SIZE_MAX;
                bool is_addnode =
                    connman_find_addnode_index(cm, &n->addr, &addnode_index);
                int timeout = is_addnode ? 600 : 120;

                if (n->state >= PEER_HANDSHAKE_COMPLETE &&
                    n->last_recv > 0 &&
                    now_check - n->last_recv > timeout) {
                    event_emitf(EV_TCP_TIMEOUT, (uint32_t)n->id,
                                "inactivity %llds state=%s",
                                (long long)(now_check - n->last_recv),
                                peer_state_name(n->state));
                    peer_lifecycle_note_timeout(n, "inactivity");
                    printf("Peer %s: timeout (no data for %llds)\n",
                           n->addr_name,
                           (long long)(now_check - n->last_recv));
                    n->disconnect = true;
                }
                /* TCP connect timeout: 10s for outbound peers stuck
                 * in PEER_CONNECTING (TCP SYN sent, never reached
                 * PEER_CONNECTED). time_connected is set at node
                 * creation, so a peer still in PEER_CONNECTING after
                 * 10s never finished the 3-way TCP handshake. Without
                 * this fast cutoff those sockets sit forever, eat
                 * outbound slots, and starve real peers — the live
                 * failure mode that left this node with 1 working
                 * outbound and 4 stuck in `connecting`.
                 *
                 * Loopback exemption: a 127.0.0.0/8 or ::1 socket either
                 * succeeds instantly or never (no SYN-loss on lo). The
                 * 10s cutoff was hitting our cold-boot zclassicd peer
                 * before the message loop pushed our version frame,
                 * killing the loopback fast lane until the next outbound
                 * retry. Use the 90s handshake budget instead. */
                int connect_timeout =
                    net_addr_is_local(&n->addr.svc.addr) ? 90 : 10;
                if (!n->inbound &&
                    n->state == PEER_CONNECTING &&
                    n->time_connected > 0 &&
                    now_check - n->time_connected > connect_timeout) {
                    event_emitf(EV_TCP_TIMEOUT, (uint32_t)n->id,
                                "tcp_connect %llds state=connecting",
                                (long long)(now_check - n->time_connected));
                    peer_lifecycle_note_timeout(n, "tcp_connect");
                    if (is_addnode)
                        connman_record_addnode_failure(
                            cm, addnode_index, CONNMAN_ADDNODE_FAILURE_TCP);
                    n->disconnect = true;
                    continue;
                }

                /* Version handshake timeout: 90s.
                 *
                 * the message-cycle loop processes
                 * one peer at a time and shares the thread with chain
                 * activation. While we're doing a multi-block reorg
                 * recovery or a heavy reducer pass, peer handshakes can
                 * wait several seconds. Live evidence: 6 inbound peers
                 * timing out at exactly 30-45s because we hadn't read
                 * their version message yet. Bumping to 90s keeps real
                 * dead peers out (TCP keepalive trips first) while
                 * giving slow ticks room. */
                if (n->state < PEER_HANDSHAKE_COMPLETE &&
                    n->time_connected > 0 &&
                    now_check - n->time_connected > 90) {
                    event_emitf(EV_TCP_TIMEOUT, (uint32_t)n->id,
                                "handshake %llds state=%s",
                                (long long)(now_check - n->time_connected),
                                peer_state_name(n->state));
                    printf("Peer %s: handshake timeout after %llds "
                           "(version=%d, state=%s, %s)\n",
                           n->addr_name,
                           (long long)(now_check - n->time_connected),
                           n->version,
                           peer_state_name(n->state),
                           n->inbound ? "inbound" : "outbound");
                    peer_lifecycle_note_timeout(n, "handshake");
                    if (is_addnode)
                        connman_record_addnode_failure(
                            cm, addnode_index,
                            CONNMAN_ADDNODE_FAILURE_PROTOCOL);
                    n->disconnect = true;
                }
            }
        }

        /* Disconnect flagged nodes — defer free to next cycle */
        static uint64_t s_hardcap_leaked = 0;
        for (size_t i = 0; i < cm->manager.num_nodes; ) {
            if (cm->manager.nodes[i]->disconnect) {
                struct p2p_node *node = cm->manager.nodes[i];
                event_emitf(EV_TCP_DISCONNECTED, (uint32_t)node->id,
                            "%s state=%s misbehavior=%d",
                            node->addr_name,
                            peer_state_name(node->state),
                            node->misbehavior);
                peer_lifecycle_note_disconnected(node, "cleanup");

                /* Re-queue any in-flight blocks from this peer */
                {
                    dl_peer_disconnected(msg_get_download_mgr(),
                                          (uint32_t)node->id);
                }

                /* Force disconnect — bypass transition validator since this
                 * is cleanup, not a normal state change. The event was
                 * already emitted (EV_TCP_DISCONNECTED above). */
                node->state = PEER_DISCONNECTED;
                p2p_node_close_socket(node);

                /* Clean send queue while holding cs_nodes. Use
                 * send_segment_free (not a raw free) so each segment's
                 * bytes are returned to the process-wide send budget;
                 * otherwise g_send_total_bytes leaks on forced disconnect. */
                zcl_mutex_lock(&node->cs_send);
                while (node->send_head) {
                    struct send_segment *seg = node->send_head;
                    node->send_head = seg->next;
                    send_segment_free(seg);
                }
                node->send_tail = NULL;
                node->send_size = 0;
                zcl_mutex_unlock(&node->cs_send);

                /* Clean recv messages */
                zcl_mutex_lock(&node->cs_recv);
                for (size_t mi = 0; mi < node->recv_msg_count; mi++)
                    net_message_free(&node->recv_msgs[mi]);
                node->recv_msg_count = 0;
                zcl_mutex_unlock(&node->cs_recv);

                cm->manager.nodes[i] =
                    cm->manager.nodes[cm->manager.num_nodes - 1];
                cm->manager.num_nodes--;
                /* Drop the manager's ownership ref (taken at creation in
                 * connect_node/accept_connection) now that the node has
                 * left nodes[]. Any remaining ref belongs to an in-flight
                 * message-cycle snapshot. Without this release no node
                 * ever reached refcount 0, so the deferred list pinned at
                 * its hard cap and every churned peer leaked. */
                p2p_node_release(node);
                if (p2p_node_get_ref(node) <= 0) {
                    p2p_node_free(node);
                } else if (cm->num_deferred_free < cm->deferred_free_cap) {
                    cm->deferred_free[cm->num_deferred_free++] = node;
                } else if (cm->deferred_free_cap <
                           CONNMAN_DEFERRED_FREE_HARD_CAP) {
                    /* grow the buffer rather than leak.
                     * Double until hitting the hard cap. */
                    size_t new_cap = cm->deferred_free_cap * 2;
                    if (new_cap > CONNMAN_DEFERRED_FREE_HARD_CAP)
                        new_cap = CONNMAN_DEFERRED_FREE_HARD_CAP;
                    struct p2p_node **grown = zcl_realloc(
                        cm->deferred_free,
                        new_cap * sizeof(*cm->deferred_free),
                        "connman_deferred_free");
                    if (grown) {
                        cm->deferred_free = grown;
                        cm->deferred_free_cap = new_cap;
                        cm->deferred_free[cm->num_deferred_free++] = node;
                    } else {
                        LOG_WARN("connman",
                                 "deferred_free realloc failed and ref "
                                 "on node %s — leaking to avoid UAF",
                                 node->addr_name);
                    }
                } else {
                    /* Hard ceiling with a live ref: a message-cycle
                     * snapshot has been stuck holding refs for a long
                     * time. Transition + running count, not a per-node
                     * log storm. */
                    s_hardcap_leaked++;
                    if (s_hardcap_leaked == 1 ||
                        s_hardcap_leaked % 100 == 0)
                        LOG_WARN("connman",
                                 "deferred_free HARD CAP %d with ref on "
                                 "node %s — leaked %llu total to avoid "
                                 "UAF (investigate refs)",
                                 CONNMAN_DEFERRED_FREE_HARD_CAP,
                                 node->addr_name,
                                 (unsigned long long)s_hardcap_leaked);
                }
            } else {
                i++;
            }
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);
    }
    return NULL;
}

bool connman_run_message_cycle(struct connman *cm)
{
    /* snapshot-then-iterate breaks the "cs_nodes held across
     * callback" anti-pattern. Pre-fix, the message thread held
     * cs_nodes for the duration of process_messages + send_messages
     * across every peer — which meant any other thread trying to
     * acquire cs_nodes (socket thread, RPC handlers, new-peer accept)
     * could be starved for tens or hundreds of ms, and any callback
     * that transitively tried to re-take cs_nodes only worked because
     * zcl_mutex_t is recursive. That recursion masked the latent
     * inversion hazard with sibling locks.
     *
     * Fix: grab cs_nodes just long enough to copy the pointer list and
     * bump each node's ref_count, release the lock, run the callbacks
     * against the local copy, then re-acquire cs_nodes to drop refs.
     * The deferred_free sweep respects ref_count > 0 so a peer
     * in-flight in a snapshot cannot be freed out from under us. */
    bool did_work = false;
    atomic_fetch_add_explicit(&cm->message_cycles, 1,
                              memory_order_relaxed);

    /* Phase 1: snapshot + add_ref under cs_nodes. */
    zcl_mutex_lock(&cm->manager.cs_nodes);
    size_t num_nodes = cm->manager.num_nodes;
    size_t snap_count = 0;
    struct p2p_node **snap = NULL;
    if (num_nodes > 0) {
        snap = zcl_malloc(num_nodes * sizeof(*snap),
                          "message_cycle_snapshot");
        if (snap) {
            for (size_t i = 0; i < num_nodes; i++) {
                struct p2p_node *n = cm->manager.nodes[i];
                if (n->disconnect) continue;
                snap[snap_count++] = n;
                p2p_node_add_ref(n);
            }
        }
    }
    /* Capture num_nodes for the trickle ratio. Stale is fine — the
     * read just picks the random-peer probability. */
    size_t trickle_denom = num_nodes;
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    if (!snap) return false;
    atomic_fetch_add_explicit(&cm->message_nodes_snapshotted,
                              (uint64_t)snap_count, memory_order_relaxed);

    /* Phase 2: send first. A received block can drive local reducer work
     * that takes locks or waits on validation workers. Queued getdata
     * dispatch must not sit behind that heavy local processing. */
    for (size_t i = 0; i < snap_count; i++) {
        struct p2p_node *node = snap[i];
        /* Peer may have disconnected mid-iteration — ref_count still
         * keeps the pointer valid, but there's no point talking to a
         * dead socket. */
        if (node->disconnect) continue;

        if (cm->manager.signals.send_messages) {
            bool trickle = trickle_denom > 0 &&
                           (GetRand(trickle_denom) == 0);
            atomic_fetch_add_explicit(&cm->message_send_calls, 1,
                                      memory_order_relaxed);
            cm->manager.signals.send_messages(
                cm->manager.signals.ctx, node, trickle);
            /* Keep tight loop when serving snapshot — don't sleep */
            if (node->state == PEER_SNAPSHOT_SERVING ||
                node->state == PEER_SNAPSHOT_RECEIVING)
                did_work = true;
        }
    }

    /* Phase 3: then process bounded inbound work, NO lock held. */
    for (size_t i = 0; i < snap_count; i++) {
        struct p2p_node *node = snap[i];
        if (node->disconnect) continue;

        bool has_recv_messages = false;
        zcl_mutex_lock(&node->cs_recv);
        has_recv_messages = node->recv_msg_count > 0;
        zcl_mutex_unlock(&node->cs_recv);

        if (has_recv_messages && cm->manager.signals.process_messages) {
            atomic_fetch_add_explicit(&cm->message_recv_ready, 1,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&cm->message_process_calls, 1,
                                      memory_order_relaxed);
            cm->manager.signals.process_messages(
                cm->manager.signals.ctx, node);
            did_work = true;
        }
    }

    /* Release refs under cs_nodes and drain deferred_free here. The socket
     * thread's
     * sweep runs only once per outer loop (potentially many ms); when
     * we've just dropped refs we are exactly the right moment to free
     * any deferred nodes whose refs went to zero. Without this drain the
     * buffer grows under churn until it hits the hard cap and starts
     * leaking. */
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < snap_count; i++)
        p2p_node_release(snap[i]);
    connman_run_deferred_free_sweep(cm);
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    free(snap);
    return did_work;
}

void connman_wake_message_handler(struct connman *cm)
{
    if (!cm)
        return;
    atomic_fetch_add_explicit(&cm->message_wakes, 1, memory_order_relaxed);
    zcl_mutex_lock(&cm->manager.msg_handler_mutex);
    zcl_cond_signal(&cm->manager.msg_handler_cond);
    zcl_mutex_unlock(&cm->manager.msg_handler_mutex);
}

void connman_run_deferred_free_sweep(struct connman *cm)
{
    /* Caller must hold cm->manager.cs_nodes. Walks the deferred_free
     * list and frees each entry whose ref_count has reached zero; any
     * still referenced by an in-flight snapshot (e.g. the message
     * handler mid-iterate) is re-packed at the front of the list so
     * the next socket cycle retries. ref_count is plain int but all
     * mutations happen under cs_nodes, so no atomics needed. */
    size_t keep = 0;
    for (size_t i = 0; i < cm->num_deferred_free; i++) {
        struct p2p_node *n = cm->deferred_free[i];
        if (p2p_node_get_ref(n) > 0)
            cm->deferred_free[keep++] = n;
        else
            p2p_node_free(n);
    }
    cm->num_deferred_free = keep;
}

static void *thread_message_handler(void *arg)
{
    struct connman *cm = (struct connman *)arg;
    uint64_t msg_cycles = 0;

    while (!g_stop) {
        thread_liveness_beat(&g_msg_liveness, (int64_t)++msg_cycles);
        bool did_work = connman_run_message_cycle(cm);
        if (!did_work) {
            struct timespec until;
            platform_time_realtime_timespec(&until);
            until.tv_nsec += 100 * 1000 * 1000;
            if (until.tv_nsec >= 1000 * 1000 * 1000) {
                until.tv_sec++;
                until.tv_nsec -= 1000 * 1000 * 1000;
            }
            atomic_fetch_add_explicit(&cm->message_idle_waits, 1,
                                      memory_order_relaxed);
            zcl_mutex_lock(&cm->manager.msg_handler_mutex);
            if (!g_stop)
                pthread_cond_timedwait(&cm->manager.msg_handler_cond,
                                       &cm->manager.msg_handler_mutex,
                                       &until);
            zcl_mutex_unlock(&cm->manager.msg_handler_mutex);
        }
    }
    return NULL;
}

/* Load the persistent v2 static identity from {datadir}/v2_identity.key, or
 * generate + persist it (0600) on first boot. Only called when -v2transport is
 * set, so the default-off node never touches disk here. On any failure the
 * transport is disabled rather than run with a zero key. */
static void v2_identity_load_or_generate(struct net_manager *nm)
{
    char datadir[1024];
    GetDataDir(true, datadir, sizeof(datadir));
    char path[1152];
    snprintf(path, sizeof(path), "%s/v2_identity.key", datadir);

    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t priv[32];
        size_t rd = fread(priv, 1, sizeof(priv), f);
        fclose(f);
        if (rd == sizeof(priv) &&
            curve25519_scalarmult_base(nm->identity_pub, priv)) {
            memcpy(nm->identity_priv, priv, sizeof(priv));
            memory_cleanse(priv, sizeof(priv));
            return;
        }
        memory_cleanse(priv, sizeof(priv));
        LOG_WARN("net", "v2_identity: unreadable key at %s, regenerating", path);
    }

    uint8_t priv[32];
    if (!zcl_random_secret_bytes(priv, sizeof(priv), "v2_identity") ||
        !curve25519_scalarmult_base(nm->identity_pub, priv)) {
        memory_cleanse(priv, sizeof(priv));
        nm->v2_enabled = false;
        LOG_WARN("net", "v2_identity: key generation failed; v2 transport disabled");
        return;
    }
    memcpy(nm->identity_priv, priv, sizeof(priv));

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ssize_t wr = write(fd, priv, sizeof(priv));
        if (wr != (ssize_t)sizeof(priv))
            LOG_WARN("net", "v2_identity: short write persisting key at %s", path);
        close(fd);
    } else {
        LOG_WARN("net", "v2_identity: could not persist key at %s", path);
    }
    memory_cleanse(priv, sizeof(priv));
}

bool connman_init(struct connman *cm, const struct chain_params *params,
                   struct node_signals *signals)
{
    if (!cm || !params || !signals)
        return false;

    /* Load peer-scoring config from environment. Safe to call multiple
     * times; late env-var changes don't matter since connman_init runs
     * once per process startup. Done here so every binary that spins up
     * a connman (node, test, tool) honours the operator's settings. */
    peer_scoring_init();

    memset(cm, 0, sizeof(*cm));
    net_manager_init(&cm->manager);
    cm->params = params;
    cm->manager.signals = *signals;

    /* dynamic deferred_free buffer. */
    cm->deferred_free_cap = CONNMAN_DEFERRED_FREE_INIT_CAP;
    cm->deferred_free = zcl_malloc(
        cm->deferred_free_cap * sizeof(*cm->deferred_free),
        "connman.deferred_free");
    if (!cm->deferred_free) {
        cm->deferred_free_cap = 0;
        return false;
    }
    cm->num_deferred_free = 0;

    memcpy(cm->manager.message_start, params->pchMessageStart,
           MESSAGE_START_SIZE);
    cm->manager.default_port = (uint16_t)params->nDefaultPort;
    cm->manager.local_services = NODE_NETWORK | NODE_ZCL23;
    if (bip37_enabled())
        cm->manager.local_services |= NODE_BLOOM;

    /* v2 Noise transport: default OFF. When -v2transport is passed, advertise
     * NODE_V2TRANSPORT and load/generate the persistent static identity. */
    cm->manager.v2_enabled = GetBoolArg("-v2transport", false);
    if (cm->manager.v2_enabled) {
        v2_identity_load_or_generate(&cm->manager);
        if (cm->manager.v2_enabled)
            cm->manager.local_services |= NODE_V2TRANSPORT;
    }
    cm->manager.local_host_nonce = GetRand(UINT64_MAX);
    snprintf(cm->manager.sub_version, MAX_SUBVERSION_LENGTH, "%s",
             msg_version_user_agent());

    return true;
}

/* Pure admission-clamp helper — no side effects (no logging, no blocker,
 * no globals), so it is exercisable from a fast unit test without spawning
 * the real reactor threads. Returns the max_connections to actually use
 * (unchanged if it already fits, else clamped to whatever room remains
 * alongside `listen_sockets` under REACTOR_MAX_FDS). Sets *impossible_out
 * (if non-NULL) when listen sockets ALONE leave no room for any
 * connection at all — the one case connman_start() still refuses on. */
static int connman_reactor_admit(size_t listen_sockets, int requested_max,
                                  bool *impossible_out)
{
    if (impossible_out)
        *impossible_out = false;
    size_t reactor_needed = listen_sockets + (size_t)requested_max;
    if (reactor_needed <= REACTOR_MAX_FDS)
        return requested_max;
    long long room = (long long)REACTOR_MAX_FDS - (long long)listen_sockets;
    if (room > 0)
        return (int)room;
    if (impossible_out)
        *impossible_out = true;
    return requested_max;   /* caller refuses; value unused on that path */
}

#ifdef ZCL_TESTING
int connman_reactor_admit_for_test(size_t listen_sockets, int requested_max,
                                    bool *impossible_out)
{
    return connman_reactor_admit(listen_sockets, requested_max, impossible_out);
}
#endif

bool connman_start(struct connman *cm)
{
    if (!cm)
        LOG_FAIL("net", "connman_start called with NULL connman");
    if (cm->started)
        return true;

    /* Stamp the time-to-first-peer epoch before any dial can complete a
     * handshake (connman_note_first_handshaked_peer measures against this). */
    atomic_store(&g_connman_start_us, platform_time_monotonic_us());

    /* Initialize per-peer bandwidth quotas from env vars. */
    if (!g_peer_bw_active) {
        peer_bandwidth_init(&g_peer_bw);
        peer_bandwidth_load_from_env(&g_peer_bw);
        g_peer_bw_active = true;
    }

    /* Listen sockets are already bound by the time connman_start() runs
     * (boot_services binds before starting the service), so
     * num_listen_sockets is final here. If it plus max_connections would
     * overflow the hand-sized poll() fd arrays in thread_socket_handler,
     * CLAMP max_connections down to whatever room remains instead of
     * refusing P2P outright — a configured max_connections is an operator
     * request for a CEILING, not a floor the node must hit-or-die on; a
     * clamp keeps the node serving peers (loudly, at reduced capacity)
     * where a refuse would silently produce a node with ZERO peers. Only
     * the genuinely impossible case — listen sockets ALONE already meet or
     * exceed REACTOR_MAX_FDS, leaving no room for any peer connection at
     * all — still refuses to start with the named PERMANENT blocker. */
    atomic_store(&g_reactor_configured_listen_sockets,
                 cm->manager.num_listen_sockets);
    atomic_store(&g_reactor_configured_max_connections,
                 cm->manager.max_connections);
    {
        bool impossible = false;
        int admitted = connman_reactor_admit(cm->manager.num_listen_sockets,
                                             cm->manager.max_connections,
                                             &impossible);
        if (impossible) {
            char reason[BLOCKER_REASON_MAX];
            snprintf(reason, sizeof reason,
                     "connman reactor overflow: %zu listen socket(s) "
                     "alone leave no room for any peer connection under "
                     "REACTOR_MAX_FDS=%d — lower the listen socket "
                     "count before starting P2P",
                     cm->manager.num_listen_sockets, REACTOR_MAX_FDS);
            struct blocker_record rec;
            if (blocker_init(&rec, "connman_reactor_overflow", "net",
                             BLOCKER_PERMANENT, reason))
                (void)blocker_set(&rec);
            LOG_FAIL("net", "%s", reason);
        } else if (admitted != cm->manager.max_connections) {
            LOG_WARN("net",
                     "connman reactor near-overflow: %zu listen socket(s) "
                     "+ %d configured max_connections exceeds "
                     "REACTOR_MAX_FDS=%d — clamping max_connections %d -> "
                     "%d so the reactor still fits its fd arrays",
                     cm->manager.num_listen_sockets,
                     cm->manager.max_connections, REACTOR_MAX_FDS,
                     cm->manager.max_connections, admitted);
            cm->manager.max_connections = admitted;
        }
    }

    g_stop = false;

    if (thread_registry_spawn("zcl_dns_seed", thread_dns_seed, cm,
                                  &g_thread_dns_seed) != 0) {
        perror("connman: thread_registry_spawn dns_seed");
        g_stop = true;
        LOG_FAIL("net", "thread_registry_spawn failed for dns_seed thread");
    }
    cm->dns_seed_thread_started = true;
    thread_liveness_register(&g_dns_seed_liveness, "zcl_dns_seed", 0, 0);

    if (thread_registry_spawn("zcl_connman_sock", thread_socket_handler,
                                  cm, &g_thread_socket) != 0) {
        perror("connman: thread_registry_spawn socket");
        g_stop = true;
        pthread_join(g_thread_dns_seed, NULL);
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "thread_registry_spawn failed for socket_handler thread");
    }
    cm->socket_thread_started = true;
    /* Real deadline: this thread wakes on a deterministic 50ms poll()
     * timeout (or immediately on socket readiness), so a 30s silence is
     * genuinely wedged, not idle. Progress marker = poll iterations. */
    thread_liveness_register(&g_sock_liveness, "zcl_connman_sock",
                             /*deadline_secs=*/30, /*progress_quiet_us=*/0);

    if (thread_registry_spawn("zcl_connman_open", thread_open_connections,
                                  cm, &g_thread_open) != 0) {
        perror("connman: thread_registry_spawn open");
        g_stop = true;
        pthread_join(g_thread_socket, NULL);
        pthread_join(g_thread_dns_seed, NULL);
        cm->socket_thread_started = false;
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "thread_registry_spawn failed for open_connections thread");
    }
    cm->open_thread_started = true;
    thread_liveness_register(&g_open_liveness, "zcl_connman_open", 0, 0);

    if (thread_registry_spawn("zcl_connman_msg", thread_message_handler,
                                  cm, &g_thread_message) != 0) {
        perror("connman: thread_registry_spawn message");
        g_stop = true;
        pthread_join(g_thread_open, NULL);
        pthread_join(g_thread_socket, NULL);
        pthread_join(g_thread_dns_seed, NULL);
        cm->open_thread_started = false;
        cm->socket_thread_started = false;
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "thread_registry_spawn failed for message_handler thread");
    }
    cm->message_thread_started = true;
    thread_liveness_register(&g_msg_liveness, "zcl_connman_msg", 0, 0);

    cm->started = true;
    printf("P2P threads started.\n");
    return true;
}

void connman_signal_stop(struct connman *cm)
{
    (void)cm;
    g_stop = true;
}

/* pthread_timedjoin_np-based bounded join.
 *
 * Old implementation spawned a helper thread per join and polled a flag;
 * the global g_join_target / g_join_done state meant joins serialised,
 * timed_join leaked the helper on timeout, and 30 s per thread × 4
 * threads breached systemd's 90 s TimeoutStopSec. Linux's
 * pthread_timedjoin_np is the right tool. */
static bool timed_join(pthread_t thread, int timeout_sec)
{
    struct timespec ts;
    if (platform_time_realtime_timespec(&ts) != 0) {
        pthread_join(thread, NULL);
        return true;
    }
    ts.tv_sec += timeout_sec;
    int rc = pthread_timedjoin_np(thread, NULL, &ts);
    if (rc == 0)
        return true;
    /* Timeout (ETIMEDOUT) or other error — detach and move on. */
    pthread_detach(thread);
    return false;
}

void connman_join(struct connman *cm)
{
    if (!cm)
        return;

    /* Use 30-second timeout per thread to prevent SIGKILL from systemd. If a
     * thread is stuck (e.g., message thread in reducer activation), detach it
     * rather than blocking shutdown indefinitely. */
    if (cm->started || cm->dns_seed_thread_started || cm->socket_thread_started ||
        cm->open_thread_started || cm->message_thread_started) {
        if (cm->dns_seed_thread_started) {
            if (!timed_join(g_thread_dns_seed, 5))
                LOG_WARN("connman", "dns_seed thread join timed out");
            cm->dns_seed_thread_started = false;
            thread_liveness_retire(&g_dns_seed_liveness);
        }
        if (cm->socket_thread_started) {
            if (!timed_join(g_thread_socket, 5))
                LOG_WARN("connman", "socket thread join timed out");
            cm->socket_thread_started = false;
            thread_liveness_retire(&g_sock_liveness);
        }
        if (cm->open_thread_started) {
            if (!timed_join(g_thread_open, 5))
                LOG_WARN("connman", "open thread join timed out");
            cm->open_thread_started = false;
            thread_liveness_retire(&g_open_liveness);
        }
        if (cm->message_thread_started) {
            if (!timed_join(g_thread_message, 5))
                LOG_WARN("connman", "message thread join timed out");
            cm->message_thread_started = false;
            thread_liveness_retire(&g_msg_liveness);
        }
        cm->started = false;
    }
    printf("P2P threads stopped.\n");
}

void connman_stop(struct connman *cm)
{
    connman_signal_stop(cm);
    connman_join(cm);

    /* Tear down bandwidth quotas. */
    if (g_peer_bw_active) {
        peer_bandwidth_destroy(&g_peer_bw);
        g_peer_bw_active = false;
    }
}

void connman_save_addrman(struct connman *cm)
{
    if (!cm->datadir) return;

    /* Remember the datadir on net_manager itself so ban_addr()/
     * unban_addr()/clear_banned() (which only see a net_manager*, not
     * this connman*) can persist bans the moment they mutate, not just
     * at whatever cadence connman_save_addrman() is called. */
    cm->manager.datadir = cm->datadir;
    (void)ban_db_write(&cm->manager, cm->datadir);

    char path[512];
    snprintf(path, sizeof(path), "%s/peers.dat", cm->datadir);

    struct byte_stream s;
    stream_init(&s, 65536);
    zcl_mutex_lock(&cm->manager.addrman.cs);
    bool ok = addrman_serialize(&cm->manager.addrman, &s);
    zcl_mutex_unlock(&cm->manager.addrman.cs);

    if (ok && s.size > 0) {
        char tmp_path[520];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        FILE *f = fopen(tmp_path, "wb");
        if (f) {
            size_t written = fwrite(s.data, 1, s.size, f);
            fflush(f);
            int fd = fileno(f);
            if (fd >= 0) (void)fsync(fd);
            fclose(f);
            if (written == s.size) {
                rename(tmp_path, path);
                /* Write the SHA3 sidecar so the next boot can
                 * detect tampering or partial corruption. Best-
                 * effort — a sidecar write failure is logged but
                 * not fatal; the next load will see
                 * AII_SIDECAR_MISSING and accept the body. */
                (void)aii_write_sidecar(cm->datadir);
                printf("Saved %zu peers to %s (%zu bytes)\n",
                       addrman_size(&cm->manager.addrman), path, s.size);
            } else {
                remove(tmp_path);
                LOG_WARN("addrman", "save: short write (%zu/%zu)",
                         written, s.size);
            }
        }
    }
    stream_free(&s);

    /* Persist the currently-healthy outbound set as anchors for the NEXT boot
     * (dialed first, before the addrman random walk). Called on every addrman
     * flush AND clean shutdown, so a kill-9 loses at most one flush interval of
     * anchor freshness. Best-effort — a failure is logged, never fatal, and the
     * next boot simply starts with no anchors. */
    struct anchor_peer_set anchors;
    connman_collect_healthy_anchors(cm, &anchors);
    struct zcl_result ar = anchor_peers_save(cm->datadir, &anchors);
    if (!ar.ok)
        LOG_WARN("anchor_peers", "anchors save failed: %s", ar.message);
}

void connman_load_addrman(struct connman *cm)
{
    if (!cm->datadir) return;

    /* Set BEFORE ban_db_read() so a ban recorded moments after boot (e.g.
     * an immediately-hostile inbound peer) persists too, and before any
     * connection is accepted so nm->banned[] is populated first. */
    cm->manager.datadir = cm->datadir;
    (void)ban_db_read(&cm->manager, cm->datadir);

    char path[512];
    snprintf(path, sizeof(path), "%s/peers.dat", cm->datadir);

    /* Integrity check before we deserialize. On any non-OK,
     * non-MISSING verdict we quarantine the file (rename aside)
     * and return — the caller ends up with an empty addrman and
     * must re-learn from DNS seeds / hardcoded peers. A corrupt
     * peers.dat is always safer discarded than deserialized:
     * the contents directly influence outbound peer selection,
     * which is exactly what an attacker would target. The
     * `SIDECAR_MISSING` verdict is expected on the first boot
     * after this service ships (the body has no companion yet);
     * we accept it. */
    char aii_err[256];
    enum aii_verdict verdict = aii_verify(cm->datadir, aii_err, sizeof(aii_err));
    if (verdict != AII_OK && verdict != AII_SIDECAR_MISSING &&
        verdict != AII_BODY_MISSING) {
        LOG_WARN("addrman", "load: integrity check failed (%s): %s",
                 aii_verdict_name(verdict), aii_err);
        aii_quarantine_corrupt(cm->datadir, verdict);
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return; /* no saved peers — normal on first run */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 50 * 1024 * 1024) { /* sanity: max 50 MB */
        fclose(f);
        return;
    }

    uint8_t *buf = zcl_malloc((size_t)sz, "connman_read_buf");
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (rd == (size_t)sz) {
        struct byte_stream s;
        stream_init_from_data(&s, buf, (size_t)sz);
        zcl_mutex_lock(&cm->manager.addrman.cs);
        if (addrman_deserialize(&cm->manager.addrman, &s)) {
            printf("Loaded %zu peers from %s\n",
                   addrman_size(&cm->manager.addrman), path);
        } else {
            LOG_WARN("addrman", "load: deserialize failed, starting fresh");
            addrman_clear(&cm->manager.addrman);
        }
        zcl_mutex_unlock(&cm->manager.addrman.cs);
        stream_free(&s);
    }
    free(buf);

    /* Load the persisted anchor set, dialed FIRST at boot (before the addrman
     * random walk). Corruption is quarantined inside anchor_peers_load; a bad
     * or missing anchors file yields an empty set and never blocks boot. Reset
     * the tried flags so every loaded anchor gets its one priority attempt. */
    memset(cm->anchors_tried, 0, sizeof(cm->anchors_tried));
    enum anchor_load_status as = anchor_peers_load(cm->datadir, &cm->anchors);
    if (cm->anchors.count > 0)
        printf("Loaded %zu anchor peer(s) from anchors.dat (%s)\n",
               cm->anchors.count, anchor_load_status_name(as));
    else if (as == ANCHOR_LOAD_QUARANTINED)
        LOG_WARN("anchor_peers", "anchors.dat quarantined at boot — starting with no anchors");
}

void connman_free(struct connman *cm)
{
    if (cm->started)
        connman_stop(cm);
    connman_save_addrman(cm);
    net_manager_free(&cm->manager);

    /* free any deferred entries still pending. After
     * connman_stop returns, no other thread should hold node refs. */
    if (cm->deferred_free) {
        for (size_t i = 0; i < cm->num_deferred_free; i++) {
            if (cm->deferred_free[i])
                p2p_node_free(cm->deferred_free[i]);
        }
        free(cm->deferred_free);
        cm->deferred_free = NULL;
        cm->num_deferred_free = 0;
        cm->deferred_free_cap = 0;
    }
}

void connman_relay_transaction(struct connman *cm,
                                const struct uint256 *txid)
{
    struct inv_item inv;
    inv_item_init_typed(&inv, MSG_TX, txid);

    int relayed = 0;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *node = cm->manager.nodes[i];
        if (node->state >= PEER_HANDSHAKE_COMPLETE && !node->disconnect) {
            p2p_node_push_inventory(node, &inv);
            relayed++;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    char hex[65];
    uint256_get_hex(txid, hex);
    printf("Relay tx %s to %d peers\n", hex, relayed);
}

void connman_add_seed_node(struct connman *cm, const char *host,
                            uint16_t port)
{
    struct net_address addr;
    net_address_init(&addr);
    addr.svc.port = port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)res->ai_addr;
            net_addr_set_ipv4(&addr.svc.addr,
                              (const unsigned char *)&s4->sin_addr);
        } else if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(addr.svc.addr.ip, &s6->sin6_addr, 16);
        }
        freeaddrinfo(res);
        { struct net_addr src; net_addr_init(&src);
            addrman_add(&cm->manager.addrman, &addr, &src, 0); }
    }
}

void connman_open_connection(struct connman *cm,
                              const struct net_address *addr)
{
    /* Store in persistent addnode list for automatic reconnection */
    if (cm->num_addnodes < MAX_ADDNODES) {
        /* Avoid duplicates */
        bool dup = false;
        for (int i = 0; i < cm->num_addnodes; i++) {
            if (net_addr_eq(&cm->addnodes[i].svc.addr, &addr->svc.addr) &&
                cm->addnodes[i].svc.port == addr->svc.port) {
                dup = true;
                /* REVIVE: an operator `addnode add` on an already-listed
                 * (possibly retired) entry is an explicit signal to try
                 * again — the other manual escape hatch out of retirement
                 * besides a successful dial (see the RETIRE/HARVEST doc
                 * block in net/connman.h). Also drop the backoff and
                 * failure history so the next dial isn't still paying for
                 * the streak that got it retired. */
                if (cm->addnode_retired[i]) {
                    cm->addnode_retired[i] = false;
                    cm->addnode_backoff_sec[i] = 0;
                    cm->addnode_tcp_failures[i] = 0;
                    cm->addnode_protocol_failures[i] = 0;
                    cm->addnode_first_failure_ts[i] = 0;
                    char revived_addr[64];
                    net_service_to_string(&cm->addnodes[i].svc, revived_addr,
                                          sizeof(revived_addr));
                    LOG_INFO("connman",
                             "addnode revived by operator re-add: addr=%s",
                             revived_addr);
                }
                break;
            }
        }
        if (!dup)
            cm->addnodes[cm->num_addnodes++] = *addr;
    }

    /* Already connected (e.g. repeated RPC addnode): connect_node would just
     * dedupe-return the existing node. Skip the redundant dedupe round-trip. */
    if (connman_addr_is_connected(cm, addr))
        return;

    /* Pass addr_name as dest so connect_node skips is_local check.
     * This allows connecting to localhost (e.g. local zclassicd peer). */
    char dest[64];
    net_service_to_string(&addr->svc, dest, sizeof(dest));
    peer_lifecycle_note_attempt(addr, PEER_LIFECYCLE_SOURCE_MANUAL);
    struct p2p_node *node = connect_node(&cm->manager,
                                         (struct net_address *)addr, dest);
    if (node) {
        peer_lifecycle_note_connected(node, PEER_LIFECYCLE_SOURCE_MANUAL);
        /* Drop the +1 caller ref connect_node returns. */
        connman_release_connect_node_ref(cm, node);
    }
}

bool connman_remove_addnode(struct connman *cm,
                            const struct net_address *addr)
{
    if (!cm || !addr)
        return false;

    for (int i = 0; i < cm->num_addnodes; i++) {
        if (!net_addr_eq(&cm->addnodes[i].svc.addr, &addr->svc.addr) ||
            cm->addnodes[i].svc.port != addr->svc.port)
            continue;

        const int last = cm->num_addnodes - 1;
        for (int j = i; j < last; j++) {
            cm->addnodes[j] = cm->addnodes[j + 1];
            cm->addnode_last_attempt[j] = cm->addnode_last_attempt[j + 1];
            cm->addnode_backoff_sec[j] = cm->addnode_backoff_sec[j + 1];
            cm->addnode_tcp_failures[j] = cm->addnode_tcp_failures[j + 1];
            cm->addnode_protocol_failures[j] =
                cm->addnode_protocol_failures[j + 1];
            cm->addnode_first_failure_ts[j] =
                cm->addnode_first_failure_ts[j + 1];
            cm->addnode_retired[j] = cm->addnode_retired[j + 1];
            cm->addnode_retired_at[j] = cm->addnode_retired_at[j + 1];
        }

        memset(&cm->addnodes[last], 0, sizeof(cm->addnodes[last]));
        cm->addnode_last_attempt[last] = 0;
        cm->addnode_backoff_sec[last] = 0;
        cm->addnode_tcp_failures[last] = 0;
        cm->addnode_protocol_failures[last] = 0;
        cm->addnode_first_failure_ts[last] = 0;
        cm->addnode_retired[last] = false;
        cm->addnode_retired_at[last] = 0;
        cm->num_addnodes = last;

        if (cm->num_addnodes <= 0) {
            cm->next_addnode_cursor = 0;
        } else if (cm->next_addnode_cursor > (size_t)i) {
            cm->next_addnode_cursor--;
        } else if (cm->next_addnode_cursor >= (size_t)cm->num_addnodes) {
            cm->next_addnode_cursor = 0;
        }
        return true;
    }

    return false;
}

size_t connman_get_node_count(const struct connman *cm)
{
    struct net_manager *nm = (struct net_manager *)&cm->manager;
    zcl_mutex_lock(&nm->cs_nodes);
    size_t n = nm->num_nodes;
    zcl_mutex_unlock(&nm->cs_nodes);
    return n;
}

size_t connman_outbound_healthy_count(struct connman *cm)
{
    if (!cm) return 0;
    size_t healthy = 0;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        const struct p2p_node *n = cm->manager.nodes[i];
        if (n && !n->inbound && !n->disconnect && !n->is_feeler &&
            n->state >= PEER_HANDSHAKE_COMPLETE &&
            (n->services & NODE_NETWORK) != 0)
            healthy++;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return healthy;
}

size_t connman_handshaked_peer_count(struct connman *cm)
{
    if (!cm) return 0;
    size_t n = 0;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        const struct p2p_node *p = cm->manager.nodes[i];
        if (p && !p->disconnect && p->state >= PEER_HANDSHAKE_COMPLETE)
            n++;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return n;
}

struct addr_man *connman_addrman(struct connman *cm)
{
    return cm ? &cm->manager.addrman : NULL;
}

int connman_max_peer_height(struct connman *cm)
{
    if (!cm) return -1;
    int max_height = -1;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        const struct p2p_node *node = cm->manager.nodes[i];
        if (!node || node->disconnect || node->is_feeler ||
            node->state < PEER_HANDSHAKE_COMPLETE ||
            (node->services & NODE_NETWORK) == 0)
            continue;
        if (node->starting_height > max_height)
            max_height = node->starting_height;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return max_height;
}

int connman_force_outbound_rotation(struct connman *cm, const char *reason)
{
    if (!cm) return 0;
    int disconnected = 0;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *node = cm->manager.nodes[i];
        if (node && !node->inbound && !node->disconnect) {
            node->disconnect = true;
            disconnected++;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    printf("[connman] outbound rotation reason=%s disconnected=%d\n",
           reason ? reason : "unspecified", disconnected);
    connman_kick_seed_discovery(cm);
    return disconnected;
}

void connman_get_outbound_health(struct connman *cm,
                                 struct connman_outbound_health *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cm) return;

    out->addnode_count = cm->num_addnodes > 0 ? (size_t)cm->num_addnodes : 0;
    for (int i = 0; i < cm->num_addnodes; i++) {
        out->addnode_tcp_failures += cm->addnode_tcp_failures[i];
        out->addnode_protocol_failures +=
            cm->addnode_protocol_failures[i];
        if (cm->addnode_backoff_sec[i] > 0) {
            out->addnode_backoff_active++;
            if (cm->addnode_backoff_sec[i] > out->addnode_backoff_max_sec)
                out->addnode_backoff_max_sec = cm->addnode_backoff_sec[i];
        }
        if (cm->addnode_retired[i])
            out->addnode_retired_count++;
    }

    uint16_t groups[DEFAULT_MAX_PEER_CONNECTIONS];
    size_t group_counts[DEFAULT_MAX_PEER_CONNECTIONS];
    size_t num_groups = 0;
    uint16_t healthy_groups[DEFAULT_MAX_PEER_CONNECTIONS];
    size_t healthy_group_counts[DEFAULT_MAX_PEER_CONNECTIONS];
    size_t healthy_num_groups = 0;
    memset(groups, 0, sizeof(groups));
    memset(group_counts, 0, sizeof(group_counts));
    memset(healthy_groups, 0, sizeof(healthy_groups));
    memset(healthy_group_counts, 0, sizeof(healthy_group_counts));

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        const struct p2p_node *n = cm->manager.nodes[i];
        if (!n || n->disconnect || n->is_feeler)
            continue;

        bool handshaked = n->state >= PEER_HANDSHAKE_COMPLETE;
        bool block_serving = handshaked &&
            (n->services & NODE_NETWORK) != 0;
        if (n->inbound) {
            out->inbound_total++;
            if (block_serving)
                out->inbound_healthy++;
            else if (!handshaked)
                out->inbound_handshake_incomplete++;
            continue;
        }

        out->outbound_total++;
        if (n->state == PEER_CONNECTING)
            out->connecting++;
        if (!handshaked)
            out->handshake_incomplete++;
        else if (block_serving)
            out->healthy++;

        if (net_addr_is_ipv4(&n->addr.svc.addr)) {
            uint16_t group = ipv4_group16(n->addr.svc.addr.ip);
            size_t gi = 0;
            for (; gi < num_groups; gi++) {
                if (groups[gi] == group)
                    break;
            }
            if (gi == num_groups &&
                num_groups < DEFAULT_MAX_PEER_CONNECTIONS) {
                groups[num_groups] = group;
                group_counts[num_groups] = 0;
                num_groups++;
            }
            if (gi < num_groups) {
                group_counts[gi]++;
                if (group_counts[gi] > out->ipv4_max_group_size)
                    out->ipv4_max_group_size = group_counts[gi];
            }
            if (block_serving) {
                size_t hgi = 0;
                for (; hgi < healthy_num_groups; hgi++) {
                    if (healthy_groups[hgi] == group)
                        break;
                }
                if (hgi == healthy_num_groups &&
                    healthy_num_groups < DEFAULT_MAX_PEER_CONNECTIONS) {
                    healthy_groups[healthy_num_groups] = group;
                    healthy_group_counts[healthy_num_groups] = 0;
                    healthy_num_groups++;
                }
                if (hgi < healthy_num_groups) {
                    healthy_group_counts[hgi]++;
                    if (healthy_group_counts[hgi] >
                        out->healthy_ipv4_max_group_size)
                        out->healthy_ipv4_max_group_size =
                            healthy_group_counts[hgi];
                }
            }
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    out->ipv4_group_count = num_groups;
    out->healthy_ipv4_group_count = healthy_num_groups;
}

void connman_get_message_cycle_stats(
    struct connman *cm,
    struct connman_message_cycle_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cm) return;

    out->cycles = atomic_load_explicit(&cm->message_cycles,
                                       memory_order_relaxed);
    out->nodes_snapshotted = atomic_load_explicit(
        &cm->message_nodes_snapshotted, memory_order_relaxed);
    out->send_calls = atomic_load_explicit(&cm->message_send_calls,
                                           memory_order_relaxed);
    out->process_calls = atomic_load_explicit(&cm->message_process_calls,
                                              memory_order_relaxed);
    out->recv_ready = atomic_load_explicit(&cm->message_recv_ready,
                                           memory_order_relaxed);
    out->idle_waits = atomic_load_explicit(&cm->message_idle_waits,
                                           memory_order_relaxed);
    out->wakes = atomic_load_explicit(&cm->message_wakes,
                                      memory_order_relaxed);
}
