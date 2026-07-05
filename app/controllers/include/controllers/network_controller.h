/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_NET_H
#define ZCL_RPC_NET_H

#include "rpc/server.h"

struct connman;
struct msg_processor;

void rpc_net_set_connman(struct connman *cm);
struct connman *rpc_net_get_connman(void);
void rpc_net_set_msg_processor(struct msg_processor *mp);
struct msg_processor *rpc_net_get_msg_processor(void);
void rpc_net_set_boot_context(const char *datadir,
                              const char *load_snapshot_at_own_height);
void register_net_rpc_commands(struct rpc_table *t);

#endif
