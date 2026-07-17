/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral operator rollup-dashboard bodies.
 *
 * The native successors of the legacy MCP ops controller's operator read
 * tools that had no native twin: zcl_operator_snapshot, zcl_operator_summary,
 * zcl_milestone, zcl_mirror_status, zcl_self_heal_stats. Each takes the
 * leaf's argument object and returns one heap-allocated JSON body (caller
 * frees) — the native command bridge (tools/command/native_command.c) calls
 * these directly and wraps the body in the zcl.result.v1 envelope; the MCP
 * router is never entered. On failure each returns NULL and fills
 * struct zcl_native_body_err with the error tier + a human-readable message,
 * having already logged the failure (LOG_NULL). A body that itself carries an
 * RPC-level {"error":...} object is a SUCCESS return, forwarded verbatim — the
 * bridge decides how to surface it. These are native-only leaves (not shared
 * with an MCP wrapper), so the dual-run byte-identity constraint does not
 * apply. */

#include "controllers/ops_native_handlers.h"
#include "controllers/native_handler_body.h"
#include "controllers/status_native_helpers.h"

#include "json/json.h"
#include "mcp/rpc_client.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/process_block.h"

#include <stdio.h>
#include <stdlib.h>

/* Forward the raw body of a no-argument, read-only RPC. The node RPC returns
 * the composition already (the legacy MCP tool was a pure pass-through), so the
 * only failure this body distinguishes is a null response; a returned RPC-level
 * error object is forwarded verbatim for the bridge to surface. */
static char *ops_rpc_passthrough_body(const char *method,
                                      struct zcl_native_body_err *err)
{
    char *raw = mcp_node_rpc(method, NULL);
    if (!raw) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        (void)snprintf(err->message, sizeof(err->message),
                       "RPC %s returned null", method);
        LOG_NULL("ops.native", "RPC %s returned null", method);
    }
    return raw;
}

char *zcl_native_operator_snapshot_body(const struct json_value *args,
                                        struct zcl_native_body_err *err)
{
    (void)args;
    return ops_rpc_passthrough_body("operatorsnapshot", err);
}

char *zcl_native_operator_summary_body(const struct json_value *args,
                                       struct zcl_native_body_err *err)
{
    (void)args;
    char *raw = mcp_node_rpc("operatorsnapshot", NULL);
    struct json_value root;
    if (!status_parse_rpc_json(&root, raw, JSON_OBJ)) {
        json_free(&root);
        free(raw);
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        (void)snprintf(err->message, sizeof(err->message),
                       "operatorsnapshot returned null or an error");
        LOG_NULL("ops.native",
                 "operatorsnapshot returned null or an error");
    }
    const struct json_value *summary = json_get(&root, "summary");
    if (!summary || summary->type != JSON_OBJ) {
        json_free(&root);
        free(raw);
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        (void)snprintf(err->message, sizeof(err->message),
                       "operatorsnapshot response has no summary object");
        LOG_NULL("ops.native",
                 "operatorsnapshot response has no summary object");
    }
    char *body = zcl_json_value_to_body((struct json_value *)summary,
                                        "native_operator_summary_body");
    json_free(&root);
    free(raw);
    if (!body) {
        err->status = ZCL_NATIVE_BODY_INTERNAL;
        (void)snprintf(err->message, sizeof(err->message),
                       "malloc failed for %s", "operator summary response");
        LOG_NULL("ops.native", "malloc failed for %s",
                 "operator summary response");
    }
    return body;
}

char *zcl_native_milestone_body(const struct json_value *args,
                                struct zcl_native_body_err *err)
{
    (void)args;
    return ops_rpc_passthrough_body("milestone", err);
}

char *zcl_native_mirror_status_body(const struct json_value *args,
                                    struct zcl_native_body_err *err)
{
    (void)args;
    return ops_rpc_passthrough_body("getmirrorstatus", err);
}

char *zcl_native_self_heal_stats_body(const struct json_value *args,
                                      struct zcl_native_body_err *err)
{
    (void)args;
    struct self_heal_scan_stats stats;
    process_block_self_heal_stats_snapshot(&stats);

    char *out = zcl_malloc(512, "self_heal_stats_body");
    if (!out) {
        err->status = ZCL_NATIVE_BODY_INTERNAL;
        (void)snprintf(err->message, sizeof(err->message),
                       "malloc failed for %s", "self-heal stats response");
        LOG_NULL("ops.native", "malloc failed for %s",
                 "self-heal stats response");
    }
    (void)snprintf(out, 512,
        "{"
        "\"tx_index_hits\":%llu,"
        "\"scan_hits\":%llu,"
        "\"scan_exhausted\":%llu,"
        "\"scan_blocks_checked_total\":%llu,"
        "\"scan_depth_limit\":%d"
        "}",
        (unsigned long long)stats.tx_index_hits,
        (unsigned long long)stats.scan_hits,
        (unsigned long long)stats.scan_exhausted,
        (unsigned long long)stats.scan_blocks_checked_total,
        process_block_self_heal_scan_depth_limit());
    return out;
}
