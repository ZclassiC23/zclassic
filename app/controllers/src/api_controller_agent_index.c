/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Agent-facing fragments of the self-describing REST API index. */

#include "api_controller_internal.h"

#include "json/json.h"

void api_rest_index_mcp_json(struct json_value *mcp)
{
    json_set_object(mcp);
    json_push_kv_str(mcp, "first_tool", "zcl_agent");
    json_push_kv_str(mcp, "map_tool", "zcl_agent_map");
    json_push_kv_str(mcp, "impact_tool", "zcl_agent_impact");
    json_push_kv_str(mcp, "contracts_tool", "zcl_agent_contracts");
    json_push_kv_str(mcp, "build_tool", "zcl_agent_build");
    json_push_kv_str(mcp, "interface_tool", "zcl_agent_interface");
    json_push_kv_str(mcp, "deploy_guard_tool", "zcl_agent_deploy_guard");
    json_push_kv_str(mcp, "milestone_tool", "zcl_milestone");
    json_push_kv_str(mcp, "refold_tool", "zcl_refold_status");
    json_push_kv_str(mcp, "drilldown_tool", "zcl_status");
}

void api_rest_index_cli_json(struct json_value *cli)
{
    json_set_object(cli);
    json_push_kv_str(cli, "api_command", "zclassic23 api");
    json_push_kv_str(cli, "first_command", "zclassic23 agent");
    json_push_kv_str(cli, "map_command", "zclassic23 agentmap");
    json_push_kv_str(cli, "impact_command", "zclassic23 agentimpact <files...>");
    json_push_kv_str(cli, "contracts_command", "zclassic23 agentcontracts");
    json_push_kv_str(cli, "build_command", "zclassic23 agentbuild");
    json_push_kv_str(cli, "interface_command", "zclassic23 agentinterface");
    json_push_kv_str(cli, "deploy_guard_command",
                     "zclassic23 agentdeployguard [action]");
    json_push_kv_str(cli, "milestone_command", "zclassic23 milestone");
    json_push_kv_str(cli, "refold_command", "zclassic23 refold");
    json_push_kv_str(cli, "drilldown_command", "zclassic23 healthcheck");
}
