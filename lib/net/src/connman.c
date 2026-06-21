/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#define _GNU_SOURCE  /* pthread_timedjoin_np */

#define _DEFAULT_SOURCE
#include "platform/time_compat.h"
#include "net/connman.h"
#include "net/addrman.h"
#include "event/event.h"
#include "net/peer_bandwidth.h"
#include "net/peer_lifecycle.h"
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
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/utiltime.h"
#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "util/thread_registry.h"

extern bool msg_version_get_external_ip(char *buf, size_t buflen,
                                        uint16_t *port);

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

static void *thread_dns_seed(void *arg)
{
    struct connman *cm = (struct connman *)arg;

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
    const int PEER_FLOOR_MIN = 3;
    const int PEER_FLOOR_GRACE_SECS = 120;
    int64_t start_ts = (int64_t)platform_time_wall_time_t();
    int64_t floor_below_since = 0;
    while (!g_stop) {
        size_t n = cm->manager.num_nodes;
        int interval = (n == 0) ? 30 : (n < 3) ? 60 : 300;
        sleep(interval);
        if (g_stop) break;
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
             * seeds while below the floor. Blocking fetches are fine
             * on this dedicated discovery thread. */
            if (tor_integration_is_ready()) {
                for (size_t si = 0;
                     si < cm->params->nOnionSeeds && !g_stop; si++)
                    try_onion_seed_fetch(cm, cm->params->onionSeeds[si]);
            }
        } else {
            floor_below_since = 0;
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

static bool connman_addrman_port_allowed(uint16_t port)
{
    return port == 8033 || port == 18033 || port == 8034 ||
           port == 9033 || port == 20022;
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

    if (!connman_addrman_port_allowed(info->addr.svc.port))
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

    if (net_addr_is_ipv4(&info->addr.svc.addr)) {
        uint16_t group = ipv4_group16(info->addr.svc.addr.ip);
        if (connman_outbound_group_count(cm, group) >=
                MAX_OUTBOUND_PER_GROUP16)
            return false;
    }

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
    if (kind == CONNMAN_ADDNODE_FAILURE_PROTOCOL)
        n = ++cm->addnode_protocol_failures[addnode_index];
    else
        n = ++cm->addnode_tcp_failures[addnode_index];

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

static void *thread_open_connections(void *arg)
{
    struct connman *cm = (struct connman *)arg;

    static int64_t s_last_addrman_attempt = 0;

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
        size_t outbound_slot = 0;
        size_t outbound_healthy = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes; i++) {
            struct p2p_node *n = cm->manager.nodes[i];
            if (n->inbound || n->disconnect) continue;
            outbound_slot++;
            if (n->state >= PEER_HANDSHAKE_COMPLETE)
                outbound_healthy++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

        size_t outbound = outbound_slot;

        if (outbound >= MAX_OUTBOUND_CONNECTIONS ||
            cm->manager.num_nodes >= (size_t)cm->manager.max_connections) {
            sleep(1);
            continue;
        }

        /* In connect-only mode, only maintain 1 outbound connection
         * per addnode. Multiple connections cause snapshot serving to
         * split across connections and stall. */
        if (g_connect_only && outbound >= 1) {
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
                    }
                }
            }
        }

        /* Rate-limited addrman outbound: 1 attempt per 10s to avoid
         * flooding. Below the healthy-peer floor (3 fully-handshaked
         * outbound) we try harder — peers stuck in PEER_CONNECTING
         * don't count, so a node with 1 working peer + 4 sockets
         * stuck in connecting still gets aggressive backfill. */
        int64_t now_oc = (int64_t)platform_time_wall_time_t();
        const size_t OUTBOUND_HEALTHY_FLOOR = 3;
        bool below_floor = (outbound_healthy < OUTBOUND_HEALTHY_FLOOR);
        bool rate_ok = below_floor ||
                       (now_oc - s_last_addrman_attempt >= 10);
        int attempts = 0;
        if (rate_ok && !tried_zcl23) {
            if (outbound_healthy == 0)
                attempts = 3;
            else if (below_floor)
                attempts = 2;
            else
                attempts = 1;
        }
        for (int a = 0; a < attempts && !g_stop; a++) {
            struct addr_info info;
            enum connman_outbound_target_source source;
            size_t addnode_index = SIZE_MAX;
            memset(&info, 0, sizeof(info));
            if (!connman_pick_next_outbound_target(cm,
                                                   &cm->next_addnode_cursor,
                                                   &info,
                                                   &source,
                                                   &addnode_index))
                continue;

            if (source == CONNMAN_TARGET_ADDRMAN) {
                /* Skip non-ZClassic ports (16125 etc from corrupted addrman).
                 * ZClassic mainnet uses port 8033. */
                uint16_t port = info.addr.svc.port;
                if (port != 8033 && port != 18033 && port != 8034 &&
                    port != 9033 && port != 20022)
                    continue;
            }

            /* Skip already-connected peers (compare IP only, not port —
             * inbound peers connect from ephemeral ports) */
            bool already_connected = false;
            zcl_mutex_lock(&cm->manager.cs_nodes);
            for (size_t ni = 0; ni < cm->manager.num_nodes; ni++) {
                struct p2p_node *n = cm->manager.nodes[ni];
                if (connman_node_conflicts_with_target(n, &info.addr)) {
                    already_connected = true;
                    if (n->addr.svc.port != info.addr.svc.port) {
                        char skip_buf[64];
                        net_addr_to_string(&info.addr.svc.addr, skip_buf, sizeof(skip_buf));
                        printf("Skipping %s — already connected outbound\n", skip_buf);
                    }
                    break;
                }
            }
            zcl_mutex_unlock(&cm->manager.cs_nodes);
            if (already_connected) {
                if (source == CONNMAN_TARGET_ADDNODE)
                    connman_record_addnode_attempt(cm, addnode_index, true);
                continue;
            }

            /* Eclipse attack defense: limit outbound peers per /16 subnet */
            if (source == CONNMAN_TARGET_ADDRMAN &&
                net_addr_is_ipv4(&info.addr.svc.addr)) {
                uint16_t group = ipv4_group16(info.addr.svc.addr.ip);
                zcl_mutex_lock(&cm->manager.cs_nodes);
                int in_group = count_outbound_in_group(&cm->manager, group);
                zcl_mutex_unlock(&cm->manager.cs_nodes);
                if (in_group >= MAX_OUTBOUND_PER_GROUP16)
                    continue;
            }

            s_last_addrman_attempt = now_oc;
            char dest[64];
            char ipbuf[64];
            struct p2p_node *node = NULL;

            net_addr_to_string(&info.addr.svc.addr, ipbuf, sizeof(ipbuf));
            if (source == CONNMAN_TARGET_ADDNODE) {
                net_service_to_string(&info.addr.svc, dest, sizeof(dest));
                peer_lifecycle_note_attempt(&info.addr,
                                            PEER_LIFECYCLE_SOURCE_ADDNODE);
                node = connect_node(&cm->manager, &info.addr, dest);
            } else {
                peer_lifecycle_note_attempt(&info.addr,
                                            PEER_LIFECYCLE_SOURCE_ADDRMAN);
                node = connect_node(&cm->manager, &info.addr, NULL);
            }

            if (!node) {
                if (source == CONNMAN_TARGET_ADDRMAN) {
                    addrman_attempt(&cm->manager.addrman, &info.addr.svc,
                                    (int64_t)platform_time_wall_time_t());
                } else {
                    connman_record_addnode_attempt(cm, addnode_index, false);
                }
                event_emitf(EV_TCP_CONNECT_FAILED, 0,
                            "%s:%u", ipbuf, info.addr.svc.port);
            } else {
                if (source == CONNMAN_TARGET_ADDNODE)
                    connman_record_addnode_attempt(cm, addnode_index, true);
                peer_lifecycle_note_connected(
                    node,
                    source == CONNMAN_TARGET_ADDNODE
                        ? PEER_LIFECYCLE_SOURCE_ADDNODE
                        : PEER_LIFECYCLE_SOURCE_ADDRMAN);
                printf("Outbound diversity: connected to %s:%u (%zu/%d outbound)\n",
                       ipbuf, info.addr.svc.port, outbound + 1,
                       MAX_OUTBOUND_CONNECTIONS);
            }
        }

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
         * which caused stack corruption with high fd numbers. */
        struct pollfd pfds[256]; /* max: 8 listen + ~200 peers */
        size_t npfds = 0;
        size_t listen_count = cm->manager.num_listen_sockets;

        /* Add listen sockets */
        for (size_t i = 0; i < listen_count && npfds < 256; i++) {
            pfds[npfds].fd = (int)cm->manager.listen_sockets[i].socket;
            pfds[npfds].events = POLLIN;
            pfds[npfds].revents = 0;
            npfds++;
        }

        /* Add connected nodes — snapshot fd + index under lock */
        struct { int fd; size_t node_idx; } node_fds[200];
        size_t n_node_fds = 0;
        zcl_mutex_lock(&cm->manager.cs_nodes);
        for (size_t i = 0; i < cm->manager.num_nodes && npfds < 256; i++) {
            struct p2p_node *node = cm->manager.nodes[i];
            int fd = (int)node->socket;
            if (fd < 0) continue;
            short events = POLLIN;
            if (node->send_size > 0)
                events |= POLLOUT;
            pfds[npfds].fd = fd;
            pfds[npfds].events = events;
            pfds[npfds].revents = 0;
            if (n_node_fds < 200) {
                node_fds[n_node_fds].fd = fd;
                node_fds[n_node_fds].node_idx = i;
                n_node_fds++;
            }
            npfds++;
        }
        zcl_mutex_unlock(&cm->manager.cs_nodes);

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
                if (node->recv_msg_count >= MAX_RECV_MESSAGES) {
                    zcl_mutex_unlock(&node->cs_recv);
                    goto skip_recv;
                }
                char buf[0x10000];
                /* Cap recv size to bandwidth budget. */
                size_t recv_cap = sizeof(buf);
                if (bw_avail < recv_cap) recv_cap = bw_avail;
                ssize_t n = recv(target_fd, buf, recv_cap, MSG_DONTWAIT);
                if (n > 0) {
                    if (!p2p_node_receive_bytes(node, buf, (unsigned int)n,
                                                cm->manager.message_start)) {
                        connman_note_addnode_prehandshake_disconnect(
                            cm, node, "message-parse");
                        node->disconnect = true;
                    }
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

    /* Phase 2: drive callbacks against the local copy, NO lock held. */
    for (size_t i = 0; i < snap_count; i++) {
        struct p2p_node *node = snap[i];
        /* Peer may have disconnected mid-iteration — ref_count still
         * keeps the pointer valid, but there's no point talking to a
         * dead socket. */
        if (node->disconnect) continue;

        bool has_recv_messages = false;
        zcl_mutex_lock(&node->cs_recv);
        has_recv_messages = node->recv_msg_count > 0;
        zcl_mutex_unlock(&node->cs_recv);

        if (has_recv_messages && cm->manager.signals.process_messages) {
            cm->manager.signals.process_messages(
                cm->manager.signals.ctx, node);
            did_work = true;
        }

        if (!node->disconnect && cm->manager.signals.send_messages) {
            bool trickle = trickle_denom > 0 &&
                           (GetRand(trickle_denom) == 0);
            cm->manager.signals.send_messages(
                cm->manager.signals.ctx, node, trickle);
            /* Keep tight loop when serving snapshot — don't sleep */
            if (node->state == PEER_SNAPSHOT_SERVING ||
                node->state == PEER_SNAPSHOT_RECEIVING)
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

    while (!g_stop) {
        bool did_work = connman_run_message_cycle(cm);
        if (!did_work)
            usleep(100000);
    }
    return NULL;
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
    cm->manager.local_host_nonce = GetRand(UINT64_MAX);
    snprintf(cm->manager.sub_version, MAX_SUBVERSION_LENGTH,
             "/MagicBean:2.1.2-beta1/ZClassic-C23:1.0.0/");

    return true;
}

bool connman_start(struct connman *cm)
{
    if (!cm)
        LOG_FAIL("net", "connman_start called with NULL connman");
    if (cm->started)
        return true;

    /* Initialize per-peer bandwidth quotas from env vars. */
    if (!g_peer_bw_active) {
        peer_bandwidth_init(&g_peer_bw);
        peer_bandwidth_load_from_env(&g_peer_bw);
        g_peer_bw_active = true;
    }

    g_stop = false;

    if (thread_registry_spawn_ex("zcl_dns_seed", thread_dns_seed, cm,
                                  &g_thread_dns_seed) != 0) {
        perror("connman: thread_registry_spawn dns_seed");
        g_stop = true;
        LOG_FAIL("net", "thread_registry_spawn_ex failed for dns_seed thread");
    }
    cm->dns_seed_thread_started = true;

    if (thread_registry_spawn_ex("zcl_connman_sock", thread_socket_handler,
                                  cm, &g_thread_socket) != 0) {
        perror("connman: thread_registry_spawn socket");
        g_stop = true;
        pthread_join(g_thread_dns_seed, NULL);
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "thread_registry_spawn_ex failed for socket_handler thread");
    }
    cm->socket_thread_started = true;

    if (thread_registry_spawn_ex("zcl_connman_open", thread_open_connections,
                                  cm, &g_thread_open) != 0) {
        perror("connman: thread_registry_spawn open");
        g_stop = true;
        pthread_join(g_thread_socket, NULL);
        pthread_join(g_thread_dns_seed, NULL);
        cm->socket_thread_started = false;
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "thread_registry_spawn_ex failed for open_connections thread");
    }
    cm->open_thread_started = true;

    if (thread_registry_spawn_ex("zcl_connman_msg", thread_message_handler,
                                  cm, &g_thread_message) != 0) {
        perror("connman: thread_registry_spawn message");
        g_stop = true;
        pthread_join(g_thread_open, NULL);
        pthread_join(g_thread_socket, NULL);
        pthread_join(g_thread_dns_seed, NULL);
        cm->open_thread_started = false;
        cm->socket_thread_started = false;
        cm->dns_seed_thread_started = false;
        LOG_FAIL("net", "thread_registry_spawn_ex failed for message_handler thread");
    }
    cm->message_thread_started = true;

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
        }
        if (cm->socket_thread_started) {
            if (!timed_join(g_thread_socket, 5))
                LOG_WARN("connman", "socket thread join timed out");
            cm->socket_thread_started = false;
        }
        if (cm->open_thread_started) {
            if (!timed_join(g_thread_open, 5))
                LOG_WARN("connman", "open thread join timed out");
            cm->open_thread_started = false;
        }
        if (cm->message_thread_started) {
            if (!timed_join(g_thread_message, 5))
                LOG_WARN("connman", "message thread join timed out");
            cm->message_thread_started = false;
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
}

void connman_load_addrman(struct connman *cm)
{
    if (!cm->datadir) return;
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
                break;
            }
        }
        if (!dup)
            cm->addnodes[cm->num_addnodes++] = *addr;
    }

    /* Already connected (e.g. repeated RPC addnode): connect_node would
     * dedupe-return the existing node with an extra borrowed ref this
     * caller has no way to release — skip the dial entirely. */
    if (connman_addr_is_connected(cm, addr))
        return;

    /* Pass addr_name as dest so connect_node skips is_local check.
     * This allows connecting to localhost (e.g. local zclassicd peer). */
    char dest[64];
    net_service_to_string(&addr->svc, dest, sizeof(dest));
    peer_lifecycle_note_attempt(addr, PEER_LIFECYCLE_SOURCE_MANUAL);
    struct p2p_node *node = connect_node(&cm->manager,
                                         (struct net_address *)addr, dest);
    if (node)
        peer_lifecycle_note_connected(node, PEER_LIFECYCLE_SOURCE_MANUAL);
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
        if (n && !n->inbound && !n->disconnect &&
            n->state >= PEER_HANDSHAKE_COMPLETE)
            healthy++;
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);
    return healthy;
}

int connman_max_peer_height(struct connman *cm)
{
    if (!cm) return -1;
    int max_height = -1;
    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        const struct p2p_node *node = cm->manager.nodes[i];
        if (node && node->starting_height > max_height)
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
        if (!n || n->disconnect)
            continue;

        bool handshaked = n->state >= PEER_HANDSHAKE_COMPLETE;
        if (n->inbound) {
            out->inbound_total++;
            if (handshaked)
                out->inbound_healthy++;
            else
                out->inbound_handshake_incomplete++;
            continue;
        }

        out->outbound_total++;
        if (n->state == PEER_CONNECTING)
            out->connecting++;
        if (n->state < PEER_HANDSHAKE_COMPLETE)
            out->handshake_incomplete++;
        else
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
            if (handshaked) {
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
