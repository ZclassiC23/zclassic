/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for MCP net-domain read tools
 *. Called by both the MCP wrapper
 * (tools/mcp/controllers/net_controller.c) and the future native
 * command bridge (tools/command/native_command.c) — see
 * controllers/native_handler_body.h for the shared contract these
 * functions satisfy. */

#ifndef ZCL_CONTROLLERS_NET_NATIVE_HANDLERS_H
#define ZCL_CONTROLLERS_NET_NATIVE_HANDLERS_H

#include "controllers/native_handler_body.h"

#ifdef __cplusplus
extern "C" {
#endif

/* zcl_peer_incidents — bounded peer-lifecycle incident view. Tries the
 * direct "peerincidents" RPC first; on METHOD_NOT_FOUND (talking to an
 * older node build) falls back to normalizing `dumpstate peer_lifecycle
 * incidents`. args is unused (no parameters). */
char *zcl_native_peer_incidents_body(const struct json_value *args,
                                     struct zcl_native_body_err *err);

/* zcl_onion_health — probe the in-process onion service via a direct
 * onion_service_handle_request() call (no Tor circuit, no SOCKS).
 * Optional args: {"path": "/directory.json"}. */
char *zcl_native_onion_health_body(const struct json_value *args,
                                   struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_NET_NATIVE_HANDLERS_H */
