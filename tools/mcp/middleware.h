/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP Middleware — security, rate limiting, and per-tool timeouts.
 *
 * Wraps `mcp_router_dispatch()` so every inbound call passes through
 * a consistent policy chain:
 *
 *   1. Bearer-token auth (if configured)
 *   2. Global rate limit (token bucket, requests/sec)
 *   3. Destructive rate limit (stricter bucket for write tools)
 *   4. Per-tool timeout (pthread_cond_timedwait)
 *   5. Router dispatch (validation + handler)
 *
 * New error codes surfaced through the same envelope shape:
 *   AUTH_REQUIRED   — missing or wrong bearer token
 *   RATE_LIMITED    — bucket empty
 *   TOOL_TIMEOUT    — handler did not complete before deadline
 *
 * Configuration is primarily from environment variables.  Call
 * `mcp_middleware_load_from_env()` once at boot.  Tests can set fields
 * directly on the struct.
 */

#ifndef ZCL_MCP_MIDDLEWARE_H
#define ZCL_MCP_MIDDLEWARE_H

#include "mcp/router.h"
#include "json/json.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_MIDDLEWARE_TOKEN_MAX  128
#define MCP_MIDDLEWARE_DESTRUCT_MAX 32

struct mcp_middleware {
    /* Auth — empty token disables auth check for that tier.
     *
     * Two-tier model (backward-compatible):
     *   - required_bearer_token governs NON-destructive tools, and — when
     *     required_destructive_bearer_token is EMPTY — governs EVERY tool
     *     (today's behavior, preserved exactly when the destructive env
     *     var is unset).
     *   - required_destructive_bearer_token, when NON-empty, escalates the
     *     auth requirement for tools flagged MCP_TOOL_FLAG_DESTRUCTIVE: a
     *     destructive tool then REQUIRES this token and REJECTS the normal
     *     one (least-privilege: an ops/destructive credential cannot be
     *     reused to read introspection or normal RPC, and vice-versa).
     *     For the tier separation to be meaningful the two tokens MUST
     *     differ; if the operator configures them identically the tiers
     *     collapse to a single shared credential. */
    char required_bearer_token[MCP_MIDDLEWARE_TOKEN_MAX];
    char required_destructive_bearer_token[MCP_MIDDLEWARE_TOKEN_MAX];

    /* Rate limiting — tokens-per-second for each bucket. */
    int64_t global_rps;       /* default 100 */
    int64_t destructive_rps;  /* default 1   */
    int64_t burst_global;     /* max tokens in global bucket (default = global_rps * 2) */
    int64_t burst_destructive;

    /* Per-tool timeout — default wallclock budget for a handler. */
    int64_t default_timeout_ms;  /* default 5000 */

    /* Destructive tool allow-list: tools that drain the destructive bucket
     * AND are considered write operations for auth/audit purposes. */
    const char *destructive_tools[MCP_MIDDLEWARE_DESTRUCT_MAX];
    size_t      num_destructive_tools;

    /* ── Runtime state (internal) ── */
    double   global_bucket;       /* current tokens, fractional allowed */
    double   destructive_bucket;
    int64_t  last_global_refill_us;
    int64_t  last_destructive_refill_us;
    pthread_mutex_t bucket_lock;

    /* Counters for metrics / tests. */
    int64_t stat_allowed;
    int64_t stat_auth_denied;
    int64_t stat_rate_limited_global;
    int64_t stat_rate_limited_destructive;
    int64_t stat_timeout;

    bool initialized;
};

/* Initialize the middleware to safe defaults (no auth, 100 rps global,
 * 1 rps destructive, 5s timeout, canonical destructive list). */
void mcp_middleware_init(struct mcp_middleware *mw);

/* Tear down runtime state (mutex).  Safe to call on uninitialized. */
void mcp_middleware_destroy(struct mcp_middleware *mw);

/* Populate config fields from environment variables:
 *   ZCL_MCP_BEARER_TOKEN             → required_bearer_token
 *   ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN → required_destructive_bearer_token
 *   ZCL_MCP_GLOBAL_RPS               → global_rps      (default 100)
 *   ZCL_MCP_DESTRUCTIVE_RPS          → destructive_rps (default 1)
 *   ZCL_MCP_TIMEOUT_MS               → default_timeout_ms (default 5000)
 *
 * When ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN is UNSET, behavior is identical to
 * today: ZCL_MCP_BEARER_TOKEN governs every tool (destructive included).
 * When SET, destructive tools require the destructive token and reject the
 * normal token; non-destructive tools require the normal token and reject
 * the destructive token.  Unknown / unset vars keep their current values. */
void mcp_middleware_load_from_env(struct mcp_middleware *mw);

/* Process-wide middleware singleton.  init_global() is idempotent and
 * intended to be called from mcp_server_main() once at startup; get_global()
 * returns NULL until init_global() runs (e.g. in tests that link only
 * the controller lib). */
void mcp_middleware_init_global(void);
struct mcp_middleware *mcp_middleware_get_global(void);

/* True if the tool name is in the destructive list. */
bool mcp_middleware_is_destructive(const struct mcp_middleware *mw,
                                    const char *tool_name);

/* Resolve the wallclock timeout budget for a tool: a longer per-tool budget
 * for the few handlers that legitimately run past the global default
 * (zcl_profile, zcl_waitfor*), otherwise mw->default_timeout_ms. `tool_name`
 * may be NULL → the global default. Pure; used by the dispatch path and
 * directly testable. */
int64_t mcp_middleware_resolve_timeout_ms(const struct mcp_middleware *mw,
                                          const char *tool_name);

/* True iff every tool named in the long-running timeout table resolves to a
 * registered route. A test asserts this so renaming a tool without updating
 * the table (which would silently revert it to the 5s default) fails loudly. */
bool mcp_long_running_tools_all_registered(void);

/* Wraps `mcp_router_dispatch` with the full policy chain.  bearer_token
 * is the value of the caller's `authorization` header / request metadata
 * (may be NULL if no token was provided).  Returns a malloc'd JSON
 * envelope — success body OR error envelope.  Never NULL. */
char *mcp_middleware_dispatch(struct mcp_middleware *mw,
                               const char *tool_name,
                               const struct json_value *args,
                               const char *bearer_token);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_MCP_MIDDLEWARE_H */
