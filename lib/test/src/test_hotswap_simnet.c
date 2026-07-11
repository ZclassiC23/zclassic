/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Hermetic deterministic proof for transactional MCP hot-swap.  A real
 * three-node simnet_cluster supplies seeded peer delivery while a call holds
 * the generation-0 route across an atomic two-provider generation commit.
 * No sockets, services, files, or datadirs are used.
 */

#include "test/test_helpers.h"

#include "json/json.h"
#include "kernel/command_registry.h"
#include "mcp/router.h"
#include "platform/time_compat.h"
#include "sim/simnet_cluster.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HSIM_DEFAULT_SEED UINT64_C(0x4853574150563201)
#define HSIM_GENERATION 73u

#define HSIM_CHECK(name, expr) do {         \
    printf("%s... ", (name));               \
    if (expr) {                             \
        printf("OK\n");                    \
    } else {                                \
        printf("FAIL\n");                  \
        failures++;                         \
    }                                       \
} while (0)

struct hsim_gate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool entered;
    bool release;
};

static struct hsim_gate g_gate = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static int hsim_body(struct mcp_response *res, const char *body)
{
    size_t len = strlen(body) + 1;
    res->body = zcl_malloc(len, "hotswap simnet response");
    if (!res->body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "hot-swap simnet response allocation failed");
        return -1;
    }
    memcpy(res->body, body, len);
    return 0;
}

static int hsim_call_v0(const struct mcp_request *req,
                        struct mcp_response *res)
{
    (void)req;
    int rc = pthread_mutex_lock(&g_gate.mutex);
    if (rc != 0) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "hot-swap simnet gate lock failed: %d", rc);
        return -1;
    }

    g_gate.entered = true;
    pthread_cond_signal(&g_gate.cond);
    while (!g_gate.release) {
        rc = pthread_cond_wait(&g_gate.cond, &g_gate.mutex);
        if (rc != 0) {
            pthread_mutex_unlock(&g_gate.mutex);
            res->error = MCP_ERR_INTERNAL;
            snprintf(res->error_message, sizeof(res->error_message),
                     "hot-swap simnet gate wait failed: %d", rc);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_gate.mutex);
    return hsim_body(res,
                     "{\"provider\":\"call-v0\",\"generation\":0,\"peer\":1}");
}

static int hsim_catalog_v0(const struct mcp_request *req,
                           struct mcp_response *res)
{
    (void)req;
    return hsim_body(res,
                     "{\"provider\":\"catalog-v0\",\"generation\":0}");
}

static int hsim_call_v73(const struct mcp_request *req,
                         struct mcp_response *res)
{
    (void)req;
    return hsim_body(res,
                     "{\"provider\":\"call-v73\",\"generation\":73,\"peer\":2}");
}

static int hsim_catalog_v73(const struct mcp_request *req,
                            struct mcp_response *res)
{
    (void)req;
    return hsim_body(res,
                     "{\"provider\":\"catalog-v73\",\"generation\":73}");
}

static const struct mcp_tool_route g_call_v0 = {
    "sim.call", "simnet", "generation-zero call", NULL, 0,
    hsim_call_v0, 0, NULL
};
static const struct mcp_tool_route g_catalog_v0 = {
    "sim.catalog", "simnet", "generation-zero catalog", NULL, 0,
    hsim_catalog_v0, 0, NULL
};
static const struct mcp_tool_route g_call_v73 = {
    "sim.call", "simnet", "generation-73 call", NULL, 0,
    hsim_call_v73, 0, NULL
};
static const struct mcp_tool_route g_catalog_v73 = {
    "sim.catalog", "simnet", "generation-73 catalog", NULL, 0,
    hsim_catalog_v73, 0, NULL
};
static const struct mcp_tool_route g_catalog_bad = {
    "sim.catalog.mismatch", "simnet", "must be rejected", NULL, 0,
    hsim_catalog_v73, 0, NULL
};

struct hsim_call {
    const struct mcp_tool_route *route;
    int handler_rc;
    char *body;
};

struct hsim_result {
    struct uint256 converged_tip;
    uint64_t delivery_fingerprint;
    uint64_t transcript_fingerprint;
    uint32_t active_generation;
    bool cluster_converged;
    bool rejected_without_publication;
    bool atomic_commit_visible;
    bool old_call_completed;
    bool new_calls_visible;
    char old_call[96];
    char new_call[96];
    char new_catalog[96];
};

static void *hsim_inflight_call(void *arg)
{
    struct hsim_call *call = arg;
    call->route = mcp_router_find("sim.call");
    if (!call->route) {
        call->handler_rc = -1;
        return NULL;
    }
    struct mcp_request req = { .tool = "sim.call", .args = NULL };
    struct mcp_response res = {0};
    call->handler_rc = call->route->handler(&req, &res);
    call->body = res.body;
    return NULL;
}

static char *hsim_call_current(const char *name,
                               const struct mcp_tool_route *expected)
{
    const struct mcp_tool_route *route = mcp_router_find(name);
    if (route != expected)
        return NULL;
    struct mcp_request req = { .tool = name, .args = NULL };
    struct mcp_response res = {0};
    if (route->handler(&req, &res) != 0) {
        free(res.body);
        return NULL;
    }
    return res.body;
}

static bool hsim_gate_reset(void)
{
    if (pthread_mutex_lock(&g_gate.mutex) != 0)
        return false;
    g_gate.entered = false;
    g_gate.release = false;
    return pthread_mutex_unlock(&g_gate.mutex) == 0;
}

static bool hsim_gate_wait_entered(void)
{
    int rc = pthread_mutex_lock(&g_gate.mutex);
    if (rc != 0)
        return false;
    while (!g_gate.entered) {
        rc = pthread_cond_wait(&g_gate.cond, &g_gate.mutex);
        if (rc != 0) {
            pthread_mutex_unlock(&g_gate.mutex);
            return false;
        }
    }
    return pthread_mutex_unlock(&g_gate.mutex) == 0;
}

static void hsim_gate_release(void)
{
    if (pthread_mutex_lock(&g_gate.mutex) != 0)
        return;
    g_gate.release = true;
    pthread_cond_broadcast(&g_gate.cond);
    pthread_mutex_unlock(&g_gate.mutex);
}

static uint64_t hsim_fnv1a(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void hsim_fingerprint(uint64_t seed, struct hsim_result *result)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hsim_fnv1a(hash, &seed, sizeof(seed));
    hash = hsim_fnv1a(hash, result->converged_tip.data,
                      sizeof(result->converged_tip.data));
    hash = hsim_fnv1a(hash, &result->delivery_fingerprint,
                      sizeof(result->delivery_fingerprint));
    hash = hsim_fnv1a(hash, &result->active_generation,
                      sizeof(result->active_generation));
    hash = hsim_fnv1a(hash, result->old_call, strlen(result->old_call) + 1);
    hash = hsim_fnv1a(hash, result->new_call, strlen(result->new_call) + 1);
    hash = hsim_fnv1a(hash, result->new_catalog,
                      strlen(result->new_catalog) + 1);
    result->transcript_fingerprint = hash;
}

static bool hsim_results_equal(const struct hsim_result *a,
                               const struct hsim_result *b)
{
    return uint256_eq(&a->converged_tip, &b->converged_tip) &&
           a->delivery_fingerprint == b->delivery_fingerprint &&
           a->transcript_fingerprint == b->transcript_fingerprint &&
           a->active_generation == b->active_generation &&
           a->cluster_converged == b->cluster_converged &&
           a->rejected_without_publication ==
               b->rejected_without_publication &&
           a->atomic_commit_visible == b->atomic_commit_visible &&
           a->old_call_completed == b->old_call_completed &&
           a->new_calls_visible == b->new_calls_visible &&
           strcmp(a->old_call, b->old_call) == 0 &&
           strcmp(a->new_call, b->new_call) == 0 &&
           strcmp(a->new_catalog, b->new_catalog) == 0;
}

static bool hsim_cluster_relay(uint64_t seed, struct simnet_cluster **out,
                               struct hsim_result *result)
{
    struct simnet_cluster *cluster = simnet_cluster_init(3, seed);
    if (!cluster)
        return false;
    struct uint256 block_hash;
    struct uint256 tips[3];
    bool ok = simnet_cluster_mint_on(cluster, 0, &block_hash) &&
              simnet_cluster_broadcast(cluster, 0, &block_hash) &&
              simnet_cluster_deliver_pending(cluster) &&
              simnet_cluster_tip_hash(cluster, 0, &tips[0]) &&
              simnet_cluster_tip_hash(cluster, 1, &tips[1]) &&
              simnet_cluster_tip_hash(cluster, 2, &tips[2]) &&
              uint256_eq(&tips[0], &tips[1]) &&
              uint256_eq(&tips[0], &tips[2]);
    if (!ok) {
        simnet_cluster_free(cluster);
        return false;
    }
    result->converged_tip = tips[0];
    result->delivery_fingerprint =
        simnet_cluster_delivery_fingerprint(cluster);
    result->cluster_converged = true;
    *out = cluster;
    return true;
}

static bool hsim_run(uint64_t seed, struct hsim_result *result)
{
    struct simnet_cluster *cluster = NULL;
    pthread_t thread;
    bool thread_started = false;
    struct hsim_call inflight = {0};
    char *new_call = NULL;
    char *new_catalog = NULL;
    bool ok = false;

    memset(result, 0, sizeof(*result));
    mcp_router_reset();
    if (!mcp_router_register(&g_call_v0) ||
        !mcp_router_register(&g_catalog_v0) ||
        !hsim_cluster_relay(seed, &cluster, result) ||
        !hsim_gate_reset())
        goto cleanup;

    if (pthread_create(&thread, NULL, hsim_inflight_call, &inflight) != 0)
        goto cleanup;
    thread_started = true;
    if (!hsim_gate_wait_entered())
        goto cleanup;

    const struct mcp_router_replacement rejected[] = {
        { "sim.call", &g_call_v73 },
        { "sim.catalog", &g_catalog_bad },
    };
    char why[256];
    bool rejected_batch =
        !mcp_router_replace_batch(HSIM_GENERATION, rejected,
                                  sizeof(rejected) / sizeof(rejected[0]),
                                  why, sizeof(why));
    result->rejected_without_publication =
        rejected_batch && mcp_router_active_generation() == 0 &&
        mcp_router_find("sim.call") == &g_call_v0 &&
        mcp_router_find("sim.catalog") == &g_catalog_v0;
    if (!result->rejected_without_publication)
        goto cleanup;

    const struct mcp_router_replacement committed[] = {
        { "sim.call", &g_call_v73 },
        { "sim.catalog", &g_catalog_v73 },
    };
    result->atomic_commit_visible =
        mcp_router_replace_batch(HSIM_GENERATION, committed,
                                 sizeof(committed) / sizeof(committed[0]),
                                 why, sizeof(why)) &&
        mcp_router_active_generation() == HSIM_GENERATION &&
        mcp_router_find("sim.call") == &g_call_v73 &&
        mcp_router_find("sim.catalog") == &g_catalog_v73;
    if (!result->atomic_commit_visible)
        goto cleanup;

    /* Node 2 and node 1 acquire generation 73 while node 1's earlier call
     * remains blocked inside generation 0. */
    new_call = hsim_call_current("sim.call", &g_call_v73);
    new_catalog = hsim_call_current("sim.catalog", &g_catalog_v73);
    result->new_calls_visible =
        new_call && new_catalog && strstr(new_call, "call-v73") &&
        strstr(new_catalog, "catalog-v73");
    if (!result->new_calls_visible)
        goto cleanup;

    hsim_gate_release();
    if (pthread_join(thread, NULL) != 0)
        goto cleanup;
    thread_started = false;
    result->old_call_completed =
        inflight.route == &g_call_v0 && inflight.handler_rc == 0 &&
        inflight.body && strstr(inflight.body, "call-v0");
    if (!result->old_call_completed)
        goto cleanup;

    snprintf(result->old_call, sizeof(result->old_call), "%s", inflight.body);
    snprintf(result->new_call, sizeof(result->new_call), "%s", new_call);
    snprintf(result->new_catalog, sizeof(result->new_catalog), "%s",
             new_catalog);
    result->active_generation = mcp_router_active_generation();
    hsim_fingerprint(seed, result);
    ok = true;

cleanup:
    if (thread_started) {
        hsim_gate_release();
        pthread_join(thread, NULL);
    }
    free(inflight.body);
    free(new_call);
    free(new_catalog);
    if (cluster)
        simnet_cluster_free(cluster);
    mcp_router_reset();
    return ok;
}

static bool hsim_seed_from_env(uint64_t *seed_out)
{
    const char *text = getenv("ZCL_HOTSWAP_SIM_SEED");
    if (!text || !text[0]) {
        *seed_out = HSIM_DEFAULT_SEED;
        return true;
    }
    if (text[0] == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || !end || *end != '\0')
        return false;
    *seed_out = (uint64_t)parsed;
    return true;
}

/* ── native.leaves re-point (Wave W1-B/C) ─────────────────────────────
 *
 * The MCP-router simnet above proves the mcp.routes generation transaction
 * end to end. The zero-MCP successor is the kernel command-registry override
 * layer (lib/kernel/src/command_registry.c): a leaf-handler analogue of
 * mcp_router_replace_batch. This does NOT touch mcp/router.h at all — it
 * builds a small fixture registry (mirroring
 * lib/test/src/test_command_handler_snapshot.c's g_specs/g_reg pattern),
 * binds it as the active registry, and drives two successive
 * zcl_command_registry_replace_batch() generations across the SAME leaf
 * path, proving the atomic re-point plus generation monotonicity. */

static void hsim_leaf_h_base(const struct zcl_command_request *request,
                             struct zcl_command_reply *reply)
{
    (void)request;
    (void)json_push_kv_str(&reply->data, "gen", "BASE");
}

static void hsim_leaf_h_gen_a(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply)
{
    (void)request;
    (void)json_push_kv_str(&reply->data, "gen", "A");
}

static void hsim_leaf_h_gen_b(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply)
{
    (void)request;
    (void)json_push_kv_str(&reply->data, "gen", "B");
}

static const struct zcl_command_spec g_hsim_leaf_specs[] = {
    {
        .path = "sim.leaf.repoint",
        .summary = "hot-swap native-leaf re-point probe",
        .layer = ZCL_COMMAND_LAYER_CORE,
        .effect = ZCL_COMMAND_EFFECT_READ,
        .availability = ZCL_COMMAND_READY,
        .mode = ZCL_COMMAND_MODE_SYNC,
        .allowed_lanes = ZCL_COMMAND_LANE_LOCAL,
        .handler = hsim_leaf_h_base,
    },
};

static const struct zcl_command_registry g_hsim_leaf_registry = {
    .commands = g_hsim_leaf_specs,
    .count = sizeof(g_hsim_leaf_specs) / sizeof(g_hsim_leaf_specs[0]),
};

static enum zcl_command_exit hsim_leaf_dispatch(char *out, size_t out_size)
{
    struct json_value input;
    json_init(&input);
    json_set_object(&input);
    enum zcl_command_exit exit_code = ZCL_COMMAND_EXIT_INTERNAL;
    (void)zcl_command_registry_execute_json(
        &g_hsim_leaf_registry, &g_hsim_leaf_specs[0], NULL, &input, false,
        "sim.leaf.repoint", "normal", 0, 0, NULL, out, out_size, &exit_code);
    json_free(&input);
    return exit_code;
}

static int test_hotswap_native_leaf_repoint(void)
{
    int failures = 0;
    TEST("native.leaves: replace_batch atomically re-points a leaf handler "
         "the SAME way mcp_router_replace_batch re-points an MCP route; "
         "generation is strictly monotonic") {
        zcl_command_registry_reset_overrides();
        zcl_command_registry_set_active(&g_hsim_leaf_registry);
        char out[4096];
        char why[256];

        /* Baseline: catalog handler runs. */
        ASSERT_EQ((int)hsim_leaf_dispatch(out, sizeof(out)),
                  (int)ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"gen\":\"BASE\"") != NULL);

        /* Generation A publishes and dispatch observes it immediately. */
        struct zcl_command_handler_override ovr_a = {
            .path = "sim.leaf.repoint", .handler = hsim_leaf_h_gen_a,
        };
        why[0] = '\0';
        ASSERT(zcl_command_registry_replace_batch(0, &ovr_a, 1, why,
                                                  sizeof(why)));
        uint32_t gen_a = zcl_command_registry_active_generation();
        ASSERT(gen_a > 0);
        ASSERT_EQ((int)hsim_leaf_dispatch(out, sizeof(out)),
                  (int)ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"gen\":\"A\"") != NULL);

        /* Generation B atomically re-points the SAME leaf path — no
         * restart, no intermediate torn state observable by dispatch. */
        struct zcl_command_handler_override ovr_b = {
            .path = "sim.leaf.repoint", .handler = hsim_leaf_h_gen_b,
        };
        why[0] = '\0';
        ASSERT(zcl_command_registry_replace_batch(0, &ovr_b, 1, why,
                                                  sizeof(why)));
        uint32_t gen_b = zcl_command_registry_active_generation();
        ASSERT(gen_b > gen_a);
        ASSERT_EQ((int)hsim_leaf_dispatch(out, sizeof(out)),
                  (int)ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"gen\":\"B\"") != NULL);

        /* Generation monotonicity: a generation <= the active one is
         * rejected outright and publishes nothing. */
        why[0] = '\0';
        ASSERT(!zcl_command_registry_replace_batch(gen_b, &ovr_a, 1, why,
                                                    sizeof(why)));
        ASSERT(strstr(why, "not newer") != NULL);
        ASSERT_EQ((unsigned)zcl_command_registry_active_generation(),
                  (unsigned)gen_b);
        ASSERT_EQ((int)hsim_leaf_dispatch(out, sizeof(out)),
                  (int)ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"gen\":\"B\"") != NULL);

        zcl_command_registry_reset_overrides();
        zcl_command_registry_set_active(NULL);
        PASS();
    } _test_next:;
    return failures;
}

int test_hotswap_simnet(void)
{
    printf("\n=== hot-swap v2 deterministic 3-node simnet ===\n");
    int failures = 0;
    uint64_t seed = 0;
    struct hsim_result first = {0};
    struct hsim_result replay = {0};
    uint64_t started_us = platform_time_monotonic_us();

    bool seed_ok = hsim_seed_from_env(&seed);
    HSIM_CHECK("simnet: replay seed parses", seed_ok);
    bool first_ok = seed_ok && hsim_run(seed, &first);
    HSIM_CHECK("simnet: first seeded generation transaction completes",
               first_ok);
    bool replay_ok = seed_ok && hsim_run(seed, &replay);
    HSIM_CHECK("simnet: same-seed replay completes", replay_ok);
    HSIM_CHECK("simnet: three in-memory nodes converge",
               first_ok && first.cluster_converged);
    HSIM_CHECK("simnet: rejected batch publishes zero providers",
               first_ok && first.rejected_without_publication);
    HSIM_CHECK("simnet: two providers publish in one generation",
               first_ok && first.atomic_commit_visible &&
               first.active_generation == HSIM_GENERATION);
    HSIM_CHECK("simnet: in-flight call finishes on old generation",
               first_ok && first.old_call_completed);
    HSIM_CHECK("simnet: new peer calls see only committed generation",
               first_ok && first.new_calls_visible);
    HSIM_CHECK("simnet: seed replay transcript is bit-identical",
               first_ok && replay_ok && hsim_results_equal(&first, &replay));

    uint64_t finished_us = platform_time_monotonic_us();
    uint64_t elapsed_us = finished_us >= started_us
                              ? finished_us - started_us : 0;
    if (first_ok) {
        printf("simnet: seed=0x%016" PRIx64
               " delivery=0x%016" PRIx64
               " transcript=0x%016" PRIx64
               " two_run_wall_us=%" PRIu64 "\n",
               seed, first.delivery_fingerprint,
               first.transcript_fingerprint, elapsed_us);
        printf("simnet: replay with ZCL_HOTSWAP_SIM_SEED=0x%016" PRIx64
               " make hotswap-sim\n", seed);
    }
    failures += test_hotswap_native_leaf_repoint();
    printf("=== hotswap_simnet: %d failures ===\n", failures);
    return failures;
}
