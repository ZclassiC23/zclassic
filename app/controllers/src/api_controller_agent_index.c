/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Agent-facing fragments of the self-describing REST API index. */

#include "api_controller_internal.h"

#include "controllers/agent_controller.h"

#include "json/json.h"

void api_rest_index_mcp_json(struct json_value *mcp)
{
    json_set_object(mcp);
    agent_push_contract_api_mcp_fields_json(mcp);
    json_push_kv_str(mcp, "milestone_tool", "zcl_milestone");
    json_push_kv_str(mcp, "refold_tool", "zcl_refold_status");
    json_push_kv_str(mcp, "drilldown_tool", "zcl_status");
}

void api_rest_index_cli_json(struct json_value *cli)
{
    json_set_object(cli);
    agent_push_contract_api_cli_fields_json(cli);
    json_push_kv_str(cli, "milestone_command", "zclassic23 milestone");
    json_push_kv_str(cli, "refold_command", "zclassic23 refold");
    json_push_kv_str(cli, "drilldown_command", "zclassic23 healthcheck");
}
