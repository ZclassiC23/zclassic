/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Thin RPC client used by every MCP controller.  Talks to the local
 * zclassic23 node over HTTP on 127.0.0.1 using the cookie file in
 * the data directory for auth.  This is the *only* file in the MCP
 * tree that knows how to speak to the node. */

#ifndef ZCL_MCP_RPC_CLIENT_H
#define ZCL_MCP_RPC_CLIENT_H

/* Call once at startup (from tools/mcp_server.c main loop). */
void mcp_rpc_client_init(const char *datadir, int rpc_port);

/* Return the datadir passed to mcp_rpc_client_init (empty string if
 * not yet initialized). The pointer is to a static buffer owned by
 * the client; callers must not free or modify it. */
const char *mcp_rpc_client_datadir(void);

/* Invoke a JSON-RPC method on the local node.
 * params_json may be NULL (sends []) or a JSON array string.
 * Returns a malloc'd JSON string (either the "result" field or an
 * error stub).  Never returns NULL in practice — on connection
 * failure, returns a minimal error object instead.  Caller frees. */
char *mcp_node_rpc(const char *method, const char *params_json);

#ifdef ZCL_TESTING
typedef char *(*mcp_node_rpc_test_fn)(const char *method,
                                      const char *params_json);
void mcp_rpc_client_set_test_hook(mcp_node_rpc_test_fn fn);
#endif

#endif
