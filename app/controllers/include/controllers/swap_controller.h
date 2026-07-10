/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Swap Controller — RPC commands for HTLC atomic swaps.
 *
 * Commands:
 *   swap_initiate    — create HTLC, generate secret, return contract
 *   swap_participate — create counter-HTLC for a given secret_hash
 *   swap_redeem      — claim funds with preimage
 *   swap_refund      — reclaim funds after locktime
 *   swap_list        — list swap contracts
 *   swap_chains      — list supported chains */

#ifndef ZCL_CONTROLLERS_SWAP_H
#define ZCL_CONTROLLERS_SWAP_H

#include "rpc/server.h"
#include "models/database.h"

struct wallet;
struct tx_mempool;
struct main_state;
struct coins_view_cache;
struct connman;

void rpc_swap_set_state(struct node_db *ndb);

/* Wire the chain/wallet context needed to settle (redeem/refund) a funded
 * HTLC on the local ZCL chain: the wallet (keys + fee), the mempool + coins
 * tip (funding lookup + acceptance), the chain tip (height for CLTV), and
 * connman (relay). Any NULL leaves the corresponding settlement RPC returning
 * a clear "node context unavailable" error rather than crashing. */
void rpc_swap_set_wallet(struct wallet *w, struct tx_mempool *mp,
                         struct main_state *ms,
                         struct coins_view_cache *coins_tip,
                         struct connman *cm);

void register_swap_rpc_commands(struct rpc_table *t);

#include "json/json.h"
bool api_swap_list(struct json_value *result);
bool api_swap_chains(struct json_value *result);

#endif
