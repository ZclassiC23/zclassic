/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "platform/time_compat.h"
#include "controllers/network_controller.h"
#include "util/log_macros.h"
#include "controllers/strong_params.h"
#include "event/event.h"
#include "json/json.h"
#include "net/connman.h"
#include "net/peer_lifecycle.h"
#include "net/version.h"
#include "util/clientversion.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct network_context {
    struct connman *connman;
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
    size_t conns = ctx->connman ? connman_get_node_count(ctx->connman) : 0;
    int inbound = 0, outbound = 0, handshaked = 0;
    int inbound_handshaked = 0, outbound_handshaked = 0;
    int magicbean = 0, zcl23 = 0;
    size_t listen_socket_count = ctx->connman
        ? ctx->connman->manager.num_listen_sockets
        : 0;
    json_push_kv_int(result, "connections", (int64_t)conns);
    json_push_kv_int(result, "localservices",
                     ctx->connman ? (int64_t)ctx->connman->manager.local_services : 0);
    json_push_kv_int(result, "advertised_services",
                     ctx->connman ? (int64_t)ctx->connman->manager.local_services : 0);

    struct json_value networks = {0};
    json_set_array(&networks);
    json_push_kv(result, "networks", &networks);
    json_free(&networks);

    json_push_kv_real(result, "relayfee", 0.00000100);

    struct json_value localaddrs = {0};
    json_set_array(&localaddrs);
    char ext_ip[INET_ADDRSTRLEN];
    uint16_t ext_port = 0;
    if (msg_version_get_external_ip(ext_ip, sizeof(ext_ip), &ext_port)) {
        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_str(&entry, "address", ext_ip);
        json_push_kv_int(&entry, "port", ext_port);
        json_push_kv_int(&entry, "score", 1);
        json_push_back(&localaddrs, &entry);
        json_free(&entry);
    }
    json_push_kv(result, "localaddresses", &localaddrs);
    json_free(&localaddrs);

    if (ctx->connman) {
        zcl_mutex_lock(&ctx->connman->manager.cs_nodes);
        for (size_t i = 0; i < ctx->connman->manager.num_nodes; i++) {
            struct p2p_node *node = ctx->connman->manager.nodes[i];
            if (!node || node->disconnect) continue;
            if (node->inbound) inbound++; else outbound++;
            if (node->state >= PEER_HANDSHAKE_COMPLETE) {
                bool is_mb = false, is_z23 = false;
                handshaked++;
                if (node->inbound) inbound_handshaked++;
                else outbound_handshaked++;
                msg_version_classify_peer(node->sub_ver, node->services,
                                          &is_mb, &is_z23);
                if (is_mb) magicbean++;
                if (is_z23) zcl23++;
            }
        }
        zcl_mutex_unlock(&ctx->connman->manager.cs_nodes);
    }
    json_push_kv_int(result, "inbound_connections", inbound);
    json_push_kv_int(result, "outbound_connections", outbound);
    json_push_kv_int(result, "handshaked_connections", handshaked);
    json_push_kv_int(result, "inbound_handshaked_connections",
                     inbound_handshaked);
    json_push_kv_int(result, "outbound_handshaked_connections",
                     outbound_handshaked);
    json_push_kv_int(result, "magicbean_peers", magicbean);
    json_push_kv_int(result, "zclassic23_peers", zcl23);
    json_push_kv_int(result, "zclassic_c23_peers", zcl23);
    json_push_kv_int(result, "listen_socket_count",
                     (int64_t)listen_socket_count);
    json_push_kv_bool(result, "listening", listen_socket_count > 0);
    json_push_kv_bool(result, "externalip_configured", ext_port != 0);
    json_push_kv_bool(result, "inbound_handshake_seen",
                      inbound_handshaked > 0);
    json_push_kv_bool(result, "remote_handshake_seen", handshaked > 0);
    push_addnode_status(result, ctx->connman);

    struct json_value life = {0};
    peer_lifecycle_summary_json(&life);
    json_push_kv(result, "peer_lifecycle", &life);
    json_free(&life);

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
        { "network", "getpeerinfo",       rpc_getpeerinfo,       true },
        { "network", "getconnectioncount", rpc_getconnectioncount, true },
        { "network", "ping",              rpc_ping_rpc,          true },
        { "network", "addnode",           rpc_addnode,           true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
