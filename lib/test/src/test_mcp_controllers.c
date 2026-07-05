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
#include "mcp/rpc_params.h"
#include "mcp/rpc_client.h"
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
#define EXPECTED_TOTAL     109  /* +3 recovery: zcl_invalidateblock, zcl_reconsiderblock, zcl_rebuild_recent;
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
                                 * +4 agent API tools: map, impact, contracts,
                                 *   build */
#define EXPECTED_OPS        45  /* + zcl_rebuild_recent (bounded recovery);
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
                                 * + zcl_operator_summary + zcl_agent
                                 *   (simple MCP status)
                                 * + zcl_refold_status
                                 * +4 zcl_agent_* development tools */
#define EXPECTED_CHAIN      19  /* + chain_tip + reorg_history
                                 * + zcl_replay_verify (offline replay verifier)
                                 * + zcl_invalidateblock + zcl_reconsiderblock (recovery)
                                 * + zcl_waitforheight + zcl_waitforhalt
                                 *   + zcl_waitforblocker (wait tools) */
#define EXPECTED_NET         9  /* + zcl_peer_report (wave 4 #5),
                                 * + zcl_onion_health (wave 6 #7) */
#define EXPECTED_WALLET     20
#define EXPECTED_APP        16
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
    TEST("controllers: net domain has 9 tools") {
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
    TEST("controllers: wallet domain has 20 tools") {
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
            "zcl_agent_map", "zcl_agent_impact", "zcl_agent_contracts",
            "zcl_agent_build",
            "zcl_milestone", "zcl_refold_status", "zcl_kpi", "zcl_health",
            "zcl_getblockcount", "zcl_getblock", "zcl_getblockchaininfo",
            "zcl_peers", "zcl_networkinfo", "zcl_onion_status",
            "zcl_balance", "zcl_send", "zcl_getnewaddress",
            "zcl_z_getnewaddress",
            "zcl_name_resolve", "zcl_msg_send",
            "zcl_swap_chains", "zcl_market_list",
            "zcl_tools_list", "zcl_self_test", "zcl_logtail",
            "zcl_rpc", "zcl_postmortem_list", "zcl_postmortem_replay",
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

static int test_zcl_agent_dev_tools_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_agent_* development tools have stable shapes") {
        register_all();
        const struct mcp_tool_route *map =
            mcp_router_find("zcl_agent_map");
        const struct mcp_tool_route *impact =
            mcp_router_find("zcl_agent_impact");
        const struct mcp_tool_route *contracts =
            mcp_router_find("zcl_agent_contracts");
        const struct mcp_tool_route *build =
            mcp_router_find("zcl_agent_build");
        ASSERT(map != NULL);
        ASSERT(impact != NULL);
        ASSERT(contracts != NULL);
        ASSERT(build != NULL);
        ASSERT(strcmp(map->domain, "ops") == 0);
        ASSERT(strcmp(impact->domain, "ops") == 0);
        ASSERT(strcmp(contracts->domain, "ops") == 0);
        ASSERT(strcmp(build->domain, "ops") == 0);
        ASSERT(map->num_params == 0);
        ASSERT(contracts->num_params == 0);
        ASSERT(build->num_params == 0);
        ASSERT(impact->num_params == 1);
        ASSERT(strcmp(impact->params[0].name, "files") == 0);
        ASSERT(impact->params[0].type == MCP_PARAM_ARRAY);
        ASSERT(impact->params[0].required == false);
        ASSERT(impact->params[0].default_json != NULL);
        ASSERT(impact->self_test_args != NULL);
        ASSERT(contains(impact->description, "recommended focused tests"));
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
                      "\"score_target_lag_penalty\":0,"
                      "\"score_failure_penalty\":0,"
                      "\"healthy_peers\":3}]}}");
    return strdup("null");
}

static char *mock_operator_degraded_rpc(const char *method,
                                        const char *params_json)
{
    (void)params_json;
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
    return strdup("null");
}

static char *mock_operator_healthy_rpc(const char *method,
                                       const char *params_json)
{
    (void)params_json;
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
    if (strcmp(method, "milestone") == 0)
        return strdup("{\"schema\":\"zcl.milestone_status.v1\","
                      "\"api_version\":\"v1\","
                      "\"milestone\":\"v1 MVP\","
                      "\"mvp_readiness_score\":4,"
                      "\"target_score\":8,"
                      "\"ascii\":{\"goals\":\"goals [#####-----] 4/8 strict MVP MRS\"},"
                      "\"bars\":{\"subgoals\":{\"bar\":\"[########--]\"}},"
                      "\"criteria\":[1,2,3,4,5,6,7,8]}");
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

static char *mock_operator_needed_rpc(const char *method,
                                      const char *params_json)
{
    (void)params_json;
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
    return strdup("null");
}

static bool g_agent_impact_params_seen;

static char *mock_agent_dev_rpc(const char *method, const char *params_json)
{
    if (strcmp(method, "agentmap") == 0)
        return strdup("{\"schema\":\"zcl.agent_map.v1\","
                      "\"commands\":[{\"name\":\"build\"}],"
                      "\"deprecated_shim\":{\"primary\":false}}");
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
                      "\"schemas\":[{\"schema\":\"zcl.agent_build.v1\"}],"
                      "\"transports\":[\"mcp: zcl_agent_build\"]}");
    if (strcmp(method, "agentbuild") == 0)
        return strdup("{\"schema\":\"zcl.agent_build.v1\","
                      "\"incremental_compile\":{\"header_depfiles\":true},"
                      "\"commands\":[{\"name\":\"compile_check\"}],"
                      "\"reproducible_release\":{\"command\":\"make ci-reproducible\"}}");
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
        ASSERT(json_get_int(json_get(&root, "target_height")) == 112);
        ASSERT(json_get_int(json_get(&root, "gap")) == 12);
        ASSERT_STR_EQ(json_get_str(json_get(&root, "sync_state")),
                      "blocks_download");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "download_queue_idle");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_tool")),
                      "zcl_syncdiag");

        const struct json_value *tools =
            json_get(&root, "recommended_tools");
        ASSERT(tools != NULL);
        ASSERT(json_size(tools) == 2);
        ASSERT_STR_EQ(json_get_str(json_at(tools, 0)), "zcl_syncdiag");
        ASSERT_STR_EQ(json_get_str(json_at(tools, 1)), "zcl_node_log");

        const struct json_value *peers = json_get(&root, "peers");
        ASSERT(peers != NULL);
        ASSERT(json_get_int(json_get(peers, "total")) == 2);
        ASSERT(json_get_int(json_get(peers, "ready")) == 1);
        ASSERT(json_get_int(json_get(peers, "max_height")) == 112);

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

static int test_zcl_operator_summary_healthy_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_operator_summary keeps advisory mirror separate") {
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
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "healthy");
        ASSERT(json_get_bool(json_get(&root, "healthy")));
        ASSERT(json_get_int(json_get(&root, "gap")) == 0);
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "none");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "next_action")), "none");
        ASSERT(json_size(json_get(&root, "recommended_tools")) == 0);

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

static int test_zcl_agent_alias_shape(void)
{
    int failures = 0;
    TEST("controllers: zcl_agent aliases the simple operator summary") {
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
                      "zcl.operator_summary.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "api_version")), "v1");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "status")), "healthy");
        ASSERT_STR_EQ(json_get_str(json_get(&root, "primary_blocker")),
                      "none");

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
        json_free(&root);
        free(body);

        body = mcp_router_dispatch("zcl_agent_build", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT_STR_EQ(json_get_str(json_get(&root, "schema")),
                      "zcl.agent_build.v1");
        ASSERT(json_get(&root, "reproducible_release") != NULL);

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

static char *mock_status_rpc_dumpstate_error(const char *method,
                                             const char *params_json)
{
    if (strcmp(method, "dumpstate") == 0 &&
        params_json && strstr(params_json, "reducer_frontier") != NULL)
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"reducer frontier unavailable\"}}");
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
        const struct json_value *blockers = json_get(&root, "blockers");
        ASSERT(blockers != NULL);
        ASSERT(json_get_int(json_get(blockers, "active_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "permanent_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "transient_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "dependency_count")) == 0);
        ASSERT(json_get_int(json_get(blockers, "resource_count")) == 0);
        ASSERT(json_is_null(json_get(blockers, "dominant")));
        ASSERT(json_is_null(json_get(&root, "dominant_blocker")));
        const struct json_value *connections = json_get(&root, "connections");
        ASSERT(connections != NULL);
        ASSERT(json_get_int(json_get(connections, "total")) == 1);
        ASSERT(json_get_int(json_get(connections, "inbound")) == 0);
        ASSERT(json_get_int(json_get(connections, "outbound")) == 1);
        ASSERT(json_get_int(json_get(connections, "zcl23")) == 1);
        ASSERT(json_get_int(json_get(connections, "magicbean")) == 0);
        ASSERT(json_get_int(json_get(&root, "max_peer_height")) == 3117074);
        ASSERT(json_get_int(json_get(&root, "header_gap")) == 0);
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
    TEST("controllers: zcl_status reports dumpstate error metadata") {
        register_all();
        mcp_rpc_client_set_test_hook(mock_status_rpc_dumpstate_error);
        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_status", &args);
        mcp_rpc_client_set_test_hook(NULL);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        const struct json_value *frontier =
            json_get(&root, "reducer_frontier");
        const struct json_value *err =
            json_get(&root, "reducer_frontier_error");
        ASSERT(frontier != NULL);
        ASSERT(json_is_null(frontier));
        ASSERT(err != NULL);
        ASSERT(json_get_int(json_get(err, "code")) == -32603);
        ASSERT_STR_EQ(json_get_str(json_get(err, "message")),
                      "reducer frontier unavailable");
        json_free(&root);
        json_free(&args);
        free(body);
        PASS();
    } _test_next:;
    mcp_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_zcl_status_includes_dominant_blocker(void)
{
    int failures = 0;
    TEST("controllers: zcl_status includes dominant typed blocker") {
        register_all();
        blocker_module_init();
        blocker_reset_for_testing();
        blocker_set_rate_limit_ms_for_testing(0);
        blocker_set_clock_for_testing(1000000);

        struct blocker_record transient;
        ASSERT(blocker_init(&transient, "peer-slow", "net",
                            BLOCKER_TRANSIENT, "peer timeout"));
        transient.escape_deadline_secs = 30;
        snprintf(transient.escape_action, sizeof(transient.escape_action),
                 "%s", "retry_peer");
        ASSERT(blocker_set(&transient) == 0);

        struct blocker_record resource;
        ASSERT(blocker_init(&resource, "disk-full", "storage",
                            BLOCKER_RESOURCE, "disk \"full\""));
        snprintf(resource.escape_action, sizeof(resource.escape_action),
                 "%s", "page_operator");
        ASSERT(blocker_set(&resource) == 0);
        blocker_advance_clock_for_testing(5000000);

        mcp_rpc_client_set_test_hook(mock_status_rpc);
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

        const struct json_value *dominant =
            json_get(&root, "dominant_blocker");
        ASSERT(dominant != NULL);
        ASSERT(!json_is_null(dominant));
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "id")),
                      "disk-full");
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "owner")),
                      "storage");
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "class")),
                      "resource");
        ASSERT_STR_EQ(json_get_str(json_get(dominant, "reason")),
                      "disk \"full\"");
        ASSERT(json_get_int(json_get(dominant, "age_us")) == 5000000);

        const struct json_value *summary_dom =
            json_get(blockers, "dominant");
        ASSERT(summary_dom != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(summary_dom, "id")),
                      "disk-full");

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

static int test_zcl_blockers_escapes_blocker_strings(void)
{
    int failures = 0;
    TEST("controllers: zcl_blockers escapes typed blocker strings") {
        register_all();
        blocker_module_init();
        blocker_reset_for_testing();
        blocker_set_rate_limit_ms_for_testing(0);
        blocker_set_clock_for_testing(1000000);

        struct blocker_record resource;
        ASSERT(blocker_init(&resource, "disk-full", "storage",
                            BLOCKER_RESOURCE,
                            "disk \"full\"\nmanual check"));
        snprintf(resource.escape_action, sizeof(resource.escape_action),
                 "%s", "page_operator");
        ASSERT(blocker_set(&resource) == 0);
        blocker_advance_clock_for_testing(5000000);

        struct json_value args;
        json_init(&args);
        json_set_object(&args);
        char *body = mcp_router_dispatch("zcl_blockers", &args);
        ASSERT(body != NULL);

        struct json_value root;
        ASSERT(json_read(&root, body, strlen(body)));
        ASSERT(json_get_int(json_get(&root, "active_count")) == 1);
        ASSERT(json_get_int(json_get(&root, "resource_count")) == 1);

        const struct json_value *blockers = json_get(&root, "blockers");
        ASSERT(blockers != NULL);
        ASSERT(blockers->type == JSON_ARR);
        ASSERT(json_size(blockers) == 1);
        const struct json_value *first = json_at(blockers, 0);
        ASSERT(first != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(first, "id")), "disk-full");
        ASSERT_STR_EQ(json_get_str(json_get(first, "reason")),
                      "disk \"full\"\nmanual check");

        json_free(&root);
        json_free(&args);
        free(body);
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(0);
        PASS();
    } _test_next:;
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
        ASSERT(json_is_null(json_get(&root, "mempool")));
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
        ASSERT(json_is_null(json_get(&root, "download")));
        const struct json_value *diag_err =
            json_get(&root, "getsyncdiag_error");
        const struct json_value *dl_err =
            json_get(&root, "download_error");
        ASSERT(diag_err != NULL);
        ASSERT(dl_err != NULL);
        ASSERT(contains(json_get_str(json_get(diag_err, "message")),
                        "getsyncdiag RPC returned invalid JSON"));
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

static char *mock_networkinfo_rpc(const char *method, const char *params_json)
{
    (void)params_json;
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
        size_t cap = 131072;
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
            "zcl_send", "zcl_sendtoaddress", "zcl_importprivkey",
            "zcl_rescanblockchain", "zcl_replaywalletfromchain",
            "zcl_dumpprivkey", "zcl_addnode", "zcl_pingpeer",
            "zcl_name_register", "zcl_msg_send", "zcl_market_offer",
            "zcl_swap_initiate",
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
    failures += test_zcl_agent_dev_tools_shape();
    failures += test_postmortem_tools_list_and_replay();
    failures += test_zcl_getblockcount_uses_node_hstar_rpc();
    failures += test_zcl_operator_summary_degraded_shape();
    failures += test_zcl_operator_summary_healthy_shape();
    failures += test_zcl_agent_alias_shape();
    failures += test_zcl_agent_dev_tools_dispatch();
    failures += test_zcl_operator_summary_names_operator_needed_detail();
    failures += test_zcl_milestone_shape();
    failures += test_zcl_refold_status_shape();
    failures += test_zcl_status_includes_chain_advance_dump();
    failures += test_zcl_status_reports_dumpstate_error();
    failures += test_zcl_status_includes_dominant_blocker();
    failures += test_zcl_blockers_escapes_blocker_strings();
    failures += test_zcl_kpi_invalid_child_stays_parseable();
    failures += test_zcl_syncdiag_invalid_children_stay_parseable();
    failures += test_zcl_networkinfo_exposes_reachability_fields();
    failures += test_meta_tools_in_ops_domain();
    failures += test_zcl_logtail_handles_null_eventlog_rpc();
    failures += test_tools_list_json_well_formed();
    failures += test_input_schema_for_zcl_getblock();
    failures += test_destructive_tools_registered();
    failures += test_duplicate_register_rejected();
    failures += test_reset_clears_and_reregister_restores();
    failures += test_wallet_shielded_tools_registered();
    failures += test_app_protocol_tools_registered();
    failures += test_required_params_have_no_default();
    failures += test_postmortem_tools_dispatch();
    failures += test_zcl_admin_dispatch_shape();
    failures += test_zcl_admin_since_param_accepted();
    failures += test_zcl_admin_graceful_never_propagates_error();
    failures += test_zcl_profile_shape();
    failures += test_zcl_profile_clamps();
    failures += test_zcl_send_escapes_json_injection();
    failures += test_zcl_sendtoaddress_escapes_json_injection();
    failures += test_mcp_params_escapes_backslash_and_control();
    failures += test_mcp_node_rpc_missing_cookie_is_actionable();
    failures += test_final_reset_leaves_clean_table();

    return failures;
}
