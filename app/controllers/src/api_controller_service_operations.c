/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Operation-level contracts for the sovereign service catalog. Keeping these
 * separate from service-level contracts keeps the catalog renderer small and
 * makes new CRUD operations easier to review. */

#include "api_controller_internal.h"

#include "json/json.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct api_service_operation_contract {
    const char *service_name;
    const char *operation;
    const char *crud_capability;
    const char *status;
    const char *rest_method;
    const char *rest_route;
    const char *rpc_method;
    const char *mcp_tool;
    const char *input_contract;
    const char *output_schema;
    const char *authority;
    const char *effect;
    bool public_read;
    bool operator_private;
    bool destructive;
};

static const struct api_service_operation_contract k_api_service_operations[] = {
    {
        .service_name = "full_node",
        .operation = "read_status",
        .crud_capability = "read_singleton",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/agent",
        .rpc_method = "agent",
        .mcp_tool = "zcl_agent",
        .input_contract = "none",
        .output_schema = ZCL_PUBLIC_STATUS_SCHEMA,
        .authority = "public_projection",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "bootstrap",
        .operation = "read_bootstrap_status",
        .crud_capability = "read_singleton",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/bootstrap",
        .rpc_method = "bootstrapstatus",
        .mcp_tool = "zcl_bootstrapstatus",
        .input_contract = "none",
        .output_schema = "zcl.bootstrap_status.v1",
        .authority = "public_projection",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "bootstrap",
        .operation = "inspect_peer_bootstrap_readiness",
        .crud_capability = "read_collection",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/peers",
        .rpc_method = "peerincidents",
        .mcp_tool = "zcl_peer_incidents",
        .input_contract = "none",
        .output_schema = "zcl.peer_incidents.v1",
        .authority = "operator_diagnostics",
        .effect = "read_only",
        .operator_private = true,
    },
    {
        .service_name = "znam_names",
        .operation = "list_names",
        .crud_capability = "read_collection",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/names",
        .rpc_method = "name_list",
        .mcp_tool = "zcl_name_list",
        .input_contract = "optional_owner_filter",
        .output_schema = "zcl.names.index.v1",
        .authority = "confirmed_chain_projection",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "znam_names",
        .operation = "resolve_name",
        .crud_capability = "read_item",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/names/{name}",
        .rpc_method = "name_resolve",
        .mcp_tool = "zcl_name_resolve",
        .input_contract = "name",
        .output_schema = "zcl.names.show.v1",
        .authority = "confirmed_chain_projection",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "znam_names",
        .operation = "construct_name_register",
        .crud_capability = "construct_transaction",
        .status = "active",
        .rpc_method = "name_register",
        .mcp_tool = "zcl_name_register",
        .input_contract = "name,type,value",
        .output_schema = "zcl.names.register_result.v1",
        .authority = "operator_wallet_transaction",
        .effect = "construct_or_broadcast_znam_op_return_transaction",
        .operator_private = true,
        .destructive = true,
    },
    {
        .service_name = "onion_directory",
        .operation = "list_onion_announcements",
        .crud_capability = "read_collection",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/onion/announcements",
        .input_contract = "limit",
        .output_schema = "zcl.onion_announcements.index.v1",
        .authority = "public_projection",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "onion_directory",
        .operation = "inspect_onion_status",
        .crud_capability = "read_singleton",
        .status = "active",
        .rpc_method = "healthcheck",
        .mcp_tool = "zcl_onion_status",
        .input_contract = "none",
        .output_schema = "zcl.healthcheck.v1",
        .authority = "operator_diagnostics",
        .effect = "read_only",
        .operator_private = true,
    },
    {
        .service_name = "file_services",
        .operation = "list_file_services",
        .crud_capability = "read_collection",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/file-services",
        .rpc_method = "zmarket_list",
        .mcp_tool = "zcl_market_list",
        .input_contract = "limit",
        .output_schema = "zcl.file_services.index.v1",
        .authority = "public_projection",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "file_services",
        .operation = "read_file_by_sha3",
        .crud_capability = "read_item",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/files/{sha3}",
        .input_contract = "sha3",
        .output_schema = "zcl.files.show.v1",
        .authority = "payment_or_allowlist_gate",
        .effect = "streams_hash_addressed_content_when_authorized",
        .public_read = true,
        .operator_private = true,
    },
    {
        .service_name = "market",
        .operation = "list_market",
        .crud_capability = "read_collection",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/market",
        .rpc_method = "zmarket_list",
        .mcp_tool = "zcl_market_list",
        .input_contract = "none",
        .output_schema = "zcl.market.index.v1",
        .authority = "public_projection",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "market",
        .operation = "create_market_offer",
        .crud_capability = "create_offer",
        .status = "active",
        .rpc_method = "zmarket_offer",
        .mcp_tool = "zcl_market_offer",
        .input_contract = "filepath,price_per_mb_zat,z_addr",
        .output_schema = "zcl.market.offer_result.v1",
        .authority = "operator_local_file_and_wallet",
        .effect = "announces_signed_file_offer",
        .operator_private = true,
        .destructive = true,
    },
    {
        .service_name = "market",
        .operation = "create_market_purchase",
        .crud_capability = "create_purchase",
        .status = "active",
        .rpc_method = "zmarket_buy",
        .mcp_tool = "zcl_market_buy",
        .input_contract = "root_hash,output_path",
        .output_schema = "zcl.market.buy_result.v1",
        .authority = "operator_wallet_payment",
        .effect = "initiates_payment_gated_download",
        .operator_private = true,
        .destructive = true,
    },
    {
        .service_name = "messaging",
        .operation = "read_inbox",
        .crud_capability = "read_collection",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/messages",
        .rpc_method = "msg_inbox",
        .mcp_tool = "zcl_msg_inbox",
        .input_contract = "include_read",
        .output_schema = "zcl.messages.index.v1",
        .authority = "operator_local_state",
        .effect = "read_only",
        .operator_private = true,
    },
    {
        .service_name = "messaging",
        .operation = "send_peer_message",
        .crud_capability = "create_message",
        .status = "active",
        .rpc_method = "msg_send",
        .mcp_tool = "zcl_msg_send",
        .input_contract = "peer_id,message",
        .output_schema = "zcl.messages.send_result.v1",
        .authority = "operator_p2p_send",
        .effect = "sends_plaintext_p2p_message",
        .operator_private = true,
        .destructive = true,
    },
    {
        .service_name = "messaging",
        .operation = "send_named_message",
        .crud_capability = "create_message",
        .status = "active",
        .rpc_method = "msg_send_named",
        .mcp_tool = "zcl_msg_send_named",
        .input_contract = "name,message",
        .output_schema = "zcl.messages.send_result.v1",
        .authority = "operator_znam_resolution_and_p2p_send",
        .effect = "resolves_name_then_sends_plaintext_p2p_message",
        .operator_private = true,
        .destructive = true,
    },
    {
        .service_name = "script_contracts",
        .operation = "list_swap_chains",
        .crud_capability = "read_capabilities",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/swaps/chains",
        .rpc_method = "swap_chains",
        .mcp_tool = "zcl_swap_chains",
        .input_contract = "none",
        .output_schema = "zcl.swaps.chains.v1",
        .authority = "static_contract_registry",
        .effect = "read_only",
        .public_read = true,
    },
    {
        .service_name = "script_contracts",
        .operation = "construct_swap_initiate",
        .crud_capability = "construct_contract",
        .status = "in_progress",
        .rpc_method = "swap_initiate",
        .mcp_tool = "zcl_swap_initiate",
        .input_contract = "my_address,counter_address,amount,locktime,chain",
        .output_schema = "zcl.swaps.contract.v1",
        .authority = "operator_script_contract_builder",
        .effect = "constructs_htlc_redeem_script_and_contract_row",
        .operator_private = true,
        .destructive = true,
    },
    {
        .service_name = "script_contracts",
        .operation = "construct_swap_participate",
        .crud_capability = "construct_contract",
        .status = "in_progress",
        .rpc_method = "swap_participate",
        .mcp_tool = "zcl_swap_participate",
        .input_contract =
            "my_address,counter_address,amount,locktime,secret_hash,chain",
        .output_schema = "zcl.swaps.contract.v1",
        .authority = "operator_script_contract_builder",
        .effect = "constructs_counterparty_htlc_contract_row",
        .operator_private = true,
        .destructive = true,
    },
    {
        .service_name = "events",
        .operation = "read_events",
        .crud_capability = "read_collection",
        .status = "active",
        .rest_method = "GET",
        .rest_route = "/api/v1/events",
        .rpc_method = "eventlog",
        .mcp_tool = "zcl_events",
        .input_contract = "count,type",
        .output_schema = "zcl.event_log.v1",
        .authority = "operator_diagnostics",
        .effect = "read_only",
        .operator_private = true,
    },
    {
        .service_name = "events",
        .operation = "read_timeline",
        .crud_capability = "read_collection",
        .status = "active",
        .rpc_method = "timeline",
        .mcp_tool = "zcl_timeline",
        .input_contract = "category,count,since_secs",
        .output_schema = "zcl.timeline.v1",
        .authority = "operator_diagnostics",
        .effect = "read_only",
        .operator_private = true,
    },
};

static size_t api_service_operation_count(void)
{
    return sizeof(k_api_service_operations) /
           sizeof(k_api_service_operations[0]);
}

static bool api_service_operation_matches(
    const struct api_service_operation_contract *op,
    const char *service_name)
{
    if (!op)
        return false;
    if (!service_name || !service_name[0])
        return true;
    return strcmp(op->service_name, service_name) == 0;
}

static void api_service_operation_id(
    char *buf,
    size_t buf_len,
    const struct api_service_operation_contract *op)
{
    if (!buf || buf_len == 0)
        return;

    snprintf(buf, buf_len, "%s.%s",
             op && op->service_name ? op->service_name : "",
             op && op->operation ? op->operation : "");
    buf[buf_len - 1] = '\0';
}

static const char *api_service_operation_agent_interface(
    const struct api_service_operation_contract *op)
{
    if (!op)
        return "native_or_planned";

    if ((op->operator_private || op->destructive) &&
        op->mcp_tool && op->mcp_tool[0])
        return "mcp";
    if (op->public_read && op->rest_method && op->rest_method[0] &&
        op->rest_route && op->rest_route[0])
        return "rest";
    if (op->mcp_tool && op->mcp_tool[0])
        return "mcp";
    if (op->rpc_method && op->rpc_method[0])
        return "rpc";
    if (op->rest_method && op->rest_method[0] &&
        op->rest_route && op->rest_route[0])
        return "rest";
    return "native_or_planned";
}

static const char *api_service_operation_agent_next_step(
    const struct api_service_operation_contract *op)
{
    const char *iface = api_service_operation_agent_interface(op);

    if (!op)
        return "inspect_operation_contract";
    if (op->destructive)
        return "review_destructive_write_safety_then_call_mcp_tool";
    if (strcmp(iface, "rest") == 0)
        return "call_rest_route_and_validate_output_schema";
    if (strcmp(iface, "mcp") == 0)
        return "call_mcp_tool_with_operator_context";
    if (strcmp(iface, "rpc") == 0)
        return "call_rpc_method_with_explicit_datadir_or_port";
    return "inspect_native_or_planned_contract";
}

static const struct api_service_operation_contract *
api_service_operation_lookup_id(const char *operation_id)
{
    char id[128];

    if (!operation_id || !operation_id[0])
        return NULL;

    for (size_t i = 0; i < api_service_operation_count(); i++) {
        api_service_operation_id(id, sizeof(id),
                                 &k_api_service_operations[i]);
        if (strcmp(id, operation_id) == 0)
            return &k_api_service_operations[i];
    }

    return NULL;
}

static void api_service_operation_json(
    struct json_value *obj,
    const struct api_service_operation_contract *op)
{
    char operation_id[128];
    char service_route[160];
    char self_route[192];
    const char *agent_interface;

    if (!obj || !op)
        return;

    api_service_operation_id(operation_id, sizeof(operation_id), op);
    snprintf(service_route, sizeof(service_route),
             "/api/v1/service-catalog/%s", op->service_name);
    service_route[sizeof(service_route) - 1] = '\0';
    snprintf(self_route, sizeof(self_route),
             "/api/v1/service-operations/%s", operation_id);
    self_route[sizeof(self_route) - 1] = '\0';
    agent_interface = api_service_operation_agent_interface(op);

    json_set_object(obj);
    json_push_kv_str(obj, "schema", ZCL_SERVICE_OPERATION_SCHEMA);
    json_push_kv_str(obj, "operation_id", operation_id);
    json_push_kv_str(obj, "self_route", self_route);
    json_push_kv_str(obj, "service_catalog_route", service_route);
    json_push_kv_str(obj, "service", op->service_name);
    json_push_kv_str(obj, "operation", op->operation);
    json_push_kv_str(obj, "crud_capability", op->crud_capability);
    json_push_kv_str(obj, "status", op->status);
    if (op->rest_method && op->rest_method[0])
        json_push_kv_str(obj, "rest_method", op->rest_method);
    if (op->rest_route && op->rest_route[0])
        json_push_kv_str(obj, "rest_route", op->rest_route);
    if (op->rpc_method && op->rpc_method[0])
        json_push_kv_str(obj, "rpc_method", op->rpc_method);
    if (op->mcp_tool && op->mcp_tool[0])
        json_push_kv_str(obj, "mcp_tool", op->mcp_tool);
    json_push_kv_str(obj, "input_contract", op->input_contract);
    json_push_kv_str(obj, "output_schema", op->output_schema);
    json_push_kv_str(obj, "authority", op->authority);
    json_push_kv_str(obj, "effect", op->effect);
    json_push_kv_str(obj, "execution_surface",
                     op->rest_method && op->rest_method[0]
                         ? "rest"
                         : op->mcp_tool && op->mcp_tool[0]
                               ? "mcp_or_native"
                               : "native_or_planned");
    json_push_kv_str(obj, "write_safety",
                     op->destructive
                         ? "operator_private_destructive"
                         : op->operator_private ? "operator_private"
                                                : "public_read_only");
    json_push_kv_str(obj, "agent_preferred_interface", agent_interface);
    json_push_kv_str(obj, "agent_next_step",
                     api_service_operation_agent_next_step(op));
    json_push_kv_bool(obj, "rest_callable",
                      op->rest_method && op->rest_method[0] &&
                      op->rest_route && op->rest_route[0]);
    json_push_kv_bool(obj, "mcp_callable",
                      op->mcp_tool && op->mcp_tool[0]);
    json_push_kv_bool(obj, "rpc_callable",
                      op->rpc_method && op->rpc_method[0]);
    json_push_kv_bool(obj, "public_read", op->public_read);
    json_push_kv_bool(obj, "operator_private", op->operator_private);
    json_push_kv_bool(obj, "destructive", op->destructive);
}

void api_service_operations_json(struct json_value *out,
                                 const char *service_name)
{
    json_set_array(out);
    for (size_t i = 0; i < api_service_operation_count(); i++) {
        struct json_value op;
        if (!api_service_operation_matches(&k_api_service_operations[i],
                                           service_name))
            continue;
        json_init(&op);
        api_service_operation_json(&op, &k_api_service_operations[i]);
        json_push_back(out, &op);
        json_free(&op);
    }
}

bool api_service_operations_index_json(struct json_value *out)
{
    struct json_value operations;

    if (!out)
        return false;

    json_set_object(out);
    json_push_kv_str(out, "schema", ZCL_SERVICE_OPERATIONS_INDEX_SCHEMA);
    json_push_kv_str(out, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(out, "catalog_route", "/api/v1/service-catalog");
    json_push_kv_str(out, "service_member_route",
                     "/api/v1/service-catalog/{service}");
    json_push_kv_str(out, "member_route",
                     "/api/v1/service-operations/{operation_id}");
    json_push_kv_str(out, "operation_schema",
                     ZCL_SERVICE_OPERATION_SCHEMA);
    json_push_kv_str(out, "base_layer", "zclassic_l1");
    json_push_kv_str(out, "service_layer",
                     "zclassic23_application_layer");
    json_push_kv_str(out, "filter_model",
                     "service-specific operation subsets are embedded in "
                     "/api/v1/service-catalog/{service}");

    json_init(&operations);
    api_service_operations_json(&operations, NULL);
    json_push_kv(out, "operations", &operations);
    json_push_kv_int(out, "operation_count",
                     (int64_t)json_size(&operations));
    json_free(&operations);

    return true;
}

bool api_service_operation_show_json(const char *operation_id,
                                     struct json_value *out)
{
    const struct api_service_operation_contract *op =
        api_service_operation_lookup_id(operation_id);

    if (!out || !op)
        return false;

    api_service_operation_json(out, op);
    json_push_kv_str(out, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(out, "catalog_route", "/api/v1/service-catalog");
    json_push_kv_str(out, "operation_route",
                     "/api/v1/service-operations/{operation_id}");
    json_push_kv_str(out, "base_layer", "zclassic_l1");
    json_push_kv_str(out, "service_layer", "zclassic23_application_layer");
    return true;
}
