/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP meta controller: operator tools that introspect the MCP surface
 * itself.  These tools are domain="ops" but live in their own file
 * because they pull in the router internals.
 *
 *   zcl_tools_list — dump the routing table as JSON
 *   zcl_self_test  — call every tool with safe defaults, report pass/fail
 *   zcl_logtail    — tail the structured event log, optional domain filter
 */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../metrics.h"

#include "json/json.h"
#include "net/peer_scoring.h"
#include "net/peer_bandwidth.h"
#include "rpc/http_middleware.h"
#include "rpc/rpc_timeout.h"
#include "services/legacy_mirror_sync_service.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "../middleware.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Destructive flags and self_test argument overrides live inline on the
 * route itself — see the `flags` and `self_test_args` fields on each entry
 * in the k_routes[] tables in {wallet,net,app,ops,chain,meta}_controller.c. */

/* True if any required param has no default value we can synthesize. */
static bool has_unfillable_required(const struct mcp_tool_route *r)
{
    for (size_t i = 0; i < r->num_params; i++) {
        const struct mcp_param_spec *p = &r->params[i];
        if (p->required && !p->default_json)
            return true;
    }
    return false;
}

/* ── Handlers ────────────────────────────────────────────────── */

static int h_zcl_tools_list(const struct mcp_request *req,
                             struct mcp_response *res)
{
    (void)req;
    size_t cap = 131072;
    char *out = zcl_malloc(cap, "tools_list_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for tools list response");
        LOG_ERR("mcp.meta", "malloc failed for tools_list body (%zu bytes)", cap);
        return -1; // raw-return-ok:logged-oom
    }
    int pos = snprintf(out, cap, "{\"count\":%zu,\"tools\":",
                       mcp_router_count());
    pos += (int)mcp_router_tools_list_json(out + pos, cap - (size_t)pos);
    if ((size_t)pos + 2 < cap) { out[pos++] = '}'; out[pos] = 0; }
    res->body = out;
    return 0;
}

static int h_zcl_self_test(const struct mcp_request *req,
                            struct mcp_response *res)
{
    (void)req;

    size_t cap = 131072;
    char *out = zcl_malloc(cap, "self_test_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for self-test response");
        LOG_ERR("mcp.meta", "malloc failed for self_test body (%zu bytes)", cap);
        return -1; // raw-return-ok:logged-oom
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos,
                            "{\"results\":[");

    size_t total = 0, passed = 0, failed = 0, skipped = 0;
    bool first = true;

    for (size_t i = 0; i < mcp_router_count(); i++) {
        const struct mcp_tool_route *r = mcp_router_at(i);
        if (!r) continue;
        total++;

        const char *status;
        const char *reason = NULL;

        const char *override = r->self_test_args;
        struct json_value override_val = {0};
        bool have_override = false;
        if (override && json_read(&override_val, override, strlen(override)))
            have_override = true;

        if (r->flags & MCP_TOOL_FLAG_DESTRUCTIVE) {
            status = "skipped";
            reason = "destructive";
            skipped++;
        } else if (!have_override && has_unfillable_required(r)) {
            status = "skipped";
            reason = "required-param-without-default";
            skipped++;
        } else {
            /* Call the tool with the override args if we have one;
             * otherwise empty args — optional-default params will fall
             * through to router defaults. */
            char *body = mcp_router_dispatch(
                r->name, have_override ? &override_val : NULL);
            if (!body) {
                status = "fail";
                reason = "no-body";
                failed++;
            } else if (strstr(body, "\"error\":{\"code\":") != NULL) {
                status = "fail";
                reason = "error-envelope";
                failed++;
            } else {
                status = "pass";
                passed++;
            }
            free(body);
        }

        if (have_override) json_free(&override_val);

        if (pos + 256 >= cap) break;
        if (!first) out[pos++] = ',';
        first = false;
        pos += (size_t)snprintf(out + pos, cap - pos,
            "{\"tool\":\"%s\",\"domain\":\"%s\",\"status\":\"%s\"%s%s%s}",
            r->name, r->domain ? r->domain : "",
            status,
            reason ? ",\"reason\":\"" : "",
            reason ? reason : "",
            reason ? "\"" : "");
    }

    pos += (size_t)snprintf(out + pos, cap - pos,
        "],\"summary\":{\"total\":%zu,\"pass\":%zu,\"fail\":%zu,"
        "\"skip\":%zu}}",
        total, passed, failed, skipped);

    res->body = out;
    return 0;
}

static int h_zcl_logtail(const struct mcp_request *req,
                          struct mcp_response *res)
{
    int count = (int)json_get_int_or(req->args, "count",  100);
    const char *dom = json_get_str_or(req->args, "domain", NULL);

    char params[64];
    snprintf(params, sizeof(params), "[%d]", count);
    char *raw = mcp_node_rpc("eventlog", params);
    if (!raw) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC eventlog returned null");
        /* LOG_ERR returns -1; keep this terminal before parsing raw. */
        LOG_ERR("mcp.meta", "logtail: eventlog returned null");
    }
    if (!dom || !dom[0]) { res->body = raw; return 0; }

    /* Parse the eventlog response and filter events[] by type prefix. */
    struct json_value root;
    if (!json_read(&root, raw, strlen(raw))) {
        res->body = raw;
        return 0;
    }
    free(raw);

    const struct json_value *events = json_get(&root, "events");
    const struct json_value *ss     = json_get(&root, "sync_state");
    const char *sstate = ss ? json_get_str(ss) : "";

    size_t out_cap = 65536;
    char *out = zcl_malloc(out_cap, "logtail_filtered_body");
    if (!out) {
        json_free(&root);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for logtail filtered response");
        LOG_ERR("mcp.meta", "malloc failed for logtail filtered body (%zu bytes)", out_cap);
        return -1; // raw-return-ok:logged-oom
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_cap - pos,
                            "{\"sync_state\":\"%s\",\"filter\":\"", sstate);
    for (const char *c = dom; *c && pos + 4 < out_cap; c++) {
        if (*c == '"' || *c == '\\') out[pos++] = '\\';
        out[pos++] = *c;
    }
    pos += (size_t)snprintf(out + pos, out_cap - pos, "\",\"events\":[");

    bool first = true;
    size_t dlen = strlen(dom);
    size_t matched = 0;
    if (events && events->type == JSON_ARR) {
        for (size_t i = 0; i < events->num_children; i++) {
            const struct json_value *ev = &events->children[i];
            const struct json_value *ty = json_get(ev, "type");
            const char *tstr = ty ? json_get_str(ty) : "";
            if (!tstr) continue;
            if (strncmp(tstr, dom, dlen) != 0) continue;

            /* Write this event object. Reserve headroom for a possible
             * leading comma and the trailing "],\"matched\":N}" suffix. */
            if (pos + 2048 >= out_cap) break;
            size_t avail = out_cap - pos - (first ? 0 : 1);
            size_t need = json_write(ev, NULL, 0); /* sizing probe */
            if (need >= avail) continue; /* drop this event, don't overrun */
            if (!first) out[pos++] = ',';
            first = false;
            pos += json_write(ev, out + pos, out_cap - pos);
            matched++;
        }
    }
    /* Belt-and-suspenders: never let the trailing snprintf write past the
     * buffer (out_cap - pos must stay a valid, non-underflowing size). */
    if (pos >= out_cap) pos = out_cap - 1;
    pos += (size_t)snprintf(out + pos, out_cap - pos,
                            "],\"matched\":%zu}", matched);

    json_free(&root);
    res->body = out;
    return 0;
}

/* ── Schema export (OpenAPI-ish) ─────────────────────────────── */

static int h_zcl_openapi(const struct mcp_request *req,
                          struct mcp_response *res)
{
    (void)req;
    size_t cap = 262144;
    char *out = zcl_malloc(cap, "openapi_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for OpenAPI response");
        LOG_ERR("mcp.meta", "malloc failed for openapi body (%zu bytes)", cap);
        return -1; // raw-return-ok:logged-oom
    }
    size_t pos = 0;

    pos += (size_t)snprintf(out + pos, cap - pos,
        "{"
        "\"openapi\":\"3.0.0\","
        "\"info\":{\"title\":\"zclassic23 MCP surface\","
                 "\"version\":\"1.0.0\","
                 "\"description\":\"Auto-derived from the MCP router table.\"},"
        "\"paths\":{");

    bool first = true;
    for (size_t i = 0; i < mcp_router_count(); i++) {
        const struct mcp_tool_route *r = mcp_router_at(i);
        if (!r || !r->name) continue;

        if (pos + 4096 >= cap) break;
        if (!first) out[pos++] = ',';
        first = false;

        /* Path entry: "/tools/<name>": { post: { summary, tags, requestBody:
         * { content: { application/json: { schema: <inputSchema> } } },
         * responses: { 200, 4xx } } } */
        pos += (size_t)snprintf(out + pos, cap - pos,
            "\"/tools/%s\":{\"post\":{"
            "\"summary\":\"", r->name);

        /* description (JSON-escape) */
        const char *desc = r->description ? r->description : "";
        for (size_t k = 0; desc[k] && pos + 4 < cap; k++) {
            char c = desc[k];
            if (c == '"' || c == '\\') out[pos++] = '\\';
            if (c == '\n') { out[pos++] = '\\'; c = 'n'; }
            out[pos++] = c;
        }

        pos += (size_t)snprintf(out + pos, cap - pos,
            "\",\"tags\":[\"%s\"],"
            "\"operationId\":\"%s\","
            "\"requestBody\":{\"content\":{\"application/json\":{\"schema\":",
            r->domain ? r->domain : "default", r->name);

        /* inputSchema object */
        pos += mcp_router_input_schema_json(r, out + pos, cap - pos);

        pos += (size_t)snprintf(out + pos, cap - pos,
            "}}},"
            "\"responses\":{"
            "\"200\":{\"description\":\"Success — JSON body from handler\"},"
            "\"400\":{\"description\":\"Validation error envelope\"},"
            "\"401\":{\"description\":\"AUTH_REQUIRED envelope\"},"
            "\"429\":{\"description\":\"RATE_LIMITED envelope\"},"
            "\"504\":{\"description\":\"TOOL_TIMEOUT envelope\"}"
            "}}}");
    }

    pos += (size_t)snprintf(out + pos, cap - pos,
        "},"
        "\"components\":{\"schemas\":{"
        "\"ErrorEnvelope\":{\"type\":\"object\",\"properties\":{"
        "\"error\":{\"type\":\"object\",\"properties\":{"
        "\"code\":{\"type\":\"string\"},"
        "\"message\":{\"type\":\"string\"},"
        "\"tool\":{\"type\":\"string\"},"
        "\"param\":{\"type\":\"string\"}"
        "}}}}"
        "}}}");

    if (pos < cap) out[pos] = 0;
    res->body = out;
    return 0;
}

/* ── Prometheus metrics ─────────────────────────────────────── */

static int h_zcl_metrics(const struct mcp_request *req,
                          struct mcp_response *res)
{
    (void)req;
    size_t cap = 131072;
    char *raw = zcl_malloc(cap, "metrics_raw");
    if (!raw) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for metrics buffer");
        LOG_ERR("mcp.meta", "malloc failed for metrics raw buffer (%zu bytes)", cap);
        return -1; // raw-return-ok:logged-oom
    }
    size_t n = mcp_metrics_render_prometheus(raw, cap);

    /* Wrap the Prometheus text in a JSON envelope so the stdio layer
     * can shuttle it as a tool result.  Escape quotes + newlines. */
    size_t out_cap = n * 2 + 128;
    char *out = zcl_malloc(out_cap, "metrics_json_body");
    if (!out) {
        free(raw);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for metrics JSON envelope");
        LOG_ERR("mcp.meta", "malloc failed for metrics json body (%zu bytes)", out_cap);
        return -1; // raw-return-ok:logged-oom
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
    res->body = out;
    return 0;
}

static int h_zcl_metrics_reset(const struct mcp_request *req,
                                struct mcp_response *res)
{
    (void)req;
    mcp_metrics_reset();
    char *out = strdup("{\"ok\":true,\"reset\":\"mcp_metrics\"}");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "strdup failed for metrics_reset response");
        LOG_ERR("mcp.meta", "strdup failed for metrics_reset response");
    }
    res->body = out;
    return 0;
}

/* zcl_rpc_report — HTTP RPC middleware summary.
 * Live config + stat counters + tracked IPs + active bans from the
 * global rpc_http_middleware registered by httpserver.c. The report
 * also appears in the Prometheus dump emitted by zcl_metrics, but
 * this tool returns a smaller structured JSON object for operators
 * who want a single-call snapshot instead of a full text scrape. */
static int h_zcl_rpc_report(const struct mcp_request *req,
                             struct mcp_response *res)
{
    (void)req;
    char body[2048];
    size_t n = mcp_metrics_rpc_report_json(body, sizeof(body));
    if (n == 0) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC report generation returned empty");
        LOG_ERR("mcp.meta", "rpc_report_json returned 0 bytes");
    }
    res->body = strdup(body);
    if (!res->body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "strdup failed for RPC report");
        LOG_ERR("mcp.meta", "strdup failed for rpc_report (%zu bytes)", n);
    }
    return 0;
}

/* zcl_consensus_report — consensus-reject counter snapshot.
 * Surfaces the `EV_CONSENSUS_REJECT_TX`/`_BLOCK` ring as a bounded
 * (kind, reason) → count table plus per-kind totals and overflow
 * buckets. This is the dashboards/alerting view; the per-hash
 * `zcl_explain_reject` lookup is the targeted companion. */
static int h_zcl_consensus_report(const struct mcp_request *req,
                                   struct mcp_response *res)
{
    (void)req;
    /* Cap large enough for the full 48-slot table plus overflow +
     * totals envelope (worst case ≈ 48 × 80 bytes per entry). */
    char body[8192];
    size_t n = mcp_metrics_consensus_report_json(body, sizeof(body));
    if (n == 0) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "consensus report generation returned empty");
        LOG_ERR("mcp.meta", "consensus_report_json returned 0 bytes");
    }
    res->body = strdup(body);
    if (!res->body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "strdup failed for consensus report");
        LOG_ERR("mcp.meta", "strdup failed for consensus_report (%zu bytes)", n);
    }
    return 0;
}

/* ── Admin dashboard ──────────────────────────────────────────
 *
 * zcl_admin is a composite snapshot tool: it dispatches the existing
 * observability tools (zcl_kpi / zcl_peer_report / zcl_rpc_report /
 * zcl_events) in-process, stitches the raw bodies into one envelope,
 * and derives a small "alerts" array from threshold conditions over
 * the nested counters.
 *
 * "Handles missing subsystems gracefully" means: if any sub-dispatch
 * returns an error envelope (tool missing, handler failed, RPC node
 * offline in test mode), that slot is rendered as JSON null and the
 * other slots still populate.  Tests rely on this — they exercise
 * zcl_admin with no live RPC backend and still get back a parseable
 * document.
 *
 * The `since` parameter is accepted for API stability but currently
 * has no runtime effect: the nested counters are cumulative since
 * boot, not windowed.  A future session can add a baseline-snapshot
 * layer to make `since` meaningful.
 */

/* Write `body` into dst as an embedded JSON value.  If body is NULL,
 * looks like an error envelope ({"error":{...}}), or fails to parse
 * as JSON (some legacy sub-tools splice raw "Unauthorized"-style RPC
 * error strings into their output — that's valid for their own
 * downstream consumers but not for embedding inside a composite
 * envelope), writes "null" instead.  Returns bytes written. */
static size_t embed_or_null(const char *body, char *dst, size_t cap)
{
    if (cap == 0) return 0;
    if (!body || strncmp(body, "{\"error\":", 9) == 0 ||
        strstr(body, "\"error\":{") != NULL) {
        if (cap < 5) return 0;
        memcpy(dst, "null", 4);
        return 4;
    }
    /* Validate as JSON first — reject bare-word "Unauthorized" etc.
     * json_read is forgiving about trailing whitespace and returns
     * false only on a real parse error, so this is a structural
     * guard rather than a strict dialect check. */
    struct json_value probe = {0};
    if (!json_read(&probe, body, strlen(body))) {
        if (cap < 5) return 0;
        memcpy(dst, "null", 4);
        return 4;
    }
    json_free(&probe);

    size_t n = strlen(body);
    if (n >= cap) n = cap - 1;
    memcpy(dst, body, n);
    return n;
}

/* The alerts heuristics in h_zcl_admin pull integers out of the
 * embedded JSON counters via mcp_scan_int_field (controllers.h). */

/* zcl_config_reload — on-demand re-read of env-tunable config
 * without restarting the node.  Applies to the live HTTP RPC
 * middleware and the peer scoring layer; emits a summary of the
 * new active values so an operator can verify the change took
 * effect in a single call.
 *
 * This is the manual alternative to a SIGHUP handler — a future
 * session can add a signal trampoline that calls into the same
 * reload path, but even just the MCP tool is enough to tune a
 * live node without downtime. */
static int h_zcl_config_reload(const struct mcp_request *req,
                                struct mcp_response *res)
{
    (void)req;

    /* Peer scoring: re-reads ZCL_PEER_BAN_THRESHOLD /
     * ZCL_PEER_BAN_HOURS / ZCL_PEER_SCORE_DECAY_PER_MIN. */
    peer_scoring_init();

    /* HTTP RPC middleware: re-read ZCL_RPC_* via the live handle.
     * load_from_env preserves the token-bucket state and only
     * replaces config fields, so in-flight rate-limit decisions
     * don't reset under the caller's feet. */
    bool rpc_active = false;
    int  rpc_global_rps = 0,     rpc_global_burst = 0;
    int  rpc_per_ip_rps = 0,     rpc_per_ip_burst = 0;
    int  rpc_auth_thresh = 0,    rpc_ban_seconds = 0;
    struct rpc_http_middleware *mw = rpc_http_middleware_get_global();
    if (mw) {
        rpc_http_middleware_load_from_env(mw);
        struct rpc_http_stats_snapshot s;
        rpc_http_middleware_stats_snapshot(mw, &s);
        rpc_active        = true;
        rpc_global_rps    = s.global_rps;
        rpc_global_burst  = s.global_burst;
        rpc_per_ip_rps    = s.per_ip_rps;
        rpc_per_ip_burst  = s.per_ip_burst;
        rpc_auth_thresh   = s.auth_fail_threshold;
        rpc_ban_seconds   = s.ban_seconds;
    }

    /* Legacy mirror sync: re-reads ZCL_MIRROR_CADENCE_SECS,
     * MAX_BLOCKS_PER_TICK, LAG_SLA, LAG_SLA_BREACH_BLOCKS/_SECS,
     * LAG_SLA_CRITICAL_BLOCKS/_SECS. The mirror service holds a
     * snapshot in g_lms behind its own mutex; reload swaps the
     * numeric thresholds atomically wrt the next tick. */
    legacy_mirror_sync_reload_from_env();
    struct legacy_mirror_sync_stats mstats = {0};
    legacy_mirror_sync_stats_snapshot(&mstats);

    /* Per-peer bandwidth: re-reads ZCL_PEER_UP_BPS / DOWN_BPS / BURST. */
    struct peer_bandwidth *pb = peer_bandwidth_get_global();
    if (pb) peer_bandwidth_load_from_env(pb);

    /* RPC timeout watchdog: re-reads ZCL_RPC_TIMEOUT_MS / _SWEEP_MS. */
    struct rpc_timeout_mgr *rt = rpc_timeout_get_global();
    if (rt) rpc_timeout_load_from_env(rt);

    /* MCP middleware (this process): re-reads ZCL_MCP_BEARER_TOKEN /
     * GLOBAL_RPS / DESTRUCTIVE_RPS / TIMEOUT_MS. NULL when called from
     * a non-MCP build (e.g. tests linking only the controller lib). */
    struct mcp_middleware *mcp_mw = mcp_middleware_get_global();
    if (mcp_mw) mcp_middleware_load_from_env(mcp_mw);

    size_t cap = 2048;
    char *out = zcl_malloc(cap, "config_reload_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for config reload response");
        LOG_ERR("mcp.meta", "malloc failed for config_reload body (%zu bytes)", cap);
        return -1; // raw-return-ok:logged-oom
    }
    snprintf(out, cap,
        "{\"ok\":true,"
         "\"peer_scoring\":{"
            "\"ban_threshold\":%d,"
            "\"ban_hours\":%d,"
            "\"decay_per_min\":%d"
         "},"
         "\"mirror\":{"
            "\"lag_sla_breach_blocks\":%d,"
            "\"lag_sla_breach_secs\":%d,"
            "\"lag_sla_critical_blocks\":%d,"
            "\"lag_sla_critical_secs\":%d"
         "},"
         "\"peer_bandwidth\":%s,"
         "\"rpc_timeout\":%s,"
         "\"mcp_middleware\":%s,"
         "\"rpc_middleware\":%s",
        peer_scoring_ban_threshold(),
        peer_scoring_ban_hours(),
        peer_scoring_decay_rate(),
        mstats.lag_sla_breach_blocks,
        mstats.lag_sla_breach_secs,
        mstats.lag_sla_critical_blocks,
        mstats.lag_sla_critical_secs,
        pb     ? "\"reloaded\"" : "null",
        rt     ? "\"reloaded\"" : "null",
        mcp_mw ? "\"reloaded\"" : "null",
        rpc_active ? "{" : "null");

    if (rpc_active) {
        size_t pos = strlen(out);
        snprintf(out + pos, cap - pos,
            "\"global_rps\":%d,"
            "\"global_burst\":%d,"
            "\"per_ip_rps\":%d,"
            "\"per_ip_burst\":%d,"
            "\"auth_fail_threshold\":%d,"
            "\"ban_seconds\":%d"
            "}}",
            rpc_global_rps, rpc_global_burst,
            rpc_per_ip_rps, rpc_per_ip_burst,
            rpc_auth_thresh, rpc_ban_seconds);
    } else {
        size_t pos = strlen(out);
        snprintf(out + pos, cap - pos, "}");
    }

    res->body = out;
    return 0;
}

static int h_zcl_admin(const struct mcp_request *req,
                        struct mcp_response *res)
{
    int64_t since = json_get_int_or(req->args, "since", 0);

    /* Dispatch each sub-tool.  Each returns a malloc'd body or an
     * error envelope — we treat both identically via embed_or_null. */
    char *kpi   = mcp_router_dispatch("zcl_kpi",        NULL);
    char *peer  = mcp_router_dispatch("zcl_peer_report", NULL);
    char *rpc   = mcp_router_dispatch("zcl_rpc_report",  NULL);

    /* zcl_events requires its args as a JSON value, not a string. */
    struct json_value ev_args = {0};
    const char *ev_args_src = "{\"count\":10}";
    bool have_ev_args = json_read(&ev_args, ev_args_src, strlen(ev_args_src));
    char *events = mcp_router_dispatch("zcl_events",
                                        have_ev_args ? &ev_args : NULL);
    if (have_ev_args) json_free(&ev_args);

    /* Compose the composite envelope. */
    size_t cap = 131072;
    char *out = zcl_malloc(cap, "admin_body");
    if (!out) {
        free(kpi); free(peer); free(rpc); free(events);
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for admin dashboard response");
        LOG_ERR("mcp.meta", "malloc failed for admin body (%zu bytes)", cap);
        return -1; // raw-return-ok:logged-oom
    }
    size_t pos = 0;

    pos += (size_t)snprintf(out + pos, cap - pos,
        "{\"since\":%lld,\"kpi\":", (long long)since);
    pos += embed_or_null(kpi,   out + pos, cap - pos);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"peer_report\":");
    pos += embed_or_null(peer,  out + pos, cap - pos);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"rpc_report\":");
    pos += embed_or_null(rpc,   out + pos, cap - pos);
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"events\":");
    pos += embed_or_null(events, out + pos, cap - pos);

    /* ── Alerts: simple threshold heuristics over the embedded JSON
     * counters.  Anything flagged as a non-zero problem gets a
     * short human-readable string pushed onto the array.  Keep the
     * surface small — operators can still scrape the nested bodies
     * for full detail. */
    pos += (size_t)snprintf(out + pos, cap - pos, ",\"alerts\":[");
    bool first_alert = true;
    #define PUSH_ALERT(fmt, ...)                                           \
        do {                                                                \
            if (!first_alert) out[pos++] = ',';                             \
            first_alert = false;                                            \
            pos += (size_t)snprintf(out + pos, cap - pos,                   \
                                    "\"" fmt "\"", __VA_ARGS__);            \
        } while (0)

    long long rpc_rl_global = mcp_scan_int_field(rpc, "rate_limited_global");
    long long rpc_rl_per_ip = mcp_scan_int_field(rpc, "rate_limited_per_ip");
    long long rpc_banned    = mcp_scan_int_field(rpc, "banned_rejected");
    long long rpc_active    = mcp_scan_int_field(rpc, "active_bans");
    long long rpc_auth_fail = mcp_scan_int_field(rpc, "auth_failures");
    if (rpc_rl_global > 0)
        PUSH_ALERT("rpc_rate_limited_global=%lld", rpc_rl_global);
    if (rpc_rl_per_ip > 0)
        PUSH_ALERT("rpc_rate_limited_per_ip=%lld", rpc_rl_per_ip);
    if (rpc_banned > 0)
        PUSH_ALERT("rpc_banned_rejected=%lld", rpc_banned);
    if (rpc_active > 0)
        PUSH_ALERT("rpc_active_bans=%lld", rpc_active);
    if (rpc_auth_fail > 0)
        PUSH_ALERT("rpc_auth_failures=%lld", rpc_auth_fail);

    long long peer_bans     = mcp_scan_int_field(peer, "bans_total");
    long long peer_offences = mcp_scan_int_field(peer, "offences_total");
    if (peer_bans > 0)
        PUSH_ALERT("peer_bans_total=%lld", peer_bans);
    if (peer_offences > 100)
        PUSH_ALERT("peer_offences_total=%lld", peer_offences);

    #undef PUSH_ALERT
    pos += (size_t)snprintf(out + pos, cap - pos, "]}");

    free(kpi); free(peer); free(rpc); free(events);

    if (pos < cap) out[pos] = '\0';
    res->body = out;
    return 0;
}

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_logtail[] = {
    { "count",  MCP_PARAM_INT, false, "Number of events to scan",
      1, 10000, 0, 0, NULL, "100" },
    { "domain", MCP_PARAM_STR, false,
      "Event type prefix filter (e.g. \"MCP\", \"VAL.\", \"NET.\")",
      0, 0, 0, 64, NULL, NULL },
};

static const struct mcp_param_spec p_admin[] = {
    /* Accepted for API stability; currently echoed back but the
     * embedded counters are cumulative since boot. */
    { "since", MCP_PARAM_INT, false,
      "Unix-seconds baseline for future windowed counters (unused).",
      0, INT32_MAX, 0, 0, NULL, "0" },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_tools_list", "ops",
      "Dump the full MCP routing table: every tool with its domain, "
      "description, and parameter schema. Self-documenting surface.",
      NULL, 0, h_zcl_tools_list, 0, NULL },
    { "zcl_self_test", "ops",
      "Call every registered tool with safe defaults, reporting "
      "pass/fail/skip. Destructive tools are skipped.",
      NULL, 0, h_zcl_self_test,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE /* avoid recursion */ },
    { "zcl_logtail", "ops",
      "Tail the structured event log. Optional domain prefix filter.",
      p_logtail, PARAM_COUNT(p_logtail), h_zcl_logtail, 0, NULL },
    { "zcl_openapi", "ops",
      "Emit an OpenAPI 3.0-flavored schema document derived from the "
      "MCP routing table. Clients can use it for type generation or "
      "auto-test harnesses.",
      NULL, 0, h_zcl_openapi, 0, NULL },
    { "zcl_metrics", "ops",
      "Prometheus-text metrics dump: request counters, latency histogram, "
      "and summary totals accumulated in-process.",
      NULL, 0, h_zcl_metrics, 0, NULL },
    { "zcl_metrics_reset", "ops",
      "Reset all MCP metric counters. Destructive — gated by the "
      "middleware rate limiter.",
      NULL, 0, h_zcl_metrics_reset, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
    { "zcl_rpc_report", "ops",
      "HTTP RPC middleware report: live rate-limit / ban config plus "
      "allowed/rate-limited/banned/auth-failure counters and current "
      "tracked-IP and active-ban gauges. Parallel to zcl_peer_report "
      "for the RPC surface.",
      NULL, 0, h_zcl_rpc_report, 0, NULL },
    { "zcl_consensus_report", "ops",
      "Consensus-reject snapshot: per-(kind, reason) counts plus "
      "tx/block totals and overflow buckets for the in-process "
      "EV_CONSENSUS_REJECT_TX / EV_CONSENSUS_REJECT_BLOCK stream. "
      "Dashboard companion to AGENT2's zcl_explain_reject.",
      NULL, 0, h_zcl_consensus_report, 0, NULL },
    { "zcl_config_reload", "ops",
      "Re-read env-tunable config for live subsystems (peer_scoring, "
      "rpc_middleware) without restarting the node. Returns the new "
      "effective values so an operator can verify the change took "
      "effect.",
      NULL, 0, h_zcl_config_reload,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE /* re-applies env vars to live subsystems */ },
    { "zcl_admin", "ops",
      "Admin dashboard: aggregates zcl_kpi + zcl_peer_report + "
      "zcl_rpc_report + zcl_events into one snapshot and derives "
      "threshold-based alerts from the nested counters. Missing "
      "subsystems render as null; flagship single-call operator tool.",
      p_admin, PARAM_COUNT(p_admin), h_zcl_admin, 0, NULL },
};

void mcp_register_meta(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
}
