/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Entry points for the MCP domain controllers.  Each function
 * registers all tools in one domain with the router.  Call from
 * mcp_server_main() after mcp_router_reset(). */

#ifndef ZCL_MCP_CONTROLLERS_H
#define ZCL_MCP_CONTROLLERS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"
#include "controllers/native_handler_body.h" /* json_get_*_or + body contract */
#include "router.h"
#include "controllers/rpc_client.h"
#include "util/log_macros.h"

void mcp_register_ops(void);      /* zcl_status, zcl_health, zcl_events, zcl_rpc, ... */
void mcp_register_diagnostics(void); /* zcl_sql, zcl_state, zcl_node_log, zcl_conditions, zcl_profile, ... */
void mcp_register_chain(void);    /* zcl_getblock, zcl_mmb, zcl_syncstate, ...        */
void mcp_register_net(void);      /* zcl_peers, zcl_addnode, zcl_pingpeer, ...        */
void mcp_register_wallet(void);   /* zcl_balance, zcl_send, zcl_getnewaddress, ...    */
void mcp_register_app(void);      /* zcl_name_*, zcl_msg_*, zcl_market_*, zcl_swap_*  */
void mcp_register_meta(void);     /* zcl_tools_list, zcl_self_test, zcl_logtail       */
void mcp_register_dev_hotswap(void); /* DEV-ONLY zcl_agent_hotswap (no-op in release)  */

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)
struct hotswap_load_report;
bool mcp_dev_hotswap_probe_allowed(
    const char *probe,
    const struct hotswap_load_report *rep,
    const struct mcp_tool_route *active_route,
    const char **error_code_out);
bool mcp_dev_hotswap_probe_body_ok(
    const char *body,
    char *error_code_out, size_t error_code_sz,
    char *error_message_out, size_t error_message_sz);
#endif

/* Compile-time element count for a statically-sized array. Used by the
 * route + param-spec tables in every controller, and by the register
 * loops. Replaces the `sizeof(arr) / sizeof(arr[0])` idiom. */
#define PARAM_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

/* ── Defaulted JSON accessors ─────────────────────────────────────
 *
 * json_get_int_or / json_get_bool_or / json_get_str_or now live in
 * controllers/native_handler_body.h (included above) so the re-homed
 * transport-neutral body functions and the MCP controllers share one
 * definition. */

/* ── Pass-through handler macro ────────────────────────────────────
 *
 * Most MCP handlers are just "call the RPC, forward the JSON body,
 * log on failure". DEFINE_PT generates such a handler from a single
 * line. The macro includes LOG_ERR on the null path so the
 * check-silent-errors lint gate stays green.
 *
 * Usage:
 *   DEFINE_PT(h_zcl_foo, "rpc_method", "mcp.bar")
 *
 * Required headers in the .c that uses this macro:
 *   #include "../router.h"        — struct mcp_request/response
 *   #include "controllers/rpc_client.h"    — mcp_node_rpc()
 *   #include "util/log_macros.h"  — LOG_ERR
 *   <stdio.h>                     — snprintf
 *
 * Handlers that need typed args, conditional dispatch, or richer
 * error messages keep their hand-written form — this macro is
 * deliberately limited to the "no params, generic null log" shape.
 */
#define DEFINE_PT(fn_name, rpc_method, log_tag)                                \
    static int fn_name(const struct mcp_request *req,                          \
                       struct mcp_response *res)                               \
    {                                                                          \
        (void)req;                                                             \
        return mcp_return_rpc_body(res, mcp_node_rpc(rpc_method, NULL),        \
                                    rpc_method, log_tag);                      \
    }

/* ── Single-string-arg pass-through macro ──────────────────────────
 *
 * DEFINE_PT_STR generates a handler for the very common shape: read one
 * string field `param_key` from args, build a one-element params array,
 * dispatch `rpc_method`, free the params, and forward the body with a
 * `"<param_key>=<value>"` failure context. The ctx label is derived from
 * the literal param_key so there is one fewer thing to keep in sync.
 *
 * Usage:
 *   DEFINE_PT_STR(h_zcl_gettransaction, "txid", "gettransaction", "mcp.wallet")
 *
 * Required headers in the .c that uses this macro (a superset of
 * DEFINE_PT's): also needs "controllers/rpc_params.h" (mcp_params_*) and
 * <stdlib.h> (free). All current callers already include them.
 *
 * Handlers that need extra args, a typed (non-string) arg, or
 * pre-dispatch validation (e.g. path_check_fs_arg in h_zcl_market_offer,
 * the 2-arg getrawtransaction / getblock) stay hand-written — this macro
 * is deliberately limited to the single-string-arg shape.
 */
#define DEFINE_PT_STR(fn_name, param_key, rpc_method, log_tag)                 \
    static int fn_name(const struct mcp_request *req,                          \
                       struct mcp_response *res)                               \
    {                                                                          \
        const char *v = json_get_str(json_get(req->args, param_key));          \
        struct mcp_params p;                                                   \
        mcp_params_init(&p);                                                   \
        mcp_params_push_str(&p, v);                                            \
        char *params = mcp_params_to_json(&p);                                 \
        char *out = params ? mcp_node_rpc(rpc_method, params) : NULL;          \
        free(params);                                                          \
        return mcp_return_rpc_body_ctx(res, out, rpc_method, log_tag,          \
                                       param_key "=%s", v ? v : "(null)");     \
    }

/* ── Shared handler tail ──────────────────────────────────────────
 *
 * Many handlers do the same thing after calling mcp_node_rpc(): if the
 * returned body is non-NULL hand it to the response; otherwise set
 * MCP_ERR_HANDLER_FAILED, format a "RPC X returned null" error message
 * (visible in the error envelope), and emit a LOG_ERR with the same
 * text so check-silent-errors stays green.
 *
 * This helper captures that ~7-line tail in one call so handlers that
 * need a custom params construction don't have to duplicate the rest.
 * Used by DEFINE_PT and by any handler that hand-constructs params.
 *
 * Required includes (see DEFINE_PT block):
 *   "../router.h", "controllers/rpc_client.h", "util/log_macros.h", <stdio.h>
 */
static inline int mcp_return_rpc_body(struct mcp_response *res,
                                       char *body_or_null,
                                       const char *rpc_method,
                                       const char *log_tag)
{
    if (!body_or_null) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC %s returned null", rpc_method);
        LOG_ERR(log_tag, "RPC %s returned null", rpc_method);
    }
    res->body = body_or_null;
    return 0;
}

/* Same as mcp_return_rpc_body but with a printf-style parameter context
 * appended to the error message and log line. Use when the failure
 * message benefits from caller-visible inputs (e.g., "name=foo",
 * "peer_id=5"). On success ctx_fmt is ignored. */
__attribute__((format(printf, 5, 6)))
static inline int mcp_return_rpc_body_ctx(struct mcp_response *res,
                                          char *body_or_null,
                                          const char *rpc_method,
                                          const char *log_tag,
                                          const char *ctx_fmt, ...)
{
    if (!body_or_null) {
        char ctx[192];
        va_list ap;
        va_start(ap, ctx_fmt);
        vsnprintf(ctx, sizeof(ctx), ctx_fmt, ap);
        va_end(ap);
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "RPC %s failed: %s", rpc_method, ctx);
        LOG_ERR(log_tag, "%s failed: %s", rpc_method, ctx);
    }
    res->body = body_or_null;
    return 0;
}

/* ── OOM error-body helper ─────────────────────────────────────────
 *
 * Collapses the ~5-line "malloc/zcl_malloc failed" tail duplicated
 * ~25 times across the controllers into one call: sets
 * MCP_ERR_INTERNAL, formats res->error_message, LOG_ERRs the same
 * failure (with the attempted byte count when one is known), and
 * returns. The prior call sites had drifted between `return -1;` and
 * `return 0;` on this path — the router treats a nonzero rc
 * identically to a NULL body (router.c ~600), so this helper picks
 * -1 (LOG_ERR's own return value) as the one canonical OOM code.
 *
 * `cap` is the attempted allocation size in bytes; pass 0 when no
 * single byte count applies (e.g. the failure came from an internal
 * json_value_to_body() helper that doesn't expose one) and the log
 * line omits the "(N bytes)" suffix. `what` is the short noun phrase
 * already used at each site's error_message, e.g. "status response"
 * or "postmortem summary list" — reuse it verbatim so
 * res->error_message text is unchanged at every call site.
 *
 * Required includes: same as mcp_return_rpc_body (already present
 * everywhere this header is included). */
static inline int mcp_res_set_oom(struct mcp_response *res, size_t cap,
                                  const char *log_tag, const char *what)
{
    res->error = MCP_ERR_INTERNAL;
    snprintf(res->error_message, sizeof(res->error_message),
             "malloc failed for %s", what);
    if (cap > 0)
        LOG_ERR(log_tag, "malloc failed for %s (%zu bytes)", what, cap);
    LOG_ERR(log_tag, "malloc failed for %s", what);
}

/* ── Lightweight "key":N scanner ──────────────────────────────────
 *
 * Pull an integer out of a JSON-ish body for cheap field reads.
 * Returns -1 if the key isn't present or can't be parsed as an
 * integer. Intentionally not a full parser — we only read counters
 * our own RPCs/handlers emit. The `// raw-return-ok:sentinel` markers
 * keep the silent-error lint gate green (-1 is a documented sentinel,
 * not an unlogged failure). */
static inline long long mcp_scan_int_field(const char *body, const char *key)
{
    if (!body || !key) return -1;  // raw-return-ok:sentinel
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return -1;  // raw-return-ok:sentinel
    const char *p = strstr(body, needle);
    if (!p) return -1;  // raw-return-ok:sentinel
    p += (size_t)n;
    while (*p == ' ') p++;
    if (*p == '\0') return -1;  // raw-return-ok:sentinel
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return -1;  // raw-return-ok:sentinel
    return v;
}

#endif
