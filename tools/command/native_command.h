/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_TOOLS_NATIVE_COMMAND_H
#define ZCL_TOOLS_NATIVE_COMMAND_H

#include "kernel/command_registry.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool zcl_native_command_is_root(const char *word);

/* Resolve argv under a canonical root through the registry and print exactly
 * one bounded JSON document (branch menu, discovery document, common result
 * envelope, or structured unknown-command error). Returns a contract exit code
 * (0..6). A typo under a canonical branch returns the structured unknown-branch
 * error and NEVER falls through to the arbitrary RPC method fallback.
 * datadir/rpc_port target the running node for READ-ONLY bridge leaves. */
int zcl_native_command_main(const char *root_word,
                            const char *const *args, int nargs,
                            const char *datadir, int rpc_port);

/* Generic transport binding for READ-ONLY Core/Ops leaves. Resolves the
 * leaf's canonical path to exactly one live MCP tool and proxies the call
 * through the in-process MCP middleware, then wraps the tool body in the
 * common zcl.result.v1 envelope. Bound by config/src/command_catalog.c. */
void zcl_native_bridge_command(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply);

/* Return the MCP tool name bound to a canonical READ-ONLY leaf path, or NULL
 * when the path has no bridge binding. Pure lookup — no node contact. Used by
 * the golden catalog test to prove every bridged READY leaf has a binding. */
const char *zcl_native_bridge_tool_for_path(const char *path);

void zcl_native_handle_discover_help(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_search(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_describe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_schema(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_core_boundary(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_describe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_simulate(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_change_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_app_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_app_inspect(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_NATIVE_COMMAND_H */
