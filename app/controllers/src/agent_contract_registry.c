/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/agent_controller.h"

#include "json/json.h"

#include <string.h>

static const struct agent_contract g_agent_contracts[] = {
#define AGENT_CONTRACT(method, capability, schema, native, mcp, rest, purpose) \
    { method, capability, schema, native, mcp, rest, purpose },
#include "controllers/agent_contracts.def"
#undef AGENT_CONTRACT
};

static const size_t g_agent_contract_count =
    sizeof(g_agent_contracts) / sizeof(g_agent_contracts[0]);

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
