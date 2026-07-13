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

/* A route probe proves usable behavior, not merely that the handler returned
 * syntactically valid JSON. MCP handlers encode operational failure as a
 * top-level {"error":...} envelope; accepting that envelope made an offline
 * node look like a successful hot-swap probe. Kept pure/test-visible so the
 * transport and controller share a pinned definition of success. */
bool mcp_dev_hotswap_probe_body_ok(
    const char *body,
    char *error_code_out, size_t error_code_sz,
    char *error_message_out, size_t error_message_sz)
{
    if (error_code_out && error_code_sz)
        error_code_out[0] = '\0';
    if (error_message_out && error_message_sz)
        error_message_out[0] = '\0';

    struct json_value parsed;
    json_init(&parsed);
    if (!body || !json_read(&parsed, body, body ? strlen(body) : 0)) {
        if (error_code_out && error_code_sz)
            snprintf(error_code_out, error_code_sz, "unparseable_response");
        if (error_message_out && error_message_sz)
            snprintf(error_message_out, error_message_sz,
                     "probe dispatch returned no parseable body");
        json_free(&parsed);
        return false;
    }

    if (parsed.type != JSON_OBJ) {
        if (error_code_out && error_code_sz)
            snprintf(error_code_out, error_code_sz, "invalid_response_shape");
        if (error_message_out && error_message_sz)
            snprintf(error_message_out, error_message_sz,
                     "probe response must be one JSON object");
        json_free(&parsed);
        return false;
    }
    const struct json_value *error = json_get(&parsed, "error");
    if (error && error->type != JSON_NULL) {
        const struct json_value *message = error->type == JSON_OBJ
            ? json_get(error, "message") : NULL;
        if (error_code_out && error_code_sz)
            snprintf(error_code_out, error_code_sz, "handler_error");
        if (error_message_out && error_message_sz)
            snprintf(error_message_out, error_message_sz,
                     "probe handler failed: %s",
                     message && message->type == JSON_STR
                         ? json_get_str(message) : "MCP error envelope");
        json_free(&parsed);
        return false;
    }
    const struct json_value *ok = json_get(&parsed, "ok");
    if (ok && (ok->type != JSON_BOOL || !json_get_bool(ok))) {
        if (error_code_out && error_code_sz)
            snprintf(error_code_out, error_code_sz, "unsuccessful_response");
        if (error_message_out && error_message_sz)
            snprintf(error_message_out, error_message_sz,
                     "probe response explicitly reported ok=false");
        json_free(&parsed);
        return false;
    }
    const struct json_value *status = json_get(&parsed, "status");
    if (status && status->type == JSON_STR &&
        (strcmp(json_get_str(status), "failed") == 0 ||
         strcmp(json_get_str(status), "blocked") == 0 ||
         strcmp(json_get_str(status), "error") == 0)) {
        if (error_code_out && error_code_sz)
            snprintf(error_code_out, error_code_sz, "unsuccessful_status");
        if (error_message_out && error_message_sz)
            snprintf(error_message_out, error_message_sz,
                     "probe response reported status=%s",
                     json_get_str(status));
        json_free(&parsed);
        return false;
    }
    json_free(&parsed);
    return true;
}

#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

#ifdef ZCL_DEV_BUILD

struct hotswap_commit_context {
    const char *probe;
    char *probe_body;
    char probe_error_code[64];
    char probe_error_message[256];
};

static bool hotswap_commit_probe_candidate(
    struct hotswap_commit_context *context,
    const struct zcl_hotswap_mcp_replacement *replacements,
    size_t replacement_count,
    char *why,
    size_t why_sz)
{
    if (!context || !context->probe || !context->probe[0])
        return true;

    const struct mcp_tool_route *route = NULL;
    for (size_t i = 0; i < replacement_count; i++) {
        if (replacements[i].name &&
            strcmp(replacements[i].name, context->probe) == 0) {
            route = replacements[i].route;
            break;
        }
    }
    if (!route || !route->name || strcmp(route->name, context->probe) != 0) {
        snprintf(context->probe_error_code,
                 sizeof(context->probe_error_code), "not_replaced");
        snprintf(context->probe_error_message,
                 sizeof(context->probe_error_message),
                 "probe must name a route replaced by this generation");
    } else if (route->flags & MCP_TOOL_FLAG_DESTRUCTIVE) {
        snprintf(context->probe_error_code,
                 sizeof(context->probe_error_code), "destructive_route");
        snprintf(context->probe_error_message,
                 sizeof(context->probe_error_message),
                 "direct probe dispatch of destructive routes is forbidden");
    } else {
        context->probe_body = mcp_router_dispatch_route(route, NULL);
        if (mcp_dev_hotswap_probe_body_ok(
                context->probe_body,
                context->probe_error_code,
                sizeof(context->probe_error_code),
                context->probe_error_message,
                sizeof(context->probe_error_message)))
            return true;
    }

    if (why && why_sz)
        snprintf(why, why_sz, "candidate probe '%s' failed: %s",
                 context->probe,
                 context->probe_error_message[0]
                     ? context->probe_error_message : "unknown probe failure");
    free(context->probe_body);
    context->probe_body = NULL;
    return false;
}

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
    void *opaque_context,
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
    if (!hotswap_commit_probe_candidate(opaque_context, replacements,
                                        replacement_count, why, why_sz))
        return false;
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
    /* Legacy MCP must never remain an alternate publication authority while
     * the native transaction is contained.  Keep the route only long enough
     * to return an explicit, machine-readable refusal during zero-MCP
     * migration; do not reach dlopen or a registry commit. */
    (void)req;
    struct json_value contained;
    json_init(&contained);
    json_set_object(&contained);
    json_push_kv_bool(&contained, "ok", false);
    json_push_kv_str(&contained, "error", "runtime_publication_contained");
    json_push_kv_str(&contained, "rejection_stage", "authority");
    json_push_kv_str(
        &contained, "detail",
        "legacy MCP hot-swap cannot publish; resident candidate probing is "
        "also contained until disposable workers, pre-load ELF policy, "
        "immutable epochs, and proof receipts are transactional");
    char *contained_body =
        hotswap_json_to_body(&contained, "contained hotswap response");
    json_free(&contained);
    if (!contained_body)
        return mcp_res_set_oom(res, 0, "mcp.dev.hotswap",
                               "contained hotswap response");
    res->body = contained_body;
    return 0;

    const char *so_path = json_get_str(json_get(req->args, "so_path"));
    const char *probe   = json_get_str(json_get(req->args, "probe_tool"));

    struct hotswap_load_report rep;
    struct hotswap_commit_context commit_context = { .probe = probe };
    /* The loader stages every route first; this callback publishes the whole
     * validated batch as one immutable router snapshot. The exact worker
     * datadir guard is a second boundary behind the DEV compile gate. */
    bool ok = hotswap_load(so_path, mcp_rpc_client_datadir(), probe,
                           hotswap_commit_mcp_routes, &commit_context, &rep);

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

    /* The commit callback ran the selected route against the candidate table
     * before mcp_router_replace_batch's release-store. A failed probe therefore
     * reaches this response with ok=false and the resident router unchanged. */
    if (commit_context.probe_error_code[0]) {
        hotswap_push_probe_error(&root, commit_context.probe_error_code,
                                 probe, commit_context.probe_error_message);
    } else if (ok && commit_context.probe_body) {
        struct json_value pj;
        json_init(&pj);
        if (json_read(&pj, commit_context.probe_body,
                      strlen(commit_context.probe_body)))
            json_push_kv(&root, "probe", &pj);
        json_free(&pj);
    }
    free(commit_context.probe_body);

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
    { "probe_tool", MCP_PARAM_STR, true,
      "Canonical non-destructive MCP tool owned by this generation; it is "
      "dispatched against the candidate route table before publication",
      0, 0, 1, 64, NULL, NULL },
};

static const struct mcp_tool_route k_dev_hotswap_routes[] = {
    { "zcl_agent_hotswap", "ops",
      "DEV-ONLY legacy compatibility route: runtime publication is contained; "
      "always returns runtime_publication_contained and never calls dlopen.",
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
