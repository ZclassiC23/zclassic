/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_MCP_DEV_RPC_BRIDGE_H
#define ZCL_MCP_DEV_RPC_BRIDGE_H

#include <stdbool.h>

struct rpc_table;

/* Exact-lane predicate shared by boot and focused tests. Only the literal
 * $HOME/.zclassic-c23-dev path (with an optional trailing slash) is accepted. */
bool dev_mcp_rpc_bridge_datadir_allowed(const char *datadir);

/* Register the authenticated JSON-RPC bridge. Release and non-dev lanes are
 * successful no-ops. In ZCL_DEV_BUILD the exact dev lane initializes one
 * resident MCP router with the in-process RPC backend and appends
 * dev_hotswap/dev_mcp_call. False means required exact-dev registration
 * failed. */
bool register_dev_mcp_rpc_commands(struct rpc_table *table,
                                   const char *datadir,
                                   int rpc_port);

#ifdef ZCL_TESTING
/* Test-only entry to compile/prove the dev branch while the ordinary test
 * binary still proves the public release registration function is a stub. */
bool dev_mcp_rpc_bridge_test_register(struct rpc_table *table,
                                      const char *datadir,
                                      int rpc_port);
#endif

#endif /* ZCL_MCP_DEV_RPC_BRIDGE_H */
