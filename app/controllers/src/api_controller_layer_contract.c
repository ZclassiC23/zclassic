/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Machine-readable boundary between ZClassic L1 consensus and the
 * zclassic23 application layer exposed through REST/OpenAPI. */

#include "api_controller_internal.h"

#include "json/json.h"

static void api_rest_push_app_protocol(
    struct json_value *protocols,
    const char *name,
    const char *status,
    const char *anchor,
    const char *rest_resource,
    const char *read_model,
    const char *write_semantics)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "name", name);
    json_push_kv_str(&obj, "status", status);
    json_push_kv_str(&obj, "layer", "zclassic23_application_layer");
    json_push_kv_str(&obj, "anchor", anchor);
    json_push_kv_str(&obj, "rest_resource", rest_resource);
    json_push_kv_str(&obj, "read_model", read_model);
    json_push_kv_str(&obj, "write_semantics", write_semantics);
    json_push_kv_str(&obj, "consensus_boundary",
                     "interprets_or_constructs_valid_zcl_transactions_only");
    json_push_back(protocols, &obj);
    json_free(&obj);
}

void api_rest_layer_model_json(struct json_value *layer_model)
{
    json_set_object(layer_model);
    json_push_kv_str(layer_model, "schema", "zcl.rest_layer_model.v1");
    json_push_kv_str(layer_model, "base_layer", "zclassic_l1");
    json_push_kv_str(layer_model, "service_layer",
                     "zclassic23_application_layer");
    json_push_kv_str(layer_model, "consensus_authority",
                     "local_consensus_reducer");
    json_push_kv_str(layer_model, "read_rule",
                     "GET reads chain-derived projections at the served frontier");
    json_push_kv_str(layer_model, "mutation_rule",
                     "mutations construct or broadcast explicit ZCL transactions "
                     "or operator-gated actions; they do not directly write "
                     "chain-derived state");
    json_push_kv_str(layer_model, "consensus_boundary",
                     "application protocols may interpret OP_RETURN, memo, and "
                     "script-contract data but must not change block, tx, PoW, "
                     "activation, or coin-accounting rules");

    struct json_value protocols;
    json_init(&protocols);
    json_set_array(&protocols);
    api_rest_push_app_protocol(
        &protocols, "zslp", "active",
        "OP_RETURN token transactions",
        "/api/v1/zslp/tokens",
        "zslp_projection",
        "construct_and_broadcast_zslp_transactions");
    api_rest_push_app_protocol(
        &protocols, "znam", "active",
        "OP_RETURN name registry transactions",
        "/api/v1/names",
        "znam_projection",
        "construct_and_broadcast_znam_transactions");
    api_rest_push_app_protocol(
        &protocols, "market", "active",
        "ZCL payments, ZSLP gating, and file-service manifests",
        "/api/v1/market",
        "file_market_projection",
        "offer_and_purchase_flows_are_operator_or_payment_gated");
    api_rest_push_app_protocol(
        &protocols, "messaging", "in_progress",
        "P2P messages plus planned shielded memo channel",
        "/api/v1/messages",
        "message_projection",
        "send_p2p_messages_or_construct_memo_transactions_when_wired");
    api_rest_push_app_protocol(
        &protocols, "atomic_swaps", "in_progress",
        "standard script HTLC transactions",
        "/api/v1/swaps",
        "swap_contract_projection",
        "construct_script_contract_transactions_without_consensus_changes");
    json_push_kv(layer_model, "application_protocols", &protocols);
    json_free(&protocols);
}
