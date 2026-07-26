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

/* address -> z_getbalance, projected to
 * {address, balance, minconf}. z_getbalance answers with a bare decimal
 * string; the object names the address and confirmation floor the number
 * was computed under. */
char *zcl_native_z_getbalance_body(const struct json_value *args,
                                    struct zcl_native_body_err *err);

/* z_listunspent[0] projected to {count, notes:[...]}. A wallet with no
 * shielded notes answers count=0 with an empty list, never a missing key. */
char *zcl_native_z_listunspent_body(const struct json_value *args,
                                     struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_WALLET_NATIVE_HANDLERS_H */
