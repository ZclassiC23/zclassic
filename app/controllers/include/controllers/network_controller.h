/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_NET_H
#define ZCL_RPC_NET_H

#include "rpc/server.h"

struct connman;

void rpc_net_set_connman(struct connman *cm);
struct connman *rpc_net_get_connman(void);
void register_net_rpc_commands(struct rpc_table *t);

#endif
