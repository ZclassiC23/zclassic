/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP dev hot-swap controller (DEV-ONLY): `zcl_agent_hotswap`.
 *
 * Drives the Tier-1 in-process hot-swap loader — an AI agent edits an
 * app-layer MCP controller .c, builds a generation .so with
 * `make hotswap-so`, then calls this tool to dlopen it into the RUNNING
 * dev node and atomically re-point the affected routes, no restart.
 *
 * The ENTIRE tool (handler, specs, route, registration body) is
 * `#ifdef ZCL_DEV_BUILD`: a release build registers nothing and links no
 * hot-swap surface. See docs/work/HOTSWAP.md. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"

#include "hotswap/hotswap.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZCL_DEV_BUILD

/* Serialize a json_value into a freshly malloc'd body (caller frees). */
static char *hotswap_json_to_body(struct json_value *v, const char *label)
{
    size_t need = json_write(v, NULL, 0);
    char *out = zcl_malloc(need + 1u, label);
    if (!out)
        return NULL;
    json_write(v, out, need + 1u);
    return out;
}

static int h_zcl_agent_hotswap(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const char *so_path = json_get_str(json_get(req->args, "so_path"));
    const char *probe   = json_get_str(json_get(req->args, "probe_tool"));

    struct hotswap_load_report rep;
    /* mcp_router_replace has the exact zcl_hotswap_replace_cb signature, so
     * it is the route-publish callback directly. The datadir comes from the
     * MCP rpc client (the node's -datadir); the loader refuses the canonical
     * live datadir as a belt-and-suspenders check behind the compile gate. */
    bool ok = hotswap_load(so_path, mcp_rpc_client_datadir(),
                           mcp_router_replace, &rep);

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_bool(&root, "ok", ok);
    json_push_kv_int(&root, "gen", (int64_t)rep.gen);
    json_push_kv_str(&root, "so_path", so_path ? so_path : "");
    if (rep.error[0])
        json_push_kv_str(&root, "error", rep.error);

    struct json_value replaced;
    json_init(&replaced);
    json_set_array(&replaced);
    for (size_t i = 0; i < rep.replaced_count; i++) {
        struct json_value item;
        json_init(&item);
        json_set_str(&item, rep.replaced[i]);
        json_push_back(&replaced, &item);
        json_free(&item);
    }
    json_push_kv(&root, "replaced", &replaced);
    json_free(&replaced);
    json_push_kv_bool(&root, "replaced_overflow", rep.replaced_overflow);

    /* Optional one-shot probe of a tool (typically one just re-pointed) so
     * the caller sees the NEW behavior in the same round-trip. */
    if (ok && probe && probe[0]) {
        char *praw = mcp_router_dispatch(probe, NULL);
        struct json_value pj;
        json_init(&pj);
        if (praw && json_read(&pj, praw, strlen(praw)))
            json_push_kv(&root, "probe", &pj);
        else
            json_push_kv_str(&root, "probe_error",
                             "probe dispatch returned no parseable body");
        json_free(&pj);
        free(praw);
    }

    if (!ok)
        LOG_WARN("mcp.dev.hotswap", "hot-swap failed: %s",
                 rep.error[0] ? rep.error : "(no detail)");

    char *body = hotswap_json_to_body(&root, "hotswap response");
    json_free(&root);
    if (!body)
        return mcp_res_set_oom(res, 0, "mcp.dev.hotswap", "hotswap response");
    res->body = body;
    return 0;
}

static const struct mcp_param_spec p_agent_hotswap[] = {
    { "so_path", MCP_PARAM_STR, true,
      "Absolute path to a generation .so built by `make hotswap-so` "
      "(must resolve under /tmp or a build/hotswap dir)",
      0, 0, 1, 500, NULL, NULL },
    { "probe_tool", MCP_PARAM_STR, false,
      "Optional MCP tool to dispatch once after load; its result is "
      "embedded under \"probe\"",
      0, 0, 0, 64, NULL, NULL },
};

static const struct mcp_tool_route k_dev_hotswap_routes[] = {
    { "zcl_agent_hotswap", "ops",
      "DEV-ONLY: dlopen a generation .so into the running dev node and "
      "atomically re-point its MCP routes with no restart. Hot-swaps are "
      "ephemeral — they revert on restart; persist via a normal rebuild.",
      p_agent_hotswap, PARAM_COUNT(p_agent_hotswap),
      h_zcl_agent_hotswap, .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
};

void mcp_register_dev_hotswap(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_dev_hotswap_routes); i++)
        mcp_router_register_required(&k_dev_hotswap_routes[i]);
}

#else /* !ZCL_DEV_BUILD — release: no hot-swap tool at all */

void mcp_register_dev_hotswap(void)
{
    /* Intentionally empty: the hot-swap surface does not exist in release. */
}

#endif /* ZCL_DEV_BUILD */
