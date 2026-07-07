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
    const char *depends_on_services_csv;
    const char *read_model;
    const char *write_model;
    const char *verified_by;
    const char *trust_model;
    const char *privacy_model;
    const char *user_story;
    bool public_read;
    bool operator_private_write;
};

struct api_service_operation_summary_counts {
    int64_t operation_count;
    int64_t public_read_count;
    int64_t operator_private_count;
    int64_t destructive_count;
    int64_t rest_callable_count;
    int64_t mcp_callable_count;
    int64_t rpc_callable_count;
    int64_t active_count;
    int64_t in_progress_count;
    int64_t preferred_rest_count;
    int64_t preferred_mcp_count;
    int64_t preferred_rpc_count;
    int64_t preferred_native_count;
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
        .depends_on_services_csv = "",
        .read_model = "reducer_frontier_and_node_health_projection",
        .write_model = "consensus_state_is_chain_derived_no_public_writes",
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
        .depends_on_services_csv = "full_node",
        .read_model = "network_bootstrap_status_and_peer_projection",
        .write_model =
            "runtime_peer_connection_attempts_and_snapshot_offer_state",
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
        .depends_on_services_csv = "full_node",
        .read_model = "znam_projection_confirmed_chain_records",
        .write_model = "construct_znam_op_return_transactions",
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
        .depends_on_services_csv = "full_node,bootstrap,znam_names",
        .read_model = "onion_announcement_projection_and_peer_lifecycle",
        .write_model = "local_node_announces_runtime_endpoints_when_enabled",
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
        .depends_on_services_csv = "full_node,onion_directory,market",
        .read_model = "file_manifest_and_offer_projection",
        .write_model = "operator_hosts_or_gates_hash_addressed_content",
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
        .depends_on_services_csv = "full_node,znam_names,file_services",
        .read_model = "market_offer_projection",
        .write_model = "operator_creates_signed_offer_or_purchase_flow",
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
        .depends_on_services_csv = "full_node,znam_names,onion_directory",
        .read_model = "local_message_projection",
        .write_model = "operator_sends_p2p_message_or_future_sapling_memo",
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
        .depends_on_services_csv = "full_node,znam_names",
        .read_model = "swap_contract_projection_and_static_script_registry",
        .write_model = "operator_constructs_standard_script_contracts",
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
        .depends_on_services_csv = "full_node",
        .read_model = "append_only_event_log_projection",
        .write_model = "node_subsystems_append_events_no_public_writes",
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

static size_t api_service_count(void)
{
    return sizeof(k_api_services) / sizeof(k_api_services[0]);
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

static void api_service_operation_summary_count(
    const struct json_value *op,
    struct api_service_operation_summary_counts *counts)
{
    const char *status;
    const char *iface;

    if (!op || !counts)
        return;

    status = json_get_str(json_get(op, "status"));
    iface = json_get_str(json_get(op, "agent_preferred_interface"));

    counts->operation_count++;
    if (json_get_bool(json_get(op, "public_read")))
        counts->public_read_count++;
    if (json_get_bool(json_get(op, "operator_private")))
        counts->operator_private_count++;
    if (json_get_bool(json_get(op, "destructive")))
        counts->destructive_count++;
    if (json_get_bool(json_get(op, "rest_callable")))
        counts->rest_callable_count++;
    if (json_get_bool(json_get(op, "mcp_callable")))
        counts->mcp_callable_count++;
    if (json_get_bool(json_get(op, "rpc_callable")))
        counts->rpc_callable_count++;
    if (strcmp(status, "active") == 0)
        counts->active_count++;
    if (strcmp(status, "in_progress") == 0)
        counts->in_progress_count++;
    if (strcmp(iface, "rest") == 0)
        counts->preferred_rest_count++;
    else if (strcmp(iface, "mcp") == 0)
        counts->preferred_mcp_count++;
    else if (strcmp(iface, "rpc") == 0)
        counts->preferred_rpc_count++;
    else
        counts->preferred_native_count++;
}

static void api_service_operation_summary_json(
    const struct json_value *operations,
    struct json_value *summary)
{
    struct api_service_operation_summary_counts counts = {0};

    if (!summary)
        return;

    for (size_t i = 0; i < json_size(operations); i++)
        api_service_operation_summary_count(json_at(operations, i),
                                            &counts);

    json_set_object(summary);
    json_push_kv_int(summary, "operation_count", counts.operation_count);
    json_push_kv_int(summary, "public_read_count",
                     counts.public_read_count);
    json_push_kv_int(summary, "operator_private_count",
                     counts.operator_private_count);
    json_push_kv_int(summary, "destructive_count",
                     counts.destructive_count);
    json_push_kv_int(summary, "rest_callable_count",
                     counts.rest_callable_count);
    json_push_kv_int(summary, "mcp_callable_count",
                     counts.mcp_callable_count);
    json_push_kv_int(summary, "rpc_callable_count",
                     counts.rpc_callable_count);
    json_push_kv_int(summary, "active_count", counts.active_count);
    json_push_kv_int(summary, "in_progress_count",
                     counts.in_progress_count);
    json_push_kv_int(summary, "preferred_rest_count",
                     counts.preferred_rest_count);
    json_push_kv_int(summary, "preferred_mcp_count",
                     counts.preferred_mcp_count);
    json_push_kv_int(summary, "preferred_rpc_count",
                     counts.preferred_rpc_count);
    json_push_kv_int(summary, "preferred_native_count",
                     counts.preferred_native_count);
}

static void api_service_object_json(struct json_value *obj,
                                    const struct api_service_contract *svc)
{
    struct json_value crud;
    struct json_value transports;
    struct json_value object_types;
    struct json_value dependencies;
    struct json_value operations;
    struct json_value operation_summary;
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

    json_init(&dependencies);
    api_app_protocol_csv_json(svc->depends_on_services_csv, &dependencies);
    json_push_kv(obj, "depends_on_services", &dependencies);
    json_free(&dependencies);

    json_init(&operations);
    api_service_operations_json(&operations, svc->name);
    json_init(&operation_summary);
    api_service_operation_summary_json(&operations, &operation_summary);
    json_push_kv(obj, "operation_summary", &operation_summary);
    json_free(&operation_summary);
    json_push_kv(obj, "operations", &operations);
    json_push_kv_int(obj, "operation_count",
                     (int64_t)json_size(&operations));
    json_free(&operations);

    json_push_kv_str(obj, "verified_by", svc->verified_by);
    json_push_kv_str(obj, "read_model", svc->read_model);
    json_push_kv_str(obj, "write_model", svc->write_model);
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
    json_push_kv_str(out, "operation_collection_route",
                     "/api/v1/service-operations");
    json_push_kv_str(out, "operation_route",
                     "/api/v1/service-operations/{operation_id}");
    json_push_kv_str(out, "operation_schema", ZCL_SERVICE_OPERATION_SCHEMA);
    json_push_kv_str(out, "consensus_boundary",
                     "services interpret, index, advertise, or construct "
                     "valid ZCL data without changing legacy consensus");
    json_push_kv_str(out, "crud_contract",
                     "CRUD writes are transaction builders or operator-local "
                     "state changes; public reads are projection snapshots");

    struct json_value ux;
    json_init(&ux);
    api_sovereign_ux_contract_json(&ux);
    json_push_kv(out, "sovereign_ux", &ux);
    json_free(&ux);

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
    json_push_kv_str(out, "operation_collection_route",
                     "/api/v1/service-operations");
    json_push_kv_str(out, "operation_route",
                     "/api/v1/service-operations/{operation_id}");
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
