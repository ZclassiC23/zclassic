/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Thin loopback RPC client used by the native command handlers and the
 * tools/command CLI.  Talks to the local zclassic23 node over HTTP on
 * 127.0.0.1 using the cookie file in the data directory for auth. This is
 * the only public interface that knows how to speak to the node this way. */

#ifndef ZCL_CONTROLLERS_RPC_CLIENT_H
#define ZCL_CONTROLLERS_RPC_CLIENT_H

/* Call once at startup (from the node/CLI entry point). */
void node_rpc_client_init(const char *datadir, int rpc_port);

/* Return the datadir passed to node_rpc_client_init (empty string if
 * not yet initialized). The pointer is to a static buffer owned by
 * the client; callers must not free or modify it. */
const char *node_rpc_client_datadir(void);

/* Invoke a JSON-RPC method on the local node.
 * params_json may be NULL (sends []) or a JSON array string.
 * Returns a malloc'd JSON string (either the "result" field or an
 * error stub).  Never returns NULL in practice — on connection
 * failure, returns a minimal error object instead.  Caller frees.
 *
 * The client uses the out-of-process HTTP path and returns the bare JSON-RPC
 * result value, or the error object on failure. */
char *node_rpc_call(const char *method, const char *params_json);

/* The default out-of-process HTTP backend (socket + JSON-RPC POST). */
char *node_rpc_call_http(const char *method, const char *params_json);

#ifdef ZCL_TESTING
typedef char *(*node_rpc_test_fn)(const char *method,
                                  const char *params_json);
void node_rpc_client_set_test_hook(node_rpc_test_fn fn);
#endif

#endif
