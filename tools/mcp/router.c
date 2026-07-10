/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP router implementation.  See router.h for the public API. */

#include "platform/time_compat.h"
#include "router.h"
#include "replay.h"

#include "event/event.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "util/trace.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "util/safe_alloc.h"

#define MCP_ROUTER_MAX_ROUTES 256

/* Slots are atomic so an in-process hot-swap (mcp_router_replace, dev-only)
 * can re-point a route with a release store while dispatch/find/tools-list
 * readers on RPC/HTTP worker threads observe it with an acquire load — no
 * torn pointer, no lock on the hot path. Registration only grows the table
 * at boot (single-threaded), so g_num_routes itself stays a plain size_t. */
static const struct mcp_tool_route *_Atomic g_routes[MCP_ROUTER_MAX_ROUTES];
static size_t g_num_routes = 0;

static inline const struct mcp_tool_route *route_load(size_t i)
{
    return atomic_load_explicit(&g_routes[i], memory_order_acquire);
}

/* ── Helpers ─────────────────────────────────────────────────── */

static uint64_t now_us(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* Append an escaped JSON string (without surrounding quotes) to buf.
 * Returns number of bytes written (0 on overflow). */
static size_t append_escaped(char *buf, size_t buflen, size_t pos,
                              const char *s)
{
    if (!s) return pos;
    while (*s && pos + 2 < buflen) {
        unsigned char c = (unsigned char)*s++;
        if (c == '"') {
            if (pos + 3 > buflen) return pos;
            buf[pos++] = '\\';
            buf[pos++] = '"';
        } else if (c == '\\') {
            if (pos + 3 > buflen) return pos;
            buf[pos++] = '\\';
            buf[pos++] = '\\';
        } else if (c == '\n') {
            if (pos + 3 > buflen) return pos;
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == '\r') {
            if (pos + 3 > buflen) return pos;
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (c == '\t') {
            if (pos + 3 > buflen) return pos;
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if (c < 0x20) {
            if (pos + 7 > buflen) return pos;
            pos += (size_t)snprintf(buf + pos, buflen - pos, "\\u%04x", c);
        } else {
            buf[pos++] = (char)c;
        }
    }
    return pos;
}

/* True if csv contains s as a comma-separated token. */
static bool csv_contains(const char *csv, const char *s)
{
    if (!csv || !s) return false;
    size_t slen = strlen(s);
    const char *p = csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        if (tlen == slen && memcmp(p, s, slen) == 0)
            return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

/* ── Registry ────────────────────────────────────────────────── */

void mcp_router_reset(void)
{
    for (size_t i = 0; i < MCP_ROUTER_MAX_ROUTES; i++)
        atomic_store_explicit(&g_routes[i], NULL, memory_order_release);
    g_num_routes = 0;
}

bool mcp_router_register(const struct mcp_tool_route *route)
{
    if (!route || !route->name || !route->handler)
        return false;
    if (g_num_routes >= MCP_ROUTER_MAX_ROUTES)
        return false;
    /* Reject duplicate names */
    for (size_t i = 0; i < g_num_routes; i++) {
        if (strcmp(route_load(i)->name, route->name) == 0)
            return false;
    }
    atomic_store_explicit(&g_routes[g_num_routes], route,
                          memory_order_release);
    g_num_routes++;
    return true;
}

void mcp_router_register_required(const struct mcp_tool_route *route)
{
    const char *name = (route && route->name) ? route->name : "(null)";
    const char *domain = (route && route->domain) ? route->domain : "(null)";

    if (!route || !route->name || !route->handler) {
        fprintf(stderr,
                "[mcp_router] FATAL: malformed required route "
                "name=%s domain=%s count=%zu capacity=%zu\n",
                name, domain, g_num_routes, (size_t)MCP_ROUTER_MAX_ROUTES);
        abort();
    }

    const struct mcp_tool_route *existing = mcp_router_find(route->name);
    if (existing) {
        if (existing == route)
            return;
        fprintf(stderr,
                "[mcp_router] FATAL: duplicate required route name=%s "
                "existing_domain=%s new_domain=%s count=%zu capacity=%zu\n",
                route->name,
                existing->domain ? existing->domain : "(null)",
                domain, g_num_routes, (size_t)MCP_ROUTER_MAX_ROUTES);
        abort();
    }

    if (!mcp_router_register(route)) {
        fprintf(stderr,
                "[mcp_router] FATAL: required route registration failed "
                "name=%s domain=%s count=%zu capacity=%zu\n",
                name, domain, g_num_routes, (size_t)MCP_ROUTER_MAX_ROUTES);
        abort();
    }
}

const struct mcp_tool_route *mcp_router_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < g_num_routes; i++) {
        const struct mcp_tool_route *r = route_load(i);
        if (r && strcmp(r->name, name) == 0)
            return r;
    }
    return NULL;
}

size_t mcp_router_count(void)
{
    return g_num_routes;
}

size_t mcp_router_capacity(void)
{
    return MCP_ROUTER_MAX_ROUTES;
}

const struct mcp_tool_route *mcp_router_at(size_t idx)
{
    if (idx >= g_num_routes) return NULL;
    return route_load(idx);
}

/* ── In-process route swap (dev-only caller; harmless + tested always) ──
 *
 * Re-point the slot named `name` at `new_route` with a release store. The
 * new route passes the SAME structural checks as registration (name +
 * handler present). Readers on other threads see either the old or the new
 * route pointer, never a torn one. Returns false (with logged context) if
 * the name is unknown or the new route is malformed; the table is unchanged
 * on any failure. Nothing calls this outside the dev hot-swap loader + tests. */
bool mcp_router_replace(const char *name, const struct mcp_tool_route *new_route)
{
    if (!name || !name[0])
        LOG_FAIL("mcp.router", "mcp_router_replace: null/empty name");
    if (!new_route || !new_route->name || !new_route->handler)
        LOG_FAIL("mcp.router",
                 "mcp_router_replace: malformed route for '%s'", name);
    if (strcmp(new_route->name, name) != 0)
        LOG_FAIL("mcp.router",
                 "mcp_router_replace: route name '%s' != slot '%s'",
                 new_route->name, name);
    for (size_t i = 0; i < g_num_routes; i++) {
        const struct mcp_tool_route *r = route_load(i);
        if (r && strcmp(r->name, name) == 0) {
            atomic_store_explicit(&g_routes[i], new_route,
                                  memory_order_release);
            return true;
        }
    }
    LOG_FAIL("mcp.router",
             "mcp_router_replace: no route named '%s'", name);
}

/* ── Type / code naming ──────────────────────────────────────── */

const char *mcp_param_type_name(enum mcp_param_type t)
{
    switch (t) {
    case MCP_PARAM_STR:    return "string";
    case MCP_PARAM_INT:    return "integer";
    case MCP_PARAM_REAL:   return "number";
    case MCP_PARAM_BOOL:   return "boolean";
    case MCP_PARAM_ARRAY:  return "array";
    case MCP_PARAM_OBJECT: return "object";
    }
    return "unknown";
}

const char *mcp_error_code_name(enum mcp_error_code c)
{
    switch (c) {
    case MCP_OK:                   return "OK";
    case MCP_ERR_UNKNOWN_TOOL:     return "UNKNOWN_TOOL";
    case MCP_ERR_MISSING_PARAM:    return "MISSING_PARAM";
    case MCP_ERR_INVALID_TYPE:     return "INVALID_TYPE";
    case MCP_ERR_OUT_OF_RANGE:     return "OUT_OF_RANGE";
    case MCP_ERR_STRING_TOO_SHORT: return "STRING_TOO_SHORT";
    case MCP_ERR_STRING_TOO_LONG:  return "STRING_TOO_LONG";
    case MCP_ERR_ENUM_MISMATCH:    return "ENUM_MISMATCH";
    case MCP_ERR_HANDLER_FAILED:   return "HANDLER_FAILED";
    case MCP_ERR_INTERNAL:         return "INTERNAL";
    case MCP_ERR_AUTH_REQUIRED:    return "AUTH_REQUIRED";
    case MCP_ERR_RATE_LIMITED:     return "RATE_LIMITED";
    case MCP_ERR_TOOL_TIMEOUT:     return "TOOL_TIMEOUT";
    }
    return "UNKNOWN";
}

/* ── Validation ──────────────────────────────────────────────── */

enum mcp_error_code mcp_router_validate(const struct mcp_tool_route *route,
                                         const struct json_value *args,
                                         char *err_param, size_t err_param_sz,
                                         char *err_msg, size_t err_msg_sz)
{
    if (err_param && err_param_sz > 0) err_param[0] = 0;
    if (err_msg && err_msg_sz > 0) err_msg[0] = 0;
    if (!route) return MCP_ERR_INTERNAL;

    for (size_t i = 0; i < route->num_params; i++) {
        const struct mcp_param_spec *p = &route->params[i];
        const struct json_value *v = args ? json_get(args, p->name) : NULL;
        if (!v || v->type == JSON_NULL) {
            if (p->required) {
                if (err_param) snprintf(err_param, err_param_sz, "%s", p->name);
                if (err_msg)
                    snprintf(err_msg, err_msg_sz,
                             "missing required parameter '%s'", p->name);
                return MCP_ERR_MISSING_PARAM;
            }
            continue;
        }

        bool type_ok = false;
        switch (p->type) {
        case MCP_PARAM_STR:    type_ok = (v->type == JSON_STR); break;
        case MCP_PARAM_INT:    type_ok = (v->type == JSON_INT); break;
        case MCP_PARAM_REAL:
            type_ok = (v->type == JSON_REAL || v->type == JSON_INT);
            break;
        case MCP_PARAM_BOOL:   type_ok = (v->type == JSON_BOOL); break;
        case MCP_PARAM_ARRAY:  type_ok = (v->type == JSON_ARR); break;
        case MCP_PARAM_OBJECT: type_ok = (v->type == JSON_OBJ); break;
        }
        if (!type_ok) {
            if (err_param) snprintf(err_param, err_param_sz, "%s", p->name);
            if (err_msg)
                snprintf(err_msg, err_msg_sz,
                         "parameter '%s' has wrong type, expected %s",
                         p->name, mcp_param_type_name(p->type));
            return MCP_ERR_INVALID_TYPE;
        }

        if (p->type == MCP_PARAM_INT && p->max_int > p->min_int) {
            int64_t iv = json_get_int(v);
            if (iv < p->min_int || iv > p->max_int) {
                if (err_param) snprintf(err_param, err_param_sz, "%s", p->name);
                if (err_msg)
                    snprintf(err_msg, err_msg_sz,
                             "parameter '%s' = %lld out of range [%lld, %lld]",
                             p->name, (long long)iv,
                             (long long)p->min_int, (long long)p->max_int);
                return MCP_ERR_OUT_OF_RANGE;
            }
        }

        if (p->type == MCP_PARAM_STR) {
            const char *s = json_get_str(v);
            size_t len = s ? strlen(s) : 0;
            if (p->min_len > 0 && len < p->min_len) {
                if (err_param) snprintf(err_param, err_param_sz, "%s", p->name);
                if (err_msg)
                    snprintf(err_msg, err_msg_sz,
                             "parameter '%s' length %zu < min %zu",
                             p->name, len, p->min_len);
                return MCP_ERR_STRING_TOO_SHORT;
            }
            if (p->max_len > 0 && len > p->max_len) {
                if (err_param) snprintf(err_param, err_param_sz, "%s", p->name);
                if (err_msg)
                    snprintf(err_msg, err_msg_sz,
                             "parameter '%s' length %zu > max %zu",
                             p->name, len, p->max_len);
                return MCP_ERR_STRING_TOO_LONG;
            }
            if (p->enum_csv && s && !csv_contains(p->enum_csv, s)) {
                if (err_param) snprintf(err_param, err_param_sz, "%s", p->name);
                if (err_msg)
                    snprintf(err_msg, err_msg_sz,
                             "parameter '%s' value '%s' not in enum [%s]",
                             p->name, s, p->enum_csv);
                return MCP_ERR_ENUM_MISMATCH;
            }
        }
    }
    return MCP_OK;
}

/* ── Schema / envelope JSON ──────────────────────────────────── */

/* Append a JSON array of enum values split from a csv string. */
static size_t append_enum_array(char *buf, size_t buflen, size_t pos,
                                 const char *csv)
{
    if (pos + 2 > buflen) return pos;
    buf[pos++] = '[';
    const char *p = csv;
    bool first = true;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        if (!first) {
            if (pos + 1 > buflen) return pos;
            buf[pos++] = ',';
        }
        if (pos + 1 > buflen) return pos;
        buf[pos++] = '"';
        for (size_t i = 0; i < tlen && pos + 2 < buflen; i++)
            buf[pos++] = p[i];
        if (pos + 1 > buflen) return pos;
        buf[pos++] = '"';
        first = false;
        if (!comma) break;
        p = comma + 1;
    }
    if (pos + 1 > buflen) return pos;
    buf[pos++] = ']';
    return pos;
}

size_t mcp_router_input_schema_json(const struct mcp_tool_route *route,
                                     char *buf, size_t buflen)
{
    if (!route || !buf || buflen == 0) return 0;
    size_t pos = 0;
    int n;

    n = snprintf(buf + pos, buflen - pos, "{\"type\":\"object\",\"properties\":{");
    if (n < 0 || (size_t)n >= buflen - pos) return pos;
    pos += (size_t)n;

    for (size_t i = 0; i < route->num_params; i++) {
        const struct mcp_param_spec *p = &route->params[i];
        if (i > 0) {
            if (pos + 1 >= buflen) return pos;
            buf[pos++] = ',';
        }
        n = snprintf(buf + pos, buflen - pos,
                     "\"%s\":{\"type\":\"%s\"",
                     p->name, mcp_param_type_name(p->type));
        if (n < 0 || (size_t)n >= buflen - pos) return pos;
        pos += (size_t)n;

        if (p->description) {
            if (pos + 18 >= buflen) return pos;
            memcpy(buf + pos, ",\"description\":\"", 16);
            pos += 16;
            pos = append_escaped(buf, buflen, pos, p->description);
            if (pos + 2 >= buflen) return pos;
            buf[pos++] = '"';
        }
        if (p->type == MCP_PARAM_INT && p->max_int > p->min_int) {
            n = snprintf(buf + pos, buflen - pos,
                         ",\"minimum\":%lld,\"maximum\":%lld",
                         (long long)p->min_int, (long long)p->max_int);
            if (n < 0 || (size_t)n >= buflen - pos) return pos;
            pos += (size_t)n;
        }
        if (p->type == MCP_PARAM_STR && p->min_len > 0) {
            n = snprintf(buf + pos, buflen - pos, ",\"minLength\":%zu", p->min_len);
            if (n < 0 || (size_t)n >= buflen - pos) return pos;
            pos += (size_t)n;
        }
        if (p->type == MCP_PARAM_STR && p->max_len > 0) {
            n = snprintf(buf + pos, buflen - pos, ",\"maxLength\":%zu", p->max_len);
            if (n < 0 || (size_t)n >= buflen - pos) return pos;
            pos += (size_t)n;
        }
        if (p->type == MCP_PARAM_STR && p->enum_csv) {
            n = snprintf(buf + pos, buflen - pos, ",\"enum\":");
            if (n < 0 || (size_t)n >= buflen - pos) return pos;
            pos += (size_t)n;
            pos = append_enum_array(buf, buflen, pos, p->enum_csv);
        }
        if (p->default_json) {
            n = snprintf(buf + pos, buflen - pos, ",\"default\":%s",
                         p->default_json);
            if (n < 0 || (size_t)n >= buflen - pos) return pos;
            pos += (size_t)n;
        }
        if (pos + 1 >= buflen) return pos;
        buf[pos++] = '}';
    }

    if (pos + 1 >= buflen) return pos;
    buf[pos++] = '}'; /* close properties */

    /* required array */
    size_t req_count = 0;
    for (size_t i = 0; i < route->num_params; i++)
        if (route->params[i].required) req_count++;
    if (req_count > 0) {
        n = snprintf(buf + pos, buflen - pos, ",\"required\":[");
        if (n < 0 || (size_t)n >= buflen - pos) return pos;
        pos += (size_t)n;
        bool first = true;
        for (size_t i = 0; i < route->num_params; i++) {
            if (!route->params[i].required) continue;
            n = snprintf(buf + pos, buflen - pos, "%s\"%s\"",
                         first ? "" : ",", route->params[i].name);
            if (n < 0 || (size_t)n >= buflen - pos) return pos;
            pos += (size_t)n;
            first = false;
        }
        if (pos + 1 >= buflen) return pos;
        buf[pos++] = ']';
    }

    if (pos + 1 >= buflen) return pos;
    buf[pos++] = '}';
    if (pos < buflen) buf[pos] = 0;
    return pos;
}

size_t mcp_router_tools_list_json(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return 0;
    size_t pos = 0;
    if (pos + 1 >= buflen) return pos;
    buf[pos++] = '[';
    for (size_t i = 0; i < g_num_routes; i++) {
        const struct mcp_tool_route *r = route_load(i);
        if (i > 0) {
            if (pos + 1 >= buflen) return pos;
            buf[pos++] = ',';
        }
        int n = snprintf(buf + pos, buflen - pos, "{\"name\":\"%s\"", r->name);
        if (n < 0 || (size_t)n >= buflen - pos) return pos;
        pos += (size_t)n;

        if (r->domain) {
            if (pos + 12 >= buflen) return pos;
            memcpy(buf + pos, ",\"domain\":\"", 11);
            pos += 11;
            pos = append_escaped(buf, buflen, pos, r->domain);
            if (pos + 1 >= buflen) return pos;
            buf[pos++] = '"';
        }
        if (r->description) {
            if (pos + 18 >= buflen) return pos;
            memcpy(buf + pos, ",\"description\":\"", 16);
            pos += 16;
            pos = append_escaped(buf, buflen, pos, r->description);
            if (pos + 1 >= buflen) return pos;
            buf[pos++] = '"';
        }

        if (pos + 15 >= buflen) return pos;
        memcpy(buf + pos, ",\"inputSchema\":", 15);
        pos += 15;
        pos += mcp_router_input_schema_json(r, buf + pos, buflen - pos);

        if (pos + 1 >= buflen) return pos;
        buf[pos++] = '}';
    }
    if (pos + 1 >= buflen) return pos;
    buf[pos++] = ']';
    if (pos < buflen) buf[pos] = 0;
    return pos;
}

size_t mcp_router_error_envelope(char *buf, size_t buflen,
                                  enum mcp_error_code code,
                                  const char *tool,
                                  const char *param,
                                  const char *message)
{
    if (!buf || buflen == 0) return 0;
    size_t pos = 0;
    int n = snprintf(buf + pos, buflen - pos,
                     "{\"error\":{\"code\":\"%s\",\"message\":\"",
                     mcp_error_code_name(code));
    if (n < 0 || (size_t)n >= buflen - pos) { buf[0] = 0; return 0; }
    pos += (size_t)n;
    pos = append_escaped(buf, buflen, pos, message ? message : "");
    if (pos + 10 >= buflen) { buf[0] = 0; return 0; }
    memcpy(buf + pos, "\",\"tool\":\"", 10);
    pos += 10;
    pos = append_escaped(buf, buflen, pos, tool ? tool : "");
    if (pos + 1 >= buflen) { buf[0] = 0; return 0; }
    buf[pos++] = '"';
    if (param && param[0]) {
        if (pos + 11 >= buflen) { buf[0] = 0; return 0; }
        memcpy(buf + pos, ",\"param\":\"", 10);
        pos += 10;
        pos = append_escaped(buf, buflen, pos, param);
        if (pos + 1 >= buflen) { buf[0] = 0; return 0; }
        buf[pos++] = '"';
    }
    if (pos + 2 >= buflen) { buf[0] = 0; return 0; }
    buf[pos++] = '}';
    buf[pos++] = '}';
    if (pos < buflen) buf[pos] = 0;
    return pos;
}

/* ── Dispatch ────────────────────────────────────────────────── */

char *mcp_router_error_envelope_strdup(enum mcp_error_code code,
                                       const char *tool,
                                       const char *param, const char *msg)
{
    char tmp[2048];
    size_t n = mcp_router_error_envelope(tmp, sizeof(tmp), code, tool,
                                          param, msg);
    if (n == 0) {
        const char *fallback =
            "{\"error\":{\"code\":\"INTERNAL\",\"message\":\"envelope build failed\"}}";
        return strdup(fallback);
    }
    char *out = zcl_malloc(n + 1, "mcp error envelope");
    if (!out) return NULL;
    memcpy(out, tmp, n);
    out[n] = 0;
    return out;
}

/* Skip recording replay-related tools to avoid recursion. */
static bool is_replay_tool(const char *name)
{
    return name && (strncmp(name, "zcl_replay_", 11) == 0);
}

static void replay_record_if_enabled(const char *tool,
                                       const struct json_value *args,
                                       const char *response,
                                       uint64_t dur_us, bool is_error)
{
    if (is_replay_tool(tool)) return;
    char args_buf[1024];
    if (args)
        json_write(args, args_buf, sizeof(args_buf));
    else
        args_buf[0] = '\0';
    mcp_replay_record(tool, args_buf, response, dur_us, is_error);
}

char *mcp_router_dispatch(const char *tool_name,
                          const struct json_value *args)
{
    uint64_t t0 = now_us();
    struct trace_span *span = trace_start("mcp.dispatch");
    trace_attr_str(span, "tool", tool_name ? tool_name : "(null)");

    const struct mcp_tool_route *route = mcp_router_find(tool_name);
    if (!route) {
        char msg[200];
        snprintf(msg, sizeof(msg), "unknown tool: %s",
                 tool_name ? tool_name : "(null)");
        char *out = mcp_router_error_envelope_strdup(MCP_ERR_UNKNOWN_TOOL,
                                     tool_name ? tool_name : "", NULL, msg);
        uint64_t dur = now_us() - t0;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=UNKNOWN_TOOL dur_us=%lld",
                    tool_name ? tool_name : "-",
                    (long long)dur);
        replay_record_if_enabled(tool_name, args, out, dur, true);
        trace_set_status(span, TRACE_STATUS_ERROR);
        trace_attr_str(span, "error", "unknown_tool");
        trace_end(span);
        return out ? out : strdup("{\"error\":{\"code\":\"INTERNAL\"}}");
    }

    char err_param[64] = {0};
    char err_msg[256] = {0};
    enum mcp_error_code vcode =
        mcp_router_validate(route, args, err_param, sizeof(err_param),
                            err_msg, sizeof(err_msg));
    if (vcode != MCP_OK) {
        char *out = mcp_router_error_envelope_strdup(vcode, tool_name,
                                     err_param[0] ? err_param : NULL, err_msg);
        uint64_t dur = now_us() - t0;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=%s param=%s dur_us=%lld",
                    tool_name, mcp_error_code_name(vcode),
                    err_param[0] ? err_param : "-",
                    (long long)dur);
        replay_record_if_enabled(tool_name, args, out, dur, true);
        trace_set_status(span, TRACE_STATUS_ERROR);
        trace_attr_str(span, "error", mcp_error_code_name(vcode));
        trace_end(span);
        return out ? out : strdup("{\"error\":{\"code\":\"INTERNAL\"}}");
    }

    struct mcp_request req = { .tool = tool_name, .args = args };
    struct mcp_response res = { 0 };
    int rc = route->handler(&req, &res);
    uint64_t dur = now_us() - t0;

    if (rc != 0 || res.body == NULL) {
        enum mcp_error_code ec = res.error ? res.error : MCP_ERR_HANDLER_FAILED;
        const char *msg = res.error_message[0] ? res.error_message
                                               : "handler failed";
        const char *prm = res.error_param[0] ? res.error_param : NULL;
        char *out = mcp_router_error_envelope_strdup(ec, tool_name, prm, msg);
        free(res.body);
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=%s dur_us=%lld",
                    tool_name, mcp_error_code_name(ec), (long long)dur);
        replay_record_if_enabled(tool_name, args, out, dur, true);
        trace_set_status(span, TRACE_STATUS_ERROR);
        trace_attr_str(span, "error", mcp_error_code_name(ec));
        trace_end(span);
        return out ? out : strdup("{\"error\":{\"code\":\"INTERNAL\"}}");
    }

    event_emitf(EV_MCP_REQUEST, 0,
                "tool=%s code=OK dur_us=%lld",
                tool_name, (long long)dur);
    replay_record_if_enabled(tool_name, args, res.body, dur, false);
    trace_attr_int(span, "dur_us", (int64_t)dur);
    trace_end(span);
    return res.body;
}
