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

struct peer_lifecycle_entry {
    bool used;
    int64_t peer_id;
    char addr[256];
    enum peer_lifecycle_source source;
    int64_t first_seen;
    int64_t last_seen;
    int64_t connected_at;
    int64_t version_sent_at;
    int64_t version_received_at;
    int64_t verack_received_at;
    int64_t handshake_complete_at;
    int64_t active_at;
    int64_t disconnected_at;
    int64_t timeout_at;
    int64_t rejected_at;
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
    uint64_t services;
    int start_height;
    char subver[MAX_SUBVERSION_LENGTH];
    char last_reason[128];
};

static struct {
    pthread_mutex_t lock;
    struct peer_lifecycle_entry entries[PEER_LIFECYCLE_MAX];
    struct peer_lifecycle_summary totals;
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
        bool terminal_after_connect =
            e->disconnected_at >= e->connected_at ||
            e->timeout_at >= e->connected_at ||
            e->rejected_at >= e->connected_at;
        e->peer_id = node ? node->id : e->peer_id;
        e->connected++;
        if (e->connected_at == 0 || terminal_after_connect) {
            e->connected_at = now;
            e->version_sent_at = 0;
            e->version_received_at = 0;
            e->verack_received_at = 0;
            e->handshake_complete_at = 0;
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
        bool pre_handshake = e->handshake_complete == 0;
        e->last_seen = now;
        snprintf(e->last_reason, sizeof(e->last_reason), "%s",
                 reason ? reason : "");
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
    json_push_kv_int(out, "first_seen", e->first_seen);
    json_push_kv_int(out, "last_seen", e->last_seen);
    json_push_kv_int(out, "connected_at", e->connected_at);
    json_push_kv_int(out, "handshake_complete_at", e->handshake_complete_at);
    json_push_kv_int(out, "handshake_duration_secs",
                     handshake_duration_secs(e));
    json_push_kv_int(out, "services", (int64_t)e->services);
    json_push_kv_int(out, "startingheight", e->start_height);
    json_push_kv_str(out, "subver", e->subver);
    json_push_kv_bool(out, "magicbean",
                      subver_is_magicbean(e->subver));
    json_push_kv_bool(out, "zclassic23",
                      subver_is_zcl23(e->subver, e->services));
    json_push_kv_bool(out, "zclassic_c23",
                      subver_is_zcl23(e->subver, e->services));
    json_push_kv_str(out, "last_reason", e->last_reason);
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
    json_push_kv_int(out, "legacy_magicbean_handshakes",
                     s->legacy_compatible_handshakes);
    json_push_kv_int(out, "legacy_compatible_handshakes",
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
    if (e->handshake_complete == 0)
        s->pre_handshake_disconnects += e->disconnected + e->rejected +
                                        e->timeout;
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
    (void)key;
    if (!out) return false;
    pthread_mutex_lock(&g_pl.lock);
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
    pthread_mutex_unlock(&g_pl.lock);
}
