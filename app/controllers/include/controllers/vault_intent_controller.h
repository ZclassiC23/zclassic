/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: register the durable transaction-intent wallet RPC surface. */
#ifndef ZCL_VAULT_INTENT_CONTROLLER_H
#define ZCL_VAULT_INTENT_CONTROLLER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct rpc_table;
struct json_value;
struct wallet_rpc_context;
struct vault_intent_row;
void register_vault_intent_rpc_commands(struct rpc_table *table);
bool vault_intent_parse_zcl_amount(const char *text, int64_t *out_zat);
bool vault_intent_context_ready(struct wallet_rpc_context *ctx,
                                struct json_value *out);
void vault_intent_digest_payload(const uint8_t *raw, size_t len,
                                 const struct vault_intent_row *row,
                                 uint8_t out[32]);
void vault_intent_render_row(struct wallet_rpc_context *ctx,
                             struct json_value *out,
                             const struct vault_intent_row *row);
#endif
