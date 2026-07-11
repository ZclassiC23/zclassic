/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for zcl_sql / zcl_node_log
 * (ZERO-MCP W0-A). Each function is the argument-parsing plus
 * RPC-composition core of the legacy MCP handler in
 * tools/mcp/controllers/diagnostics_controller.c, with the MCP-specific
 * error envelope stripped out — see controllers/native_handler_body.h for
 * the failure contract. Called by both the MCP wrapper handler (which maps
 * a NULL return onto the historical res->error / res->error_message) and
 * the native command bridge (tools/command/native_command.c). */

#include "controllers/diagnostics_native_handlers.h"

#include "json/json.h"
#include "mcp/rpc_client.h"
#include "mcp/rpc_params.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>

/* SELECT-only SQL passthrough to node.db. Marked destructive in middleware
 * not because it mutates (it can't) but because arbitrary scans against a
 * 100M-row table can be expensive. */
char *zcl_native_sql_body(const struct json_value *args,
                           struct zcl_native_body_err *err)
{
    const char *sql = json_get_str(json_get(args, "sql"));

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, sql ? sql : "");
    mcp_params_push_int(&p, json_get_int_or(args, "limit", 10));
    char *pjson = mcp_params_to_json(&p);

    char *out = pjson ? mcp_node_rpc("dbquery", pjson) : NULL;
    free(pjson);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "dbquery");
        LOG_NULL("mcp.diag", "RPC %s returned null", "dbquery");
    }
    return out;
}

/* Reverse-scan node.log via getnodelog RPC. Server-side regex match + level
 * filter. Timestamp filtering is exact for lines that carry a supported
 * timestamp; legacy undated lines remain eligible and are counted in the
 * result metadata. Bounded memory: chunks the live log and stops at
 * max_lines. */
char *zcl_native_node_log_body(const struct json_value *args,
                                struct zcl_native_body_err *err)
{
    const char *pattern = json_get_str(json_get(args, "pattern"));
    const char *level = json_get_str(json_get(args, "level"));

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, pattern ? pattern : "");
    mcp_params_push_int(&p, json_get_int_or(args, "since_secs", 300));
    mcp_params_push_int(&p, json_get_int_or(args, "max_lines",   50));
    mcp_params_push_str(&p, level && level[0] ? level : "all");
    char *pjson = mcp_params_to_json(&p);

    char *out = pjson ? mcp_node_rpc("getnodelog", pjson) : NULL;
    free(pjson);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "getnodelog");
        LOG_NULL("mcp.diag", "RPC %s returned null", "getnodelog");
    }
    return out;
}

/* ── Tier-1 hot-swap: NOT YET ELIGIBLE (W1-B/C) ─────────────────────────
 * No ZCL_HOTSWAP_EXPORT_LEAVES here on purpose. A native.leaves generation
 * self-test dispatches the manifest's declared probe leaf with NO input
 * (see zcl_hotswap_default_self_test / hotswap_commit_probe_candidate),
 * and BOTH leaves this controller owns reject that:
 *   - core.storage.query  -> zcl_native_sql_body:      diag_rpc_dbquery
 *     requires a non-empty `sql` (returns "dbquery: missing sql").
 *   - ops.logs             -> zcl_native_node_log_body: diag_rpc_getnodelog
 *     requires a non-empty `pattern` (returns "getnodelog: missing pattern").
 * Both surface as a top-level RPC error, which would make the generation
 * self-test spuriously fail on every load attempt. Revisit if a param-free
 * probe leaf becomes available for this controller (see the "Still
 * reload-required" note in config/hotswap_eligible.def). */
