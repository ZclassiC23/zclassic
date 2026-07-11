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

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)

/* Pure authorization predicate kept available to the non-dev test binary.
 * A direct router dispatch bypasses middleware, so a post-load probe is
 * permitted only for a route this exact generation replaced and only while
 * that active route is non-destructive. */
bool mcp_dev_hotswap_probe_allowed(
    const char *probe,
    const struct hotswap_load_report *rep,
    const struct mcp_tool_route *active_route,
    const char **error_code_out)
{
    const char *code = NULL;
    if (!probe || !probe[0] || !rep) {
        code = "invalid_request";
    } else if (rep->replaced_overflow ||
               rep->replaced_count > ZCL_HOTSWAP_GEN_MAX_REPLACED) {
        code = "invalid_report";
    } else {
        bool replaced = false;
        size_t probe_len = strlen(probe);
        if (probe_len < sizeof(rep->replaced[0])) {
            for (size_t i = 0; i < rep->replaced_count; i++) {
                const char *nul = memchr(rep->replaced[i], '\0',
                                         sizeof(rep->replaced[i]));
                if (nul && strcmp(rep->replaced[i], probe) == 0) {
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced)
            code = "not_replaced";
        else if (!active_route)
            code = "route_missing";
        else if (!active_route->name ||
                 strcmp(active_route->name, probe) != 0)
            code = "route_changed";
        else if (active_route->flags & MCP_TOOL_FLAG_DESTRUCTIVE)
            code = "destructive_route";
    }
    if (error_code_out)
        *error_code_out = code;
    return code == NULL;
}

#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

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

static bool hotswap_commit_mcp_routes(
    uint32_t gen,
    const struct zcl_hotswap_mcp_replacement *replacements,
    size_t replacement_count,
    char *why,
    size_t why_sz)
{
    if (!replacements || replacement_count == 0 ||
        replacement_count > ZCL_HOTSWAP_GEN_MAX_REPLACED) {
        if (why)
            snprintf(why, why_sz, "invalid staged replacement count: %zu",
                     replacement_count);
        return false;
    }
    struct mcp_router_replacement batch[ZCL_HOTSWAP_GEN_MAX_REPLACED];
    for (size_t i = 0; i < replacement_count; i++) {
        batch[i].name = replacements[i].name;
        batch[i].route = replacements[i].route;
    }
    return mcp_router_replace_batch(gen, batch, replacement_count,
                                    why, why_sz);
}

static void hotswap_push_probe_error(struct json_value *root,
                                     const char *code,
                                     const char *tool,
                                     const char *message)
{
    struct json_value error;
    json_init(&error);
    json_set_object(&error);
    json_push_kv_str(&error, "code", code ? code : "probe_failed");
    json_push_kv_str(&error, "tool", tool ? tool : "");
    json_push_kv_str(&error, "message", message ? message : "probe failed");
    json_push_kv(root, "probe_error", &error);
    json_free(&error);
}

static int h_zcl_agent_hotswap(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const char *so_path = json_get_str(json_get(req->args, "so_path"));
    const char *probe   = json_get_str(json_get(req->args, "probe_tool"));

    struct hotswap_load_report rep;
    /* The loader stages every route first; this callback publishes the whole
     * validated batch as one immutable router snapshot. The exact worker
     * datadir guard is a second boundary behind the DEV compile gate. */
    bool ok = hotswap_load(so_path, mcp_rpc_client_datadir(),
                           hotswap_commit_mcp_routes, &rep);

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_bool(&root, "ok", ok);
    json_push_kv_int(&root, "gen", (int64_t)rep.gen);
    json_push_kv_str(&root, "so_path", so_path ? so_path : "");
    if (rep.error[0])
        json_push_kv_str(&root, "error", rep.error);
    if (rep.rejection_stage[0])
        json_push_kv_str(&root, "rejection_stage", rep.rejection_stage);
    if (rep.provider_id[0])
        json_push_kv_str(&root, "provider_id", rep.provider_id);
    if (rep.build_identity[0])
        json_push_kv_str(&root, "build_identity", rep.build_identity);
    if (rep.source_identity[0])
        json_push_kv_str(&root, "source_identity", rep.source_identity);
    if (rep.input_digest[0])
        json_push_kv_str(&root, "input_content_sha256", rep.input_digest);
    if (rep.artifact_sha256[0])
        json_push_kv_str(&root, "artifact_sha256", rep.artifact_sha256);

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

    /* Optional one-shot probe of a newly replaced, non-destructive tool so
     * the caller sees the NEW behavior in the same round-trip. This calls the
     * router directly (without middleware), making both checks mandatory. */
    if (ok && probe && probe[0]) {
        const struct mcp_tool_route *active = mcp_router_find(probe);
        const char *probe_error = NULL;
        if (!mcp_dev_hotswap_probe_allowed(probe, &rep, active,
                                           &probe_error)) {
            const char *message =
                strcmp(probe_error, "destructive_route") == 0
                    ? "direct probe dispatch of destructive routes is forbidden"
                    : "probe must name a non-destructive route replaced by this generation";
            hotswap_push_probe_error(&root, probe_error, probe, message);
        } else {
            /* Dispatch the exact route that passed the policy check. Router
             * generations remain mapped, so a concurrent commit cannot turn
             * this into a destructive check/use race. */
            char *praw = mcp_router_dispatch_route(active, NULL);
            struct json_value pj;
            json_init(&pj);
            if (praw && json_read(&pj, praw, strlen(praw)))
                json_push_kv(&root, "probe", &pj);
            else
                hotswap_push_probe_error(
                    &root, "unparseable_response", probe,
                    "probe dispatch returned no parseable body");
            json_free(&pj);
            free(praw);
        }
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
      "Optional non-destructive MCP tool replaced by this generation to "
      "dispatch once after load; its result is embedded under \"probe\"",
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
