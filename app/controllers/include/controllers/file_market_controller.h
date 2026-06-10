/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * File Market Controller — RPC commands for ZCL Market.
 *
 * Commands:
 *   zmarket_list    — list files available on the network
 *   zmarket_offer   — announce a file for sale
 *   zmarket_buy     — initiate purchase/download
 *   zmarket_status  — market status and active downloads */

#ifndef ZCL_CONTROLLERS_FILE_MARKET_H
#define ZCL_CONTROLLERS_FILE_MARKET_H

#include "rpc/server.h"
#include "models/database.h"

void rpc_market_set_state(struct node_db *ndb);
void register_market_rpc_commands(struct rpc_table *t);

#include "json/json.h"
bool api_market_list(struct json_value *result);

#endif
