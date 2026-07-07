/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Shared query-filter contracts for REST collections. Endpoint responses,
 * route contracts, and OpenAPI extensions use this file as the single source
 * for strict filter semantics and accepted aliases. */

#include "api_controller_internal.h"

#include "json/json.h"

#include <string.h>

#define API_FILTER_SERVICE_OPERATIONS "service_operations"
#define API_FILTER_NAME_SERVICE_DIRECTORY "name_service_directory"

const char *api_query_filter_contract_for_route(const char *method,
                                                const char *public_path)
{
    if (!method || !public_path || strcmp(method, "GET") != 0)
        return NULL;
    if (strcmp(public_path, "/api/v1/service-operations") == 0)
        return API_FILTER_SERVICE_OPERATIONS;
    if (strcmp(public_path, "/api/v1/names/{name}/services") == 0)
        return API_FILTER_NAME_SERVICE_DIRECTORY;
    return NULL;
}

void api_query_filter_allowed_filters_json(const char *contract_name,
                                           struct json_value *out)
{
    json_set_object(out);
    if (!contract_name)
        return;

    if (strcmp(contract_name, API_FILTER_SERVICE_OPERATIONS) == 0) {
        json_push_kv_str(out, "service",
                         "letters,digits,underscore,dash,dot");
        json_push_kv_str(out, "write_safety",
                         "public_read_only,operator_private,"
                         "operator_private_destructive");
        json_push_kv_str(out, "preferred_interface",
                         "rest,mcp,rpc,native_or_planned");
        json_push_kv_str(out, "interface",
                         "alias_for_preferred_interface");
        json_push_kv_str(out, "status", "active,in_progress");
        json_push_kv_str(out, "surface", "rest,mcp,rpc");
    } else if (strcmp(contract_name, API_FILTER_NAME_SERVICE_DIRECTORY) == 0) {
        json_push_kv_str(out, "service",
                         "letters,digits,underscore,dash,dot");
        json_push_kv_str(out, "service_contract",
                         "letters,digits,underscore,dash,dot");
        json_push_kv_str(out, "contract",
                         "alias_for_service_contract");
        json_push_kv_str(out, "transport",
                         "p2p,onion,p2p_or_onion,unspecified,none");
        json_push_kv_str(out, "endpoint_kind",
                         "letters,digits,underscore,dash,dot");
        json_push_kv_str(out, "valid", "true,false");
        json_push_kv_str(out, "endpoint_only", "true,false");
    }
}

void api_query_filter_contract_json(const char *contract_name,
                                    struct json_value *out)
{
    struct json_value allowed = {0};
    const bool service_ops = contract_name &&
        strcmp(contract_name, API_FILTER_SERVICE_OPERATIONS) == 0;
    const bool name_services = contract_name &&
        strcmp(contract_name, API_FILTER_NAME_SERVICE_DIRECTORY) == 0;

    json_set_object(out);
    json_push_kv_str(out, "schema", ZCL_QUERY_FILTER_CONTRACT_SCHEMA);
    json_push_kv_bool(out, "unknown_filters_error", true);
    json_push_kv_str(out, "semantics", "server_side_exact_match");
    if (contract_name && contract_name[0])
        json_push_kv_str(out, "contract_name", contract_name);

    api_query_filter_allowed_filters_json(contract_name, &allowed);
    json_push_kv(out, "allowed_filters", &allowed);
    json_free(&allowed);

    if (service_ops) {
        json_push_kv_str(out, "example",
                         "/api/v1/service-operations?"
                         "service=bootstrap&write_safety=public_read_only");
    } else if (name_services) {
        json_push_kv_str(out, "example",
                         "/api/v1/names/alice/services?"
                         "transport=p2p&valid=true&endpoint_only=true");
    }
}
