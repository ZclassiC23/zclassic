/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the MCP middleware: auth, rate limiting, destructive bucket,
 * per-tool timeout, and envelope shapes.  Uses stub routes so the tests
 * run without a node.
 */

#include "test/test_helpers.h"
#include "mcp/router.h"
#include "mcp/middleware.h"
#include "event/event.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── Stub handlers ──────────────────────────────────────────── */

static int h_ok(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    res->body = strdup("{\"ok\":true}");
    return 0;
}

static int h_slow(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    sleep_ms(500);  /* 500 ms */
    res->body = strdup("{\"ok\":true,\"slow\":true}");
    return 0;
}

/* ── Gated slow handler (UAF regression) ────────────────────────
 *
 * A handler whose completion is controlled by the test, so the worker
 * thread is guaranteed to touch the shared timeout context STRICTLY
 * AFTER mcp_middleware_dispatch() returns on the timeout path. That is
 * exactly the window where the old stack-allocated ctx had already been
 * destroyed and its frame reused — a use-after-free. With the heap +
 * refcount fix the ctx outlives both threads.
 */
static pthread_mutex_t g_gate_m       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cv      = PTHREAD_COND_INITIALIZER;
static bool            g_gate_open     = false;  /* test releases worker */
static bool            g_handler_entered = false;
static atomic_bool     g_handler_finished = false;

static void gate_reset(void)
{
    pthread_mutex_lock(&g_gate_m);
    g_gate_open = false;
    g_handler_entered = false;
    pthread_mutex_unlock(&g_gate_m);
    atomic_store(&g_handler_finished, false);
}

static void gate_open(void)
{
    pthread_mutex_lock(&g_gate_m);
    g_gate_open = true;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_m);
}

static int h_gated(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    pthread_mutex_lock(&g_gate_m);
    g_handler_entered = true;
    pthread_cond_broadcast(&g_gate_cv);
    /* Block until the test explicitly releases us — guarantees we finish
     * after the caller has already timed out and returned. */
    while (!g_gate_open)
        pthread_cond_wait(&g_gate_cv, &g_gate_m);
    pthread_mutex_unlock(&g_gate_m);

    res->body = strdup("{\"ok\":true,\"gated\":true}");
    /* The worker is about to touch the shared timeout context (lock its
     * mutex). Mark that it returned cleanly so the test can confirm the
     * ctx outlived this thread. */
    atomic_store(&g_handler_finished, true);
    return 0;
}

static const struct mcp_tool_route r_read = {
    "t_read", "test", "read-only", NULL, 0, h_ok, 0, NULL
};
static const struct mcp_tool_route r_send = {
    "zcl_send", "test", "sim send", NULL, 0, h_ok, 0, NULL
};
static const struct mcp_tool_route r_slow = {
    "t_slow", "test", "slow handler", NULL, 0, h_slow, 0, NULL
};
static const struct mcp_tool_route r_gated = {
    "t_gated", "test", "gated handler", NULL, 0, h_gated, 0, NULL
};

static void register_routes(void)
{
    mcp_router_reset();
    mcp_router_register(&r_read);
    mcp_router_register(&r_send);
    mcp_router_register(&r_slow);
    mcp_router_register(&r_gated);
}

/* ── Helpers ───────────────────────────────────────────────── */

static bool contains(const char *s, const char *sub)
{
    return s && sub && strstr(s, sub) != NULL;
}

static char *run(struct mcp_middleware *mw, const char *tool,
                  const char *bearer)
{
    return mcp_middleware_dispatch(mw, tool, NULL, bearer);
}

/* ── Test cases ────────────────────────────────────────────── */

static int test_init_defaults(void)
{
    int failures = 0;
    TEST("mcp_middleware_init sets safe defaults") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        ASSERT(mw.initialized);
        ASSERT(mw.global_rps == 100);
        ASSERT(mw.destructive_rps == 1);
        ASSERT(mw.default_timeout_ms == 5000);
        ASSERT(mw.num_destructive_tools >= 10);
        ASSERT(mw.required_bearer_token[0] == '\0');
        ASSERT(mcp_middleware_is_destructive(&mw, "zcl_send"));
        ASSERT(mcp_middleware_is_destructive(&mw, "zcl_importprivkey"));
        ASSERT(!mcp_middleware_is_destructive(&mw, "zcl_status"));
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_env_overrides(void)
{
    int failures = 0;
    TEST("mcp_middleware_load_from_env overrides defaults") {
        setenv("ZCL_MCP_BEARER_TOKEN", "secret123", 1);
        setenv("ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN", "destruct456", 1);
        setenv("ZCL_MCP_GLOBAL_RPS", "50", 1);
        setenv("ZCL_MCP_DESTRUCTIVE_RPS", "3", 1);
        setenv("ZCL_MCP_TIMEOUT_MS", "1000", 1);

        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        mcp_middleware_load_from_env(&mw);

        ASSERT(strcmp(mw.required_bearer_token, "secret123") == 0);
        ASSERT(strcmp(mw.required_destructive_bearer_token, "destruct456") == 0);
        ASSERT(mw.global_rps == 50);
        ASSERT(mw.destructive_rps == 3);
        ASSERT(mw.default_timeout_ms == 1000);

        unsetenv("ZCL_MCP_BEARER_TOKEN");
        unsetenv("ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN");
        unsetenv("ZCL_MCP_GLOBAL_RPS");
        unsetenv("ZCL_MCP_DESTRUCTIVE_RPS");
        unsetenv("ZCL_MCP_TIMEOUT_MS");
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_auth_pass_when_unset(void)
{
    int failures = 0;
    TEST("no auth required → any token works") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        register_routes();

        char *r = run(&mw, "t_read", NULL);
        ASSERT(r != NULL);
        ASSERT(contains(r, "\"ok\":true"));
        free(r);

        r = run(&mw, "t_read", "anything");
        ASSERT(contains(r, "\"ok\":true"));
        free(r);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_auth_fail(void)
{
    int failures = 0;
    TEST("wrong bearer token → AUTH_REQUIRED envelope") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "expected-token");
        register_routes();

        char *r = run(&mw, "t_read", NULL);
        ASSERT(contains(r, "AUTH_REQUIRED"));
        free(r);

        r = run(&mw, "t_read", "wrong-token");
        ASSERT(contains(r, "AUTH_REQUIRED"));
        free(r);

        ASSERT(mw.stat_auth_denied >= 2);
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_auth_pass_with_bearer_prefix(void)
{
    int failures = 0;
    TEST("correct bearer token accepted with or without prefix") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "token-abc");
        register_routes();

        char *r = run(&mw, "t_read", "token-abc");
        ASSERT(contains(r, "\"ok\":true"));
        ASSERT(!contains(r, "AUTH_REQUIRED"));
        free(r);

        r = run(&mw, "t_read", "Bearer token-abc");
        ASSERT(contains(r, "\"ok\":true"));
        free(r);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Two-tier (escalated destructive) auth ───────────────────────
 *
 * ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN is opt-in.  Unset → today's behavior.
 * Set → destructive tools require the destructive token and reject the
 * normal token; non-destructive tools require the normal token and reject
 * the destructive token (least-privilege in both directions).
 */

/* T1: back-compat — no destructive token configured. */
static int test_auth_back_compat_no_destructive_tier(void)
{
    int failures = 0;
    TEST("destructive token unset → normal token governs destructive tools (back-compat)") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "normal-secret");
        /* required_destructive_bearer_token intentionally left empty */
        register_routes();

        /* Destructive tool + correct normal token → allowed (today's behavior). */
        char *r = run(&mw, "zcl_send", "normal-secret");
        ASSERT(contains(r, "\"ok\":true"));
        ASSERT(!contains(r, "AUTH_REQUIRED"));
        free(r);

        /* Destructive tool + no token → AUTH_REQUIRED. */
        r = run(&mw, "zcl_send", NULL);
        ASSERT(contains(r, "AUTH_REQUIRED"));
        free(r);

        /* Destructive tool + wrong token → AUTH_REQUIRED. */
        r = run(&mw, "zcl_send", "wrong-token");
        ASSERT(contains(r, "AUTH_REQUIRED"));
        free(r);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

/* T2: destructive token configured → destructive tool accepts destructive,
 * rejects the normal token. */
static int test_auth_destructive_tool_requires_destructive_tier(void)
{
    int failures = 0;
    TEST("destructive token set → destructive tool accepts destructive, rejects normal") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "normal-secret");
        snprintf(mw.required_destructive_bearer_token,
                 sizeof(mw.required_destructive_bearer_token),
                 "%s", "destruct-secret");
        register_routes();

        /* Correct destructive token → allowed. */
        char *r = run(&mw, "zcl_send", "destruct-secret");
        ASSERT(contains(r, "\"ok\":true"));
        ASSERT(!contains(r, "AUTH_REQUIRED"));
        free(r);

        /* "Bearer " prefix form also accepted for the destructive tier. */
        r = run(&mw, "zcl_send", "Bearer destruct-secret");
        ASSERT(contains(r, "\"ok\":true"));
        ASSERT(!contains(r, "AUTH_REQUIRED"));
        free(r);

        /* Normal token is rejected for the destructive tool — 401 names the
         * destructive tier, no token material disclosed. */
        r = run(&mw, "zcl_send", "normal-secret");
        ASSERT(contains(r, "AUTH_REQUIRED"));
        ASSERT(contains(r, "destructive bearer token required"));
        ASSERT(!contains(r, "destruct-secret"));
        ASSERT(!contains(r, "normal-secret"));
        free(r);

        /* No token → rejected with destructive-tier reason. */
        r = run(&mw, "zcl_send", NULL);
        ASSERT(contains(r, "AUTH_REQUIRED"));
        ASSERT(contains(r, "destructive bearer token required"));
        free(r);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

/* T3: non-destructive tool rejects the destructive token, accepts normal. */
static int test_auth_read_tool_rejects_destructive_tier(void)
{
    int failures = 0;
    TEST("destructive token set → read tool rejects destructive, accepts normal") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "normal-secret");
        snprintf(mw.required_destructive_bearer_token,
                 sizeof(mw.required_destructive_bearer_token),
                 "%s", "destruct-secret");
        register_routes();

        /* Correct normal token → allowed. */
        char *r = run(&mw, "t_read", "normal-secret");
        ASSERT(contains(r, "\"ok\":true"));
        ASSERT(!contains(r, "AUTH_REQUIRED"));
        free(r);

        /* Destructive token rejected for the read tool — 401 names the
         * normal tier.  An ops/destructive credential cannot be reused to
         * read introspection or normal RPC. */
        r = run(&mw, "t_read", "destruct-secret");
        ASSERT(contains(r, "AUTH_REQUIRED"));
        ASSERT(contains(r, "normal bearer token required"));
        ASSERT(!contains(r, "destruct-secret"));
        ASSERT(!contains(r, "normal-secret"));
        free(r);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

/* T4: destructive rate-limit fires independently of which auth tier passed.
 * Auth tier ≠ rate-limit tier: a destructive tool always drains the
 * destructive bucket regardless of which credential satisfied auth. */
static int test_destructive_ratelimit_independent_of_auth_tier(void)
{
    int failures = 0;
    TEST("destructive rate-limit fires regardless of which auth tier passed") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "normal-secret");
        snprintf(mw.required_destructive_bearer_token,
                 sizeof(mw.required_destructive_bearer_token),
                 "%s", "destruct-secret");
        /* Pin buckets: global plenty, destructive tiny. */
        mw.global_rps = 1000;
        mw.burst_global = 1000;
        mw.global_bucket = 1000.0;
        mw.destructive_rps = 1;
        mw.burst_destructive = 1;
        mw.destructive_bucket = 1.0;
        register_routes();

        /* First destructive call (valid destructive token) → allowed,
         * drains the single destructive bucket token. */
        char *r1 = run(&mw, "zcl_send", "destruct-secret");
        ASSERT(contains(r1, "\"ok\":true"));
        ASSERT(!contains(r1, "AUTH_REQUIRED"));
        ASSERT(!contains(r1, "RATE_LIMITED"));
        free(r1);

        /* Second destructive call (same valid token) → RATE_LIMITED, NOT
         * auth-denied: auth passed, the destructive bucket is empty. */
        char *r2 = run(&mw, "zcl_send", "destruct-secret");
        ASSERT(contains(r2, "RATE_LIMITED"));
        ASSERT(contains(r2, "\"param\":\"destructive\""));
        ASSERT(!contains(r2, "AUTH_REQUIRED"));
        free(r2);

        ASSERT(mw.stat_rate_limited_destructive >= 1);
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_global_rate_limit(void)
{
    int failures = 0;
    TEST("global rate limit fires when bucket drained") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        /* Tiny bucket: 1 rps, burst 1 */
        mw.global_rps = 1;
        mw.burst_global = 1;
        mw.global_bucket = 1.0;  /* start with 1 token */
        register_routes();

        char *r1 = run(&mw, "t_read", NULL);
        ASSERT(contains(r1, "\"ok\":true"));
        free(r1);

        char *r2 = run(&mw, "t_read", NULL);
        ASSERT(contains(r2, "RATE_LIMITED"));
        ASSERT(contains(r2, "\"param\":\"global\""));
        free(r2);

        ASSERT(mw.stat_rate_limited_global >= 1);
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_destructive_rate_limit(void)
{
    int failures = 0;
    TEST("destructive rate limit drains a separate bucket") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        mw.global_rps = 1000;       /* plenty */
        mw.burst_global = 1000;
        mw.global_bucket = 1000.0;
        mw.destructive_rps = 1;
        mw.burst_destructive = 1;
        mw.destructive_bucket = 1.0;
        register_routes();

        /* First destructive call: allowed */
        char *r1 = run(&mw, "zcl_send", NULL);
        ASSERT(!contains(r1, "RATE_LIMITED"));
        free(r1);

        /* Second destructive call: denied */
        char *r2 = run(&mw, "zcl_send", NULL);
        ASSERT(contains(r2, "RATE_LIMITED"));
        ASSERT(contains(r2, "\"param\":\"destructive\""));
        free(r2);

        /* Non-destructive still works */
        char *r3 = run(&mw, "t_read", NULL);
        ASSERT(contains(r3, "\"ok\":true"));
        free(r3);

        ASSERT(mw.stat_rate_limited_destructive >= 1);
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_timeout_fires(void)
{
    int failures = 0;
    TEST("slow handler beyond deadline produces TOOL_TIMEOUT") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        mw.default_timeout_ms = 50;  /* 50 ms */
        register_routes();

        char *r = run(&mw, "t_slow", NULL);
        ASSERT(contains(r, "TOOL_TIMEOUT"));
        free(r);
        ASSERT(mw.stat_timeout >= 1);

        /* Cleanup: let the slow worker thread finish before destroying */
        sleep_ms(600);
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

/* Recursively descend the stack, poisoning a wide region of each frame
 * with a non-zero pattern. After mcp_middleware_dispatch() returns on the
 * timeout path, the buggy (stack-allocated) timeout context — including
 * its already-destroyed mutex/cv — sits in a reclaimed frame at a HIGHER
 * address than the test's frame. Re-descending here overwrites that exact
 * region, so a late worker locking that mutex operates on poisoned memory
 * (deterministic misbehaviour) rather than on bytes that happen to survive.
 * With the heap+refcount fix the worker's mutex lives on the heap and is
 * untouched by this. `volatile` + the returned checksum prevent the
 * compiler from eliding the writes. */
static volatile int g_poison_sink;
static int poison_stack(int depth)
{
    volatile unsigned char buf[2048];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (unsigned char)(0xC3 ^ (i & 0xFF) ^ (unsigned)depth);
    int sum = 0;
    for (size_t i = 0; i < sizeof(buf); i++) sum += buf[i];
    if (depth > 0) sum += poison_stack(depth - 1);
    g_poison_sink = sum;
    return sum;
}

/* Regression for the detached-worker UAF: a handler that completes
 * AFTER the caller times out must not touch a destroyed mutex on a
 * reclaimed stack frame. The shared timeout context is heap-allocated
 * and refcounted, so it outlives whichever side finishes last. */
static int test_timeout_worker_outlives_caller(void)
{
    int failures = 0;
    TEST("late worker safely touches ctx after caller timed out (no UAF)") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        mw.default_timeout_ms = 50;  /* short deadline */
        register_routes();
        gate_reset();

        /* The handler blocks on the gate, so dispatch is guaranteed to
         * exceed the 50 ms deadline and return TOOL_TIMEOUT. */
        char *r = run(&mw, "t_gated", NULL);
        ASSERT(contains(r, "TOOL_TIMEOUT"));
        free(r);
        ASSERT(mw.stat_timeout >= 1);

        /* mcp_middleware_dispatch has now fully returned and unwound its
         * frame. The worker is still blocked on the gate and has NOT yet
         * re-acquired the timeout context's mutex. Poison the reclaimed
         * stack region so that — under the OLD stack-allocated ctx — the
         * worker's subsequent pthread_mutex_lock would hit clobbered bytes
         * (a use-after-free that crashes/hangs/corrupts). Under the fix the
         * ctx is on the heap and this is a no-op for the worker. */
        ASSERT(poison_stack(8) != 0);

        gate_open();

        /* The worker must complete cleanly: it acquires the (heap) ctx
         * mutex, stores/frees its result, and drops its reference. If the
         * ctx had been freed/destroyed this would crash or hang. Bounded
         * wait so a regression manifests as a test failure, not a hang. */
        bool finished = false;
        for (int i = 0; i < 200; i++) {  /* up to ~2 s */
            if (atomic_load(&g_handler_finished)) { finished = true; break; }
            sleep_ms(10);
        }
        ASSERT(finished);

        /* Give the worker a moment to drop its ref and (if last) free the
         * ctx before we tear down the middleware. */
        sleep_ms(50);
        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_timeout_pass(void)
{
    int failures = 0;
    TEST("fast handler within deadline returns normally") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        mw.default_timeout_ms = 5000;
        register_routes();

        char *r = run(&mw, "t_read", NULL);
        ASSERT(contains(r, "\"ok\":true"));
        ASSERT(!contains(r, "TOOL_TIMEOUT"));
        free(r);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_destructive_detection(void)
{
    int failures = 0;
    TEST("is_destructive recognises canonical destructive tools") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);

        const char *destructive[] = {
            "zcl_send", "zcl_sendtoaddress", "zcl_importprivkey",
            "zcl_wallet_receive_intent",
            "zcl_rescanblockchain", "zcl_replaywalletfromchain",
            "zcl_wallet_backup_now",
            "zcl_addnode", "zcl_swap_initiate", "zcl_swap_participate",
            "zcl_market_buy", "zcl_market_offer",
            "zcl_msg_send", "zcl_msg_send_named", "zcl_name_register",
        };
        for (size_t i = 0; i < sizeof(destructive) / sizeof(destructive[0]); i++)
            ASSERT(mcp_middleware_is_destructive(&mw, destructive[i]));

        const char *readonly[] = {
            "zcl_status", "zcl_kpi", "zcl_getblock",
            "zcl_balance", "zcl_peers",
        };
        for (size_t i = 0; i < sizeof(readonly) / sizeof(readonly[0]); i++)
            ASSERT(!mcp_middleware_is_destructive(&mw, readonly[i]));

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_uninitialized_passthrough(void)
{
    int failures = 0;
    TEST("uninitialized middleware falls through to router") {
        struct mcp_middleware mw = {0};  /* not initialized */
        register_routes();

        char *r = run(&mw, "t_read", NULL);
        ASSERT(contains(r, "\"ok\":true"));
        free(r);
        PASS();
    } _test_next:;
    return failures;
}

static int test_stats_counters(void)
{
    int failures = 0;
    TEST("stat counters increment for each outcome") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        register_routes();

        for (int i = 0; i < 5; i++) {
            char *r = run(&mw, "t_read", NULL);
            free(r);
        }
        ASSERT(mw.stat_allowed >= 5);
        ASSERT(mw.stat_auth_denied == 0);

        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "x");
        char *r = run(&mw, "t_read", NULL);
        free(r);
        ASSERT(mw.stat_auth_denied >= 1);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_envelope_shape(void)
{
    int failures = 0;
    TEST("envelope shape is canonical for every new error code") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        snprintf(mw.required_bearer_token, sizeof(mw.required_bearer_token),
                 "%s", "required");
        register_routes();

        /* AUTH_REQUIRED envelope */
        char *r = run(&mw, "t_read", "wrong");
        ASSERT(contains(r, "\"error\""));
        ASSERT(contains(r, "\"code\":\"AUTH_REQUIRED\""));
        ASSERT(contains(r, "\"tool\":\"t_read\""));
        free(r);

        /* Unknown error code names */
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_AUTH_REQUIRED), "AUTH_REQUIRED");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_RATE_LIMITED), "RATE_LIMITED");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_TOOL_TIMEOUT), "TOOL_TIMEOUT");

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

static int test_refill_recovery(void)
{
    int failures = 0;
    TEST("drained bucket refills over wall-clock time") {
        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        mw.global_rps = 1000;       /* 1 token per millisecond */
        mw.burst_global = 1000;
        mw.global_bucket = 0.0;     /* fully drained */
        register_routes();

        char *r1 = run(&mw, "t_read", NULL);
        /* First call should be rate-limited (bucket empty) */
        ASSERT(contains(r1, "RATE_LIMITED"));
        free(r1);

        sleep_ms(200);  /* 200 ms -> ~200 tokens added */

        char *r2 = run(&mw, "t_read", NULL);
        ASSERT(contains(r2, "\"ok\":true"));
        free(r2);

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_mcp_middleware(void);

int test_mcp_middleware(void)
{
    int failures = 0;
    event_log_init();

    failures += test_init_defaults();
    failures += test_env_overrides();
    failures += test_auth_pass_when_unset();
    failures += test_auth_fail();
    failures += test_auth_pass_with_bearer_prefix();
    failures += test_auth_back_compat_no_destructive_tier();
    failures += test_auth_destructive_tool_requires_destructive_tier();
    failures += test_auth_read_tool_rejects_destructive_tier();
    failures += test_destructive_ratelimit_independent_of_auth_tier();
    failures += test_global_rate_limit();
    failures += test_destructive_rate_limit();
    failures += test_timeout_fires();
    failures += test_timeout_worker_outlives_caller();
    failures += test_timeout_pass();
    failures += test_destructive_detection();
    failures += test_uninitialized_passthrough();
    failures += test_stats_counters();
    failures += test_envelope_shape();
    failures += test_refill_recovery();

    mcp_router_reset();
    return failures;
}
