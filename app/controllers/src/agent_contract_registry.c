/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/agent_controller.h"

#include "json/json.h"

#include <stdio.h>
#include <string.h>

static const struct agent_contract g_agent_contracts[] = {
#define AGENT_CONTRACT(method, capability, schema, native, mcp, rest,          \
                       api_cli_field, api_mcp_field, ops_surface, ops_rank,   \
                       ops_name, ops_purpose, purpose)                        \
    { method, capability, schema, native, mcp, rest,                           \
      api_cli_field, api_mcp_field, ops_surface, ops_rank, ops_name,          \
      ops_purpose, purpose },
#include "controllers/agent_contracts.def"
#undef AGENT_CONTRACT
};

static const size_t g_agent_contract_count =
    sizeof(g_agent_contracts) / sizeof(g_agent_contracts[0]);

struct agent_contract_command_surface {
    const char *surface;
    int rank;
    const char *name;
    const char *method;
    const char *purpose_override;
};

struct agent_contract_work_surface {
    const char *surface;
    int rank;
    const char *name;
    const char *why;
    const char *first_slice;
    const char *proof;
};

static const struct agent_contract_command_surface g_agent_command_surfaces[] = {
    { "agentmap.commands.core", 1, "status", "agent",
      "compact live health/status contract" },
    { "agentmap.commands.core", 2, "map", "agentmap",
      "where code, docs, and tests live" },
    { "agentmap.commands.core", 3, "impact", "agentimpact",
      "changed files to recommended tests and risk flags" },
    { "agentmap.commands.core", 4, "contracts", "agentcontracts",
      "versioned schemas and transport contract list" },
    { "agentmap.commands.core", 5, "build", "agentbuild",
      "incremental/cache/reproducible build contract" },
    { "agentmap.commands.core", 6, "anchor_status", "anchorstatus",
      "offline anchor-mint progress and next action" },
    { "agentmap.commands.core", 7, "interface", "agentinterface",
      "preferred AI/operator transport and JSON rules" },
    { "agentmap.commands.core", 8, "lanes", "agentlanes",
      "canonical/soak/dev topology and restart/deploy rules" },
    { "agentmap.commands.core", 9, "liveness", "agentliveness",
      "lane/service/supervisor/background-quality liveness" },
    { "agentmap.commands.core", 10, "deploy_guard", "agentdeployguard",
      "C-native deploy/restart allow-refuse decision" },

    { "agentmap.commands.drilldown", 1, "health", "healthcheck",
      "strict health drill-down" },
    { "agentmap.commands.drilldown", 2, "logs", "getnodelog",
      "bounded server-side log search" },
    { "agentmap.commands.drilldown", 3, "timeline", "timeline",
      "category-filtered event timeline with bounded filters" },
    { "agentmap.commands.drilldown", 4, "state", "dumpstate",
      "generic subsystem diagnostics" },
    { "agentmap.commands.drilldown", 5, "state_catalog", "statecatalog",
      "zcl_state subsystem catalog" },

    { "agentmap.telemetry", 1, "subsystem_state", "dumpstate",
      "semantic subsystem internals through diagnostics registry" },
    { "agentmap.telemetry", 2, "subsystem_catalog", "statecatalog",
      "machine catalog of diagnostics registry subsystems" },
    { "agentmap.telemetry", 3, "node_log", "getnodelog",
      "server-side regex tail over node.log history" },
    { "agentmap.telemetry", 4, "timeline", "timeline",
      "versioned category-filtered event timeline with bounded filters" },
    { "agentmap.telemetry", 5, "anchor_status", "anchorstatus",
      "read-only progress.kv status for the sovereign anchor producer" },
};

static const size_t g_agent_command_surface_count =
    sizeof(g_agent_command_surfaces) / sizeof(g_agent_command_surfaces[0]);

static const struct agent_contract_work_surface g_agent_work_surfaces[] = {
    { "agentops.api_gaps", 1, "runtime_identity_everywhere",
      "Agents must know whether a payload came from source HEAD, dev, soak, or canonical.",
      "add build/lane identity to every first-call compact response",
      "syncdiag_rpc + mcp_controllers + api" },
    { "agentops.api_gaps", 2, "state_catalog_schema",
      "The first catalog now exists; next it should be kept rich enough for automated routing.",
      "extend catalog metadata as new dumpers need cost, freshness, key, and owner hints",
      "statecatalog RPC + MCP catalog tests" },
    { "agentops.api_gaps", 3, "timeline_query",
      "Agents still stitch together logs, SQL, events, and condition detail to answer what happened.",
      "ship and extend zcl.timeline.v1 before adding more bespoke log readers",
      "event + mcp_controllers + syncdiag_rpc" },

    { "agentops.top_next_work", 1,
      "finish_self_verified_utxo_anchor_rebuild",
      "It replaces the borrowed snapshot seed with a UTXO anchor rebuilt from zclassic23's own verified block history.",
      "run anchorstatus on the producer, fix any named blocker, then copy-prove -refold-from-anchor artifact and cutover gates",
      "copy fixture, refold tests, parity checks, live H* climb" },
    { "agentops.top_next_work", 2, "harden_peer_bootstrap_lifecycle",
      "Tip-following depends on failing over slow peers, avoiding duplicate peer rows, and proving zclassic23 peers can bootstrap other nodes.",
      "promote peer lifecycle incidents, downloader failover, and bootstrapstatus into one operator proof",
      "download + peer_lifecycle + syncdiag_rpc + bootstrap harness" },
    { "agentops.top_next_work", 3, "promote_mvp_operator_proofs",
      "MRS is 4/8; cold-start sync, live store flow, 168h soak, and exact parity still need full run-pass evidence.",
      "wire the remaining full proofs into milestone and background quality verdicts",
      "mvp-verify + soak-evidence + parity service" },
    { "agentops.top_next_work", 4, "extend_semantic_timeline_durability",
      "The event-ring timeline is semantic now; longer root-cause windows need durable event_log/node.log references.",
      "extend zcl.timeline.v1 toward durable event_log/node.log references",
      "event + mcp_controllers + syncdiag_rpc" },
    { "agentops.top_next_work", 5, "shrink_boot_refold_supervised_units",
      "The largest code-health risk is still oversized boot/refold orchestration that future agents must understand before changing liveness.",
      "split one behavior-preserving boot/refold responsibility behind existing supervisor contracts",
      "make lint + boot smoke + refold tests + live H* climb" },
};

static const size_t g_agent_work_surface_count =
    sizeof(g_agent_work_surfaces) / sizeof(g_agent_work_surfaces[0]);

static void agent_push_str(struct json_value *arr, const char *s)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, s ? s : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static void agent_append_summary(char *buf, size_t buf_sz, size_t *pos,
                                 const char *separator, const char *text)
{
    if (!buf || buf_sz == 0 || !pos || !text || !text[0])
        return;
    if (*pos >= buf_sz) {
        buf[buf_sz - 1] = '\0';
        return;
    }
    int n = snprintf(buf + *pos, buf_sz - *pos, "%s%s",
                     separator ? separator : "", text);
    if (n < 0)
        return;
    size_t wrote = (size_t)n;
    if (wrote >= buf_sz - *pos)
        *pos = buf_sz - 1;
    else
        *pos += wrote;
}

static bool agent_contract_surface_has(const char *surfaces,
                                       const char *surface)
{
    if (!surfaces || !surfaces[0] || !surface || !surface[0])
        return false;

    size_t want_len = strlen(surface);
    const char *p = surfaces;
    while (*p) {
        while (*p == ',' || *p == ' ')
            p++;
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len > 0 && p[len - 1] == ' ')
            len--;
        if (len == want_len && strncmp(p, surface, want_len) == 0)
            return true;
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}

size_t agent_contract_count(void)
{
    return g_agent_contract_count;
}

const struct agent_contract *agent_contract_at(size_t index)
{
    if (index >= g_agent_contract_count)
        return NULL;
    return &g_agent_contracts[index];
}

const struct agent_contract *agent_contract_lookup(const char *method)
{
    if (!method || !method[0])
        return NULL;
    for (size_t i = 0; i < g_agent_contract_count; i++) {
        if (strcmp(g_agent_contracts[i].method, method) == 0)
            return &g_agent_contracts[i];
    }
    return NULL;
}

bool agent_push_contract_json(struct json_value *arr,
                              const struct agent_contract *contract)
{
    if (!arr || !contract)
        return false;

    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "method", contract->method);
    json_push_kv_str(&obj, "capability", contract->capability);
    json_push_kv_str(&obj, "schema", contract->schema);
    json_push_kv_str(&obj, "native", contract->native_command);
    json_push_kv_str(&obj, "mcp", contract->mcp_tool);
    json_push_kv_str(&obj, "rest", contract->rest_route);
    json_push_kv_str(&obj, "api_cli_field", contract->api_cli_field);
    json_push_kv_str(&obj, "api_mcp_field", contract->api_mcp_field);
    json_push_kv_str(&obj, "ops_surface", contract->ops_surface);
    json_push_kv_int(&obj, "ops_rank", contract->ops_rank);
    json_push_kv_str(&obj, "ops_name", contract->ops_name);
    json_push_kv_str(&obj, "ops_purpose", contract->ops_purpose);
    json_push_kv_str(&obj, "purpose", contract->purpose);
    json_push_back(arr, &obj);
    json_free(&obj);
    return true;
}

void agent_push_contracts_json(struct json_value *arr)
{
    if (!arr)
        return;
    for (size_t i = 0; i < g_agent_contract_count; i++)
        agent_push_contract_json(arr, &g_agent_contracts[i]);
}

void agent_push_contract_transport_summary_json(struct json_value *arr)
{
    if (!arr)
        return;

    char native[4096];
    char mcp[4096];
    char rest[2048];
    size_t native_pos = 0;
    size_t mcp_pos = 0;
    size_t rest_pos = 0;
    bool native_first = true;
    bool mcp_first = true;
    bool rest_first = true;

    native[0] = '\0';
    mcp[0] = '\0';
    rest[0] = '\0';
    agent_append_summary(native, sizeof(native), &native_pos, "", "native: ");
    agent_append_summary(mcp, sizeof(mcp), &mcp_pos, "", "mcp: ");
    agent_append_summary(rest, sizeof(rest), &rest_pos, "", "rest: ");

    for (size_t i = 0; i < g_agent_contract_count; i++) {
        const struct agent_contract *c = &g_agent_contracts[i];
        agent_append_summary(native, sizeof(native), &native_pos,
                             native_first ? "" : " | ",
                             c->native_command);
        native_first = false;
        agent_append_summary(mcp, sizeof(mcp), &mcp_pos,
                             mcp_first ? "" : ", ", c->mcp_tool);
        mcp_first = false;
        if (c->rest_route && c->rest_route[0]) {
            agent_append_summary(rest, sizeof(rest), &rest_pos,
                                 rest_first ? "" : "; ", c->rest_route);
            rest_first = false;
        }
    }

    agent_push_str(arr, native);
    agent_push_str(arr, mcp);
    agent_push_str(arr, rest_first ? "rest: no REST-only agent route" : rest);
    agent_push_str(arr, "deprecated: tools/z compatibility shim only");
}

void agent_push_contract_ops_surface_json(struct json_value *arr,
                                          const char *surface)
{
    if (!arr || !surface || !surface[0])
        return;

    int max_rank = 0;
    for (size_t i = 0; i < g_agent_contract_count; i++) {
        const struct agent_contract *c = &g_agent_contracts[i];
        if (agent_contract_surface_has(c->ops_surface, surface) &&
            c->ops_rank > max_rank)
            max_rank = c->ops_rank;
    }

    for (int rank = 1; rank <= max_rank; rank++) {
        for (size_t i = 0; i < g_agent_contract_count; i++) {
            const struct agent_contract *c = &g_agent_contracts[i];
            if (c->ops_rank != rank ||
                !agent_contract_surface_has(c->ops_surface, surface))
                continue;
            agent_push_contract_command_json(arr, c->ops_name, c->method,
                                             c->ops_purpose);
        }
    }
}

size_t agent_contract_command_surface_count(const char *surface)
{
    if (!surface || !surface[0])
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < g_agent_command_surface_count; i++) {
        const struct agent_contract_command_surface *e =
            &g_agent_command_surfaces[i];
        if (e->surface && strcmp(e->surface, surface) == 0)
            n++;
    }
    return n;
}

size_t agent_push_contract_command_surface_json(struct json_value *arr,
                                                const char *surface)
{
    if (!arr || !surface || !surface[0])
        return 0;

    int max_rank = 0;
    for (size_t i = 0; i < g_agent_command_surface_count; i++) {
        const struct agent_contract_command_surface *e =
            &g_agent_command_surfaces[i];
        if (e->surface && strcmp(e->surface, surface) == 0 &&
            e->rank > max_rank)
            max_rank = e->rank;
    }

    size_t pushed = 0;
    for (int rank = 1; rank <= max_rank; rank++) {
        for (size_t i = 0; i < g_agent_command_surface_count; i++) {
            const struct agent_contract_command_surface *e =
                &g_agent_command_surfaces[i];
            if (e->rank != rank || !e->surface ||
                strcmp(e->surface, surface) != 0)
                continue;
            if (agent_push_contract_command_json(arr, e->name, e->method,
                                                 e->purpose_override))
                pushed++;
        }
    }
    return pushed;
}

size_t agent_contract_work_surface_count(const char *surface)
{
    if (!surface || !surface[0])
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < g_agent_work_surface_count; i++) {
        const struct agent_contract_work_surface *e =
            &g_agent_work_surfaces[i];
        if (e->surface && strcmp(e->surface, surface) == 0)
            n++;
    }
    return n;
}

static bool agent_push_work_item_json(
    struct json_value *arr, const struct agent_contract_work_surface *item)
{
    if (!arr || !item)
        return false;

    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_int(&obj, "rank", item->rank);
    json_push_kv_str(&obj, "name", item->name);
    json_push_kv_str(&obj, "why", item->why);
    json_push_kv_str(&obj, "first_slice", item->first_slice);
    json_push_kv_str(&obj, "proof", item->proof);
    json_push_back(arr, &obj);
    json_free(&obj);
    return true;
}

size_t agent_push_contract_work_surface_json(struct json_value *arr,
                                             const char *surface)
{
    if (!arr || !surface || !surface[0])
        return 0;

    int max_rank = 0;
    for (size_t i = 0; i < g_agent_work_surface_count; i++) {
        const struct agent_contract_work_surface *e =
            &g_agent_work_surfaces[i];
        if (e->surface && strcmp(e->surface, surface) == 0 &&
            e->rank > max_rank)
            max_rank = e->rank;
    }

    size_t pushed = 0;
    for (int rank = 1; rank <= max_rank; rank++) {
        for (size_t i = 0; i < g_agent_work_surface_count; i++) {
            const struct agent_contract_work_surface *e =
                &g_agent_work_surfaces[i];
            if (e->rank != rank || !e->surface ||
                strcmp(e->surface, surface) != 0)
                continue;
            if (agent_push_work_item_json(arr, e))
                pushed++;
        }
    }
    return pushed;
}

bool agent_push_contract_command_json(struct json_value *arr,
                                      const char *name,
                                      const char *method,
                                      const char *purpose_override)
{
    const struct agent_contract *c = agent_contract_lookup(method);
    if (!arr || !c)
        return false;

    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    const char *command_name = name && name[0] ? name : c->ops_name;
    json_push_kv_str(&obj, "name",
                     command_name && command_name[0]
                         ? command_name : c->capability);
    json_push_kv_str(&obj, "method", c->method);
    json_push_kv_str(&obj, "schema", c->schema);
    json_push_kv_str(&obj, "native", c->native_command);
    json_push_kv_str(&obj, "mcp", c->mcp_tool);
    const char *command_purpose =
        purpose_override && purpose_override[0]
            ? purpose_override : c->ops_purpose;
    json_push_kv_str(&obj, "purpose",
                     command_purpose && command_purpose[0]
                         ? command_purpose : c->purpose);
    json_push_back(arr, &obj);
    json_free(&obj);
    return true;
}

bool agent_push_contract_native_field_json(struct json_value *obj,
                                           const char *key,
                                           const char *method)
{
    const struct agent_contract *c = agent_contract_lookup(method);
    if (!obj || !key || !c)
        return false;
    return json_push_kv_str(obj, key, c->native_command);
}

bool agent_push_contract_mcp_field_json(struct json_value *obj,
                                        const char *key,
                                        const char *method)
{
    const struct agent_contract *c = agent_contract_lookup(method);
    if (!obj || !key || !c)
        return false;
    return json_push_kv_str(obj, key, c->mcp_tool);
}

void agent_push_contract_api_cli_fields_json(struct json_value *obj)
{
    if (!obj)
        return;
    for (size_t i = 0; i < g_agent_contract_count; i++) {
        const struct agent_contract *c = &g_agent_contracts[i];
        if (!c->api_cli_field || !c->api_cli_field[0])
            continue;
        json_push_kv_str(obj, c->api_cli_field, c->native_command);
    }
}

void agent_push_contract_api_mcp_fields_json(struct json_value *obj)
{
    if (!obj)
        return;
    for (size_t i = 0; i < g_agent_contract_count; i++) {
        const struct agent_contract *c = &g_agent_contracts[i];
        if (!c->api_mcp_field || !c->api_mcp_field[0])
            continue;
        json_push_kv_str(obj, c->api_mcp_field, c->mcp_tool);
    }
}
