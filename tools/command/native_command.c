/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native command adapter (contract §3, §8). Normalizes argv into a
 * registry lookup: longest-registered-path resolution, one JSON input object
 * from --input='<obj>' / --input=- / typed flags, and exactly one bounded JSON
 * document out. Unknown keys and out-of-range values are rejected before any
 * side effect; an unknown branch fails with nearby valid paths plus one
 * executable next action and NEVER becomes an arbitrary RPC method.
 *
 * READ-ONLY Core/Ops leaves execute through zcl_native_bridge_command, which
 * proxies the one canonical MCP tool for the leaf via the in-process MCP
 * middleware. Discovery leaves (help/search/describe/schema) render the native
 * discovery document directly. The `dev` subtree is intentionally NOT a root
 * here — it stays owned by tools/dev/devloop_cli.c until Wave 2.2 replaces the
 * hardcoded devloop menu with registry wrappers.
 */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "config/command_catalog.h"
#include "kernel/command_registry.h"
#include "json/json.h"

#include "chain/chainparams.h"
#include "util/safe_alloc.h"
#include "mcp/controllers.h"
#include "mcp/middleware.h"
#include "mcp/router.h"
#include "mcp/rpc_client.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── root recognition ──────────────────────────────────────────────── */
/* Canonical roots this adapter owns. `status` keeps its existing static-agent
 * path (the contract's compact status), and `dev` keeps the devloop dispatcher;
 * both are documented seams handled before this adapter is reached. */
bool zcl_native_command_is_root(const char *word)
{
    if (!word || !word[0])
        return false;
    static const char *const roots[] = {
        "core", "app", "ops", "discover", "help", "search",
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (strcmp(word, roots[i]) == 0)
            return true;
    }
    return false;
}

/* ── path -> MCP tool bindings for READ-ONLY bridge leaves ─────────────
 * Transport binding lives here, never in registry metadata. Every entry MUST
 * name a READY core/ops leaf whose .handler is zcl_native_bridge_command; the
 * golden catalog test proves the two sets agree. */
static const struct {
    const char *path;
    const char *tool;
} g_bridge_tools[] = {
    { "core.status", "zcl_status" },
    { "core.chain.tip", "zcl_chain_tip" },
    { "core.chain.block.get", "zcl_getblock" },
    { "core.chain.transaction.get", "zcl_getrawtransaction" },
    { "core.chain.mempool.status", "zcl_getmempoolinfo" },
    { "core.chain.mempool.list", "zcl_getrawmempool" },
    { "core.sync.status", "zcl_syncstate" },
    { "core.sync.validation", "zcl_validationstatus" },
    { "core.sync.blockers", "zcl_blockers" },
    { "core.sync.diagnose", "zcl_syncdiag" },
    { "core.consensus.report", "zcl_consensus_report" },
    { "core.consensus.integrity", "zcl_dataintegrity" },
    { "core.consensus.utxo.commitment", "zcl_utxocommitment" },
    { "core.consensus.utxo.audit", "zcl_utxo_audit" },
    { "core.consensus.mmb", "zcl_mmb" },
    { "core.network.status", "zcl_networkinfo" },
    { "core.network.peers.list", "zcl_peers" },
    { "core.network.peers.incidents", "zcl_peer_incidents" },
    { "core.network.peers.latency", "zcl_peerlatency" },
    { "core.network.onion.status", "zcl_onion_status" },
    { "core.network.onion.health", "zcl_onion_health" },
    { "core.wallet.status", "zcl_getwalletinfo" },
    { "core.wallet.balance", "zcl_balance" },
    { "core.wallet.address.list", "zcl_listaddresses" },
    { "core.wallet.utxo.list", "zcl_listunspent" },
    { "core.wallet.transaction.list", "zcl_listtransactions" },
    { "core.wallet.transaction.get", "zcl_gettransaction" },
    { "core.wallet.backup.status", "zcl_wallet_backup_status" },
    { "core.wallet.audit", "zcl_walletaudit" },
    { "core.storage.stats", "zcl_dbstats" },
    { "core.storage.query", "zcl_sql" },
    { "core.mining.status", "zcl_getmininginfo" },
    { "core.mining.benchmark", "zcl_benchmark" },
    { "ops.health", "zcl_health" },
    { "ops.diagnose", "zcl_agent_diagnose" },
    { "ops.lanes", "zcl_agent_lanes" },
    { "ops.logs", "zcl_node_log" },
    { "ops.timeline", "zcl_timeline" },
    { "ops.metrics", "zcl_metrics" },
    { "ops.postmortem.list", "zcl_postmortem_list" },
    { "ops.recovery.status", "zcl_refold_status" },
};

const char *zcl_native_bridge_tool_for_path(const char *path)
{
    if (!path)
        return NULL;
    for (size_t i = 0; i < sizeof(g_bridge_tools) / sizeof(g_bridge_tools[0]);
         i++) {
        if (strcmp(g_bridge_tools[i].path, path) == 0)
            return g_bridge_tools[i].tool;
    }
    return NULL;
}

/* ── in-process MCP middleware for the bridge ──────────────────────── */
static char g_bridge_datadir[512];
static int g_bridge_rpc_port;
static struct mcp_middleware g_bridge_mw;
static bool g_bridge_mw_ready;

static void bridge_ensure_middleware(void)
{
    if (g_bridge_mw_ready)
        return;
    /* Same construction as the mcpcall CLI path: a one-shot process has no
     * app_init(), so select chain params and register controllers before any
     * route can dispatch. */
    mcp_rpc_client_init(g_bridge_datadir, g_bridge_rpc_port);
    chain_params_select(CHAIN_MAIN);
    mcp_router_reset();
    mcp_register_ops();
    mcp_register_diagnostics();
    mcp_register_chain();
    mcp_register_net();
    mcp_register_wallet();
    mcp_register_app();
    mcp_register_meta();
    mcp_register_dev_hotswap();
    mcp_middleware_init(&g_bridge_mw);
    mcp_middleware_load_from_env(&g_bridge_mw);
    /* No detached timeout worker for a one-shot dispatch. */
    g_bridge_mw.default_timeout_ms = 0;
    g_bridge_mw_ready = true;
}

/* Translate the CLI leaf input into the exact argument object the bound MCP
 * tool expects. Most leaves are pass-through (their input_keys already match
 * the tool's parameter names); a few need a rename. */
static bool bridge_build_args(const char *path,
                              const struct json_value *input,
                              struct json_value *out, bool *use_out)
{
    *use_out = false;
    if (strcmp(path, "core.chain.block.get") == 0) {
        json_init(out);
        json_set_object(out);
        const struct json_value *height = json_get(input, "height");
        const struct json_value *hash = json_get(input, "hash");
        const struct json_value *verbosity = json_get(input, "verbosity");
        if (hash && !json_is_null(hash)) {
            if (!json_push_kv_str(out, "block_id", json_get_str(hash))) {
                json_free(out);
                return false;
            }
        } else if (height && !json_is_null(height)) {
            char idbuf[32];
            (void)snprintf(idbuf, sizeof(idbuf), "%lld",
                           (long long)json_get_int(height));
            if (!json_push_kv_str(out, "block_id", idbuf)) {
                json_free(out);
                return false;
            }
        }
        if (verbosity && !json_is_null(verbosity)) {
            if (!json_push_kv_int(out, "verbosity", json_get_int(verbosity))) {
                json_free(out);
                return false;
            }
        }
        *use_out = true;
        return true;
    }
    return true; /* pass-through: caller uses `input` directly */
}

void zcl_native_bridge_command(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply)
{
    if (!request || !request->spec || !reply)
        return;
    const char *tool = zcl_native_bridge_tool_for_path(request->spec->path);
    if (!tool) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "NO_BRIDGE_BINDING",
                               "dispatch", false, false,
                               "ready leaf has no MCP tool binding",
                               request->spec->path);
        return;
    }

    bridge_ensure_middleware();

    struct json_value translated;
    bool use_translated = false;
    if (!bridge_build_args(request->spec->path, request->input, &translated,
                           &use_translated)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                               "normalize", false, false,
                               "could not normalize leaf arguments",
                               request->spec->path);
        return;
    }
    const struct json_value *args =
        use_translated ? &translated : request->input;

    const char *bearer = getenv("ZCL_MCP_CALL_BEARER_TOKEN");
    if (!bearer || !bearer[0]) {
        if (mcp_middleware_is_destructive(&g_bridge_mw, tool))
            bearer = getenv("ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN");
        if (!bearer || !bearer[0])
            bearer = getenv("ZCL_MCP_BEARER_TOKEN");
    }

    char *result = mcp_middleware_dispatch(&g_bridge_mw, tool, args,
                                           bearer && bearer[0] ? bearer : NULL);
    if (use_translated)
        json_free(&translated);

    if (!result) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "the node did not return a result body", tool);
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the node is running");
        return;
    }

    struct json_value body;
    if (!json_read(&body, result, strlen(result)) || body.type != JSON_OBJ) {
        json_free(&body);
        free(result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_TOOL_BODY",
                               "serialize", false, false,
                               "tool returned a non-object body", tool);
        return;
    }
    free(result);

    const struct json_value *err = json_get(&body, "error");
    if (err && !json_is_null(err)) {
        const char *msg = NULL;
        if (err->type == JSON_OBJ)
            msg = json_get_str(json_get(err, "message"));
        else if (err->type == JSON_STR)
            msg = json_get_str(err);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "TOOL_ERROR",
                               "execute", false, false,
                               msg && msg[0] ? msg : "tool reported an error",
                               tool);
        json_free(&body);
        return;
    }

    /* Success: the tool body becomes the result envelope's data. */
    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &body);
    json_free(&body);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* ── discovery + app handlers (bound by the catalog) ───────────────── */
/* These make the discovery and app leaves independently executable through the
 * registry (e.g. via a direct execute); the CLI adapter below renders the
 * native discovery documents directly for tighter budgets. */
static const struct zcl_command_registry *catalog(void)
{
    return zcl_command_catalog();
}

static void discover_reply_document(struct zcl_command_reply *reply,
                                    size_t (*render)(
                                        const struct zcl_command_registry *,
                                        const char *, char *, size_t),
                                    const char *arg)
{
    char buf[ZCL_COMMAND_LIST_BUDGET + 1];
    size_t n = render(catalog(), arg, buf, sizeof(buf));
    if (n == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "UNKNOWN_PATH",
                               "resolve", false, false,
                               "no such command path", arg ? arg : "");
        return;
    }
    struct json_value doc;
    if (!json_read(&doc, buf, n) || doc.type != JSON_OBJ) {
        json_free(&doc);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "RENDER_FAILED",
                               "serialize", false, false,
                               "discovery document did not parse", "");
        return;
    }
    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &doc);
    json_free(&doc);
}

void zcl_native_handle_discover_help(const struct zcl_command_request *request,
                                     struct zcl_command_reply *reply)
{
    const char *path = json_get_str(json_get(request->input, "path"));
    discover_reply_document(reply, zcl_command_registry_menu_json,
                            path ? path : "");
}

void zcl_native_handle_discover_describe(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *path = json_get_str(json_get(request->input, "path"));
    discover_reply_document(reply, zcl_command_registry_describe_json,
                            path ? path : "");
}

void zcl_native_handle_discover_search(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *query = json_get_str(json_get(request->input, "query"));
    discover_reply_document(reply, zcl_command_registry_search_json,
                            query ? query : "");
}

void zcl_native_handle_discover_schema(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *path = json_get_str(json_get(request->input, "path"));
    const char *side = json_get_str(json_get(request->input, "side"));
    bool alias = false;
    const struct zcl_command_spec *spec =
        zcl_command_registry_find(catalog(), path ? path : "", &alias);
    if (!spec) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "UNKNOWN_PATH",
                               "resolve", false, false, "no such command path",
                               path ? path : "");
        return;
    }
    bool want_output = side && strcmp(side, "output") == 0;
    (void)json_push_kv_str(&reply->data, "path", spec->path);
    (void)json_push_kv_str(&reply->data, "side",
                           want_output ? "output" : "input");
    (void)json_push_kv_str(&reply->data, "id",
                           want_output ? spec->output_schema
                                       : spec->input_schema);
    (void)json_push_kv_str(&reply->data, "allowed_keys",
                           spec->input_keys ? spec->input_keys : "");
}

void zcl_native_handle_app_list(const struct zcl_command_request *request,
                                struct zcl_command_reply *reply)
{
    (void)request;
    /* Checkout-local: an installed App is a directory under apps/ with an
     * app.def manifest. Enumerate deterministically. */
    struct json_value apps;
    json_init(&apps);
    json_set_array(&apps);
    static const char *const known[] = { "social" };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        struct json_value item;
        json_init(&item);
        json_set_str(&item, known[i]);
        (void)json_push_back(&apps, &item);
        json_free(&item);
    }
    (void)json_push_kv(&reply->data, "apps", &apps);
    (void)json_push_kv_int(&reply->data, "count",
                           (int64_t)(sizeof(known) / sizeof(known[0])));
    json_free(&apps);
}

void zcl_native_handle_app_inspect(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    const char *app_id = json_get_str(json_get(request->input, "app_id"));
    if (!app_id || !app_id[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_APP_ID",
                               "normalize", false, false,
                               "app_id is required", "");
        return;
    }
    if (strcmp(app_id, "social") != 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "UNKNOWN_APP",
                               "resolve", false, false,
                               "no such installed App", app_id);
        (void)zcl_command_reply_add_next(reply, "app.list", "{}",
                                         "list installed Apps");
        return;
    }
    (void)json_push_kv_str(&reply->data, "app_id", "social");
    (void)json_push_kv_str(&reply->data, "manifest", "apps/social/app.def");
    (void)json_push_kv_str(&reply->data, "status", "checkout-only");
}

/* ── argv normalization + dispatch ─────────────────────────────────── */
enum { NC_MAX_WORDS = 64 };

static bool nc_is_flag(const char *word)
{
    return word && word[0] == '-';
}

static bool nc_is_integer(const char *s)
{
    if (!s || !s[0])
        return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+')
        i = 1;
    if (!s[i])
        return false;
    for (; s[i]; i++) {
        if (!isdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

/* Split "--key=value" (or "-key=value"). Returns false if not a value flag. */
static bool nc_split_flag(const char *word, char *key, size_t key_size,
                          const char **value)
{
    const char *p = word;
    while (*p == '-')
        p++;
    const char *eq = strchr(p, '=');
    size_t klen = eq ? (size_t)(eq - p) : strlen(p);
    if (klen == 0 || klen >= key_size)
        return false;
    memcpy(key, p, klen);
    key[klen] = 0;
    *value = eq ? eq + 1 : NULL;
    return true;
}

static bool nc_reserved_control(const char *key)
{
    static const char *const reserved[] = {
        "input", "view", "format", "max-items", "budget-bytes", "fields",
        "cursor", "quiet", "side",
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(key, reserved[i]) == 0)
            return true;
    }
    return false;
}

static bool nc_set_typed_value(struct json_value *obj, const char *key,
                               const char *value)
{
    if (!value)
        return json_push_kv_bool(obj, key, true);
    if (strcmp(value, "true") == 0)
        return json_push_kv_bool(obj, key, true);
    if (strcmp(value, "false") == 0)
        return json_push_kv_bool(obj, key, false);
    if (nc_is_integer(value))
        return json_push_kv_int(obj, key, (int64_t)strtoll(value, NULL, 10));
    return json_push_kv_str(obj, key, value);
}

static char *nc_read_stdin(void)
{
    size_t cap = 4096, len = 0;
    char *buf = (char *)zcl_malloc(cap, "native_command.stdin");
    if (!buf)
        return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= ZCL_COMMAND_MAX_INPUT) {
                free(buf);
                return NULL;
            }
            size_t ncap = cap * 2;
            char *nb = (char *)zcl_realloc(buf, ncap, "native_command.stdin");
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(STDIN_FILENO, buf + len, cap - len - 1);
        if (r < 0) {
            free(buf);
            return NULL;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    buf[len] = 0;
    return buf;
}

static void nc_print_error(const char *command, const char *code,
                           const char *phase, const char *message,
                           const char *evidence,
                           const char *next_command,
                           const char *next_input, const char *next_reason)
{
    struct json_value root, error, blockers, next, item, input;
    json_init(&root);
    json_init(&error);
    json_init(&blockers);
    json_init(&next);
    json_init(&item);
    json_init(&input);
    json_set_object(&root);
    json_set_object(&error);
    json_set_array(&blockers);
    json_set_array(&next);
    json_set_object(&item);
    json_set_object(&input);

    (void)json_push_kv_str(&root, "schema", "zcl.result.v1");
    (void)json_push_kv_str(&root, "command", command ? command : "");
    (void)json_push_kv_bool(&root, "ok", false);
    (void)json_push_kv_str(&root, "status", "failed");
    (void)json_push_kv_str(&root, "request_id", "local-cli");
    (void)json_push_kv_int(&root, "elapsed_us", 0);
    (void)json_push_kv_str(&error, "code", code);
    (void)json_push_kv_str(&error, "message", message);
    (void)json_push_kv_str(&error, "phase", phase);
    (void)json_push_kv_bool(&error, "retryable", false);
    (void)json_push_kv_bool(&error, "mutated", false);
    if (evidence && evidence[0])
        (void)json_push_kv_str(&error, "evidence", evidence);
    (void)json_push_kv(&error, "blockers", &blockers);
    (void)json_push_kv(&root, "error", &error);
    if (next_command && next_command[0]) {
        struct json_value parsed;
        if (json_read(&parsed, next_input ? next_input : "{}",
                      next_input ? strlen(next_input) : 2) &&
            parsed.type == JSON_OBJ) {
            (void)json_push_kv_str(&item, "command", next_command);
            (void)json_push_kv(&item, "input", &parsed);
            (void)json_push_kv_str(&item, "reason",
                                   next_reason ? next_reason : "");
            (void)json_push_back(&next, &item);
        }
        json_free(&parsed);
    }
    (void)json_push_kv(&root, "next", &next);

    char out[ZCL_COMMAND_ERROR_BUDGET + 1];
    size_t n = json_write(&root, out, sizeof(out));
    if (n > 0 && n < sizeof(out))
        printf("%s\n", out);
    else
        printf("{\"schema\":\"zcl.result.v1\",\"ok\":false,"
               "\"status\":\"failed\",\"error\":{\"code\":\"%s\"}}\n", code);
    json_free(&input);
    json_free(&item);
    json_free(&next);
    json_free(&blockers);
    json_free(&error);
    json_free(&root);
}

/* Print a branch menu. Returns a contract exit code. */
static int nc_emit_menu(const char *path)
{
    char out[ZCL_COMMAND_BRANCH_BUDGET + 1];
    size_t n = zcl_command_registry_menu_json(catalog(), path, out,
                                              sizeof(out));
    if (n == 0) {
        nc_print_error(path, "MENU_BUDGET", "serialize",
                       "menu exceeded its byte budget", path, "discover.help",
                       "{}", "retry discovery");
        return ZCL_COMMAND_EXIT_INTERNAL;
    }
    printf("%s\n", out);
    return ZCL_COMMAND_EXIT_OK;
}

/* Handle the four discovery leaves by rendering the native document directly. */
static int nc_run_discover(const struct zcl_command_spec *spec,
                           const char *arg, const char *side)
{
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    size_t n = 0;
    if (strcmp(spec->path, "discover.help") == 0) {
        n = zcl_command_registry_menu_json(catalog(), arg ? arg : "", out,
                                           sizeof(out));
    } else if (strcmp(spec->path, "discover.describe") == 0) {
        if (!arg || !arg[0]) {
            nc_print_error(spec->path, "MISSING_PATH", "normalize",
                           "describe requires a command path", "",
                           "discover.help", "{}", "browse the tree first");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        n = zcl_command_registry_describe_json(catalog(), arg, out,
                                               sizeof(out));
    } else if (strcmp(spec->path, "discover.search") == 0) {
        if (!arg || !arg[0]) {
            nc_print_error(spec->path, "MISSING_QUERY", "normalize",
                           "search requires a query", "", "discover.help",
                           "{}", "browse the tree first");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        n = zcl_command_registry_search_json(catalog(), arg, out,
                                             sizeof(out));
    } else { /* discover.schema */
        struct zcl_command_request req = { 0 };
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        (void)json_push_kv_str(&input, "path", arg ? arg : "");
        if (side)
            (void)json_push_kv_str(&input, "side", side);
        req.spec = spec;
        req.input = &input;
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, spec->output_schema);
        zcl_native_handle_discover_schema(&req, &reply);
        (void)json_push_kv_str(&reply.data, "schema", "zcl.command_schema.v1");
        n = json_write(&reply.data, out, sizeof(out));
        if (reply.exit_code != ZCL_COMMAND_EXIT_OK) {
            nc_print_error(spec->path, "UNKNOWN_PATH", "resolve",
                           "no such command path", arg ? arg : "",
                           "discover.help", "{}", "browse the tree first");
            zcl_command_reply_free(&reply);
            json_free(&input);
            return ZCL_COMMAND_EXIT_INVALID;
        }
        zcl_command_reply_free(&reply);
        json_free(&input);
    }
    if (n == 0) {
        nc_print_error(spec->path, "UNKNOWN_PATH", "resolve",
                       "no such command path or budget exceeded",
                       arg ? arg : "", "discover.help", "{}",
                       "browse the tree first");
        return ZCL_COMMAND_EXIT_INVALID;
    }
    printf("%s\n", out);
    return ZCL_COMMAND_EXIT_OK;
}

int zcl_native_command_main(const char *root_word, const char *const *args,
                            int nargs, const char *datadir, int rpc_port)
{
    if (!root_word || !root_word[0])
        return ZCL_COMMAND_EXIT_INVALID;
    (void)snprintf(g_bridge_datadir, sizeof(g_bridge_datadir), "%s",
                   datadir ? datadir : "");
    g_bridge_rpc_port = rpc_port;

    const struct zcl_command_registry *reg = catalog();
    char why[128];
    if (!zcl_command_registry_validate(reg, why, sizeof(why))) {
        nc_print_error(root_word, "REGISTRY_INVALID", "startup", why, "",
                       "", "", "");
        return ZCL_COMMAND_EXIT_INTERNAL;
    }

    /* Word list = root + args (flags included). Resolution stops at the first
     * flag or dotted/pathy word. */
    const char *words[NC_MAX_WORDS];
    size_t count = 0;
    words[count++] = root_word;
    for (int i = 0; i < nargs && count < NC_MAX_WORDS; i++)
        words[count++] = args[i];

    size_t consumed = 0;
    bool was_alias = false;
    char invoked[ZCL_COMMAND_MAX_PATH];
    const struct zcl_command_spec *spec = zcl_command_registry_resolve_words(
        reg, words, count, &consumed, &was_alias, invoked, sizeof(invoked));
    if (!spec) {
        nc_print_error(root_word, "UNKNOWN_COMMAND", "resolve",
                       "unknown command root", root_word, "discover.search",
                       "{}", "search for the intended command");
        return ZCL_COMMAND_EXIT_INVALID;
    }

    /* Collect the tokens the path did not consume: positionals in order and
     * value flags into a scratch object. */
    const char *positional[NC_MAX_WORDS];
    size_t npos = 0;
    struct json_value flags;
    json_init(&flags);
    json_set_object(&flags);
    const char *input_flag = NULL;
    const char *view = NULL;
    const char *side = NULL;
    size_t budget = 0;
    bool flag_error = false;
    char flag_key[128];
    for (size_t i = consumed; i < count; i++) {
        const char *w = words[i];
        if (!nc_is_flag(w)) {
            if (npos < NC_MAX_WORDS)
                positional[npos++] = w;
            continue;
        }
        const char *value = NULL;
        if (!nc_split_flag(w, flag_key, sizeof(flag_key), &value)) {
            flag_error = true;
            break;
        }
        if (strcmp(flag_key, "input") == 0) {
            input_flag = value ? value : "";
        } else if (strcmp(flag_key, "view") == 0) {
            view = value;
        } else if (strcmp(flag_key, "side") == 0) {
            side = value;
        } else if (strcmp(flag_key, "budget-bytes") == 0) {
            if (value && nc_is_integer(value))
                budget = (size_t)strtoull(value, NULL, 10);
        } else if (nc_reserved_control(flag_key)) {
            /* accepted, no effect in Wave 1.2 */
        } else if (!nc_set_typed_value(&flags, flag_key, value)) {
            flag_error = true;
            break;
        }
    }
    if (flag_error) {
        json_free(&flags);
        nc_print_error(spec->path, "BAD_FLAG", "normalize",
                       "malformed or duplicate option", spec->path,
                       "discover.describe", "{}", "inspect the input schema");
        return ZCL_COMMAND_EXIT_INVALID;
    }

    /* Discovery leaves render their native document directly. */
    if (spec->layer == ZCL_COMMAND_LAYER_DISCOVER &&
        spec->mode != ZCL_COMMAND_MODE_BRANCH) {
        const char *arg = npos > 0 ? positional[0] : NULL;
        int rc = nc_run_discover(spec, arg, side);
        json_free(&flags);
        return rc;
    }

    /* A branch: no deeper leaf resolved. */
    if (spec->mode == ZCL_COMMAND_MODE_BRANCH) {
        if (npos > 0) {
            char attempted[ZCL_COMMAND_MAX_PATH];
            (void)snprintf(attempted, sizeof(attempted), "%s.%s", spec->path,
                           positional[0]);
            char query[160];
            (void)snprintf(query, sizeof(query), "{\"query\":\"%s\"}",
                           positional[0]);
            json_free(&flags);
            nc_print_error(attempted, "UNKNOWN_COMMAND", "resolve",
                           "no such command under this branch", attempted,
                           "discover.search", query,
                           "search for the intended command");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        int rc = nc_emit_menu(spec->path);
        json_free(&flags);
        return rc;
    }

    /* A leaf: build the one JSON input object. */
    struct json_value input;
    json_init(&input);
    if (input_flag) {
        if (strcmp(input_flag, "-") == 0) {
            char *raw = nc_read_stdin();
            bool ok = raw && json_read(&input, raw, strlen(raw)) &&
                      input.type == JSON_OBJ;
            free(raw);
            if (!ok) {
                json_free(&input);
                json_free(&flags);
                nc_print_error(spec->path, "BAD_INPUT", "normalize",
                               "stdin --input=- must be one JSON object",
                               spec->path, "discover.schema", "{}",
                               "inspect the input schema");
                return ZCL_COMMAND_EXIT_INVALID;
            }
        } else if (!json_read(&input, input_flag, strlen(input_flag)) ||
                   input.type != JSON_OBJ) {
            json_free(&input);
            json_free(&flags);
            nc_print_error(spec->path, "BAD_INPUT", "normalize",
                           "--input must be one JSON object", spec->path,
                           "discover.schema", "{}", "inspect the input schema");
            return ZCL_COMMAND_EXIT_INVALID;
        }
    } else {
        json_set_object(&input);
    }

    /* Merge typed flags into the input object. */
    for (size_t i = 0; i < flags.num_children; i++) {
        struct json_value copy;
        json_init(&copy);
        json_copy(&copy, &flags.children[i]);
        (void)json_push_kv(&input, flags.keys[i], &copy);
        json_free(&copy);
    }
    json_free(&flags);

    /* Map positionals onto positional_keys in order. */
    if (npos > 0) {
        const char *pk = spec->positional_keys ? spec->positional_keys : "";
        size_t used = 0;
        const char *at = pk;
        for (size_t i = 0; i < npos; i++) {
            if (!at || !*at) {
                json_free(&input);
                nc_print_error(spec->path, "TOO_MANY_ARGS", "normalize",
                               "more positional arguments than the leaf accepts",
                               spec->path, "discover.schema", "{}",
                               "inspect the input schema");
                return ZCL_COMMAND_EXIT_INVALID;
            }
            const char *end = strchr(at, ',');
            size_t klen = end ? (size_t)(end - at) : strlen(at);
            char key[64];
            if (klen >= sizeof(key)) {
                json_free(&input);
                nc_print_error(spec->path, "BAD_SCHEMA", "normalize",
                               "positional key too long", spec->path, "", "",
                               "");
                return ZCL_COMMAND_EXIT_INTERNAL;
            }
            memcpy(key, at, klen);
            key[klen] = 0;
            if (!nc_set_typed_value(&input, key, positional[i])) {
                json_free(&input);
                nc_print_error(spec->path, "BAD_INPUT", "normalize",
                               "could not set positional argument", key, "",
                               "", "");
                return ZCL_COMMAND_EXIT_INTERNAL;
            }
            used++;
            at = end ? end + 1 : NULL;
        }
        (void)used;
    }

    /* Reject unknown keys and duplicates before any side effect. */
    if (!zcl_command_registry_input_validate(spec, &input, why, sizeof(why))) {
        json_free(&input);
        nc_print_error(spec->path, "INVALID_INPUT", "normalize", why,
                       spec->path, "discover.schema", "{}",
                       "inspect the input schema");
        return ZCL_COMMAND_EXIT_INVALID;
    }

    struct zcl_command_context ctx = {
        .registry = reg,
        .source_root = getenv("ZCL_DEV_SOURCE_ROOT"),
        .operator_lane = getenv("ZCL_OPERATOR_LANE"),
        .granted_capabilities = ~(uint64_t)0,
#ifdef ZCL_DEV_BUILD
        .dev_build = true,
#else
        .dev_build = false,
#endif
    };

    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    enum zcl_command_exit exit_code = ZCL_COMMAND_EXIT_INTERNAL;
    size_t n = zcl_command_registry_execute_json(
        reg, spec, &ctx, &input, was_alias, invoked, view, budget, out,
        sizeof(out), &exit_code);
    json_free(&input);
    if (n == 0) {
        nc_print_error(spec->path, "EXECUTE_FAILED", "serialize",
                       "handler produced no bounded result", spec->path, "",
                       "", "");
        return ZCL_COMMAND_EXIT_INTERNAL;
    }
    printf("%s\n", out);
    return (int)exit_code;
}
