/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Transport-neutral re-homed bodies for the zcl_metrics and
 * zcl_consensus_report MCP tools (ZERO-MCP W0-A). Moved out of
 * tools/mcp/controllers/meta_controller.c so both the MCP wrapper and
 * the future native command bridge can call the same composition;
 * see controllers/native_handler_body.h for the contract these
 * functions satisfy. The MCP wrapper (meta_controller.c) maps a NULL
 * return + err->status back onto the historical res->error /
 * res->error_message so the MCP surface stays byte-identical. */

#include "controllers/meta_native_handlers.h"

#include "json/json.h"
#include "mcp/metrics.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors the legacy mcp_res_set_oom (tools/mcp/controllers.h) tail but
 * returns NULL (this TU's functions are char*-returning body fns, not
 * int-returning MCP handlers). cap==0 means no single byte count applies,
 * matching the legacy convention (never hit by the call sites below, both
 * pass a nonzero cap). */
static char *meta_native_oom(struct zcl_native_body_err *err, size_t cap,
                             const char *what)
{
    err->status = ZCL_NATIVE_BODY_INTERNAL;
    snprintf(err->message, sizeof(err->message), "malloc failed for %s", what);
    if (cap > 0)
        LOG_NULL("mcp.meta", "malloc failed for %s (%zu bytes)", what, cap);
    LOG_NULL("mcp.meta", "malloc failed for %s", what);
}

/* ── zcl_metrics ─────────────────────────────────────────────────── */

char *zcl_native_metrics_body(const struct json_value *args,
                              struct zcl_native_body_err *err)
{
    (void)args;
    size_t cap = 131072;
    char *raw = zcl_malloc(cap, "metrics_raw");
    if (!raw)
        return meta_native_oom(err, cap, "metrics buffer");
    size_t n = mcp_metrics_render_prometheus(raw, cap);

    /* Wrap the Prometheus text in a JSON envelope so the stdio layer
     * can shuttle it as a tool result.  Escape quotes + newlines. */
    size_t out_cap = n * 2 + 128;
    char *out = zcl_malloc(out_cap, "metrics_json_body");
    if (!out) {
        free(raw);
        return meta_native_oom(err, out_cap, "metrics JSON envelope");
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_cap - pos,
        "{\"format\":\"prometheus\",\"text\":\"");
    for (size_t i = 0; i < n && pos + 4 < out_cap; i++) {
        char c = raw[i];
        if (c == '"')       { out[pos++] = '\\'; out[pos++] = '"'; }
        else if (c == '\\') { out[pos++] = '\\'; out[pos++] = '\\'; }
        else if (c == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (c == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else if (c == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else                { out[pos++] = c; }
    }
    pos += (size_t)snprintf(out + pos, out_cap - pos,
        "\",\"total_requests\":%llu,\"total_errors\":%llu,\"counter_count\":%zu}",
        (unsigned long long)mcp_metrics_total_requests(),
        (unsigned long long)mcp_metrics_total_errors(),
        mcp_metrics_counter_count());

    free(raw);
    return out;
}

/* ── zcl_consensus_report ────────────────────────────────────────── */

/* zcl_consensus_report — consensus-reject counter snapshot.
 * Surfaces the `EV_CONSENSUS_REJECT_TX`/`_BLOCK` ring as a bounded
 * (kind, reason) -> count table plus per-kind totals and overflow
 * buckets. This is the dashboards/alerting view; the per-hash
 * `zcl_explain_reject` lookup is the targeted companion. */
char *zcl_native_consensus_report_body(const struct json_value *args,
                                       struct zcl_native_body_err *err)
{
    (void)args;
    /* Cap large enough for the full 48-slot table plus overflow +
     * totals envelope (worst case ~= 48 x 80 bytes per entry). */
    char body[8192];
    size_t n = mcp_metrics_consensus_report_json(body, sizeof(body));
    if (n == 0) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "consensus report generation returned empty");
        LOG_NULL("mcp.meta", "consensus_report_json returned 0 bytes");
    }
    char *out = strdup(body);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_INTERNAL;
        snprintf(err->message, sizeof(err->message),
                 "strdup failed for consensus report");
        LOG_NULL("mcp.meta", "strdup failed for consensus_report (%zu bytes)", n);
    }
    return out;
}

/* ── Tier-1 hot-swap: native.leaves generation entrypoint (W1-B/C) ──────
 * Dev-only (compiled only under -DZCL_HOTSWAP_GEN, a generation .so build;
 * expands to nothing in the node/release TU — see ZCL_HOTSWAP_EXPORT_LEAVES
 * in lib/hotswap/include/hotswap/hotswap.h). Stages every native command
 * leaf this controller owns; the resident bridge re-points them at THIS
 * TU's freshly-compiled bodies via zcl_native_bridge_run(). Probe is
 * ops.metrics: zcl_native_metrics_body ignores `args` ((void)args) and
 * unconditionally renders the Prometheus text buffer into a JSON envelope
 * with no top-level "error" key, so the empty-args self-test dispatch
 * succeeds. core.consensus.report is equally args-free ((void)args) and
 * would work as a probe too — either is valid; ops.metrics is kept as the
 * pilot probe. See config/hotswap_eligible.def. */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_PROBE_LEAF "ops.metrics"
#include "hotswap/hotswap.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void tramp_consensus_report(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_consensus_report_body, reply);
}

static void tramp_metrics(const struct zcl_command_request *request,
                          struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_metrics_body, reply);
}

static const struct zcl_hotswap_leaf_replacement k_leaves[] = {
    { "core.consensus.report", tramp_consensus_report },
    { "ops.metrics",           tramp_metrics },
};

ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, sizeof(k_leaves) / sizeof(k_leaves[0]))
#endif /* ZCL_HOTSWAP_GEN */
