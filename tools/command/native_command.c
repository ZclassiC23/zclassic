/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native command adapter (contract §3, §8). Normalizes argv into a
 * registry lookup: longest-registered-path resolution, one JSON input object
 * from --input='<obj>' / --input=- / typed flags, and exactly one bounded JSON
 * document out. Unknown keys and out-of-range values are rejected before any
 * side effect; an unknown branch fails with nearby valid paths plus one
 * executable next action and NEVER becomes an arbitrary RPC method.
 *
 * READ-ONLY Core/Ops leaves execute through zcl_native_bridge_command: a leaf
 * either calls its transport-neutral body function (app/controllers/
 * *_native_handlers.c) or, for a pure 1:1 proxy, calls the backing JSON-RPC
 * method directly. Discovery
 * leaves (help/search/describe/schema) render the native discovery document
 * directly. The `dev` subtree uses this same resolver; its process and watcher
 * handlers are injected only in a ZCL_DEV_BUILD catalog.
 */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "config/command_catalog.h"
#include "framework/app_definition.h"
#include "kernel/command_registry.h"
#include "json/json.h"

#include "chain/chainparams.h"
#include "chain/checkpoints.h"
#include "platform/time_compat.h"
#include "util/safe_alloc.h"
#include "util/boot_status.h"
#include "controllers/native_handler_body.h"
#include "controllers/status_native_helpers.h"
#include "controllers/status_native_handlers.h"
#include "controllers/chain_native_handlers.h"
#include "controllers/wallet_native_handlers.h"
#include "controllers/diagnostics_native_handlers.h"
#include "controllers/net_native_handlers.h"
#include "controllers/app_native_handlers.h"
#include "controllers/meta_native_handlers.h"
#include "controllers/ops_native_handlers.h"
#include "controllers/explain_native_handlers.h"
#include "config/consensus_state_producer_receipt.h"
#include "command/rom_compile_render.h"
#include "command/rom_compile_offline.h"
#include "command/rom_watch_loop.h"
#include "controllers/rpc_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── root recognition ──────────────────────────────────────────────── */
/* Canonical roots this adapter owns. `status` is the compact native entry
 * point; the large diagnostic document remains explicit under core.status. */
bool zcl_native_command_is_root(const char *word)
{
    if (!word || !word[0])
        return false;
    static const char *const roots[] = {
        "status", "core", "app", "dev", "ops", "discover", "code", "help",
        "search",
        /* Operator-UX convenience roots: bare aliases of ops.explain /
         * ops.profile so `zclassic23 explain sync` / `zclassic23 profile`
         * work without the `ops` prefix (each leaf carries the matching
         * alias in config/commands/ops.def). */
        "explain", "profile",
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (strcmp(word, roots[i]) == 0)
            return true;
    }
    return false;
}

/* ── dispatch bindings for the bridge ─────────────────────────────────
 * Every bridged leaf resolves to exactly ONE of:
 *   - a transport-neutral body function, or
 *   - a direct JSON-RPC method for a pure pass-through leaf.
 * The golden catalog test proves the union covers every bridged leaf exactly. */
static const struct {
    const char *path;
    zcl_native_body_fn body;
} g_bridge_native_body[] = {
    { "status", zcl_native_status_brief_body },
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
    { "ops.debug.dash.kpi", zcl_native_kpi_body },
    { "ops.debug.dash.snapshot", zcl_native_operator_snapshot_body },
    { "ops.debug.dash.summary", zcl_native_operator_summary_body },
    { "ops.debug.dash.milestone", zcl_native_milestone_body },
    { "ops.debug.dash.mirror", zcl_native_mirror_status_body },
    { "ops.debug.dash.selfheal", zcl_native_self_heal_stats_body },
    /* app features (ZCL app controller port — read surface) */
    { "app.names.resolve", zcl_native_name_resolve_body },
    { "app.names.list", zcl_native_name_list_body },
    { "app.tokens.list", zcl_native_zslp_listtokens_body },
    { "app.messaging.inbox", zcl_native_msg_inbox_body },
    { "app.market.list", zcl_native_zmarket_list_body },
    { "app.market.status", zcl_native_zmarket_status_body },
    { "app.swap.chains", zcl_native_swap_chains_body },
    { "app.swap.list", zcl_native_swap_list_body },
};

enum bridge_rpc_array_kind {
    BRIDGE_RPC_ARRAY_NONE = 0,
    BRIDGE_RPC_ARRAY_TXIDS,
    BRIDGE_RPC_ARRAY_PEERS,
    BRIDGE_RPC_ARRAY_LATENCY,
};

struct bridge_rpc_required_field {
    const char *name;
    enum json_type type;
};

struct bridge_rpc_binding {
    const char *path;
    const char *rpc_method;
    enum json_type top_type;
    struct bridge_rpc_required_field required[3];
    enum bridge_rpc_array_kind array_kind;
};

static const struct bridge_rpc_binding g_bridge_rpc_direct[] = {
    { "core.chain.tip", "getchaintip", JSON_OBJ,
      {{"hash", JSON_STR}, {"height", JSON_INT}}, BRIDGE_RPC_ARRAY_NONE },
    { "core.chain.mempool.status", "getmempoolinfo", JSON_OBJ,
      {{"size", JSON_INT}, {"bytes", JSON_INT}}, BRIDGE_RPC_ARRAY_NONE },
    { "core.chain.mempool.list", "getrawmempool", JSON_ARR,
      {{0}}, BRIDGE_RPC_ARRAY_TXIDS },
    { "core.sync.status", "syncstate", JSON_OBJ,
      {{"state", JSON_STR}, {"state_id", JSON_INT}}, BRIDGE_RPC_ARRAY_NONE },
    { "core.sync.validation", "validationstatus", JSON_OBJ,
      {{"state", JSON_STR}}, BRIDGE_RPC_ARRAY_NONE },
    { "core.consensus.integrity", "getdataintegrity", JSON_OBJ,
      {{"source", JSON_STR}, {"master", JSON_STR}}, BRIDGE_RPC_ARRAY_NONE },
    { "core.consensus.utxo.commitment", "getutxocommitment", JSON_OBJ,
      {{"sha3_hash", JSON_STR}, {"height", JSON_INT},
       {"utxo_count", JSON_INT}}, BRIDGE_RPC_ARRAY_NONE },
    { "core.consensus.mmb", "getmmrroot", JSON_OBJ,
      {{"mmr_root", JSON_STR}, {"num_leaves", JSON_INT}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.network.status", "getnetworkinfo", JSON_OBJ,
      {{"connections", JSON_INT}, {"networks", JSON_ARR}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.network.peers.list", "getpeerinfo", JSON_ARR,
      {{0}}, BRIDGE_RPC_ARRAY_PEERS },
    { "core.network.peers.latency", "getpeerlatency", JSON_ARR,
      {{0}}, BRIDGE_RPC_ARRAY_LATENCY },
    { "core.network.onion.status", "healthcheck", JSON_OBJ,
      {{"status", JSON_STR}, {"healthy", JSON_BOOL}, {"serving", JSON_BOOL}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.wallet.status", "getwalletinfo", JSON_OBJ,
      {{"balance", JSON_STR}, {"txcount", JSON_INT}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.wallet.balance", "z_gettotalbalance", JSON_OBJ,
      {{"transparent", JSON_STR}, {"total", JSON_STR}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.wallet.backup.status", "walletbackupstatus", JSON_OBJ,
      {{"running", JSON_BOOL}, {"total_runs", JSON_INT}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.wallet.audit", "walletaudit", JSON_OBJ,
      {{"chain_height", JSON_INT}, {"summary", JSON_OBJ}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.storage.stats", "db_info", JSON_OBJ,
      {{"tip_height", JSON_INT}, {"utxo_count", JSON_INT}},
      BRIDGE_RPC_ARRAY_NONE },
    { "core.mining.status", "getmininginfo", JSON_OBJ,
      {{"blocks", JSON_INT}, {"chain", JSON_STR}}, BRIDGE_RPC_ARRAY_NONE },
    { "core.mining.benchmark", "benchmark", JSON_OBJ,
      {{"primary_benchmark_source", JSON_STR},
       {"primary_benchmarks", JSON_ARR}}, BRIDGE_RPC_ARRAY_NONE },
    { "ops.health", "healthcheck", JSON_OBJ,
      {{"status", JSON_STR}, {"healthy", JSON_BOOL}, {"serving", JSON_BOOL}},
      BRIDGE_RPC_ARRAY_NONE },
    { "ops.lanes", "agentlanes", JSON_OBJ,
      {{"status", JSON_STR}, {"lanes", JSON_ARR}}, BRIDGE_RPC_ARRAY_NONE },
    { "ops.recovery.status", "refold", JSON_OBJ,
      {{"ready_for_refold", JSON_BOOL}, {"primary_blocker", JSON_STR}},
      BRIDGE_RPC_ARRAY_NONE },
};

static const struct bridge_rpc_binding *bridge_rpc_binding_for_path(
    const char *path)
{
    if (!path)
        return NULL;
    for (size_t i = 0;
         i < sizeof(g_bridge_rpc_direct) / sizeof(g_bridge_rpc_direct[0]);
         i++) {
        if (strcmp(g_bridge_rpc_direct[i].path, path) == 0)
            return &g_bridge_rpc_direct[i];
    }
    return NULL;
}

static const char *bridge_json_type_name(enum json_type type)
{
    static const char *const names[] = {
        "null", "bool", "int", "real", "string", "array", "object",
    };
    return (unsigned)type < sizeof(names) / sizeof(names[0])
               ? names[type]
               : "unknown";
}

static bool bridge_is_hex64(const char *s)
{
    if (!s || strlen(s) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isxdigit(c))
            return false;
    }
    return true;
}

static bool bridge_validate_array_item(
    const struct bridge_rpc_binding *binding,
    const struct json_value *item, size_t index,
    char *why, size_t why_cap)
{
    const char *field_a = NULL;
    const char *field_b = NULL;
    switch (binding->array_kind) {
    case BRIDGE_RPC_ARRAY_TXIDS:
        if (item->type == JSON_STR && bridge_is_hex64(json_get_str(item)))
            return true;
        (void)snprintf(why, why_cap,
                       "item %zu must be a 64-hex transaction id", index);
        return false;
    case BRIDGE_RPC_ARRAY_PEERS:
        field_a = "id";
        field_b = "addr";
        break;
    case BRIDGE_RPC_ARRAY_LATENCY:
        field_a = "peer_id";
        field_b = "addr";
        break;
    case BRIDGE_RPC_ARRAY_NONE:
        return true;
    }
    if (item->type != JSON_OBJ) {
        (void)snprintf(why, why_cap, "item %zu must be an object", index);
        return false;
    }
    const struct json_value *a = json_get(item, field_a);
    const struct json_value *b = json_get(item, field_b);
    if (!a || a->type != JSON_INT) {
        (void)snprintf(why, why_cap, "item %zu field %s must be int",
                       index, field_a);
        return false;
    }
    if (!b || b->type != JSON_STR) {
        (void)snprintf(why, why_cap, "item %zu field %s must be string",
                       index, field_b);
        return false;
    }
    return true;
}

/* A bare, parseable JSON value is not proof that the requested RPC exists or
 * that the running node speaks this source epoch's contract. Direct-RPC
 * leaves therefore validate the stable minimum of the legacy result shape.
 * These checks intentionally do not require a synthetic `schema` member:
 * zclassicd-compatible RPCs predate schema labels, but their field/type shape
 * is stable. Empty list results remain valid; every populated element is
 * checked so a mixed or arbitrary array fails closed. */
static bool bridge_validate_rpc_success(
    const struct bridge_rpc_binding *binding,
    const struct json_value *doc, char *why, size_t why_cap)
{
    if (!binding || !doc) {
        (void)snprintf(why, why_cap, "missing direct-RPC contract");
        return false;
    }
    if (doc->type != binding->top_type) {
        (void)snprintf(why, why_cap, "top level must be %s (got %s)",
                       bridge_json_type_name(binding->top_type),
                       bridge_json_type_name(doc->type));
        return false;
    }
    for (size_t i = 0;
         i < sizeof(binding->required) / sizeof(binding->required[0]); i++) {
        const struct bridge_rpc_required_field *required =
            &binding->required[i];
        if (!required->name)
            break;
        const struct json_value *field = json_get(doc, required->name);
        if (!field || field->type != required->type) {
            (void)snprintf(why, why_cap, "field %s must be %s",
                           required->name,
                           bridge_json_type_name(required->type));
            return false;
        }
    }
    if (binding->top_type == JSON_ARR) {
        for (size_t i = 0; i < doc->num_children; i++) {
            if (!bridge_validate_array_item(binding, &doc->children[i], i,
                                            why, why_cap))
                return false;
        }
    }
    return true;
}

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
    const struct bridge_rpc_binding *binding =
        bridge_rpc_binding_for_path(path);
    return binding ? binding->rpc_method : NULL;
}

static bool bridge_has_exact_binding(const char *path)
{
    return (zcl_native_bridge_body_for_path(path) != NULL) !=
           (zcl_native_bridge_rpc_for_path(path) != NULL);
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
     * body function that consults them. */
    node_rpc_client_init(g_bridge_datadir, g_bridge_rpc_port);
    chain_params_select(CHAIN_MAIN);
    g_bridge_rpc_ready = true;
}

/* Translate the CLI leaf input into the exact argument object its handler
 * expects. Most leaves are pass-through; a few need a rename. */
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
 * A bridged command body can exceed the 4096-byte ordinary-result budget. Rather
 * than fail with RESPONSE_BUDGET_EXCEEDED, project the top-level object to fit:
 *   summary — scalar top-level fields only (containers dropped);
 *   normal  — greedy top-level fields in order until the budget (default);
 *   full    — greedy from --cursor, honoring --max-items, paging via a cursor.
 * Truncation is always explicit: a `_page` object records the advancing
 * cursor, while `next` points at the leaf contract instead of creating an
 * executable self-loop. */
enum { NC_ENVELOPE_RESERVE = 768 };

static void nc_add_describe_next(struct zcl_command_reply *reply,
                                 const char *path, const char *reason)
{
    if (!reply || !path || !path[0])
        return;
    char input[ZCL_COMMAND_MAX_PATH + 16];
    int n = snprintf(input, sizeof(input), "{\"path\":\"%s\"}", path);
    if (n > 0 && (size_t)n < sizeof(input))
        (void)zcl_command_reply_add_next(reply, "discover.describe", input,
                                         reason);
}

static void nc_add_string_next(struct zcl_command_reply *reply,
                               const char *command, const char *key,
                               const char *value, const char *reason)
{
    if (!reply || !command || !key || !value)
        return;
    struct json_value input;
    json_init(&input);
    json_set_object(&input);
    char encoded[sizeof(reply->next[0].input_json)];
    bool ok = json_push_kv_str(&input, key, value);
    size_t n = ok ? json_write(&input, encoded, sizeof(encoded)) : 0;
    json_free(&input);
    if (n > 0 && n < sizeof(encoded))
        (void)zcl_command_reply_add_next(reply, command, encoded, reason);
}

static bool nc_is_scalar(const struct json_value *v)
{
    return v && v->type <= JSON_STR; /* NULL/BOOL/INT/REAL/STR */
}

static size_t nc_json_size(const struct json_value *value)
{
    char scratch[ZCL_COMMAND_LIST_BUDGET + 1];
    size_t n = json_write(value, scratch, sizeof(scratch));
    return (n == 0 || n >= sizeof(scratch)) ? sizeof(scratch) : n;
}

static void nc_project_array(const struct zcl_command_request *request,
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
    /* Reserve room inside data for the stable page descriptor. */
    size_t items_budget = data_budget > 256 ? data_budget - 256
                                             : data_budget / 2;

    size_t start = 0;
    if (full && request->cursor && request->cursor[0]) {
        char *end = NULL;
        unsigned long long c = strtoull(request->cursor, &end, 10);
        if (end && !*end)
            start = (size_t)c;
    }

    struct json_value items;
    json_init(&items);
    json_set_array(&items);
    size_t included = 0;
    size_t next_cursor = body->num_children;
    bool truncated = summary && body->num_children > 0;
    bool skipped_oversize = false;
    size_t skipped_index = 0;
    if (summary)
        next_cursor = 0;

    for (size_t i = start; !summary && i < body->num_children; i++) {
        if (full && request->max_items > 0 && included >= request->max_items) {
            truncated = true;
            next_cursor = i;
            break;
        }
        struct json_value probe, copy;
        json_init(&probe);
        json_init(&copy);
        json_copy(&probe, &items);
        json_copy(&copy, &body->children[i]);
        (void)json_push_back(&probe, &copy);
        size_t sz = nc_json_size(&probe);
        json_free(&probe);
        if (sz <= items_budget) {
            (void)json_push_back(&items, &copy);
            included++;
            json_free(&copy);
            continue;
        }
        json_free(&copy);
        truncated = true;
        if (included == 0) {
            skipped_oversize = true;
            skipped_index = i;
            next_cursor = i + 1;
        } else {
            next_cursor = i;
        }
        break;
    }

    struct json_value page, data;
    json_init(&page);
    json_init(&data);
    json_set_object(&page);
    json_set_object(&data);
    (void)json_push_kv_str(&page, "view", view);
    (void)json_push_kv_int(&page, "total_items",
                           (int64_t)body->num_children);
    (void)json_push_kv_int(&page, "included", (int64_t)included);
    (void)json_push_kv_bool(&page, "truncated", truncated);
    if (truncated)
        (void)json_push_kv_int(&page, "next_cursor", (int64_t)next_cursor);
    if (skipped_oversize)
        (void)json_push_kv_int(&page, "skipped_oversize_index",
                               (int64_t)skipped_index);
    (void)json_push_kv(&data, "items", &items);
    (void)json_push_kv(&data, "_page", &page);
    json_free(&items);
    json_free(&page);

    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &data);
    json_free(&data);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;

    if (truncated)
        nc_add_describe_next(
            reply, request->spec->path,
            summary ? "inspect paging controls before retrieving list items"
                    : "inspect paging controls before continuing this list");
}

void zcl_native_bridge_project(const struct zcl_command_request *request,
                               const struct json_value *body,
                               struct zcl_command_reply *reply)
{
    if (body && body->type == JSON_ARR) {
        nc_project_array(request, body, reply);
        return;
    }
    if (!body || body->type != JSON_OBJ) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_TOOL_BODY",
                               "serialize", false, false,
                               "command returned an unsupported body shape",
                               request && request->spec
                                   ? request->spec->path : "");
        return;
    }
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
        size_t sz = nc_json_size(&probe);
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
             * it narrowly (a wider budget, --fields, or the command directly). */
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

    if (truncated)
        nc_add_describe_next(
            reply, request->spec->path,
            summary ? "inspect paging controls before retrieving full fields"
                    : "inspect paging controls before continuing these fields");
}

/* Run a bridged leaf with an EXPLICIT body function — everything
 * zcl_native_bridge_command does after resolving the body pointer: build the
 * command arguments from the request, dispatch (the supplied body function, or —
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
    zcl_native_body_fn resident_body =
        zcl_native_bridge_body_for_path(request->spec->path);
    const struct bridge_rpc_binding *rpc_binding =
        bridge_rpc_binding_for_path(request->spec->path);
    const char *rpc_method = rpc_binding ? rpc_binding->rpc_method : NULL;
    bool valid_binding = body ? (resident_body != NULL && rpc_method == NULL)
                              : (resident_body == NULL && rpc_method != NULL);
    if (!valid_binding) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "NO_BRIDGE_BINDING",
                               "dispatch", false, false,
                               body && rpc_method
                                   ? "ready leaf has ambiguous dispatch bindings"
                                   : "ready leaf has no dispatch binding",
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

    /* Dispatch through the supplied body function or the backing RPC. */
    struct zcl_native_body_err body_err = { 0 };
    char *result = body ? body(args, &body_err)
                        : node_rpc_call(rpc_method, NULL);
    if (use_translated)
        json_free(&translated);

    if (!result) {
        char msgbuf[224];
        const char *msg;
        if (body) {
            msg = body_err.message[0] ? body_err.message
                                      : "command handler reported an error";
        } else {
            (void)snprintf(msgbuf, sizeof(msgbuf), "RPC %s returned null",
                           rpc_method);
            msg = msgbuf;
        }
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "TOOL_ERROR",
                               "execute", false, false, msg,
                               request->spec->path);
        nc_add_describe_next(reply, request->spec->path,
                             "inspect this command before retrying");
        return;
    }

    struct json_value body_doc;
    if (!json_read(&body_doc, result, strlen(result))) {
        json_free(&body_doc);
        free(result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_TOOL_BODY",
                               "serialize", false, false,
                               "command returned an invalid JSON body",
                               request->spec->path);
        return;
    }
    free(result);

    const struct json_value *err = body_doc.type == JSON_OBJ
                                       ? json_get(&body_doc, "error") : NULL;
    if (body_doc.type == JSON_OBJ && status_json_is_rpc_error(&body_doc)) {
        const char *msg = NULL;
        if (err && err->type == JSON_OBJ)
            msg = json_get_str(json_get(err, "message"));
        else if (err && err->type == JSON_STR)
            msg = json_get_str(err);
        else
            msg = json_get_str(json_get(&body_doc, "message"));
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "TOOL_ERROR",
                               "execute", false, false,
                               msg && msg[0] ? msg : "command reported an error",
                               request->spec->path);
        json_free(&body_doc);
        return;
    }

    if (body && body_doc.type != JSON_OBJ) {
        json_free(&body_doc);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_TOOL_BODY",
                               "serialize", false, false,
                               "command returned a non-object body",
                               request->spec->path);
        return;
    }
    if (!body) {
        char why[160];
        if (!bridge_validate_rpc_success(rpc_binding, &body_doc,
                                         why, sizeof(why))) {
            char msg[224];
            (void)snprintf(msg, sizeof(msg),
                           "RPC %s returned an incompatible success body: %s",
                           rpc_method ? rpc_method : "(unbound)", why);
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_FAILED, "TOOL_ERROR",
                                   "execute", false, false, msg,
                                   request->spec->path);
            json_free(&body_doc);
            return;
        }
    }

    /* Success: project the command body into the result envelope's data, bounded
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
    /* Compile-time catalog only. Definitions are data and grant no runtime
     * authority; checkout inspection is a separate dev command. */
    struct json_value apps;
    json_init(&apps);
    json_set_array(&apps);
    size_t count = zcl_app_definition_builtin_count_v1();
    for (size_t i = 0; i < count; i++) {
        const char *app_id = zcl_app_definition_builtin_id_v1(i);
        if (!app_id)
            continue;
        struct json_value item;
        json_init(&item);
        json_set_str(&item, app_id);
        (void)json_push_back(&apps, &item);
        json_free(&item);
    }
    (void)json_push_kv(&reply->data, "apps", &apps);
    (void)json_push_kv_int(&reply->data, "count", (int64_t)count);
    (void)json_push_kv_str(&reply->data, "catalog", "built-in-strict-v1");
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
    if (!zcl_app_definition_builtin_v1(app_id)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "UNKNOWN_APP",
                               "resolve", false, false,
                               "no such installed App", app_id);
        (void)zcl_command_reply_add_next(reply, "app.list", "{}",
                                         "list installed Apps");
        return;
    }
    char manifest[ZCL_APP_ID_MAX + sizeof("apps//app.def")];
    int n = snprintf(manifest, sizeof(manifest), "apps/%s/app.def", app_id);
    if (n <= 0 || (size_t)n >= sizeof(manifest)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "APP_PATH_OVERFLOW",
                               "render", false, false,
                               "built-in App path exceeds its bound", app_id);
        return;
    }
    (void)json_push_kv_str(&reply->data, "app_id", app_id);
    (void)json_push_kv_str(&reply->data, "manifest", manifest);
    (void)json_push_kv_str(&reply->data, "status", "checkout-only");
    (void)json_push_kv_str(&reply->data, "authority", "definition-only");
}

/* ── ops.state / ops.selftest native leaves ──────────────────────────────
 * ops.state calls the `dumpstate` RPC method directly, while ops.selftest is
 * a node-free, deterministic well-formedness sweep of the registry. */
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
        nc_add_describe_next(reply, request->spec->path,
                             "inspect the subsystem input contract");
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
    /* Call the RPC layer directly. */
    char *result = node_rpc_call("dumpstate", params_json);
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
    /* node_rpc_call surfaces a JSON-RPC failure as either {"error":{...}}
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

void zcl_native_handle_network_chain_view(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;

    /* The reachable-network chain view lives in the running node's
     * network_monitor subsystem; surface it through the same SELECT-only
     * dumpstate RPC that ops.state uses, pinned to that subsystem. */
    bridge_ensure_rpc_client();
    char *result = node_rpc_call("dumpstate", "[\"network_monitor\"]");
    if (!result) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "the node did not return the network view",
                               "network_monitor");
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
                               "network view returned a non-object body",
                               "network_monitor");
        return;
    }
    free(result);
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
                                             : "network view reported an error",
                               "network_monitor");
        json_free(&body);
        return;
    }
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
        else if (!s->semantics || !s->semantics[0])
            reason = "missing-semantics";
        else if (strcmp(s->semantics, s->summary) == 0)
            reason = "semantics-equals-summary";
        else if (s->budget_bytes != 0 &&
                 (s->budget_bytes < 256 || s->budget_bytes > 65536))
            reason = "budget-out-of-range";
        else if (s->handler == zcl_native_bridge_command &&
                 !bridge_has_exact_binding(s->path))
            reason = "bridge-leaf-without-exact-binding";

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

/* ── ops.debug.backtrace native leaf ───────────────────────────────────────
 * Dispatches the `selfbacktrace` RPC method directly so the running node dumps a
 * backtrace for every thread and returns the log path + thread_count. This is
 * the typed answer to "what is every thread doing right now" on hosts where
 * perf_event_paranoid / yama ptrace_scope block perf and gdb attach. */
void zcl_native_handle_ops_debug_backtrace(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;

    bridge_ensure_rpc_client();
    char *result = node_rpc_call("selfbacktrace", "[]");
    if (!result) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "the node did not return a backtrace body",
                               "ops.debug.backtrace");
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the node is running");
        return;
    }
    struct json_value body;
    if (!json_read(&body, result, strlen(result)) || body.type != JSON_OBJ) {
        json_free(&body);
        free(result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_BACKTRACE_BODY",
                               "serialize", false, false,
                               "selfbacktrace returned a non-object body",
                               "ops.debug.backtrace");
        return;
    }
    free(result);

    const struct json_value *err = json_get(&body, "error");
    if (err && err->type == JSON_STR) {
        const char *msg = json_get_str(err);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "BACKTRACE_ERROR",
                               "execute", false, false,
                               msg && msg[0] ? msg
                                             : "self-backtrace dump failed",
                               "ops.debug.backtrace");
        json_free(&body);
        return;
    }

    const char *path = json_get_str(json_get(&body, "path"));
    const struct json_value *tc = json_get(&body, "thread_count");
    (void)json_push_kv_str(&reply->data, "path", path ? path : "");
    (void)json_push_kv_int(&reply->data, "thread_count",
                           tc ? json_get_int(tc) : 0);
    json_free(&body);
}

/* ── ops.explain <topic> native leaf ───────────────────────────────────────
 * Composes, IN C, what an operator otherwise stitches together from four
 * surfaces (reducer frontier, blocker registry, condition engine, health/sync
 * RPCs). explain_build fetches the shared RPC bundle and dispatches through the
 * topic table; the reply carries a prose `text` block + the structured fields.
 * The CLI renders `text` verbatim unless --format=json (see nc_render_prose). */
void zcl_native_handle_ops_explain(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *topic = json_get_str(json_get(request->input, "topic"));
    if (!topic || !topic[0])
        topic = "sync";

    bridge_ensure_rpc_client();
    struct json_value data;
    json_init(&data);
    bool ok = explain_build(topic, &data);
    if (!ok) {
        const char *emsg = json_get_str(json_get(&data, "error"));
        char known[128];
        explain_topics_csv(known, sizeof(known));
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "UNKNOWN_TOPIC",
                               "resolve", false, false,
                               emsg && emsg[0] ? emsg : "unknown explain topic",
                               known);
        nc_add_describe_next(reply, request->spec->path,
                             "inspect the supported explain topics");
        json_free(&data);
        return;
    }
    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &data);
    json_free(&data);
}

/* ── ops.profile native leaf ────────────────────────────────────────────────
 * Dispatches the `profile` RPC (samples this node's /proc/self/task twice
 * `seconds` apart, in-process) and renders a prose top-N thread table + verdict
 * + reducer stage step-EWMA. This replaces the /proc sampling an operator does
 * by hand to find a bottleneck. */
void zcl_native_handle_ops_profile(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    int64_t seconds = json_get_int(json_get(request->input, "seconds"));
    if (seconds < 1) seconds = 3;
    if (seconds > 60) seconds = 60;
    int64_t top_n = json_get_int(json_get(request->input, "top_n"));
    if (top_n < 1) top_n = 8;
    if (top_n > 32) top_n = 32;

    char params[64];
    (void)snprintf(params, sizeof(params), "[%lld,%lld]",
                   (long long)seconds, (long long)top_n);

    bridge_ensure_rpc_client();
    char *result = node_rpc_call("profile", params);
    struct json_value body;
    if (!result || !json_read(&body, result, strlen(result)) ||
        body.type != JSON_OBJ) {
        if (result) json_free(&body);
        free(result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "the node did not return a profile body",
                               "ops.debug.profile");
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the node is running");
        return;
    }
    free(result);

    const struct json_value *err = json_get(&body, "error");
    if (err && err->type == JSON_STR) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "PROFILE_ERROR",
                               "execute", false, false,
                               json_get_str(err), "ops.debug.profile");
        json_free(&body);
        return;
    }

    /* Render a prose block from the structured profile body. */
    char t[1600];
    size_t len = 0;
    int n;
    n = snprintf(t + len, sizeof(t) - len, "profile — %s\n",
                 json_get_str(json_get(&body, "verdict"))
                     ? json_get_str(json_get(&body, "verdict")) : "unknown");
    if (n > 0) len += (size_t)n;
    n = snprintf(t + len, sizeof(t) - len,
                 "  sampled %lld threads over %lld ms\n",
                 (long long)json_get_int(json_get(&body, "sampled_threads")),
                 (long long)json_get_int(json_get(&body, "sample_ms")));
    if (n > 0) len += (size_t)n;

    const struct json_value *threads = json_get(&body, "threads");
    if (threads && threads->type == JSON_ARR) {
        n = snprintf(t + len, sizeof(t) - len,
                     "  busiest threads (cpu_ms / wchan):\n");
        if (n > 0) len += (size_t)n;
        for (size_t i = 0; i < threads->num_children && len < sizeof(t) - 128;
             i++) {
            const struct json_value *th = &threads->children[i];
            n = snprintf(t + len, sizeof(t) - len,
                         "    %-16s tid=%lld cpu=%lldms (%lld%%) wchan=%s\n",
                         json_get_str(json_get(th, "name"))
                             ? json_get_str(json_get(th, "name")) : "?",
                         (long long)json_get_int(json_get(th, "tid")),
                         (long long)json_get_int(json_get(th, "cpu_ms")),
                         (long long)json_get_int(json_get(th, "cpu_pct")),
                         json_get_str(json_get(th, "wchan"))
                             ? json_get_str(json_get(th, "wchan")) : "-");
            if (n > 0) len += (size_t)n;
        }
    }
    const struct json_value *stages = json_get(&body, "stage_ewma");
    if (stages && stages->type == JSON_ARR && len < sizeof(t) - 256) {
        n = snprintf(t + len, sizeof(t) - len,
                     "  reducer stage rates (steps/sec, cursor):\n");
        if (n > 0) len += (size_t)n;
        for (size_t i = 0; i < stages->num_children && len < sizeof(t) - 96;
             i++) {
            const struct json_value *sg = &stages->children[i];
            n = snprintf(t + len, sizeof(t) - len, "    %-16s %lld  (%lld)\n",
                         json_get_str(json_get(sg, "stage"))
                             ? json_get_str(json_get(sg, "stage")) : "?",
                         (long long)json_get_int(json_get(sg, "steps_per_sec")),
                         (long long)json_get_int(json_get(sg, "cursor")));
            if (n > 0) len += (size_t)n;
        }
    }

    json_free(&reply->data);
    json_init(&reply->data);
    json_copy(&reply->data, &body);
    (void)json_push_kv_str(&reply->data, "text", t);
    json_free(&body);
}

/* ── ops.producer.status native leaf (node-free) ────────────────────────────
 * Reads another datadir's producer progress.kv (stage cursors + session/receipt
 * lifecycle) and the mint-progress.log tail, with NO node contact, for an
 * operator watching a mint/anchor producer. Read-only. Takes datadir=. */
static void nc_read_log_tail(const char *datadir, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    char path[CONSENSUS_STATE_PRODUCER_DATADIR_MAX +
              sizeof("/mint-progress.log")];
    int path_len = snprintf(path, sizeof(path), "%s/mint-progress.log",
                            datadir);
    if (path_len < 0 || (size_t)path_len >= sizeof(path))
        return;
    FILE *f = fopen(path, "re");
    if (!f)
        return;
    /* Read the last <=4KB, keep the last non-empty line. */
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        long off = sz > 4096 ? sz - 4096 : 0;
        if (off > 0) (void)fseek(f, off, SEEK_SET);
    }
    char buf[4200];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r'))
        buf[--got] = '\0';
    char *nl = strrchr(buf, '\n');
    const char *line = nl ? nl + 1 : buf;
    (void)snprintf(out, cap, "%s", line);
}

/* A digest-verified finalized receipt is the immutable completion authority;
 * live stage cursors may belong to a later/restarted writer and must not
 * override its H*+1 fold cursor. */
static int64_t nc_producer_applied_height(
    const struct producer_status_read *st)
{
    if (!st)
        return -1;
    if (st->receipt_finalized)
        return st->fold_cursor > 0 ? st->fold_cursor - 1 : -1;
    if (st->utxo_apply_cursor > 0)
        return st->utxo_apply_cursor - 1;
    if (st->utxo_apply_cursor == 0)
        return -1;
    return st->tip_finalize_cursor;
}

#ifdef ZCL_TESTING
int64_t zcl_native_producer_applied_height_for_test(
    const struct producer_status_read *st);

int64_t zcl_native_producer_applied_height_for_test(
    const struct producer_status_read *st)
{
    return nc_producer_applied_height(st);
}
#endif

/* applied_at has one-second resolution.  Five seconds admits ordinary clock
 * scheduling/NTP jitter, while a larger future timestamp is rejected as ETA
 * evidence until wall time catches up. */
enum { NC_PRODUCER_RATE_FUTURE_SKEW_TOLERANCE_SECONDS = 5 };

void zcl_native_handle_ops_producer_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = json_get_str(json_get(request->input, "datadir"));
    if ((!datadir || !datadir[0]) && g_bridge_datadir[0])
        datadir = g_bridge_datadir;
    if (!datadir || !datadir[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               "ops.debug.producer");
        nc_add_describe_next(reply, request->spec->path,
                             "inspect the required producer datadir input");
        return;
    }
    if (strlen(datadir) >= CONSENSUS_STATE_PRODUCER_DATADIR_MAX) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "DATADIR_TOO_LONG",
                               "normalize", false, false,
                               "producer datadir must be at most 1023 bytes",
                               "ops.debug.producer");
        return;
    }

    struct producer_status_read st;
    char why[256];
    if (!consensus_state_producer_status_read(datadir, &st, why,
                                              sizeof(why))) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "PRODUCER_UNREADABLE",
                               "execute", true, false,
                               why[0] ? why : "producer datadir unreadable",
                               datadir);
        return;
    }

    char log_tail[512];
    nc_read_log_tail(datadir, log_tail, sizeof(log_tail));

    const char *receipt_state = st.receipt_finalized ? "finalized"
                              : st.session_open ? "open" : "absent";
    /* utxo_apply names the NEXT height to process; tip_finalize owns its
     * already-served height. A finalized receipt supersedes both. */
    int64_t height = nc_producer_applied_height(&st);
    const struct sha3_utxo_checkpoint *checkpoint =
        get_sha3_utxo_checkpoint();
    int64_t target_height = checkpoint ? checkpoint->height : -1;
    int64_t remaining = -1;
    if (height >= 0 && target_height >= 0)
        remaining = target_height > height ? target_height - height : 0;
    int64_t rate_sample_age_seconds = -1;
    int64_t rate_stale_after_seconds = -1;
    int64_t rate_future_skew_seconds = 0;
    bool rate_sample_clock_valid = false;
    bool durable_rate_recent = false;
    if (st.durable_rate_available) {
        int64_t now = platform_time_wall_unix();
        int64_t interval = st.rate_newer_time_unix - st.rate_older_time_unix;
        rate_stale_after_seconds = interval > 43200 ? 86400 : interval * 2;
        if (rate_stale_after_seconds < 300)
            rate_stale_after_seconds = 300;
        if (now > 0 && st.rate_newer_time_unix > now) {
            rate_future_skew_seconds = st.rate_newer_time_unix - now;
            rate_sample_clock_valid = rate_future_skew_seconds <=
                NC_PRODUCER_RATE_FUTURE_SKEW_TOLERANCE_SECONDS;
            if (rate_sample_clock_valid)
                rate_sample_age_seconds = 0;
        } else if (now > 0) {
            rate_sample_clock_valid = true;
            rate_sample_age_seconds = now - st.rate_newer_time_unix;
        }
        durable_rate_recent = rate_sample_clock_valid &&
            rate_sample_age_seconds <= rate_stale_after_seconds;
    }
    bool target_reached = height >= 0 && target_height >= 0 &&
                          height >= target_height;
    bool eta_available = target_reached ||
        (st.durable_rate_available && durable_rate_recent &&
         height >= 0 && target_height >= 0);
    int64_t eta_seconds = eta_available && remaining > 0
        ? (remaining * INT64_C(1000) +
           st.rate_blocks_per_second_milli - 1) /
              st.rate_blocks_per_second_milli
        : eta_available ? 0 : -1;

    char t[2048];
    if (!st.progress_kv_present) {
        (void)snprintf(t, sizeof(t),
                       "producer=%s state=not_started height=unknown",
                       datadir);
    } else if (eta_available && st.durable_rate_available) {
        int64_t rate_whole = st.rate_blocks_per_second_milli / 1000;
        int64_t rate_tenth =
            (st.rate_blocks_per_second_milli % 1000) / 100;
        (void)snprintf(
            t, sizeof(t),
            "producer=%s receipt=%s height=%lld target=%lld remaining=%lld "
            "rate=%lld.%lldblk/s eta=%llds",
            datadir, receipt_state, (long long)height,
            (long long)target_height, (long long)remaining,
            (long long)rate_whole, (long long)rate_tenth,
            (long long)eta_seconds);
    } else if (eta_available) {
        (void)snprintf(t, sizeof(t),
                       "producer=%s receipt=%s height=%lld target=%lld "
                       "remaining=0 rate=unknown eta=0s",
                       datadir, receipt_state, (long long)height,
                       (long long)target_height);
    } else {
        (void)snprintf(t, sizeof(t),
                       "producer=%s receipt=%s height=%lld target=%lld "
                       "rate=unknown eta=unknown",
                       datadir, receipt_state, (long long)height,
                       (long long)target_height);
    }

    (void)json_push_kv_str(&reply->data, "datadir", datadir);
    (void)json_push_kv_bool(&reply->data, "progress_kv_present",
                            st.progress_kv_present);
    (void)json_push_kv_str(&reply->data, "receipt_state", receipt_state);
    (void)json_push_kv_bool(&reply->data, "session_open", st.session_open);
    (void)json_push_kv_bool(&reply->data, "receipt_finalized",
                            st.receipt_finalized);
    (void)json_push_kv_int(&reply->data, "height", height);
    (void)json_push_kv_int(&reply->data, "utxo_apply_cursor",
                           st.utxo_apply_cursor);
    (void)json_push_kv_int(&reply->data, "tip_finalize_cursor",
                           st.tip_finalize_cursor);
    (void)json_push_kv_int(&reply->data, "fold_cursor", st.fold_cursor);
    (void)json_push_kv_str(&reply->data, "receipt_schema",
                           st.receipt_schema);
    (void)json_push_kv_str(&reply->data, "source_tree_root",
                           st.source_tree_root);
    (void)json_push_kv_str(&reply->data, "source_epoch_digest",
                           st.source_epoch_digest);
    (void)json_push_kv_str(&reply->data, "producer_commit",
                           st.producer_commit);
    (void)json_push_kv_int(&reply->data, "validation_profile",
                           st.validation_profile);
    (void)json_push_kv_int(&reply->data, "target_height", target_height);
    (void)json_push_kv_str(&reply->data, "target_kind",
                           "compiled_sovereign_anchor");
    (void)json_push_kv_int(&reply->data, "remaining_blocks", remaining);
    if (height >= 0 && target_height > 0) {
        int64_t progress_ppm = height >= target_height
            ? INT64_C(1000000)
            : height * INT64_C(1000000) / target_height;
        (void)json_push_kv_int(&reply->data, "progress_ppm", progress_ppm);
    }
    (void)json_push_kv_bool(&reply->data, "durable_rate_available",
                            st.durable_rate_available);
    (void)json_push_kv_bool(&reply->data, "durable_rate_recent",
                            durable_rate_recent);
    (void)json_push_kv_bool(&reply->data, "rate_sample_clock_valid",
                            rate_sample_clock_valid);
    (void)json_push_kv_int(
        &reply->data, "rate_future_skew_tolerance_seconds",
        NC_PRODUCER_RATE_FUTURE_SKEW_TOLERANCE_SECONDS);
    (void)json_push_kv_str(&reply->data, "rate_source",
                           "progress.kv:utxo_apply_log.applied_at");
    if (st.durable_rate_available) {
        (void)json_push_kv_int(&reply->data, "rate_older_height",
                               st.rate_older_height);
        (void)json_push_kv_int(&reply->data, "rate_older_time_unix",
                               st.rate_older_time_unix);
        (void)json_push_kv_int(&reply->data, "rate_newer_height",
                               st.rate_newer_height);
        (void)json_push_kv_int(&reply->data, "rate_newer_time_unix",
                               st.rate_newer_time_unix);
        (void)json_push_kv_int(&reply->data,
                               "rate_blocks_per_second_milli",
                               st.rate_blocks_per_second_milli);
        (void)json_push_kv_int(&reply->data, "rate_sample_age_seconds",
                               rate_sample_age_seconds);
        (void)json_push_kv_int(&reply->data, "rate_stale_after_seconds",
                               rate_stale_after_seconds);
        (void)json_push_kv_int(&reply->data, "rate_future_skew_seconds",
                               rate_future_skew_seconds);
    }
    (void)json_push_kv_bool(&reply->data, "eta_available", eta_available);
    if (eta_available) {
        (void)json_push_kv_int(&reply->data, "eta_seconds", eta_seconds);
        (void)json_push_kv_str(&reply->data, "eta_target",
                               "compiled_sovereign_anchor");
    }
    (void)json_push_kv_str(&reply->data, "last_log", log_tail);
    (void)json_push_kv_str(&reply->data, "text", t);
}

/* ── ops.rom native leaf ─────────────────────────────────────────────────
 * Dispatches `dumpstate rom_compile` (app/jobs/src/rom_compile_status.c —
 * pure composition over EXISTING telemetry: the per-stage step-EWMA
 * counters, the refold-in-progress signal, the L0 reducer frontier, the
 * sealed segment store, and the state-seal ring — no second producer)
 * against THIS running node and renders the rich-ASCII human view via
 * rom_compile_render_ascii. The structured zcl.rom_compile.v1 body is
 * copied into reply->data verbatim for machine consumers; the CLI prints
 * data.text unless --format=json. */
void zcl_native_handle_ops_rom(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;

    bridge_ensure_rpc_client();
    char *result = node_rpc_call("dumpstate", "[\"rom_compile\"]");
    if (!result) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "the node did not return a rom_compile body",
                               "ops.rom");
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the node is running");
        return;
    }
    struct json_value envelope;
    if (!json_read(&envelope, result, strlen(result)) ||
        envelope.type != JSON_OBJ) {
        json_free(&envelope);
        free(result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_BODY",
                               "execute", false, false,
                               "dumpstate rom_compile returned unparsable JSON",
                               "ops.rom");
        return;
    }
    free(result);

    const struct json_value *err = json_get(&envelope, "error");
    if (err && err->type == JSON_STR) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "ROM_STATUS_ERROR",
                               "execute", false, false,
                               json_get_str(err), "ops.rom");
        json_free(&envelope);
        return;
    }

    const struct json_value *state = json_get(&envelope, "state");
    char text[4096];
    rom_compile_render_ascii(state, text, sizeof(text));

    json_free(&reply->data);
    json_init(&reply->data);
    if (state && state->type == JSON_OBJ)
        json_copy(&reply->data, state);
    else
        json_set_object(&reply->data);
    (void)json_push_kv_str(&reply->data, "text", text);
    json_free(&envelope);
}

/* ── ops.rom --watch: live redraw loop (Phase B) ─────────────────────────
 * The single-shot ops.rom leaf above is unchanged. When the operator adds
 * --watch/--once (optionally --interval=<secs>, --datadir=<dir>), the argv path
 * intercepts BEFORE registry dispatch (native_command_main) and drives
 * rom_watch_run with one of two fetch closures: the live-node dumpstate RPC, or
 * the read-only offline composer against a foreign producer datadir. All the
 * loop/redraw/parse logic lives in rom_watch_loop.c + rom_compile_offline.c;
 * this file only builds the closure and parses the four flags. */

/* Live fetch: dumpstate rom_compile against THIS node, unwrap the `state`. */
static bool nc_rom_fetch_live(void *ctx, struct json_value *out, char *err,
                              size_t errlen)
{
    (void)ctx;
    bridge_ensure_rpc_client();
    char *result = node_rpc_call("dumpstate", "[\"rom_compile\"]");
    if (!result) {
        (void)snprintf(err, errlen, "node did not return a rom_compile body");
        return false;
    }
    struct json_value env;
    if (!json_read(&env, result, strlen(result)) || env.type != JSON_OBJ) {
        json_free(&env);
        free(result);
        (void)snprintf(err, errlen, "dumpstate rom_compile returned unparsable JSON");
        return false;
    }
    free(result);
    const struct json_value *e = json_get(&env, "error");
    if (e && e->type == JSON_STR) {
        (void)snprintf(err, errlen, "%s", json_get_str(e));
        json_free(&env);
        return false;
    }
    const struct json_value *state = json_get(&env, "state");
    if (!state || state->type != JSON_OBJ) {
        (void)snprintf(err, errlen, "dumpstate rom_compile returned no state body");
        json_free(&env);
        return false;
    }
    json_copy(out, state);
    json_free(&env);
    return true;
}

/* Offline fetch: compose a rom_compile body from a foreign producer datadir. */
static bool nc_rom_fetch_offline(void *ctx, struct json_value *out, char *err,
                                 size_t errlen)
{
    const char *datadir = (const char *)ctx;
    return rom_compile_offline_compose(datadir, out, err, errlen);
}

/* Scan the unconsumed argv words for the ops.rom watch flags. If neither
 * --watch, --once, nor --datadir=<dir> is present, returns false (the caller
 * falls through to the normal single-shot dispatch). Otherwise builds the fetch
 * closure + opts, runs rom_watch_run, stores its exit code in *rc, and returns
 * true. Recognizes: --watch, --once, --interval=<secs>, --datadir=<dir>. */
static bool nc_ops_rom_try_watch(const char *const *words, size_t count,
                                 size_t consumed, const char *cli_datadir,
                                 int *rc)
{
    bool want_watch = false, want_once = false;
    int interval_ms = 2000;
    const char *offline_datadir = NULL;

    for (size_t i = consumed; i < count; i++) {
        const char *w = words[i];
        if (!w)
            continue;
        if (strcmp(w, "--watch") == 0) {
            want_watch = true;
        } else if (strcmp(w, "--once") == 0) {
            want_once = true;
        } else if (strncmp(w, "--interval=", 11) == 0) {
            int secs = atoi(w + 11);
            if (secs > 0)
                interval_ms = secs * 1000;
        } else if (strncmp(w, "--datadir=", 10) == 0) {
            offline_datadir = w + 10;
        }
    }

    if (!want_watch && !want_once && !offline_datadir)
        return false;

    struct rom_watch_opts opts = {
        .interval_ms = interval_ms,
        /* --once (or the default single offline shot) renders exactly once;
         * --watch loops until interrupted. */
        .max_iters = want_watch && !want_once ? 0 : 1,
        .ansi = isatty(fileno(stdout)) ? true : false,
        .stream = stdout,
    };

    if (offline_datadir && offline_datadir[0]) {
        *rc = rom_watch_run(nc_rom_fetch_offline, (void *)offline_datadir,
                            &opts);
    } else if (offline_datadir) {
        /* --datadir= with an empty value: fall back to the CLI default if any,
         * else run live. */
        if (cli_datadir && cli_datadir[0])
            *rc = rom_watch_run(nc_rom_fetch_offline, (void *)cli_datadir,
                                &opts);
        else
            *rc = rom_watch_run(nc_rom_fetch_live, NULL, &opts);
    } else {
        *rc = rom_watch_run(nc_rom_fetch_live, NULL, &opts);
    }
    return true;
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
        nc_add_describe_next(reply, request->spec->path,
                             "inspect the boot-status datadir input");
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
        nc_add_string_next(reply, "core.node.bootwait", "datadir", datadir,
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

static bool nc_next_input_valid(const char *current_command,
                                const char *next_command,
                                const struct json_value *input)
{
    const struct zcl_command_spec *next_spec =
        zcl_command_registry_find(catalog(), next_command, NULL);
    char why[160] = {0};
    if (!next_spec || next_spec->mode == ZCL_COMMAND_MODE_BRANCH ||
        (current_command && current_command[0] &&
         strcmp(current_command, next_spec->path) == 0) ||
        !zcl_command_registry_input_validate(next_spec, input, why,
                                             sizeof(why)))
        return false;

    return true;
}

static void nc_print_error(const char *command, const char *code,
                           const char *phase, const char *message,
                           const char *evidence,
                           const char *next_command,
                           const char *next_input, const char *next_reason)
{
    struct json_value root, error, blockers, next, item;
    json_init(&root);
    json_init(&error);
    json_init(&blockers);
    json_init(&next);
    json_init(&item);
    json_set_object(&root);
    json_set_object(&error);
    json_set_array(&blockers);
    json_set_array(&next);
    json_set_object(&item);

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
        if (next_input && next_input[0] &&
            json_read(&parsed, next_input, strlen(next_input)) &&
            parsed.type == JSON_OBJ &&
            nc_next_input_valid(command, next_command, &parsed)) {
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
    json_free(&item);
    json_free(&next);
    json_free(&blockers);
    json_free(&error);
    json_free(&root);
}

static void nc_print_error_next_string(
    const char *command, const char *code, const char *phase,
    const char *message, const char *evidence, const char *next_command,
    const char *next_key, const char *next_value, const char *next_reason)
{
    struct json_value input;
    json_init(&input);
    json_set_object(&input);
    char encoded[512];
    bool ok = next_key && next_value &&
              json_push_kv_str(&input, next_key, next_value);
    size_t n = ok ? json_write(&input, encoded, sizeof(encoded)) : 0;
    json_free(&input);
    nc_print_error(command, code, phase, message, evidence,
                   n > 0 && n < sizeof(encoded) ? next_command : NULL,
                   n > 0 && n < sizeof(encoded) ? encoded : NULL,
                   next_reason);
}

/* Print a branch menu. Returns a contract exit code. */
static int nc_emit_menu(const char *path)
{
    char out[ZCL_COMMAND_BRANCH_BUDGET + 1];
    size_t n = zcl_command_registry_menu_json(catalog(), path, out,
                                              sizeof(out));
    if (n == 0) {
        nc_print_error_next_string(
            path, "MENU_BUDGET", "serialize",
            "menu exceeded its byte budget", path, "discover.describe",
            "path", path, "inspect this branch contract");
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
        if (strcmp(spec->path, "discover.describe") == 0) {
            nc_print_error(spec->path, "UNKNOWN_PATH", "resolve",
                           "no such command path or budget exceeded",
                           arg ? arg : "", "discover.help", "{}",
                           "browse the tree first");
        } else {
            nc_print_error_next_string(
                spec->path, "UNKNOWN_PATH", "resolve",
                "no such command path or budget exceeded", arg ? arg : "",
                "discover.describe", "path", spec->path,
                "inspect this discovery command");
        }
        return ZCL_COMMAND_EXIT_INVALID;
    }
    printf("%s\n", out);
    return ZCL_COMMAND_EXIT_OK;
}

/* Operator-UX prose leaves: default output is a human/AI-readable text block
 * rendered from reply->data; --format=json emits the structured envelope. */
static bool nc_is_prose_leaf(const char *path)
{
    return path &&
           (strcmp(path, "status") == 0 ||
            strcmp(path, "ops.debug.explain") == 0 ||
            strcmp(path, "ops.debug.profile") == 0 ||
            strcmp(path, "ops.debug.producer") == 0 ||
            strcmp(path, "ops.debug.rom") == 0 ||
            strcmp(path, "core.status.brief") == 0);
}

/* ── CLI UX contract: ONE-LINE status brief ──────────────────────────
 * See docs/NATIVE_COMMAND_INTERFACE.md "CLI UX contract". Exactly one line
 * (<=200 bytes), stable `key=value` pairs separated by single spaces, no
 * JSON braces. Reads only the flat fields core.status.brief already computed
 * (zcl_native_status_brief_body) — the render and the field selector below
 * both read this one JSON object; neither builds a second data path. */
static void nc_kv_int_or_unknown(char *buf, size_t cap, size_t *len,
                                 const char *key, const struct json_value *v)
{
    int n;
    if (v && v->type == JSON_INT)
        n = snprintf(buf + *len, cap - *len, "%s=%lld ", key,
                     (long long)json_get_int(v));
    else
        n = snprintf(buf + *len, cap - *len, "%s=unknown ", key);
    if (n > 0 && (size_t)n < cap - *len)
        *len += (size_t)n;
}

/* Exposed (non-static) so test_operator_ux can drive it with a fabricated
 * brief body and assert each key=value pair renders. */
void zcl_native_status_brief_render(const struct json_value *d, char *buf,
                                    size_t cap)
{
    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    size_t len = 0;

    nc_kv_int_or_unknown(buf, cap, &len, "hstar", json_get(d, "hstar"));
    nc_kv_int_or_unknown(buf, cap, &len, "gap", json_get(d, "gap"));
    nc_kv_int_or_unknown(buf, cap, &len, "peer_best", json_get(d, "peer_best"));

    const char *sync_state = json_get_str(json_get(d, "sync_state"));
    int n = snprintf(buf + len, cap - len, "sync=%s ",
                     (sync_state && sync_state[0]) ? sync_state : "unknown");
    if (n > 0 && (size_t)n < cap - len) len += (size_t)n;

    const char *blocker = json_get_str(json_get(d, "primary_blocker"));
    n = snprintf(buf + len, cap - len, "blocker=%s ",
                (blocker && blocker[0]) ? blocker : "unknown");
    if (n > 0 && (size_t)n < cap - len) len += (size_t)n;

    const struct json_value *bage = json_get(d, "blocker_age_s");
    if (bage && bage->type == JSON_INT)
        n = snprintf(buf + len, cap - len, "blocker_age=%llds ",
                    (long long)json_get_int(bage));
    else
        n = snprintf(buf + len, cap - len, "blocker_age=unknown ");
    if (n > 0 && (size_t)n < cap - len) len += (size_t)n;

    nc_kv_int_or_unknown(buf, cap, &len, "conditions",
                        json_get(d, "active_conditions"));
    nc_kv_int_or_unknown(buf, cap, &len, "peers", json_get(d, "peer_count"));

    const struct json_value *rss = json_get(d, "rss_mb");
    if (rss && rss->type == JSON_INT)
        n = snprintf(buf + len, cap - len, "rss_mb=%lld",
                    (long long)json_get_int(rss));
    else
        n = snprintf(buf + len, cap - len, "rss_mb=unknown");
    if (n > 0 && (size_t)n < cap - len) len += (size_t)n;

    /* Every field above but the last appends its own trailing separator
     * space; trim it defensively (also covers a mid-line snprintf that hit
     * the buffer edge). Then hard-clamp to the 200-byte contract — real
     * fields never get near this, but the CLI must never emit a line the
     * spec forbids. */
    if (len > 0 && buf[len - 1] == ' ')
        buf[--len] = '\0';
    if (len > 200)
        buf[200] = '\0';
}

/* Pick one short, deterministic next step from the same brief body: a named
 * blocker outranks "still behind" outranks the native health leaf. Never
 * allocates. */
const char *zcl_native_status_brief_next_command(const struct json_value *d)
{
    const char *blocker = json_get_str(json_get(d, "primary_blocker"));
    if (blocker && blocker[0] && strcmp(blocker, "none") != 0 &&
        strcmp(blocker, "unknown") != 0)
        return "zclassic23 explain blockers";
    const struct json_value *gapv = json_get(d, "gap");
    if (gapv && gapv->type == JSON_INT && json_get_int(gapv) > 0)
        return "zclassic23 explain sync";
    return "zclassic23 ops health";
}

/* ── CLI UX contract: field selector ─────────────────────────────────
 * See docs/NATIVE_COMMAND_INTERFACE.md "CLI UX contract". `status field=` and
 * `dumpstate <subsystem> field=` both call this one function — neither
 * hand-rolls its own key lookup. */
bool zcl_native_render_field_selection(const struct json_value *obj,
                                       const char *fields_csv,
                                       char *out, size_t out_cap,
                                       char *err, size_t err_cap)
{
    if (err && err_cap)
        err[0] = '\0';
    if (!obj || obj->type != JSON_OBJ) {
        if (err) snprintf(err, err_cap, "nothing to select fields from");
        return false;
    }
    if (!fields_csv || !fields_csv[0]) {
        if (err) snprintf(err, err_cap, "field= requires at least one name");
        return false;
    }

    enum { NC_FIELD_MAX = 24, NC_FIELD_NAME_MAX = 65 };
    char names[NC_FIELD_MAX][NC_FIELD_NAME_MAX];
    size_t nnames = 0;
    const char *p = fields_csv;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && end[-1] == ' ') end--;
        size_t flen = (size_t)(end - start);
        if (flen == 0 || flen >= NC_FIELD_NAME_MAX) {
            if (err) snprintf(err, err_cap,
                             "malformed field name in 'field=%s'", fields_csv);
            return false;
        }
        if (nnames >= NC_FIELD_MAX) {
            if (err) snprintf(err, err_cap,
                             "too many fields requested (max %d)",
                             NC_FIELD_MAX);
            return false;
        }
        memcpy(names[nnames], start, flen);
        names[nnames][flen] = '\0';
        for (size_t j = 0; j < nnames; j++) {
            if (strcmp(names[j], names[nnames]) == 0) {
                if (err) snprintf(err, err_cap, "duplicate field '%s'",
                                 names[nnames]);
                return false;
            }
        }
        nnames++;
    }
    if (nnames == 0) {
        if (err) snprintf(err, err_cap, "field= requires at least one name");
        return false;
    }

    /* Validate every name exists before rendering anything — never a
     * partial selection. */
    for (size_t i = 0; i < nnames; i++) {
        if (json_get(obj, names[i]))
            continue;
        char known[320];
        size_t klen = 0;
        known[0] = '\0';
        for (size_t k = 0; k < obj->num_children && k < 16; k++) {
            int n = snprintf(known + klen, sizeof(known) - klen, "%s%s",
                             klen ? "," : "", obj->keys[k]);
            if (n > 0 && (size_t)n < sizeof(known) - klen)
                klen += (size_t)n;
        }
        if (err)
            snprintf(err, err_cap, "no such field '%s'; known: %s", names[i],
                     known);
        return false;
    }

    size_t len = 0;
    for (size_t i = 0; i < nnames; i++) {
        const struct json_value *v = json_get(obj, names[i]);
        char valbuf[4096];
        switch (v->type) {
        case JSON_BOOL:
            snprintf(valbuf, sizeof(valbuf), "%s",
                    json_get_bool(v) ? "true" : "false");
            break;
        case JSON_INT:
            snprintf(valbuf, sizeof(valbuf), "%lld",
                    (long long)json_get_int(v));
            break;
        case JSON_REAL:
            snprintf(valbuf, sizeof(valbuf), "%g", json_get_real(v));
            break;
        case JSON_STR: {
            const char *s = json_get_str(v);
            snprintf(valbuf, sizeof(valbuf), "%s", s ? s : "");
            break;
        }
        case JSON_NULL:
            snprintf(valbuf, sizeof(valbuf), "null");
            break;
        case JSON_ARR:
        case JSON_OBJ:
        default: {
            /* json_write returns the bytes NEEDED — >= cap means the value
             * was cut mid-string. Never emit a truncated container: fail
             * typed so the caller reaches for --format=json instead. */
            size_t need = json_write(v, valbuf, sizeof(valbuf));
            if (need >= sizeof(valbuf)) {
                if (err)
                    snprintf(err, err_cap,
                             "field '%s' is a %zu-byte container — too large "
                             "for key=value rendering; use --format=json",
                             names[i], need);
                return false;
            }
            break;
        }
        }
        int n = snprintf(out + len, out_cap - len, "%s=%s\n", names[i],
                         valbuf);
        if (n <= 0 || (size_t)n >= out_cap - len) {
            if (err) snprintf(err, err_cap,
                             "field selection exceeded the output buffer");
            return false;
        }
        len += (size_t)n;
    }
    return true;
}

/* ── CLI UX contract: unrecognized-command diagnostic ────────────────
 * See docs/NATIVE_COMMAND_INTERFACE.md "CLI UX contract". Pure text
 * builder — src/main.c's raw-RPC fallback calls this once it has confirmed
 * (via the RPC layer's method-not-found response) that `method` is not a
 * real command, then fprintf's the result to stderr. */
size_t zcl_native_render_unknown_command(
    const struct zcl_command_registry *reg, const char *method, char *out,
    size_t out_cap)
{
    if (!out || out_cap == 0 || !method || !method[0])
        return 0;
    out[0] = '\0';
    size_t len = 0;
    int n = snprintf(out + len, out_cap - len,
                     "error=UNKNOWN_COMMAND detail=no such command '%s' "
                     "try=zclassic23 discover search %s\n",
                     method, method);
    if (n <= 0 || (size_t)n >= out_cap - len)
        return 0;
    len += (size_t)n;

    if (!reg)
        return len;
    char buf[ZCL_COMMAND_LIST_BUDGET + 1];
    size_t bn = zcl_command_registry_search_json(reg, method, buf,
                                                 sizeof(buf));
    if (bn == 0)
        return len;
    struct json_value doc;
    if (!json_read(&doc, buf, bn) || doc.type != JSON_OBJ) {
        json_free(&doc);
        return len;
    }
    const struct json_value *matches = json_get(&doc, "matches");
    if (matches && matches->type == JSON_ARR && matches->num_children > 0) {
        n = snprintf(out + len, out_cap - len, "did you mean:");
        if (n > 0 && (size_t)n < out_cap - len) {
            len += (size_t)n;
            for (size_t i = 0; i < matches->num_children && i < 3; i++) {
                const char *path = json_get_str(
                    json_get(&matches->children[i], "path"));
                if (!path || !path[0])
                    continue;
                n = snprintf(out + len, out_cap - len, " %s", path);
                if (n > 0 && (size_t)n < out_cap - len)
                    len += (size_t)n;
            }
            n = snprintf(out + len, out_cap - len, "\n");
            if (n > 0 && (size_t)n < out_cap - len)
                len += (size_t)n;
        }
    }
    json_free(&doc);
    return len;
}

/* Render the prose text for a prose leaf into `buf`. Returns true if a text
 * block was produced; false means the caller should print the JSON envelope
 * (e.g. an error result with no data.text). `data` is the envelope's `data`. */
static bool nc_prose_text(const char *path, const struct json_value *data,
                          char *buf, size_t cap)
{
    if (!data || data->type != JSON_OBJ)
        return false;
    const char *text = json_get_str(json_get(data, "text"));
    if (text && text[0]) {
        (void)snprintf(buf, cap, "%s", text);
        return true;
    }
    if (strcmp(path, "status") == 0 ||
        strcmp(path, "core.status.brief") == 0) {
        zcl_native_status_brief_render(data, buf, cap);
        return true;
    }
    return false;
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
        nc_print_error_next_string(
            root_word, "UNKNOWN_COMMAND", "resolve", "unknown command root",
            root_word, "discover.search", "query", root_word,
            "search for the intended command");
        return ZCL_COMMAND_EXIT_INVALID;
    }

    /* ops.rom watch mode: intercept --watch / --once / --interval=<secs> /
     * --datadir=<dir> BEFORE the flag parser below (ops.debug.rom takes empty
     * input, so those flags would otherwise be rejected as unknown keys). When
     * one is present this runs the redraw loop and returns its exit code. */
    if (strcmp(spec->path, "ops.debug.rom") == 0) {
        int rc = 0;
        if (nc_ops_rom_try_watch(words, count, consumed, datadir, &rc))
            return rc;
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
    /* CLI UX contract: field selector + the bare no-arg entry point's next-
     * command hint. `field=` is accepted BOTH as a bare dash-less word (the
     * documented `zclassic23 status field=a,b` convention) and as a normal
     * `--field=a,b` flag; both set the same field_csv. --next is
     * internal-ish (used by the bare no-arg entry point) but harmless for a
     * caller to pass directly. */
    const char *field_csv = NULL;
    bool seen_field = false;
    bool suggest_next = false;
    char flag_key[128];
    char flag_why[160] = "malformed or duplicate option";
    for (size_t i = consumed; i < count; i++) {
        const char *w = words[i];
        if (!nc_is_flag(w)) {
            if (strncmp(w, "field=", 6) == 0) {
                if (seen_field || !w[6]) {
                    flag_error = true;
                    (void)snprintf(flag_why, sizeof(flag_why),
                                  "field= requires one non-empty value and "
                                  "may appear once");
                    break;
                }
                seen_field = true;
                field_csv = w + 6;
                continue;
            }
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
        } else if (strcmp(flag_key, "field") == 0) {
            if (seen_field || !value || !value[0]) {
                flag_error = true;
                (void)snprintf(flag_why, sizeof(flag_why),
                               "--field requires one non-empty value and "
                               "may appear once");
                break;
            }
            seen_field = true;
            field_csv = value;
        } else if (strcmp(flag_key, "next") == 0) {
            suggest_next = true;
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
        nc_print_error_next_string(
            spec->path, "BAD_FLAG", "normalize", flag_why, spec->path,
            "discover.describe", "path", spec->path,
            "inspect the input schema");
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
            json_free(&flags);
            nc_print_error_next_string(
                attempted, "UNKNOWN_COMMAND", "resolve",
                "no such command under this branch", attempted,
                "discover.search", "query", positional[0],
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
                nc_print_error_next_string(
                    spec->path, "BAD_INPUT", "normalize",
                    "stdin --input=- must be one JSON object", spec->path,
                    "discover.schema", "path", spec->path,
                    "inspect the input schema");
                return ZCL_COMMAND_EXIT_INVALID;
            }
        } else if (!json_read(&input, input_flag, strlen(input_flag)) ||
                   input.type != JSON_OBJ) {
            json_free(&input);
            json_free(&flags);
            nc_print_error_next_string(
                spec->path, "BAD_INPUT", "normalize",
                "--input must be one JSON object", spec->path,
                "discover.schema", "path", spec->path,
                "inspect the input schema");
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
                nc_print_error_next_string(
                    spec->path, "TOO_MANY_ARGS", "normalize",
                    "more positional arguments than the leaf accepts",
                    spec->path, "discover.schema", "path", spec->path,
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
        nc_print_error_next_string(
            spec->path, "INVALID_INPUT", "normalize", why, spec->path,
            "discover.schema", "path", spec->path,
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
            nc_print_error_next_string(
                spec->path, "DUPLICATE_CONTROL", "normalize",
                "view was supplied both inside --input and as a flag", "view",
                "discover.schema", "path", spec->path,
                "supply each response control once");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        view = json_get_str(input_view);
    }
    const struct json_value *input_max_items = json_get(&input, "max_items");
    if (input_max_items) {
        if (seen_max_items) {
            json_free(&input);
            nc_print_error_next_string(
                spec->path, "DUPLICATE_CONTROL", "normalize",
                "max_items was supplied both inside --input and as a flag",
                "max_items", "discover.schema", "path", spec->path,
                "supply each response control once");
            return ZCL_COMMAND_EXIT_INVALID;
        }
        max_items = (size_t)json_get_int(input_max_items);
    }
    const struct json_value *input_cursor_value = json_get(&input, "cursor");
    if (input_cursor_value) {
        if (seen_cursor) {
            json_free(&input);
            nc_print_error_next_string(
                spec->path, "DUPLICATE_CONTROL", "normalize",
                "cursor was supplied both inside --input and as a flag",
                "cursor", "discover.schema", "path", spec->path,
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
        /* The local argv operator is omnipotent: full capabilities and the
         * OWNER authority ceiling. Remote/multi-user sessions raise the ceiling
         * from their role instead (never reaching this argv path). */
        .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
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

    /* CLI UX contract: field selector. `field=`/--field= wins over prose and
     * --format=json alike — a caller who named fields wants exactly those
     * lines, nothing else. Selects out of reply.data, the SAME object the
     * JSON envelope and the prose renderer below both read; no second data
     * path. Unknown field name -> the frozen `error=... detail=... try=...`
     * one-line error contract (docs/NATIVE_COMMAND_INTERFACE.md). */
    if (field_csv) {
        struct json_value env;
        bool handled = false;
        if (json_read(&env, out, n) && env.type == JSON_OBJ) {
            const struct json_value *data = json_get(&env, "data");
            char sel[ZCL_COMMAND_LIST_BUDGET + 1];
            char selerr[320];
            if (data && zcl_native_render_field_selection(
                            data, field_csv, sel, sizeof(sel), selerr,
                            sizeof(selerr))) {
                fputs(sel, stdout);
                handled = true;
            } else {
                fprintf(stderr,
                       "error=UNKNOWN_FIELD detail=%s try=%s\n",
                       data ? selerr : "this result has no selectable data",
                       spec->path);
                json_free(&env);
                return ZCL_COMMAND_EXIT_INVALID;
            }
        }
        json_free(&env);
        if (handled)
            return (int)exit_code;
    }

    /* Prose leaves render a human/AI-readable text block by default; an
     * explicit --format=json (seen_format) keeps the structured envelope. On a
     * failed result (no data.text) fall back to the JSON envelope so the
     * structured error/next-action is never hidden. */
    if (!seen_format && nc_is_prose_leaf(spec->path)) {
        struct json_value env;
        if (json_read(&env, out, n) && env.type == JSON_OBJ) {
            char text[ZCL_COMMAND_LIST_BUDGET + 1];
            const struct json_value *data = json_get(&env, "data");
            if (nc_prose_text(spec->path, data, text, sizeof(text))) {
                printf("%s\n", text);
                if (suggest_next &&
                    (strcmp(spec->path, "status") == 0 ||
                     strcmp(spec->path, "core.status.brief") == 0))
                    printf("next: %s\n",
                          zcl_native_status_brief_next_command(data));
                json_free(&env);
                return (int)exit_code;
            }
        }
        json_free(&env);
    }

    printf("%s\n", out);
    return (int)exit_code;
}
