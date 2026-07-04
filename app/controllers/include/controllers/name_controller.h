/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Name Controller — RPC commands for ZCL Names (ZNAM).
 *
 * Commands:
 *   name_register  — register a name on-chain
 *   name_resolve   — look up a name
 *   name_list      — list registered names */

#ifndef ZCL_CONTROLLERS_NAME_H
#define ZCL_CONTROLLERS_NAME_H

#include "rpc/server.h"
#include "models/database.h"

struct wallet;
struct tx_mempool;

void rpc_name_set_state(struct node_db *ndb);
void rpc_name_set_wallet(struct wallet *w, struct tx_mempool *mp);
void register_name_rpc_commands(struct rpc_table *t);

/* REST API */
#include "json/json.h"
bool api_name_list(struct json_value *result);
bool rpc_name_resolve_api(const char *name, struct json_value *result);

#endif
