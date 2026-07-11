/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for MCP meta-domain read tools
 * (ZERO-MCP W0-A). Called by both the MCP wrapper
 * (tools/mcp/controllers/meta_controller.c) and the future native
 * command bridge (tools/command/native_command.c) — see
 * controllers/native_handler_body.h for the shared contract these
 * functions satisfy. */

#ifndef ZCL_CONTROLLERS_META_NATIVE_HANDLERS_H
#define ZCL_CONTROLLERS_META_NATIVE_HANDLERS_H

#include "controllers/native_handler_body.h"

#ifdef __cplusplus
extern "C" {
#endif

/* zcl_metrics — Prometheus-text metrics dump wrapped in a JSON
 * envelope. args is unused (no parameters). */
char *zcl_native_metrics_body(const struct json_value *args,
                              struct zcl_native_body_err *err);

/* zcl_consensus_report — consensus-reject counter snapshot (per
 * (kind, reason) counts plus totals/overflow). args is unused (no
 * parameters). */
char *zcl_native_consensus_report_body(const struct json_value *args,
                                       struct zcl_native_body_err *err);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_META_NATIVE_HANDLERS_H */
