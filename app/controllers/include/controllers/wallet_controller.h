/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_RPC_WALLET_RPC_H
#define ZCL_RPC_WALLET_RPC_H

#include "rpc/server.h"

struct wallet;
struct main_state;
struct wallet_sqlite;
struct tx_mempool;
struct connman;
struct node_db;
struct coins_view_cache;

void rpc_wallet_set_state(struct wallet *w, struct main_state *ms,
                          const char *datadir, struct wallet_sqlite *wdb,
                          struct tx_mempool *mempool,
                          struct connman *connman);
void rpc_wallet_set_coins_tip(struct coins_view_cache *tip);
void rpc_wallet_set_node_db(struct node_db *ndb);
void register_wallet_rpc_commands(struct rpc_table *t);

/* Direct C API for wallet view controller (no RPC round-trip) */
bool wallet_direct_sendtoaddress(const char *address, int64_t amount_sat,
                                  char *txid_out, size_t txid_out_size,
                                  char *error_out, size_t error_out_size);

#endif
