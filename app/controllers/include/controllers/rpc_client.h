/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Thin loopback RPC client used by the native command handlers and the
 * tools/command CLI.  Talks to the local zclassic23 node over HTTP on
 * 127.0.0.1 using the cookie file in the data directory for auth.  This is
 * the *only* file that knows how to speak to the node this way.  Symbols keep
 * their historical mcp_* names (re-homed from tools/mcp/ in zero-MCP W2). */

#ifndef ZCL_CONTROLLERS_RPC_CLIENT_H
#define ZCL_CONTROLLERS_RPC_CLIENT_H

/* Call once at startup (from the node/CLI entry point). */
void mcp_rpc_client_init(const char *datadir, int rpc_port);

/* Return the datadir passed to mcp_rpc_client_init (empty string if
 * not yet initialized). The pointer is to a static buffer owned by
 * the client; callers must not free or modify it. */
const char *mcp_rpc_client_datadir(void);

/* Invoke a JSON-RPC method on the local node.
 * params_json may be NULL (sends []) or a JSON array string.
 * Returns a malloc'd JSON string (either the "result" field or an
 * error stub).  Never returns NULL in practice — on connection
 * failure, returns a minimal error object instead.  Caller frees.
 *
 * Transport is selectable: the default is the out-of-process HTTP path
 * (mcp_node_rpc_http). When the in-process MCP transport is enabled
 * (-mcp-inprocess), mcp_rpc_client_use_inprocess() flips the backend to
 * mcp_node_rpc_inproc, which calls the node's live rpc_table directly
 * — no socket, no second JSON marshal. Both return the SAME malloc'd
 * JSON shape (the bare "result" value, or the "error" object on failure)
 * so every controller and mcp_return_rpc_body is transport-agnostic. */
char *mcp_node_rpc(const char *method, const char *params_json);

/* The default out-of-process HTTP backend (socket + JSON-RPC POST). */
char *mcp_node_rpc_http(const char *method, const char *params_json);

/* The in-process backend: executes the live rpc_table directly.
 * Requires a fully booted node (rpc_http_active_table() != NULL); if the
 * table is not yet live it returns the same actionable error envelope the
 * HTTP path produces when it cannot reach the node. */
char *mcp_node_rpc_inproc(const char *method, const char *params_json);

/* Select the in-process backend for all subsequent mcp_node_rpc calls.
 * Default (never called) keeps the HTTP backend, so the existing proxy
 * path is byte-for-byte unchanged. Idempotent. */
void mcp_rpc_client_use_inprocess(void);

#ifdef ZCL_TESTING
typedef char *(*mcp_node_rpc_test_fn)(const char *method,
                                      const char *params_json);
void mcp_rpc_client_set_test_hook(mcp_node_rpc_test_fn fn);
#endif

#endif
