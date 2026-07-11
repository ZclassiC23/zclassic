/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_TOOLS_NATIVE_COMMAND_H
#define ZCL_TOOLS_NATIVE_COMMAND_H

#include "kernel/command_registry.h"
#include "controllers/native_handler_body.h"

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
 * leaf's canonical path to exactly one MCP-free dispatch (W0-A): either a
 * re-homed transport-neutral body function (the same composition the MCP
 * controller now wraps — app/controllers *_native_handlers.c) or, for a
 * pure pass-through leaf, the backing JSON-RPC method directly. The MCP
 * router/middleware is never entered. The body is wrapped in the common
 * zcl.result.v1 envelope. Bound by config/src/command_catalog.c. */
void zcl_native_bridge_command(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply);

/* Run a bridged leaf with an EXPLICIT body function — the reusable core of
 * zcl_native_bridge_command: build args from the request, dispatch the passed
 * `body` (or, when `body` is NULL and the leaf is a pure 1:1 proxy, the
 * backing JSON-RPC method directly), then project the body into the reply
 * envelope. zcl_native_bridge_command is a thin wrapper that supplies
 * zcl_native_bridge_body_for_path(path); a hot-swap generation instead
 * supplies its own freshly-compiled body. A NULL `body` on a path with no
 * direct-RPC binding yields the same NO_BRIDGE_BINDING reply as an unknown
 * path. */
void zcl_native_bridge_run(const struct zcl_command_request *request,
                           zcl_native_body_fn body,
                           struct zcl_command_reply *reply);

/* Project a bridged tool body into reply->data bounded by request->view
 * (summary|normal|full), request->budget_bytes, request->max_items, and
 * request->cursor, emitting an explicit `_page` descriptor and — when
 * truncated — one structured retrieval next-command. Exposed for golden tests
 * so progressive disclosure can be proven without contacting a node. */
void zcl_native_bridge_project(const struct zcl_command_request *request,
                               const struct json_value *body,
                               struct zcl_command_reply *reply);

/* Return the MCP tool name bound to a canonical READ-ONLY leaf path, or NULL
 * when the path has no bridge binding. Pure lookup — no node contact. Used by
 * the golden catalog test to prove every bridged READY leaf has a binding.
 * (Kept as dual-run equivalence metadata: it names which MCP tool a native
 * leaf mirrors; the native dispatch itself no longer routes through it.) */
const char *zcl_native_bridge_tool_for_path(const char *path);

/* MCP-free dispatch lookups (W0-A). Every bridged leaf resolves to exactly
 * one of the two: a re-homed body function OR a direct JSON-RPC method.
 * Pure lookups — no node contact. The golden catalog test proves the union
 * covers every bridged leaf and never overlaps. */
zcl_native_body_fn zcl_native_bridge_body_for_path(const char *path);
const char *zcl_native_bridge_rpc_for_path(const char *path);

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
/* dev.vcs.revert — one-command source+binary revert (see
 * tools/command/native_dev_command.c). A release build's copy of this
 * function is a `#ifndef ZCL_DEV_BUILD` stub that fails BLOCKED without
 * touching lib/vcs/ or spawning anything. */
void zcl_native_handle_dev_vcs_revert(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* dev.vcs.seal.grant — owner-run ZVCS unseal-token ritual (see
 * tools/command/native_dev_command.c). Mirrors dev.vcs.revert's shape: a
 * release build's copy of this function is a `#ifndef ZCL_DEV_BUILD` stub
 * that fails BLOCKED without touching lib/vcs/ or spawning anything. */
void zcl_native_handle_dev_vcs_seal_grant(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_app_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_app_inspect(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.state — generic subsystem state dump (the native successor of the MCP
 * `zcl_state` primitive). Dispatches the `dumpstate` RPC method directly; the
 * MCP router/middleware is never entered. `subsystem` (required) selects the
 * owning module's *_dump_state_json; `key` is subsystem-specific (e.g. a
 * block_index height/hash). Bound by config/src/command_catalog.c. */
void zcl_native_handle_ops_state(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.selftest — node-free registry self-test (the native successor of the MCP
 * `zcl_self_test mode=registry`). Sweeps every catalog leaf for the static
 * well-formedness the registry guarantees (READY ⇒ dispatchable handler +
 * schema/example + read-effect/risk agreement + a bound bridge tool) and
 * reports total/pass/fail/skip + the failing paths. Deterministic and
 * node-independent so the dev-lane deploy verify can gate on fail == 0. */
void zcl_native_handle_ops_selftest(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* Dev-build-only executors.  The catalog binds these only when
 * ZCL_DEV_BUILD is set; release objects neither reference nor link them. */
#ifdef ZCL_DEV_BUILD
void zcl_native_handle_dev_change_apply(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_ensure(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_wait(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_stop(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_test_run(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_test_sim(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_generation_current(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_generation_history(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_diagnose_latest(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* dev.hotswap.apply / dev.hotswap.probe — Tier-1 in-process hot-swap of native
 * command leaves. Short-lived CLI processes that forward over JSON-RPC to the
 * resident dev node (see tools/command/native_dev_hotswap.c). */
void zcl_native_handle_dev_hotswap_apply(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_hotswap_probe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_NATIVE_COMMAND_H */
