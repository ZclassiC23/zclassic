/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: publish one exact prepared vault transaction restart-safely. */
#ifndef ZCL_VAULT_INTENT_PUBLISH_H
#define ZCL_VAULT_INTENT_PUBLISH_H

#include <stdbool.h>
#include <stdint.h>

struct json_value;
struct wallet_rpc_context;
struct wallet_tx;

bool vault_intent_publish_prepared(struct wallet_rpc_context *ctx,
                                   const uint8_t id[32],
                                   struct wallet_tx *wtx, int64_t now,
                                   struct json_value *result);

#endif
