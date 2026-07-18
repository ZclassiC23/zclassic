/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native body functions for wallet read commands. See
 * controllers/native_handler_body.h for the shared contract. */

#ifndef ZCL_CONTROLLERS_WALLET_NATIVE_HANDLERS_H
#define ZCL_CONTROLLERS_WALLET_NATIVE_HANDLERS_H

#include "controllers/native_handler_body.h"

#ifdef __cplusplus
extern "C" {
#endif

/* minconf/maxconf -> listunspent. */
char *zcl_native_listunspent_body(const struct json_value *args,
                                   struct zcl_native_body_err *err);

/* count/skip -> listtransactions. */
char *zcl_native_listtransactions_body(const struct json_value *args,
                                        struct zcl_native_body_err *err);

/* txid -> gettransaction. */
char *zcl_native_gettransaction_body(const struct json_value *args,
                                      struct zcl_native_body_err *err);

/* listwalletkeys[false] projected to
 * {t_addresses:[...], z_addresses:[...]}. */
char *zcl_native_listaddresses_body(const struct json_value *args,
                                     struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_WALLET_NATIVE_HANDLERS_H */
