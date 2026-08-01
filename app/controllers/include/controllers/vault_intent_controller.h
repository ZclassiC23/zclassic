/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: register the durable transaction-intent wallet RPC surface. */
#ifndef ZCL_VAULT_INTENT_CONTROLLER_H
#define ZCL_VAULT_INTENT_CONTROLLER_H
#include <stdbool.h>
#include <stdint.h>
struct rpc_table;
void register_vault_intent_rpc_commands(struct rpc_table *table);
bool vault_intent_parse_zcl_amount(const char *text, int64_t *out_zat);
#endif
