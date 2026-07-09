/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP Middleware — implementation.  See middleware.h for the contract.
 */

#include "platform/time_compat.h"
#include "mcp/middleware.h"
#include "mcp/router.h"
#include "event/event.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "util/safe_alloc.h"
#include "util/log_macros.h"

/* ── Canonical destructive tool list ─────────────────────────── *
 *
 * Any tool that changes wallet, network, or chain state goes here.  Kept
 * in sync with the list `zcl_self_test` uses to skip destructive calls.
 */
static const char *k_default_destructive[] = {
    "zcl_send",
    "zcl_sendtoaddress",
    "zcl_importprivkey",
    "zcl_wallet_receive_intent",
    "zcl_rescanblockchain",
    "zcl_replaywalletfromchain",
    "zcl_wallet_backup_now",
    "zcl_addnode",
    "zcl_swap_initiate",
    "zcl_swap_participate",
    "zcl_market_buy",
    "zcl_market_offer",
    "zcl_msg_send",
    "zcl_msg_send_named",
    "zcl_name_register",
    "zcl_invalidateblock",
    "zcl_reconsiderblock",
    /* Not a mutation, but unbounded scans can be expensive — gate
     * at the destructive rate (1 RPS) to prevent abuse. */
    "zcl_sql",
};

static int64_t now_us(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

void mcp_middleware_init(struct mcp_middleware *mw)
{
    memset(mw, 0, sizeof(*mw));
    mw->global_rps = 100;
    mw->destructive_rps = 1;
    mw->burst_global = 200;
    mw->burst_destructive = 2;
    mw->default_timeout_ms = 5000;

    /* Populate destructive list */
    size_t n = sizeof(k_default_destructive) / sizeof(k_default_destructive[0]);
    if (n > MCP_MIDDLEWARE_DESTRUCT_MAX) n = MCP_MIDDLEWARE_DESTRUCT_MAX;
    for (size_t i = 0; i < n; i++)
        mw->destructive_tools[i] = k_default_destructive[i];
    mw->num_destructive_tools = n;

    /* Start with full buckets. */
    mw->global_bucket = (double)mw->burst_global;
    mw->destructive_bucket = (double)mw->burst_destructive;
    mw->last_global_refill_us = now_us();
    mw->last_destructive_refill_us = mw->last_global_refill_us;

    pthread_mutex_init(&mw->bucket_lock, NULL);
    mw->initialized = true;
}

void mcp_middleware_destroy(struct mcp_middleware *mw)
{
    if (!mw || !mw->initialized) return;
    pthread_mutex_destroy(&mw->bucket_lock);
    mw->initialized = false;
}

/* Process-wide MCP middleware singleton.  Lives in the controller
 * library (not mcp_server.c) so test_zcl can link the accessor symbol
 * without dragging in the server's stdio loop.  Returns NULL until
 * mcp_middleware_init_global() runs at server startup. */
static struct mcp_middleware g_mcp_middleware;
static bool g_mcp_middleware_active = false;

void mcp_middleware_init_global(void)
{
    if (g_mcp_middleware_active) return;
    mcp_middleware_init(&g_mcp_middleware);
    mcp_middleware_load_from_env(&g_mcp_middleware);
    g_mcp_middleware_active = true;
}

struct mcp_middleware *mcp_middleware_get_global(void)
{
    return g_mcp_middleware_active ? &g_mcp_middleware : NULL;
}

void mcp_middleware_load_from_env(struct mcp_middleware *mw)
{
    if (!mw->initialized) mcp_middleware_init(mw);

    const char *token = getenv("ZCL_MCP_BEARER_TOKEN");
    if (token && token[0]) {
        snprintf(mw->required_bearer_token,
                 sizeof(mw->required_bearer_token), "%s", token);
    }

    /* Escalated destructive tier — UNSET preserves today's behavior (the
     * normal token governs all tools). When set, destructive tools require
     * this token and reject the normal one (least-privilege). See middleware.h. */
    const char *dtoken = getenv("ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN");
    if (dtoken && dtoken[0]) {
        snprintf(mw->required_destructive_bearer_token,
                 sizeof(mw->required_destructive_bearer_token), "%s", dtoken);
    }

    const char *grps = getenv("ZCL_MCP_GLOBAL_RPS");
    if (grps && grps[0]) {
        int64_t v = strtoll(grps, NULL, 10);
        if (v > 0 && v < 1000000) {
            mw->global_rps = v;
            mw->burst_global = v * 2;
            if (mw->global_bucket > (double)mw->burst_global)
                mw->global_bucket = (double)mw->burst_global;
        }
    }

    const char *drps = getenv("ZCL_MCP_DESTRUCTIVE_RPS");
    if (drps && drps[0]) {
        int64_t v = strtoll(drps, NULL, 10);
        if (v > 0 && v < 1000000) {
            mw->destructive_rps = v;
            mw->burst_destructive = v * 2 < 1 ? 1 : v * 2;
            if (mw->destructive_bucket > (double)mw->burst_destructive)
                mw->destructive_bucket = (double)mw->burst_destructive;
        }
    }

    const char *timeout = getenv("ZCL_MCP_TIMEOUT_MS");
    if (timeout && timeout[0]) {
        int64_t v = strtoll(timeout, NULL, 10);
        if (v > 0 && v < 600000)
            mw->default_timeout_ms = v;
    }
}

bool mcp_middleware_is_destructive(const struct mcp_middleware *mw,
                                    const char *tool_name)
{
    if (!mw || !tool_name) return false;
    /* Source of truth is the route's `flags` field, populated inline
     * by each controller. Falls back to the seed list in mw->destructive_tools
     * for tools that aren't (yet) registered when this is called. */
    const struct mcp_tool_route *r = mcp_router_find(tool_name);
    if (r && (r->flags & MCP_TOOL_FLAG_DESTRUCTIVE))
        return true;
    for (size_t i = 0; i < mw->num_destructive_tools; i++) {
        if (mw->destructive_tools[i] &&
            strcmp(mw->destructive_tools[i], tool_name) == 0)
            return true;
    }
    return false;
}

/* Tools whose handler legitimately runs longer than the global 5s dispatch
 * deadline, with the budget each needs. zcl_profile nanosleeps its
 * schema-clamped duration_ms (up to 10000); the zcl_waitfor* long-polls are
 * internally capped at WAIT_RPC_MAX_MS=9000. Without headroom over the global
 * default, a legal call is killed with a spurious MCP_ERR_TOOL_TIMEOUT while
 * its detached worker keeps running and its result is discarded. Kept as a
 * name-keyed table (a sibling of k_default_destructive) so no per-route struct
 * field is needed; mcp_long_running_tools_all_registered() guards against a
 * rename silently reverting a tool to the 5s default. */
static const struct {
    const char *tool;
    int64_t     timeout_ms;
} k_long_running_tools[] = {
    { "zcl_profile",       12000 },
    { "zcl_waitforheight", 12000 },
    { "zcl_waitforhalt",   12000 },
    { "zcl_waitforblocker", 12000 },
};

int64_t mcp_middleware_resolve_timeout_ms(const struct mcp_middleware *mw,
                                          const char *tool_name)
{
    int64_t base = mw ? mw->default_timeout_ms : 0;
    if (tool_name) {
        size_t n = sizeof(k_long_running_tools) /
                   sizeof(k_long_running_tools[0]);
        for (size_t i = 0; i < n; i++)
            if (strcmp(tool_name, k_long_running_tools[i].tool) == 0)
                return k_long_running_tools[i].timeout_ms;
    }
    return base;
}

bool mcp_long_running_tools_all_registered(void)
{
    size_t n = sizeof(k_long_running_tools) / sizeof(k_long_running_tools[0]);
    for (size_t i = 0; i < n; i++)
        if (!mcp_router_find(k_long_running_tools[i].tool))
            return false;
    return true;
}

/* ── Auth ────────────────────────────────────────────────────── */

static int constant_time_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    unsigned int diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned int)(x[i] ^ y[i]);
    return diff == 0 ? 0 : 1;
}

/* True iff `bearer_token` matches one specific required token.  An empty
 * `required` means auth is DISABLED for that tier (pass-through), preserving
 * the original `auth_ok` semantic.  Accepts both "Bearer <token>" and the
 * bare token form.  Constant-time over the token bytes. */
static bool token_matches(const char *required, const char *bearer_token)
{
    if (required[0] == '\0') return true;
    if (!bearer_token) return false;
    const char *candidate = bearer_token;
    if (strncmp(candidate, "Bearer ", 7) == 0) candidate += 7;
    size_t req_len = strlen(required);
    size_t got_len = strlen(candidate);
    if (req_len != got_len) return false;
    return constant_time_memcmp(required, candidate, req_len) == 0;
}

/* Back-compat single-tier check: the normal token governs every tool.
 * Used when no destructive tier is configured (today's behavior). */
static bool auth_ok(const struct mcp_middleware *mw, const char *bearer_token)
{
    return token_matches(mw->required_bearer_token, bearer_token);
}

/* Two-tier auth classifier.  Returns NULL on success; on failure returns a
 * static reason string naming the tier that was required/missing — no token
 * material is disclosed.  `is_destructive` is the tool's classified tier.
 *
 * Semantics (see middleware.h):
 *   - destructive token UNSET → normal token governs ALL tools (back-compat).
 *   - destructive token SET   → destructive tool REQUIRES the destructive
 *                               token; non-destructive REQUIRES the normal
 *                               token.  The "wrong" tier is rejected even
 *                               when it is a valid token of the other tier. */
static const char *auth_check(const struct mcp_middleware *mw,
                              const char *bearer_token, bool is_destructive)
{
    /* No destructive tier configured → back-compat: normal token for all. */
    if (mw->required_destructive_bearer_token[0] == '\0') {
        if (auth_ok(mw, bearer_token)) return NULL;
        return "bearer token required";
    }

    /* Two-tier mode: select the required token by tool class.  An empty
     * normal token still means "auth disabled for non-destructive tools",
     * but a SET destructive token always gates destructive tools. */
    const char *required = is_destructive
        ? mw->required_destructive_bearer_token
        : mw->required_bearer_token;
    if (token_matches(required, bearer_token)) return NULL;
    return is_destructive
        ? "destructive bearer token required"
        : "normal bearer token required";
}

/* ── Token bucket ────────────────────────────────────────────── */

static void refill_bucket(double *bucket, int64_t *last_us,
                           int64_t rps, int64_t burst)
{
    int64_t now = now_us();
    int64_t dt_us = now - *last_us;
    if (dt_us <= 0) return;
    double add = ((double)rps * (double)dt_us) / 1000000.0;
    *bucket += add;
    if (*bucket > (double)burst) *bucket = (double)burst;
    *last_us = now;
}

static bool consume_global(struct mcp_middleware *mw)
{
    pthread_mutex_lock(&mw->bucket_lock);
    refill_bucket(&mw->global_bucket, &mw->last_global_refill_us,
                  mw->global_rps, mw->burst_global);
    bool ok = mw->global_bucket >= 1.0;
    if (ok) mw->global_bucket -= 1.0;
    pthread_mutex_unlock(&mw->bucket_lock);
    return ok;
}

static bool consume_destructive(struct mcp_middleware *mw)
{
    pthread_mutex_lock(&mw->bucket_lock);
    refill_bucket(&mw->destructive_bucket, &mw->last_destructive_refill_us,
                  mw->destructive_rps, mw->burst_destructive);
    bool ok = mw->destructive_bucket >= 1.0;
    if (ok) mw->destructive_bucket -= 1.0;
    pthread_mutex_unlock(&mw->bucket_lock);
    return ok;
}

/* ── Timeout dispatch ────────────────────────────────────────── *
 *
 * The dispatch context is HEAP-allocated and shared between two threads:
 * the caller (which waits up to `timeout_ms`) and a detached worker
 * (which runs the handler). On a timeout the caller returns immediately
 * but the worker keeps running — it must still touch ctx->m / ctx->cv
 * when it eventually completes. A stack-allocated ctx would be destroyed
 * and its frame reclaimed the moment the caller returned, so the late
 * worker would lock a destroyed mutex on a reused frame (UB + UAF).
 *
 * Ownership is a refcount: each side holds one reference and drops it
 * when it is done touching the ctx. Whoever drops the LAST reference
 * destroys the mutex/cv and frees the ctx — never the loser of the race.
 */

struct timeout_ctx {
    const char              *tool_name;
    const struct json_value *args;
    char                    *body;        /* handler result, if any */
    bool                     done;
    pthread_mutex_t          m;
    pthread_cond_t           cv;
    atomic_int               refcount;    /* caller + worker; last frees */
};

/* Drop a reference; the last toucher tears down and frees the ctx. */
static void timeout_ctx_release(struct timeout_ctx *ctx)
{
    if (atomic_fetch_sub_explicit(&ctx->refcount, 1,
                                  memory_order_acq_rel) != 1)
        return;
    /* We are the last reference: no other thread can touch the mutex/cv
     * now, so destroying them here is safe. */
    pthread_mutex_destroy(&ctx->m);
    pthread_cond_destroy(&ctx->cv);
    free(ctx);
}

static void *run_dispatch(void *arg)
{
    struct timeout_ctx *ctx = arg;
    char *b = mcp_router_dispatch(ctx->tool_name, ctx->args);
    pthread_mutex_lock(&ctx->m);
    /* If the caller already gave up (timeout), discard our result. */
    if (ctx->done) {
        pthread_mutex_unlock(&ctx->m);
        free(b);
    } else {
        ctx->body = b;
        ctx->done = true;
        pthread_cond_signal(&ctx->cv);
        pthread_mutex_unlock(&ctx->m);
    }
    timeout_ctx_release(ctx);
    return NULL;
}

static char *dispatch_with_timeout(const char *tool_name,
                                    const struct json_value *args,
                                    int64_t timeout_ms,
                                    bool *out_timed_out)
{
    *out_timed_out = false;

    /* Zero / negative timeout disables the guard. */
    if (timeout_ms <= 0)
        return mcp_router_dispatch(tool_name, args);

    /* Cap before the deadline calc below: a wild caller value would make
     * `deadline.tv_sec += timeout_ms / 1000` overflow time_t. One hour is
     * far longer than any legitimate MCP dispatch. */
    if (timeout_ms > 3600000)
        timeout_ms = 3600000;

    struct timeout_ctx *ctx = zcl_malloc(sizeof(*ctx), "mcp.timeout_ctx");
    if (!ctx) {
        /* zcl_malloc already logged the OOM with context. Degrade
         * gracefully: run synchronously, with no timeout guard. */
        LOG_WARN("mcp", "timeout_ctx alloc failed; dispatching synchronously");
        return mcp_router_dispatch(tool_name, args);
    }
    ctx->tool_name = tool_name;
    ctx->args      = args;
    ctx->body      = NULL;
    ctx->done      = false;
    /* Two owners: this caller and the worker we are about to spawn. */
    atomic_init(&ctx->refcount, 2);
    pthread_mutex_init(&ctx->m, NULL);
    pthread_cond_init(&ctx->cv, NULL);

    pthread_t th;
    /* Per-dispatch helper, detached; ctx is refcounted so the worker
     * safely outlives this frame on the timeout path. */
    /* raw-pthread-ok: detached per-dispatch helper, refcounted ctx */
    if (pthread_create(&th, NULL, run_dispatch, ctx) != 0) {
        /* Worker never started — reclaim its reference too, then ours. */
        timeout_ctx_release(ctx);
        timeout_ctx_release(ctx);
        /* Fall back to synchronous dispatch. */
        return mcp_router_dispatch(tool_name, args);
    }
    pthread_detach(th);

    struct timespec deadline;
    platform_time_realtime_timespec(&deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ctx->m);
    int rc = 0;
    while (!ctx->done && rc == 0)
        rc = pthread_cond_timedwait(&ctx->cv, &ctx->m, &deadline);

    char *result = NULL;
    if (ctx->done) {
        result = ctx->body;
        ctx->body = NULL;
    } else {
        /* Timed out — abandon the worker thread.  It will free its own
         * body when it eventually completes (see run_dispatch). */
        *out_timed_out = true;
        ctx->done = true;  /* instruct worker to discard its result */
    }
    pthread_mutex_unlock(&ctx->m);

    /* Drop our reference. If the worker already finished it dropped its
     * reference first and this frees the ctx; otherwise the still-running
     * worker holds the last reference and frees it on completion. Either
     * way the mutex/cv outlive every thread that touches them. */
    timeout_ctx_release(ctx);
    return result;
}

/* ── Public dispatch ─────────────────────────────────────────── */

char *mcp_middleware_dispatch(struct mcp_middleware *mw,
                               const char *tool_name,
                               const struct json_value *args,
                               const char *bearer_token)
{
    if (!mw || !mw->initialized) {
        /* Middleware not configured — fall straight through. */
        return mcp_router_dispatch(tool_name, args);
    }
    if (!tool_name) tool_name = "";

    /* Classify the tool once: this drives BOTH the auth tier selection and
     * the destructive rate-limit bucket.  Auth and rate-limit remain
     * INDEPENDENT policies — which tier passed auth does not change which
     * bucket is consumed (a destructive tool always drains the destructive
     * bucket regardless of which credential satisfied auth). */
    bool is_dest = mcp_middleware_is_destructive(mw, tool_name);

    /* 1. Auth (tiered) */
    const char *auth_reason = auth_check(mw, bearer_token, is_dest);
    if (auth_reason) {
        mw->stat_auth_denied++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=AUTH_REQUIRED", tool_name);
        return mcp_router_error_envelope_strdup(MCP_ERR_AUTH_REQUIRED,
                              tool_name, NULL, auth_reason);
    }

    /* 2. Global rate limit */
    if (!consume_global(mw)) {
        mw->stat_rate_limited_global++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=RATE_LIMITED kind=global", tool_name);
        return mcp_router_error_envelope_strdup(MCP_ERR_RATE_LIMITED,
                              tool_name, "global", "global rate limit exceeded");
    }

    /* 3. Destructive rate limit (independent of auth tier) */
    if (is_dest && !consume_destructive(mw)) {
        mw->stat_rate_limited_destructive++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=RATE_LIMITED kind=destructive",
                    tool_name);
        return mcp_router_error_envelope_strdup(MCP_ERR_RATE_LIMITED,
                              tool_name, "destructive",
                              "destructive rate limit exceeded");
    }

    /* 4. Timeout-guarded dispatch. A few tools legitimately run longer than
     * the global default (zcl_profile, zcl_waitfor*); resolve their budget
     * from the long-running table, else fall back to the global default. */
    int64_t timeout_ms = mcp_middleware_resolve_timeout_ms(mw, tool_name);

    bool timed_out = false;
    char *body = dispatch_with_timeout(tool_name, args,
                                        timeout_ms, &timed_out);
    if (timed_out) {
        mw->stat_timeout++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=TOOL_TIMEOUT deadline_ms=%lld",
                    tool_name, (long long)timeout_ms);
        return mcp_router_error_envelope_strdup(MCP_ERR_TOOL_TIMEOUT,
                              tool_name, NULL, "handler exceeded timeout");
    }

    mw->stat_allowed++;
    return body;
}
