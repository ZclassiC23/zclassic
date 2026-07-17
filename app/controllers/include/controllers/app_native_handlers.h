/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral body functions for the read-only ZCL application MCP
 * tools (names, tokens, messaging inbox, file market, atomic swaps),
 * re-homed out of tools/mcp/controllers/app_controller.c. See
 * controllers/native_handler_body.h for the shared contract. Both the MCP
 * wrapper handlers in app_controller.c (dual-run through W2) and the native
 * command bridge (tools/command/native_command.c) call these directly.
 *
 * Only the read surface is re-homed here: the destructive app-layer commands
 * (name register/update/transfer/renew/set-record/set-text, message send,
 * market offer/buy, swap initiate/participate, message mark-read) land in the
 * catalog as PLANNED native leaves (fail-closed, exit 3) until the app-write
 * plan/commit handshake is wired — mirroring the wallet-send precedent in
 * config/commands/core.def. The backing RPC methods stay available at the
 * node RPC layer, so no capability is lost by keeping them PLANNED here. */

#ifndef ZCL_CONTROLLERS_APP_NATIVE_HANDLERS_H
#define ZCL_CONTROLLERS_APP_NATIVE_HANDLERS_H

#include "controllers/native_handler_body.h"

#ifdef __cplusplus
extern "C" {
#endif

/* zcl_tokens: no args -> zslp_listtokens. */
char *zcl_native_zslp_listtokens_body(const struct json_value *args,
                                      struct zcl_native_body_err *err);

/* zcl_name_resolve: name -> name_resolve. */
char *zcl_native_name_resolve_body(const struct json_value *args,
                                   struct zcl_native_body_err *err);

/* zcl_name_list: no args -> name_list. */
char *zcl_native_name_list_body(const struct json_value *args,
                                struct zcl_native_body_err *err);

/* zcl_msg_inbox: no args -> msg_inbox (full inbox, newest first). */
char *zcl_native_msg_inbox_body(const struct json_value *args,
                                struct zcl_native_body_err *err);

/* zcl_market_list: no args -> zmarket_list. */
char *zcl_native_zmarket_list_body(const struct json_value *args,
                                   struct zcl_native_body_err *err);

/* zcl_market_status: no args -> zmarket_status. */
char *zcl_native_zmarket_status_body(const struct json_value *args,
                                     struct zcl_native_body_err *err);

/* zcl_swap_chains: no args -> swap_chains. */
char *zcl_native_swap_chains_body(const struct json_value *args,
                                  struct zcl_native_body_err *err);

/* zcl_swap_list: optional state filter -> swap_list. */
char *zcl_native_swap_list_body(const struct json_value *args,
                                struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_APP_NATIVE_HANDLERS_H */
