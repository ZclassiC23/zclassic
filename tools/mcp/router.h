/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP router — schema-driven dispatch for the MCP tool surface.
 *
 * Each tool is registered as a `mcp_tool_route` with a parameter spec
 * (name, type, range, length, enum, required flag).  The router does
 * parameter validation before the handler runs, so handlers see only
 * well-typed, well-ranged inputs.  On any validation failure the router
 * returns a consistent error envelope instead of invoking the handler.
 *
 * The router is independent of stdio / the MCP protocol wire format —
 * mcp_server.c wraps it; tests can exercise it directly.
 */

#ifndef ZCL_MCP_ROUTER_H
#define ZCL_MCP_ROUTER_H

#include "json/json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mcp_param_type {
    MCP_PARAM_STR = 0,
    MCP_PARAM_INT,
    MCP_PARAM_REAL,
    MCP_PARAM_BOOL,
    MCP_PARAM_ARRAY,
    MCP_PARAM_OBJECT,
};

enum mcp_error_code {
    MCP_OK = 0,
    MCP_ERR_UNKNOWN_TOOL,
    MCP_ERR_MISSING_PARAM,
    MCP_ERR_INVALID_TYPE,
    MCP_ERR_OUT_OF_RANGE,
    MCP_ERR_STRING_TOO_SHORT,
    MCP_ERR_STRING_TOO_LONG,
    MCP_ERR_ENUM_MISMATCH,
    MCP_ERR_HANDLER_FAILED,
    MCP_ERR_INTERNAL,
    /* Middleware-specific codes */
    MCP_ERR_AUTH_REQUIRED,
    MCP_ERR_RATE_LIMITED,
    MCP_ERR_TOOL_TIMEOUT,
};

struct mcp_param_spec {
    const char *name;
    enum mcp_param_type type;
    bool required;
    const char *description;
    /* Integer range — enforced only if max_int > min_int. */
    int64_t min_int;
    int64_t max_int;
    /* String length — enforced only if max_len > 0. */
    size_t min_len;
    size_t max_len;
    /* Comma-separated enum values for MCP_PARAM_STR, NULL if none.
     * e.g. "add,remove,onetry" */
    const char *enum_csv;
    /* Default value as a JSON fragment for schema output, NULL if none.
     * e.g. "1", "20", "\"zcl\"", "false" */
    const char *default_json;
};

struct mcp_request {
    const char *tool;
    const struct json_value *args;   /* pre-validated */
};

struct mcp_response {
    /* Handler sets body on success (malloc'd, ownership transfers to caller). */
    char *body;
    /* On failure, handler may set these. */
    enum mcp_error_code error;
    char error_message[256];
    char error_param[64];
};

/* Returns 0 on success (body set), non-zero on failure. */
typedef int (*mcp_handler_fn)(const struct mcp_request *req,
                               struct mcp_response *res);

struct mcp_tool_route {
    const char *name;              /* e.g. "zcl_getblock" */
    const char *domain;            /* "chain" | "wallet" | "net" | "ops" | "app" */
    const char *description;
    const struct mcp_param_spec *params;
    size_t                       num_params;
    mcp_handler_fn               handler;
    /* Optional metadata — set inline in the route initializer.
     * flags: OR of mcp_tool_flag bits. Default 0.
     * self_test_args: NUL-terminated JSON literal that lives for the
     *   process lifetime; used by zcl_self_test to call this tool
     *   with non-default arguments. Default NULL. */
    uint32_t      flags;
    const char   *self_test_args;
};

/* ── Registry ────────────────────────────────────────────────── */

/* Reset the routing table (tests). */
void mcp_router_reset(void);

/* Register a route.  The pointer must remain valid for the lifetime of
 * the process (or until mcp_router_reset).  Returns false if the table
 * is full or a route with the same name already exists. */
bool mcp_router_register(const struct mcp_tool_route *route);

/* Register a route that must exist in production. Duplicate registration of
 * the same static route is idempotent; any malformed route, duplicate name
 * with a different route, or capacity exhaustion aborts loudly. Controller
 * registration loops use this so a future operator/MCP tool cannot silently
 * disappear. */
void mcp_router_register_required(const struct mcp_tool_route *route);

const struct mcp_tool_route *mcp_router_find(const char *name);
size_t mcp_router_count(void);
size_t mcp_router_capacity(void);
const struct mcp_tool_route *mcp_router_at(size_t idx);

/* ── Tool metadata (set inline in the route initializer) ─────── */

/* Flag bits attached to a route. Set inline in the route struct's
 * `flags` field. */
enum mcp_tool_flag {
    /* Tool writes state on the node, network, or wallet — skipped by
     * zcl_self_test, treated as destructive by middleware (rate-gated). */
    MCP_TOOL_FLAG_DESTRUCTIVE = 1u << 0,
    /* String enums describe values known to this proxy build, but the
     * target node is authoritative and may support newer values.  Keep
     * the values in schema output as x-advisoryEnum, and do not reject
     * other well-typed values during proxy-side validation. */
    MCP_TOOL_FLAG_ADVISORY_ENUMS = 1u << 1,
};

/* ── Validation / dispatch ───────────────────────────────────── */

/* Validate args against the route's param schema.
 * On failure, err_param and err_msg are filled. */
enum mcp_error_code mcp_router_validate(const struct mcp_tool_route *route,
                                         const struct json_value *args,
                                         char *err_param, size_t err_param_sz,
                                         char *err_msg, size_t err_msg_sz);

/* Look up the tool, validate the args, call the handler, and emit a
 * structured log event.  Returns a malloc'd JSON string — either the
 * handler body on success, or an error envelope on any failure.  Never
 * returns NULL.  Caller frees. */
char *mcp_router_dispatch(const char *tool_name,
                          const struct json_value *args);

/* ── JSON output ─────────────────────────────────────────────── */

/* Write the inputSchema object for one route into buf. */
size_t mcp_router_input_schema_json(const struct mcp_tool_route *route,
                                     char *buf, size_t buflen);

/* Write the JSON array of all registered tools (as the value of
 * `tools/list`.result.tools).  Includes name, description, inputSchema,
 * and domain. */
size_t mcp_router_tools_list_json(char *buf, size_t buflen);

/* Write the standard error envelope JSON.
 *   {"error":{"code":"<code>","message":"<msg>","tool":"<name>","param":"<p>"}}
 * Message / tool / param are escaped for JSON.  param may be NULL. */
size_t mcp_router_error_envelope(char *buf, size_t buflen,
                                  enum mcp_error_code code,
                                  const char *tool,
                                  const char *param,
                                  const char *message);

/* Build the error envelope into a freshly malloc'd, NUL-terminated string
 * (caller frees). Wraps mcp_router_error_envelope with a fixed 2 KB scratch
 * buffer; on overflow/failure returns a strdup'd INTERNAL fallback. Returns
 * NULL only if the final allocation fails. */
char *mcp_router_error_envelope_strdup(enum mcp_error_code code,
                                       const char *tool,
                                       const char *param,
                                       const char *msg);

const char *mcp_error_code_name(enum mcp_error_code c);
const char *mcp_param_type_name(enum mcp_param_type t);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_MCP_ROUTER_H */
