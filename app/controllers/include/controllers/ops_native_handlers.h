/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral operator rollup-dashboard bodies (native successors of the
 * legacy MCP ops controller's zcl_operator_snapshot / zcl_operator_summary /
 * zcl_milestone / zcl_mirror_status / zcl_self_heal_stats tools). Each takes
 * the leaf's argument object and returns one heap-allocated JSON body (caller
 * frees); on failure it returns NULL and fills struct zcl_native_body_err with
 * the error tier + a human-readable message, having already logged the failure.
 * The native command bridge (tools/command/native_command.c) calls these
 * directly and wraps the body in the zcl.result.v1 envelope — no MCP router.
 * These are native-only (not wired into any MCP wrapper), so the byte-identity
 * dual-run constraint does not apply. */

#ifndef ZCL_CONTROLLERS_OPS_NATIVE_HANDLERS_H
#define ZCL_CONTROLLERS_OPS_NATIVE_HANDLERS_H

#include "controllers/native_handler_body.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ops.debug.dash.snapshot — the exact native operatorsnapshot payload
 * (bounded target-owned component snapshots, capture coherence, typed
 * blockers, invariants, and the native summary projection), forwarded
 * verbatim after a JSON-object shape check. */
char *zcl_native_operator_snapshot_body(const struct json_value *args,
                                        struct zcl_native_body_err *err);

/* ops.debug.dash.summary — the fail-closed operator summary: the `summary`
 * sub-object projected out of the native operatorsnapshot payload. */
char *zcl_native_operator_summary_body(const struct json_value *args,
                                       struct zcl_native_body_err *err);

/* ops.debug.dash.milestone — node-computed progress toward the next version
 * milestone (systems/goals/subgoals bars and MVP criteria). */
char *zcl_native_milestone_body(const struct json_value *args,
                                struct zcl_native_body_err *err);

/* ops.debug.dash.mirror — canonical zclassic23/zclassicd mirror lockstep
 * status: both heights and hashes, lag, reachability, catch-up counters. */
char *zcl_native_mirror_status_body(const struct json_value *args,
                                    struct zcl_native_body_err *err);

/* ops.debug.dash.selfheal — self-heal UTXO recovery counters (tx-index hits,
 * bounded scan hits/exhaustion, total scanned blocks, active scan depth). */
char *zcl_native_self_heal_stats_body(const struct json_value *args,
                                      struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_OPS_NATIVE_HANDLERS_H */
