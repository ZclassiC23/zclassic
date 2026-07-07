/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Canonical application-protocol registry for the REST/OpenAPI contract.
 * These protocols interpret or construct valid ZCL transactions; none of
 * them change consensus rules. */

#include "api_controller_internal.h"

#include "json/json.h"

#include <string.h>

static const struct api_app_protocol_contract k_api_app_protocols[] = {
    {
        .name = "zlsp",
        .status = "design",
        .layer = "zclassic23_application_layer",
        .base_layer = "zclassic_l1",
        .family = "application_protocol_framework",
        .anchor =
            "versioned services that interpret or construct valid ZCL transactions",
        .anchor_kind = "base_layer_transaction_contract",
        .rest_resource = "/api/v1/protocols",
        .read_model = "application_protocol_registry",
        .crud_capabilities_csv =
            "read_collection,read_item,construct_transaction",
        .construction_status = "protocol_framework_design",
        .mutation_authority = "versioned_controller_transaction_builder",
        .write_semantics =
            "CRUD mutations become valid ZCL transaction-construction requests",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "zslp",
        .status = "active",
        .layer = "zclassic23_application_layer",
        .base_layer = "zclassic_l1",
        .family = "token",
        .anchor = "OP_RETURN token transactions",
        .anchor_kind = "op_return",
        .rest_resource = "/api/v1/zslp/tokens",
        .read_model = "zslp_projection",
        .crud_capabilities_csv = "read_collection,read_item,read_subcollection",
        .construction_status = "transaction_builders_active",
        .mutation_authority = "operator_wallet_transaction",
        .write_semantics = "construct_and_broadcast_zslp_transactions",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "znam",
        .status = "active",
        .layer = "zclassic23_application_layer",
        .base_layer = "zclassic_l1",
        .family = "name_registry",
        .anchor = "OP_RETURN name registry transactions",
        .anchor_kind = "op_return",
        .rest_resource = "/api/v1/names",
        .read_model = "znam_projection",
        .crud_capabilities_csv = "read_collection,read_item",
        .construction_status = "transaction_builders_active",
        .mutation_authority = "operator_wallet_transaction",
        .write_semantics = "construct_and_broadcast_znam_transactions",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "market",
        .status = "active",
        .layer = "zclassic23_application_layer",
        .base_layer = "zclassic_l1",
        .family = "commerce",
        .anchor = "ZCL payments, ZSLP gating, and file-service manifests",
        .anchor_kind = "payment_and_manifest",
        .rest_resource = "/api/v1/market",
        .read_model = "file_market_projection",
        .crud_capabilities_csv = "read_collection,create_offer,create_purchase",
        .construction_status = "payment_gated_flows_active",
        .mutation_authority = "operator_or_payment_gated",
        .write_semantics =
            "offer_and_purchase_flows_are_operator_or_payment_gated",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "messaging",
        .status = "in_progress",
        .layer = "zclassic23_application_layer",
        .base_layer = "zclassic_l1",
        .family = "messaging",
        .anchor = "P2P messages plus planned shielded memo channel",
        .anchor_kind = "p2p_and_sapling_memo",
        .rest_resource = "/api/v1/messages",
        .read_model = "message_projection",
        .crud_capabilities_csv = "read_collection,create_message",
        .construction_status = "p2p_active_memo_channel_in_progress",
        .mutation_authority = "operator_private_message_send",
        .write_semantics =
            "send_p2p_messages_or_construct_memo_transactions_when_wired",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "script_contracts",
        .status = "in_progress",
        .layer = "zclassic23_application_layer",
        .base_layer = "zclassic_l1",
        .family = "script_contract",
        .anchor = "standard script contracts including HTLC atomic swaps",
        .anchor_kind = "standard_script",
        .rest_resource = "/api/v1/swaps",
        .read_model = "swap_contract_projection",
        .crud_capabilities_csv = "read_collection,read_capabilities,construct_contract",
        .construction_status = "htlc_builders_active_settlement_in_progress",
        .mutation_authority = "operator_wallet_transaction",
        .write_semantics =
            "construct_standard_script_contract_transactions_without_consensus_changes",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "atomic_swaps",
        .status = "in_progress",
        .layer = "zclassic23_application_layer",
        .base_layer = "zclassic_l1",
        .family = "script_contract",
        .anchor = "standard script HTLC transactions",
        .anchor_kind = "standard_script",
        .rest_resource = "/api/v1/swaps",
        .read_model = "swap_contract_projection",
        .crud_capabilities_csv = "read_collection,read_capabilities,construct_htlc",
        .construction_status = "htlc_builders_active_settlement_in_progress",
        .mutation_authority = "operator_wallet_transaction",
        .write_semantics =
            "construct_script_contract_transactions_without_consensus_changes",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
};

static size_t api_app_protocol_count(void)
{
    return sizeof(k_api_app_protocols) / sizeof(k_api_app_protocols[0]);
}

const struct api_app_protocol_contract *
api_app_protocol_lookup(const char *name)
{
    if (!name || !name[0])
        return NULL;

    for (size_t i = 0; i < api_app_protocol_count(); i++) {
        const struct api_app_protocol_contract *p = &k_api_app_protocols[i];
        if (strcmp(p->name, name) == 0)
            return p;
    }
    return NULL;
}

const struct api_app_protocol_contract *
api_app_protocol_for_resource(const char *resource)
{
    if (!resource || !resource[0])
        return NULL;

    if (strncmp(resource, "zslp_", 5) == 0)
        return api_app_protocol_lookup("zslp");
    if (strcmp(resource, "protocols") == 0)
        return api_app_protocol_lookup("zlsp");
    if (strcmp(resource, "names") == 0)
        return api_app_protocol_lookup("znam");
    if (strcmp(resource, "market") == 0 ||
        strcmp(resource, "files") == 0 ||
        strcmp(resource, "file_services") == 0)
        return api_app_protocol_lookup("market");
    if (strcmp(resource, "messages") == 0)
        return api_app_protocol_lookup("messaging");
    if (strcmp(resource, "swaps") == 0)
        return api_app_protocol_lookup("script_contracts");

    return NULL;
}

static void api_app_protocol_set_json(
    struct json_value *obj,
    const struct api_app_protocol_contract *p)
{
    if (!obj || !p)
        return;

    json_set_object(obj);
    json_push_kv_str(obj, "schema", ZCL_APP_PROTOCOL_CONTRACT_SCHEMA);
    json_push_kv_str(obj, "name", p->name);
    json_push_kv_str(obj, "status", p->status);
    json_push_kv_str(obj, "layer", p->layer);
    json_push_kv_str(obj, "base_layer", p->base_layer);
    json_push_kv_str(obj, "family", p->family);
    json_push_kv_str(obj, "anchor", p->anchor);
    json_push_kv_str(obj, "anchor_kind", p->anchor_kind);
    json_push_kv_str(obj, "rest_resource", p->rest_resource);
    json_push_kv_str(obj, "read_model", p->read_model);

    struct json_value crud;
    json_init(&crud);
    api_app_protocol_crud_json(p, &crud);
    json_push_kv(obj, "crud_capabilities", &crud);
    json_free(&crud);

    json_push_kv_str(obj, "construction_status",
                     p->construction_status);
    json_push_kv_str(obj, "mutation_authority", p->mutation_authority);
    json_push_kv_str(obj, "write_semantics", p->write_semantics);
    json_push_kv_str(obj, "consensus_boundary", p->consensus_boundary);
}

static void api_app_protocol_push_json(
    struct json_value *protocols,
    const struct api_app_protocol_contract *p)
{
    if (!protocols || !p)
        return;

    struct json_value obj;
    json_init(&obj);
    api_app_protocol_set_json(&obj, p);
    json_push_back(protocols, &obj);
    json_free(&obj);
}

void api_app_protocol_crud_json(const struct api_app_protocol_contract *p,
                                struct json_value *crud)
{
    json_set_array(crud);
    if (!p || !p->crud_capabilities_csv || !p->crud_capabilities_csv[0])
        return;

    const char *cursor = p->crud_capabilities_csv;
    while (*cursor) {
        while (*cursor == ',' || *cursor == ' ')
            cursor++;
        const char *start = cursor;
        while (*cursor && *cursor != ',')
            cursor++;

        size_t len = (size_t)(cursor - start);
        while (len > 0 && start[len - 1] == ' ')
            len--;
        if (len > 0) {
            char item_buf[64];
            if (len >= sizeof(item_buf))
                len = sizeof(item_buf) - 1;
            memcpy(item_buf, start, len);
            item_buf[len] = '\0';

            struct json_value item;
            json_init(&item);
            json_set_str(&item, item_buf);
            json_push_back(crud, &item);
            json_free(&item);
        }
        if (*cursor == ',')
            cursor++;
    }
}

void api_app_protocols_json(struct json_value *protocols)
{
    if (!protocols)
        return;

    json_set_array(protocols);
    for (size_t i = 0; i < api_app_protocol_count(); i++)
        api_app_protocol_push_json(protocols, &k_api_app_protocols[i]);
}

bool api_app_protocols_index_json(struct json_value *out)
{
    if (!out)
        return false;

    json_set_object(out);
    json_push_kv_str(out, "schema", ZCL_APP_PROTOCOLS_INDEX_SCHEMA);
    json_push_kv_str(out, "base_layer", "zclassic_l1");
    json_push_kv_str(out, "service_layer", "zclassic23_application_layer");
    json_push_kv_str(out, "consensus_boundary",
                     "overlay protocols interpret or construct valid ZCL "
                     "transactions without changing consensus rules");

    struct json_value protocols;
    json_init(&protocols);
    api_app_protocols_json(&protocols);
    json_push_kv(out, "protocols", &protocols);
    json_push_kv_int(out, "protocol_count", (int64_t)json_size(&protocols));
    json_free(&protocols);
    return true;
}

bool api_app_protocol_show_json(const char *name, struct json_value *out)
{
    if (!out)
        return false;
    const struct api_app_protocol_contract *p =
        api_app_protocol_lookup(name);
    if (!p)
        return false;

    api_app_protocol_set_json(out, p);
    return true;
}

void api_app_protocol_push_openapi_extensions(
    const struct json_value *contract,
    struct json_value *operation)
{
    if (!contract || !operation)
        return;

    const char *application_protocol =
        json_get_str(json_get(contract, "application_protocol"));
    const char *layer = json_get_str(json_get(contract, "layer"));
    const char *base_layer = json_get_str(json_get(contract, "base_layer"));
    const char *protocol_family =
        json_get_str(json_get(contract, "protocol_family"));
    const char *source_anchor =
        json_get_str(json_get(contract, "source_anchor"));
    const char *anchor_kind =
        json_get_str(json_get(contract, "protocol_anchor_kind"));
    const char *read_model =
        json_get_str(json_get(contract, "read_model"));
    const char *construction_status =
        json_get_str(json_get(contract, "protocol_construction_status"));
    const char *mutation_authority =
        json_get_str(json_get(contract, "mutation_authority"));
    const char *write_semantics =
        json_get_str(json_get(contract, "write_semantics"));
    const char *consensus_boundary =
        json_get_str(json_get(contract, "consensus_boundary"));

    if (layer && layer[0])
        json_push_kv_str(operation, "x-zcl-layer", layer);
    if (base_layer && base_layer[0])
        json_push_kv_str(operation, "x-zcl-base-layer", base_layer);
    if (application_protocol && application_protocol[0])
        json_push_kv_str(operation, "x-zcl-application-protocol",
                         application_protocol);
    if (protocol_family && protocol_family[0])
        json_push_kv_str(operation, "x-zcl-protocol-family",
                         protocol_family);
    if (source_anchor && source_anchor[0])
        json_push_kv_str(operation, "x-zcl-source-anchor", source_anchor);
    if (anchor_kind && anchor_kind[0])
        json_push_kv_str(operation, "x-zcl-protocol-anchor-kind",
                         anchor_kind);
    if (read_model && read_model[0])
        json_push_kv_str(operation, "x-zcl-read-model", read_model);
    const struct json_value *crud = json_get(contract, "protocol_crud");
    if (crud)
        json_push_kv(operation, "x-zcl-protocol-crud", crud);
    if (construction_status && construction_status[0])
        json_push_kv_str(operation, "x-zcl-protocol-construction-status",
                         construction_status);
    if (mutation_authority && mutation_authority[0])
        json_push_kv_str(operation, "x-zcl-mutation-authority",
                         mutation_authority);
    if (write_semantics && write_semantics[0])
        json_push_kv_str(operation, "x-zcl-write-semantics",
                         write_semantics);
    if (consensus_boundary && consensus_boundary[0])
        json_push_kv_str(operation, "x-zcl-consensus-boundary",
                         consensus_boundary);
}
