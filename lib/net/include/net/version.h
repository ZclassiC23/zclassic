/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VERSION_H
#define ZCL_VERSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_VERSION 170011
#define INIT_PROTO_VERSION 209
#define GETHEADERS_VERSION 31800
#define MIN_PEER_PROTO_VERSION 170002
#define CADDR_TIME_VERSION 31402
#define BIP0031_VERSION 60000
#define MEMPOOL_GD_VERSION 60002
#define NO_BLOOM_VERSION 170004

void msg_version_set_external_ip(const char *ip_str, uint16_t port);
bool msg_version_get_external_ip(char *buf, size_t buflen, uint16_t *port);
#ifdef ZCL_TESTING
void msg_version_clear_external_ip_for_test(void);
#endif
const char *msg_version_user_agent(void);
bool msg_version_classify_peer(const char *subver, uint64_t services,
                               bool *is_magicbean, bool *is_zcl23);

#endif
