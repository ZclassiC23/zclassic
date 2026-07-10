/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Integration tests for the MCP domain controllers: verify every
 * controller registers its tools with well-formed metadata, no
 * duplicate names, and consistent domain labels.  These tests touch
 * the real tool registration code (they link the same controller .c
 * files as the live build/bin/zclassic23 -mcp binary).  Handler dispatch tests
 * use the ZCL_TESTING mcp_node_rpc hook instead of a running node.
 *
 * Coverage:
 *   1. mcp_register_* populate the router with the expected number of
 *      tools per domain and a correct total.
 *   2. Every registered tool has a non-null handler, description, and
 *      domain from the small known set.
 *   3. Every tool name starts with "zcl_" and is unique within the
 *      table.
 *   4. Schema generation (tools/list JSON, inputSchema per tool) is
 *      well-formed for real controller routes.
 *   5. Specific high-traffic tools exist with the expected parameter
 *      shape (zcl_getblock, zcl_status, zcl_kpi, zcl_self_test, ...).
 *   6. Reset leaves the table empty and re-registration restores it.
 */

#include "test/test_helpers.h"
#include "mcp/router.h"
#include "mcp/controllers.h"
#include "mcp/middleware.h"
#include "mcp/replay.h"
#include "mcp/rpc_params.h"
#include "mcp/rpc_client.h"
#include "controllers/agent_controller.h"
#include "event/event.h"
#include "json/json.h"
#include "sim/postmortem.h"
#include "sim/seed_tape.h"
#include "util/blocker.h"
#include "util/clientversion.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>
#include "util/safe_alloc.h"

/* Expected tool counts.  If a future commit intentionally adds or
 * removes tools, bump these numbers in the same commit — they are the
 * contract for "how big is the MCP surface." */
#define EXPECTED_TOTAL     138  /* +3 metrics baseline tools:
                                 *   zcl_metrics_baseline_set/_list/_diff
                                 *   (lane C2, "what changed since X");
                                 * +5 ZNAM write RPCs: zcl_name_update,
                                 *   zcl_name_transfer, zcl_name_renew,
                                 *   zcl_name_set_record, zcl_name_set_text;
                                 * +3 recovery: zcl_invalidateblock, zcl_reconsiderblock, zcl_rebuild_recent;
                                 * +3 power-user tools: chain_tip,
                                 * reorg_history, mempool_inspect;
                                 * +1 Round 6 C5: zcl_blockers;
                                 * +1 framework Phase 0: zcl_conditions;
                                 * +2 Phase 6b postmortem capsule tools
                                 * +1 offline replay verifier: zcl_replay_verify
                                 * +3 wait tools: zcl_waitforheight,
                                 *   zcl_waitforhalt, zcl_waitforblocker
                                 * +1 native milestone status: zcl_milestone
                                 * +1 native refold readiness: zcl_refold_status
                                 * +12 agent API tools: map, lanes, impact,
                                 *   contracts, build, dev_status, interface,
                                 *   ops, diagnose, liveness, deploy_guard,
                                 *   proof_bundle
                                 * +1 net bootstrapstatus
                                 * +1 net peer incident view
                                 * +1 state catalog: zcl_state_catalog
                                 * +1 app protocol catalog: zcl_app_protocols
                                 * +1 service catalog: zcl_service_catalog
                                 * +1 service operation catalog: zcl_service_operations
                                 * +1 semantic timeline: zcl_timeline
                                 * +1 native operator snapshot
                                 * +2 wallet backup tools: zcl_wallet_backup_status,
                                 *   zcl_wallet_backup_now
                                 * +1 wallet receive intent
                                 * +1 async copy-prove launch:
                                 *   zcl_agent_copy_prove (destructive-tier)
                                 * +1 async test launch: zcl_agent_test
                                 *   (destructive-tier) */
#define EXPECTED_OPS        64  /* + zcl_metrics_baseline_set/_list/_diff
                                 *   (lane C2, tools/mcp/baseline.c);
                                 * + zcl_agent_copy_prove (async copy-prove);
                                 * + zcl_agent_test (async test launch);
                                 * + zcl_rebuild_recent (bounded recovery);
                                 * status, health, kpi, self_heal_stats, mempool*, mininginfo,
                                 * benchmark, dbstats, filemanifest, events,
                                 * rpc, state + node_log + sql (round 6.5 MCP primitives),
                                 * tools_list, self_test, logtail,
                                 * openapi, metrics, metrics_reset,
                                 * rpc_report (wave 5 sess 1),
                                 * admin (wave 5 #5),
                                 * profile (wave 6),
                                 * config_reload (wave 6),
                                 * consensus_report (wave 8),
                                 * syncdiag, replay_dump, replay_exec,
                                 * + mirror status and zclassicd probe,
                                 * + mempool_inspect (fee+age histograms)
                                 * + zcl_postmortem_list/replay (Phase 6b)
                                 * + zcl_operator_summary,
                                 *   zcl_operator_snapshot + zcl_agent
                                 *   (simple MCP status)
                                 * + zcl_refold_status
                                 * +10 zcl_agent_* development/proof tools
                                 * + zcl_app_protocols
                                 * + zcl_service_catalog
                                 * + zcl_service_operations
                                 * + zcl_timeline */
#define EXPECTED_CHAIN      19  /* + chain_tip + reorg_history
                                 * + zcl_replay_verify (offline replay verifier)
                                 * + zcl_invalidateblock + zcl_reconsiderblock (recovery)
                                 * + zcl_waitforheight + zcl_waitforhalt
                                 *   + zcl_waitforblocker (wait tools) */
#define EXPECTED_NET        11  /* + zcl_peer_report (wave 4 #5),
                                 * + zcl_onion_health (wave 6 #7),
                                 * + zcl_bootstrapstatus
                                 * + zcl_peer_incidents */
#define EXPECTED_WALLET     23
#define EXPECTED_APP        21  /* +5 ZNAM write RPCs (name_update,
                                 * name_transfer, name_renew, name_set_record,
                                 * name_set_text) */
#define EXPECTED_HEADROOM   32

/* ── Helpers ────────────────────────────────────────────────── */

static void register_all(void)
{
    mcp_router_reset();
    mcp_register_ops();
    mcp_register_diagnostics();
    mcp_register_chain();
    mcp_register_net();
    mcp_register_wallet();
    mcp_register_app();
    mcp_register_meta();
}

static size_t count_by_domain(const char *domain)
{
    size_t n = 0;
    for (size_t i = 0; i < mcp_router_count(); i++) {
        const struct mcp_tool_route *r = mcp_router_at(i);
        if (r && r->domain && strcmp(r->domain, domain) == 0)
            n++;
    }
    return n;
}

static bool is_known_domain(const char *d)
{
    if (!d) return false;
    return strcmp(d, "ops")    == 0 ||
           strcmp(d, "chain")  == 0 ||
           strcmp(d, "net")    == 0 ||
           strcmp(d, "wallet") == 0 ||
           strcmp(d, "app")    == 0;
}

static bool contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static bool json_array_has_str(const struct json_value *arr,
                               const char *value)
{
    if (!arr || arr->type != JSON_ARR || !value)
        return false;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *child = json_at(arr, i);
        if (child && strcmp(json_get_str(child), value) == 0)
            return true;
    }
    return false;
}

static bool mcp_test_exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    bool ok = sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK;
    if (err)
        sqlite3_free(err);
    return ok;
}

static bool seed_mcp_projection_height(const char *dir, int64_t height)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/node.db", dir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return false;
    }

    char insert_sql[160];
    snprintf(insert_sql, sizeof(insert_sql),
             "INSERT INTO blocks(height,status) VALUES (%lld,3)",
             (long long)height);
    bool ok =
        mcp_test_exec_sql(db,
            "CREATE TABLE blocks(height INTEGER NOT NULL, status INTEGER NOT NULL)") &&
        mcp_test_exec_sql(db, insert_sql);
    ok = sqlite3_close(db) == SQLITE_OK && ok;
    return ok;
}

static bool g_name_list_rpc_called;
static bool g_name_list_rpc_params_null;

static char *mock_name_list_rpc(const char *method, const char *params_json)
{
    g_name_list_rpc_called = true;
    g_name_list_rpc_params_null = params_json == NULL;

    if (strcmp(method, "name_list") != 0)
        return strdup("{\"error\":{\"code\":-32601,"
                      "\"message\":\"unexpected method\"}}");

    return strdup("{\"schema\":\"zcl.names.index.v1\","
                  "\"limit\":100,\"count\":1,\"filtered\":false,"
                  "\"names\":[{\"name\":\"alice\",\"owner\":\"t1owner\","
                  "\"target_type\":1,\"type\":\"onion\","
                  "\"value\":\"aliceexample.onion:8033\"}],"
                  "\"_links\":{\"collection\":\"/api/v1/names\","
                  "\"read\":\"/api/v1/names/{name}\"},"
                  "\"zcl_verification\":{\"base_layer\":\"zclassic_l1\","
                  "\"consensus_boundary\":"
                  "\"legacy_zclassic_consensus_unchanged\"}}");
}

/* ── Tests ──────────────────────────────────────────────────── */

static int test_register_total_count(void)
{
    int failures = 0;
    TEST("controllers: total tool count matches expected surface") {
        register_all();
        size_t n = mcp_router_count();
        if (n != EXPECTED_TOTAL) {
            printf("FAIL (got %zu, expected %d)\n", n, EXPECTED_TOTAL);
            failures++; goto _test_next;
        }
        if (mcp_router_capacity() < EXPECTED_TOTAL + EXPECTED_HEADROOM) {
            printf("FAIL (capacity=%zu, expected at least %d total + %d headroom)\n",
                   mcp_router_capacity(), EXPECTED_TOTAL, EXPECTED_HEADROOM);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_ops_domain_count(void)
{
    int failures = 0;
    TEST("controllers: ops domain includes self-heal stats tool") {
        register_all();
        size_t n = count_by_domain("ops");
        if (n != EXPECTED_OPS) {
            printf("FAIL (ops=%zu, expected %d)\n", n, EXPECTED_OPS);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_chain_domain_count(void)
{
    int failures = 0;
    TEST("controllers: chain domain has EXPECTED_CHAIN tools") {
        register_all();
        size_t n = count_by_domain("chain");
        if (n != EXPECTED_CHAIN) {
            printf("FAIL (chain=%zu, expected %d)\n", n, EXPECTED_CHAIN);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_net_domain_count(void)
{
    int failures = 0;
    TEST("controllers: net domain has EXPECTED_NET tools") {
        register_all();
        size_t n = count_by_domain("net");
        if (n != EXPECTED_NET) {
            printf("FAIL (net=%zu, expected %d)\n", n, EXPECTED_NET);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_wallet_domain_count(void)
{
    int failures = 0;
    TEST("controllers: wallet domain has 23 tools") {
        register_all();
        size_t n = count_by_domain("wallet");
        if (n != EXPECTED_WALLET) {
            printf("FAIL (wallet=%zu, expected %d)\n", n, EXPECTED_WALLET);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_app_domain_count(void)
{
    int failures = 0;
    TEST("controllers: app domain has 16 tools") {
        register_all();
        size_t n = count_by_domain("app");
        if (n != EXPECTED_APP) {
            printf("FAIL (app=%zu, expected %d)\n", n, EXPECTED_APP);
            failures++; goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_has_handler(void)
{
    int failures = 0;
    TEST("controllers: every registered tool has a non-null handler") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            ASSERT(r->handler != NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_has_description(void)
{
    int failures = 0;
    TEST("controllers: every tool has a non-empty description") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            ASSERT(r->description != NULL);
            ASSERT(r->description[0] != 0);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_tool_descriptions_do_not_claim_zclassicd_authority(void)
{
    int failures = 0;
    TEST("controllers: tool descriptions do not claim zclassicd authority") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            ASSERT(r->description != NULL);
            ASSERT(!contains(r->description, "authoritative local "
                             "zclassicd"));
        }
        const struct mcp_tool_route *rebuild =
            mcp_router_find("zcl_rebuild_recent");
        ASSERT(rebuild != NULL);
        ASSERT(contains(rebuild->description, "legacy advisory source"));
        ASSERT(contains(rebuild->description,
                        "local consensus validation"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_has_known_domain(void)
{
    int failures = 0;
    TEST("controllers: every tool has a known domain label") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            if (!is_known_domain(r->domain)) {
                printf("FAIL (%s has domain=%s)\n",
                       r->name, r->domain ? r->domain : "(null)");
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_every_tool_name_prefixed(void)
{
    int failures = 0;
    TEST("controllers: every tool name starts with zcl_") {
        register_all();
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            ASSERT(r->name != NULL);
            if (strncmp(r->name, "zcl_", 4) != 0) {
                printf("FAIL (%s is not zcl_-prefixed)\n", r->name);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_no_duplicate_names(void)
{
    int failures = 0;
    TEST("controllers: no duplicate tool names across all domains") {
        register_all();
        size_t n = mcp_router_count();
        for (size_t i = 0; i < n; i++) {
            const struct mcp_tool_route *a = mcp_router_at(i);
            ASSERT(a != NULL);
            for (size_t j = i + 1; j < n; j++) {
                const struct mcp_tool_route *b = mcp_router_at(j);
                ASSERT(b != NULL);
                if (strcmp(a->name, b->name) == 0) {
                    printf("FAIL (duplicate %s)\n", a->name);
                    failures++; goto _test_next;
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_specific_flagship_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: flagship tools registered") {
        register_all();
        /* Canon set — documented in CLAUDE.md.  If any goes missing,
         * the compat contract is broken. */
        const char *k[] = {
            "zcl_agent", "zcl_status", "zcl_operator_summary",
            "zcl_operator_snapshot",
            "zcl_agent_map", "zcl_agent_lanes", "zcl_agent_impact",
            "zcl_agent_contracts", "zcl_agent_build",
            "zcl_agent_dev_status", "zcl_agent_interface", "zcl_agent_ops",
            "zcl_agent_diagnose", "zcl_agent_liveness",
            "zcl_agent_deploy_guard", "zcl_proof_bundle",
            "zcl_app_protocols",
            "zcl_service_catalog",
            "zcl_milestone", "zcl_refold_status", "zcl_kpi", "zcl_health",
            "zcl_getblockcount", "zcl_getblock", "zcl_getblockchaininfo",
            "zcl_peers", "zcl_networkinfo", "zcl_peer_incidents",
            "zcl_bootstrapstatus", "zcl_onion_status",
            "zcl_balance", "zcl_send", "zcl_getnewaddress",
            "zcl_z_getnewaddress",
            "zcl_name_resolve", "zcl_msg_send",
            "zcl_swap_chains", "zcl_market_list",
            "zcl_tools_list", "zcl_self_test", "zcl_logtail",
            "zcl_rpc", "zcl_state_catalog", "zcl_timeline",
            "zcl_postmortem_list", "zcl_postmortem_replay",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            if (mcp_router_find(k[i]) == NULL) {
                printf("FAIL (missing %s)\n", k[i]);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_postmortem_tools_dispatch(void)
{
    int failures = 0;
    TEST("controllers: postmortem list/replay dispatch over MCP") {
        register_all();

        char dir_template[128];
        snprintf(dir_template, sizeof(dir_template),
                 "/tmp/zcl_mcp_postmortem_%d_XXXXXX", (int)getpid());
        char *dir = mkdtemp(dir_template);
        ASSERT(dir != NULL);

        seed_tape_t *tape = seed_tape_open(0xBADCAFEULL, 1779667000);
        ASSERT(tape != NULL);
        ASSERT(seed_tape_advance(tape, 5000) == 0);
        ASSERT(seed_tape_inject(tape, 9, "abc", 3) == 0);

        char capsule_path[512];
        struct postmortem_capture_opts opts = {
            .dir = dir,
            .tape = tape,
            .crash_signal = 11,
            .crash_unix = 1779667999,
            .reason = "mcp-test",
            .log_path = NULL,
        };
        ASSERT(postmortem_capture_write(&opts, capsule_path,
                                        sizeof(capsule_path)) == 0);

        char args_src[768];
        snprintf(args_src, sizeof(args_src),
                 "{\"dir\":\"%s\",\"limit\":10}", dir);
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));
        char *body = mcp_router_dispatch("zcl_postmortem_list", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"total\":1"));
        ASSERT(contains(body, "\"returned\":1"));
        ASSERT(contains(body, "\"crash_signal\":11"));
        ASSERT(contains(body, "1779667999"));
        free(body);
        json_free(&args);

        snprintf(args_src, sizeof(args_src),
                 "{\"path\":\"%s\",\"max_events\":10}", capsule_path);
        ASSERT(json_read(&args, args_src, strlen(args_src)));
        body = mcp_router_dispatch("zcl_postmortem_replay", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"returned\":1"));
        ASSERT(contains(body, "\"type\":9"));
        ASSERT(contains(body, "\"payload_hex\":\"616263\""));
        free(body);
        json_free(&args);

        seed_tape_close(tape);
        test_rm_rf_recursive(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* Security regression: zcl_replay_exec re-dispatches through
 * mcp_router_dispatch, which bypasses the middleware auth-tier and
 * destructive rate-limit checks. Replaying a destructive tool must be
 * refused so a normal-tier caller can't launder a destructive action
 * through the replay buffer, and zcl_replay_exec itself must be flagged
 * destructive so the middleware gates it. */
static int test_zcl_replay_exec_refuses_destructive(void)
{
    int failures = 0;
    TEST("replay_exec is destructive-flagged and refuses destructive replay") {
        register_all();

        /* zcl_replay_exec itself is gated by the destructive tier. */
        const struct mcp_tool_route *self = mcp_router_find("zcl_replay_exec");
        ASSERT(self != NULL);
        ASSERT(self->flags & MCP_TOOL_FLAG_DESTRUCTIVE);

        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        ASSERT(mcp_middleware_is_destructive(&mw, "zcl_replay_exec"));

        /* Seed the replay ring with a destructive tool call, then try to
         * replay it — it must be refused, not re-executed. */
        mcp_replay_init();
        mcp_replay_record("zcl_metrics_reset", "{}", "{}", 100, false);
        size_t n = mcp_replay_count();
        ASSERT(n >= 1);

        char args_src[64];
        snprintf(args_src, sizeof(args_src), "{\"index\":%zu}", n - 1);
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));
        char *body = mcp_router_dispatch("zcl_replay_exec", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "AUTH_REQUIRED"));
        ASSERT(contains(body, "destructive"));
        free(body);
        json_free(&args);

        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_getblock_param_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_getblock has required block_id + optional verbosity") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_getblock");
        ASSERT(r != NULL);
        ASSERT(r->num_params == 2);
        ASSERT(strcmp(r->params[0].name, "block_id") == 0);
        ASSERT(r->params[0].required == true);
        ASSERT(r->params[0].type == MCP_PARAM_STR);
        ASSERT(strcmp(r->params[1].name, "verbosity") == 0);
        ASSERT(r->params[1].required == false);
        ASSERT(r->params[1].type == MCP_PARAM_INT);
        ASSERT(r->params[1].default_json != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_status_no_params(void)
{
    int failures = 0;
    TEST("controllers: zcl_status takes no parameters") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_status");
        ASSERT(r != NULL);
        ASSERT(r->num_params == 0);
        ASSERT(strcmp(r->domain, "ops") == 0);
        ASSERT(contains(r->description, "chain advance source scoring"));
        PASS();
    } _test_next:;
    return failures;
}

static int g_target_only_state_rpc_calls;

static char *mock_target_only_state_rpc(const char *method,
                                        const char *params_json)
{
    if (method && params_json && strcmp(method, "dumpstate") == 0 &&
        strcmp(params_json, "[\"target_only_future_subsystem\"]") == 0) {
        g_target_only_state_rpc_calls++;
        return strdup("{\"subsystem\":\"target_only_future_subsystem\","
                      "\"captured_at\":1782240014,"
                      "\"state\":{\"generation\":2}}");
    }
    return strdup("{\"code\":-32601,\"message\":\"unexpected RPC\"}");
}

static int test_zcl_state_catalog_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_state_catalog is discoverable no-param catalog") {
        register_all();
        const struct mcp_tool_route *catalog =
            mcp_router_find("zcl_state_catalog");
        const struct mcp_tool_route *state = mcp_router_find("zcl_state");
        ASSERT(catalog != NULL);
        ASSERT(state != NULL);
        ASSERT(strcmp(catalog->domain, "ops") == 0);
        ASSERT(catalog->num_params == 0);
        ASSERT(contains(catalog->description, "diagnostics registry"));
        ASSERT(state->num_params >= 1);
        ASSERT(state->params[0].enum_csv != NULL);
        ASSERT(contains(state->params[0].enum_csv, "reducer_frontier"));
        ASSERT(contains(state->params[0].enum_csv, "block_index"));
        ASSERT(contains(state->params[0].description,
                        "target zcl_state_catalog is authoritative"));
        ASSERT(state->flags & MCP_TOOL_FLAG_ADVISORY_ENUMS);

        char schema[8192];
        size_t schema_len =
            mcp_router_input_schema_json(state, schema, sizeof(schema));
        ASSERT(schema_len > 0);
        ASSERT(contains(schema, "\"x-advisoryEnum\":"));
        ASSERT(!contains(schema, "\"enum\":"));

        g_target_only_state_rpc_calls = 0;
        mcp_rpc_client_set_test_hook(mock_target_only_state_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        json_push_kv_str(&args, "subsystem",
                         "target_only_future_subsystem");
        char *body = mcp_router_dispatch("zcl_state", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT(g_target_only_state_rpc_calls == 1);
        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "subsystem")),
                      "target_only_future_subsystem");
        ASSERT(json_get_int(json_get(json_get(&root, "state"),
                                     "generation")) == 2);
        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_agent_dev_tools_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_agent_* development tools have stable shapes") {
        register_all();
        const struct mcp_tool_route *agent =
            mcp_router_find("zcl_agent");
        const struct mcp_tool_route *map =
            mcp_router_find("zcl_agent_map");
        const struct mcp_tool_route *impact =
            mcp_router_find("zcl_agent_impact");
        const struct mcp_tool_route *lanes =
            mcp_router_find("zcl_agent_lanes");
        const struct mcp_tool_route *contracts =
            mcp_router_find("zcl_agent_contracts");
        const struct mcp_tool_route *build =
            mcp_router_find("zcl_agent_build");
        const struct mcp_tool_route *interface =
            mcp_router_find("zcl_agent_interface");
        const struct mcp_tool_route *ops =
            mcp_router_find("zcl_agent_ops");
        const struct mcp_tool_route *liveness =
            mcp_router_find("zcl_agent_liveness");
        const struct mcp_tool_route *deploy_guard =
            mcp_router_find("zcl_agent_deploy_guard");
        const struct mcp_tool_route *proof_bundle =
            mcp_router_find("zcl_proof_bundle");
        const struct mcp_tool_route *app_protocols =
            mcp_router_find("zcl_app_protocols");
        const struct mcp_tool_route *service_catalog =
            mcp_router_find("zcl_service_catalog");
        const struct mcp_tool_route *service_operations =
            mcp_router_find("zcl_service_operations");
        ASSERT(agent != NULL);
        ASSERT(map != NULL);
        ASSERT(lanes != NULL);
        ASSERT(impact != NULL);
        ASSERT(contracts != NULL);
        ASSERT(build != NULL);
        ASSERT(interface != NULL);
        ASSERT(ops != NULL);
        ASSERT(liveness != NULL);
        ASSERT(deploy_guard != NULL);
        ASSERT(proof_bundle != NULL);
        ASSERT(app_protocols != NULL);
        ASSERT(service_catalog != NULL);
        ASSERT(service_operations != NULL);
        ASSERT(strcmp(agent->domain, "ops") == 0);
        ASSERT(strcmp(map->domain, "ops") == 0);
        ASSERT(strcmp(lanes->domain, "ops") == 0);
        ASSERT(strcmp(impact->domain, "ops") == 0);
        ASSERT(strcmp(contracts->domain, "ops") == 0);
        ASSERT(strcmp(build->domain, "ops") == 0);
        ASSERT(strcmp(interface->domain, "ops") == 0);
        ASSERT(strcmp(ops->domain, "ops") == 0);
        ASSERT(strcmp(liveness->domain, "ops") == 0);
        ASSERT(strcmp(deploy_guard->domain, "ops") == 0);
        ASSERT(strcmp(proof_bundle->domain, "ops") == 0);
        ASSERT(strcmp(app_protocols->domain, "ops") == 0);
        ASSERT(strcmp(service_catalog->domain, "ops") == 0);
        ASSERT(strcmp(service_operations->domain, "ops") == 0);
        ASSERT(agent->num_params == 0);
        ASSERT(map->num_params == 0);
        ASSERT(contracts->num_params == 0);
        ASSERT(build->num_params == 0);
        ASSERT(interface->num_params == 0);
        ASSERT(proof_bundle->num_params == 1);
        ASSERT(strcmp(proof_bundle->params[0].name,
                      "anchor_datadir") == 0);
        ASSERT(proof_bundle->params[0].type == MCP_PARAM_STR);
        ASSERT(proof_bundle->params[0].required == false);
        ASSERT(service_catalog->num_params == 1);
        ASSERT(strcmp(service_catalog->params[0].name, "name") == 0);
        ASSERT(service_catalog->params[0].type == MCP_PARAM_STR);
        ASSERT(service_catalog->params[0].required == false);
        ASSERT_STR_EQ(service_catalog->params[0].default_json, "\"\"");
        ASSERT(service_catalog->self_test_args != NULL);
        ASSERT(service_operations->num_params == 6);
        ASSERT(strcmp(service_operations->params[0].name,
                      "operation_id") == 0);
        ASSERT(service_operations->params[0].type == MCP_PARAM_STR);
        ASSERT(service_operations->params[0].required == false);
        ASSERT_STR_EQ(service_operations->params[0].default_json, "\"\"");
        ASSERT_STR_EQ(service_operations->params[1].name, "service");
        ASSERT(service_operations->params[1].type == MCP_PARAM_STR);
        ASSERT_STR_EQ(service_operations->params[2].name, "write_safety");
        ASSERT(service_operations->params[2].enum_csv != NULL);
        ASSERT(strstr(service_operations->params[2].enum_csv,
                      "public_read_only") != NULL);
        ASSERT_STR_EQ(service_operations->params[3].name,
                      "preferred_interface");
        ASSERT_STR_EQ(service_operations->params[4].name, "status");
        ASSERT_STR_EQ(service_operations->params[5].name, "surface");
        ASSERT(service_operations->self_test_args != NULL);
        ASSERT(ops->num_params == 0);
        ASSERT(liveness->num_params == 1);
        ASSERT(strcmp(liveness->params[0].name, "mode") == 0);
        ASSERT(liveness->params[0].type == MCP_PARAM_STR);
        ASSERT(liveness->params[0].required == false);
        ASSERT_STR_EQ(liveness->params[0].default_json, "\"brief\"");
        ASSERT(liveness->self_test_args != NULL);
        ASSERT(app_protocols->num_params == 0);
        ASSERT(contains(liveness->description, "liveness"));
        ASSERT(contains(app_protocols->description, "protocol catalog"));
        ASSERT(impact->num_params == 1);
        ASSERT(strcmp(impact->params[0].name, "files") == 0);
        ASSERT(impact->params[0].type == MCP_PARAM_ARRAY);
        ASSERT(impact->params[0].required == false);
        ASSERT(impact->params[0].default_json != NULL);
        ASSERT(impact->self_test_args != NULL);
        const struct agent_contract *agent_contract =
            agent_contract_lookup("agent");
        const struct agent_contract *ops_contract =
            agent_contract_lookup("agentops");
        ASSERT(agent_contract != NULL);
        ASSERT(ops_contract != NULL);
        ASSERT_STR_EQ(agent->description, agent_contract->purpose);
        ASSERT_STR_EQ(ops->description, ops_contract->purpose);
        ASSERT(contains(impact->description, "focused validation"));
        ASSERT(deploy_guard->num_params == 1);
        ASSERT(strcmp(deploy_guard->params[0].name, "action") == 0);
        ASSERT(deploy_guard->params[0].type == MCP_PARAM_STR);
        ASSERT(deploy_guard->params[0].required == false);
        ASSERT(deploy_guard->params[0].default_json != NULL);
        ASSERT(contains(deploy_guard->description, "allow/refuse"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_agent_contract_mcp_registry_coverage(void)
{
    int failures = 0;
    TEST("controllers: every agent contract MCP tool is registered") {
        register_all();
        size_t declared_mcp = 0;
        for (size_t i = 0; i < agent_contract_count(); i++) {
            const struct agent_contract *c = agent_contract_at(i);
            ASSERT(c != NULL);
            if (!c->mcp_tool || !c->mcp_tool[0])
                continue;
            declared_mcp++;
            const struct mcp_tool_route *route =
                mcp_router_find(c->mcp_tool);
            if (!route) {
                printf("FAIL (missing MCP route for %s -> %s)\n",
                       c->method ? c->method : "(null)", c->mcp_tool);
                failures++; goto _test_next;
            }
            ASSERT(route->description != NULL);
        }
        ASSERT(declared_mcp >= 20);
        PASS();
    } _test_next:;
    return failures;
}

static int test_postmortem_tools_list_and_replay(void)
{
    int failures = 0;
    TEST("controllers: postmortem tools list capsules and replay events") {
        register_all();
        const struct mcp_tool_route *list =
            mcp_router_find("zcl_postmortem_list");
        const struct mcp_tool_route *replay =
            mcp_router_find("zcl_postmortem_replay");
        ASSERT(list != NULL);
        ASSERT(replay != NULL);
        ASSERT(strcmp(list->domain, "ops") == 0);
        ASSERT(strcmp(replay->domain, "ops") == 0);
        ASSERT(list->num_params == 2);
        ASSERT(replay->num_params == 2);
        ASSERT(strcmp(list->params[0].name, "dir") == 0);
        ASSERT(list->params[0].required == false);
        ASSERT(strcmp(list->params[1].name, "limit") == 0);
        ASSERT(strcmp(replay->params[0].name, "path") == 0);
        ASSERT(replay->params[0].required == true);
        ASSERT(strcmp(replay->params[1].name, "limit") == 0);

        char dir_template[128];
        snprintf(dir_template, sizeof(dir_template),
                 "/tmp/zcl_mcp_postmortem_%d_XXXXXX", (int)getpid());
        char *dir = mkdtemp(dir_template);
        ASSERT(dir != NULL);

        seed_tape_t *tape = seed_tape_open(0xfeed1234ULL, 1779667000);
        ASSERT(tape != NULL);
        ASSERT(seed_tape_inject(tape, 7, "abc", 3) == 0);

        char cap_path[512];
        struct postmortem_capture_opts opts = {
            .dir = dir,
            .tape = tape,
            .crash_signal = 11,
            .crash_unix = 1779667123,
            .reason = "mcp-test",
            .log_path = NULL,
        };
        ASSERT(postmortem_capture_write(&opts, cap_path,
                                        sizeof(cap_path)) == 0);
        char old_cap_path[512];
        opts.crash_unix = 1779667001;
        opts.crash_signal = 6;
        opts.reason = "mcp-test-older";
        ASSERT(postmortem_capture_write(&opts, old_cap_path,
                                        sizeof(old_cap_path)) == 0);
        seed_tape_close(tape);

        char list_args_src[768];
        snprintf(list_args_src, sizeof(list_args_src),
                 "{\"dir\":\"%s\",\"limit\":1}", dir);
        struct json_value list_args;
        json_init(&list_args);
        ASSERT(json_read(&list_args, list_args_src, strlen(list_args_src)));

        char *list_body = mcp_router_dispatch("zcl_postmortem_list",
                                              &list_args);
        ASSERT(list_body != NULL);
        ASSERT(strstr(list_body, "\"error\":{") == NULL);

        struct json_value list_root;
        json_init(&list_root);
        ASSERT(json_read(&list_root, list_body, strlen(list_body)));
        const struct json_value *total = json_get(&list_root, "total");
        ASSERT(total != NULL);
        ASSERT(json_get_int(total) == 2);
        const struct json_value *returned = json_get(&list_root, "returned");
        ASSERT(returned != NULL);
        ASSERT(json_get_int(returned) == 1);
        const struct json_value *capsules = json_get(&list_root, "capsules");
        ASSERT(capsules != NULL);
        ASSERT(capsules->type == JSON_ARR);
        ASSERT(capsules->num_children == 1);
        const struct json_value *first = json_at(capsules, 0);
        ASSERT(first != NULL);
        const struct json_value *path_v = json_get(first, "path");
        ASSERT(path_v != NULL);
        ASSERT_STR_EQ(json_get_str(path_v), cap_path);
        json_free(&list_root);
        free(list_body);
        json_free(&list_args);

        char replay_args_src[768];
        snprintf(replay_args_src, sizeof(replay_args_src),
                 "{\"path\":\"%s\",\"limit\":5}", cap_path);
        struct json_value replay_args;
        json_init(&replay_args);
        ASSERT(json_read(&replay_args, replay_args_src,
                         strlen(replay_args_src)));

        char *replay_body = mcp_router_dispatch("zcl_postmortem_replay",
                                                &replay_args);
        ASSERT(replay_body != NULL);
        ASSERT(strstr(replay_body, "\"error\":{") == NULL);

        struct json_value replay_root;
        json_init(&replay_root);
        ASSERT(json_read(&replay_root, replay_body, strlen(replay_body)));
        const struct json_value *events = json_get(&replay_root, "events");
        ASSERT(events != NULL);
        ASSERT(events->type == JSON_ARR);
        ASSERT(events->num_children == 1);
        const struct json_value *ev = json_at(events, 0);
        ASSERT(ev != NULL);
        const struct json_value *type_v = json_get(ev, "type");
        const struct json_value *len_v = json_get(ev, "payload_len");
        const struct json_value *hex_v = json_get(ev, "payload_hex");
        ASSERT(type_v != NULL);
        ASSERT(len_v != NULL);
        ASSERT(hex_v != NULL);
        ASSERT(json_get_int(type_v) == 7);
        ASSERT(json_get_int(len_v) == 3);
        ASSERT_STR_EQ(json_get_str(hex_v), "616263");

        json_free(&replay_root);
        free(replay_body);
        json_free(&replay_args);
        test_rm_rf_recursive(dir);
        PASS();
    } _test_next:;
    return failures;
}

static bool mock_is_blocker_dump(const char *method, const char *params_json)
{
    return method && strcmp(method, "dumpstate") == 0 && params_json &&
           strcmp(params_json, "[\"blocker\"]") == 0;
}

static char *mock_empty_blocker_dump(void)
{
    return strdup("{\"subsystem\":\"blocker\","
                  "\"captured_at\":1782240005,"
                  "\"state\":{\"active_count\":0,"
                  "\"permanent_count\":0,\"transient_count\":0,"
                  "\"dependency_count\":0,\"resource_count\":0,"
                  "\"escape_dispatched_total\":0,\"rate_limit_ms\":1000,"
                  "\"blockers\":[],"
                  "\"_health\":{\"ok\":true,\"reason\":\"\"}}}");
}

static char *mock_target_blocker_dump(void)
{
    return strdup("{\"subsystem\":\"blocker\","
                  "\"captured_at\":1782240010,"
                  "\"state\":{\"active_count\":2,"
                  "\"permanent_count\":0,\"transient_count\":1,"
                  "\"dependency_count\":0,\"resource_count\":1,"
                  "\"escape_dispatched_total\":7,\"rate_limit_ms\":250,"
                  "\"blockers\":[{"
                  "\"id\":\"target-peer-slow\",\"owner\":\"net\","
                  "\"class\":\"transient\",\"age_us\":9000000,"
                  "\"deadline_remaining_us\":21000000,"
                  "\"escape_action\":\"retry_peer\","
                  "\"retry_count\":1,\"retry_budget\":3,"
                  "\"fire_count\":4,\"reason\":\"peer timeout\"},{"
                  "\"id\":\"target-disk-full\",\"owner\":\"storage\","
                  "\"class\":\"resource\",\"age_us\":5000000,"
                  "\"deadline_remaining_us\":0,"
                  "\"escape_action\":\"page_operator\","
                  "\"retry_count\":0,\"retry_budget\":0,"
                  "\"fire_count\":2,"
                  "\"reason\":\"disk \\\"full\\\"\\nmanual check\"}],"
                  "\"_health\":{\"ok\":false,"
                  "\"reason\":\"2 active target blockers\"}}}");
}

static char *mock_operator_healthy_rpc(const char *method,
                                       const char *params_json);

/* ISO C caps a single string constant at 4095 chars, and the full node
 * build compiles tests with -pedantic -Werror(-Woverlength-strings) — the
 * ~5.6KB fixture is stored as two halves and joined at runtime via
 * native_operator_snapshot_json_dup(). */
static const char k_native_operator_snapshot_json_p1[] =
    "{\"schema\":\"zcl.operator_snapshot.v1\",\"schema_version\":1,"
    "\"api_version\":\"v1\",\"execution_locus\":\"target_node\","
    "\"producer\":\"event_operator_snapshot_controller\","
    "\"authority\":\"target_node_internal_state\","
    "\"trust\":\"target_owned_evidence\","
    "\"build_commit\":\"nativecafe1\",\"network\":\"main\","
    "\"process_id\":42,\"node_instance_id\":\"fixture-node-a1\","
    "\"identity_initialized_at_unix_us\":999000,"
    "\"snapshot_sequence\":9,"
    "\"status\":\"healthy\",\"healthy\":true,"
    "\"verdict_complete\":true,\"primary_blocker\":\"none\","
    "\"next_action\":\"none\","
    "\"capture\":{\"model\":\"single_target_bounded_component_snapshots\","
    "\"globally_linearizable\":false,\"started_at_unix_us\":1000000,"
    "\"completed_at_unix_us\":1000100,\"duration_us\":100,"
    "\"component_skew_upper_bound_us\":100,\"attempts\":1,"
    "\"critical_frontier_stable\":true,"
    "\"verdict_inputs_complete\":true,\"partial\":false},"
    "\"chain\":{\"status\":\"ok\","
    "\"authority\":\"local_consensus_validation\","
    "\"trust\":\"authoritative\",\"authority_pair_known\":true,"
    "\"durable_authority_known\":true,"
    "\"authority_matches_served\":true,"
    "\"served_authority_source\":\"durable_tip_finalize_log\","
    "\"ancestry_known\":true,\"served_ancestor_indexed\":true,"
    "\"indexed_ancestor_header\":true,\"work_known\":true,"
    "\"work_monotone\":true,\"validity_known\":true,"
    "\"validity_sufficient\":true,\"failure_free\":true,"
    "\"consistent\":true,"
    "\"served\":{\"height_known\":true,\"binding_known\":true,"
    "\"status_known\":true,\"validity_sufficient\":true,"
    "\"failure_free\":true,\"height\":112,"
    "\"hash\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"chain_work\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"block_status\":13,\"source\":\"reducer_frontier_hstar\","
    "\"authority\":\"durable_tip_finalize_log\"},"
    "\"indexed\":{\"height_known\":true,\"binding_known\":true,"
    "\"status_known\":true,\"validity_sufficient\":true,"
    "\"failure_free\":true,\"height\":112,"
    "\"hash\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"chain_work\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"block_status\":13,\"source\":\"active_chain_window\","
    "\"authority\":\"raw_indexed_window\"},"
    "\"validated_header\":{\"height_known\":true,"
    "\"binding_known\":true,\"status_known\":true,"
    "\"validity_sufficient\":true,\"failure_free\":true,"
    "\"height\":112,"
    "\"hash\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"chain_work\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"block_status\":13,\"source\":\"pindex_best_header\","
    "\"authority\":\"local_header_validation\"},"
    "\"gap\":0,\"index_gap\":0},"
    "\"peers\":{\"known\":true,\"stale\":false,"
    "\"direction_known\":true,\"ready_known\":true,"
    "\"advertised_max_height_known\":true,\"status\":\"ok\","
    "\"authority\":\"live_connman_snapshot\","
    "\"peer_height_trust\":\"untrusted_peer_advertisement\","
    "\"generation\":3,\"age_seconds\":0,\"total\":1,"
    "\"inbound\":0,\"outbound\":1,\"ready\":1,"
    "\"advertised_max_height\":112},"
    "\"download\":{\"status\":\"ok\","
    "\"capture_model\":\"single_leaf_lock\",\"requested\":0,"
    "\"received\":0,\"timed_out\":0,\"in_flight\":0,"
    "\"queued\":0},"
    "\"blockers\":{\"known\":true,\"execution_locus\":\"target_node\","
    "\"authority\":\"typed_blocker_registry\","
    "\"trust\":\"authoritative_local_state\",\"generation\":4,"
    "\"active_count\":0,\"permanent_count\":0,\"transient_count\":0,"
    "\"dependency_count\":0,\"resource_count\":0,"
    "\"escape_dispatched_total\":0,\"rate_limit_ms\":12000,"
    "\"blockers\":[],\"dominant\":null},"
    "\"conditions\":{\"status\":\"ok\","
    "\"capture_model\":\"single_registry_pass_per_condition_atomic_fields\","
    "\"registered_count\":0,\"active_count\":0,"
    "\"unresolved_count\":0,\"unresolved_critical_count\":0},"
    "\"operator_latch\":{\"status\":\"ok\",\"active\":false,"
    "\"since_unix\":0,\"detail\":\"\",\"read_only_capture\":true},"
    "\"invariants\":{"
    "\"critical_frontier_stable\":{\"status\":\"pass\","
    "\"detail\":\"fixture chain tuple stable\"},"
    "\"frontier_order\":{\"status\":\"pass\","
    "\"detail\":\"served <= indexed <= header\"},"
    "\"chain_lineage_and_work\":{\"status\":\"pass\","
    "\"detail\":\"durable ancestry and work agree\"},"
    "\"frontier_validity\":{\"status\":\"pass\","
    "\"detail\":\"frontiers satisfy validation floors\"},"
    "\"blocker_counts\":{\"status\":\"pass\","
    "\"detail\":\"counts match entries\"},"
    "\"peer_direction_sum\":{\"status\":\"pass\","
    "\"detail\":\"directions sum to total\"}},";
static const char k_native_operator_snapshot_json_p2[] =
    "\"summary\":{\"schema\":\"zcl.operator_summary.v1\","
    "\"schema_version\":1,\"api_version\":\"v1\","
    "\"execution_locus\":\"target_node\","
    "\"source_rpc\":\"operatorsnapshot\",\"captured_at\":1,"
    "\"build_commit\":\"nativecafe1\",\"network\":\"main\","
    "\"process_id\":42,"
    "\"node_instance_id\":\"fixture-node-a1\","
    "\"identity_initialized_at_unix_us\":999000,"
    "\"snapshot_sequence\":9,\"capture_started_at_unix_us\":1000000,"
    "\"capture_completed_at_unix_us\":1000100,"
    "\"component_skew_upper_bound_us\":100,"
    "\"critical_frontier_stable\":true,\"atomic\":false,"
    "\"compatibility_fallback\":false,\"verdict_complete\":true,"
    "\"status\":\"healthy\",\"healthy\":true,\"serving\":true,"
    "\"operator_needed\":false,\"primary_blocker\":\"none\","
    "\"blocking_reason\":\"none\",\"next_action\":\"none\","
    "\"next_tool\":\"\",\"recommended_tools\":[],"
    "\"height\":112,\"served_height\":112,\"indexed_height\":112,"
    "\"header_height\":112,\"target_height\":112,"
    "\"target_height_source\":\"target_node.validated_header_tip\","
    "\"gap\":0,\"served_gap\":0,\"index_gap\":0,"
    "\"chain_evidence_consistent\":true,\"sync_state\":\"at_tip\","
    "\"active_conditions\":0,\"unresolved_conditions\":0,"
    "\"peers\":{\"known\":true,\"stale\":false,\"generation\":3,"
    "\"direction_known\":true,\"ready_known\":true,\"total\":1,"
    "\"inbound\":0,\"outbound\":1,\"ready\":1,\"max_height\":112,"
    "\"max_height_known\":true,"
    "\"max_height_trust\":\"untrusted_peer_advertisement\"},"
    "\"download\":{\"known\":true,\"in_flight\":0,\"queued\":0,"
    "\"sync_state\":\"at_tip\"},"
    "\"blockers\":{\"known\":true,\"execution_locus\":\"target_node\","
    "\"authority\":\"typed_blocker_registry\","
    "\"trust\":\"authoritative_local_state\",\"generation\":4,"
    "\"active_count\":0,\"permanent_count\":0,\"transient_count\":0,"
    "\"dependency_count\":0,\"resource_count\":0,"
    "\"escape_dispatched_total\":0,\"rate_limit_ms\":12000,"
    "\"blockers\":[],\"dominant\":null},"
    "\"summary\":\"healthy native snapshot\","
    "\"future_field\":{\"kept\":true}}}";

/* Join the two fixture halves into one malloc'd string (caller frees). */
static char *native_operator_snapshot_json_dup(void)
{
    size_t n1 = sizeof(k_native_operator_snapshot_json_p1) - 1u;
    size_t n2 = sizeof(k_native_operator_snapshot_json_p2) - 1u;
    char *s = malloc(n1 + n2 + 1u);
    if (!s)
        return NULL;
    memcpy(s, k_native_operator_snapshot_json_p1, n1);
    memcpy(s + n1, k_native_operator_snapshot_json_p2, n2);
    s[n1 + n2] = '\0';
    return s;
}

static int g_native_operator_snapshot_calls;
static int g_native_operator_legacy_calls;

static char *mock_native_operator_rpc(const char *method,
                                      const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "operatorsnapshot") == 0) {
        g_native_operator_snapshot_calls++;
        return native_operator_snapshot_json_dup();
    }
    g_native_operator_legacy_calls++;
    return mock_operator_healthy_rpc(method, params_json);
}

enum mock_native_operator_failure_mode {
    MOCK_NATIVE_INVALID_JSON,
    MOCK_NATIVE_INTERNAL_ERROR,
    MOCK_NATIVE_MIXED_METHOD_NOT_FOUND,
    MOCK_NATIVE_WRONG_VERSION,
};

static enum mock_native_operator_failure_mode g_native_operator_failure_mode;

static char *mock_native_operator_failure_rpc(const char *method,
                                              const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "operatorsnapshot") != 0) {
        g_native_operator_legacy_calls++;
        return mock_operator_healthy_rpc(method, params_json);
    }
    g_native_operator_snapshot_calls++;
    switch (g_native_operator_failure_mode) {
    case MOCK_NATIVE_INVALID_JSON:
        return strdup("{");
    case MOCK_NATIVE_INTERNAL_ERROR:
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"snapshot failed\"}}");
    case MOCK_NATIVE_MIXED_METHOD_NOT_FOUND:
        return strdup("{\"error\":{\"code\":-32601,"
                      "\"message\":\"Method not found\"},"
                      "\"schema\":\"zcl.operator_snapshot.v1\"}");
    case MOCK_NATIVE_WRONG_VERSION:
        return strdup("{\"schema\":\"zcl.operator_snapshot.v1\","
                      "\"schema_version\":2,"
                      "\"execution_locus\":\"target_node\"}");
    }
    return strdup("null");
}

static char *mock_native_operator_fallback_rpc(const char *method,
                                               const char *params_json)
{
    if (strcmp(method, "operatorsnapshot") == 0) {
        g_native_operator_snapshot_calls++;
        return strdup("{\"error\":{\"code\":-32601,"
                      "\"message\":\"Method not found\"}}");
    }
    g_native_operator_legacy_calls++;
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_status_rpc(const char *method, const char *params_json)
{
    if (strcmp(method, "getblockcount") == 0)
        return strdup("3117073");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,\"subver\":\"/ZClassic23:0.1.0/\",\"startingheight\":3117074}]");
    if (strcmp(method, "syncstate") == 0)
        return strdup("{\"state\":\"at_tip\"}");
    if (strcmp(method, "validationstatus") == 0)
        return strdup("{\"ok\":true}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"ok\":true,\"build_commit\":\"nodecafe123\","
                      "\"memory_rss_mb\":128,\"uptime_seconds\":9}");
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"best_header_height\":3117074}");
    if (mock_is_blocker_dump(method, params_json))
        return mock_empty_blocker_dump();
    if (strcmp(method, "dumpstate") == 0 &&
        params_json && strstr(params_json, "reducer_frontier") != NULL)
        return strdup("{\"subsystem\":\"reducer_frontier\","
                      "\"captured_at\":1782240001,"
                      "\"state\":{\"open\":true,"
                      "\"authority\":\"reducer_frontier_hstar\","
                      "\"hstar\":3157646,"
                      "\"served_floor\":3157646,"
                      "\"first_validate_failure_found\":true,"
                      "\"first_validate_failure_height\":3157647,"
                      "\"first_validate_failure_reason\":"
                      "\"header-source-hash-mismatch\","
                      "\"first_validate_failure_repair_owner\":"
                      "\"stale_validate_headers_repair\"}}");
    if (strcmp(method, "dumpstate") == 0 &&
        params_json && strstr(params_json, "condition_engine") != NULL)
        return strdup("{\"subsystem\":\"condition_engine\","
                      "\"captured_at\":1782240002,"
                      "\"state\":{\"registered_count\":28,"
                      "\"active_count\":2,"
                      "\"unresolved_count\":1,"
                      "\"conditions\":[{"
                      "\"name\":\"stale_validate_headers_repair\","
                      "\"currently_active\":true,"
                      "\"attempts\":5,"
                      "\"operator_needed_emitted\":true,"
                      "\"last_operator_needed_unix\":1782240000,"
                      "\"target_at_detect\":1782239000}]}}");
    if (strcmp(method, "dumpstate") == 0 &&
        params_json && strstr(params_json, "tip_finalize") != NULL)
        return strdup("{\"subsystem\":\"tip_finalize\","
                      "\"captured_at\":1782240004,"
                      "\"state\":{\"stage_name\":\"tip_finalize\","
                      "\"cursor\":3157646,"
                      "\"last_precondition_height\":3157646,"
                      "\"last_precondition_reason\":\"have_data_missing\","
                      "\"precondition_repeat_count\":7}}");
    if (strcmp(method, "dumpstate") == 0)
        return strdup("{\"subsystem\":\"chain_advance_coordinator\","
                      "\"captured_at\":1782240003,"
                      "\"state\":{\"initialized\":true,"
                      "\"has_connman\":true,"
                      "\"has_main_state\":true,"
                      "\"has_node_db\":true,"
                      "\"authority\":\"local_consensus_validation\","
                      "\"decision\":\"use_source\","
                      "\"selected_source\":\"p2p\","
                      "\"selected_source_trust\":\"native_peer_validated\","
                      "\"selected_source_selectable\":true,"
                      "\"selected_source_selection_blocker\":\"\","
                      "\"selected_source_score_base\":100,"
                      "\"selected_source_score_health\":20,"
                      "\"selected_source_score_height\":10,"
                      "\"selected_source_score_authorized\":0,"
                      "\"selected_source_score_redundancy_bonus\":0,"
                      "\"selected_source_score_target_lag_penalty\":0,"
                      "\"selected_source_score_failure_penalty\":0,"
                      "\"selected_source_score_mirror_gate_penalty\":0,"
                      "\"has_last_decision\":true,"
                      "\"last_decision\":{"
                      "\"op\":\"peer_floor\","
                      "\"selected_source\":\"p2p\","
                      "\"selected_source_trust\":\"native_peer_validated\","
                      "\"selected_source_selectable\":true,"
                      "\"selected_source_selection_blocker\":\"\","
                      "\"selected_source_score_base\":100,"
                      "\"selected_source_score_health\":20,"
                      "\"selected_source_score_height\":10,"
                      "\"selected_source_score_authorized\":0,"
                      "\"selected_source_score_redundancy_bonus\":0,"
                      "\"selected_source_score_target_lag_penalty\":0,"
                      "\"selected_source_score_failure_penalty\":0,"
                      "\"selected_source_score_mirror_gate_penalty\":0,"
                      "\"authority\":\"local_consensus_validation\","
                      "\"selected_source_reason\":\"healthy=3 connecting=0 groups=3 backoff=0/0 tcp_fail=0 proto_fail=0\","
                      "\"sources\":[{\"source\":\"p2p\","
                      "\"trust\":\"native_peer_validated\","
                      "\"state\":\"healthy\","
                      "\"selectable\":true,"
                      "\"selection_blocker\":\"\","
                      "\"score_base\":100,"
                      "\"score_redundancy_bonus\":0,"
                      "\"score_target_lag_penalty\":0,"
                      "\"score_failure_penalty\":0,"
                      "\"reason\":\"healthy=3 connecting=0 groups=3 backoff=0/0 tcp_fail=0 proto_fail=0\","
                      "\"blocker\":\"\"}]"
                      "},"
                      "\"sources\":[{\"source\":\"p2p\","
                      "\"trust\":\"native_peer_validated\","
                      "\"state\":\"healthy\","
                      "\"selectable\":true,"
                      "\"selection_blocker\":\"\","
                      "\"score_base\":100,"
                      "\"score_redundancy_bonus\":0,"
                      "\"score_target_lag_penalty\":0,"
                      "\"score_failure_penalty\":0,"
                      "\"healthy_peers\":3}]}}");
    return strdup("null");
}

static int g_target_blocker_rpc_calls;

static char *mock_status_rpc_with_target_blockers(const char *method,
                                                   const char *params_json)
{
    if (mock_is_blocker_dump(method, params_json)) {
        g_target_blocker_rpc_calls++;
        return mock_target_blocker_dump();
    }
    return mock_status_rpc(method, params_json);
}

static char *mock_status_rpc_blocker_error(const char *method,
                                           const char *params_json)
{
    if (mock_is_blocker_dump(method, params_json))
        return strdup("{\"code\":-32603,"
                      "\"message\":\"target blocker state unavailable\","
                      "\"method\":\"dumpstate\"}");
    return mock_status_rpc(method, params_json);
}

enum mock_blocker_failure_mode {
    MOCK_BLOCKER_WRAPPED_ERROR,
    MOCK_BLOCKER_MALFORMED,
    MOCK_BLOCKER_WRONG_SUBSYSTEM,
    MOCK_BLOCKER_MISSING_ARRAY,
    MOCK_BLOCKER_COUNT_CONTRADICTION,
    MOCK_BLOCKER_MIXED_ERROR_STATE,
    MOCK_BLOCKER_NULL_RESULT,
};

static enum mock_blocker_failure_mode g_mock_blocker_failure_mode;

static char *mock_status_rpc_blocker_failure_matrix(
    const char *method, const char *params_json)
{
    if (!mock_is_blocker_dump(method, params_json))
        return mock_status_rpc(method, params_json);
    switch (g_mock_blocker_failure_mode) {
    case MOCK_BLOCKER_WRAPPED_ERROR:
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"wrapped blocker failure\"}}");
    case MOCK_BLOCKER_MALFORMED:
        return strdup("{not-json");
    case MOCK_BLOCKER_WRONG_SUBSYSTEM:
        return strdup("{\"subsystem\":\"supervisor\","
                      "\"captured_at\":1782240011,"
                      "\"state\":{\"active_count\":0,"
                      "\"permanent_count\":0,\"transient_count\":0,"
                      "\"dependency_count\":0,\"resource_count\":0,"
                      "\"escape_dispatched_total\":0,\"blockers\":[]}}");
    case MOCK_BLOCKER_MISSING_ARRAY:
        return strdup("{\"subsystem\":\"blocker\","
                      "\"captured_at\":1782240012,"
                      "\"state\":{\"active_count\":0,"
                      "\"permanent_count\":0,\"transient_count\":0,"
                      "\"dependency_count\":0,\"resource_count\":0,"
                      "\"escape_dispatched_total\":0}}");
    case MOCK_BLOCKER_COUNT_CONTRADICTION:
        return strdup("{\"subsystem\":\"blocker\","
                      "\"captured_at\":1782240013,"
                      "\"state\":{\"active_count\":0,"
                      "\"permanent_count\":0,\"transient_count\":0,"
                      "\"dependency_count\":0,\"resource_count\":0,"
                      "\"escape_dispatched_total\":0,\"blockers\":[{"
                      "\"id\":\"hidden\",\"class\":\"permanent\"}]}}");
    case MOCK_BLOCKER_MIXED_ERROR_STATE:
        return strdup("{\"code\":-32603,"
                      "\"message\":\"must not be hidden by state\","
                      "\"subsystem\":\"blocker\","
                      "\"captured_at\":1782240014,"
                      "\"state\":{\"active_count\":0,"
                      "\"permanent_count\":0,\"transient_count\":0,"
                      "\"dependency_count\":0,\"resource_count\":0,"
                      "\"escape_dispatched_total\":0,\"blockers\":[]}}");
    case MOCK_BLOCKER_NULL_RESULT:
        return strdup("null");
    }
    return strdup("null");
}

static char *mock_status_rpc_lagged(const char *method,
                                    const char *params_json)
{
    if (strcmp(method, "getblockcount") == 0)
        return strdup("1000");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,\"startingheight\":1300}]");
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"best_header_height\":1300}");
    return mock_status_rpc(method, params_json);
}

static char *mock_status_rpc_spoofed_peer_height(const char *method,
                                                 const char *params_json)
{
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,"
                      "\"startingheight\":2000000000}]");
    return mock_status_rpc_lagged(method, params_json);
}

static char *mock_status_rpc_sync_inputs_failed(const char *method,
                                                const char *params_json)
{
    if (strcmp(method, "getblockcount") == 0)
        return strdup("{\"code\":-32603,"
                      "\"message\":\"height unavailable\"}");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"peers unavailable\"}}");
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"code\":-32603,"
                      "\"message\":\"headers unavailable\"}");
    return mock_status_rpc(method, params_json);
}

static char *mock_status_rpc_without_node_commit(const char *method,
                                                 const char *params_json)
{
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"ok\":true,\"memory_rss_mb\":128,"
                      "\"uptime_seconds\":9}");
    return mock_status_rpc(method, params_json);
}

static char *mock_status_rpc_negative_health_metrics(
    const char *method, const char *params_json)
{
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"ok\":true,\"build_commit\":\"nodecafe123\","
                      "\"memory_rss_mb\":-1,\"uptime_seconds\":-2}");
    return mock_status_rpc(method, params_json);
}

static char *mock_status_rpc_inconsistent_frontier(
    const char *method, const char *params_json)
{
    if (strcmp(method, "getblockcount") == 0)
        return strdup("111");
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"best_header_height\":110}");
    return mock_status_rpc(method, params_json);
}

static char *mock_status_rpc_peer_without_height(
    const char *method, const char *params_json)
{
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,"
                      "\"state\":\"handshake_complete\"}]");
    return mock_status_rpc(method, params_json);
}

static char *mock_status_rpc_peer_without_direction(
    const char *method, const char *params_json)
{
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"state\":\"handshake_complete\","
                      "\"startingheight\":3117074}]");
    return mock_status_rpc(method, params_json);
}

static char *mock_operator_degraded_rpc(const char *method,
                                        const char *params_json)
{
    if (strcmp(method, "operatorsnapshot") == 0)
        return strdup("{\"code\":-32601,\"message\":\"Method not found\"}");
    if (mock_is_blocker_dump(method, params_json))
        return mock_empty_blocker_dump();
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"blocks\":100,\"best_header_height\":110}");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,\"state\":\"handshake_complete\","
                      "\"startingheight\":112},"
                      "{\"inbound\":true,\"state\":\"version_sent\","
                      "\"startingheight\":111}]");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"blocks_download\","
                      "\"chain_height\":100,\"best_header_height\":110,"
                      "\"watchdog\":{\"active_conditions\":1}}");
    if (strcmp(method, "downloadstats") == 0)
        return strdup("{\"in_flight\":0,\"queued\":0,"
                      "\"sync_state\":\"at_tip\"}");
    if (strcmp(method, "getmirrorstatus") == 0)
        return strdup("{\"mirror_enabled\":true,\"mirror_running\":true,"
                      "\"reachable\":false,"
                      "\"active_error_code\":\"rpc-unreachable\","
                      "\"active_error_detail\":\"zclassicd warming\"}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"healthy\":false,\"serving\":true,"
                      "\"checks\":{\"operator_needed\":false,"
                      "\"condition_engine\":{\"active_count\":1,"
                      "\"unresolved_count\":0}}}");
    if (strcmp(method, "agent") == 0)
        return strdup("{\"schema\":\"zcl.public_status.v1\","
                      "\"readiness\":{"
                      "\"schema\":\"zcl.agent_readiness.v1\","
                      "\"chain_serving_ready\":true,"
                      "\"index_projection_ready\":false,"
                      "\"agent_work_ready\":true},"
                      "\"operator_lane\":{"
                      "\"schema\":\"zcl.operator_lane.v1\","
                      "\"schema_version\":1,"
                      "\"lane\":\"dev\","
                      "\"runtime_profile\":\"full\","
                      "\"datadir\":\"/tmp/zcl-dev\","
                      "\"canonical\":false,"
                      "\"soak_evidence\":false,"
                      "\"development\":true,"
                      "\"restart_policy\":\"frequent_deploy_ok\","
                      "\"automation_restart_ok\":true,"
                      "\"automation_deploy_ok\":true,"
                      "\"requires_operator_confirmation\":false,"
                      "\"deployment_safety\":{"
                      "\"schema\":\"zcl.operator_deployment_safety.v1\","
                      "\"schema_version\":1,"
                      "\"automation_restart_ok\":true,"
                      "\"automation_deploy_ok\":true,"
                      "\"requires_operator_confirmation\":false,"
                      "\"preferred_deploy_target\":\"dev\","
                      "\"safe_default_action\":\"deploy_dev_lane\"}}}");
    return strdup("null");
}

static char *mock_operator_healthy_rpc(const char *method,
                                       const char *params_json)
{
    if (strcmp(method, "operatorsnapshot") == 0)
        return strdup("{\"error\":{\"code\":-32601,"
                      "\"message\":\"Method not found\"}}");
    if (mock_is_blocker_dump(method, params_json))
        return mock_empty_blocker_dump();
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"blocks\":112,\"best_header_height\":112}");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,\"state\":\"handshake_complete\","
                      "\"startingheight\":112}]");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"at_tip\",\"chain_height\":112,"
                      "\"best_header_height\":112,"
                      "\"watchdog\":{\"active_conditions\":0}}");
    if (strcmp(method, "downloadstats") == 0)
        return strdup("{\"in_flight\":0,\"queued\":0,"
                      "\"sync_state\":\"at_tip\"}");
    if (strcmp(method, "getmirrorstatus") == 0)
        return strdup("{\"mirror_enabled\":true,\"mirror_running\":true,"
                      "\"reachable\":false,"
                      "\"active_error_code\":\"rpc-unreachable\"}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"healthy\":true,\"serving\":true,"
                      "\"checks\":{\"operator_needed\":false,"
                      "\"condition_engine\":{\"active_count\":0,"
                      "\"unresolved_count\":0}}}");
    if (strcmp(method, "agent") == 0)
        return strdup("{\"schema\":\"zcl.public_status.v1\","
                      "\"readiness\":{"
                      "\"schema\":\"zcl.agent_readiness.v1\","
                      "\"chain_serving_ready\":true,"
                      "\"index_projection_ready\":true,"
                      "\"agent_work_ready\":true},"
                      "\"operator_lane\":{"
                      "\"schema\":\"zcl.operator_lane.v1\","
                      "\"schema_version\":1,"
                      "\"lane\":\"canonical\","
                      "\"runtime_profile\":\"full\","
                      "\"datadir\":\"/tmp/zcl-canonical\","
                      "\"canonical\":true,"
                      "\"soak_evidence\":false,"
                      "\"development\":false,"
                      "\"restart_policy\":\"operator_gated\","
                      "\"automation_restart_ok\":false,"
                      "\"automation_deploy_ok\":false,"
                      "\"requires_operator_confirmation\":true,"
                      "\"deployment_safety\":{"
                      "\"schema\":\"zcl.operator_deployment_safety.v1\","
                      "\"schema_version\":1,"
                      "\"automation_restart_ok\":false,"
                      "\"automation_deploy_ok\":false,"
                      "\"requires_operator_confirmation\":true,"
                      "\"protects_public_endpoint\":true,"
                      "\"preferred_deploy_target\":\"dev\","
                      "\"safe_default_action\":"
                      "\"observe_only_or_use_dev_lane\"}}}");
    if (strcmp(method, "milestone") == 0)
        return strdup("{\"schema\":\"zcl.milestone_status.v1\","
                      "\"api_version\":\"v1\","
                      "\"milestone\":\"v1 MVP\","
                      "\"mvp_readiness_score\":4,"
                      "\"target_score\":8,"
                      "\"ascii\":{\"goals\":\"goals [#####-----] 4/8 strict MVP MRS\"},"
                      "\"bars\":{\"subgoals\":{\"bar\":\"[########--]\"}},"
                      "\"criteria\":[1,2,3,4,5,6,7,8],"
                      "\"operator_proofs\":{"
                      "\"schema\":\"zcl.mvp_operator_proofs.v1\","
                      "\"accepted_count\":4,"
                      "\"pending_count\":4,"
                      "\"items\":[{\"key\":\"seven_day_soak\","
                      "\"proof_scope\":\"live_window\","
                      "\"proof_command\":\"make soak-evidence-report\"}]}}");
    if (strcmp(method, "refold") == 0)
        return strdup("{\"schema\":\"zcl.refold_status.v1\","
                      "\"api_version\":\"v1\","
                      "\"ready_for_refold\":false,"
                      "\"primary_blocker\":\"missing_verified_anchor_snapshot\","
                      "\"anchor_snapshot\":{\"verified\":false,"
                      "\"verification\":\"missing\"},"
                      "\"commands\":{\"native\":\"zclassic23 refold\"}}");
    return strdup("null");
}

static char *mock_operator_healthy_with_target_blockers(
    const char *method, const char *params_json)
{
    if (mock_is_blocker_dump(method, params_json))
        return mock_target_blocker_dump();
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_healthy_with_blocker_error(
    const char *method, const char *params_json)
{
    if (mock_is_blocker_dump(method, params_json))
        return strdup("{\"code\":-32603,"
                      "\"message\":\"blocker telemetry unavailable\"}");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_invalid_core_evidence(
    const char *method, const char *params_json)
{
    if (mock_is_blocker_dump(method, params_json))
        return mock_empty_blocker_dump();
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"code\":-32603,"
                      "\"message\":\"chain evidence unavailable\"}");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"sync evidence unavailable\"}}");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("{\"code\":-32603,"
                      "\"message\":\"peer evidence unavailable\"}");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_nonobject_peer_rpc(const char *method,
                                              const char *params_json)
{
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[null]");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_zero_peers_rpc(const char *method,
                                          const char *params_json)
{
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[]");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_inconsistent_frontier_rpc(
    const char *method, const char *params_json)
{
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"blocks\":111,\"best_header_height\":110}");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"at_tip\","
                      "\"chain_height\":111,"
                      "\"best_header_height\":110,"
                      "\"watchdog\":{\"active_conditions\":0}}");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_non_tip_zero_gap_rpc(
    const char *method, const char *params_json)
{
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"blocks_download\","
                      "\"chain_height\":112,"
                      "\"best_header_height\":112,"
                      "\"watchdog\":{\"active_conditions\":0}}");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_not_serving_with_blocker_error(
    const char *method, const char *params_json)
{
    if (mock_is_blocker_dump(method, params_json))
        return strdup("{\"code\":-32603,"
                      "\"message\":\"blocker telemetry unavailable\"}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"healthy\":false,\"serving\":false,"
                      "\"checks\":{\"operator_needed\":false,"
                      "\"blocking_reason\":\"database_read_only\","
                      "\"condition_engine\":{\"active_count\":0,"
                      "\"unresolved_count\":0}}}");
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"code\":-32603,"
                      "\"message\":\"chain RPC unavailable\"}");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"peer RPC unavailable\"}}");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_served_lag_rpc(const char *method,
                                          const char *params_json)
{
    if (mock_is_blocker_dump(method, params_json))
        return mock_empty_blocker_dump();
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"blocks\":100,\"best_header_height\":110}");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,"
                      "\"state\":\"handshake_complete\","
                      "\"startingheight\":110}]");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"blocks_download\","
                      "\"chain_height\":100,"
                      "\"best_header_height\":110,"
                      "\"watchdog\":{\"active_conditions\":0}}");
    if (strcmp(method, "downloadstats") == 0)
        return strdup("{\"in_flight\":0,\"queued\":0,"
                      "\"sync_state\":\"blocks_download\"}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"healthy\":true,\"serving\":true,"
                      "\"chain_advance\":{\"local_height\":110},"
                      "\"chain_evidence\":{\"active_tip\":110},"
                      "\"checks\":{\"operator_needed\":false,"
                      "\"condition_engine\":{\"active_count\":0,"
                      "\"unresolved_count\":0}}}");
    return mock_operator_healthy_rpc(method, params_json);
}

enum mock_download_failure_mode {
    MOCK_DOWNLOAD_MISSING_COUNTERS,
    MOCK_DOWNLOAD_WRONG_TYPES,
    MOCK_DOWNLOAD_NEGATIVE_COUNTER,
};

static enum mock_download_failure_mode g_mock_download_failure_mode;

static char *mock_operator_invalid_download_rpc(
    const char *method, const char *params_json)
{
    if (strcmp(method, "downloadstats") != 0)
        return mock_operator_served_lag_rpc(method, params_json);
    if (g_mock_download_failure_mode == MOCK_DOWNLOAD_MISSING_COUNTERS)
        return strdup("{}");
    if (g_mock_download_failure_mode == MOCK_DOWNLOAD_WRONG_TYPES)
        return strdup("{\"in_flight\":\"0\",\"queued\":\"0\"}");
    return strdup("{\"in_flight\":-1,\"queued\":0}");
}

static char *mock_operator_max_download_rpc(const char *method,
                                            const char *params_json)
{
    if (strcmp(method, "downloadstats") == 0)
        return strdup("{\"in_flight\":9223372036854775807,"
                      "\"queued\":9223372036854775807,"
                      "\"sync_state\":\"blocks_download\"}");
    return mock_operator_served_lag_rpc(method, params_json);
}

static char *mock_operator_recovered_mirror_rpc(const char *method,
                                                const char *params_json)
{
    if (strcmp(method, "getmirrorstatus") == 0)
        return strdup("{\"mirror_enabled\":true,\"mirror_running\":true,"
                      "\"reachable\":true,"
                      "\"active_error_code\":\"hash-disagreement\","
                      "\"activation_blocker\":\"hash-disagreement\","
                      "\"active_error_detail\":\"stale mismatch\","
                      "\"mirror_contract\":{"
                      "\"schema\":\"zcl.mirror_status.v1\","
                      "\"schema_version\":1,"
                      "\"advisory_only\":true,"
                      "\"consensus_authority\":\"local_consensus_validation\","
                      "\"status\":\"healthy\","
                      "\"mirror_running\":true,"
                      "\"reachable\":true,"
                      "\"legacy_oracle_usable\":true,"
                      "\"lag_known\":true,"
                      "\"lag_blocks\":0,"
                      "\"same_height\":true,"
                      "\"tip_hashes_agree\":true,"
                      "\"blocker_active\":false,"
                      "\"blocker_code\":\"\","
                      "\"blocker_recovered_by_tip_agreement\":true,"
                      "\"operator_action_required\":false}}");
    return mock_operator_healthy_rpc(method, params_json);
}

static char *mock_operator_needed_rpc(const char *method,
                                      const char *params_json)
{
    if (strcmp(method, "operatorsnapshot") == 0)
        return strdup("{\"code\":-32601,\"message\":\"Method not found\"}");
    if (mock_is_blocker_dump(method, params_json))
        return mock_empty_blocker_dump();
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"blocks\":112,\"best_header_height\":112}");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false,\"state\":\"handshake_complete\","
                      "\"startingheight\":112}]");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"at_tip\",\"chain_height\":112,"
                      "\"best_header_height\":112,"
                      "\"watchdog\":{\"active_conditions\":2}}");
    if (strcmp(method, "downloadstats") == 0)
        return strdup("{\"in_flight\":0,\"queued\":0,"
                      "\"sync_state\":\"at_tip\"}");
    if (strcmp(method, "getmirrorstatus") == 0)
        return strdup("{\"mirror_enabled\":true,\"mirror_running\":true,"
                      "\"reachable\":true}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"healthy\":false,\"serving\":false,"
                      "\"checks\":{\"operator_needed\":true,"
                      "\"operator_needed_detail\":\"chain_integrity_failed\","
                      "\"blocking_reason\":\"operator_needed:chain_integrity_failed\","
                      "\"condition_engine\":{\"active_count\":2,"
                      "\"unresolved_count\":2}}}");
    if (strcmp(method, "agent") == 0)
        return strdup("{\"schema\":\"zcl.public_status.v1\","
                      "\"operator_lane\":{"
                      "\"schema\":\"zcl.operator_lane.v1\","
                      "\"schema_version\":1,"
                      "\"lane\":\"canonical\","
                      "\"canonical\":true,"
                      "\"development\":false,"
                      "\"restart_policy\":\"operator_gated\","
                      "\"automation_restart_ok\":false,"
                      "\"automation_deploy_ok\":false,"
                      "\"requires_operator_confirmation\":true,"
                      "\"deployment_safety\":{"
                      "\"schema\":\"zcl.operator_deployment_safety.v1\","
                      "\"schema_version\":1,"
                      "\"automation_restart_ok\":false,"
                      "\"automation_deploy_ok\":false,"
                      "\"requires_operator_confirmation\":true,"
                      "\"preferred_deploy_target\":\"dev\","
                      "\"safe_default_action\":"
                      "\"observe_only_or_use_dev_lane\"}}}");
    return strdup("null");
}

static bool g_agent_impact_params_seen;
static bool g_agent_deploy_guard_params_seen;
static bool g_agent_timeline_params_seen;
static bool g_agent_diagnose_brief_params_seen;
static bool g_agent_diagnose_full_params_seen;
static bool g_agent_liveness_brief_params_seen;
static bool g_agent_liveness_full_params_seen;
static bool g_service_catalog_name_params_seen;
static bool g_service_operations_id_params_seen;
static bool g_service_operations_filter_params_seen;

static char *mock_agent_dev_rpc(const char *method, const char *params_json)
{
    if (strcmp(method, "agentmap") == 0)
        return strdup("{\"schema\":\"zcl.agent_map.v1\","
                      "\"commands\":[{\"name\":\"build\"}],"
                      "\"deprecated_shim\":{\"primary\":false}}");
    if (strcmp(method, "agentlanes") == 0)
        return strdup("{\"schema\":\"zcl.agent_lanes.v1\","
                      "\"default_deploy_target\":\"dev\","
                      "\"current_runtime_services\":{"
                      "\"schema\":\"zcl.agent_runtime_services.v1\","
                      "\"rpc_running\":true,"
                      "\"https_running\":false,"
                      "\"fs_running\":true},"
                      "\"lanes\":[{\"lane\":\"canonical\","
                      "\"unit\":\"zclassic23\","
                      "\"deployment_safety\":{"
                      "\"requires_operator_confirmation\":true,"
                      "\"automation_deploy_ok\":false}},"
                      "{\"lane\":\"dev\","
                      "\"unit\":\"zcl23-dev\","
                      "\"deployment_safety\":{"
                      "\"requires_operator_confirmation\":false,"
                      "\"automation_deploy_ok\":true}}]}");
    if (strcmp(method, "agentimpact") == 0) {
        g_agent_impact_params_seen =
            params_json &&
            contains(params_json,
                     "\"app/controllers/src/agent_controller.c\"") &&
            contains(params_json,
                     "\"tools/mcp/controllers/ops_controller.c\"");
        return strdup("{\"schema\":\"zcl.agent_impact.v1\","
                      "\"files_count\":2,"
                      "\"mcp_changed\":true,"
                      "\"relevant_test_groups\":[\"mcp_controllers\"],"
                      "\"recommended_commands\":[\"make fast-ci\"]}");
    }
    if (strcmp(method, "agentcontracts") == 0)
        return strdup("{\"schema\":\"zcl.agent_contracts.v1\","
                      "\"contract_summary\":{\"contract_count\":22,"
                      "\"mcp_declared_count\":21},"
                      "\"schemas\":[{\"schema\":\"zcl.agent_build.v1\"},"
                      "{\"schema\":\"zcl.agent_readiness.v1\"}],"
                      "\"transports\":[\"mcp: zcl_agent_build\"]}");
    if (strcmp(method, "agentbuild") == 0)
        return strdup("{\"schema\":\"zcl.agent_build.v1\","
                      "\"incremental_compile\":{\"header_depfiles\":true},"
                      "\"commands\":[{\"name\":\"compile_check\"}],"
                      "\"reproducible_release\":{\"command\":\"make ci-reproducible\"}}");
    if (strcmp(method, "proofbundle") == 0)
        return strdup("{\"schema\":\"zcl.operator_proof_bundle.v1\","
                      "\"api_version\":\"v1\","
                      "\"build_commit\":\"nodecafe123\","
                      "\"anchor_datadir\":\"/tmp\","
                      "\"agent\":{\"schema\":\"zcl.public_status.v1\"},"
                      "\"milestone\":{\"schema\":\"zcl.milestone_status.v1\"},"
                      "\"refold\":{\"schema\":\"zcl.refold_status.v1\"},"
                      "\"anchor_status\":{\"schema\":\"zcl.anchor_mint_status.v1\"},"
                      "\"lanes\":{\"schema\":\"zcl.agent_lanes.v1\"},"
                      "\"dev_status\":{\"schema\":\"zcl.agent_dev_status.v1\"}}");
    if (strcmp(method, "agentinterface") == 0)
        return strdup("{\"schema\":\"zcl.agent_interface.v1\","
                      "\"build_commit\":\"nodecafe123\","
                      "\"preferred_transport\":\"mcp\","
                      "\"preferred_payload\":\"json\","
                      "\"capabilities\":[{\"name\":\"runtime_status\","
                      "\"schema\":\"zcl.public_status.v1\","
                      "\"mcp\":\"zcl_agent\"}],"
                      "\"machine_contract\":{\"schema\":\"zcl.agent_machine_contract.v1\","
                      "\"payload\":\"json_object\","
                      "\"schema_required\":true,"
                      "\"transport_equivalent_payloads\":true,"
                      "\"no_python_required\":true,"
                      "\"no_tools_z_required\":true},"
                      "\"runtime_identity\":{\"schema\":\"zcl.agent_runtime_identity.v1\","
                      "\"build_commit\":\"nodecafe123\","
                      "\"binary\":\"zclassic23\"},"
                      "\"must_live_in_c\":[\"deploy/restart safety decisions\"],"
                      "\"avoid\":[\"do not add new operator logic to tools/z\"]}");
    if (strcmp(method, "agentops") == 0)
        return strdup("{\"schema\":\"zcl.agent_ops.v1\","
                      "\"api_version\":\"v1\","
                      "\"status\":\"ok\","
                      "\"method\":\"agentops\","
                      "\"native_command\":\"zclassic23 agentops\","
                      "\"mcp_tool\":\"zcl_agent_ops\","
                      "\"contract_source\":\"agent_contracts.def\","
                      "\"api_style\":\"one compact first call, then registry-owned primitive drilldowns\","
                      "\"api_ux\":{\"start_here\":\"zclassic23 agentops / zcl_agent_ops\"},"
                      "\"no_jq_required\":true,"
                      "\"workflow\":[{\"rank\":1,"
                      "\"name\":\"first_call\"}],"
                      "\"top_next_work\":[{\"rank\":1,"
                      "\"name\":\"finish_self_verified_utxo_anchor_rebuild\"}],"
                      "\"direct_commands\":[{\"name\":\"live_status\"}]}");
    if (strcmp(method, "appprotocols") == 0)
        return strdup("{\"schema\":\"zcl.application_protocols.index.v1\","
                      "\"base_layer\":\"zclassic_l1\","
                      "\"service_layer\":\"zclassic23_application_layer\","
                      "\"protocol_count\":2,"
                      "\"protocols\":[{\"name\":\"zslp\","
                      "\"schema\":\"zcl.application_protocol_contract.v1\"},"
                      "{\"name\":\"script_contracts\","
                      "\"anchor_kind\":\"standard_script\"}]}");
    if (strcmp(method, "servicecatalog") == 0) {
        if (params_json && contains(params_json, "\"bootstrap\"")) {
            g_service_catalog_name_params_seen = true;
            return strdup("{\"schema\":\"zcl.service_contract.v1\","
                          "\"base_layer\":\"zclassic_l1\","
                          "\"service_layer\":\"zclassic23_application_layer\","
                          "\"name\":\"bootstrap\","
                          "\"self_route\":\"/api/v1/service-catalog/bootstrap\","
                          "\"depends_on_services\":[\"full_node\"],"
                          "\"read_model\":\"network_bootstrap_status_and_peer_projection\","
                          "\"write_model\":\"seed_inventory_and_endpoint_advertisement\","
                          "\"operations\":[{\"schema\":\"zcl.service_operation.v1\","
                          "\"operation\":\"read_bootstrap_status\","
                          "\"mcp_tool\":\"zcl_bootstrapstatus\"}]}");
        }
        return strdup("{\"schema\":\"zcl.service_catalog.v1\","
                      "\"base_layer\":\"zclassic_l1\","
                      "\"service_layer\":\"zclassic23_application_layer\","
                      "\"operation_schema\":\"zcl.service_operation.v1\","
                      "\"sovereign_ux\":{\"schema\":\"zcl.sovereign_ux_contract.v1\","
                      "\"flow\":[\"read_agent_status\",\"inspect_service_catalog\","
                      "\"resolve_znam_name\"]},"
                      "\"service_count\":2,"
                      "\"services\":[{\"name\":\"bootstrap\","
                      "\"rest_collection\":\"/api/v1/bootstrap\","
                      "\"depends_on_services\":[\"full_node\"],"
                      "\"read_model\":\"network_bootstrap_status_and_peer_projection\","
                      "\"operations\":[{\"operation\":\"read_bootstrap_status\"}]},"
                      "{\"name\":\"znam_names\","
                      "\"application_protocol\":\"znam\","
                      "\"depends_on_services\":[\"full_node\"],"
                      "\"read_model\":\"name_records_by_confirmed_chain_state\","
                      "\"write_model\":\"op_return_name_operation_lifecycle\"}]}");
    }
    if (strcmp(method, "serviceoperations") == 0) {
        if (params_json &&
            contains(params_json,
                     "\"bootstrap.read_bootstrap_status\"")) {
            g_service_operations_id_params_seen = true;
            return strdup("{\"schema\":\"zcl.service_operation.v1\","
                          "\"api_version\":\"v1\","
                          "\"operation_id\":\"bootstrap.read_bootstrap_status\","
                          "\"service\":\"bootstrap\","
                          "\"operation\":\"read_bootstrap_status\","
                          "\"crud_capability\":\"read_singleton\","
                          "\"rest_route\":\"/api/v1/bootstrap\","
                          "\"mcp_tool\":\"zcl_bootstrapstatus\","
                          "\"write_safety\":\"public_read_only\","
                          "\"agent_preferred_interface\":\"rest\"}");
        }
        if (params_json && contains(params_json, "service=bootstrap") &&
            contains(params_json, "write_safety=public_read_only")) {
            g_service_operations_filter_params_seen = true;
            return strdup("{\"schema\":\"zcl.service_operations.index.v1\","
                          "\"api_version\":\"v1\","
                          "\"filters\":{\"active\":true,"
                          "\"service\":\"bootstrap\","
                          "\"write_safety\":\"public_read_only\"},"
                          "\"operation_count\":2,"
                          "\"summary\":{\"operation_count\":2,"
                          "\"destructive_count\":0},"
                          "\"operations\":["
                          "{\"operation_id\":\"bootstrap.read_bootstrap_status\","
                          "\"service\":\"bootstrap\"},"
                          "{\"operation_id\":\"bootstrap.list_peer_projection\","
                          "\"service\":\"bootstrap\"}]}");
        }
        return strdup("{\"schema\":\"zcl.service_operations.index.v1\","
                      "\"api_version\":\"v1\","
                      "\"catalog_route\":\"/api/v1/service-catalog\","
                      "\"operation_count\":2,"
                      "\"summary\":{\"operation_count\":2},"
                      "\"operations\":["
                      "{\"operation_id\":\"bootstrap.read_bootstrap_status\","
                      "\"service\":\"bootstrap\"},"
                      "{\"operation_id\":\"znam_names.resolve_name\","
                      "\"service\":\"znam_names\"}]}");
    }
    if (strcmp(method, "agentdiagnose") == 0) {
        if (params_json && contains(params_json, "\"brief\""))
            g_agent_diagnose_brief_params_seen = true;
        if (params_json && contains(params_json, "\"full\"")) {
            g_agent_diagnose_full_params_seen = true;
            return strdup("{\"schema\":\"zcl.agent_diagnose.v1\","
                          "\"api_version\":\"v1\","
                          "\"method\":\"agentdiagnose\","
                          "\"native_command\":\"zclassic23 agentdiagnose\","
                          "\"mcp_tool\":\"zcl_agent_diagnose\","
                          "\"contract_source\":\"agent_contracts.def\","
                          "\"detail_mode\":\"full\","
                          "\"embedded_drilldowns\":true,"
                          "\"verdict\":\"healthy\","
                          "\"safe_next_action\":\"monitor_agent_and_liveness\","
                          "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
                          "\"api\":\"agentdiagnose\"},"
                          "\"peer_incidents\":{\"schema\":\"zcl.peer_incidents.v1\"}}");
        }
        return strdup("{\"schema\":\"zcl.agent_diagnose.v1\","
                      "\"api_version\":\"v1\","
                      "\"method\":\"agentdiagnose\","
                      "\"native_command\":\"zclassic23 agentdiagnose\","
                      "\"mcp_tool\":\"zcl_agent_diagnose\","
                      "\"contract_source\":\"agent_contracts.def\","
                      "\"detail_mode\":\"brief\","
                      "\"embedded_drilldowns\":false,"
                      "\"verdict\":\"healthy\","
                      "\"safe_next_action\":\"monitor_agent_and_liveness\","
                      "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
                      "\"api\":\"agentdiagnose\"},"
                      "\"omitted_sections\":[\"peer_incidents\"]}");
    }
    if (strcmp(method, "agentliveness") == 0) {
        if (params_json && contains(params_json, "\"brief\""))
            g_agent_liveness_brief_params_seen = true;
        if (params_json && contains(params_json, "\"full\"")) {
            g_agent_liveness_full_params_seen = true;
            return strdup("{\"schema\":\"zcl.agent_liveness.v1\","
                          "\"api_version\":\"v1\","
                          "\"method\":\"agentliveness\","
                          "\"native_command\":\"zclassic23 agentliveness\","
                          "\"mcp_tool\":\"zcl_agent_liveness\","
                          "\"contract_source\":\"agent_contracts.def\","
                          "\"detail_mode\":\"full\","
                          "\"embedded_drilldowns\":true,"
                          "\"overall_liveness\":\"active\","
                          "\"agent_next_action\":\"inspect_recommended_drilldowns\","
                          "\"runtime_availability\":{\"methods\":[{\"method\":\"agent\"}]},"
                          "\"background_quality_status\":{\"lanes\":[{\"lane\":\"tests\"}]},"
                          "\"supervisor_state\":{\"domains\":[]},"
                          "\"liveness_summary\":{\"quality_failed_count\":0},"
                          "\"recommended_drilldowns\":[\"zcl_state subsystem=supervisor\"]}");
        }
        return strdup("{\"schema\":\"zcl.agent_liveness.v1\","
                      "\"api_version\":\"v1\","
                      "\"method\":\"agentliveness\","
                      "\"native_command\":\"zclassic23 agentliveness\","
                      "\"mcp_tool\":\"zcl_agent_liveness\","
                      "\"contract_source\":\"agent_contracts.def\","
                      "\"detail_mode\":\"brief\","
                      "\"embedded_drilldowns\":false,"
                      "\"overall_liveness\":\"active\","
                      "\"agent_next_action\":\"inspect_recommended_drilldowns\","
                      "\"runtime_availability\":{\"object_completeness\":\"compact\"},"
                      "\"background_quality_status\":{\"object_completeness\":\"compact\"},"
                      "\"supervisor_state\":{\"object_completeness\":\"compact\"},"
                      "\"liveness_summary\":{\"quality_failed_count\":0},"
                      "\"first_call\":{\"schema\":\"zcl.first_call_contract.v1\","
                      "\"api\":\"agentliveness\"},"
                      "\"omitted_sections\":[\"runtime_availability.methods\"],"
                      "\"recommended_drilldowns\":[\"zcl_state subsystem=supervisor\"]}");
    }
    if (strcmp(method, "timeline") == 0) {
        g_agent_timeline_params_seen =
            params_json &&
            contains(params_json, "\"category\":\"sync\"") &&
            contains(params_json, "\"scan_count\":16") &&
            contains(params_json, "\"since_secs\":3600") &&
            contains(params_json, "\"peer\":7") &&
            contains(params_json, "\"height\":42") &&
            contains(params_json, "\"reducer_stage\":\"body_fetch\"") &&
            contains(params_json,
                     "\"condition\":\"download_queue_starved\"") &&
            contains(params_json, "\"deploy\":\"make-deploy\"") &&
            contains(params_json, "\"lane\":\"dev\"");
        return strdup("{\"schema\":\"zcl.timeline.v1\","
                      "\"api_version\":\"v1\","
                      "\"status\":\"ok\","
                      "\"source\":\"event_ring\","
                      "\"category\":\"sync\","
                      "\"type_prefix\":\"sync.\","
                      "\"mcp_tool\":\"zcl_timeline\","
                      "\"head_seq\":12,"
                      "\"filters\":{\"active\":true,"
                      "\"peer\":7,\"height\":42,"
                      "\"reducer_stage\":\"body_fetch\","
                      "\"condition\":\"download_queue_starved\","
                      "\"deploy\":\"make-deploy\","
                      "\"lane\":\"dev\"},"
                      "\"events\":[{\"seq\":11,"
                      "\"type\":\"sync.heartbeat\","
                      "\"data\":\"state=headers\"}]}");
    }
    if (strcmp(method, "agentdeployguard") == 0) {
        g_agent_deploy_guard_params_seen =
            params_json && contains(params_json, "canonical-deploy");
        return strdup("{\"schema\":\"zcl.agent_deploy_guard.v1\","
                      "\"allowed\":false,"
                      "\"decision\":\"refuse\","
                      "\"reason\":\"operator_confirmation_required\","
                      "\"lane\":\"canonical\","
                      "\"exit_code\":1}");
    }
    (void)params_json;
    return strdup("null");
}

static int test_zcl_operator_summary_degraded_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_operator_summary names degraded next action") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_degraded_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.operator_summary.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT(json_get_int(json_get(&root, "height")) == 100);
        ASSERT(json_get_int(json_get(&root, "target_height")) == 110);
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "target_height_source")),
                      "target_node.validated_header_tip");
        ASSERT(json_get_int(json_get(&root, "gap")) == 10);
        ASSERT_STR_EQ(json_get_str(json_get(&root, "sync_state")),
                      "blocks_download");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "condition_active");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_tool")),
                      "zcl_conditions");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "operator_lane_name")),
                      "dev");
        ASSERT(json_get_bool(json_get(&root, "automation_restart_ok")));
        ASSERT(json_get_bool(json_get(&root, "automation_deploy_ok")));
        ASSERT(!json_get_bool(json_get(&root,
                                       "requires_operator_confirmation")));
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "preferred_deploy_target")),
                      "dev");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "safe_default_action")),
                      "deploy_dev_lane");
        const struct json_value *lane =
            json_get(&root, "operator_lane");
        ASSERT(lane && lane->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(lane, "schema")),
                      "zcl.operator_lane.v1");
        ASSERT_STR_EQ(json_get_str(json_get(lane, "lane")), "dev");
        ASSERT(json_get_bool(json_get(lane, "development")));
        ASSERT_STR_EQ(json_get_str(json_get(lane, "restart_policy")),
                      "frequent_deploy_ok");
        ASSERT(json_get_bool(json_get(lane, "automation_restart_ok")));
        ASSERT(json_get_bool(json_get(lane, "automation_deploy_ok")));
        ASSERT(!json_get_bool(json_get(lane,
                                       "requires_operator_confirmation")));
        const struct json_value *safety =
            json_get(lane, "deployment_safety");
        ASSERT(safety && safety->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(safety, "schema")),
                      "zcl.operator_deployment_safety.v1");
        ASSERT_STR_EQ(json_get_str(json_get(safety,
                                            "safe_default_action")),
                      "deploy_dev_lane");

        const struct json_value *tools =
            json_get(&root, "recommended_tools");
        ASSERT(tools != NULL);
        ASSERT(json_size(tools) == 1);
        ASSERT_STR_EQ(json_get_str(json_at(tools, 0)), "zcl_conditions");

        const struct json_value *peers = json_get(&root, "peers");
        ASSERT(peers != NULL);
        ASSERT(json_get_int(json_get(peers, "total")) == 2);
        ASSERT(json_get_int(json_get(peers, "ready")) == 1);
        ASSERT(json_get_int(json_get(peers, "max_height")) == 112);
        ASSERT_STR_EQ(json_get_str(json_get(peers, "max_height_trust")),
                      "untrusted_peer_advertisement");

        const struct json_value *mirror = json_get(&root, "mirror");
        ASSERT(mirror != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(mirror, "blocker")),
                      "rpc-unreachable");

        const struct json_value *raw = json_get(&root, "raw");
        ASSERT(raw != NULL);
        ASSERT(json_get(raw, "chain") != NULL);
        ASSERT(json_get(raw, "syncdiag") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_operator_summary_compat_shape(void)
{
    int failures = 0;
    TEST("controllers: legacy operator summary is explicit and never green") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_healthy_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT(json_get_int(json_get(&root, "gap")) == 0);
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "compatibility_snapshot_non_atomic");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_action")),
                      "upgrade target for native operatorsnapshot support");
        ASSERT(json_size(json_get(&root, "recommended_tools")) == 1);
        ASSERT(json_get_bool(json_get(&root, "compatibility_fallback")));
        ASSERT(!json_get_bool(json_get(&root, "atomic")));
        ASSERT(!json_get_bool(json_get(&root, "verdict_complete")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "capture_model")),
                      "multi_rpc_compat");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "operator_lane_name")),
                      "canonical");
        ASSERT(!json_get_bool(json_get(&root, "automation_restart_ok")));
        ASSERT(!json_get_bool(json_get(&root, "automation_deploy_ok")));
        ASSERT(json_get_bool(json_get(&root,
                                      "requires_operator_confirmation")));
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "preferred_deploy_target")),
                      "dev");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "safe_default_action")),
                      "observe_only_or_use_dev_lane");
        const struct json_value *lane =
            json_get(&root, "operator_lane");
        ASSERT(lane && lane->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(lane, "schema")),
                      "zcl.operator_lane.v1");
        ASSERT_STR_EQ(json_get_str(json_get(lane, "lane")),
                      "canonical");
        ASSERT(json_get_bool(json_get(lane, "canonical")));
        ASSERT_STR_EQ(json_get_str(json_get(lane, "restart_policy")),
                      "operator_gated");
        ASSERT(!json_get_bool(json_get(lane, "automation_restart_ok")));
        ASSERT(!json_get_bool(json_get(lane, "automation_deploy_ok")));
        ASSERT(json_get_bool(json_get(lane,
                                      "requires_operator_confirmation")));
        const struct json_value *safety =
            json_get(lane, "deployment_safety");
        ASSERT(safety && safety->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(safety, "schema")),
                      "zcl.operator_deployment_safety.v1");
        ASSERT(json_get_bool(json_get(safety,
                                      "protects_public_endpoint")));

        const struct json_value *mirror = json_get(&root, "mirror");
        ASSERT(mirror != NULL);
        ASSERT(json_get_bool(json_get(mirror, "enabled")));
        ASSERT(!json_get_bool(json_get(mirror, "reachable")));
        ASSERT_STR_EQ(json_get_str(json_get(mirror, "blocker")),
                      "rpc-unreachable");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_native_operator_snapshot_single_rpc(void)
{
    int failures = 0;
    TEST("controllers: native operator snapshot is one target call") {
        register_all();
        g_native_operator_snapshot_calls = 0;
        g_native_operator_legacy_calls = 0;
        mcp_rpc_client_set_test_hook(mock_native_operator_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);

        char *summary_body =
            mcp_router_dispatch("zcl_operator_summary", &args);
        ASSERT(summary_body != NULL);
        ASSERT(g_native_operator_snapshot_calls == 1);
        ASSERT(g_native_operator_legacy_calls == 0);
        struct json_value summary;
        ASSERT(json_read(&summary, summary_body, strlen(summary_body)));
        ASSERT_STR_EQ(json_get_str(json_get(&summary, "schema")),
                      "zcl.operator_summary.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&summary, "execution_locus")),
                      "target_node");
        ASSERT(!json_get_bool(json_get(&summary, "compatibility_fallback")));
        ASSERT(json_get_bool(json_get(json_get(&summary, "future_field"),
                                      "kept")));
        json_free(&summary);
        free(summary_body);

        g_native_operator_snapshot_calls = 0;
        g_native_operator_legacy_calls = 0;
        char *snapshot_body =
            mcp_router_dispatch("zcl_operator_snapshot", &args);
        ASSERT(snapshot_body != NULL);
        ASSERT(g_native_operator_snapshot_calls == 1);
        ASSERT(g_native_operator_legacy_calls == 0);
        char *snapshot_expect = native_operator_snapshot_json_dup();
        ASSERT(snapshot_expect != NULL);
        ASSERT_STR_EQ(snapshot_body, snapshot_expect);
        free(snapshot_expect);

        free(snapshot_body);
        json_free(&args);
        mcp_rpc_client_set_test_hook(NULL);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_native_operator_snapshot_failure_never_falls_back(void)
{
    int failures = 0;
    TEST("controllers: malformed supported snapshot never downgrades") {
        register_all();
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        for (int mode = MOCK_NATIVE_INVALID_JSON;
             mode <= MOCK_NATIVE_WRONG_VERSION; mode++) {
            g_native_operator_failure_mode =
                (enum mock_native_operator_failure_mode)mode;
            g_native_operator_snapshot_calls = 0;
            g_native_operator_legacy_calls = 0;
            mcp_rpc_client_set_test_hook(mock_native_operator_failure_rpc);
            char *body = mcp_router_dispatch("zcl_operator_summary", &args);
            ASSERT(body != NULL);
            ASSERT(g_native_operator_snapshot_calls == 1);
            ASSERT(g_native_operator_legacy_calls == 0);
            ASSERT(strstr(body, "operatorsnapshot rejected") != NULL);
            free(body);
        }
        json_free(&args);
        mcp_rpc_client_set_test_hook(NULL);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_native_operator_snapshot_exact_fallback(void)
{
    int failures = 0;
    TEST("controllers: exact method-not-found uses marked compatibility") {
        register_all();
        g_native_operator_snapshot_calls = 0;
        g_native_operator_legacy_calls = 0;
        mcp_rpc_client_set_test_hook(mock_native_operator_fallback_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        ASSERT(body != NULL);
        ASSERT(g_native_operator_snapshot_calls == 1);
        ASSERT(g_native_operator_legacy_calls == 8);
        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_bool(json_get(&root, "compatibility_fallback")));
        ASSERT(!json_get_bool(json_get(&root, "atomic")));
        ASSERT(!json_get_bool(json_get(&root, "verdict_complete")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "compatibility_snapshot_non_atomic");
        json_free(&root);
        free(body);
        json_free(&args);
        mcp_rpc_client_set_test_hook(NULL);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_target_blockers_override_cached_healthy(void)
{
    int failures = 0;
    TEST("controllers: target blockers override cached healthy summary") {
        register_all();
        mcp_rpc_client_set_test_hook(
            mock_operator_healthy_with_target_blockers);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")),
                      "operator_needed");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT(json_get_bool(json_get(&root, "operator_needed")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "typed_blocker:target-disk-full");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_tool")),
                      "zcl_blockers");
        const struct json_value *blockers = json_get(&root, "blockers");
        ASSERT(blockers != NULL && blockers->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(blockers, "active_count")) == 2);
        ASSERT(json_get_int(json_get(blockers, "resource_count")) == 1);
        ASSERT(!contains(body, "\"status\":\"healthy\""));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_unknown_blockers_cannot_be_healthy(void)
{
    int failures = 0;
    TEST("controllers: unavailable blocker evidence prevents healthy verdict") {
        register_all();
        mcp_rpc_client_set_test_hook(
            mock_operator_healthy_with_blocker_error);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "blocker_state_unavailable");
        const struct json_value *blockers = json_get(&root, "blockers");
        const struct json_value *error = json_get(&root, "blockers_error");
        ASSERT(blockers != NULL && blockers->type == JSON_NULL);
        ASSERT(error != NULL && error->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(error, "message")),
                      "blocker telemetry unavailable");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_invalid_core_evidence_cannot_be_healthy(void)
{
    int failures = 0;
    TEST("controllers: invalid core evidence stays unknown in operator summary") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_invalid_core_evidence);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "chain_evidence_unavailable");
        const struct json_value *height = json_get(&root, "height");
        const struct json_value *target = json_get(&root, "target_height");
        const struct json_value *gap = json_get(&root, "gap");
        ASSERT(height != NULL && height->type == JSON_NULL);
        ASSERT(target != NULL && target->type == JSON_NULL);
        ASSERT(gap != NULL && gap->type == JSON_NULL);
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "target_height_source")),
                      "unavailable");
        const struct json_value *peers = json_get(&root, "peers");
        ASSERT(peers != NULL && peers->type == JSON_OBJ);
        ASSERT(!json_get_bool(json_get(peers, "known")));
        const struct json_value *peer_total = json_get(peers, "total");
        ASSERT(peer_total != NULL && peer_total->type == JSON_NULL);
        ASSERT(json_get(&root, "chain_error") != NULL);
        ASSERT(json_get(&root, "syncdiag_error") != NULL);
        ASSERT(json_get(&root, "peers_error") != NULL);
        ASSERT(contains(json_get_str(json_get(&root, "summary")),
                        "height=unknown"));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_nonobject_peer_is_unknown(void)
{
    int failures = 0;
    TEST("controllers: non-object peer entries are not known peers") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_nonobject_peer_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "peer_state_unavailable");
        const struct json_value *peers = json_get(&root, "peers");
        ASSERT(peers != NULL && peers->type == JSON_OBJ);
        ASSERT(!json_get_bool(json_get(peers, "known")));
        const struct json_value *total = json_get(peers, "total");
        ASSERT(total != NULL && total->type == JSON_NULL);
        ASSERT(json_get(&root, "peers_error") != NULL);
        ASSERT(contains(json_get_str(json_get(&root, "summary")),
                        "peers=unknown"));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_zero_peers_at_tip_is_blocked(void)
{
    int failures = 0;
    TEST("controllers: zero peers blocks even when cached health says synced") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_zero_peers_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "blocked");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "no_peers");
        const struct json_value *peers = json_get(&root, "peers");
        ASSERT(peers != NULL && peers->type == JSON_OBJ);
        ASSERT(json_get_bool(json_get(peers, "known")));
        ASSERT(json_get_int(json_get(peers, "total")) == 0);
        ASSERT(!json_get_bool(json_get(peers, "max_height_known")));
        const struct json_value *max_height = json_get(peers, "max_height");
        ASSERT(max_height != NULL && max_height->type == JSON_NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_inconsistent_frontier_is_not_healthy(void)
{
    int failures = 0;
    TEST("controllers: contradictory frontier ordering cannot be healthy") {
        register_all();
        mcp_rpc_client_set_test_hook(
            mock_operator_inconsistent_frontier_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "chain_evidence_inconsistent");
        ASSERT(!json_get_bool(json_get(&root,
                                       "chain_evidence_consistent")));
        const struct json_value *gap = json_get(&root, "gap");
        const struct json_value *index_gap = json_get(&root, "index_gap");
        ASSERT(gap != NULL && gap->type == JSON_NULL);
        ASSERT(index_gap != NULL && index_gap->type == JSON_NULL);
        ASSERT(json_get(&root, "chain_evidence_error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_non_tip_state_cannot_be_healthy(void)
{
    int failures = 0;
    TEST("controllers: non-tip sync state cannot be healthy at zero gap") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_non_tip_zero_gap_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root, "gap")) == 0);
        ASSERT_STR_EQ(json_get_str(json_get(&root, "sync_state")),
                      "blocks_download");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "sync_not_at_tip");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_tool")),
                      "zcl_syncdiag");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_known_blocker_beats_telemetry_outage(void)
{
    int failures = 0;
    TEST("controllers: known not-serving reason beats blocker telemetry outage") {
        register_all();
        mcp_rpc_client_set_test_hook(
            mock_operator_not_serving_with_blocker_error);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "blocked");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "database_read_only");
        ASSERT(json_get(&root, "chain_error") != NULL);
        ASSERT(json_get(&root, "peers_error") != NULL);
        const struct json_value *blockers = json_get(&root, "blockers");
        const struct json_value *error = json_get(&root, "blockers_error");
        ASSERT(blockers != NULL && blockers->type == JSON_NULL);
        ASSERT(error != NULL && error->type == JSON_OBJ);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_served_gap_cannot_be_hidden_by_index_tip(void)
{
    int failures = 0;
    TEST("controllers: indexed tip cannot hide a lagging served H-star") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_served_lag_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT(!json_get_bool(json_get(&root, "healthy")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "download_queue_idle");
        ASSERT(json_get_int(json_get(&root, "served_height")) == 100);
        ASSERT(json_get_int(json_get(&root, "indexed_height")) == 110);
        ASSERT(json_get_int(json_get(&root, "target_height")) == 110);
        ASSERT(json_get_int(json_get(&root, "gap")) == 10);
        ASSERT(json_get_int(json_get(&root, "served_gap")) == 10);
        ASSERT(json_get_int(json_get(&root, "index_gap")) == 0);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_invalid_download_is_unknown(void)
{
    int failures = 0;
    TEST("controllers: invalid download counters never mean idle") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_invalid_download_rpc);
        for (int mode = MOCK_DOWNLOAD_MISSING_COUNTERS;
             mode <= MOCK_DOWNLOAD_NEGATIVE_COUNTER; mode++) {
            g_mock_download_failure_mode =
                (enum mock_download_failure_mode)mode;
            struct json_value args;
            json_init(&args);
            json_set_object(&args);
            char *body = mcp_router_dispatch("zcl_operator_summary", &args);
            ASSERT(body != NULL);

            struct json_value root;
            ASSERT(json_read(&root, body, strlen(body)));
            ASSERT_STR_EQ(json_get_str(json_get(&root, "status")),
                          "degraded");
            ASSERT_STR_EQ(json_get_str(json_get(&root,
                                                "primary_blocker")),
                          "download_state_unavailable");
            const struct json_value *download = json_get(&root, "download");
            ASSERT(download != NULL && download->type == JSON_OBJ);
            ASSERT(!json_get_bool(json_get(download, "known")));
            ASSERT(json_get(&root, "download_error") != NULL);
            ASSERT(!contains(body, "\"primary_blocker\":"
                                   "\"download_queue_idle\""));

            json_free(&root);
            json_free(&args);
            free(body);
        }
        mcp_rpc_client_set_test_hook(NULL);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_operator_summary_max_download_counters_do_not_overflow(void)
{
    int failures = 0;
    TEST("controllers: max download counters classify without overflow") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_max_download_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")),
                      "catching_up");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "chain_gap");
        const struct json_value *download = json_get(&root, "download");
        ASSERT(download != NULL && download->type == JSON_OBJ);
        ASSERT(json_get_bool(json_get(download, "known")));
        ASSERT(json_get_int(json_get(download, "in_flight")) == INT64_MAX);
        ASSERT(json_get_int(json_get(download, "queued")) == INT64_MAX);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_operator_summary_honors_mirror_contract(void)
{
    int failures = 0;
    TEST("controllers: zcl_operator_summary honors mirror contract blocker state") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_recovered_mirror_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "degraded");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "compatibility_snapshot_non_atomic");

        const struct json_value *mirror = json_get(&root, "mirror");
        ASSERT(mirror && mirror->type == JSON_OBJ);
        ASSERT(json_get_bool(json_get(mirror, "enabled")));
        ASSERT(json_get_bool(json_get(mirror, "reachable")));
        ASSERT(json_get_bool(json_get(mirror, "contract_trusted")));
        ASSERT(!json_get_bool(json_get(mirror, "blocker_active")));
        ASSERT(!json_get_bool(json_get(mirror,
                                       "operator_action_required")));
        ASSERT_STR_EQ(json_get_str(json_get(mirror, "blocker")), "");
        ASSERT_STR_EQ(json_get_str(json_get(mirror, "detail")), "");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_agent_contract_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_agent returns the bounded native agent contract") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_healthy_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_agent", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.public_status.v1");
        const struct json_value *lane = json_get(&root, "operator_lane");
        ASSERT(lane != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(lane, "schema")),
                      "zcl.operator_lane.v1");
        ASSERT_STR_EQ(json_get_str(json_get(lane, "lane")), "canonical");
        ASSERT(json_get_bool(json_get(lane,
                                      "requires_operator_confirmation")));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_agent_dev_tools_dispatch(void)
{
    int failures = 0;
    TEST("controllers: zcl_agent_* development tools dispatch") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_agent_dev_rpc);

        struct json_value args;
        json_init(&args);
        json_set_object(&args);

        char *body = mcp_router_dispatch("zcl_agent_map", &args);
        ASSERT(body != NULL);
        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_map.v1");
        ASSERT(json_get(&root, "commands") != NULL);
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_agent_lanes", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_lanes.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "default_deploy_target")),
                      "dev");
        ASSERT(json_get(&root, "current_runtime_services") != NULL);
        ASSERT(json_get(&root, "lanes") != NULL);
        json_free(&root);
        free(body);

        json_free(&args);
        const char *impact_args =
            "{\"files\":[\"app/controllers/src/agent_controller.c\","
            "\"tools/mcp/controllers/ops_controller.c\"]}";
        ASSERT(json_read(&args, impact_args, strlen(impact_args)));
        g_agent_impact_params_seen = false;
        body = mcp_router_dispatch("zcl_agent_impact", &args);
        ASSERT(body != NULL);
        ASSERT(g_agent_impact_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_impact.v1");
        ASSERT(json_get_bool(json_get(&root, "mcp_changed")));
        json_free(&root);
        free(body);

        json_free(&args);
        json_init(&args);
        json_set_object(&args);
        body = mcp_router_dispatch("zcl_agent_contracts", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_contracts.v1");
        ASSERT(json_get(&root, "schemas") != NULL);
        const struct json_value *summary =
            json_get(&root, "contract_summary");
        ASSERT(summary != NULL);
        ASSERT(json_get_int(json_get(summary, "contract_count")) >= 20);
        ASSERT(json_get_int(json_get(summary, "mcp_declared_count")) >= 20);
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_agent_build", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_build.v1");
        ASSERT(json_get(&root, "reproducible_release") != NULL);
        const struct json_value *loop = json_get(&root, "recommended_loop");
        ASSERT(loop != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(loop, "read_only_fast_plan")),
                      "make agent-plan");
        ASSERT_STR_EQ(json_get_str(json_get(loop, "dev_lane_status")),
                      "make agent-dev-status");
        ASSERT_STR_EQ(json_get_str(json_get(loop,
                                            "optional_dev_stage_no_restart")),
                      "ZCL_AGENT_LOOP_DEPLOY=stage make agent-loop");
        ASSERT_STR_EQ(json_get_str(json_get(loop,
                                            "immutable_history_canaries")),
                      "make immutable-history-canaries");
        ASSERT_STR_EQ(json_get_str(json_get(loop, "hot_mcp")),
                      "make agent-mcp-call-hot TOOL=<tool> [ARGS='{}']");
        const struct json_value *history =
            json_get(&root, "immutable_history_canaries");
        ASSERT(history != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(history, "fast_command")),
                      "make immutable-history-canaries");
        ASSERT(contains(json_get_str(json_get(history, "pinned_fixture")),
                        "h=478544"));
        const struct json_value *dev_bin =
            json_get(&root, "dev_node_binary");
        ASSERT(dev_bin != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(dev_bin, "status_command")),
                      "make agent-dev-status");
        const struct json_value *indexing = json_get(&root, "indexing");
        ASSERT(indexing != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(indexing, "schema")),
                      "zcl.agent_index_runtime.v1");
        ASSERT_STR_EQ(json_get_str(json_get(indexing, "command")),
                      "make agent-index");
        ASSERT(json_get(indexing, "freshness") != NULL);
        ASSERT(json_get(indexing, "clangd_optional") != NULL);
        const struct json_value *bench =
            json_get(&root, "dev_loop_benchmark");
        ASSERT(bench != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(bench, "schema")),
                      "zcl.dev_loop_bench.v1");
        ASSERT(json_get(bench, "slo") != NULL);
        json_free(&root);
        free(body);

        ASSERT(setenv("ZCL_AGENT_DEV_STATUS_CMD",
                      "printf '%s\\n' '{\"schema\":\"zcl.agent_dev_status.v1\","
                      "\"worker_lane\":{\"name\":\"dev\",\"role\":\"worker\","
                      "\"mutation_policy\":\"noncanonical_dev_only\","
                      "\"canonical_guard\":\"never_touches_live_or_soak\","
                      "\"stage_command\":\"make agent-stage-dev\","
                      "\"recover_command\":\"make lane-recover LANE=dev\"},"
                      "\"next_action\":\"unit-test\","
                      "\"service\":{\"active_state\":\"active\"},"
                      "\"rpc\":{\"status\":\"ok\"}}'",
                      1) == 0);
        body = mcp_router_dispatch("zcl_agent_dev_status", &args);
        unsetenv("ZCL_AGENT_DEV_STATUS_CMD");
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_dev_status.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "ok");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "native_command")),
                      "zclassic23 agentdevstatus");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_agent_dev_status");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_action")),
                      "unit-test");
        const struct json_value *worker = json_get(&root, "worker_lane");
        ASSERT(worker != NULL);
        ASSERT(worker->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(worker, "role")), "worker");
        ASSERT_STR_EQ(json_get_str(json_get(worker, "mutation_policy")),
                      "noncanonical_dev_only");
        ASSERT_STR_EQ(json_get_str(json_get(worker, "canonical_guard")),
                      "never_touches_live_or_soak");
        ASSERT_STR_EQ(json_get_str(json_get(worker, "stage_command")),
                      "make agent-stage-dev");
        ASSERT_STR_EQ(json_get_str(json_get(worker, "recover_command")),
                      "make lane-recover LANE=dev");
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_proof_bundle", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.operator_proof_bundle.v1");
        ASSERT(json_get(&root, "agent") != NULL);
        ASSERT(json_get(&root, "milestone") != NULL);
        ASSERT(json_get(&root, "anchor_status") != NULL);
        ASSERT(json_get(&root, "dev_status") != NULL);
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_agent_interface", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_interface.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "preferred_transport")),
                      "mcp");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "build_commit")),
                      "nodecafe123");
        ASSERT(json_get(&root, "capabilities") != NULL);
        ASSERT(json_get(&root, "machine_contract") != NULL);
        const struct json_value *runtime =
            json_get(&root, "runtime_identity");
        ASSERT(runtime != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(runtime, "schema")),
                      "zcl.agent_runtime_identity.v1");
        ASSERT_STR_EQ(json_get_str(json_get(runtime, "build_commit")),
                      "nodecafe123");
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_agent_ops", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_ops.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "method")),
                      "agentops");
        ASSERT(json_get_bool(json_get(&root, "no_jq_required")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "native_command")),
                      "zclassic23 agentops");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_agent_ops");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "contract_source")),
                      "agent_contracts.def");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "api_style")),
                      "one compact first call, then registry-owned primitive drilldowns");
        ASSERT(json_get(&root, "api_ux") != NULL);
        ASSERT(json_get(&root, "workflow") != NULL);
        ASSERT(json_get(&root, "top_next_work") != NULL);
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_app_protocols", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.application_protocols.index.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "base_layer")),
                      "zclassic_l1");
        ASSERT(json_get(&root, "protocols") != NULL);
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_service_catalog", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.service_catalog.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "base_layer")),
                      "zclassic_l1");
        ASSERT(json_get(&root, "services") != NULL);
        const struct json_value *ux = json_get(&root, "sovereign_ux");
        ASSERT(ux != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(ux, "schema")),
                      "zcl.sovereign_ux_contract.v1");
        ASSERT(json_array_has_str(json_get(ux, "flow"),
                                  "resolve_znam_name"));
        json_free(&root);
        free(body);

        const char *service_args = "{\"name\":\"bootstrap\"}";
        json_free(&args);
        json_init(&args);
        ASSERT(json_read(&args, service_args, strlen(service_args)));
        g_service_catalog_name_params_seen = false;
        body = mcp_router_dispatch("zcl_service_catalog", &args);
        ASSERT(body != NULL);
        ASSERT(g_service_catalog_name_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.service_contract.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "name")), "bootstrap");
        const struct json_value *ops = json_get(&root, "operations");
        ASSERT(ops && ops->type == JSON_ARR && json_size(ops) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(ops, 0), "mcp_tool")),
                      "zcl_bootstrapstatus");
        ASSERT(json_array_has_str(json_get(&root, "depends_on_services"),
                                  "full_node"));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "read_model")),
                      "network_bootstrap_status_and_peer_projection");
        json_free(&root);
        free(body);

        json_free(&args);
        json_init(&args);
        json_set_object(&args);
        body = mcp_router_dispatch("zcl_service_operations", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.service_operations.index.v1");
        ASSERT(json_get(&root, "summary") != NULL);
        ops = json_get(&root, "operations");
        ASSERT(ops && ops->type == JSON_ARR && json_size(ops) == 2);
        json_free(&root);
        free(body);

        const char *operation_filter_args =
            "{\"service\":\"bootstrap\","
            "\"write_safety\":\"public_read_only\"}";
        json_free(&args);
        json_init(&args);
        ASSERT(json_read(&args, operation_filter_args,
                         strlen(operation_filter_args)));
        g_service_operations_filter_params_seen = false;
        body = mcp_router_dispatch("zcl_service_operations", &args);
        ASSERT(body != NULL);
        ASSERT(g_service_operations_filter_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.service_operations.index.v1");
        const struct json_value *filters = json_get(&root, "filters");
        ASSERT(filters && json_get_bool(json_get(filters, "active")));
        ASSERT_STR_EQ(json_get_str(json_get(filters, "service")),
                      "bootstrap");
        ASSERT(json_get_int(json_get(json_get(&root, "summary"),
                                     "operation_count")) == 2);
        ops = json_get(&root, "operations");
        ASSERT(ops && ops->type == JSON_ARR && json_size(ops) == 2);
        json_free(&root);
        free(body);

        const char *operation_args =
            "{\"operation_id\":\"bootstrap.read_bootstrap_status\"}";
        json_free(&args);
        json_init(&args);
        ASSERT(json_read(&args, operation_args, strlen(operation_args)));
        g_service_operations_id_params_seen = false;
        body = mcp_router_dispatch("zcl_service_operations", &args);
        ASSERT(body != NULL);
        ASSERT(g_service_operations_id_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.service_operation.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "operation_id")),
                      "bootstrap.read_bootstrap_status");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_bootstrapstatus");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "write_safety")),
                      "public_read_only");
        json_free(&root);
        json_free(&args);
        json_init(&args);
        json_set_object(&args);
        free(body);

        body = mcp_router_dispatch("zcl_agent_diagnose", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_diagnose.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "method")),
                      "agentdiagnose");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "native_command")),
                      "zclassic23 agentdiagnose");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_agent_diagnose");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "contract_source")),
                      "agent_contracts.def");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "detail_mode")),
                      "brief");
        ASSERT(!json_get_bool(json_get(&root, "embedded_drilldowns")));
        ASSERT(json_get(&root, "first_call") != NULL);
        ASSERT(json_get(&root, "peer_incidents") == NULL);
        json_free(&root);
        free(body);

        json_free(&args);
        const char *diagnose_args = "{\"mode\":\"brief\"}";
        ASSERT(json_read(&args, diagnose_args, strlen(diagnose_args)));
        g_agent_diagnose_brief_params_seen = false;
        body = mcp_router_dispatch("zcl_agent_diagnose", &args);
        ASSERT(body != NULL);
        ASSERT(g_agent_diagnose_brief_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_diagnose.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "method")),
                      "agentdiagnose");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "native_command")),
                      "zclassic23 agentdiagnose");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_agent_diagnose");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "contract_source")),
                      "agent_contracts.def");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "detail_mode")),
                      "brief");
        ASSERT(!json_get_bool(json_get(&root, "embedded_drilldowns")));
        ASSERT(json_get(&root, "peer_incidents") == NULL);
        json_free(&root);
        free(body);

        json_free(&args);
        const char *diagnose_full_args = "{\"mode\":\"full\"}";
        ASSERT(json_read(&args, diagnose_full_args,
                         strlen(diagnose_full_args)));
        g_agent_diagnose_full_params_seen = false;
        body = mcp_router_dispatch("zcl_agent_diagnose", &args);
        ASSERT(body != NULL);
        ASSERT(g_agent_diagnose_full_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_diagnose.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "detail_mode")),
                      "full");
        ASSERT(json_get_bool(json_get(&root, "embedded_drilldowns")));
        ASSERT(json_get(&root, "peer_incidents") != NULL);
        json_free(&root);
        free(body);

        json_free(&args);
        json_init(&args);
        json_set_object(&args);
        g_agent_liveness_brief_params_seen = false;
        body = mcp_router_dispatch("zcl_agent_liveness", &args);
        ASSERT(body != NULL);
        ASSERT(g_agent_liveness_brief_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_liveness.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "method")),
                      "agentliveness");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "native_command")),
                      "zclassic23 agentliveness");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_agent_liveness");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "contract_source")),
                      "agent_contracts.def");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "detail_mode")),
                      "brief");
        ASSERT(!json_get_bool(json_get(&root, "embedded_drilldowns")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "overall_liveness")),
                      "active");
        ASSERT(json_get(&root, "first_call") != NULL);
        ASSERT(json_get(json_get(&root, "runtime_availability"),
                        "methods") == NULL);
        ASSERT(json_get(json_get(&root, "background_quality_status"),
                        "lanes") == NULL);
        ASSERT(json_get(json_get(&root, "supervisor_state"),
                        "domains") == NULL);
        ASSERT(json_get(&root, "liveness_summary") != NULL);
        ASSERT(json_get(&root, "recommended_drilldowns") != NULL);
        json_free(&root);
        free(body);

        json_free(&args);
        const char *liveness_full_args = "{\"mode\":\"full\"}";
        ASSERT(json_read(&args, liveness_full_args,
                         strlen(liveness_full_args)));
        g_agent_liveness_full_params_seen = false;
        body = mcp_router_dispatch("zcl_agent_liveness", &args);
        ASSERT(body != NULL);
        ASSERT(g_agent_liveness_full_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_liveness.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "detail_mode")),
                      "full");
        ASSERT(json_get_bool(json_get(&root, "embedded_drilldowns")));
        ASSERT(json_get(json_get(&root, "runtime_availability"),
                        "methods") != NULL);
        ASSERT(json_get(json_get(&root, "background_quality_status"),
                        "lanes") != NULL);
        ASSERT(json_get(json_get(&root, "supervisor_state"),
                        "domains") != NULL);
        json_free(&root);
        free(body);

        json_free(&args);
        const char *timeline_args =
            "{\"category\":\"sync\",\"count\":5,\"scan_count\":16,"
            "\"since_secs\":3600,\"peer\":7,\"height\":42,"
            "\"reducer_stage\":\"body_fetch\","
            "\"condition\":\"download_queue_starved\","
            "\"deploy\":\"make-deploy\",\"lane\":\"dev\"}";
        ASSERT(json_read(&args, timeline_args, strlen(timeline_args)));
        g_agent_timeline_params_seen = false;
        body = mcp_router_dispatch("zcl_timeline", &args);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.timeline.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_timeline");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "category")),
                      "sync");
        ASSERT(json_get(&root, "events") != NULL);
        ASSERT(json_get(&root, "filters") != NULL);
        ASSERT(g_agent_timeline_params_seen);
        json_free(&root);
        free(body);

        json_free(&args);
        const char *guard_args = "{\"action\":\"canonical-deploy\"}";
        ASSERT(json_read(&args, guard_args, strlen(guard_args)));
        g_agent_deploy_guard_params_seen = false;
        body = mcp_router_dispatch("zcl_agent_deploy_guard", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT(g_agent_deploy_guard_params_seen);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_deploy_guard.v1");
        ASSERT(!json_get_bool(json_get(&root, "allowed")));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_operator_summary_names_operator_needed_detail(void)
{
    int failures = 0;
    TEST("controllers: zcl_operator_summary names operator-needed detail") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_needed_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_operator_summary", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")),
                      "operator_needed");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "operator_needed:chain_integrity_failed");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "blocking_reason")),
                      "operator_needed:chain_integrity_failed");
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "operator_needed_detail")),
                      "chain_integrity_failed");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_tool")),
                      "zcl_conditions");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_milestone_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_milestone proxies node-computed bars") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_healthy_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_milestone", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.milestone_status.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "milestone")),
                      "v1 MVP");
        ASSERT(json_get_int(json_get(&root, "mvp_readiness_score")) == 4);
        ASSERT(strstr(json_get_str(json_get(json_get(&root, "ascii"),
                                            "goals")),
                      "goals [#####-----] 4/8") != NULL);
        const struct json_value *operator_proofs =
            json_get(&root, "operator_proofs");
        ASSERT(operator_proofs != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(operator_proofs, "schema")),
                      "zcl.mvp_operator_proofs.v1");
        ASSERT(json_get_int(json_get(operator_proofs, "accepted_count")) == 4);
        ASSERT(json_get_int(json_get(operator_proofs, "pending_count")) == 4);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_refold_status_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_refold_status proxies node readiness") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_healthy_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_refold_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.refold_status.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "api_version")), "v1");
        ASSERT(!json_get_bool(json_get(&root, "ready_for_refold")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "missing_verified_anchor_snapshot");
        ASSERT_STR_EQ(json_get_str(json_get(json_get(&root, "commands"),
                                            "native")),
                      "zclassic23 refold");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_getblockcount_uses_node_hstar_rpc(void)
{
    int failures = 0;
    TEST("controllers: zcl_getblockcount uses node RPC H*") {
        register_all();

        char tmpl[] = "/tmp/zcl-mcp-blockcount-XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT(dir != NULL);
        ASSERT(seed_mcp_projection_height(dir, 100));
        mcp_rpc_client_init(dir, 0);
        mcp_rpc_client_set_test_hook(mock_status_rpc);

        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_getblockcount", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT_STR_EQ(body, "3117073");

        json_free(&args);
        free(body);
        test_rm_rf_recursive(dir);
        mcp_rpc_client_init("", 0);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    mcp_rpc_client_init("", 0);
    return failures;
}

enum mock_dumpstate_failure_mode {
    MOCK_DUMPSTATE_WRAPPED_ERROR,
    MOCK_DUMPSTATE_BARE_ERROR,
    MOCK_DUMPSTATE_WRONG_SUBSYSTEM,
};

static enum mock_dumpstate_failure_mode g_mock_dumpstate_failure_mode;

static char *mock_status_rpc_dumpstate_error(const char *method,
                                             const char *params_json)
{
    if (strcmp(method, "dumpstate") == 0 && params_json &&
        strstr(params_json, "reducer_frontier") != NULL) {
        if (g_mock_dumpstate_failure_mode == MOCK_DUMPSTATE_WRAPPED_ERROR)
            return strdup("{\"error\":{\"code\":-32603,"
                          "\"message\":\"reducer frontier unavailable\"}}");
        if (g_mock_dumpstate_failure_mode == MOCK_DUMPSTATE_BARE_ERROR)
            return strdup("{\"code\":-32603,"
                          "\"message\":\"reducer frontier unavailable\","
                          "\"method\":\"dumpstate\"}");
        return strdup("{\"subsystem\":\"supervisor\","
                      "\"captured_at\":1782240015,"
                      "\"state\":{\"healthy\":true}}");
    }
    return mock_status_rpc(method, params_json);
}

static int test_zcl_status_includes_chain_advance_dump(void)
{
    int failures = 0;
    TEST("controllers: zcl_status includes chain advance coordinator dump") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        /* build_commit reports the NODE's hash (scraped from healthcheck);
         * the MCP process's own hash appears as mcp_build_commit only when
         * the two differ — the MCP server can outlive a node redeploy. */
        ASSERT_STR_EQ(json_get_str(json_get(&root, "build_commit")),
                      "nodecafe123");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_build_commit")),
                      zcl_build_commit());
        ASSERT_STR_EQ(json_get_str(json_get(&root, "build_commit_source")),
                      "target_node.healthcheck");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "execution_locus")),
                      "composite");
        const struct json_value *blockers = json_get(&root, "blockers");
        ASSERT(blockers != NULL);
        ASSERT(json_get_int(json_get(blockers, "active_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "permanent_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "transient_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "dependency_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "resource_count")) == 0);
        ASSERT_STR_EQ(json_get_str(json_get(blockers, "execution_locus")),
                      "target_node");
        ASSERT_STR_EQ(json_get_str(json_get(blockers, "source_rpc")),
                      "dumpstate:blocker");
        ASSERT(json_get_int(json_get(blockers, "captured_at")) ==
               1782240005);
        ASSERT(json_get(blockers, "blockers") == NULL);
        ASSERT(json_get(blockers, "_health") == NULL);
        const struct json_value *empty_dominant =
            json_get(blockers, "dominant");
        const struct json_value *empty_top_dominant =
            json_get(&root, "dominant_blocker");
        ASSERT(empty_dominant != NULL &&
               empty_dominant->type == JSON_NULL);
        ASSERT(empty_top_dominant != NULL &&
               empty_top_dominant->type == JSON_NULL);
        const struct json_value *connections = json_get(&root, "connections");
        ASSERT(connections != NULL);
        ASSERT(json_get_int(json_get(connections, "total")) == 1);
        ASSERT(json_get_int(json_get(connections, "inbound")) == 0);
        ASSERT(json_get_int(json_get(connections, "outbound")) == 1);
        ASSERT(json_get_int(json_get(connections, "zcl23")) == 1);
        ASSERT(json_get_int(json_get(connections, "magicbean")) == 0);
        ASSERT(json_get_int(json_get(&root, "max_peer_height")) == 3117074);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &root, "max_peer_height_trust")),
                      "untrusted_peer_advertisement");
        ASSERT(json_get_int(json_get(&root, "header_gap")) == 0);
        ASSERT(!json_get_bool(json_get(&root, "header_sync_behind")));
        ASSERT(json_get_int(json_get(&root, "target_height")) == 3117074);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &root, "target_height_source")),
                      "target_node.validated_header_tip");
        ASSERT(json_get_int(json_get(&root, "sync_gap")) == 1);
        ASSERT(json_get_int(json_get(
                   &root, "sync_behind_threshold_blocks")) == 144);
        ASSERT(!json_get_bool(json_get(&root, "sync_behind")));
        const struct json_value *chain_advance =
            json_get(&root, "chain_advance");
        ASSERT(chain_advance != NULL);
        ASSERT(json_get_bool(json_get(chain_advance, "initialized")));
        ASSERT(json_get(chain_advance, "state") == NULL);
        ASSERT(json_get_bool(json_get(chain_advance, "has_connman")));
        ASSERT(json_get_bool(json_get(chain_advance, "has_main_state")));
        ASSERT(json_get_bool(json_get(chain_advance, "has_node_db")));
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance, "authority")),
                      "local_consensus_validation");
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance,
                                            "selected_source")),
                      "p2p");
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance,
                                            "selected_source_trust")),
                      "native_peer_validated");
        ASSERT(json_get_bool(json_get(chain_advance,
                                      "selected_source_selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(chain_advance,
                                            "selected_source_selection_blocker")),
                      "");
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_base")) == 100);
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_health")) == 20);
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_height")) == 10);
        ASSERT(json_get_int(json_get(chain_advance,
                                     "selected_source_score_authorized")) == 0);
        ASSERT(json_get_int(json_get(
                   chain_advance,
                   "selected_source_score_redundancy_bonus")) == 0);
        ASSERT(json_get_int(json_get(
                   chain_advance,
                   "selected_source_score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   chain_advance,
                   "selected_source_score_failure_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   chain_advance,
                   "selected_source_score_mirror_gate_penalty")) == 0);
        ASSERT(json_get_bool(json_get(chain_advance,
                                      "has_last_decision")));
        const struct json_value *last =
            json_get(chain_advance, "last_decision");
        ASSERT(last != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(last, "op")), "peer_floor");
        ASSERT_STR_EQ(json_get_str(json_get(last, "selected_source_trust")),
                      "native_peer_validated");
        ASSERT(json_get_bool(json_get(last, "selected_source_selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(
                          last, "selected_source_selection_blocker")), "");
        ASSERT(json_get_int(json_get(last,
                                     "selected_source_score_base")) == 100);
        ASSERT(json_get_int(json_get(last,
                                     "selected_source_score_health")) == 20);
        ASSERT(json_get_int(json_get(last,
                                     "selected_source_score_height")) == 10);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_authorized")) == 0);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_redundancy_bonus")) == 0);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_failure_penalty")) == 0);
        ASSERT(json_get_int(json_get(
                   last, "selected_source_score_mirror_gate_penalty")) == 0);
        const char *last_reason =
            json_get_str(json_get(last, "selected_source_reason"));
        ASSERT(last_reason != NULL);
        ASSERT(contains(last_reason, "healthy=3"));
        const struct json_value *last_sources = json_get(last, "sources");
        ASSERT(last_sources != NULL);
        ASSERT(json_size(last_sources) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(last_sources, 0),
                                            "source")),
                      "p2p");
        ASSERT_STR_EQ(json_get_str(json_get(json_at(last_sources, 0),
                                            "trust")),
                      "native_peer_validated");
        ASSERT(json_get_bool(json_get(json_at(last_sources, 0),
                                      "selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(json_at(last_sources, 0),
                                            "selection_blocker")), "");
        ASSERT(json_get_int(json_get(json_at(last_sources, 0),
                                     "score_base")) == 100);
        ASSERT(json_get_int(json_get(json_at(last_sources, 0),
                                     "score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(json_at(last_sources, 0),
                                     "score_failure_penalty")) == 0);
        ASSERT(contains(json_get_str(json_get(json_at(last_sources, 0),
                                              "reason")),
                        "healthy=3"));
        const struct json_value *sources = json_get(chain_advance, "sources");
        ASSERT(sources != NULL);
        ASSERT(json_size(sources) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0), "trust")),
                      "native_peer_validated");
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0), "state")),
                      "healthy");
        ASSERT(json_get_bool(json_get(json_at(sources, 0), "selectable")));
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0),
                                            "selection_blocker")), "");
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "score_base")) == 100);
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "score_target_lag_penalty")) == 0);
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "score_failure_penalty")) == 0);
        const struct json_value *frontier =
            json_get(&root, "reducer_frontier");
        ASSERT(frontier != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(frontier, "authority")),
                      "reducer_frontier_hstar");
        ASSERT(json_get(frontier, "state") == NULL);
        ASSERT(json_get_int(json_get(frontier, "hstar")) == 3157646);
        ASSERT(json_get_int(json_get(frontier,
                                     "first_validate_failure_height"))
               == 3157647);
        ASSERT_STR_EQ(json_get_str(json_get(
                          frontier, "first_validate_failure_reason")),
                      "header-source-hash-mismatch");
        ASSERT_STR_EQ(json_get_str(json_get(
                          frontier, "first_validate_failure_repair_owner")),
                      "stale_validate_headers_repair");
        const struct json_value *tip_finalize =
            json_get(&root, "tip_finalize");
        ASSERT(tip_finalize != NULL);
        ASSERT(json_get(tip_finalize, "state") == NULL);
        ASSERT(json_get_int(json_get(tip_finalize, "cursor")) == 3157646);
        ASSERT(json_get_int(json_get(
                   tip_finalize, "last_precondition_height")) == 3157646);
        ASSERT_STR_EQ(json_get_str(json_get(
                          tip_finalize, "last_precondition_reason")),
                      "have_data_missing");
        ASSERT(json_get_int(json_get(
                   tip_finalize, "precondition_repeat_count")) == 7);
        const struct json_value *condition_engine =
            json_get(&root, "condition_engine");
        ASSERT(condition_engine != NULL);
        ASSERT(json_get_int(json_get(condition_engine,
                                     "registered_count")) == 28);
        ASSERT(json_get(condition_engine, "state") == NULL);
        ASSERT(json_get_int(json_get(condition_engine,
                                     "active_count")) == 2);
        ASSERT(json_get_int(json_get(condition_engine,
                                     "unresolved_count")) == 1);
        const struct json_value *conditions =
            json_get(condition_engine, "conditions");
        ASSERT(conditions != NULL);
        ASSERT(json_size(conditions) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(conditions, 0), "name")),
                      "stale_validate_headers_repair");
        ASSERT(json_get_bool(json_get(json_at(conditions, 0),
                                      "operator_needed_emitted")));
        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_reports_dumpstate_error(void)
{
    int failures = 0;
    TEST("controllers: zcl_status rejects dumpstate errors and wrong locus") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_dumpstate_error);
        for (int mode = MOCK_DUMPSTATE_WRAPPED_ERROR;
             mode <= MOCK_DUMPSTATE_WRONG_SUBSYSTEM; mode++) {
            g_mock_dumpstate_failure_mode =
                (enum mock_dumpstate_failure_mode)mode;
            struct json_value args;
            json_init(&args);
            json_set_object(&args);
            char *body = mcp_router_dispatch("zcl_status", &args);
            ASSERT(body != NULL);

            struct json_value root;
            ASSERT(json_read(&root, body, strlen(body)));
            const struct json_value *frontier =
                json_get(&root, "reducer_frontier");
            const struct json_value *err =
                json_get(&root, "reducer_frontier_error");
            ASSERT(frontier != NULL && frontier->type == JSON_NULL);
            ASSERT(err != NULL && err->type == JSON_OBJ);
            if (mode != MOCK_DUMPSTATE_WRONG_SUBSYSTEM) {
                ASSERT(json_get_int(json_get(err, "code")) == -32603);
                ASSERT_STR_EQ(json_get_str(json_get(err, "message")),
                              "reducer frontier unavailable");
            } else {
                ASSERT(contains(json_get_str(json_get(err, "message")),
                                "invalid subsystem envelope"));
            }
            json_free(&root);
            json_free(&args);
            free(body);
        }
        mcp_rpc_client_set_test_hook(NULL);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_target_blocker_failure_never_falls_back_to_proxy(void)
{
    int failures = 0;
    TEST("controllers: target blocker failure never becomes proxy zero/local") {
        register_all();
        blocker_module_init();
        blocker_reset_for_testing();
        blocker_set_rate_limit_ms_for_testing(0);
        blocker_set_clock_for_testing(1000000);

        struct blocker_record proxy_only;
        ASSERT(blocker_init(&proxy_only, "proxy-only", "mcp",
                            BLOCKER_RESOURCE,
                            "must not mask target RPC failure"));
        ASSERT(blocker_set(&proxy_only) == 0);

        mcp_rpc_client_set_test_hook(mock_status_rpc_blocker_error);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *missing_blockers =
            json_get(&root, "blockers");
        const struct json_value *missing_dominant =
            json_get(&root, "dominant_blocker");
        ASSERT(missing_blockers != NULL &&
               missing_blockers->type == JSON_NULL);
        ASSERT(missing_dominant != NULL &&
               missing_dominant->type == JSON_NULL);
        const struct json_value *error = json_get(&root, "blockers_error");
        ASSERT(error != NULL);
        ASSERT(json_get_int(json_get(error, "code")) == -32603);
        ASSERT_STR_EQ(json_get_str(json_get(error, "message")),
                      "target blocker state unavailable");
        ASSERT(!contains(body, "proxy-only"));

        char *blockers_body = mcp_router_dispatch("zcl_blockers", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(blockers_body != NULL);
        struct json_value blockers_error_root;
        ASSERT(json_read(&blockers_error_root, blockers_body,
                         strlen(blockers_body)));
        const struct json_value *blockers_error =
            json_get(&blockers_error_root, "error");
        ASSERT(blockers_error != NULL &&
               blockers_error->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(blockers_error, "code")),
                      "HANDLER_FAILED");
        ASSERT_STR_EQ(json_get_str(json_get(blockers_error, "tool")),
                      "zcl_blockers");
        ASSERT(contains(json_get_str(json_get(blockers_error, "message")),
                        "target blocker state unavailable"));
        ASSERT(json_get(&blockers_error_root, "active_count") == NULL);
        ASSERT(!contains(blockers_body, "proxy-only"));

        json_free(&blockers_error_root);
        json_free(&root);
        json_free(&args);
        free(blockers_body);
        free(body);
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(0);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    blocker_reset_for_testing();
    blocker_set_clock_for_testing(0);
    return failures;
}

static int test_target_blocker_failure_matrix_fails_closed(void)
{
    int failures = 0;
    TEST("controllers: malformed target blocker evidence always fails closed") {
        register_all();
        blocker_module_init();
        blocker_reset_for_testing();
        blocker_set_rate_limit_ms_for_testing(0);
        blocker_set_clock_for_testing(1000000);
        struct blocker_record proxy_only;
        ASSERT(blocker_init(&proxy_only, "proxy-only", "mcp",
                            BLOCKER_RESOURCE, "must never be fallback"));
        ASSERT(blocker_set(&proxy_only) == 0);

        mcp_rpc_client_set_test_hook(
            mock_status_rpc_blocker_failure_matrix);
        for (int mode = MOCK_BLOCKER_WRAPPED_ERROR;
             mode <= MOCK_BLOCKER_NULL_RESULT; mode++) {
            g_mock_blocker_failure_mode =
                (enum mock_blocker_failure_mode)mode;
            struct json_value args;
            json_init(&args);
            json_set_object(&args);
            char *body = mcp_router_dispatch("zcl_status", &args);
            ASSERT(body != NULL);

            struct json_value root;
            ASSERT(json_read(&root, body, strlen(body)));
            const struct json_value *blockers = json_get(&root, "blockers");
            const struct json_value *dominant =
                json_get(&root, "dominant_blocker");
            const struct json_value *error =
                json_get(&root, "blockers_error");
            ASSERT(blockers != NULL && blockers->type == JSON_NULL);
            ASSERT(dominant != NULL && dominant->type == JSON_NULL);
            ASSERT(error != NULL && error->type == JSON_OBJ);
            ASSERT(!contains(body, "proxy-only"));

            json_free(&root);
            json_free(&args);
            free(body);
        }

        mcp_rpc_client_set_test_hook(NULL);
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(0);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    blocker_reset_for_testing();
    blocker_set_clock_for_testing(0);
    return failures;
}

static int test_zcl_status_sync_gap_uses_served_tip(void)
{
    int failures = 0;
    TEST("controllers: zcl_status sync lag is target minus served H-star") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_lagged);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root, "height")) == 1000);
        ASSERT(json_get_int(json_get(&root, "header_height")) == 1300);
        ASSERT(json_get_int(json_get(&root, "target_height")) == 1300);
        ASSERT(json_get_int(json_get(&root, "header_gap")) == 0);
        ASSERT(!json_get_bool(json_get(&root, "header_sync_behind")));
        ASSERT(json_get_int(json_get(&root, "sync_gap")) == 300);
        ASSERT(json_get_bool(json_get(&root, "sync_behind")));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_peer_claim_cannot_set_target(void)
{
    int failures = 0;
    TEST("controllers: peer-advertised height cannot set sync target") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_spoofed_peer_height);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root, "max_peer_height")) ==
               2000000000);
        ASSERT(json_get_int(json_get(&root, "header_height")) == 1300);
        ASSERT(json_get_int(json_get(&root, "target_height")) == 1300);
        ASSERT(json_get_int(json_get(&root, "sync_gap")) == 300);
        ASSERT_STR_EQ(json_get_str(json_get(
                          &root, "max_peer_height_trust")),
                      "untrusted_peer_advertisement");
        ASSERT_STR_EQ(json_get_str(json_get(
                          &root, "target_height_source")),
                      "target_node.validated_header_tip");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_failed_sync_inputs_are_unknown(void)
{
    int failures = 0;
    TEST("controllers: failed sync RPCs stay null, never authoritative zero") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_sync_inputs_failed);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const char *const unknown_keys[] = {
            "height", "header_height", "max_peer_height", "header_gap",
            "header_sync_behind", "target_height", "sync_gap",
            "sync_behind", "peers",
        };
        for (size_t i = 0;
             i < sizeof(unknown_keys) / sizeof(unknown_keys[0]); i++) {
            const struct json_value *v = json_get(&root, unknown_keys[i]);
            ASSERT(v != NULL);
            ASSERT(v->type == JSON_NULL);
        }
        const struct json_value *height_error =
            json_get(&root, "height_error");
        const struct json_value *peers_error =
            json_get(&root, "peers_error");
        const struct json_value *header_error =
            json_get(&root, "header_height_error");
        ASSERT(height_error != NULL && height_error->type == JSON_OBJ);
        ASSERT(peers_error != NULL && peers_error->type == JSON_OBJ);
        ASSERT(header_error != NULL && header_error->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(height_error, "message")),
                      "height unavailable");
        ASSERT_STR_EQ(json_get_str(json_get(peers_error, "message")),
                      "peers unavailable");
        ASSERT_STR_EQ(json_get_str(json_get(header_error, "message")),
                      "headers unavailable");
        const struct json_value *connections =
            json_get(&root, "connections");
        ASSERT(connections != NULL && connections->type == JSON_OBJ);
        ASSERT(!json_get_bool(json_get(connections, "known")));
        const struct json_value *total = json_get(connections, "total");
        ASSERT(total != NULL && total->type == JSON_NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_labels_build_commit_fallback(void)
{
    int failures = 0;
    TEST("controllers: zcl_status labels proxy build fallback honestly") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_without_node_commit);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *build_commit =
            json_get(&root, "build_commit");
        ASSERT(build_commit != NULL);
        ASSERT(build_commit->type == JSON_NULL);
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_build_commit")),
                      zcl_build_commit());
        ASSERT_STR_EQ(json_get_str(json_get(&root, "build_commit_source")),
                      "target_node.unavailable");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_negative_health_metrics_are_unknown(void)
{
    int failures = 0;
    TEST("controllers: negative health metrics are unknown, not evidence") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_negative_health_metrics);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *memory = json_get(&root, "memory_rss_mb");
        const struct json_value *uptime = json_get(&root, "uptime_secs");
        ASSERT(memory != NULL && memory->type == JSON_NULL);
        ASSERT(uptime != NULL && uptime->type == JSON_NULL);
        ASSERT(json_get(&root, "memory_rss_mb_error") != NULL);
        ASSERT(json_get(&root, "uptime_secs_error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_inconsistent_frontier_is_not_zero_gap(void)
{
    int failures = 0;
    TEST("controllers: served height above header is inconsistent, not synced") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_inconsistent_frontier);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root, "height")) == 111);
        ASSERT(json_get_int(json_get(&root, "header_height")) == 110);
        ASSERT(!json_get_bool(json_get(&root,
                                       "chain_evidence_consistent")));
        const struct json_value *gap = json_get(&root, "sync_gap");
        const struct json_value *behind = json_get(&root, "sync_behind");
        ASSERT(gap != NULL && gap->type == JSON_NULL);
        ASSERT(behind != NULL && behind->type == JSON_NULL);
        ASSERT(json_get(&root, "chain_evidence_error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_missing_peer_height_is_unknown(void)
{
    int failures = 0;
    TEST("controllers: absent peer height claim is unknown, not zero") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_peer_without_height);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root, "peers")) == 1);
        ASSERT(!json_get_bool(json_get(&root, "max_peer_height_known")));
        const struct json_value *peer_height =
            json_get(&root, "max_peer_height");
        const struct json_value *header_gap = json_get(&root, "header_gap");
        ASSERT(peer_height != NULL && peer_height->type == JSON_NULL);
        ASSERT(header_gap != NULL && header_gap->type == JSON_NULL);
        ASSERT(json_get(&root, "max_peer_height_error") != NULL);
        ASSERT(json_get(&root, "header_gap_error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_missing_peer_direction_is_unknown(void)
{
    int failures = 0;
    TEST("controllers: missing peer direction never means outbound") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_peer_without_direction);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *connections =
            json_get(&root, "connections");
        ASSERT(connections != NULL && connections->type == JSON_OBJ);
        ASSERT(!json_get_bool(json_get(connections, "known")));
        ASSERT(json_get_bool(json_get(connections, "total_known")));
        ASSERT(!json_get_bool(json_get(connections, "direction_known")));
        ASSERT(json_get_int(json_get(connections, "total")) == 1);
        const struct json_value *inbound = json_get(connections, "inbound");
        const struct json_value *outbound = json_get(connections, "outbound");
        ASSERT(inbound != NULL && inbound->type == JSON_NULL);
        ASSERT(outbound != NULL && outbound->type == JSON_NULL);
        ASSERT(json_get(connections, "direction_error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_uses_target_blockers_not_proxy_registry(void)
{
    int failures = 0;
    TEST("controllers: zcl_status uses target blockers, not proxy globals") {
        register_all();
        blocker_module_init();
        blocker_reset_for_testing();
        blocker_set_rate_limit_ms_for_testing(0);
        blocker_set_clock_for_testing(1000000);

        struct blocker_record proxy_only;
        ASSERT(blocker_init(&proxy_only, "proxy-only", "mcp",
                            BLOCKER_PERMANENT,
                            "must never leak into target status"));
        ASSERT(blocker_set(&proxy_only) == 0);

        g_target_blocker_rpc_calls = 0;
        mcp_rpc_client_set_test_hook(mock_status_rpc_with_target_blockers);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *blockers = json_get(&root, "blockers");
        ASSERT(blockers != NULL);
        ASSERT(json_get_int(json_get(blockers, "active_count")) == 2);
        ASSERT(json_get_int(json_get(blockers, "transient_count")) == 1);
        ASSERT(json_get_int(json_get(blockers, "resource_count")) == 1);
        ASSERT(json_get_int(json_get(blockers,
                                     "escape_dispatched_total")) == 7);
        ASSERT(json_get_int(json_get(blockers, "rate_limit_ms")) == 250);
        ASSERT_STR_EQ(json_get_str(json_get(blockers, "execution_locus")),
                      "target_node");
        ASSERT(g_target_blocker_rpc_calls == 1);
        ASSERT(!contains(body, "proxy-only"));

        const struct json_value *dominant =
            json_get(&root, "dominant_blocker");
        ASSERT(dominant != NULL);
        ASSERT(!json_is_null(dominant));
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "id")),
                      "target-disk-full");
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "owner")),
                      "storage");
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "class")),
                      "resource");
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "reason")),
                      "disk \"full\"\nmanual check");
        ASSERT(json_get_int(json_get(dominant, "age_us")) == 5000000);

        const struct json_value *summary_dom =
            json_get(blockers, "dominant");
        ASSERT(summary_dom != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(summary_dom, "id")),
                      "target-disk-full");

        json_free(&root);
        json_free(&args);
        free(body);
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(0);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    blocker_reset_for_testing();
    blocker_set_clock_for_testing(0);
    return failures;
}

static int test_zcl_blockers_matches_target_state(void)
{
    int failures = 0;
    TEST("controllers: zcl_blockers matches target zcl_state payload") {
        register_all();
        blocker_module_init();
        blocker_reset_for_testing();
        blocker_set_rate_limit_ms_for_testing(0);
        blocker_set_clock_for_testing(1000000);

        struct blocker_record proxy_only;
        ASSERT(blocker_init(&proxy_only, "proxy-only", "mcp",
                            BLOCKER_PERMANENT,
                            "must never leak into target blocker tool"));
        ASSERT(blocker_set(&proxy_only) == 0);

        g_target_blocker_rpc_calls = 0;
        mcp_rpc_client_set_test_hook(mock_status_rpc_with_target_blockers);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_blockers", &args);
        ASSERT(body != NULL);

        struct json_value state_args;
        json_init(&state_args);
        json_set_object(&state_args);
        json_push_kv_str(&state_args, "subsystem", "blocker");
        char *state_body = mcp_router_dispatch("zcl_state", &state_args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(state_body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        struct json_value state_root;
        ASSERT(json_read(&state_root, state_body, strlen(state_body)));
        const struct json_value *target_state =
            json_get(&state_root, "state");
        ASSERT(target_state != NULL);

        ASSERT(json_get_int(json_get(&root, "active_count")) == 2);
        ASSERT(json_get_int(json_get(&root, "resource_count")) == 1);
        ASSERT(json_get_int(json_get(&root, "escape_dispatched_total")) ==
               json_get_int(json_get(target_state,
                                     "escape_dispatched_total")));
        ASSERT(json_get_int(json_get(&root, "rate_limit_ms")) ==
               json_get_int(json_get(target_state, "rate_limit_ms")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "execution_locus")),
                      "target_node");
        ASSERT(g_target_blocker_rpc_calls == 2);
        ASSERT(!contains(body, "proxy-only"));
        ASSERT(!contains(state_body, "proxy-only"));

        const struct json_value *blockers = json_get(&root, "blockers");
        ASSERT(blockers != NULL);
        ASSERT(blockers->type == JSON_ARR);
        ASSERT(json_size(blockers) == 2);
        ASSERT(json_size(blockers) ==
               json_size(json_get(target_state, "blockers")));
        const struct json_value *disk = json_at(blockers, 1);
        ASSERT(disk != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(disk, "id")),
                      "target-disk-full");
        ASSERT_STR_EQ(json_get_str(json_get(disk, "reason")),
                      "disk \"full\"\nmanual check");
        const struct json_value *root_health = json_get(&root, "_health");
        const struct json_value *target_health =
            json_get(target_state, "_health");
        ASSERT(root_health != NULL && root_health->type == JSON_OBJ);
        ASSERT(target_health != NULL && target_health->type == JSON_OBJ);
        ASSERT(!json_get_bool(json_get(root_health, "ok")));
        ASSERT(!json_get_bool(json_get(target_health, "ok")));
        ASSERT_STR_EQ(json_get_str(json_get(root_health, "reason")),
                      "2 active target blockers");
        ASSERT_STR_EQ(json_get_str(json_get(target_health, "reason")),
                      "2 active target blockers");

        json_free(&state_root);
        json_free(&root);
        json_free(&state_args);
        json_free(&args);
        free(state_body);
        free(body);
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(0);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    blocker_reset_for_testing();
    blocker_set_clock_for_testing(0);
    return failures;
}

static char *mock_composite_invalid_child_rpc(const char *method,
                                              const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "getblockcount") == 0)
        return strdup("3157703");
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"startingheight\":3157800},"
                      "{\"startingheight\":3157901}]");
    if (strcmp(method, "syncstate") == 0)
        return strdup("{\"state\":\"syncing\"}");
    if (strcmp(method, "validationstatus") == 0)
        return strdup("{\"ok\":true}");
    if (strcmp(method, "healthcheck") == 0)
        return strdup("{\"healthy\":true}");
    if (strcmp(method, "getmempoolinfo") == 0)
        return strdup("{broken");
    if (strcmp(method, "getwalletinfo") == 0)
        return strdup("{\"balance\":0}");
    if (strcmp(method, "getblockchaininfo") == 0)
        return strdup("{\"best_header_height\":3157901}");
    if (strcmp(method, "getnetworkinfo") == 0)
        return strdup("{\"connections\":2}");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"syncing\"");
    if (strcmp(method, "downloadstats") == 0)
        return strdup("{broken");
    return strdup("null");
}

static char *mock_kpi_peer_error_rpc(const char *method,
                                     const char *params_json)
{
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"peer RPC unavailable\"}}");
    return mock_composite_invalid_child_rpc(method, params_json);
}

static char *mock_syncdiag_peer_without_height_rpc(
    const char *method, const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "getpeerinfo") == 0)
        return strdup("[{\"inbound\":false}]");
    if (strcmp(method, "getsyncdiag") == 0)
        return strdup("{\"sync_state\":\"at_tip\"}");
    if (strcmp(method, "downloadstats") == 0)
        return strdup("{\"in_flight\":0,\"queued\":0}");
    return strdup("null");
}

static int test_zcl_kpi_invalid_child_stays_parseable(void)
{
    int failures = 0;
    TEST("controllers: zcl_kpi invalid child RPC stays parseable") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_composite_invalid_child_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_kpi", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root, "height")) == 3157703);
        ASSERT(json_get_int(json_get(&root, "peer_count")) == 2);
        const struct json_value *mempool = json_get(&root, "mempool");
        ASSERT(mempool != NULL && mempool->type == JSON_NULL);
        const struct json_value *err = json_get(&root, "mempool_error");
        ASSERT(err != NULL);
        ASSERT(contains(json_get_str(json_get(err, "message")),
                        "getmempoolinfo RPC returned invalid JSON"));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_kpi_peer_error_never_becomes_brace_count(void)
{
    int failures = 0;
    TEST("controllers: zcl_kpi peer RPC error never becomes a peer count") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_kpi_peer_error_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_kpi", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *peer_count = json_get(&root, "peer_count");
        const struct json_value *peers = json_get(&root, "peers");
        ASSERT(peer_count != NULL && peer_count->type == JSON_NULL);
        ASSERT(!json_get_bool(json_get(&root, "peer_count_known")));
        ASSERT(peers != NULL && peers->type == JSON_NULL);
        ASSERT(json_get(&root, "peer_count_error") != NULL);
        ASSERT(json_get(&root, "peers_error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_syncdiag_invalid_children_stay_parseable(void)
{
    int failures = 0;
    TEST("controllers: zcl_syncdiag invalid children stay parseable") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_composite_invalid_child_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_syncdiag", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "error")),
                      "getsyncdiag RPC failed");
        ASSERT(json_get_int(json_get(&root, "peer_max_height")) == 3157901);
        ASSERT(json_get_bool(json_get(&root, "peer_max_height_known")));
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "peer_max_height_trust")),
                      "untrusted_peer_advertisement");
        const struct json_value *download = json_get(&root, "download");
        ASSERT(download != NULL && download->type == JSON_NULL);
        const struct json_value *diag_err =
            json_get(&root, "getsyncdiag_error");
        const struct json_value *dl_err =
            json_get(&root, "download_error");
        ASSERT(diag_err != NULL);
        ASSERT(dl_err != NULL);
        ASSERT(contains(json_get_str(json_get(diag_err, "message")),
                        "getsyncdiag RPC returned invalid data"));
        ASSERT(contains(json_get_str(json_get(dl_err, "message")),
                        "downloadstats RPC returned invalid JSON"));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_syncdiag_rpc_errors_stay_unknown(void)
{
    int failures = 0;
    TEST("controllers: zcl_syncdiag RPC errors never become state or zero") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_operator_invalid_core_evidence);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_syncdiag", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "error")),
                      "getsyncdiag RPC failed");
        ASSERT(json_get(&root, "code") == NULL);
        const struct json_value *peer_height =
            json_get(&root, "peer_max_height");
        ASSERT(peer_height != NULL && peer_height->type == JSON_NULL);
        ASSERT(!json_get_bool(json_get(&root,
                                       "peer_max_height_known")));
        const struct json_value *diag_error =
            json_get(&root, "getsyncdiag_error");
        const struct json_value *peer_error =
            json_get(&root, "peer_max_height_error");
        ASSERT(diag_error != NULL && diag_error->type == JSON_OBJ);
        ASSERT(peer_error != NULL && peer_error->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(diag_error, "code")) == -32603);
        ASSERT(json_get_int(json_get(peer_error, "code")) == -32603);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_syncdiag_missing_peer_height_is_unknown(void)
{
    int failures = 0;
    TEST("controllers: zcl_syncdiag needs an actual peer height claim") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_syncdiag_peer_without_height_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_syncdiag", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "sync_state")), "at_tip");
        ASSERT(!json_get_bool(json_get(&root,
                                       "peer_max_height_known")));
        const struct json_value *peer_height =
            json_get(&root, "peer_max_height");
        ASSERT(peer_height != NULL && peer_height->type == JSON_NULL);
        ASSERT(json_get(&root, "peer_max_height_error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static bool g_mock_peerincidents_missing = false;

static char *mock_networkinfo_rpc(const char *method, const char *params_json)
{
    if (strcmp(method, "getnetworkinfo") == 0)
        return strdup("{\"connections\":2,"
                      "\"inbound_connections\":1,"
                      "\"outbound_connections\":1,"
                      "\"handshaked_connections\":2,"
                      "\"inbound_handshaked_connections\":1,"
                      "\"outbound_handshaked_connections\":1,"
                      "\"inbound_handshake_seen\":true,"
                      "\"remote_handshake_seen\":true,"
                      "\"legacy_compatible_peers\":2,"
                      "\"legacy_magicbean_peers\":2,"
                      "\"magicbean_peers\":2,"
                      "\"zclassic23_peers\":1,"
                      "\"zclassic_c23_peers\":1,"
                      "\"peer_lifecycle\":{"
                      "\"attempted\":4,"
                      "\"connected\":3,"
                      "\"version_sent\":3,"
                      "\"version_received\":2,"
                      "\"verack_received\":2,"
                      "\"handshake_complete\":2,"
                      "\"active\":1,"
                      "\"disconnected\":1,"
                      "\"timeout\":1,"
                      "\"rejected\":0,"
                      "\"magicbean_handshakes\":2,"
                      "\"legacy_compatible_handshakes\":2,"
                      "\"legacy_magicbean_handshakes\":2,"
                      "\"zclassic23_handshakes\":1,"
                      "\"zclassic_c23_handshakes\":1,"
                      "\"sources\":["
                      "{\"source\":\"addnode\",\"attempted\":2,"
                      "\"connected\":1,\"handshake_complete\":1,"
                      "\"timeout\":1,\"rejected\":0},"
                      "{\"source\":\"addrman\",\"attempted\":2,"
                      "\"connected\":2,\"handshake_complete\":1,"
                      "\"timeout\":0,\"rejected\":0}]},"
                      "\"localaddresses\":[{\"address\":\"203.0.113.7\","
                      "\"port\":8033,\"score\":1}],"
                      "\"listening\":true}");
    if (strcmp(method, "dumpstate") == 0 && params_json &&
        strstr(params_json, "peer_lifecycle") != NULL &&
        strstr(params_json, "incidents") != NULL)
        return strdup("{\"subsystem\":\"peer_lifecycle\","
                      "\"captured_at\":1782240005,"
                      "\"state\":{\"schema\":\"zcl.peer_incidents.v1\","
                      "\"schema_version\":1,"
                      "\"bounded\":true,"
                      "\"incident_count\":2,"
                      "\"duplicate_host_group_count\":1,"
                      "\"duplicate_open_host_group_count\":1,"
                      "\"duplicate_handshaked_host_group_count\":1,"
                      "\"current_open_connection_count\":2,"
                      "\"current_handshaked_connection_count\":2,"
                      "\"bootstrap_useful_count\":2,"
                      "\"safe_next_action\":\"inspect primary_host_issue and top_host_incidents\","
                      "\"primary_host_issue\":{"
                      "\"status\":\"attention\","
                      "\"host\":\"40.160.53.56\","
                      "\"issue_class\":\"duplicate_handshaked_connections\","
                      "\"direction\":\"mixed\","
                      "\"mixed_direction\":true,"
                      "\"duplicate_current_connections\":true,"
                      "\"duplicate_handshaked_connections\":true,"
                      "\"bootstrap_useful\":true,"
                      "\"last_disconnect_reason\":\"inbound_ephemeral_port\"},"
                      "\"top_host_incidents\":[{\"host\":\"40.160.53.56\"}],"
                      "\"top_incidents\":[],"
                      "\"duplicate_host_groups\":[{\"host\":\"40.160.53.56\"}]}}");
    if (strcmp(method, "peerincidents") == 0) {
        if (g_mock_peerincidents_missing)
            return strdup("{\"code\":-32601,\"message\":\"Method not found\"}");
        return strdup("{\"schema\":\"zcl.peer_incidents.v1\","
                      "\"schema_version\":1,"
                      "\"method\":\"peerincidents\","
                      "\"native_command\":\"zclassic23 peerincidents\","
                      "\"mcp_tool\":\"zcl_peer_incidents\","
                      "\"contract_source\":\"agent_contracts.def\","
                      "\"bounded\":true,"
                      "\"incident_count\":2,"
                      "\"duplicate_host_group_count\":1,"
                      "\"duplicate_open_host_group_count\":1,"
                      "\"duplicate_handshaked_host_group_count\":1,"
                      "\"current_open_connection_count\":2,"
                      "\"current_handshaked_connection_count\":2,"
                      "\"bootstrap_useful_count\":2,"
                      "\"safe_next_action\":\"inspect primary_host_issue and top_host_incidents\","
                      "\"primary_host_issue\":{"
                      "\"status\":\"attention\","
                      "\"host\":\"40.160.53.56\","
                      "\"issue_class\":\"duplicate_handshaked_connections\","
                      "\"direction\":\"mixed\","
                      "\"mixed_direction\":true,"
                      "\"duplicate_current_connections\":true,"
                      "\"duplicate_handshaked_connections\":true,"
                      "\"bootstrap_useful\":true,"
                      "\"last_disconnect_reason\":\"inbound_ephemeral_port\"},"
                      "\"top_host_incidents\":[{\"host\":\"40.160.53.56\"}],"
                      "\"top_incidents\":[],"
                      "\"duplicate_host_groups\":[{\"host\":\"40.160.53.56\"}]}");
    }
    if (strcmp(method, "bootstrapstatus") == 0)
        return strdup("{\"schema\":\"zcl.bootstrap_status.v1\","
                      "\"schema_version\":1,"
                      "\"readiness\":\"ready_p2p_and_addr\","
                      "\"fresh_node_next_action\":\"connect_direct_p2p_and_request_headers_blocks\","
                      "\"serving_p2p_bootstrap\":true,"
                      "\"serving_snapshot_bootstrap\":false,"
                      "\"zclassic23_fast_sync_compatible\":true,"
                      "\"zclassicd_beta6_p2p_compatible\":true,"
                      "\"zclassicd_beta6_fast_bootstrap_compatible\":false,"
                      "\"zclassic23_bootstrap\":{"
                      "\"schema\":\"zcl.bootstrap.zclassic23.v1\","
                      "\"schema_version\":1,"
                      "\"serving\":true,"
                      "\"preferred_for_fresh_zclassic23\":true,"
                      "\"full_node_bootstrap\":true,"
                      "\"addr_relay_ready\":true,"
                      "\"fast_sync_service_bit_advertised\":true,"
                      "\"fast_sync_service_bit_value\":1024,"
                      "\"route_preference\":\"direct_p2p_then_znam_onion_fallback\","
                      "\"endpoint_source\":\"localaddresses_or_znam_service_directory\","
                      "\"endpoint_record_schema\":\"zcl.names.service_record.v1\","
                      "\"name_resolution_schema\":\"zcl.names.show.v1\","
                      "\"service_catalog_member\":\"/api/v1/service-catalog/bootstrap\","
                      "\"bootstrap_api\":\"/api/v1/bootstrap\","
                      "\"clearnet_address\":\"203.0.113.7\","
                      "\"p2p_port\":8033,"
                      "\"onion_fallback\":\"use_znam_service_record_transport_onion_when_direct_p2p_unreachable\","
                      "\"next_action\":\"connect_direct_p2p_and_request_headers_blocks\","
                      "\"fresh_node_flow\":[\"read_bootstrapstatus\","
                      "\"connect_direct_p2p_endpoint\","
                      "\"request_headers_and_blocks\","
                      "\"resolve_znam_service_directory_if_direct_p2p_fails\","
                      "\"fallback_to_onion_endpoint\","
                      "\"validate_all_data_against_zclassic_l1_consensus\"]},"
                      "\"snapshot_loader\":{"
                      "\"schema\":\"zcl.snapshot_loader.v1\","
                      "\"schema_version\":1,"
                      "\"datadir\":\"/tmp/zcl-test\","
                      "\"bundle_present\":true,"
                      "\"bundle_count\":1,"
                      "\"bundle_seed_height\":3170000,"
                      "\"bundle_name\":\"utxo-seed-3170000.snapshot\","
                      "\"bundle_path\":\"/tmp/zcl-test/utxo-seed-3170000.snapshot\","
                      "\"block_index_present\":true,"
                      "\"failed_marker\":false,"
                      "\"bootable_bundle\":true,"
                      "\"active_loader_configured\":true,"
                      "\"active_loader_path\":\"/tmp/zcl-test/utxo-seed-3170000.snapshot\","
                      "\"active_loader_matches_bundle\":true,"
                      "\"recovery_hint\":\"loader_active\","
                      "\"authority\":{"
                      "\"schema\":\"zcl.snapshot_loader_authority.v1\","
                      "\"schema_version\":1,"
                      "\"progress_store_open\":true,"
                      "\"hstar_available\":true,"
                      "\"hstar\":3170000,"
                      "\"served_floor\":3170000,"
                      "\"coins_applied_height_readable\":true,"
                      "\"coins_applied_height_present\":true,"
                      "\"coins_applied_height\":3170001,"
                      "\"coins_kv_proven_authority\":true,"
                      "\"coins_cover_hstar\":true,"
                      "\"fast_rebuild_authority_ready\":true,"
                      "\"self_folded_marker\":false,"
                      "\"self_derived_tip_static_checks\":false,"
                      "\"self_derived_reason\":\"borrowed_seed_no_refold_marker\","
                      "\"authority_posture\":\"proven_but_not_self_folded\"}},"
                      "\"beta6_snapshot_bootstrap\":{"
                      "\"required_service_bit\":\"NODE_BOOTSTRAP\","
                      "\"required_service_bit_value\":16777216,"
                      "\"advertised\":false,"
                      "\"serving\":false,"
                      "\"current_blocker\":\"NODE_BOOTSTRAP service not implemented in zclassic23\","
                      "\"messages\":[\"getbsman\",\"bsman\",\"getbschk\","
                      "\"bschk\",\"getbspman\",\"bspman\",\"getbspchk\","
                      "\"bspchk\"]},"
                      "\"legacy_p2p_bootstrap\":{\"serving\":true,"
                      "\"messages\":[\"version\",\"verack\",\"getheaders\","
                      "\"headers\",\"getdata\",\"block\",\"getaddr\","
                      "\"addr\"]}}");
    return strdup("null");
}

static int test_zcl_networkinfo_exposes_reachability_fields(void)
{
    int failures = 0;
    TEST("controllers: zcl_networkinfo exposes inbound reachability fields") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_networkinfo_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_networkinfo", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root,
                                     "handshaked_connections")) == 2);
        ASSERT(json_get_int(json_get(&root,
                                     "inbound_handshaked_connections")) == 1);
        ASSERT(json_get_int(json_get(&root,
                                     "outbound_handshaked_connections")) == 1);
        ASSERT(json_get_bool(json_get(&root, "inbound_handshake_seen")));
        ASSERT(json_get_bool(json_get(&root, "remote_handshake_seen")));
        ASSERT(json_get_int(json_get(&root, "legacy_compatible_peers")) == 2);
        ASSERT(json_get_int(json_get(&root, "legacy_magicbean_peers")) == 2);
        ASSERT(json_get_int(json_get(&root, "zclassic23_peers")) == 1);
        ASSERT(json_get_int(json_get(&root, "zclassic_c23_peers")) == 1);
        const struct json_value *life = json_get(&root, "peer_lifecycle");
        const struct json_value *sources =
            life ? json_get(life, "sources") : NULL;
        ASSERT(life && life->type == JSON_OBJ);
        ASSERT(json_get_int(json_get(life, "attempted")) == 4);
        ASSERT(json_get_int(json_get(life, "timeout")) == 1);
        ASSERT(json_get_int(json_get(life,
                                      "legacy_compatible_handshakes")) == 2);
        ASSERT(json_get_int(json_get(life,
                                      "legacy_magicbean_handshakes")) == 2);
        ASSERT(json_get_int(json_get(life, "zclassic23_handshakes")) == 1);
        ASSERT(json_get_int(json_get(life, "zclassic_c23_handshakes")) == 1);
        ASSERT(sources && sources->type == JSON_ARR);
        ASSERT(json_size(sources) == 2);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 0), "source")),
                      "addnode");
        ASSERT(json_get_int(json_get(json_at(sources, 0),
                                     "handshake_complete")) == 1);
        ASSERT(json_get_int(json_get(json_at(sources, 0), "timeout")) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(sources, 1), "source")),
                      "addrman");
        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_peer_incidents_exposes_duplicate_host_view(void)
{
    int failures = 0;
    TEST("controllers: zcl_peer_incidents exposes duplicate host view") {
        register_all();
        g_mock_peerincidents_missing = false;
        mcp_rpc_client_set_test_hook(mock_networkinfo_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_peer_incidents", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(strcmp(json_get_str(json_get(&root, "schema")),
                      "zcl.peer_incidents.v1") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "method")),
                      "peerincidents") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "native_command")),
                      "zclassic23 peerincidents") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_peer_incidents") == 0);
        ASSERT(strcmp(json_get_str(json_get(&root, "contract_source")),
                      "agent_contracts.def") == 0);
        ASSERT(json_get_bool(json_get(&root, "bounded")));
        ASSERT(json_get_int(json_get(&root,
                                     "duplicate_host_group_count")) == 1);
        ASSERT(json_get_int(json_get(&root,
                         "duplicate_handshaked_host_group_count")) == 1);
        const struct json_value *primary =
            json_get(&root, "primary_host_issue");
        ASSERT(primary && primary->type == JSON_OBJ);
        ASSERT(strcmp(json_get_str(json_get(primary, "host")),
                      "40.160.53.56") == 0);
        ASSERT(json_get_bool(json_get(primary,
                                      "duplicate_current_connections")));
        ASSERT(json_get_bool(json_get(primary,
                                      "duplicate_handshaked_connections")));
        ASSERT(json_get_bool(json_get(primary, "bootstrap_useful")));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    g_mock_peerincidents_missing = false;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_peer_incidents_falls_back_to_dumpstate(void)
{
    int failures = 0;
    TEST("controllers: zcl_peer_incidents falls back to dumpstate on stale target") {
        register_all();
        g_mock_peerincidents_missing = true;
        mcp_rpc_client_set_test_hook(mock_networkinfo_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_peer_incidents", &args);
        mcp_rpc_client_set_test_hook(NULL);
        g_mock_peerincidents_missing = false;
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.peer_incidents.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "method")),
                      "peerincidents");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "native_command")),
                      "zclassic23 peerincidents");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "mcp_tool")),
                      "zcl_peer_incidents");
        ASSERT(json_get_bool(json_get(&root, "compatibility_fallback")));
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "compatibility_source")),
                      "dumpstate peer_lifecycle incidents");
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "compatibility_reason")),
                      "Method not found");
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "fallback_native_command")),
                      "zclassic23 dumpstate peer_lifecycle incidents");
        const struct json_value *primary =
            json_get(&root, "primary_host_issue");
        ASSERT(primary && primary->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(primary, "host")),
                      "40.160.53.56");

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    g_mock_peerincidents_missing = false;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_bootstrapstatus_exposes_beta6_contract(void)
{
    int failures = 0;
    TEST("controllers: zcl_bootstrapstatus exposes beta6 contract") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_networkinfo_rpc);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_bootstrapstatus", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.bootstrap_status.v1");
        ASSERT(json_get_bool(json_get(&root,
                                      "serving_p2p_bootstrap")));
        ASSERT(!json_get_bool(json_get(&root,
                                       "serving_snapshot_bootstrap")));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "readiness")),
                      "ready_p2p_and_addr");
        ASSERT_STR_EQ(json_get_str(json_get(&root,
                                            "fresh_node_next_action")),
                      "connect_direct_p2p_and_request_headers_blocks");
        ASSERT(json_get_bool(json_get(&root,
                                      "zclassic23_fast_sync_compatible")));
        ASSERT(json_get_bool(json_get(&root,
                                      "zclassicd_beta6_p2p_compatible")));
        ASSERT(!json_get_bool(json_get(&root,
            "zclassicd_beta6_fast_bootstrap_compatible")));

        const struct json_value *zcl23_bootstrap =
            json_get(&root, "zclassic23_bootstrap");
        ASSERT(zcl23_bootstrap && zcl23_bootstrap->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(zcl23_bootstrap, "schema")),
                      "zcl.bootstrap.zclassic23.v1");
        ASSERT(json_get_bool(json_get(zcl23_bootstrap,
                                      "preferred_for_fresh_zclassic23")));
        ASSERT_STR_EQ(json_get_str(json_get(zcl23_bootstrap,
                                            "route_preference")),
                      "direct_p2p_then_znam_onion_fallback");
        ASSERT_STR_EQ(json_get_str(json_get(zcl23_bootstrap,
                                            "endpoint_record_schema")),
                      "zcl.names.service_record.v1");
        ASSERT(json_array_has_str(json_get(zcl23_bootstrap,
                                           "fresh_node_flow"),
                                  "fallback_to_onion_endpoint"));

        const struct json_value *loader =
            json_get(&root, "snapshot_loader");
        ASSERT(loader && loader->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(loader, "schema")),
                      "zcl.snapshot_loader.v1");
        ASSERT(json_get_int(json_get(loader, "schema_version")) == 1);
        ASSERT(json_get_bool(json_get(loader, "bundle_present")));
        ASSERT(json_get_int(json_get(loader,
                                     "bundle_seed_height")) == 3170000);
        ASSERT(json_get_bool(json_get(loader,
                                      "active_loader_configured")));
        ASSERT_STR_EQ(json_get_str(json_get(loader,
                                            "active_loader_path")),
                      "/tmp/zcl-test/utxo-seed-3170000.snapshot");
        ASSERT_STR_EQ(json_get_str(json_get(loader, "recovery_hint")),
                      "loader_active");
        const struct json_value *authority =
            json_get(loader, "authority");
        ASSERT(authority && authority->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(authority, "schema")),
                      "zcl.snapshot_loader_authority.v1");
        ASSERT(json_get_bool(json_get(authority,
                                      "coins_kv_proven_authority")));
        ASSERT(json_get_bool(json_get(authority,
                                      "fast_rebuild_authority_ready")));
        ASSERT(!json_get_bool(json_get(authority,
                                       "self_folded_marker")));
        ASSERT_STR_EQ(json_get_str(json_get(authority,
                                            "authority_posture")),
                      "proven_but_not_self_folded");

        const struct json_value *beta6 =
            json_get(&root, "beta6_snapshot_bootstrap");
        ASSERT(beta6 && beta6->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(beta6,
                                            "required_service_bit")),
                      "NODE_BOOTSTRAP");
        ASSERT(json_get_int(json_get(beta6,
                                     "required_service_bit_value")) ==
               16777216);
        ASSERT(!json_get_bool(json_get(beta6, "advertised")));
        ASSERT(contains(json_get_str(json_get(beta6,
                                              "current_blocker")),
                        "NODE_BOOTSTRAP"));
        ASSERT(json_array_has_str(json_get(beta6, "messages"),
                                  "getbsman"));
        ASSERT(json_array_has_str(json_get(beta6, "messages"),
                                  "getbschk"));

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_meta_tools_in_ops_domain(void)
{
    int failures = 0;
    TEST("controllers: meta tools (tools_list/self_test/logtail) live in ops") {
        register_all();
        const char *k[] = {"zcl_tools_list", "zcl_self_test", "zcl_logtail"};
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            const struct mcp_tool_route *r = mcp_router_find(k[i]);
            ASSERT(r != NULL);
            ASSERT(r->domain != NULL);
            if (strcmp(r->domain, "ops") != 0) {
                printf("FAIL (%s domain=%s)\n", k[i], r->domain);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static char *mock_null_eventlog_rpc(const char *method,
                                    const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "eventlog") == 0)
        return NULL;
    return strdup("null");
}

static int test_zcl_logtail_handles_null_eventlog_rpc(void)
{
    int failures = 0;
    TEST("controllers: zcl_logtail handles null eventlog RPC") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_null_eventlog_rpc);

        const char *args_src = "{\"domain\":\"sync\",\"count\":5}";
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));

        char *body = mcp_router_dispatch("zcl_logtail", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"error\":{"));
        ASSERT(contains(body, "\"code\":\"HANDLER_FAILED\""));
        ASSERT(contains(body, "RPC eventlog returned null"));

        struct json_value root = {0};
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get(&root, "error") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_tools_list_json_well_formed(void)
{
    int failures = 0;
    TEST("controllers: mcp_router_tools_list_json produces parseable array") {
        register_all();
        size_t cap = 262144;
        char *buf = zcl_malloc(cap, "test_json_buf");
        ASSERT(buf != NULL);
        size_t wrote = mcp_router_tools_list_json(buf, cap);
        ASSERT(wrote > 0);
        ASSERT(wrote < cap);
        /* Starts with '[' and ends with ']'. */
        ASSERT(buf[0] == '[');
        ASSERT(buf[wrote - 1] == ']');
        /* Mentions at least one known tool. */
        ASSERT(contains(buf, "zcl_status"));
        ASSERT(contains(buf, "zcl_kpi"));
        /* Parseable JSON */
        struct json_value root;
        ASSERT(json_read(&root, buf, wrote));
        ASSERT(root.type == JSON_ARR);
        ASSERT(root.num_children == EXPECTED_TOTAL);
        json_free(&root);
        free(buf);
        PASS();
    } _test_next:;
    return failures;
}

static int test_input_schema_for_zcl_getblock(void)
{
    int failures = 0;
    TEST("controllers: inputSchema for zcl_getblock declares block_id required") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_getblock");
        ASSERT(r != NULL);
        char buf[4096];
        size_t n = mcp_router_input_schema_json(r, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(contains(buf, "\"block_id\""));
        ASSERT(contains(buf, "\"verbosity\""));
        ASSERT(contains(buf, "\"required\""));
        /* JSON schema lists required fields as an array containing "block_id". */
        ASSERT(contains(buf, "\"block_id\""));
        PASS();
    } _test_next:;
    return failures;
}

static int test_destructive_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: destructive tools (send/importprivkey/...) exist") {
        register_all();
        /* self_test skips these, but they must still be reachable over
         * the wire — otherwise the compat contract breaks. */
        const char *k[] = {
            "zcl_getnewaddress", "zcl_z_getnewaddress",
            "zcl_send", "zcl_sendtoaddress", "zcl_importprivkey",
            "zcl_wallet_receive_intent",
            "zcl_rescanblockchain", "zcl_replaywalletfromchain",
            "zcl_dumpprivkey", "zcl_wallet_backup_now",
            "zcl_addnode", "zcl_pingpeer",
            "zcl_name_register", "zcl_name_update", "zcl_name_transfer",
            "zcl_name_renew", "zcl_name_set_record", "zcl_name_set_text",
            "zcl_msg_send", "zcl_market_offer",
            "zcl_swap_initiate",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            const struct mcp_tool_route *route = mcp_router_find(k[i]);
            if (route == NULL) {
                printf("FAIL (missing %s)\n", k[i]);
                failures++; goto _test_next;
            }
            ASSERT(route->flags & MCP_TOOL_FLAG_DESTRUCTIVE);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int g_self_test_taddr_rpc_calls;
static int g_self_test_zaddr_rpc_calls;
static int g_registry_self_test_rpc_calls;

static char *mock_self_test_address_rpc(const char *method,
                                        const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "getnewaddress") == 0)
        g_self_test_taddr_rpc_calls++;
    if (strcmp(method, "z_getnewaddress") == 0)
        g_self_test_zaddr_rpc_calls++;
    return strdup("null");
}

static char *mock_registry_self_test_rpc(const char *method,
                                         const char *params_json)
{
    (void)method;
    (void)params_json;
    g_registry_self_test_rpc_calls++;
    return strdup("null");
}

static int test_self_test_registry_mode_is_candidate_local(void)
{
    int failures = 0;
    TEST("controllers: registry self-test is bounded and candidate-local") {
        register_all();
        g_registry_self_test_rpc_calls = 0;
        mcp_rpc_client_set_test_hook(mock_registry_self_test_rpc);

        struct json_value args = {0};
        ASSERT(json_read(&args, "{\"mode\":\"registry\"}",
                         strlen("{\"mode\":\"registry\"}")));
        char *body = mcp_router_dispatch("zcl_self_test", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"mode\":\"registry\""));
        ASSERT(contains(body, "\"fail\":0"));
        ASSERT(g_registry_self_test_rpc_calls == 0);

        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_self_test_skips_address_generation(void)
{
    int failures = 0;
    TEST("controllers: self_test skips both wallet address generators") {
        /* Keep the sweep small and hermetic: wallet + meta is enough to run
         * the real self-test loop, while the RPC hook proves neither key-
         * generating handler was dispatched. */
        mcp_router_reset();
        mcp_register_wallet();
        mcp_register_meta();
        g_self_test_taddr_rpc_calls = 0;
        g_self_test_zaddr_rpc_calls = 0;
        mcp_rpc_client_set_test_hook(mock_self_test_address_rpc);

        char *body = mcp_router_dispatch("zcl_self_test", NULL);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT(contains(body,
            "\"tool\":\"zcl_getnewaddress\",\"domain\":\"wallet\","
            "\"status\":\"skipped\",\"reason\":\"destructive\""));
        ASSERT(contains(body,
            "\"tool\":\"zcl_z_getnewaddress\",\"domain\":\"wallet\","
            "\"status\":\"skipped\",\"reason\":\"destructive\""));
        ASSERT(g_self_test_taddr_rpc_calls == 0);
        ASSERT(g_self_test_zaddr_rpc_calls == 0);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_duplicate_register_rejected(void)
{
    int failures = 0;
    TEST("controllers: re-registering the same controller is a no-op") {
        register_all();
        size_t before = mcp_router_count();
        /* Register ops a second time — mcp_router_register should reject
         * each duplicate and the count should not change. */
        mcp_register_ops();
        size_t after = mcp_router_count();
        ASSERT(before == after);
        PASS();
    } _test_next:;
    return failures;
}

static int test_reset_clears_and_reregister_restores(void)
{
    int failures = 0;
    TEST("controllers: reset clears and re-register restores the surface") {
        register_all();
        size_t before = mcp_router_count();
        ASSERT(before == EXPECTED_TOTAL);
        mcp_router_reset();
        ASSERT(mcp_router_count() == 0);
        ASSERT(mcp_router_find("zcl_status") == NULL);
        register_all();
        ASSERT(mcp_router_count() == EXPECTED_TOTAL);
        ASSERT(mcp_router_find("zcl_status") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_wallet_shielded_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: shielded wallet tools (z_*) registered") {
        register_all();
        const char *k[] = {
            "zcl_z_getnewaddress", "zcl_z_listaddresses",
            "zcl_z_listunspent",   "zcl_z_getbalance",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            const struct mcp_tool_route *r = mcp_router_find(k[i]);
            ASSERT(r != NULL);
            ASSERT(strcmp(r->domain, "wallet") == 0);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_app_protocol_tools_registered(void)
{
    int failures = 0;
    TEST("controllers: app protocol tools (name/msg/market/swap) registered") {
        register_all();
        const char *k[] = {
            "zcl_name_resolve", "zcl_name_register", "zcl_name_list",
            "zcl_name_update", "zcl_name_transfer", "zcl_name_renew",
            "zcl_name_set_record", "zcl_name_set_text",
            "zcl_msg_send", "zcl_msg_send_named", "zcl_msg_inbox", "zcl_msg_read",
            "zcl_market_list", "zcl_market_offer", "zcl_market_buy",
            "zcl_market_status",
            "zcl_swap_chains", "zcl_swap_initiate", "zcl_swap_participate",
            "zcl_swap_list",
            "zcl_tokens",
        };
        for (size_t i = 0; i < sizeof(k)/sizeof(k[0]); i++) {
            const struct mcp_tool_route *r = mcp_router_find(k[i]);
            if (!r) {
                printf("FAIL (missing %s)\n", k[i]);
                failures++; goto _test_next;
            }
            if (strcmp(r->domain, "app") != 0) {
                printf("FAIL (%s domain=%s expected app)\n", k[i], r->domain);
                failures++; goto _test_next;
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_name_list_returns_typed_index(void)
{
    int failures = 0;
    TEST("controllers: zcl_name_list returns typed ZNAM index") {
        register_all();
        g_name_list_rpc_called = false;
        g_name_list_rpc_params_null = false;
        mcp_rpc_client_set_test_hook(mock_name_list_rpc);

        char *body = mcp_router_dispatch("zcl_name_list", NULL);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT(g_name_list_rpc_called);
        ASSERT(g_name_list_rpc_params_null);

        struct json_value root = {0};
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.names.index.v1");
        ASSERT(json_get_int(json_get(&root, "limit")) == 100);
        ASSERT(json_get_int(json_get(&root, "count")) == 1);
        ASSERT(!json_get_bool(json_get(&root, "filtered")));

        const struct json_value *names = json_get(&root, "names");
        ASSERT(names != NULL);
        ASSERT(json_size(names) == 1);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(names, 0), "name")),
                      "alice");

        const struct json_value *links = json_get(&root, "_links");
        ASSERT(links != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(links, "collection")),
                      "/api/v1/names");
        ASSERT_STR_EQ(json_get_str(json_get(links, "read")),
                      "/api/v1/names/{name}");

        const struct json_value *verification =
            json_get(&root, "zcl_verification");
        ASSERT(verification != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(verification, "base_layer")),
                      "zclassic_l1");
        ASSERT_STR_EQ(json_get_str(json_get(verification,
                                            "consensus_boundary")),
                      "legacy_zclassic_consensus_unchanged");

        json_free(&root);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_required_params_have_no_default(void)
{
    int failures = 0;
    TEST("controllers: required params never carry a default_json") {
        register_all();
        /* A required param with a default would be a schema contradiction:
         * the router would never enforce "required".  This is a sanity
         * check on controller route tables. */
        for (size_t i = 0; i < mcp_router_count(); i++) {
            const struct mcp_tool_route *r = mcp_router_at(i);
            ASSERT(r != NULL);
            for (size_t j = 0; j < r->num_params; j++) {
                const struct mcp_param_spec *p = &r->params[j];
                if (p->required && p->default_json) {
                    printf("FAIL (%s.%s is required with default=%s)\n",
                           r->name, p->name, p->default_json);
                    failures++; goto _test_next;
                }
            }
        }
        PASS();
    } _test_next:;
    return failures;
}

/* ── Wave 5 #5: zcl_admin composite tool ────────────────────── */

static int test_zcl_admin_dispatch_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_admin composes sub-tools into one envelope") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_admin");
        ASSERT(r != NULL);
        ASSERT(strcmp(r->domain, "ops") == 0);
        ASSERT(r->num_params == 1);
        ASSERT(strcmp(r->params[0].name, "since") == 0);
        ASSERT(r->params[0].required == false);

        /* Dispatch with empty args — `since` falls through to default 0. */
        char *body = mcp_router_dispatch("zcl_admin", NULL);
        ASSERT(body != NULL);
        /* Not an error envelope — graceful handling even with no live RPC. */
        ASSERT(strstr(body, "\"error\":{") == NULL);
        /* Top-level fields. */
        ASSERT(contains(body, "\"since\":0"));
        ASSERT(contains(body, "\"kpi\":"));
        ASSERT(contains(body, "\"peer_report\":"));
        ASSERT(contains(body, "\"rpc_report\":"));
        ASSERT(contains(body, "\"events\":"));
        ASSERT(contains(body, "\"alerts\":["));
        /* rpc_report is always produced in-process, so it should
         * embed as an object (not null). */
        ASSERT(contains(body, "\"rpc_server\":\"inactive\"") ||
               contains(body, "\"rpc_server\":\"active\""));
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_admin_since_param_accepted(void)
{
    int failures = 0;
    TEST("controllers: zcl_admin echoes `since` back in the envelope") {
        register_all();
        const char *args_src = "{\"since\":1700000000}";
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));

        char *body = mcp_router_dispatch("zcl_admin", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"since\":1700000000"));
        free(body);
        json_free(&args);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_admin_graceful_never_propagates_error(void)
{
    int failures = 0;
    TEST("controllers: zcl_admin never propagates a sub-tool error envelope") {
        register_all();
        /* Whether or not a sub-tool returns a valid body vs an error
         * envelope in this test context, zcl_admin must wrap the
         * response as its own object — never surface a top-level
         * `{"error":...}`.  embed_or_null is the policy; this test
         * catches any regression that bypasses it. */
        char *body = mcp_router_dispatch("zcl_admin", NULL);
        ASSERT(body != NULL);

        /* The top level is an object, not an error envelope. */
        ASSERT(body[0] == '{');
        ASSERT(strncmp(body, "{\"error\":", 9) != 0);

        /* Parse to make sure it's structurally valid JSON. */
        struct json_value root = {0};
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(root.type == JSON_OBJ);

        /* Each expected top-level key is present. */
        ASSERT(json_get(&root, "since")       != NULL);
        ASSERT(json_get(&root, "kpi")         != NULL);
        ASSERT(json_get(&root, "peer_report") != NULL);
        ASSERT(json_get(&root, "rpc_report")  != NULL);
        ASSERT(json_get(&root, "events")      != NULL);
        ASSERT(json_get(&root, "alerts")      != NULL);

        /* alerts is an array. */
        const struct json_value *alerts = json_get(&root, "alerts");
        ASSERT(alerts->type == JSON_ARR);

        json_free(&root);
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Wave 6: zcl_profile ────────────────────────────────────── */

static int test_zcl_profile_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_profile returns top_threads + duration_ms") {
        register_all();
        const struct mcp_tool_route *r = mcp_router_find("zcl_profile");
        ASSERT(r != NULL);
        ASSERT(strcmp(r->domain, "ops") == 0);
        ASSERT(r->num_params == 2);

        /* Use a small duration to keep the test fast. */
        const char *args_src = "{\"duration_ms\":100,\"top_n\":5}";
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));

        char *body = mcp_router_dispatch("zcl_profile", &args);
        ASSERT(body != NULL);
        ASSERT(strstr(body, "\"error\":{") == NULL);
        ASSERT(contains(body, "\"duration_ms\":100"));
        ASSERT(contains(body, "\"sampled_threads\""));
        ASSERT(contains(body, "\"top_threads\":["));

        /* The process always has at least one thread (the test runner). */
        struct json_value root = {0};
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *st = json_get(&root, "sampled_threads");
        ASSERT(st != NULL);
        ASSERT(json_get_int(st) >= 1);

        const struct json_value *tt = json_get(&root, "top_threads");
        ASSERT(tt != NULL);
        ASSERT(tt->type == JSON_ARR);
        ASSERT(tt->num_children >= 1);
        ASSERT(tt->num_children <= 5);

        /* Each top_threads entry has tid, name, user_ms, sys_ms, cpu_pct. */
        const struct json_value *first = &tt->children[0];
        ASSERT(json_get(first, "tid")     != NULL);
        ASSERT(json_get(first, "name")    != NULL);
        ASSERT(json_get(first, "user_ms") != NULL);
        ASSERT(json_get(first, "sys_ms")  != NULL);
        ASSERT(json_get(first, "cpu_pct") != NULL);

        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_profile_clamps(void)
{
    int failures = 0;
    TEST("controllers: zcl_profile clamps duration_ms to [100, 10000]") {
        register_all();
        /* The router enforces the min/max from p_profile spec, so a
         * value below 100 should be rejected with an error envelope. */
        const char *args_src = "{\"duration_ms\":50}";
        struct json_value args = {0};
        ASSERT(json_read(&args, args_src, strlen(args_src)));
        char *body = mcp_router_dispatch("zcl_profile", &args);
        ASSERT(body != NULL);
        ASSERT(contains(body, "\"error\":{"));
        free(body);
        json_free(&args);
        PASS();
    } _test_next:;
    return failures;
}

/* Fix A: the long-running tools (zcl_profile, zcl_waitfor*) resolve to a
 * dispatch budget above their own internal cap so the middleware's global 5s
 * guard cannot kill a legal call, while ordinary tools fall back to it. The
 * budgets live in middleware's k_long_running_tools table; this asserts both
 * the resolution and the drift guard that every named tool is a real route. */
static int test_longrunning_routes_carry_timeout(void)
{
    int failures = 0;
    TEST("controllers: profile/waitfor* resolve to a budget >= their cap") {
        register_all();

        struct mcp_middleware mw;
        mcp_middleware_init(&mw);
        mw.default_timeout_ms = 5000;

        /* zcl_profile: duration_ms schema-clamped to <=10000. */
        ASSERT(mcp_middleware_resolve_timeout_ms(&mw, "zcl_profile") >= 10000);

        /* zcl_waitfor*: internally capped at WAIT_RPC_MAX_MS=9000. */
        const char *waiters[] = {
            "zcl_waitforheight", "zcl_waitforhalt", "zcl_waitforblocker",
        };
        for (size_t i = 0; i < sizeof(waiters) / sizeof(waiters[0]); i++)
            ASSERT(mcp_middleware_resolve_timeout_ms(&mw, waiters[i]) >= 9000);

        /* An ordinary tool → the global default. */
        ASSERT(mcp_middleware_resolve_timeout_ms(&mw, "zcl_status") == 5000);

        /* Drift guard: every long-running name is a registered route, so a
         * rename can't silently revert a tool to the 5s default. */
        ASSERT(mcp_long_running_tools_all_registered());

        mcp_middleware_destroy(&mw);
        PASS();
    } _test_next:;
    return failures;
}

/* Fix B: h_zcl_onion_health no longer relies on a shared function-static
 * scratch buffer; a functional dispatch returns a well-formed body. In the
 * test env no onion is started, so this exercises the not_started path (the
 * error envelope must NOT appear — that would be a router/handler failure). */
static int test_zcl_onion_health_wellformed(void)
{
    int failures = 0;
    TEST("controllers: zcl_onion_health returns a well-formed body") {
        register_all();
        char *body = mcp_router_dispatch("zcl_onion_health", NULL);
        ASSERT(body != NULL);
        /* Not a router/handler error envelope. */
        ASSERT(strstr(body, "\"error\":{") == NULL);
        ASSERT(contains(body, "\"ok\":"));
        /* Parseable JSON with an ok field. */
        struct json_value root = {0};
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get(&root, "ok") != NULL);
        json_free(&root);
        free(body);
        PASS();
    } _test_next:;
    return failures;
}

/* ── JSON injection in wallet RPC payloads ────── */

/* Before the fix, both handlers snprintf'd user-controlled strings
 * directly into their params_json:
 *
 *     snprintf(params, sizeof(params),
 *              "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}]]",
 *              from, to, amount);
 *
 * A caller sending `from = "ztest","params":["attacker_addr"] //`
 * would punch through the string context and rewrite the params
 * array — redirecting funds to `attacker_addr`. Both handlers now
 * route user strings through mcp_params_* which escape the dangerous
 * characters via the JSON encoder.
 *
 * These tests exercise the builder with the exact shapes the handlers
 * produce, then parse the output back and assert that the attacker's
 * string is preserved as a single literal string — not interpreted as
 * structure. */

static int test_zcl_send_escapes_json_injection(void)
{
    int failures = 0;
    TEST("controllers: zcl_send escapes JSON injection in from/to ") {
        /* Classic payload: close the "from" string, re-open params,
         * and point funds at an attacker-controlled address. */
        const char *attacker = "ztest\",[{\"address\":\"attacker\",\"amount\":1.0}]] //";

        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str(&p, attacker);

        struct json_value recip, recip_arr;
        json_init(&recip);     json_set_object(&recip);
        json_push_kv_str (&recip, "address", attacker);
        json_push_kv_real(&recip, "amount",  0.5);
        json_init(&recip_arr); json_set_array(&recip_arr);
        json_push_back(&recip_arr, &recip);
        mcp_params_push_value(&p, &recip_arr);
        json_free(&recip);
        json_free(&recip_arr);

        char *params = mcp_params_to_json(&p);
        ASSERT(params != NULL);

        /* Shape: params must be exactly [string, array-of-one-object]. */
        struct json_value root;
        ASSERT(json_read(&root, params, strlen(params)));
        ASSERT(root.type == JSON_ARR);
        ASSERT(root.num_children == 2);

        const struct json_value *from_v = json_at(&root, 0);
        ASSERT(from_v != NULL);
        ASSERT(from_v->type == JSON_STR);
        ASSERT_STR_EQ(from_v->val.s, attacker);

        const struct json_value *recips = json_at(&root, 1);
        ASSERT(recips != NULL);
        ASSERT(recips->type == JSON_ARR);
        ASSERT(recips->num_children == 1);

        const struct json_value *r0 = json_at(recips, 0);
        ASSERT(r0 != NULL);
        ASSERT(r0->type == JSON_OBJ);
        const struct json_value *addr_v = json_get(r0, "address");
        ASSERT(addr_v != NULL);
        ASSERT(addr_v->type == JSON_STR);
        ASSERT_STR_EQ(addr_v->val.s, attacker);

        /* The raw serialized payload must contain escaped quotes —
         * belt-and-suspenders check on the escape itself. */
        ASSERT(strstr(params, "\\\"") != NULL);

        free(params);
        json_free(&root);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_sendtoaddress_escapes_json_injection(void)
{
    int failures = 0;
    TEST("controllers: zcl_sendtoaddress escapes JSON injection in address ") {
        /* Punch through the address string, bloat amount to drain the
         * wallet, and append a bogus second recipient. */
        const char *attacker = "zaddr\",999999999,\"extra\":[\"attacker\"]";

        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str (&p, attacker);
        mcp_params_push_real(&p, 0.01);
        char *params = mcp_params_to_json(&p);
        ASSERT(params != NULL);

        struct json_value root;
        ASSERT(json_read(&root, params, strlen(params)));
        ASSERT(root.type == JSON_ARR);
        /* Exactly two — the injection did NOT add a third element. */
        ASSERT(root.num_children == 2);

        const struct json_value *addr_v = json_at(&root, 0);
        ASSERT(addr_v != NULL);
        ASSERT(addr_v->type == JSON_STR);
        ASSERT_STR_EQ(addr_v->val.s, attacker);

        const struct json_value *amt_v = json_at(&root, 1);
        ASSERT(amt_v != NULL);
        ASSERT(amt_v->type == JSON_REAL);
        /* The amount is the number we pushed, not the injected 999999999. */
        ASSERT(amt_v->val.d < 1.0);

        ASSERT(strstr(params, "\\\"") != NULL);

        free(params);
        json_free(&root);
        PASS();
    } _test_next:;
    return failures;
}

static int test_mcp_params_escapes_backslash_and_control(void)
{
    int failures = 0;
    TEST("controllers: mcp_params escapes backslash, newline, and control chars") {
        const char *s = "a\\b\"c\nd\te";
        struct mcp_params p;
        mcp_params_init(&p);
        mcp_params_push_str(&p, s);
        char *params = mcp_params_to_json(&p);
        ASSERT(params != NULL);

        struct json_value root;
        ASSERT(json_read(&root, params, strlen(params)));
        ASSERT(root.type == JSON_ARR);
        ASSERT(root.num_children == 1);
        const struct json_value *s_v = json_at(&root, 0);
        ASSERT(s_v != NULL);
        ASSERT(s_v->type == JSON_STR);
        ASSERT_STR_EQ(s_v->val.s, s);

        free(params);
        json_free(&root);
        PASS();
    } _test_next:;
    return failures;
}

/* Regression: zcl_state (and every other MCP tool) must NOT silently send
 * an empty credential and surface a cryptic 401 when the RPC auth cookie is
 * missing/unreadable.  This was the recurring "zcl_state returns 401" bug:
 * mcp_node_rpc ignored read_cookie()'s failure return, sent an empty Basic
 * credential, and the node answered 401 Unauthorized with no hint about the
 * real cause.  We now fail fast with an actionable message that names the
 * cookie path.  Exercises the REAL mcp_node_rpc path (no ZCL_TESTING hook). */
static int test_mcp_node_rpc_missing_cookie_is_actionable(void)
{
    int failures = 0;
    TEST("controllers: mcp_node_rpc reports missing cookie, never a bare 401") {
        /* Ensure the real RPC path runs, not the test hook. */
        mcp_rpc_client_set_test_hook(NULL);

        /* Point the client at a datadir that has no .cookie file.  Use a
         * port that nothing listens on so that IF the cookie gate were
         * (wrongly) bypassed, we'd see a connect error rather than a real
         * 401 — but the cookie gate must fire FIRST. */
        char tmpdir[] = "/tmp/zcl_mcp_nocookie_XXXXXX";
        ASSERT(mkdtemp(tmpdir) != NULL);
        mcp_rpc_client_init(tmpdir, 1 /* unused: rejected before connect */);

        char *body = mcp_node_rpc("dumpstate", "[\"boot\"]");
        ASSERT(body != NULL);
        /* Must be an actionable cookie message, NOT a bare 401/Unauthorized
         * and NOT a connection error (the gate runs before connect). */
        ASSERT(contains(body, "cookie"));
        ASSERT(contains(body, ".cookie"));
        ASSERT(contains(body, tmpdir));
        ASSERT(!contains(body, "Unauthorized"));
        ASSERT(!contains(body, "cannot connect"));
        free(body);

        /* Now write a cookie: the gate must NOT fire — the request proceeds
         * to the socket and fails with a connect error instead (proving the
         * gate is scoped strictly to the missing-cookie condition). */
        char cpath[1024];
        snprintf(cpath, sizeof(cpath), "%s/.cookie", tmpdir);
        FILE *cf = fopen(cpath, "w");
        ASSERT(cf != NULL);
        fputs("__cookie__:deadbeefdeadbeefdeadbeefdeadbeef\n", cf);
        fclose(cf);

        body = mcp_node_rpc("dumpstate", "[\"boot\"]");
        ASSERT(body != NULL);
        /* With a valid cookie present and a dead port, we get a connect
         * error — crucially NOT the cookie message. */
        ASSERT(!contains(body, "cannot read RPC auth cookie"));
        free(body);

        unlink(cpath);
        rmdir(tmpdir);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_final_reset_leaves_clean_table(void)
{
    int failures = 0;
    TEST("controllers: final reset leaves the registry clean for sibling tests") {
        mcp_router_reset();
        ASSERT(mcp_router_count() == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_mcp_controllers(void);

int test_mcp_controllers(void)
{
    int failures = 0;
    event_log_init();

    failures += test_register_total_count();
    failures += test_ops_domain_count();
    failures += test_chain_domain_count();
    failures += test_net_domain_count();
    failures += test_wallet_domain_count();
    failures += test_app_domain_count();
    failures += test_every_tool_has_handler();
    failures += test_every_tool_has_description();
    failures += test_tool_descriptions_do_not_claim_zclassicd_authority();
    failures += test_every_tool_has_known_domain();
    failures += test_every_tool_name_prefixed();
    failures += test_no_duplicate_names();
    failures += test_specific_flagship_tools_registered();
    failures += test_zcl_getblock_param_shape();
    failures += test_zcl_status_no_params();
    failures += test_zcl_state_catalog_shape();
    failures += test_zcl_agent_dev_tools_shape();
    failures += test_agent_contract_mcp_registry_coverage();
    failures += test_postmortem_tools_list_and_replay();
    failures += test_zcl_getblockcount_uses_node_hstar_rpc();
    failures += test_zcl_operator_summary_degraded_shape();
    failures += test_zcl_operator_summary_compat_shape();
    failures += test_native_operator_snapshot_single_rpc();
    failures += test_native_operator_snapshot_failure_never_falls_back();
    failures += test_native_operator_snapshot_exact_fallback();
    failures += test_operator_summary_target_blockers_override_cached_healthy();
    failures += test_operator_summary_unknown_blockers_cannot_be_healthy();
    failures += test_operator_summary_invalid_core_evidence_cannot_be_healthy();
    failures += test_operator_summary_nonobject_peer_is_unknown();
    failures += test_operator_summary_zero_peers_at_tip_is_blocked();
    failures += test_operator_summary_inconsistent_frontier_is_not_healthy();
    failures += test_operator_summary_non_tip_state_cannot_be_healthy();
    failures += test_operator_summary_known_blocker_beats_telemetry_outage();
    failures += test_operator_summary_served_gap_cannot_be_hidden_by_index_tip();
    failures += test_operator_summary_invalid_download_is_unknown();
    failures += test_operator_summary_max_download_counters_do_not_overflow();
    failures += test_zcl_operator_summary_honors_mirror_contract();
    failures += test_zcl_agent_contract_shape();
    failures += test_zcl_agent_dev_tools_dispatch();
    failures += test_zcl_operator_summary_names_operator_needed_detail();
    failures += test_zcl_milestone_shape();
    failures += test_zcl_refold_status_shape();
    failures += test_zcl_status_includes_chain_advance_dump();
    failures += test_zcl_status_reports_dumpstate_error();
    failures += test_target_blocker_failure_never_falls_back_to_proxy();
    failures += test_target_blocker_failure_matrix_fails_closed();
    failures += test_zcl_status_sync_gap_uses_served_tip();
    failures += test_zcl_status_peer_claim_cannot_set_target();
    failures += test_zcl_status_failed_sync_inputs_are_unknown();
    failures += test_zcl_status_labels_build_commit_fallback();
    failures += test_zcl_status_negative_health_metrics_are_unknown();
    failures += test_zcl_status_inconsistent_frontier_is_not_zero_gap();
    failures += test_zcl_status_missing_peer_height_is_unknown();
    failures += test_zcl_status_missing_peer_direction_is_unknown();
    failures += test_zcl_status_uses_target_blockers_not_proxy_registry();
    failures += test_zcl_blockers_matches_target_state();
    failures += test_zcl_kpi_invalid_child_stays_parseable();
    failures += test_zcl_kpi_peer_error_never_becomes_brace_count();
    failures += test_zcl_syncdiag_invalid_children_stay_parseable();
    failures += test_zcl_syncdiag_rpc_errors_stay_unknown();
    failures += test_zcl_syncdiag_missing_peer_height_is_unknown();
    failures += test_zcl_networkinfo_exposes_reachability_fields();
    failures += test_zcl_peer_incidents_exposes_duplicate_host_view();
    failures += test_zcl_peer_incidents_falls_back_to_dumpstate();
    failures += test_zcl_bootstrapstatus_exposes_beta6_contract();
    failures += test_meta_tools_in_ops_domain();
    failures += test_zcl_replay_exec_refuses_destructive();
    failures += test_zcl_logtail_handles_null_eventlog_rpc();
    failures += test_tools_list_json_well_formed();
    failures += test_input_schema_for_zcl_getblock();
    failures += test_destructive_tools_registered();
    failures += test_self_test_registry_mode_is_candidate_local();
    failures += test_self_test_skips_address_generation();
    failures += test_duplicate_register_rejected();
    failures += test_reset_clears_and_reregister_restores();
    failures += test_wallet_shielded_tools_registered();
    failures += test_app_protocol_tools_registered();
    failures += test_zcl_name_list_returns_typed_index();
    failures += test_required_params_have_no_default();
    failures += test_postmortem_tools_dispatch();
    failures += test_zcl_admin_dispatch_shape();
    failures += test_zcl_admin_since_param_accepted();
    failures += test_zcl_admin_graceful_never_propagates_error();
    failures += test_zcl_profile_shape();
    failures += test_zcl_profile_clamps();
    failures += test_longrunning_routes_carry_timeout();
    failures += test_zcl_onion_health_wellformed();
    failures += test_zcl_send_escapes_json_injection();
    failures += test_zcl_sendtoaddress_escapes_json_injection();
    failures += test_mcp_params_escapes_backslash_and_control();
    failures += test_mcp_node_rpc_missing_cookie_is_actionable();
    failures += test_final_reset_leaves_clean_table();

    return failures;
}
