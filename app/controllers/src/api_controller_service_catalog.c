/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* UX-oriented service catalog for sovereign-node clients. Runtime health stays
 * in /api/v1/services; this catalog declares what the node can host, advertise,
 * or verify and how those capabilities map to REST/native/MCP surfaces. */

#include "api_controller_internal.h"

#include "json/json.h"

#include <stdio.h>
#include <string.h>

struct api_service_contract {
    const char *name;
    const char *status;
    const char *category;
    const char *application_protocol;
    const char *rest_collection;
    const char *rest_item;
    const char *runtime_health_route;
    const char *crud_capabilities_csv;
    const char *transports_csv;
    const char *object_types_csv;
    const char *verified_by;
    const char *trust_model;
    const char *privacy_model;
    const char *user_story;
    bool public_read;
    bool operator_private_write;
};

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

static const struct api_service_contract k_api_services[] = {
    {
        .name = "full_node",
        .status = "active",
        .category = "base_node",
        .application_protocol = "",
        .rest_collection = "/api/v1/agent",
        .rest_item = "",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_singleton",
        .transports_csv = "rest,mcp,native,p2p",
        .object_types_csv = "block_header,block,transaction,peer",
        .verified_by =
            "zclassic_l1_consensus_pow_headers_scripts_and_utxo_frontier",
        .trust_model =
            "local_full_node_validation_is_authoritative_external_sources_are_advisory",
        .privacy_model = "public_chain_status_no_wallet_material",
        .user_story =
            "operator opens one status page or agent call and sees whether the node is serving the verified ZCL frontier",
        .public_read = true,
        .operator_private_write = false,
    },
    {
        .name = "bootstrap",
        .status = "active",
        .category = "network_bootstrap",
        .application_protocol = "",
        .rest_collection = "/api/v1/bootstrap",
        .rest_item = "",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_singleton",
        .transports_csv = "rest,mcp,native,p2p",
        .object_types_csv = "bootstrap_status,snapshot_offer,peer_capability",
        .verified_by =
            "pow_header_chain_served_frontier_utxo_snapshot_commitment_when_present",
        .trust_model =
            "fresh_peers_may_use_zclassic23_nodes_as_bootstrap_helpers_without_changing_consensus_authority",
        .privacy_model = "public_peer_capabilities_and_snapshot_metadata",
        .user_story =
            "fresh node asks whether this peer can help it bootstrap quickly and which path is safe",
        .public_read = true,
        .operator_private_write = false,
    },
    {
        .name = "znam_names",
        .status = "active",
        .category = "identity",
        .application_protocol = "znam",
        .rest_collection = "/api/v1/names",
        .rest_item = "/api/v1/names/{name}",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_collection,read_item,construct_transaction",
        .transports_csv = "rest,mcp,native,chain",
        .object_types_csv = "name_record,service_record,endpoint_record,text_record",
        .verified_by = "confirmed_op_return_bytes_in_valid_zcl_transactions",
        .trust_model =
            "names_are_chain_projected_records_rebuildable_from_confirmed_blocks",
        .privacy_model = "public_name_records_use_dedicated_identity_keys",
        .user_story =
            "user resolves a human ZCL name into addresses, services, and onion endpoints verified from chain data",
        .public_read = true,
        .operator_private_write = true,
    },
    {
        .name = "onion_directory",
        .status = "active",
        .category = "tor_p2p_discovery",
        .application_protocol = "znam",
        .rest_collection = "/api/v1/onion/announcements",
        .rest_item = "",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_collection",
        .transports_csv = "rest,onion,p2p",
        .object_types_csv = "onion_announcement,clearnet_endpoint,service_hint",
        .verified_by =
            "node_projection_plus_peer_handshake_height_and_service_capabilities",
        .trust_model =
            "onion_records_help_discovery_direct_p2p_still_requires_normal_peer_validation",
        .privacy_model = "public_reachability_metadata_no_private_keys",
        .user_story =
            "remote node discovers an onion address and then prefers direct P2P after validating peer identity and height",
        .public_read = true,
        .operator_private_write = false,
    },
    {
        .name = "file_services",
        .status = "active",
        .category = "content_hosting",
        .application_protocol = "market",
        .rest_collection = "/api/v1/file-services",
        .rest_item = "/api/v1/files/{sha3}",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_collection,read_item",
        .transports_csv = "rest,onion,p2p",
        .object_types_csv = "content_hash,file_manifest,chunk,mirror",
        .verified_by = "sha3_content_hash_and_payment_or_allowlist_gate",
        .trust_model =
            "content_is_hash_addressed_chain_or_payment_records_authorize_access",
        .privacy_model =
            "operators_choose_what_to_host_chain_stores_commitments_not_file_bytes",
        .user_story =
            "buyer downloads bytes from a node only after the content hash and payment gate match",
        .public_read = true,
        .operator_private_write = true,
    },
    {
        .name = "market",
        .status = "active",
        .category = "commerce",
        .application_protocol = "market",
        .rest_collection = "/api/v1/market",
        .rest_item = "",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_collection,create_offer,create_purchase",
        .transports_csv = "rest,onion,p2p,chain",
        .object_types_csv =
            "signed_listing,buy_intent,payment_receipt,content_descriptor",
        .verified_by =
            "signed_listing_hashes_shielded_payment_memo_and_manifest_hash",
        .trust_model =
            "market_flows_are_application_layer_records_settled_by_valid_zcl_payments",
        .privacy_model =
            "public_listings_private_payment_content_hosting_is_explicit",
        .user_story =
            "seller publishes a listing and buyer pays with ZCL before receiving content",
        .public_read = true,
        .operator_private_write = true,
    },
    {
        .name = "messaging",
        .status = "in_progress",
        .category = "communication",
        .application_protocol = "messaging",
        .rest_collection = "/api/v1/messages",
        .rest_item = "",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_collection,create_message",
        .transports_csv = "mcp,native,p2p,planned_sapling_memo",
        .object_types_csv = "p2p_message,delivery_receipt,planned_memo_message",
        .verified_by =
            "local_delivery_events_today_planned_sapling_memo_commitments",
        .trust_model =
            "current_p2p_messages_are_local_state_future_memos_rebuild_from_chain",
        .privacy_model =
            "p2p_channel_currently_plaintext_use_planned_shielded_memo_for_private_content",
        .user_story =
            "operator sends node-to-node messages and later can anchor private messages in shielded memos",
        .public_read = false,
        .operator_private_write = true,
    },
    {
        .name = "script_contracts",
        .status = "in_progress",
        .category = "contracts",
        .application_protocol = "script_contracts",
        .rest_collection = "/api/v1/swaps/chains",
        .rest_item = "",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_capabilities,construct_contract",
        .transports_csv = "rest,mcp,native,chain",
        .object_types_csv = "htlc_contract,redeem_script,refund_path",
        .verified_by =
            "legacy_valid_zclassic_script_hashlocks_timelocks_and_signatures",
        .trust_model =
            "contracts_use_existing_zclassic_script_consensus_no_consensus_extension",
        .privacy_model = "transparent_contract_terms_are_public",
        .user_story =
            "developer builds HTLC-style flows using valid ZCL script instead of a consensus fork",
        .public_read = true,
        .operator_private_write = true,
    },
    {
        .name = "events",
        .status = "active",
        .category = "telemetry",
        .application_protocol = "",
        .rest_collection = "/api/v1/events",
        .rest_item = "",
        .runtime_health_route = "/api/v1/services",
        .crud_capabilities_csv = "read_collection",
        .transports_csv = "rest,mcp,native",
        .object_types_csv = "event,incident,timeline_cursor",
        .verified_by = "append_only_node_event_ring_and_projection_state",
        .trust_model =
            "diagnostics_are_operator_observability_not_consensus_authority",
        .privacy_model = "operator_logs_may_include_local_operational_metadata",
        .user_story =
            "agent reads recent semantic events without scraping logs or piping through jq",
        .public_read = true,
        .operator_private_write = false,
    },
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

static size_t api_service_count(void)
{
    return sizeof(k_api_services) / sizeof(k_api_services[0]);
}

static size_t api_service_operation_count(void)
{
    return sizeof(k_api_service_operations) /
           sizeof(k_api_service_operations[0]);
}

static bool api_service_operation_matches(
    const struct api_service_operation_contract *op,
    const char *service_name)
{
    return op && service_name && strcmp(op->service_name, service_name) == 0;
}

static void api_service_operation_json(
    struct json_value *obj,
    const struct api_service_operation_contract *op)
{
    if (!obj || !op)
        return;

    json_set_object(obj);
    json_push_kv_str(obj, "schema", ZCL_SERVICE_OPERATION_SCHEMA);
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
    json_push_kv_bool(obj, "public_read", op->public_read);
    json_push_kv_bool(obj, "operator_private", op->operator_private);
    json_push_kv_bool(obj, "destructive", op->destructive);
}

static void api_service_operations_json(struct json_value *out,
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

static const struct api_service_contract *api_service_lookup(const char *name)
{
    if (!name || !name[0])
        return NULL;

    for (size_t i = 0; i < api_service_count(); i++) {
        if (strcmp(k_api_services[i].name, name) == 0)
            return &k_api_services[i];
    }
    return NULL;
}

static void api_service_names_json(struct json_value *out)
{
    json_set_array(out);
    for (size_t i = 0; i < api_service_count(); i++) {
        struct json_value name;
        json_init(&name);
        json_set_str(&name, k_api_services[i].name);
        json_push_back(out, &name);
        json_free(&name);
    }
}

static void api_service_object_json(struct json_value *obj,
                                    const struct api_service_contract *svc)
{
    struct json_value crud;
    struct json_value transports;
    struct json_value object_types;
    struct json_value operations;
    char self_route[128];

    if (!obj || !svc)
        return;

    json_set_object(obj);
    json_push_kv_str(obj, "schema", ZCL_SERVICE_CONTRACT_SCHEMA);
    json_push_kv_str(obj, "name", svc->name);
    json_push_kv_str(obj, "status", svc->status);
    json_push_kv_str(obj, "category", svc->category);
    if (svc->application_protocol && svc->application_protocol[0])
        json_push_kv_str(obj, "application_protocol",
                         svc->application_protocol);
    json_push_kv_str(obj, "rest_collection", svc->rest_collection);
    if (svc->rest_item && svc->rest_item[0])
        json_push_kv_str(obj, "rest_item", svc->rest_item);
    snprintf(self_route, sizeof(self_route), "/api/v1/service-catalog/%s",
             svc->name);
    json_push_kv_str(obj, "self_route", self_route);
    json_push_kv_str(obj, "runtime_health_route",
                     svc->runtime_health_route);

    json_init(&crud);
    api_app_protocol_csv_json(svc->crud_capabilities_csv, &crud);
    json_push_kv(obj, "crud_capabilities", &crud);
    json_free(&crud);

    json_init(&transports);
    api_app_protocol_csv_json(svc->transports_csv, &transports);
    json_push_kv(obj, "transports", &transports);
    json_free(&transports);

    json_init(&object_types);
    api_app_protocol_csv_json(svc->object_types_csv, &object_types);
    json_push_kv(obj, "object_types", &object_types);
    json_free(&object_types);

    json_init(&operations);
    api_service_operations_json(&operations, svc->name);
    json_push_kv(obj, "operations", &operations);
    json_push_kv_int(obj, "operation_count",
                     (int64_t)json_size(&operations));
    json_free(&operations);

    json_push_kv_str(obj, "verified_by", svc->verified_by);
    json_push_kv_str(obj, "trust_model", svc->trust_model);
    json_push_kv_str(obj, "privacy_model", svc->privacy_model);
    json_push_kv_str(obj, "user_story", svc->user_story);
    json_push_kv_bool(obj, "public_read", svc->public_read);
    json_push_kv_bool(obj, "operator_private_write",
                      svc->operator_private_write);
}

static void api_service_push_json(struct json_value *services,
                                  const struct api_service_contract *svc)
{
    struct json_value obj;

    if (!services || !svc)
        return;

    json_init(&obj);
    api_service_object_json(&obj, svc);
    json_push_back(services, &obj);
    json_free(&obj);
}

bool api_service_catalog_json(struct json_value *out)
{
    if (!out)
        return false;

    json_set_object(out);
    json_push_kv_str(out, "schema", ZCL_SERVICE_CATALOG_SCHEMA);
    json_push_kv_str(out, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(out, "base_layer", "zclassic_l1");
    json_push_kv_str(out, "service_layer", "zclassic23_application_layer");
    json_push_kv_str(out, "runtime_health_route", "/api/v1/services");
    json_push_kv_str(out, "application_protocols_route",
                     "/api/v1/protocols");
    json_push_kv_str(out, "openapi_route", "/api/v1/openapi");
    json_push_kv_str(out, "member_route",
                     "/api/v1/service-catalog/{service}");
    json_push_kv_str(out, "operation_schema", ZCL_SERVICE_OPERATION_SCHEMA);
    json_push_kv_str(out, "consensus_boundary",
                     "services interpret, index, advertise, or construct "
                     "valid ZCL data without changing legacy consensus");
    json_push_kv_str(out, "crud_contract",
                     "CRUD writes are transaction builders or operator-local "
                     "state changes; public reads are projection snapshots");

    struct json_value services;
    json_init(&services);
    json_set_array(&services);
    for (size_t i = 0; i < api_service_count(); i++)
        api_service_push_json(&services, &k_api_services[i]);
    json_push_kv(out, "services", &services);
    json_push_kv_int(out, "service_count", (int64_t)json_size(&services));
    json_free(&services);

    return true;
}

bool api_service_catalog_show_json(const char *name, struct json_value *out)
{
    const struct api_service_contract *svc = api_service_lookup(name);
    if (!out || !svc)
        return false;

    api_service_object_json(out, svc);
    json_push_kv_str(out, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(out, "catalog_route", "/api/v1/service-catalog");
    json_push_kv_str(out, "base_layer", "zclassic_l1");
    json_push_kv_str(out, "service_layer", "zclassic23_application_layer");
    return true;
}

void api_service_catalog_error_json(const char *name, struct json_value *out)
{
    struct json_value names;

    if (!out)
        return;

    json_set_object(out);
    json_push_kv_str(out, "schema", "zcl.service_catalog_error.v1");
    json_push_kv_str(out, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(out, "error", "service_not_found");
    json_push_kv_str(out, "name", name ? name : "");
    json_push_kv_str(out, "catalog_route", "/api/v1/service-catalog");

    json_init(&names);
    api_service_names_json(&names);
    json_push_kv(out, "valid_services", &names);
    json_free(&names);
}
