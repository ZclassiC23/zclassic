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
 * dispatches WITHOUT the MCP router/middleware (ZERO-MCP W0-A): a leaf either
 * calls its re-homed transport-neutral body function (app/controllers/
 * *_native_handlers.c — the same composition the MCP controller now wraps) or,
 * for a pure 1:1 proxy, calls the backing JSON-RPC method directly. Discovery
 * leaves (help/search/describe/schema) render the native discovery document
 * directly. The `dev` subtree uses this same resolver; its process and watcher
 * handlers are injected only in a ZCL_DEV_BUILD catalog.
 */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "config/command_catalog.h"
#include "kernel/command_registry.h"
#include "json/json.h"

#include "chain/chainparams.h"
#include "platform/time_compat.h"
#include "util/safe_alloc.h"
#include "util/boot_status.h"
#include "controllers/native_handler_body.h"
#include "controllers/status_native_handlers.h"
#include "controllers/chain_native_handlers.h"
#include "controllers/wallet_native_handlers.h"
#include "controllers/diagnostics_native_handlers.h"
#include "controllers/net_native_handlers.h"
#include "controllers/meta_native_handlers.h"
#include "mcp/rpc_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── root recognition ──────────────────────────────────────────────── */
/* Canonical roots this adapter owns. `status` keeps its existing static-agent
 * path (the contract's compact status). */
bool zcl_native_command_is_root(const char *word)
{
    if (!word || !word[0])
        return false;
    static const char *const roots[] = {
        "core", "app", "dev", "ops", "discover", "code", "help", "search",
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
    { "core.status.brief", "zcl_status_brief" },
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

/* ── MCP-free dispatch bindings for the bridge (W0-A) ─────────────────
 * Every bridged leaf resolves to exactly ONE of:
 *   - a re-homed transport-neutral body function (the same composition the
 *     MCP controller now wraps — app/controllers *_native_handlers.c), or
 *   - a direct JSON-RPC method (the leaf's legacy MCP handler was a pure
 *     DEFINE_PT pass-through, so the composition already lives in the RPC
 *     layer and the tool added nothing but transport).
 * The golden catalog test proves the union covers g_bridge_tools[] exactly. */
static const struct {
    const char *path;
    zcl_native_body_fn body;
} g_bridge_native_body[] = {
    { "core.status", zcl_native_status_body },
    { "core.status.brief", zcl_native_status_brief_body },
    { "core.chain.block.get", zcl_native_getblock_body },
    { "core.chain.transaction.get", zcl_native_getrawtransaction_body },
    { "core.sync.blockers", zcl_native_blockers_body },
    { "core.sync.diagnose", zcl_native_syncdiag_body },
    { "core.consensus.report", zcl_native_consensus_report_body },
    { "core.consensus.utxo.audit", zcl_native_utxo_audit_body },
    { "core.network.peers.incidents", zcl_native_peer_incidents_body },
    { "core.network.onion.health", zcl_native_onion_health_body },
    { "core.wallet.address.list", zcl_native_listaddresses_body },
    { "core.wallet.utxo.list", zcl_native_listunspent_body },
    { "core.wallet.transaction.list", zcl_native_listtransactions_body },
    { "core.wallet.transaction.get", zcl_native_gettransaction_body },
    { "core.storage.query", zcl_native_sql_body },
    { "ops.diagnose", zcl_native_agent_diagnose_body },
    { "ops.logs", zcl_native_node_log_body },
    { "ops.timeline", zcl_native_timeline_body },
    { "ops.metrics", zcl_native_metrics_body },
    { "ops.postmortem.list", zcl_native_postmortem_list_body },
};

static const struct {
    const char *path;
    const char *rpc_method;
} g_bridge_rpc_direct[] = {
    { "core.chain.tip", "getchaintip" },
    { "core.chain.mempool.status", "getmempoolinfo" },
    { "core.chain.mempool.list", "getrawmempool" },
    { "core.sync.status", "syncstate" },
    { "core.sync.validation", "validationstatus" },
    { "core.consensus.integrity", "getdataintegrity" },
    { "core.consensus.utxo.commitment", "getutxocommitment" },
    { "core.consensus.mmb", "getmmrroot" },
    { "core.network.status", "getnetworkinfo" },
    { "core.network.peers.list", "getpeerinfo" },
    { "core.network.peers.latency", "getpeerlatency" },
    { "core.network.onion.status", "healthcheck" },
    { "core.wallet.status", "getwalletinfo" },
    { "core.wallet.balance", "z_gettotalbalance" },
    { "core.wallet.backup.status", "walletbackupstatus" },
    { "core.wallet.audit", "walletaudit" },
    { "core.storage.stats", "db_info" },
    { "core.mining.status", "getmininginfo" },
    { "core.mining.benchmark", "benchmark" },
    { "ops.health", "healthcheck" },
    { "ops.lanes", "agentlanes" },
    { "ops.recovery.status", "refold" },
};

zcl_native_body_fn zcl_native_bridge_body_for_path(const char *path)
{
    if (!path)
        return NULL;
    for (size_t i = 0;
         i < sizeof(g_bridge_native_body) / sizeof(g_bridge_native_body[0]);
         i++) {
        if (strcmp(g_bridge_native_body[i].path, path) == 0)
            return g_bridge_native_body[i].body;
    }
    return NULL;
}

const char *zcl_native_bridge_rpc_for_path(const char *path)
{
    if (!path)
        return NULL;
    for (size_t i = 0;
         i < sizeof(g_bridge_rpc_direct) / sizeof(g_bridge_rpc_direct[0]);
         i++) {
        if (strcmp(g_bridge_rpc_direct[i].path, path) == 0)
            return g_bridge_rpc_direct[i].rpc_method;
    }
    return NULL;
}

/* ── one-shot RPC client bootstrap ──────────────────────────────────── */
static char g_bridge_datadir[512];
static int g_bridge_rpc_port;
static bool g_bridge_rpc_ready;

static void bridge_ensure_rpc_client(void)
{
    if (g_bridge_rpc_ready)
        return;
    /* A one-shot native process has no app_init(): initialize the JSON-RPC
     * client (datadir cookie + port) and select mainnet chain params for any
     * body function that consults them. No MCP router/middleware is built. */
    mcp_rpc_client_init(g_bridge_datadir, g_bridge_rpc_port);
    chain_params_select(CHAIN_MAIN);
    g_bridge_rpc_ready = true;
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

/* ── progressive-disclosure projection (contract §8/§9) ──────────────────
 * A bridged tool body can exceed the 4096-byte ordinary-result budget. Rather
 * than fail with RESPONSE_BUDGET_EXCEEDED, project the top-level object to fit:
 *   summary — scalar top-level fields only (containers dropped);
 *   normal  — greedy top-level fields in order until the budget (default);
 *   full    — greedy from --cursor, honoring --max-items, paging via a cursor.
 * Truncation is always explicit: a `_page` object records it and the envelope
 * gets one structured retrieval command (same leaf, --view=full --cursor=N). */
enum { NC_ENVELOPE_RESERVE = 768 };

static bool nc_is_scalar(const struct json_value *v)
{
    return v && v->type <= JSON_STR; /* NULL/BOOL/INT/REAL/STR */
}

static size_t nc_obj_size(const struct json_value *obj)
{
    char scratch[ZCL_COMMAND_LIST_BUDGET + 1];
    size_t n = json_write(obj, scratch, sizeof(scratch));
    return (n == 0 || n >= sizeof(scratch)) ? sizeof(scratch) : n;
}

void zcl_native_bridge_project(const struct zcl_command_request *request,
                               const struct json_value *body,
                               struct zcl_command_reply *reply)
{
    const char *view = request->view && request->view[0] ? request->view
                                                          : "normal";
    bool summary = strcmp(view, "summary") == 0;
    bool full = strcmp(view, "full") == 0;

    size_t contract = ZCL_COMMAND_RESULT_BUDGET;
    if (request->budget_bytes > 0 && request->budget_bytes < contract)
        contract = request->budget_bytes;
    size_t data_budget = contract > NC_ENVELOPE_RESERVE
                             ? contract - NC_ENVELOPE_RESERVE
                             : contract / 2;

    size_t start = 0;
    if (full && request->cursor && request->cursor[0]) {
        char *end = NULL;
        unsigned long long c = strtoull(request->cursor, &end, 10);
        if (end && !*end)
            start = (size_t)c;
    }
    size_t total = body->num_children;

    struct json_value acc;
    json_init(&acc);
    json_set_object(&acc);
    size_t included = 0, omitted = 0, next_cursor = total;
    bool truncated = false;
    const char *oversize_key = NULL;
    for (size_t i = (summary ? 0 : start); i < total; i++) {
        const struct json_value *val = &body->children[i];
        if (summary && !nc_is_scalar(val)) {
            omitted++;
            continue;
        }
        if (full && request->max_items > 0 && included >= request->max_items) {
            truncated = true;
            next_cursor = i;
            break;
        }
        /* Measure a copy with the candidate member before committing it. */
        struct json_value probe, copy;
        json_init(&probe);
        json_init(&copy);
        json_copy(&probe, &acc);
        json_copy(&copy, val);
        (void)json_push_kv(&probe, body->keys[i], &copy);
        size_t sz = nc_obj_size(&probe);
        json_free(&probe);
        if (sz <= data_budget) {
            (void)json_push_kv(&acc, body->keys[i], &copy);
            included++;
            json_free(&copy);
        } else {
            json_free(&copy);
            truncated = true;
            /* A single field larger than the whole page budget must not stall
             * the cursor: advance past it and name it so the caller can fetch
             * it narrowly (a wider budget, --fields, or the tool directly). */
            if (included == 0) {
                next_cursor = i + 1;
                oversize_key = body->keys[i];
            } else {
                next_cursor = i;
            }
            break;
        }
    }
    if (summary && omitted > 0)
        truncated = true;

    /* Attach the explicit page descriptor. */
    struct json_value page;
    json_init(&page);
    json_set_object(&page);
    (void)json_push_kv_str(&page, "view", view);
    (void)json_push_kv_int(&page, "total_fields", (int64_t)total);
    (void)json_push_kv_int(&page, "included", (int64_t)included);
    (void)json_push_kv_bool(&page, "truncated", truncated);
    if (truncated && !summary)
        (void)json_push_kv_int(&page, "next_cursor", (int64_t)next_cursor);
    if (oversize_key)
        (void)json_push_kv_str(&page, "skipped_oversize", oversize_key);
    (void)json_push_kv(&acc, "_page", &page);
    json_free(&page);

    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &acc);
    json_free(&acc);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;

    if (truncated) {
        char input_json[128];
        if (summary)
            (void)snprintf(input_json, sizeof(input_json), "{\"view\":\"full\"}");
        else
            (void)snprintf(input_json, sizeof(input_json),
                           "{\"view\":\"full\",\"cursor\":%zu}", next_cursor);
        (void)zcl_command_reply_add_next(
            reply, request->spec->path, input_json,
            summary ? "retrieve full fields" : "retrieve the remaining fields");
    }
}

/* Run a bridged leaf with an EXPLICIT body function — everything
 * zcl_native_bridge_command does after resolving the body pointer: build the
 * tool arguments from the request, dispatch (the supplied body function, or —
 * when `body` is NULL and the leaf is a pure 1:1 proxy — the backing JSON-RPC
 * method directly), then project the resulting body into the reply envelope.
 * A hot-swap generation supplies its OWN freshly-compiled body here; the
 * ordinary registry path passes zcl_native_bridge_body_for_path(path). When
 * `body` is NULL and the path also has no direct-RPC binding (i.e. an unknown
 * / unbound path), this fails with the same NO_BRIDGE_BINDING reply the
 * pre-extraction code produced. */
void zcl_native_bridge_run(const struct zcl_command_request *request,
                           zcl_native_body_fn body,
                           struct zcl_command_reply *reply)
{
    if (!request || !request->spec || !reply)
        return;
    const char *tool = zcl_native_bridge_tool_for_path(request->spec->path);
    const char *rpc_method =
        zcl_native_bridge_rpc_for_path(request->spec->path);
    if (!tool || (!body && !rpc_method)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "NO_BRIDGE_BINDING",
                               "dispatch", false, false,
                               "ready leaf has no MCP-free bridge binding",
                               request->spec->path);
        return;
    }

    bridge_ensure_rpc_client();

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

    /* Dispatch WITHOUT the MCP router/middleware: the supplied body function
     * (identical composition to the MCP tool) or the backing RPC directly. */
    struct zcl_native_body_err body_err = { 0 };
    char *result = body ? body(args, &body_err)
                        : mcp_node_rpc(rpc_method, NULL);
    if (use_translated)
        json_free(&translated);

    if (!result) {
        /* Match the legacy surface: a failed dispatch produced an error
         * envelope whose message the bridge surfaced as TOOL_ERROR. */
        char msgbuf[224];
        const char *msg;
        if (body) {
            msg = body_err.message[0] ? body_err.message
                                      : "tool reported an error";
        } else {
            (void)snprintf(msgbuf, sizeof(msgbuf), "RPC %s returned null",
                           rpc_method);
            msg = msgbuf;
        }
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "TOOL_ERROR",
                               "execute", false, false, msg, tool);
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the node is running");
        return;
    }

    struct json_value body_doc;
    if (!json_read(&body_doc, result, strlen(result)) ||
        body_doc.type != JSON_OBJ) {
        json_free(&body_doc);
        free(result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_TOOL_BODY",
                               "serialize", false, false,
                               "tool returned a non-object body", tool);
        return;
    }
    free(result);

    const struct json_value *err = json_get(&body_doc, "error");
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
        json_free(&body_doc);
        return;
    }

    /* Success: project the tool body into the result envelope's data, bounded
     * by view + budget so a large read pages instead of overflowing (§8/§9). */
    zcl_native_bridge_project(request, &body_doc, reply);
    json_free(&body_doc);
}

void zcl_native_bridge_command(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply)
{
    if (!request || !request->spec || !reply)
        return;
    zcl_native_bridge_run(request,
                          zcl_native_bridge_body_for_path(request->spec->path),
                          reply);
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

/* ── ops.state / ops.selftest native leaves (W0 §3) ──────────────────────
 * These two leaves are the native successors of the MCP `zcl_state` and
 * `zcl_self_test` tools. They do NOT enter mcp_middleware / mcp_router:
 *   - ops.state calls the `dumpstate` RPC method directly (the same path
 *     h_zcl_state used, minus the router), so the generic subsystem dump is
 *     reachable natively with the MCP dispatch table uninvoked;
 *   - ops.selftest is a node-free, deterministic well-formedness sweep of the
 *     registry (the native analogue of `zcl_self_test mode=registry`), so the
 *     dev-lane deploy verify can gate on `fail == 0` without a running node. */
void zcl_native_handle_ops_state(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *sub = json_get_str(json_get(request->input, "subsystem"));
    if (!sub || !sub[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_SUBSYSTEM",
                               "normalize", false, false,
                               "subsystem is required", "ops.state");
        (void)zcl_command_reply_add_next(reply, "ops.state",
                                         "{\"subsystem\":\"supervisor\"}",
                                         "name a subsystem to dump");
        return;
    }
    const char *key = json_get_str(json_get(request->input, "key"));

    /* dumpstate params: [subsystem] or [subsystem, key]. Build via JSON so the
     * subsystem/key strings are correctly escaped, never printf-spliced. */
    struct json_value params, item;
    json_init(&params);
    json_set_array(&params);
    json_init(&item);
    json_set_str(&item, sub);
    (void)json_push_back(&params, &item);
    json_free(&item);
    if (key && key[0]) {
        json_init(&item);
        json_set_str(&item, key);
        (void)json_push_back(&params, &item);
        json_free(&item);
    }
    char params_json[512];
    size_t pn = json_write(&params, params_json, sizeof(params_json));
    json_free(&params);
    if (pn == 0 || pn >= sizeof(params_json)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                               "normalize", false, false,
                               "could not encode dumpstate params", sub);
        return;
    }

    bridge_ensure_rpc_client();
    /* Call the RPC layer directly — the MCP router/middleware is never entered
     * (W0: nothing native depends on the MCP dispatch path). */
    char *result = mcp_node_rpc("dumpstate", params_json);
    if (!result) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "the node did not return a state body", sub);
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the node is running");
        return;
    }
    struct json_value body;
    if (!json_read(&body, result, strlen(result)) || body.type != JSON_OBJ) {
        json_free(&body);
        free(result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_STATE_BODY",
                               "serialize", false, false,
                               "dumpstate returned a non-object body", sub);
        return;
    }
    free(result);
    /* mcp_node_rpc surfaces a JSON-RPC failure as either {"error":{...}}
     * (transport) or a bare {"code":..,"message":..} (RPC-level). Treat both
     * as a failed dump — e.g. an unknown subsystem. */
    const struct json_value *err = json_get(&body, "error");
    const struct json_value *ecode = json_get(&body, "code");
    const struct json_value *emsg = json_get(&body, "message");
    if ((err && !json_is_null(err)) ||
        (ecode && ecode->type == JSON_INT && emsg && emsg->type == JSON_STR)) {
        const char *msg = NULL;
        if (err && err->type == JSON_OBJ)
            msg = json_get_str(json_get(err, "message"));
        else if (emsg && emsg->type == JSON_STR)
            msg = json_get_str(emsg);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "STATE_ERROR",
                               "execute", false, false,
                               msg && msg[0] ? msg
                                             : "dumpstate reported an error",
                               sub);
        json_free(&body);
        return;
    }
    /* Success: project the state body into the envelope (view/budget bounded). */
    zcl_native_bridge_project(request, &body, reply);
    json_free(&body);
}

void zcl_native_handle_ops_selftest(const struct zcl_command_request *request,
                                    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const struct zcl_command_registry *reg = catalog();
    size_t total = 0, passed = 0, failed = 0, skipped = 0;

    struct json_value failures;
    json_init(&failures);
    json_set_array(&failures);

    for (size_t i = 0; i < reg->count; i++) {
        const struct zcl_command_spec *s = &reg->commands[i];
        if (s->mode == ZCL_COMMAND_MODE_BRANCH)
            continue;
        total++;
        /* PLANNED / COMPAT leaves are intentionally non-executable — they are
         * skips, never failures (discovery already fails them closed). */
        if (s->availability != ZCL_COMMAND_READY) {
            skipped++;
            continue;
        }
        /* A READY leaf must be dispatchable: a non-NULL handler plus the
         * schema/example/effect-risk guarantees zcl_command_registry_validate
         * enforces. This is a static, node-free contract check. */
        const char *reason = NULL;
        if (!s->handler)
            reason = "ready-leaf-missing-handler";
        else if (!s->input_schema || !s->input_schema[0] ||
                 !s->output_schema || !s->output_schema[0] ||
                 !s->example || !s->example[0])
            reason = "missing-schema-or-example";
        else if (s->effect == ZCL_COMMAND_EFFECT_READ &&
                 s->risk != ZCL_COMMAND_RISK_READ)
            reason = "read-effect-risk-conflict";
        else if (s->handler == zcl_native_bridge_command &&
                 !zcl_native_bridge_tool_for_path(s->path))
            reason = "bridge-leaf-without-binding";

        if (reason) {
            failed++;
            if (failures.num_children < 32) {
                struct json_value f;
                json_init(&f);
                json_set_object(&f);
                (void)json_push_kv_str(&f, "path", s->path);
                (void)json_push_kv_str(&f, "reason", reason);
                (void)json_push_back(&failures, &f);
                json_free(&f);
            }
        } else {
            passed++;
        }
    }

    (void)json_push_kv_str(&reply->data, "mode", "registry");
    (void)json_push_kv_int(&reply->data, "total", (int64_t)total);
    (void)json_push_kv_int(&reply->data, "pass", (int64_t)passed);
    (void)json_push_kv_int(&reply->data, "fail", (int64_t)failed);
    (void)json_push_kv_int(&reply->data, "skip", (int64_t)skipped);
    (void)json_push_kv(&reply->data, "failures", &failures);
    json_free(&failures);

    reply->status = failed == 0 ? ZCL_COMMAND_STATUS_PASSED
                                : ZCL_COMMAND_STATUS_FAILED;
    reply->exit_code = failed == 0 ? ZCL_COMMAND_EXIT_OK
                                   : ZCL_COMMAND_EXIT_FAILED;
}

/* ── core.node.bootstatus / core.node.bootwait native leaves ───────────────
 * Pre-RPC boot observability. Both read <datadir>/boot_status.json directly
 * off disk (util/boot_status.h) — NO node contact, NO RPC — so they answer
 * "what boot stage are we at, is it serving yet?" during the exact window
 * (snapshot load / refold / index rebuild) when RPC has not bound and the only
 * alternative was ss/ps/tail node.log. bootstatus is a single read; bootwait
 * polls until serving or a timeout. */

/* Resolve the target datadir: explicit input.datadir wins, else the CLI's
 * --datadir (g_bridge_datadir). Returns NULL when neither is set. */
static const char *nc_bootstatus_datadir(const struct zcl_command_request *req)
{
    const char *dd = json_get_str(json_get(req->input, "datadir"));
    if (dd && dd[0])
        return dd;
    if (g_bridge_datadir[0])
        return g_bridge_datadir;
    return NULL;
}

/* Project a parsed boot_status snapshot into reply->data. */
static void nc_bootstatus_fill(struct zcl_command_reply *reply,
                               const struct boot_status_snapshot *s)
{
    (void)json_push_kv_str(&reply->data, "phase", s->phase);
    (void)json_push_kv_str(&reply->data, "stage", s->stage);
    (void)json_push_kv_int(&reply->data, "stage_ordinal", s->stage_ordinal);
    (void)json_push_kv_int(&reply->data, "height", s->height);
    (void)json_push_kv_bool(&reply->data, "rpc_bound", s->rpc_bound);
    (void)json_push_kv_bool(&reply->data, "serving", s->serving);
    (void)json_push_kv_int(&reply->data, "started_unix", s->started_unix);
    (void)json_push_kv_int(&reply->data, "updated_unix", s->updated_unix);
    (void)json_push_kv_int(&reply->data, "elapsed_s", s->elapsed_s);
}

void zcl_native_handle_core_node_bootstatus(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = nc_bootstatus_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               "core.node.bootstatus");
        (void)zcl_command_reply_add_next(
            reply, "core.node.bootstatus",
            "{\"datadir\":\"/home/you/.zclassic-c23\"}",
            "name the datadir to inspect");
        return;
    }

    struct boot_status_snapshot snap;
    char why[192];
    if (!boot_status_read(datadir, &snap, why, sizeof(why))) {
        /* No beacon yet: the node has not started booting (or is a build
         * without the writer). Fail closed (exit 3) — never invent a status. */
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "NO_BOOT_STATUS",
                               "execute", true, false,
                               why[0] ? why : "no boot_status.json yet",
                               datadir);
        (void)zcl_command_reply_add_next(reply, "core.node.bootwait",
                                         "{\"datadir\":\"\"}",
                                         "wait for the beacon to appear");
        return;
    }
    (void)json_push_kv_str(&reply->data, "datadir", datadir);
    nc_bootstatus_fill(reply, &snap);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

void zcl_native_handle_core_node_bootwait(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = nc_bootstatus_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               "core.node.bootwait");
        return;
    }

    /* Bounded poll: default 60s budget, 500ms cadence. The validator already
     * range-checks these (timeout_ms 1..300000, heartbeat_ms 100..60000). */
    int64_t timeout_ms = 60000;
    int64_t poll_ms = 500;
    const struct json_value *tmo = json_get(request->input, "timeout_ms");
    if (tmo && tmo->type == JSON_INT)
        timeout_ms = json_get_int(tmo);
    const struct json_value *hb = json_get(request->input, "heartbeat_ms");
    if (hb && hb->type == JSON_INT)
        poll_ms = json_get_int(hb);

    int64_t t0_ms = platform_time_monotonic_ms();
    struct boot_status_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.stage_ordinal = -1;
    snap.height = -1;
    bool ever_seen = false;
    int polls = 0;

    for (;;) {
        char why[192];
        if (boot_status_read(datadir, &snap, why, sizeof(why))) {
            ever_seen = true;
            if (snap.serving) {
                (void)json_push_kv_str(&reply->data, "datadir", datadir);
                (void)json_push_kv_int(&reply->data, "polls", polls);
                nc_bootstatus_fill(reply, &snap);
                reply->status = ZCL_COMMAND_STATUS_PASSED;
                reply->exit_code = ZCL_COMMAND_EXIT_OK;
                return;
            }
        }
        polls++;

        int64_t elapsed_ms = platform_time_monotonic_ms() - t0_ms;
        if (elapsed_ms >= timeout_ms)
            break;

        int64_t remain = timeout_ms - elapsed_ms;
        int64_t sleep_ms = poll_ms < remain ? poll_ms : remain;
        struct timespec ts = { .tv_sec = sleep_ms / 1000,
                               .tv_nsec = (sleep_ms % 1000) * 1000000L };
        (void)nanosleep(&ts, NULL);
    }

    /* Timed out: report the last observed state (transiently unavailable). */
    (void)json_push_kv_str(&reply->data, "datadir", datadir);
    (void)json_push_kv_int(&reply->data, "polls", polls);
    if (ever_seen)
        nc_bootstatus_fill(reply, &snap);
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_TRANSIENT,
        "BOOT_WAIT_TIMEOUT", "execute", true, false,
        ever_seen ? "boot not serving before the timeout"
                  : "no boot_status.json before the timeout",
        datadir);
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

static bool nc_set_typed_value(struct json_value *obj, const char *key,
                               const char *value)
{
    if (!value)
        return json_push_kv_bool(obj, key, true);
    if (strcmp(value, "true") == 0)
        return json_push_kv_bool(obj, key, true);
    if (strcmp(value, "false") == 0)
        return json_push_kv_bool(obj, key, false);
    if (nc_is_integer(value)) {
        errno = 0;
        long long parsed = strtoll(value, NULL, 10);
        if (errno != 0)
            return false;
        return json_push_kv_int(obj, key, (int64_t)parsed);
    }
    return json_push_kv_str(obj, key, value);
}

static bool nc_parse_size_control(const char *value, size_t minimum,
                                  size_t maximum, size_t *out)
{
    if (!value || !nc_is_integer(value) || value[0] == '-' || value[0] == '+')
        return false;
    errno = 0;
    unsigned long long parsed = strtoull(value, NULL, 10);
    if (errno != 0 || parsed < minimum || parsed > maximum)
        return false;
    *out = (size_t)parsed;
    return true;
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
    const char *cursor = NULL;
    size_t budget = 0;
    size_t max_items = 0;
    bool flag_error = false;
    bool seen_input = false, seen_view = false, seen_side = false;
    bool seen_budget = false, seen_max_items = false, seen_cursor = false;
    bool seen_format = false;
    char flag_key[128];
    char flag_why[160] = "malformed or duplicate option";
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
            if (seen_input || !value || !value[0]) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "--input requires one non-empty value and may appear once");
                break;
            }
            seen_input = true;
            input_flag = value;
        } else if (strcmp(flag_key, "view") == 0) {
            if (seen_view || !value ||
                (strcmp(value, "summary") != 0 && strcmp(value, "normal") != 0 &&
                 strcmp(value, "full") != 0)) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "--view must be summary, normal, or full and may appear once");
                break;
            }
            seen_view = true;
            view = value;
        } else if (strcmp(flag_key, "side") == 0) {
            if (seen_side || !value ||
                (strcmp(value, "input") != 0 && strcmp(value, "output") != 0)) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "--side must be input or output and may appear once");
                break;
            }
            seen_side = true;
            side = value;
        } else if (strcmp(flag_key, "budget-bytes") == 0) {
            if (seen_budget ||
                !nc_parse_size_control(value, 512, ZCL_COMMAND_LIST_BUDGET,
                                       &budget)) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "--budget-bytes must be in 512..%u and may appear once",
                               ZCL_COMMAND_LIST_BUDGET);
                break;
            }
            seen_budget = true;
        } else if (strcmp(flag_key, "max-items") == 0) {
            if (seen_max_items ||
                !nc_parse_size_control(value, 1, 100, &max_items)) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "--max-items must be in 1..100 and may appear once");
                break;
            }
            seen_max_items = true;
        } else if (strcmp(flag_key, "cursor") == 0) {
            if (seen_cursor || !value || !value[0] || strlen(value) > 256) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "--cursor requires one value of at most 256 bytes");
                break;
            }
            seen_cursor = true;
            cursor = value;
        } else if (strcmp(flag_key, "format") == 0) {
            if (seen_format || !value || strcmp(value, "json") != 0) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "only one --format=json is implemented for bounded native results");
                break;
            }
            seen_format = true;
        } else if (strcmp(flag_key, "fields") == 0 ||
                   strcmp(flag_key, "quiet") == 0) {
            flag_error = true;
            (void)snprintf(flag_why, sizeof(flag_why),
                           "--%s is not implemented; refusing a silent no-op",
                           flag_key);
            break;
        } else if (!nc_set_typed_value(&flags, flag_key, value)) {
            flag_error = true;
            (void)snprintf(flag_why, sizeof(flag_why),
                           "malformed, duplicate, or out-of-range --%s value",
                           flag_key);
            break;
        }
    }
    if (flag_error) {
        json_free(&flags);
        nc_print_error(spec->path, "BAD_FLAG", "normalize",
                       flag_why, spec->path,
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

    /* The frozen grammar permits paging/view controls inside --input as well
     * as top-level flags. Normalize both spellings to the one request object,
     * and reject ambiguous double specification. */
    char input_cursor[64];
    const struct json_value *input_view = json_get(&input, "view");
    if (input_view) {
        if (seen_view) {
            json_free(&input);
            nc_print_error(spec->path, "DUPLICATE_CONTROL", "normalize",
                           "view was supplied both inside --input and as a flag",
                           "view", "discover.schema", "{}",
                           "supply each response control once");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        view = json_get_str(input_view);
    }
    const struct json_value *input_max_items = json_get(&input, "max_items");
    if (input_max_items) {
        if (seen_max_items) {
            json_free(&input);
            nc_print_error(spec->path, "DUPLICATE_CONTROL", "normalize",
                           "max_items was supplied both inside --input and as a flag",
                           "max_items", "discover.schema", "{}",
                           "supply each response control once");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        max_items = (size_t)json_get_int(input_max_items);
    }
    const struct json_value *input_cursor_value = json_get(&input, "cursor");
    if (input_cursor_value) {
        if (seen_cursor) {
            json_free(&input);
            nc_print_error(spec->path, "DUPLICATE_CONTROL", "normalize",
                           "cursor was supplied both inside --input and as a flag",
                           "cursor", "discover.schema", "{}",
                           "supply each response control once");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        if (input_cursor_value->type == JSON_STR) {
            cursor = json_get_str(input_cursor_value);
        } else {
            (void)snprintf(input_cursor, sizeof(input_cursor), "%lld",
                           (long long)json_get_int(input_cursor_value));
            cursor = input_cursor;
        }
    }

    const char *operator_lane = getenv("ZCL_OPERATOR_LANE");
#ifdef ZCL_DEV_BUILD
    /* The development executable is itself the confined dev-lane authority:
     * its mutating handlers target only ~/.zclassic-c23-dev and are omitted
     * from release builds.  Requiring callers to repeat
     * ZCL_OPERATOR_LANE=dev made the documented one-command edit loop deny
     * itself as lane "unknown".  An explicit environment value still wins,
     * so setting canonical/soak continues to fail closed. */
    if (!operator_lane || !operator_lane[0])
        operator_lane = "dev";
#endif
    struct zcl_command_context ctx = {
        .registry = reg,
        .source_root = getenv("ZCL_DEV_SOURCE_ROOT"),
        .operator_lane = operator_lane,
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
        reg, spec, &ctx, &input, was_alias, invoked, view, budget, max_items,
        cursor, out, sizeof(out), &exit_code);
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
