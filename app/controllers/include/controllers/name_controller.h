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
#include <stddef.h>
#include <stdint.h>

struct wallet;
struct tx_mempool;
struct main_state;
struct coins_view_cache;

void rpc_name_set_state(struct node_db *ndb);
void rpc_name_set_wallet(struct wallet *w, struct tx_mempool *mp,
                         struct main_state *main_state,
                         struct coins_view_cache *coins_tip);
void register_name_rpc_commands(struct rpc_table *t);
const char *znam_type_name(uint8_t t);

/* REST API */
#include "json/json.h"
bool api_name_list(struct json_value *result);
bool rpc_name_resolve_api(const char *name, struct json_value *result);
bool api_name_service_directory(const char *name, struct json_value *result);
bool api_name_service_directory_path(const char *name, const char *path,
                                     struct json_value *result,
                                     char *err, size_t err_len);
size_t api_serve_name_service_directory(const char *name, const char *path,
                                        const char *freshness,
                                        uint8_t *response,
                                        size_t response_max);
void api_name_append_records(struct node_db *ndb, const char *name,
                             struct json_value *obj);

#endif
