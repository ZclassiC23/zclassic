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
        .name = "zslp",
        .status = "active",
        .layer = "zclassic23_application_layer",
        .anchor = "OP_RETURN token transactions",
        .rest_resource = "/api/v1/zslp/tokens",
        .read_model = "zslp_projection",
        .write_semantics = "construct_and_broadcast_zslp_transactions",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "znam",
        .status = "active",
        .layer = "zclassic23_application_layer",
        .anchor = "OP_RETURN name registry transactions",
        .rest_resource = "/api/v1/names",
        .read_model = "znam_projection",
        .write_semantics = "construct_and_broadcast_znam_transactions",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "market",
        .status = "active",
        .layer = "zclassic23_application_layer",
        .anchor = "ZCL payments, ZSLP gating, and file-service manifests",
        .rest_resource = "/api/v1/market",
        .read_model = "file_market_projection",
        .write_semantics =
            "offer_and_purchase_flows_are_operator_or_payment_gated",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "messaging",
        .status = "in_progress",
        .layer = "zclassic23_application_layer",
        .anchor = "P2P messages plus planned shielded memo channel",
        .rest_resource = "/api/v1/messages",
        .read_model = "message_projection",
        .write_semantics =
            "send_p2p_messages_or_construct_memo_transactions_when_wired",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "script_contracts",
        .status = "in_progress",
        .layer = "zclassic23_application_layer",
        .anchor = "standard script contracts including HTLC atomic swaps",
        .rest_resource = "/api/v1/swaps",
        .read_model = "swap_contract_projection",
        .write_semantics =
            "construct_standard_script_contract_transactions_without_consensus_changes",
        .consensus_boundary =
            "interprets_or_constructs_valid_zcl_transactions_only",
    },
    {
        .name = "atomic_swaps",
        .status = "in_progress",
        .layer = "zclassic23_application_layer",
        .anchor = "standard script HTLC transactions",
        .rest_resource = "/api/v1/swaps",
        .read_model = "swap_contract_projection",
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

static void api_app_protocol_push_json(
    struct json_value *protocols,
    const struct api_app_protocol_contract *p)
{
    if (!protocols || !p)
        return;

    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "name", p->name);
    json_push_kv_str(&obj, "status", p->status);
    json_push_kv_str(&obj, "layer", p->layer);
    json_push_kv_str(&obj, "anchor", p->anchor);
    json_push_kv_str(&obj, "rest_resource", p->rest_resource);
    json_push_kv_str(&obj, "read_model", p->read_model);
    json_push_kv_str(&obj, "write_semantics", p->write_semantics);
    json_push_kv_str(&obj, "consensus_boundary", p->consensus_boundary);
    json_push_back(protocols, &obj);
    json_free(&obj);
}

void api_app_protocols_json(struct json_value *protocols)
{
    if (!protocols)
        return;

    json_set_array(protocols);
    for (size_t i = 0; i < api_app_protocol_count(); i++)
        api_app_protocol_push_json(protocols, &k_api_app_protocols[i]);
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
    const char *source_anchor =
        json_get_str(json_get(contract, "source_anchor"));
    const char *read_model =
        json_get_str(json_get(contract, "read_model"));
    const char *write_semantics =
        json_get_str(json_get(contract, "write_semantics"));
    const char *consensus_boundary =
        json_get_str(json_get(contract, "consensus_boundary"));

    if (layer && layer[0])
        json_push_kv_str(operation, "x-zcl-layer", layer);
    if (application_protocol && application_protocol[0])
        json_push_kv_str(operation, "x-zcl-application-protocol",
                         application_protocol);
    if (source_anchor && source_anchor[0])
        json_push_kv_str(operation, "x-zcl-source-anchor", source_anchor);
    if (read_model && read_model[0])
        json_push_kv_str(operation, "x-zcl-read-model", read_model);
    if (write_semantics && write_semantics[0])
        json_push_kv_str(operation, "x-zcl-write-semantics",
                         write_semantics);
    if (consensus_boundary && consensus_boundary[0])
        json_push_kv_str(operation, "x-zcl-consensus-boundary",
                         consensus_boundary);
}
