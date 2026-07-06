/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "net/peer_lifecycle.h"

#include "core/utiltime.h"
#include "event/event.h"
#include "net/fast_sync.h"
#include "util/log_macros.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define PEER_LIFECYCLE_MAX 1024
#define PEER_LIFECYCLE_INCIDENT_LIMIT 16
#define PEER_LIFECYCLE_GROUP_LIMIT 64

struct peer_lifecycle_entry {
    bool used;
    int64_t peer_id;
    char addr[256];
    enum peer_lifecycle_source source;
    int64_t first_seen;
    int64_t last_seen;
    int64_t connected_at;
    uint64_t connected_seq;
    int64_t version_sent_at;
    int64_t version_received_at;
    int64_t verack_received_at;
    int64_t handshake_complete_at;
    uint64_t handshake_complete_seq;
    int64_t active_at;
    int64_t disconnected_at;
    int64_t timeout_at;
    int64_t rejected_at;
    uint64_t terminal_seq;
    int64_t attempted;
    int64_t connected;
    int64_t version_sent;
    int64_t version_received;
    int64_t verack_received;
    int64_t handshake_complete;
    int64_t active;
    int64_t disconnected;
    int64_t timeout;
    int64_t rejected;
    int64_t cache_skipped;
    int64_t pre_handshake_disconnects;
    uint64_t services;
    int start_height;
    char subver[MAX_SUBVERSION_LENGTH];
    char last_reason[128];
};

struct peer_lifecycle_host_group {
    bool used;
    char host[256];
    int64_t entries;
    int64_t inbound_entries;
    int64_t outbound_entries;
    int64_t unknown_entries;
    int64_t open_connections;
    int64_t handshaked_open_connections;
    int64_t bootstrap_useful_connections;
    int64_t fast_sync_useful_connections;
    int64_t connected;
    int64_t handshake_complete;
    int64_t active;
    int64_t disconnected;
    int64_t timeout;
    int64_t rejected;
    int64_t reconnects;
    int64_t pre_handshake_disconnects;
    int64_t last_seen;
    int64_t max_advertised_height;
    uint64_t services_or;
    bool bootstrap_useful;
    bool fast_sync_useful;
    char last_reason[128];
};

struct peer_lifecycle_incident_pick {
    const struct peer_lifecycle_entry *entry;
    int64_t score;
    int64_t duplicate_host_entries;
    char host[256];
};

static struct {
    pthread_mutex_t lock;
    struct peer_lifecycle_entry entries[PEER_LIFECYCLE_MAX];
    struct peer_lifecycle_summary totals;
    uint64_t seq;
} g_pl = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

const char *peer_lifecycle_source_name(enum peer_lifecycle_source source)
{
    switch (source) {
        case PEER_LIFECYCLE_SOURCE_INBOUND:  return "inbound";
        case PEER_LIFECYCLE_SOURCE_ADDNODE:  return "addnode";
        case PEER_LIFECYCLE_SOURCE_ADDRMAN:  return "addrman";
        case PEER_LIFECYCLE_SOURCE_ZCL23_DB: return "zcl23_db";
        case PEER_LIFECYCLE_SOURCE_MANUAL:   return "manual";
        case PEER_LIFECYCLE_SOURCE_UNKNOWN:
        default:                             return "unknown";
    }
}

static void addr_key_from_netaddr(const struct net_address *addr,
                                  char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!addr) {
        snprintf(out, out_sz, "unknown");
        return;
    }
    net_service_to_string(&addr->svc, out, out_sz);
}

static void addr_key_from_node(const struct p2p_node *node,
                               char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (node && node->addr_name[0])
        snprintf(out, out_sz, "%s", node->addr_name);
    else if (node)
        addr_key_from_netaddr(&node->addr, out, out_sz);
    else
        snprintf(out, out_sz, "unknown");
}

static struct peer_lifecycle_entry *find_entry_locked(const char *addr,
                                                      int64_t peer_id,
                                                      bool create)
{
    struct peer_lifecycle_entry *free_slot = NULL;
    for (size_t i = 0; i < PEER_LIFECYCLE_MAX; i++) {
        struct peer_lifecycle_entry *e = &g_pl.entries[i];
        if (!e->used) {
            if (!free_slot) free_slot = e;
            continue;
        }
        if (peer_id >= 0 && e->peer_id == peer_id)
            return e;
        if (addr && addr[0] && strcmp(e->addr, addr) == 0)
            return e;
    }
    if (!create)
        return NULL;
    struct peer_lifecycle_entry *e = free_slot;
    if (!e)
        e = &g_pl.entries[(size_t)(GetTime() % PEER_LIFECYCLE_MAX)];
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->peer_id = peer_id;
    e->first_seen = GetTime();
    e->last_seen = e->first_seen;
    e->source = PEER_LIFECYCLE_SOURCE_UNKNOWN;
    if (addr && addr[0])
        snprintf(e->addr, sizeof(e->addr), "%s", addr);
    return e;
}

static struct peer_lifecycle_entry *entry_for_node_locked(
    const struct p2p_node *node, bool create)
{
    char addr[256];
    addr_key_from_node(node, addr, sizeof(addr));
    return find_entry_locked(addr, node ? node->id : -1, create);
}

static bool subver_is_magicbean(const char *subver)
{
    return subver && strstr(subver, "MagicBean") != NULL;
}

static bool subver_is_zcl23(const char *subver, uint64_t services)
{
    return peer_supports_fast_sync(services) ||
           (subver && (strstr(subver, "ZClassic23") != NULL ||
                       strstr(subver, "ZClassic-C23") != NULL));
}

static int64_t handshake_duration_secs(const struct peer_lifecycle_entry *e)
{
    if (!e || e->connected_at <= 0 || e->handshake_complete_at <= 0)
        return 0;
    if (e->handshake_complete_at <= e->connected_at)
        return 0;
    return e->handshake_complete_at - e->connected_at;
}

static void addr_host_key(const char *addr, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!addr || !addr[0]) {
        snprintf(out, out_sz, "unknown");
        return;
    }

    if (addr[0] == '[') {
        const char *end = strchr(addr, ']');
        if (end && end > addr + 1) {
            size_t n = (size_t)(end - addr - 1);
            if (n >= out_sz)
                n = out_sz - 1;
            memcpy(out, addr + 1, n);
            out[n] = '\0';
            return;
        }
    }

    const char *first_colon = strchr(addr, ':');
    const char *last_colon = strrchr(addr, ':');
    if (first_colon && first_colon == last_colon && first_colon > addr) {
        size_t n = (size_t)(first_colon - addr);
        if (n >= out_sz)
            n = out_sz - 1;
        memcpy(out, addr, n);
        out[n] = '\0';
        return;
    }

    snprintf(out, out_sz, "%s", addr);
}

static bool source_is_inbound(enum peer_lifecycle_source source)
{
    return source == PEER_LIFECYCLE_SOURCE_INBOUND;
}

static bool source_is_outbound(enum peer_lifecycle_source source)
{
    return source == PEER_LIFECYCLE_SOURCE_ADDNODE ||
           source == PEER_LIFECYCLE_SOURCE_ADDRMAN ||
           source == PEER_LIFECYCLE_SOURCE_ZCL23_DB ||
           source == PEER_LIFECYCLE_SOURCE_MANUAL;
}

static const char *entry_direction(const struct peer_lifecycle_entry *e)
{
    if (!e)
        return "unknown";
    if (source_is_inbound(e->source))
        return "inbound";
    if (source_is_outbound(e->source))
        return "outbound";
    return "unknown";
}

static bool entry_connection_open(const struct peer_lifecycle_entry *e)
{
    if (!e || e->connected_seq == 0)
        return false;
    return e->connected_seq > e->terminal_seq;
}

static bool entry_current_connection_handshaked(
    const struct peer_lifecycle_entry *e)
{
    if (!entry_connection_open(e))
        return false;
    return e->handshake_complete_seq >= e->connected_seq &&
           e->handshake_complete_seq > e->terminal_seq;
}

static int64_t entry_handshake_age_secs(const struct peer_lifecycle_entry *e,
                                        int64_t now)
{
    if (!entry_current_connection_handshaked(e))
        return -1;
    if (now <= e->handshake_complete_at)
        return 0;
    return now - e->handshake_complete_at;
}

static int64_t entry_reconnects(const struct peer_lifecycle_entry *e)
{
    if (!e || e->connected <= 1)
        return 0;
    return e->connected - 1;
}

static void append_summary_token(char *out, size_t out_sz, const char *token)
{
    if (!out || out_sz == 0 || !token || !token[0])
        return;
    size_t used = strlen(out);
    if (used >= out_sz - 1)
        return;
    int n = snprintf(out + used, out_sz - used, "%s%s",
                     used > 0 ? "|" : "", token);
    if (n < 0)
        out[out_sz - 1] = '\0';
}

static void services_summary(uint64_t services, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if ((services & NODE_NETWORK) != 0)
        append_summary_token(out, out_sz, "NODE_NETWORK");
    if ((services & NODE_BLOOM) != 0)
        append_summary_token(out, out_sz, "NODE_BLOOM");
    if ((services & NODE_ZCL23) != 0)
        append_summary_token(out, out_sz, "NODE_ZCL23");
    uint64_t known = NODE_NETWORK | NODE_BLOOM | NODE_ZCL23;
    uint64_t unknown = services & ~known;
    if (unknown != 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "UNKNOWN_0x%llx",
                 (unsigned long long)unknown);
        append_summary_token(out, out_sz, buf);
    }
    if (out[0] == '\0')
        snprintf(out, out_sz, "none");
}

static bool entry_bootstrap_useful(const struct peer_lifecycle_entry *e)
{
    return entry_current_connection_handshaked(e) &&
           (e->services & NODE_NETWORK) != 0 &&
           e->start_height > 0;
}

static const char *entry_bootstrap_readiness(
    const struct peer_lifecycle_entry *e)
{
    if (!e || !entry_connection_open(e))
        return "not_connected";
    if (!entry_current_connection_handshaked(e))
        return "handshake_incomplete";
    if ((e->services & NODE_NETWORK) == 0)
        return "missing_NODE_NETWORK";
    if (e->start_height <= 0)
        return "missing_advertised_height";
    return "useful";
}

static bool entry_fast_sync_useful(const struct peer_lifecycle_entry *e)
{
    return entry_bootstrap_useful(e) &&
           subver_is_zcl23(e->subver, e->services);
}

static bool entry_cache_skip_actionable(const struct peer_lifecycle_entry *e)
{
    if (!e || e->cache_skipped <= 0)
        return false;
    if (source_is_inbound(e->source) &&
        entry_current_connection_handshaked(e) &&
        strcmp(e->last_reason, "inbound_ephemeral_port") == 0)
        return false;
    return true;
}

static int64_t entry_incident_score(const struct peer_lifecycle_entry *e,
                                    int64_t duplicate_host_entries)
{
    if (!e)
        return 0;
    int64_t score = 0;
    score += entry_reconnects(e) * 3;
    score += e->timeout * 3;
    score += e->rejected * 3;
    score += e->pre_handshake_disconnects * 2;
    score += e->disconnected;
    if (entry_cache_skip_actionable(e))
        score += e->cache_skipped;
    if (duplicate_host_entries > 1)
        score += (duplicate_host_entries - 1) * 2;
    return score;
}

void peer_lifecycle_note_attempt(const struct net_address *addr,
                                 enum peer_lifecycle_source source)
{
    char key[256];
    addr_key_from_netaddr(addr, key, sizeof(key));
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = find_entry_locked(key, -1, true);
    if (e) {
        e->attempted++;
        e->last_seen = GetTime();
        if (source != PEER_LIFECYCLE_SOURCE_UNKNOWN)
            e->source = source;
    }
    g_pl.totals.attempted++;
    pthread_mutex_unlock(&g_pl.lock);
    event_emitf(EV_PEER_HANDSHAKE_ATTEMPT, 0, "%s source=%s", key,
                peer_lifecycle_source_name(source));
}

void peer_lifecycle_note_connected(const struct p2p_node *node,
                                   enum peer_lifecycle_source source)
{
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = entry_for_node_locked(node, true);
    if (e) {
        int64_t now = GetTime();
        uint64_t seq = ++g_pl.seq;
        bool terminal_after_connect =
            e->terminal_seq > 0 && e->terminal_seq >= e->connected_seq;
        e->peer_id = node ? node->id : e->peer_id;
        e->connected++;
        if (e->connected_at == 0 || terminal_after_connect) {
            e->connected_at = now;
            e->connected_seq = seq;
            e->version_sent_at = 0;
            e->version_received_at = 0;
            e->verack_received_at = 0;
            e->handshake_complete_at = 0;
            e->handshake_complete_seq = 0;
            e->active_at = 0;
        }
        e->last_seen = now;
        if (source != PEER_LIFECYCLE_SOURCE_UNKNOWN)
            e->source = source;
    }
    g_pl.totals.connected++;
    pthread_mutex_unlock(&g_pl.lock);
}

void peer_lifecycle_note_version_sent(const struct p2p_node *node,
                                      uint64_t services,
                                      int start_height,
                                      const char *subver)
{
    (void)services;
    (void)start_height;
    (void)subver;
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = entry_for_node_locked(node, true);
    if (e) {
        e->version_sent++;
        e->version_sent_at = GetTime();
        e->last_seen = e->version_sent_at;
    }
    g_pl.totals.version_sent++;
    pthread_mutex_unlock(&g_pl.lock);
    if (node) {
        event_emitf(EV_PEER_HANDSHAKE_ATTEMPT, (uint32_t)node->id,
                    "version_sent addr=%s services=%llu height=%d subver=%s",
                    node->addr_name, (unsigned long long)services,
                    start_height, subver ? subver : "");
    }
}

void peer_lifecycle_note_version_received(const struct p2p_node *node,
                                          uint64_t services,
                                          int start_height,
                                          const char *subver)
{
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = entry_for_node_locked(node, true);
    if (e) {
        e->version_received++;
        e->version_received_at = GetTime();
        e->last_seen = e->version_received_at;
        e->services = services;
        e->start_height = start_height;
        snprintf(e->subver, sizeof(e->subver), "%s", subver ? subver : "");
    }
    g_pl.totals.version_received++;
    pthread_mutex_unlock(&g_pl.lock);
    if (node) {
        event_emitf(EV_PEER_VERSION, (uint32_t)node->id,
                    "version_received addr=%s services=%llu height=%d subver=%s",
                    node->addr_name, (unsigned long long)services,
                    start_height, subver ? subver : "");
    }
}

void peer_lifecycle_note_verack_received(const struct p2p_node *node)
{
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = entry_for_node_locked(node, true);
    if (e) {
        e->verack_received++;
        e->verack_received_at = GetTime();
        e->last_seen = e->verack_received_at;
    }
    g_pl.totals.verack_received++;
    pthread_mutex_unlock(&g_pl.lock);
}

void peer_lifecycle_note_handshake_complete(const struct p2p_node *node)
{
    bool magicbean = false;
    bool zcl23 = false;
    int64_t duration = 0;
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = entry_for_node_locked(node, true);
    if (e) {
        e->handshake_complete++;
        e->handshake_complete_at = GetTime();
        e->handshake_complete_seq = ++g_pl.seq;
        e->last_seen = e->handshake_complete_at;
        magicbean = subver_is_magicbean(e->subver);
        zcl23 = subver_is_zcl23(e->subver, e->services);
        duration = handshake_duration_secs(e);
    }
    g_pl.totals.handshake_complete++;
    if (magicbean) {
        g_pl.totals.magicbean_handshakes++;
        g_pl.totals.legacy_compatible_handshakes++;
    }
    if (zcl23) g_pl.totals.zcl23_handshakes++;
    pthread_mutex_unlock(&g_pl.lock);
    if (node) {
        event_emitf(EV_PEER_HANDSHAKE_SUCCESS, (uint32_t)node->id,
                    "addr=%s duration=%llds services=%llu subver=%s",
                    node->addr_name, (long long)duration,
                    (unsigned long long)node->services, node->sub_ver);
    }
}

void peer_lifecycle_note_active(const struct p2p_node *node)
{
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = entry_for_node_locked(node, true);
    if (e) {
        e->active++;
        e->active_at = GetTime();
        e->last_seen = e->active_at;
    }
    g_pl.totals.active++;
    pthread_mutex_unlock(&g_pl.lock);
}

static void note_terminal(const struct p2p_node *node, const char *reason,
                          const char *event_name, bool timeout, bool reject)
{
    char addr[256];
    addr_key_from_node(node, addr, sizeof(addr));
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = entry_for_node_locked(node, true);
    if (e) {
        int64_t now = GetTime();
        bool pre_handshake =
            e->handshake_complete_seq <= e->connected_seq;
        e->last_seen = now;
        snprintf(e->last_reason, sizeof(e->last_reason), "%s",
                 reason ? reason : "");
        e->terminal_seq = ++g_pl.seq;
        if (timeout) {
            e->timeout++;
            e->timeout_at = now;
        } else if (reject) {
            e->rejected++;
            e->rejected_at = now;
        } else {
            e->disconnected++;
            e->disconnected_at = now;
        }
        if (pre_handshake)
            g_pl.totals.pre_handshake_disconnects++;
        if (pre_handshake)
            e->pre_handshake_disconnects++;
    }
    if (timeout) g_pl.totals.timeout++;
    else if (reject) g_pl.totals.rejected++;
    else g_pl.totals.disconnected++;
    pthread_mutex_unlock(&g_pl.lock);
    if (node) {
        event_emitf(timeout ? EV_PEER_CONNECT_TIMEOUT :
                    reject ? EV_PEER_HANDSHAKE_FAILURE :
                    EV_TCP_DISCONNECTED,
                    (uint32_t)node->id, "%s addr=%s state=%s reason=%s",
                    event_name, addr, peer_state_name(node->state),
                    reason ? reason : "");
    }
}

void peer_lifecycle_note_timeout(const struct p2p_node *node,
                                 const char *reason)
{
    note_terminal(node, reason, "timeout", true, false);
}

void peer_lifecycle_note_reject(const struct p2p_node *node,
                                const char *reason)
{
    note_terminal(node, reason, "reject", false, true);
}

void peer_lifecycle_note_disconnected(const struct p2p_node *node,
                                      const char *reason)
{
    note_terminal(node, reason, "disconnect", false, false);
}

void peer_lifecycle_note_cache_skipped_addr(const char *addr,
                                            int64_t peer_id,
                                            const char *reason)
{
    const char *key = (addr && addr[0]) ? addr : "unknown";
    pthread_mutex_lock(&g_pl.lock);
    struct peer_lifecycle_entry *e = find_entry_locked(key, peer_id, true);
    if (e) {
        e->cache_skipped++;
        e->last_seen = GetTime();
        snprintf(e->last_reason, sizeof(e->last_reason), "%s",
                 reason ? reason : "");
    }
    g_pl.totals.cache_skipped++;
    pthread_mutex_unlock(&g_pl.lock);
    event_emitf(EV_PEER_CACHE_SKIPPED,
                peer_id >= 0 ? (uint32_t)peer_id : 0,
                "addr=%s reason=%s", key, reason ? reason : "");
}

void peer_lifecycle_note_cache_skipped(const struct p2p_node *node,
                                       const char *reason)
{
    char addr[256];
    addr_key_from_node(node, addr, sizeof(addr));
    peer_lifecycle_note_cache_skipped_addr(addr, node ? node->id : -1,
                                           reason);
}

static void entry_to_json(const struct peer_lifecycle_entry *e,
                          struct json_value *out)
{
    json_set_object(out);
    json_push_kv_int(out, "peer_id", e->peer_id);
    json_push_kv_str(out, "addr", e->addr);
    json_push_kv_str(out, "source", peer_lifecycle_source_name(e->source));
    json_push_kv_int(out, "attempted", e->attempted);
    json_push_kv_int(out, "connected", e->connected);
    json_push_kv_int(out, "version_sent", e->version_sent);
    json_push_kv_int(out, "version_received", e->version_received);
    json_push_kv_int(out, "verack_received", e->verack_received);
    json_push_kv_int(out, "handshake_complete", e->handshake_complete);
    json_push_kv_int(out, "active", e->active);
    json_push_kv_int(out, "disconnected", e->disconnected);
    json_push_kv_int(out, "timeout", e->timeout);
    json_push_kv_int(out, "rejected", e->rejected);
    json_push_kv_int(out, "cache_skipped", e->cache_skipped);
    json_push_kv_int(out, "pre_handshake_disconnects",
                     e->pre_handshake_disconnects);
    json_push_kv_str(out, "direction", entry_direction(e));
    json_push_kv_int(out, "reconnects", entry_reconnects(e));
    json_push_kv_bool(out, "connection_open", entry_connection_open(e));
    json_push_kv_bool(out, "current_connection_handshaked",
                      entry_current_connection_handshaked(e));
    json_push_kv_int(out, "handshake_age_secs",
                     entry_handshake_age_secs(e, GetTime()));
    json_push_kv_int(out, "first_seen", e->first_seen);
    json_push_kv_int(out, "last_seen", e->last_seen);
    json_push_kv_int(out, "connected_at", e->connected_at);
    json_push_kv_int(out, "handshake_complete_at", e->handshake_complete_at);
    json_push_kv_int(out, "handshake_duration_secs",
                     handshake_duration_secs(e));
    json_push_kv_int(out, "services", (int64_t)e->services);
    char summary[128];
    services_summary(e->services, summary, sizeof(summary));
    json_push_kv_str(out, "services_summary", summary);
    json_push_kv_int(out, "startingheight", e->start_height);
    json_push_kv_int(out, "advertised_height", e->start_height);
    json_push_kv_str(out, "subver", e->subver);
    json_push_kv_str(out, "bootstrap_readiness",
                     entry_bootstrap_readiness(e));
    json_push_kv_bool(out, "bootstrap_useful",
                      entry_bootstrap_useful(e));
    json_push_kv_bool(out, "fast_sync_useful",
                      entry_fast_sync_useful(e));
    json_push_kv_bool(out, "magicbean",
                      subver_is_magicbean(e->subver));
    json_push_kv_bool(out, "legacy_compatible",
                      subver_is_magicbean(e->subver));
    json_push_kv_bool(out, "zclassic23",
                      subver_is_zcl23(e->subver, e->services));
    json_push_kv_bool(out, "zclassic_c23",
                      subver_is_zcl23(e->subver, e->services));
    json_push_kv_str(out, "last_reason", e->last_reason);
    json_push_kv_str(out, "last_disconnect_reason", e->last_reason);
}

bool peer_lifecycle_peer_json(const struct p2p_node *node,
                              struct json_value *out)
{
    if (!node || !out) return false;
    pthread_mutex_lock(&g_pl.lock);
    const struct peer_lifecycle_entry *e = entry_for_node_locked(node, false);
    if (e)
        entry_to_json(e, out);
    pthread_mutex_unlock(&g_pl.lock);
    if (!e) {
        json_set_object(out);
        json_push_kv_str(out, "source",
                         node->inbound ? "inbound" : "unknown");
        json_push_kv_int(out, "handshake_duration_secs", 0);
        json_push_kv_bool(out, "magicbean",
                          subver_is_magicbean(node->sub_ver));
        json_push_kv_bool(out, "legacy_compatible",
                          subver_is_magicbean(node->sub_ver));
        json_push_kv_bool(out, "zclassic23",
                          subver_is_zcl23(node->sub_ver, node->services));
        json_push_kv_bool(out, "zclassic_c23",
                          subver_is_zcl23(node->sub_ver, node->services));
    }
    return true;
}

static void summary_to_json(const struct peer_lifecycle_summary *s,
                            struct json_value *out)
{
    json_set_object(out);
    json_push_kv_int(out, "attempted", s->attempted);
    json_push_kv_int(out, "connected", s->connected);
    json_push_kv_int(out, "version_sent", s->version_sent);
    json_push_kv_int(out, "version_received", s->version_received);
    json_push_kv_int(out, "verack_received", s->verack_received);
    json_push_kv_int(out, "handshake_complete", s->handshake_complete);
    json_push_kv_int(out, "active", s->active);
    json_push_kv_int(out, "disconnected", s->disconnected);
    json_push_kv_int(out, "timeout", s->timeout);
    json_push_kv_int(out, "rejected", s->rejected);
    json_push_kv_int(out, "cache_skipped", s->cache_skipped);
    json_push_kv_int(out, "magicbean_handshakes",
                     s->magicbean_handshakes);
    json_push_kv_int(out, "legacy_compatible_handshakes",
                     s->legacy_compatible_handshakes);
    json_push_kv_int(out, "legacy_magicbean_handshakes",
                     s->legacy_compatible_handshakes);
    json_push_kv_int(out, "zclassic23_handshakes",
                     s->zcl23_handshakes);
    json_push_kv_int(out, "zclassic_c23_handshakes",
                     s->zcl23_handshakes);
    json_push_kv_int(out, "pre_handshake_disconnects",
                     s->pre_handshake_disconnects);
}

static void summary_add_entry(struct peer_lifecycle_summary *s,
                              const struct peer_lifecycle_entry *e)
{
    if (!s || !e)
        return;

    s->attempted += e->attempted;
    s->connected += e->connected;
    s->version_sent += e->version_sent;
    s->version_received += e->version_received;
    s->verack_received += e->verack_received;
    s->handshake_complete += e->handshake_complete;
    s->active += e->active;
    s->disconnected += e->disconnected;
    s->timeout += e->timeout;
    s->rejected += e->rejected;
    s->cache_skipped += e->cache_skipped;
    if (e->handshake_complete > 0 && subver_is_magicbean(e->subver))
        s->magicbean_handshakes += e->handshake_complete;
    if (e->handshake_complete > 0 && subver_is_magicbean(e->subver))
        s->legacy_compatible_handshakes += e->handshake_complete;
    if (e->handshake_complete > 0 &&
        subver_is_zcl23(e->subver, e->services))
        s->zcl23_handshakes += e->handshake_complete;
    s->pre_handshake_disconnects += e->pre_handshake_disconnects;
}

static struct peer_lifecycle_host_group *find_host_group(
    struct peer_lifecycle_host_group *groups, const char *host,
    int64_t *overflow)
{
    struct peer_lifecycle_host_group *free_slot = NULL;
    for (size_t i = 0; i < PEER_LIFECYCLE_GROUP_LIMIT; i++) {
        if (!groups[i].used) {
            if (!free_slot)
                free_slot = &groups[i];
            continue;
        }
        if (strcmp(groups[i].host, host) == 0)
            return &groups[i];
    }
    if (!free_slot) {
        if (overflow)
            (*overflow)++;
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    snprintf(free_slot->host, sizeof(free_slot->host), "%s", host);
    return free_slot;
}

static void host_group_add_entry(struct peer_lifecycle_host_group *group,
                                 const struct peer_lifecycle_entry *e)
{
    if (!group || !e)
        return;
    group->entries++;
    if (source_is_inbound(e->source))
        group->inbound_entries++;
    else if (source_is_outbound(e->source))
        group->outbound_entries++;
    else
        group->unknown_entries++;
    if (entry_connection_open(e))
        group->open_connections++;
    if (entry_current_connection_handshaked(e))
        group->handshaked_open_connections++;
    if (entry_bootstrap_useful(e))
        group->bootstrap_useful_connections++;
    if (entry_fast_sync_useful(e))
        group->fast_sync_useful_connections++;
    group->connected += e->connected;
    group->handshake_complete += e->handshake_complete;
    group->active += e->active;
    group->disconnected += e->disconnected;
    group->timeout += e->timeout;
    group->rejected += e->rejected;
    group->reconnects += entry_reconnects(e);
    group->pre_handshake_disconnects += e->pre_handshake_disconnects;
    group->services_or |= e->services;
    if (e->start_height > group->max_advertised_height)
        group->max_advertised_height = e->start_height;
    if (entry_bootstrap_useful(e))
        group->bootstrap_useful = true;
    if (entry_fast_sync_useful(e))
        group->fast_sync_useful = true;
    if (e->last_seen >= group->last_seen) {
        group->last_seen = e->last_seen;
        snprintf(group->last_reason, sizeof(group->last_reason), "%s",
                 e->last_reason);
    }
}

static int64_t build_host_groups_locked(
    struct peer_lifecycle_host_group *groups)
{
    int64_t overflow = 0;
    for (size_t i = 0; i < PEER_LIFECYCLE_MAX; i++) {
        const struct peer_lifecycle_entry *e = &g_pl.entries[i];
        if (!e->used)
            continue;
        char host[256];
        addr_host_key(e->addr, host, sizeof(host));
        struct peer_lifecycle_host_group *group =
            find_host_group(groups, host, &overflow);
        host_group_add_entry(group, e);
    }
    return overflow;
}

static int64_t duplicate_entries_for_host(
    const struct peer_lifecycle_host_group *groups, const char *host)
{
    for (size_t i = 0; i < PEER_LIFECYCLE_GROUP_LIMIT; i++) {
        if (groups[i].used && strcmp(groups[i].host, host) == 0)
            return groups[i].entries;
    }
    return 1;
}

static bool incident_pick_better(const struct peer_lifecycle_incident_pick *a,
                                 const struct peer_lifecycle_incident_pick *b)
{
    if (a->score != b->score)
        return a->score > b->score;
    int64_t a_seen = a->entry ? a->entry->last_seen : 0;
    int64_t b_seen = b->entry ? b->entry->last_seen : 0;
    return a_seen > b_seen;
}

static void incident_pick_sort(struct peer_lifecycle_incident_pick *picks,
                               size_t count)
{
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (incident_pick_better(&picks[j], &picks[i])) {
                struct peer_lifecycle_incident_pick tmp = picks[i];
                picks[i] = picks[j];
                picks[j] = tmp;
            }
        }
    }
}

static void incident_pick_consider(
    struct peer_lifecycle_incident_pick *picks, size_t *count,
    const struct peer_lifecycle_entry *e, int64_t score,
    int64_t duplicate_host_entries, const char *host)
{
    if (!picks || !count || !e || score <= 0)
        return;
    struct peer_lifecycle_incident_pick candidate = {
        .entry = e,
        .score = score,
        .duplicate_host_entries = duplicate_host_entries,
    };
    snprintf(candidate.host, sizeof(candidate.host), "%s", host);
    if (*count < PEER_LIFECYCLE_INCIDENT_LIMIT) {
        picks[*count] = candidate;
        (*count)++;
        incident_pick_sort(picks, *count);
        return;
    }
    if (incident_pick_better(&candidate,
                             &picks[PEER_LIFECYCLE_INCIDENT_LIMIT - 1])) {
        picks[PEER_LIFECYCLE_INCIDENT_LIMIT - 1] = candidate;
        incident_pick_sort(picks, *count);
    }
}

static void append_incident_peer_json(
    const struct peer_lifecycle_incident_pick *pick, int64_t now,
    struct json_value *arr)
{
    const struct peer_lifecycle_entry *e = pick->entry;
    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_int(&obj, "peer_id", e->peer_id);
    json_push_kv_str(&obj, "addr", e->addr);
    json_push_kv_str(&obj, "host", pick->host);
    json_push_kv_str(&obj, "source", peer_lifecycle_source_name(e->source));
    json_push_kv_str(&obj, "direction", entry_direction(e));
    json_push_kv_int(&obj, "incident_score", pick->score);
    json_push_kv_int(&obj, "duplicate_host_entries",
                     pick->duplicate_host_entries);
    json_push_kv_int(&obj, "reconnects", entry_reconnects(e));
    json_push_kv_int(&obj, "connected", e->connected);
    json_push_kv_int(&obj, "handshake_complete", e->handshake_complete);
    json_push_kv_int(&obj, "disconnected", e->disconnected);
    json_push_kv_int(&obj, "timeout", e->timeout);
    json_push_kv_int(&obj, "rejected", e->rejected);
    json_push_kv_int(&obj, "pre_handshake_disconnects",
                     e->pre_handshake_disconnects);
    json_push_kv_bool(&obj, "connection_open", entry_connection_open(e));
    json_push_kv_bool(&obj, "current_connection_handshaked",
                      entry_current_connection_handshaked(e));
    json_push_kv_int(&obj, "handshake_age_secs",
                     entry_handshake_age_secs(e, now));
    json_push_kv_int(&obj, "services", (int64_t)e->services);
    char summary[128];
    services_summary(e->services, summary, sizeof(summary));
    json_push_kv_str(&obj, "services_summary", summary);
    json_push_kv_int(&obj, "advertised_height", e->start_height);
    json_push_kv_str(&obj, "subver", e->subver);
    json_push_kv_str(&obj, "bootstrap_readiness",
                     entry_bootstrap_readiness(e));
    json_push_kv_bool(&obj, "bootstrap_useful",
                      entry_bootstrap_useful(e));
    json_push_kv_bool(&obj, "fast_sync_useful",
                      entry_fast_sync_useful(e));
    json_push_kv_str(&obj, "last_reason", e->last_reason);
    json_push_kv_str(&obj, "last_disconnect_reason", e->last_reason);
    json_push_kv_int(&obj, "last_seen", e->last_seen);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static void append_host_group_json(const struct peer_lifecycle_host_group *g,
                                   struct json_value *arr)
{
    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_str(&obj, "host", g->host);
    json_push_kv_int(&obj, "entries", g->entries);
    json_push_kv_int(&obj, "inbound_entries", g->inbound_entries);
    json_push_kv_int(&obj, "outbound_entries", g->outbound_entries);
    json_push_kv_int(&obj, "unknown_entries", g->unknown_entries);
    json_push_kv_int(&obj, "open_connections", g->open_connections);
    json_push_kv_int(&obj, "handshaked_open_connections",
                     g->handshaked_open_connections);
    json_push_kv_int(&obj, "bootstrap_useful_connections",
                     g->bootstrap_useful_connections);
    json_push_kv_int(&obj, "fast_sync_useful_connections",
                     g->fast_sync_useful_connections);
    json_push_kv_bool(&obj, "duplicate_current_connections",
                      g->open_connections > 1);
    json_push_kv_bool(&obj, "duplicate_handshaked_connections",
                      g->handshaked_open_connections > 1);
    json_push_kv_int(&obj, "connected", g->connected);
    json_push_kv_int(&obj, "handshake_complete", g->handshake_complete);
    json_push_kv_int(&obj, "active", g->active);
    json_push_kv_int(&obj, "disconnected", g->disconnected);
    json_push_kv_int(&obj, "timeout", g->timeout);
    json_push_kv_int(&obj, "rejected", g->rejected);
    json_push_kv_int(&obj, "reconnects", g->reconnects);
    json_push_kv_int(&obj, "pre_handshake_disconnects",
                     g->pre_handshake_disconnects);
    json_push_kv_int(&obj, "services_or", (int64_t)g->services_or);
    char summary[128];
    services_summary(g->services_or, summary, sizeof(summary));
    json_push_kv_str(&obj, "services_summary", summary);
    json_push_kv_int(&obj, "max_advertised_height",
                     g->max_advertised_height);
    json_push_kv_bool(&obj, "bootstrap_useful", g->bootstrap_useful);
    json_push_kv_bool(&obj, "fast_sync_useful", g->fast_sync_useful);
    json_push_kv_int(&obj, "last_seen", g->last_seen);
    json_push_kv_str(&obj, "last_reason", g->last_reason);
    json_push_kv_str(&obj, "last_disconnect_reason", g->last_reason);
    json_push_back(arr, &obj);
    json_free(&obj);
}

bool peer_lifecycle_incidents_json(struct json_value *out)
{
    if (!out)
        return false;
    pthread_mutex_lock(&g_pl.lock);
    int64_t now = GetTime();
    struct peer_lifecycle_host_group groups[PEER_LIFECYCLE_GROUP_LIMIT];
    memset(groups, 0, sizeof(groups));
    int64_t group_overflow = build_host_groups_locked(groups);

    struct peer_lifecycle_incident_pick picks[PEER_LIFECYCLE_INCIDENT_LIMIT];
    memset(picks, 0, sizeof(picks));
    size_t pick_count = 0;
    int64_t incident_count = 0;
    int64_t duplicate_group_count = 0;
    int64_t duplicate_open_group_count = 0;
    int64_t duplicate_handshaked_group_count = 0;
    int64_t open_connection_count = 0;
    int64_t handshaked_open_connection_count = 0;
    int64_t bootstrap_useful_count = 0;
    int64_t fast_sync_useful_count = 0;

    for (size_t i = 0; i < PEER_LIFECYCLE_GROUP_LIMIT; i++) {
        if (!groups[i].used)
            continue;
        if (groups[i].entries > 1)
            duplicate_group_count++;
        if (groups[i].open_connections > 1)
            duplicate_open_group_count++;
        if (groups[i].handshaked_open_connections > 1)
            duplicate_handshaked_group_count++;
    }

    for (size_t i = 0; i < PEER_LIFECYCLE_MAX; i++) {
        const struct peer_lifecycle_entry *e = &g_pl.entries[i];
        if (!e->used)
            continue;
        char host[256];
        addr_host_key(e->addr, host, sizeof(host));
        int64_t duplicate_entries = duplicate_entries_for_host(groups, host);
        int64_t score = entry_incident_score(e, duplicate_entries);
        if (score > 0)
            incident_count++;
        if (entry_connection_open(e))
            open_connection_count++;
        if (entry_current_connection_handshaked(e))
            handshaked_open_connection_count++;
        if (entry_bootstrap_useful(e))
            bootstrap_useful_count++;
        if (entry_fast_sync_useful(e))
            fast_sync_useful_count++;
        incident_pick_consider(picks, &pick_count, e, score,
                               duplicate_entries, host);
    }

    json_set_object(out);
    json_push_kv_str(out, "schema", "zcl.peer_incidents.v1");
    json_push_kv_int(out, "schema_version", 1);
    json_push_kv_bool(out, "bounded", true);
    json_push_kv_int(out, "peer_limit", PEER_LIFECYCLE_INCIDENT_LIMIT);
    json_push_kv_int(out, "group_limit", PEER_LIFECYCLE_GROUP_LIMIT);
    json_push_kv_int(out, "incident_count", incident_count);
    json_push_kv_int(out, "count_returned", (int64_t)pick_count);
    json_push_kv_int(out, "duplicate_host_group_count",
                     duplicate_group_count);
    json_push_kv_int(out, "duplicate_open_host_group_count",
                     duplicate_open_group_count);
    json_push_kv_int(out, "duplicate_handshaked_host_group_count",
                     duplicate_handshaked_group_count);
    json_push_kv_int(out, "current_open_connection_count",
                     open_connection_count);
    json_push_kv_int(out, "current_handshaked_connection_count",
                     handshaked_open_connection_count);
    json_push_kv_int(out, "host_group_overflow", group_overflow);
    json_push_kv_int(out, "bootstrap_useful_count",
                     bootstrap_useful_count);
    json_push_kv_int(out, "fast_sync_useful_count", fast_sync_useful_count);
    json_push_kv_str(out, "semantics",
                     "bounded peer lifecycle incident view grouped by host; "
                     "use full peer_lifecycle only for raw forensic dumps");
    json_push_kv_str(out, "safe_next_action",
                     incident_count > 0
                         ? "inspect top_incidents and duplicate_host_groups"
                         : "peer lifecycle has no scored incidents");

    struct json_value incidents = {0};
    json_set_array(&incidents);
    for (size_t i = 0; i < pick_count; i++)
        append_incident_peer_json(&picks[i], now, &incidents);
    json_push_kv(out, "top_incidents", &incidents);
    json_free(&incidents);

    struct json_value duplicate_groups = {0};
    json_set_array(&duplicate_groups);
    for (size_t i = 0; i < PEER_LIFECYCLE_GROUP_LIMIT; i++) {
        if (!groups[i].used)
            continue;
        if (groups[i].entries <= 1 && groups[i].reconnects == 0 &&
            groups[i].timeout == 0 && groups[i].rejected == 0 &&
            groups[i].pre_handshake_disconnects == 0)
            continue;
        append_host_group_json(&groups[i], &duplicate_groups);
    }
    json_push_kv(out, "duplicate_host_groups", &duplicate_groups);
    json_free(&duplicate_groups);

    struct json_value drilldowns = {0};
    json_set_array(&drilldowns);
    struct json_value d = {0};
    json_set_str(&d, "zclassic23 dumpstate peer_lifecycle");
    json_push_back(&drilldowns, &d);
    json_set_str(&d, "zclassic23 timeline '{\"category\":\"peer\",\"count\":50,\"since_secs\":3600}'");
    json_push_back(&drilldowns, &d);
    json_set_str(&d, "zclassic23 dumpstate chain_advance_coordinator");
    json_push_back(&drilldowns, &d);
    json_free(&d);
    json_push_kv(out, "recommended_drilldowns", &drilldowns);
    json_free(&drilldowns);
    pthread_mutex_unlock(&g_pl.lock);
    return true;
}

static void append_sources_locked(struct json_value *out)
{
    struct peer_lifecycle_summary by_source[PEER_LIFECYCLE_SOURCE_MANUAL + 1];
    memset(by_source, 0, sizeof(by_source));

    for (size_t i = 0; i < PEER_LIFECYCLE_MAX; i++) {
        if (!g_pl.entries[i].used)
            continue;
        enum peer_lifecycle_source source = g_pl.entries[i].source;
        if (source < PEER_LIFECYCLE_SOURCE_UNKNOWN ||
            source > PEER_LIFECYCLE_SOURCE_MANUAL)
            source = PEER_LIFECYCLE_SOURCE_UNKNOWN;
        summary_add_entry(&by_source[source], &g_pl.entries[i]);
    }

    struct json_value sources = {0};
    json_set_array(&sources);
    for (int source = PEER_LIFECYCLE_SOURCE_UNKNOWN;
         source <= PEER_LIFECYCLE_SOURCE_MANUAL; source++) {
        struct json_value entry = {0};
        summary_to_json(&by_source[source], &entry);
        json_push_kv_str(&entry, "source",
                         peer_lifecycle_source_name(source));
        json_push_back(&sources, &entry);
        json_free(&entry);
    }
    json_push_kv(out, "sources", &sources);
    json_free(&sources);
}

bool peer_lifecycle_summary_json(struct json_value *out)
{
    if (!out) return false;
    pthread_mutex_lock(&g_pl.lock);
    summary_to_json(&g_pl.totals, out);
    append_sources_locked(out);
    pthread_mutex_unlock(&g_pl.lock);
    return true;
}

bool peer_lifecycle_dump_state_json(struct json_value *out,
                                    const char *key)
{
    if (!out) return false;
    pthread_mutex_lock(&g_pl.lock);
    if (key && (strcmp(key, "incidents") == 0 ||
                strcmp(key, "incident") == 0)) {
        pthread_mutex_unlock(&g_pl.lock);
        return peer_lifecycle_incidents_json(out);
    }
    struct peer_lifecycle_summary totals = g_pl.totals;
    struct json_value summary = {0};
    summary_to_json(&totals, &summary);
    json_push_kv(out, "summary", &summary);
    json_free(&summary);

    struct json_value peers = {0};
    json_set_array(&peers);
    for (size_t i = 0; i < PEER_LIFECYCLE_MAX; i++) {
        if (!g_pl.entries[i].used)
            continue;
        struct json_value entry = {0};
        entry_to_json(&g_pl.entries[i], &entry);
        json_push_back(&peers, &entry);
        json_free(&entry);
    }
    json_push_kv(out, "peers", &peers);
    json_free(&peers);
    append_sources_locked(out);
    pthread_mutex_unlock(&g_pl.lock);
    return true;
}

void peer_lifecycle_get_summary(struct peer_lifecycle_summary *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_pl.lock);
    *out = g_pl.totals;
    pthread_mutex_unlock(&g_pl.lock);
}

void peer_lifecycle_reset_for_test(void)
{
    pthread_mutex_lock(&g_pl.lock);
    memset(g_pl.entries, 0, sizeof(g_pl.entries));
    memset(&g_pl.totals, 0, sizeof(g_pl.totals));
    g_pl.seq = 0;
    pthread_mutex_unlock(&g_pl.lock);
}
