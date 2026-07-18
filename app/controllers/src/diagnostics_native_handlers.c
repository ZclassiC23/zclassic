/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native argument parsing and RPC composition for diagnostic commands. See
 * controllers/native_handler_body.h for the failure contract. */

#include "controllers/diagnostics_native_handlers.h"

#include "json/json.h"
#include "controllers/rpc_client.h"
#include "controllers/rpc_params.h"
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

    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, sql ? sql : "");
    rpc_arg_builder_push_int(&p, json_get_int_or(args, "limit", 10));
    char *pjson = rpc_arg_builder_to_json(&p);

    char *out = pjson ? node_rpc_call("dbquery", pjson) : NULL;
    free(pjson);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "dbquery");
        LOG_NULL("native.diag", "RPC %s returned null", "dbquery");
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

    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, pattern ? pattern : "");
    rpc_arg_builder_push_int(&p, json_get_int_or(args, "since_secs", 300));
    rpc_arg_builder_push_int(&p, json_get_int_or(args, "max_lines",   50));
    rpc_arg_builder_push_str(&p, level && level[0] ? level : "all");
    char *pjson = rpc_arg_builder_to_json(&p);

    char *out = pjson ? node_rpc_call("getnodelog", pjson) : NULL;
    free(pjson);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "getnodelog");
        LOG_NULL("native.diag", "RPC %s returned null", "getnodelog");
    }
    return out;
}

/* ── Tier-1 hot-swap: NOT YET ELIGIBLE ─────────────────────────
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
