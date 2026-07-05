/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "platform/time_compat.h"
#include "controllers/diagnostics_internal.h"
#include "controllers/network_controller.h"
#include "util/log_macros.h"
#include "controllers/strong_params.h"
#include "event/event.h"
#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "net/connman.h"
#include "net/fast_sync.h"
#include "net/peer_lifecycle.h"
#include "net/protocol.h"
#include "net/version.h"
#include "util/clientversion.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct network_context {
    struct connman *connman;
    struct msg_processor *msg_processor;
    const char *datadir;
    const char *load_snapshot_at_own_height;
};

#define BOOTSTRAP_STATUS_SCHEMA "zcl.bootstrap_status.v1"
#define BOOTSTRAP_STATUS_SCHEMA_VERSION 1
#define ZCLASSICD_BETA6_LABEL "zclassicd v2.1.2-beta6"

struct network_counts {
    size_t connections;
    int inbound;
    int outbound;
    int handshaked;
    int inbound_handshaked;
    int outbound_handshaked;
    int legacy_compatible;
    int zcl23;
    size_t listen_socket_count;
    uint64_t local_services;
    size_t addrman_entries;
    struct addrman_bucket_stats addrman_stats;
};

static struct network_context g_network_ctx = {0};

static struct network_context *network_ctx(void)
{
    return &g_network_ctx;
}

void rpc_net_set_connman(struct connman *cm)
{
    network_ctx()->connman = cm;
}

struct connman *rpc_net_get_connman(void)
{
    return network_ctx()->connman;
}

void rpc_net_set_msg_processor(struct msg_processor *mp)
{
    network_ctx()->msg_processor = mp;
}

struct msg_processor *rpc_net_get_msg_processor(void)
{
    return network_ctx()->msg_processor;
}

void rpc_net_set_boot_context(const char *datadir,
                              const char *load_snapshot_at_own_height)
{
    struct network_context *ctx = network_ctx();
    ctx->datadir = datadir;
    ctx->load_snapshot_at_own_height = load_snapshot_at_own_height;
}

static void network_counts_collect(struct connman *cm,
                                   struct network_counts *out)
{
    memset(out, 0, sizeof(*out));
    if (!cm)
        return;

    out->connections = connman_get_node_count(cm);
    out->listen_socket_count = cm->manager.num_listen_sockets;
    out->local_services = cm->manager.local_services;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *node = cm->manager.nodes[i];
        if (!node || node->disconnect)
            continue;
        if (node->inbound)
            out->inbound++;
        else
            out->outbound++;
        if (node->state >= PEER_HANDSHAKE_COMPLETE) {
            bool is_legacy = false, is_z23 = false;
            out->handshaked++;
            if (node->inbound)
                out->inbound_handshaked++;
            else
                out->outbound_handshaked++;
            msg_version_classify_peer(node->sub_ver, node->services,
                                      &is_legacy, &is_z23);
            if (is_legacy)
                out->legacy_compatible++;
            if (is_z23)
                out->zcl23++;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    zcl_mutex_lock(&cm->manager.addrman.cs);
    out->addrman_entries = addrman_size(&cm->manager.addrman);
    addrman_get_bucket_stats(&cm->manager.addrman,
                             &out->addrman_stats);
    zcl_mutex_unlock(&cm->manager.addrman.cs);
}

static void json_push_str_array(struct json_value *obj, const char *key,
                                const char *const *items, size_t count)
{
    struct json_value arr = {0};
    json_set_array(&arr);
    for (size_t i = 0; i < count; i++) {
        struct json_value item = {0};
        json_set_str(&item, items[i]);
        json_push_back(&arr, &item);
        json_free(&item);
    }
    json_push_kv(obj, key, &arr);
    json_free(&arr);
}

static void json_push_localaddresses(struct json_value *obj,
                                     const char *key,
                                     const char *ext_ip,
                                     uint16_t ext_port)
{
    struct json_value localaddrs = {0};
    json_set_array(&localaddrs);
    if (ext_ip && ext_ip[0] != '\0' && ext_port != 0) {
        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_str(&entry, "address", ext_ip);
        json_push_kv_int(&entry, "port", ext_port);
        json_push_kv_int(&entry, "score", 1);
        json_push_back(&localaddrs, &entry);
        json_free(&entry);
    }
    json_push_kv(obj, key, &localaddrs);
    json_free(&localaddrs);
}

static void json_push_string_item(struct json_value *arr, const char *value)
{
    struct json_value item = {0};
    json_set_str(&item, value);
    json_push_back(arr, &item);
    json_free(&item);
}

static bool addnode_is_connected(struct connman *cm, int addnode_index)
{
    bool connected = false;

    if (!cm || addnode_index < 0 || addnode_index >= cm->num_addnodes)
        return false;

    zcl_mutex_lock(&cm->manager.cs_nodes);
    for (size_t i = 0; i < cm->manager.num_nodes; i++) {
        const struct p2p_node *node = cm->manager.nodes[i];
        if (!node || node->disconnect)
            continue;
        if (net_addr_eq(&node->addr.svc.addr,
                        &cm->addnodes[addnode_index].svc.addr) &&
            node->addr.svc.port == cm->addnodes[addnode_index].svc.port) {
            connected = true;
            break;
        }
    }
    zcl_mutex_unlock(&cm->manager.cs_nodes);

    return connected;
}

static void push_addnode_status(struct json_value *result,
                                struct connman *cm)
{
    struct json_value arr = {0};
    int64_t now = (int64_t)platform_time_wall_time_t();

    json_set_array(&arr);
    if (cm) {
        for (int i = 0; i < cm->num_addnodes; i++) {
            struct json_value entry = {0};
            char addr[64];
            int64_t last = cm->addnode_last_attempt[i];
            int64_t elapsed = last > 0 && now >= last ? now - last : -1;
            int64_t remaining = 0;

            if (cm->addnode_backoff_sec[i] > 0 && elapsed >= 0 &&
                elapsed < cm->addnode_backoff_sec[i])
                remaining = cm->addnode_backoff_sec[i] - elapsed;

            net_service_to_string(&cm->addnodes[i].svc, addr, sizeof(addr));
            json_set_object(&entry);
            json_push_kv_int(&entry, "index", i);
            json_push_kv_str(&entry, "address", addr);
            json_push_kv_bool(&entry, "connected",
                              addnode_is_connected(cm, i));
            json_push_kv_int(&entry, "last_attempt", last);
            json_push_kv_int(&entry, "seconds_since_attempt", elapsed);
            json_push_kv_int(&entry, "backoff_seconds",
                             cm->addnode_backoff_sec[i]);
            json_push_kv_int(&entry, "backoff_remaining_seconds",
                             remaining);
            json_push_kv_int(&entry, "tcp_failures",
                             cm->addnode_tcp_failures[i]);
            json_push_kv_int(&entry, "protocol_failures",
                             cm->addnode_protocol_failures[i]);
            json_push_back(&arr, &entry);
            json_free(&entry);
        }
    }

    json_push_kv(result, "addnode_status", &arr);
    json_free(&arr);
}

static bool join_path(char *out, size_t out_sz, const char *dir,
                      const char *base)
{
    int n;

    if (!out || out_sz == 0 || !dir || !dir[0] || !base || !base[0])
        return false; // raw-return-ok:predicate-invalid-path
    n = snprintf(out, out_sz, "%s/%s", dir, base);
    return n > 0 && (size_t)n < out_sz;
}

static void push_snapshot_loader_status(struct json_value *result,
                                        const struct network_context *ctx)
{
    const char *datadir = (ctx && ctx->datadir) ? ctx->datadir : "";
    const char *loader_path =
        (ctx && ctx->load_snapshot_at_own_height)
            ? ctx->load_snapshot_at_own_height : "";
    char best_name[256] = {0};
    char bundle_path[1200] = {0};
    char failed_marker_path[1400] = {0};
    int bundle_count = 0;
    long long seed_h = bundle_scan_seed_height(datadir, &bundle_count,
                                               best_name, sizeof(best_name));
    bool bundle_present = seed_h >= 0;
    bool block_index_present = false;
    bool failed_marker = false;
    bool loader_configured = loader_path[0] != '\0';
    bool bundle_path_ready = false;
    bool bootable_bundle = false;
    const char *recovery_hint = "install_tip_seed_snapshot";

    if (bundle_present)
        bundle_path_ready = join_path(bundle_path, sizeof(bundle_path),
                                      datadir, best_name);

    if (datadir[0]) {
        char p[1200];
        if (join_path(p, sizeof(p), datadir, "block_index.bin"))
            block_index_present = access(p, F_OK) == 0;
        if (bundle_path_ready) {
            int n = snprintf(failed_marker_path,
                             sizeof(failed_marker_path),
                             "%s.failed", bundle_path);
            if (n > 0 && (size_t)n < sizeof(failed_marker_path))
                failed_marker = access(failed_marker_path, F_OK) == 0;
        }
    }

    bootable_bundle = bundle_present && block_index_present && !failed_marker;

    if (!datadir[0])
        recovery_hint = "configure_datadir";
    else if (loader_configured)
        recovery_hint = "loader_active";
    else if (bootable_bundle)
        recovery_hint = "restart_with_load_snapshot_at_own_height";
    else if (bundle_present && failed_marker)
        recovery_hint = "replace_failed_seed_snapshot";

    struct json_value loader = {0};
    json_set_object(&loader);
    json_push_kv_str(&loader, "schema", "zcl.snapshot_loader.v1");
    json_push_kv_int(&loader, "schema_version", 1);
    json_push_kv_str(&loader, "datadir", datadir);
    json_push_kv_bool(&loader, "bundle_present", bundle_present);
    json_push_kv_int(&loader, "bundle_count", bundle_count);
    json_push_kv_int(&loader, "bundle_seed_height", seed_h);
    json_push_kv_str(&loader, "bundle_name", best_name);
    json_push_kv_str(&loader, "bundle_path",
                     bundle_path_ready ? bundle_path : "");
    json_push_kv_bool(&loader, "block_index_present",
                      block_index_present);
    json_push_kv_bool(&loader, "failed_marker", failed_marker);
    json_push_kv_bool(&loader, "bootable_bundle", bootable_bundle);
    json_push_kv_bool(&loader, "active_loader_configured",
                      loader_configured);
    json_push_kv_str(&loader, "active_loader_path", loader_path);
    json_push_kv_bool(&loader, "active_loader_matches_bundle",
                      loader_configured && bundle_path_ready &&
                      strcmp(loader_path, bundle_path) == 0);
    json_push_kv_str(&loader, "recovery_hint", recovery_hint);
    json_push_kv(result, "snapshot_loader", &loader);
    json_free(&loader);
}

static bool rpc_getnetworkinfo(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getnetworkinfo\n"
        "Returns an object containing various state info "
        "regarding P2P networking.");

    json_set_object(result);
    json_push_kv_int(result, "version", CLIENT_VERSION);
    json_push_kv_str(result, "subversion", CLIENT_NAME);
    json_push_kv_str(result, "advertised_subver", msg_version_user_agent());
    json_push_kv_int(result, "protocolversion", PROTOCOL_VERSION);

    struct network_context *ctx = network_ctx();
    struct network_counts counts;
    network_counts_collect(ctx->connman, &counts);
    json_push_kv_int(result, "connections", (int64_t)counts.connections);
    json_push_kv_int(result, "localservices",
                     (int64_t)counts.local_services);
    json_push_kv_int(result, "advertised_services",
                     (int64_t)counts.local_services);

    struct json_value networks = {0};
    json_set_array(&networks);
    json_push_kv(result, "networks", &networks);
    json_free(&networks);

    json_push_kv_real(result, "relayfee", 0.00000100);

    char ext_ip[INET_ADDRSTRLEN] = {0};
    uint16_t ext_port = 0;
    msg_version_get_external_ip(ext_ip, sizeof(ext_ip), &ext_port);
    json_push_localaddresses(result, "localaddresses", ext_ip, ext_port);

    json_push_kv_int(result, "inbound_connections", counts.inbound);
    json_push_kv_int(result, "outbound_connections", counts.outbound);
    json_push_kv_int(result, "handshaked_connections", counts.handshaked);
    json_push_kv_int(result, "inbound_handshaked_connections",
                     counts.inbound_handshaked);
    json_push_kv_int(result, "outbound_handshaked_connections",
                     counts.outbound_handshaked);
    json_push_kv_int(result, "legacy_compatible_peers",
                     counts.legacy_compatible);
    json_push_kv_int(result, "legacy_magicbean_peers",
                     counts.legacy_compatible);
    json_push_kv_int(result, "magicbean_peers", counts.legacy_compatible);
    json_push_kv_int(result, "zclassic23_peers", counts.zcl23);
    json_push_kv_int(result, "zclassic_c23_peers", counts.zcl23);
    json_push_kv_int(result, "listen_socket_count",
                     (int64_t)counts.listen_socket_count);
    json_push_kv_bool(result, "listening", counts.listen_socket_count > 0);
    json_push_kv_bool(result, "externalip_configured", ext_port != 0);
    json_push_kv_bool(result, "inbound_handshake_seen",
                      counts.inbound_handshaked > 0);
    json_push_kv_bool(result, "remote_handshake_seen",
                      counts.handshaked > 0);
    push_addnode_status(result, ctx->connman);

    struct json_value life = {0};
    peer_lifecycle_summary_json(&life);
    json_push_kv(result, "peer_lifecycle", &life);
    json_free(&life);

    return true;
}

static bool rpc_bootstrapstatus(const struct json_value *params, bool help,
                                struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "bootstrapstatus\n"
        "Returns a versioned bootstrap-service contract: ordinary P2P "
        "bootstrap readiness plus zclassicd beta6 snapshot-bootstrap "
        "compatibility.");

    struct network_context *ctx = network_ctx();
    struct network_counts counts;
    network_counts_collect(ctx->connman, &counts);

    char ext_ip[INET_ADDRSTRLEN] = {0};
    uint16_t ext_port = 0;
    bool has_external_ip =
        msg_version_get_external_ip(ext_ip, sizeof(ext_ip), &ext_port);
    int32_t advertised_height = reducer_frontier_provable_tip_cached();
    bool has_connman = ctx->connman != NULL;
    bool node_network = (counts.local_services & NODE_NETWORK) != 0;
    bool node_zcl23 = (counts.local_services & NODE_ZCL23) != 0;
    bool node_bootstrap = (counts.local_services & NODE_BOOTSTRAP) != 0;
    bool protocol_ok = PROTOCOL_VERSION >= MIN_PEER_PROTO_VERSION;
    bool listening = counts.listen_socket_count > 0;
    bool has_tip = advertised_height > 0;
    bool p2p_serving =
        has_connman && listening && node_network && protocol_ok && has_tip;
    bool addr_relay_ready = counts.addrman_entries > 0;
    bool beta6_fast = p2p_serving && node_bootstrap;

    json_set_object(result);
    json_push_kv_str(result, "schema", BOOTSTRAP_STATUS_SCHEMA);
    json_push_kv_int(result, "schema_version",
                     BOOTSTRAP_STATUS_SCHEMA_VERSION);
    json_push_kv_bool(result, "ok", p2p_serving);
    json_push_kv_bool(result, "serving_p2p_bootstrap", p2p_serving);
    json_push_kv_bool(result, "serving_addr_bootstrap",
                      p2p_serving && addr_relay_ready);
    json_push_kv_bool(result, "serving_snapshot_bootstrap", beta6_fast);
    json_push_kv_bool(result, "zclassicd_beta6_p2p_compatible",
                      node_network && protocol_ok);
    json_push_kv_bool(result,
                      "zclassicd_beta6_fast_bootstrap_compatible",
                      beta6_fast);

    struct json_value identity = {0};
    json_set_object(&identity);
    json_push_kv_int(&identity, "version", CLIENT_VERSION);
    json_push_kv_str(&identity, "client_name", CLIENT_NAME);
    json_push_kv_str(&identity, "advertised_subver",
                     msg_version_user_agent());
    json_push_kv_str(&identity, "build_commit", zcl_build_commit());
    json_push_kv(result, "binary", &identity);
    json_free(&identity);

    struct json_value p2p = {0};
    json_set_object(&p2p);
    json_push_kv_int(&p2p, "protocolversion", PROTOCOL_VERSION);
    json_push_kv_int(&p2p, "minimum_peer_protocol",
                     MIN_PEER_PROTO_VERSION);
    json_push_kv_int(&p2p, "advertised_services",
                     (int64_t)counts.local_services);
    json_push_kv_bool(&p2p, "node_network", node_network);
    json_push_kv_bool(&p2p, "node_zclassic23", node_zcl23);
    json_push_kv_bool(&p2p, "node_bootstrap", node_bootstrap);
    json_push_kv_bool(&p2p, "listening", listening);
    json_push_kv_int(&p2p, "listen_socket_count",
                     (int64_t)counts.listen_socket_count);
    json_push_kv_int(&p2p, "advertised_start_height",
                     advertised_height);
    json_push_kv_bool(&p2p, "externalip_configured",
                      has_external_ip && ext_port != 0);
    json_push_localaddresses(&p2p, "localaddresses", ext_ip, ext_port);
    json_push_kv(result, "p2p", &p2p);
    json_free(&p2p);

    struct json_value peers = {0};
    json_set_object(&peers);
    json_push_kv_int(&peers, "connections", (int64_t)counts.connections);
    json_push_kv_int(&peers, "inbound_connections", counts.inbound);
    json_push_kv_int(&peers, "outbound_connections", counts.outbound);
    json_push_kv_int(&peers, "handshaked_connections", counts.handshaked);
    json_push_kv_int(&peers, "inbound_handshaked_connections",
                     counts.inbound_handshaked);
    json_push_kv_int(&peers, "outbound_handshaked_connections",
                     counts.outbound_handshaked);
    json_push_kv_int(&peers, "legacy_compatible_peers",
                     counts.legacy_compatible);
    json_push_kv_int(&peers, "zclassic23_peers", counts.zcl23);
    json_push_kv(result, "peers", &peers);
    json_free(&peers);

    struct json_value addrman = {0};
    json_set_object(&addrman);
    json_push_kv_int(&addrman, "entries",
                     (int64_t)counts.addrman_entries);
    json_push_kv_int(&addrman, "getaddr_max", MAX_ADDR_TO_SEND);
    json_push_kv_int(&addrman, "new_occupied",
                     counts.addrman_stats.new_occupied);
    json_push_kv_int(&addrman, "tried_occupied",
                     counts.addrman_stats.tried_occupied);
    json_push_kv_int(&addrman, "new_buckets_nonempty",
                     counts.addrman_stats.new_buckets_nonempty);
    json_push_kv_int(&addrman, "tried_buckets_nonempty",
                     counts.addrman_stats.tried_buckets_nonempty);
    json_push_kv_bool(&addrman, "addr_relay_ready", addr_relay_ready);
    json_push_kv(result, "addrman", &addrman);
    json_free(&addrman);

    push_snapshot_loader_status(result, ctx);

    static const char *const legacy_msgs[] = {
        "version", "verack", "sendheaders", "getheaders", "headers",
        "getblocks", "inv", "getdata", "block", "getaddr", "addr",
        "ping", "pong"
    };
    static const char *const beta6_msgs[] = {
        "getbsman", "bsman", "getbschk", "bschk",
        "getbspman", "bspman", "getbspchk", "bspchk"
    };

    struct json_value legacy = {0};
    json_set_object(&legacy);
    json_push_kv_str(&legacy, "schema", "zcl.bootstrap.p2p.v1");
    json_push_kv_str(&legacy, "target_client", ZCLASSICD_BETA6_LABEL);
    json_push_kv_str(&legacy, "required_service_bit", "NODE_NETWORK");
    json_push_kv_int(&legacy, "required_service_bit_value", NODE_NETWORK);
    json_push_kv_bool(&legacy, "serving", p2p_serving);
    json_push_kv_bool(&legacy, "addr_relay_ready",
                      p2p_serving && addr_relay_ready);
    json_push_str_array(&legacy, "messages", legacy_msgs,
                        sizeof(legacy_msgs) / sizeof(legacy_msgs[0]));
    json_push_kv(result, "legacy_p2p_bootstrap", &legacy);
    json_free(&legacy);

    struct json_value beta6 = {0};
    json_set_object(&beta6);
    json_push_kv_str(&beta6, "schema",
                     "zclassicd.bootstrap.snapshot.v3");
    json_push_kv_str(&beta6, "target_client", ZCLASSICD_BETA6_LABEL);
    json_push_kv_str(&beta6, "required_service_bit", "NODE_BOOTSTRAP");
    json_push_kv_int(&beta6, "required_service_bit_value",
                     NODE_BOOTSTRAP);
    json_push_kv_bool(&beta6, "advertised", node_bootstrap);
    json_push_kv_bool(&beta6, "serving", beta6_fast);
    json_push_kv_int(&beta6, "chunk_size_bytes", 1024 * 1024);
    json_push_kv_str(&beta6, "current_blocker",
                     node_bootstrap ? "" :
                     "NODE_BOOTSTRAP service not implemented in zclassic23");
    json_push_str_array(&beta6, "messages", beta6_msgs,
                        sizeof(beta6_msgs) / sizeof(beta6_msgs[0]));
    json_push_kv(result, "beta6_snapshot_bootstrap", &beta6);
    json_free(&beta6);

    struct json_value blockers = {0};
    json_set_array(&blockers);
    if (!has_connman)
        json_push_string_item(&blockers, "p2p_not_initialized");
    if (has_connman && !listening)
        json_push_string_item(&blockers, "not_listening");
    if (!node_network)
        json_push_string_item(&blockers, "NODE_NETWORK_not_advertised");
    if (!protocol_ok)
        json_push_string_item(&blockers, "protocol_below_min_peer_version");
    if (!has_tip)
        json_push_string_item(&blockers, "provable_tip_not_published");
    if (!node_bootstrap)
        json_push_string_item(&blockers,
                              "beta6_NODE_BOOTSTRAP_not_advertised");
    json_push_kv(result, "blockers", &blockers);
    json_free(&blockers);

    struct json_value warnings = {0};
    json_set_array(&warnings);
    if (!has_external_ip || ext_port == 0)
        json_push_string_item(&warnings, "externalip_not_configured");
    if (!addr_relay_ready)
        json_push_string_item(&warnings, "addrman_empty");
    json_push_kv(result, "warnings", &warnings);
    json_free(&warnings);

    return true;
}

static bool rpc_getpeerinfo(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getpeerinfo\n"
        "Returns data about each connected network node.");

    json_set_array(result);
    struct network_context *ctx = network_ctx();
    if (!ctx->connman) return true;

    zcl_mutex_lock(&ctx->connman->manager.cs_nodes);
    for (size_t i = 0; i < ctx->connman->manager.num_nodes; i++) {
        struct p2p_node *node = ctx->connman->manager.nodes[i];
        struct json_value entry = {0};
        json_set_object(&entry);

        json_push_kv_int(&entry, "id", (int64_t)node->id);
        json_push_kv_str(&entry, "addr", node->addr_name);
        json_push_kv_str(&entry, "subver", node->clean_sub_ver);
        json_push_kv_int(&entry, "version", (int64_t)node->version);
        json_push_kv_bool(&entry, "inbound", node->inbound);
        json_push_kv_int(&entry, "startingheight",
                          (int64_t)node->starting_height);
        json_push_kv_int(&entry, "conntime", node->time_connected);
        json_push_kv_int(&entry, "lastsend", node->last_send);
        json_push_kv_int(&entry, "lastrecv", node->last_recv);
        json_push_kv_int(&entry, "bytessent", (int64_t)node->send_bytes);
        json_push_kv_int(&entry, "bytesrecv", (int64_t)node->recv_bytes);

        double ping_ms = (double)node->ping_usec_time / 1000000.0;
        json_push_kv_real(&entry, "pingtime", ping_ms);

        /* State machine fields — full observability */
        json_push_kv_str(&entry, "state",
                          peer_state_name(node->state));
        json_push_kv_int(&entry, "state_id", (int64_t)node->state);
        json_push_kv_int(&entry, "misbehavior",
                          (int64_t)node->misbehavior);
        json_push_kv_int(&entry, "blocks_received",
                          (int64_t)node->blocks_received);
        if (node->avg_latency_us > 0)
            json_push_kv_real(&entry, "avg_latency_ms",
                               (double)node->avg_latency_us / 1000.0);

        {
            bool is_mb = false, is_z23 = false;
            struct json_value lifecycle = {0};
            msg_version_classify_peer(node->sub_ver, node->services,
                                      &is_mb, &is_z23);
            json_push_kv_bool(&entry, "magicbean", is_mb);
            json_push_kv_bool(&entry, "zclassic23", is_z23);
            json_push_kv_bool(&entry, "zclassic_c23", is_z23);
            peer_lifecycle_peer_json(node, &lifecycle);
            json_push_kv(&entry, "lifecycle", &lifecycle);
            json_free(&lifecycle);
        }

        json_push_back(result, &entry);
        json_free(&entry);
    }
    zcl_mutex_unlock(&ctx->connman->manager.cs_nodes);

    return true;
}

static bool rpc_getconnectioncount(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getconnectioncount\n"
        "Returns the number of connections to other nodes.");

    struct network_context *ctx = network_ctx();
    size_t conns = ctx->connman ? connman_get_node_count(ctx->connman) : 0;
    json_set_int(result, (int64_t)conns);
    return true;
}

static bool rpc_ping_rpc(const struct json_value *params, bool help,
                           struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "ping\n"
        "Requests that a ping be sent to all other nodes.");

    struct network_context *ctx = network_ctx();
    if (ctx->connman) {
        zcl_mutex_lock(&ctx->connman->manager.cs_nodes);
        for (size_t i = 0; i < ctx->connman->manager.num_nodes; i++)
            ctx->connman->manager.nodes[i]->ping_queued = true;
        zcl_mutex_unlock(&ctx->connman->manager.cs_nodes);
    }

    json_set_null(result);
    return true;
}

static bool rpc_addnode(const struct json_value *params, bool help,
                         struct json_value *result)
{
    RPC_HELP(help, result,
        "addnode \"node\" \"add|remove|onetry\"\n"
        "Attempts to add or remove a node from the addnode list.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 2, 2);
    const char *node_str = rpc_require_str(&p, 0, "node");
    const char *cmd = rpc_require_str(&p, 1, "command");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    struct network_context *ctx = network_ctx();
    if (!ctx->connman) {
        json_set_str(result, "P2P not initialized");
        return false;
    }

    if (strcmp(cmd, "onetry") == 0 || strcmp(cmd, "add") == 0) {
        /* Parse host:port — split on last colon */
        char host[256];
        uint16_t port = ctx->connman->manager.default_port;
        strncpy(host, node_str, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        char *colon = strrchr(host, ':');
        if (colon && colon != host) {
            *colon = '\0';
            int p_val = atoi(colon + 1);
            if (p_val > 0 && p_val <= 65535)
                port = (uint16_t)p_val;
        }

        /* addnode is a direct-connect to a known peer and must be a numeric
         * IP[:port]. Reject DNS names up front: the resolution below runs
         * getaddrinfo() synchronously on an RPC worker thread, and the
         * rpc_timeout watchdog (socket shutdown) cannot interrupt a thread
         * parked in getaddrinfo — so a few slow/dead hostnames would wedge the
         * small RPC/MCP worker pool. A clear error beats a hung node. */
        {
            struct in_addr a4; struct in6_addr a6;
            if (inet_pton(AF_INET, host, &a4) != 1 &&
                inet_pton(AF_INET6, host, &a6) != 1) {
                json_set_str(result,
                    "addnode requires a numeric IP address (DNS names are not resolved)");
                return false;
            }
        }
        connman_add_seed_node(ctx->connman, host, port);

        /* Direct connect — don't rely on addrman random selection */
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
            connman_open_connection(ctx->connman, &addr);
        }

        json_set_null(result);
        return true;
    }

    json_set_null(result);
    return true;
}

void register_net_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "network", "getnetworkinfo",    rpc_getnetworkinfo,    true },
        { "network", "bootstrapstatus",   rpc_bootstrapstatus,   true },
        { "network", "getpeerinfo",       rpc_getpeerinfo,       true },
        { "network", "getconnectioncount", rpc_getconnectioncount, true },
        { "network", "ping",              rpc_ping_rpc,          true },
        { "network", "addnode",           rpc_addnode,           true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
