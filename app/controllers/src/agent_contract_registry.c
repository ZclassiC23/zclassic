/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/agent_controller.h"

#include "json/json.h"

#include <stdio.h>
#include <string.h>

static const struct agent_contract g_agent_contracts[] = {
#define AGENT_CONTRACT(method, capability, schema, native, mcp, rest,          \
                       api_cli_field, api_mcp_field, purpose)                 \
    { method, capability, schema, native, mcp, rest,                           \
      api_cli_field, api_mcp_field, purpose },
#include "controllers/agent_contracts.def"
#undef AGENT_CONTRACT
};

static const size_t g_agent_contract_count =
    sizeof(g_agent_contracts) / sizeof(g_agent_contracts[0]);

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
    json_push_kv_str(&obj, "name", name && name[0] ? name : c->capability);
    json_push_kv_str(&obj, "method", c->method);
    json_push_kv_str(&obj, "schema", c->schema);
    json_push_kv_str(&obj, "native", c->native_command);
    json_push_kv_str(&obj, "mcp", c->mcp_tool);
    json_push_kv_str(&obj, "purpose",
                     purpose_override && purpose_override[0]
                         ? purpose_override : c->purpose);
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
