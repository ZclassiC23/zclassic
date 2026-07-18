/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native body functions for chain read commands. See
 * controllers/native_handler_body.h for the shared contract. */

#ifndef ZCL_CONTROLLERS_CHAIN_NATIVE_HANDLERS_H
#define ZCL_CONTROLLERS_CHAIN_NATIVE_HANDLERS_H

#include "controllers/native_handler_body.h"

#ifdef __cplusplus
extern "C" {
#endif

/* block_id (height or hash) + verbosity. Resolves a numeric
 * block_id to a hash via getblockhash before dispatching getblock. */
char *zcl_native_getblock_body(const struct json_value *args,
                                struct zcl_native_body_err *err);

/* txid + verbose. */
char *zcl_native_getrawtransaction_body(const struct json_value *args,
                                         struct zcl_native_body_err *err);

/* Optional remote_sha3/remote_height/source vs
 * getutxoaudit. */
char *zcl_native_utxo_audit_body(const struct json_value *args,
                                  struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_CHAIN_NATIVE_HANDLERS_H */
