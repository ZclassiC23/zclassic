/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Hot-swap end-to-end DEMO fixture (NOT compiled into the node — it lives
 * under tools/scripts, which the MCP source glob does not pick up). It is
 * compiled ONLY by `make hotswap-so FILES=tools/scripts/hotswap_demo_controller.c`
 * into a generation .so.
 *
 * The generation re-points the "zcl_name_list" route at a self-contained
 * handler that returns a marker string WITHOUT calling the node RPC, so
 * tools/scripts/hotswap_demo.sh can prove — in a single process, with no
 * running node — that dispatch runs the freshly-loaded code. Edit the marker
 * below and rebuild to watch the swapped-in behavior change. */

#include "mcp/router.h"
#include "hotswap/hotswap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The "visibly changed" behavior: a constant marker body. Bump this string
 * and rebuild the .so to see the running dispatcher pick up the new value. */
static int h_hotswap_demo(const struct mcp_request *req,
                          struct mcp_response *res)
{
    (void)req;
    res->body = strdup(
        "{\"hotswap_demo\":\"NEW-GENERATION-HANDLER\","
        "\"tool\":\"zcl_name_list\"}");
    if (!res->body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "hotswap demo strdup failed");
        return -1; /* raw-return-ok:demo-fixture-not-in-node */
    }
    return 0;
}

/* Same NAME as the live route so mcp_router_replace finds the slot. */
static const struct mcp_tool_route k_routes[] = {
    { "zcl_name_list", "app",
      "hot-swap demo: this route was re-pointed by a generation .so",
      NULL, 0, h_hotswap_demo, 0, NULL },
};

ZCL_HOTSWAP_EXPORT_ROUTES(k_routes, sizeof(k_routes) / sizeof(k_routes[0]))
