/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_NETBASE_H
#define ZCL_NETBASE_H

#include "net/netaddr.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
typedef int zcl_socket_t;
#define ZCL_INVALID_SOCKET (-1)
#define ZCL_SOCKET_ERROR (-1)
#else
#include <winsock2.h>
typedef SOCKET zcl_socket_t;
#define ZCL_INVALID_SOCKET INVALID_SOCKET
#define ZCL_SOCKET_ERROR SOCKET_ERROR
#endif

#define DEFAULT_CONNECT_TIMEOUT 5000

struct proxy_info {
    struct net_service proxy;
    bool randomize_credentials;
    bool valid;
};

static inline void proxy_info_init(struct proxy_info *p)
{
    net_service_init(&p->proxy);
    p->randomize_credentials = false;
    p->valid = false;
}

bool lookup_host(const char *name, struct net_addr *results,
                 size_t max_results, size_t *num_results,
                 bool allow_lookup);

bool lookup_numeric(const char *name, struct net_service *result,
                    uint16_t default_port);

bool connect_socket_directly(const struct net_service *addr,
                             zcl_socket_t *sock_out,
                             int timeout_ms);

bool close_socket(zcl_socket_t *sock);

/* Prefixed to avoid conflict with Tor's socket.c */
bool zcl_set_socket_nonblocking(zcl_socket_t sock, bool nonblocking);
#define set_socket_nonblocking zcl_set_socket_nonblocking

void split_host_port(const char *in, char *host_out, size_t host_size,
                     int *port_out);

struct timeval millis_to_timeval(int64_t ms);

#endif
