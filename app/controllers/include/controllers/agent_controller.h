/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_AGENT_H
#define ZCL_CONTROLLERS_AGENT_H

#include <stdbool.h>

struct json_value;

bool rpc_agent_map(const struct json_value *params, bool help,
                   struct json_value *result);
bool rpc_agent_impact(const struct json_value *params, bool help,
                      struct json_value *result);
bool rpc_agent_contracts(const struct json_value *params, bool help,
                         struct json_value *result);
bool rpc_agent_build(const struct json_value *params, bool help,
                     struct json_value *result);

#endif
