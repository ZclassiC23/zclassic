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
    agent_push_contract_mcp_field_json(mcp, "first_tool", "agent");
    agent_push_contract_mcp_field_json(mcp, "map_tool", "agentmap");
    agent_push_contract_mcp_field_json(mcp, "lanes_tool", "agentlanes");
    agent_push_contract_mcp_field_json(mcp, "liveness_tool",
                                       "agentliveness");
    agent_push_contract_mcp_field_json(mcp, "diagnose_tool",
                                       "agentdiagnose");
    agent_push_contract_mcp_field_json(mcp, "impact_tool", "agentimpact");
    agent_push_contract_mcp_field_json(mcp, "contracts_tool",
                                       "agentcontracts");
    agent_push_contract_mcp_field_json(mcp, "build_tool", "agentbuild");
    agent_push_contract_mcp_field_json(mcp, "interface_tool",
                                       "agentinterface");
    agent_push_contract_mcp_field_json(mcp, "ops_tool", "agentops");
    agent_push_contract_mcp_field_json(mcp, "state_catalog_tool",
                                       "statecatalog");
    agent_push_contract_mcp_field_json(mcp, "timeline_tool", "timeline");
    agent_push_contract_mcp_field_json(mcp, "deploy_guard_tool",
                                       "agentdeployguard");
    agent_push_contract_mcp_field_json(mcp, "mirror_tool",
                                       "getmirrorstatus");
    json_push_kv_str(mcp, "milestone_tool", "zcl_milestone");
    json_push_kv_str(mcp, "refold_tool", "zcl_refold_status");
    json_push_kv_str(mcp, "drilldown_tool", "zcl_status");
}

void api_rest_index_cli_json(struct json_value *cli)
{
    json_set_object(cli);
    agent_push_contract_native_field_json(cli, "api_command", "api");
    agent_push_contract_native_field_json(cli, "first_command", "agent");
    agent_push_contract_native_field_json(cli, "map_command", "agentmap");
    agent_push_contract_native_field_json(cli, "lanes_command",
                                          "agentlanes");
    agent_push_contract_native_field_json(cli, "liveness_command",
                                          "agentliveness");
    agent_push_contract_native_field_json(cli, "diagnose_command",
                                          "agentdiagnose");
    agent_push_contract_native_field_json(cli, "impact_command",
                                          "agentimpact");
    agent_push_contract_native_field_json(cli, "contracts_command",
                                          "agentcontracts");
    agent_push_contract_native_field_json(cli, "build_command",
                                          "agentbuild");
    agent_push_contract_native_field_json(cli, "interface_command",
                                          "agentinterface");
    agent_push_contract_native_field_json(cli, "ops_command", "agentops");
    agent_push_contract_native_field_json(cli, "state_catalog_command",
                                          "statecatalog");
    agent_push_contract_native_field_json(cli, "timeline_command",
                                          "timeline");
    agent_push_contract_native_field_json(cli, "deploy_guard_command",
                                          "agentdeployguard");
    agent_push_contract_native_field_json(cli, "mirror_command",
                                          "getmirrorstatus");
    json_push_kv_str(cli, "milestone_command", "zclassic23 milestone");
    json_push_kv_str(cli, "refold_command", "zclassic23 refold");
    json_push_kv_str(cli, "drilldown_command", "zclassic23 healthcheck");
}
