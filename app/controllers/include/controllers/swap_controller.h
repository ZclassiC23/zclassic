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

void rpc_swap_set_state(struct node_db *ndb);
void register_swap_rpc_commands(struct rpc_table *t);

#include "json/json.h"
bool api_swap_list(struct json_value *result);
bool api_swap_chains(struct json_value *result);

#endif
