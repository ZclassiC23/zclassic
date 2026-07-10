/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Focused proofs for the persistent dev-node MCP RPC bridge. */

#include "test/test_helpers.h"

#include "mcp/dev_rpc_bridge.h"
#include "mcp/router.h"
#include "json/json.h"
#include "rpc/client.h"
#include "rpc/server.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int h_bridge_generation_one(const struct mcp_request *req,
                                   struct mcp_response *res)
{
    (void)req;
    res->body = zcl_malloc(32, "dev bridge test generation one");
    if (!res->body)
        return -1;
    snprintf(res->body, 32, "{\"generation\":1}");
    return 0;
}

static int h_bridge_generation_two(const struct mcp_request *req,
                                   struct mcp_response *res)
{
    (void)req;
    res->body = zcl_malloc(32, "dev bridge test generation two");
    if (!res->body)
        return -1;
    snprintf(res->body, 32, "{\"generation\":2}");
    return 0;
}

static const struct mcp_tool_route k_bridge_generation_one = {
    "t.dev_bridge_generation", "test", "resident generation one",
    NULL, 0, h_bridge_generation_one, 0, NULL
};

static const struct mcp_tool_route k_bridge_generation_two = {
    "t.dev_bridge_generation", "test", "resident generation two",
    NULL, 0, h_bridge_generation_two, 0, NULL
};

static bool call_dev_mcp(const struct rpc_command *command,
                         int64_t *generation_out)
{
    struct json_value params;
    struct json_value tool;
    struct json_value args;
    struct json_value result;
    bool ok;

    json_init(&params);
    json_set_array(&params);
    json_init(&tool);
    json_set_str(&tool, "t.dev_bridge_generation");
    json_push_back(&params, &tool);
    json_free(&tool);
    json_init(&args);
    json_set_object(&args);
    json_push_back(&params, &args);
    json_free(&args);
    json_init(&result);

    ok = command && command->actor(&params, false, &result);
    if (ok && generation_out)
        *generation_out = json_get_int(json_get(&result, "generation"));
    json_free(&result);
    json_free(&params);
    return ok;
}

int test_dev_mcp_rpc_bridge(void)
{
    int failures = 0;
    TEST("dev MCP RPC bridge is release-absent, exact-lane, and persistent") {
        const char *old_home = getenv("HOME");
        char old_home_copy[1024] = {0};
        bool old_home_present = old_home && old_home[0];
        char test_home[256];
        char dev_datadir[512];
        char canonical[512];
        char soak[512];
        struct rpc_table release_table;
        struct rpc_table dev_table;

        if (old_home_present)
            snprintf(old_home_copy, sizeof(old_home_copy), "%s", old_home);
        snprintf(test_home, sizeof(test_home), "/tmp/zcl-dev-bridge-%d",
                 (int)getpid());
        ASSERT(setenv("HOME", test_home, 1) == 0);
        snprintf(dev_datadir, sizeof(dev_datadir), "%s/.zclassic-c23-dev",
                 test_home);
        snprintf(canonical, sizeof(canonical), "%s/.zclassic-c23", test_home);
        snprintf(soak, sizeof(soak), "%s/.zclassic-c23-soak", test_home);

        ASSERT(dev_mcp_rpc_bridge_datadir_allowed(dev_datadir));
        ASSERT(!dev_mcp_rpc_bridge_datadir_allowed(canonical));
        ASSERT(!dev_mcp_rpc_bridge_datadir_allowed(soak));
        ASSERT(!dev_mcp_rpc_bridge_datadir_allowed("/tmp/arbitrary-dev"));

        /* The documented Make/watcher invocation passes both positional
         * values as strings. Keep this method out of the legacy CLI JSON
         * conversion table so an absolute path or probe name cannot be
         * reinterpreted before it reaches the resident RPC command. */
        const char *cli_args[] = {
            "/tmp/zclassic23-generation.so", "zcl_name_list"
        };
        struct json_value converted;
        ASSERT(!rpc_should_convert_param("dev_hotswap", 0));
        ASSERT(!rpc_should_convert_param("dev_hotswap", 1));
        ASSERT(rpc_convert_values("dev_hotswap", cli_args, 2, &converted));
        ASSERT(json_at(&converted, 0)->type == JSON_STR);
        ASSERT(strcmp(json_get_str(json_at(&converted, 0)), cli_args[0]) == 0);
        ASSERT(json_at(&converted, 1)->type == JSON_STR);
        ASSERT(strcmp(json_get_str(json_at(&converted, 1)), cli_args[1]) == 0);
        json_free(&converted);

        const char *mcp_cli_args[] = {
            "zcl_state", "{\"subsystem\":\"hotswap\"}"
        };
        ASSERT(!rpc_should_convert_param("dev_mcp_call", 0));
        ASSERT(rpc_should_convert_param("dev_mcp_call", 1));
        ASSERT(rpc_convert_values("dev_mcp_call", mcp_cli_args, 2,
                                  &converted));
        ASSERT(json_at(&converted, 0)->type == JSON_STR);
        ASSERT(strcmp(json_get_str(json_at(&converted, 0)),
                      mcp_cli_args[0]) == 0);
        ASSERT(json_at(&converted, 1)->type == JSON_OBJ);
        ASSERT(strcmp(json_get_str(json_get(json_at(&converted, 1),
                                            "subsystem")),
                      "hotswap") == 0);
        json_free(&converted);

        /* The ordinary test/release registration entry is a hard stub: no
         * callable loader-facing command enters a non-dev RPC table. */
        rpc_table_init(&release_table);
        ASSERT(register_dev_mcp_rpc_commands(&release_table, dev_datadir,
                                             18252));
        ASSERT(rpc_table_find(&release_table, "dev_hotswap") == NULL);
        ASSERT(rpc_table_find(&release_table, "dev_mcp_call") == NULL);

        /* The ZCL_TESTING-only seam exercises the exact code compiled by a
         * ZCL_DEV_BUILD. Wrong lane appends nothing; exact dev appends both. */
        rpc_table_init(&dev_table);
        ASSERT(!dev_mcp_rpc_bridge_test_register(&dev_table, canonical, 18252));
        ASSERT(dev_table.num_commands == 0);
        ASSERT(dev_mcp_rpc_bridge_test_register(&dev_table, dev_datadir, 18252));
        ASSERT(rpc_table_find(&dev_table, "dev_hotswap") != NULL);
        const struct rpc_command *call =
            rpc_table_find(&dev_table, "dev_mcp_call");
        ASSERT(call != NULL);

        /* Two RPC calls use the same resident router. Re-pointing the route
         * between them is visible on the second call without re-registering or
         * constructing another process — the persistence property the bridge
         * exists to provide. */
        ASSERT(mcp_router_register(&k_bridge_generation_one));
        int64_t generation = 0;
        ASSERT(call_dev_mcp(call, &generation));
        ASSERT(generation == 1);
        ASSERT(mcp_router_replace("t.dev_bridge_generation",
                                  &k_bridge_generation_two));
        ASSERT(call_dev_mcp(call, &generation));
        ASSERT(generation == 2);

        if (old_home_present)
            ASSERT(setenv("HOME", old_home_copy, 1) == 0);
        else
            ASSERT(unsetenv("HOME") == 0);
        PASS();
    } _test_next:;
    return failures;
}
