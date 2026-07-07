/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZNAM service-directory filters and HTTP response helpers.
 *
 * The base service-directory serializer owns record classification. This file
 * owns read-only, server-side filtering for the UX/API subcollection route. */

#include "controllers/name_controller.h"
#include "api_controller_internal.h"
#include "json/json.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "util/log_macros.h"

struct api_name_service_filter {
    char service[64];
    char service_contract[64];
    char transport[32];
    char endpoint_kind[64];
    char valid[8];
    char endpoint_only[8];
    bool active;
};

static bool api_name_service_filter_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

static bool api_name_service_filter_value_safe(const char *value)
{
    if (!value || !value[0])
        return true;
    for (const char *p = value; *p; p++) {
        if (!api_name_service_filter_char_ok(*p))
            return false; /* raw-return-ok:filter-validation-error */
    }
    return true;
}

static bool api_name_service_allowed_value(const char *value,
                                           const char *csv)
{
    const char *p = csv;
    size_t value_len;

    if (!value || !value[0])
        return true;
    if (!csv || !csv[0])
        return false; /* raw-return-ok:filter-validation-error */

    value_len = strlen(value);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == value_len && strncmp(p, value, len) == 0)
            return true;
        if (!end)
            break;
        p = end + 1;
    }
    return false; /* raw-return-ok:filter-validation-error */
}

static bool api_name_service_filter_validate(
    const struct api_name_service_filter *filter,
    char *err,
    size_t err_len)
{
    if (!filter)
        return true;

#define FILTER_FAIL(field_, value_, allowed_) do { \
    if (err && err_len > 0) \
        snprintf(err, err_len, \
                 "invalid %s '%s' (allowed: %s)", \
                 field_, value_ ? value_ : "", allowed_); \
    return false; /* raw-return-ok:filter-validation-error */ \
} while (0)

    if (!api_name_service_filter_value_safe(filter->service))
        FILTER_FAIL("service", filter->service,
                    "letters,digits,underscore,dash,dot");
    if (!api_name_service_filter_value_safe(filter->service_contract))
        FILTER_FAIL("service_contract", filter->service_contract,
                    "letters,digits,underscore,dash,dot");
    if (!api_name_service_allowed_value(
            filter->transport, "p2p,onion,p2p_or_onion,unspecified,none"))
        FILTER_FAIL("transport", filter->transport,
                    "p2p,onion,p2p_or_onion,unspecified,none");
    if (!api_name_service_filter_value_safe(filter->endpoint_kind))
        FILTER_FAIL("endpoint_kind", filter->endpoint_kind,
                    "letters,digits,underscore,dash,dot");
    if (!api_name_service_allowed_value(filter->valid, "true,false"))
        FILTER_FAIL("valid", filter->valid, "true,false");
    if (!api_name_service_allowed_value(filter->endpoint_only,
                                        "true,false"))
        FILTER_FAIL("endpoint_only", filter->endpoint_only, "true,false");

#undef FILTER_FAIL
    return true;
}

static void api_name_service_filter_set(
    struct api_name_service_filter *filter,
    const char *key,
    const char *value)
{
    if (!filter || !key || !value || !value[0])
        return;
    if (strcmp(key, "service") == 0) {
        snprintf(filter->service, sizeof(filter->service), "%s", value);
        filter->active = true;
    } else if (strcmp(key, "service_contract") == 0 ||
               strcmp(key, "contract") == 0) {
        snprintf(filter->service_contract,
                 sizeof(filter->service_contract), "%s", value);
        filter->active = true;
    } else if (strcmp(key, "transport") == 0) {
        snprintf(filter->transport, sizeof(filter->transport), "%s", value);
        filter->active = true;
    } else if (strcmp(key, "endpoint_kind") == 0) {
        snprintf(filter->endpoint_kind, sizeof(filter->endpoint_kind),
                 "%s", value);
        filter->active = true;
    } else if (strcmp(key, "valid") == 0) {
        snprintf(filter->valid, sizeof(filter->valid), "%s", value);
        filter->active = true;
    } else if (strcmp(key, "endpoint_only") == 0) {
        snprintf(filter->endpoint_only, sizeof(filter->endpoint_only),
                 "%s", value);
        filter->active = true;
    }
}

static void api_name_service_filter_from_query(
    const char *path,
    struct api_name_service_filter *filter)
{
    const char *q;

    if (!path || !filter)
        return;
    q = strchr(path, '?');
    if (!q)
        return;
    q++;

    while (*q) {
        char key[48] = {0};
        char value[128] = {0};
        const char *pair_end = strchr(q, '&');
        const char *eq = strchr(q, '=');
        size_t pair_len = pair_end ? (size_t)(pair_end - q) : strlen(q);
        size_t key_len;
        size_t value_len;

        if (!eq || (size_t)(eq - q) >= pair_len) {
            if (!pair_end)
                break;
            q = pair_end + 1;
            continue;
        }

        key_len = (size_t)(eq - q);
        value_len = pair_len - key_len - 1;
        if (key_len >= sizeof(key))
            key_len = sizeof(key) - 1;
        if (value_len >= sizeof(value))
            value_len = sizeof(value) - 1;
        memcpy(key, q, key_len);
        key[key_len] = '\0';
        memcpy(value, eq + 1, value_len);
        value[value_len] = '\0';
        api_name_service_filter_set(filter, key, value);

        if (!pair_end)
            break;
        q = pair_end + 1;
    }
}

static void api_name_service_filters_json(
    struct json_value *out,
    const struct api_name_service_filter *filter)
{
    json_set_object(out);
    json_push_kv_bool(out, "active", filter && filter->active);
    if (!filter)
        return;
    if (filter->service[0])
        json_push_kv_str(out, "service", filter->service);
    if (filter->service_contract[0])
        json_push_kv_str(out, "service_contract",
                         filter->service_contract);
    if (filter->transport[0])
        json_push_kv_str(out, "transport", filter->transport);
    if (filter->endpoint_kind[0])
        json_push_kv_str(out, "endpoint_kind", filter->endpoint_kind);
    if (filter->valid[0])
        json_push_kv_bool(out, "valid",
                          strcmp(filter->valid, "true") == 0);
    if (filter->endpoint_only[0])
        json_push_kv_bool(out, "endpoint_only",
                          strcmp(filter->endpoint_only, "true") == 0);
    json_push_kv_str(out, "semantics",
                     "exact-match filters over the verified ZNAM service "
                     "record projection");
}

static bool api_name_service_record_matches_filter(
    const struct json_value *record,
    const struct api_name_service_filter *filter)
{
    const char *value;

    if (!record)
        return false; /* raw-return-ok:predicate-null-input */
    if (!filter || !filter->active)
        return true;

    if (filter->service[0]) {
        value = json_get_str(json_get(record, "service_name"));
        if (!value || strcmp(value, filter->service) != 0)
            return false; /* raw-return-ok:predicate-negative-match */
    }
    if (filter->service_contract[0]) {
        value = json_get_str(json_get(record, "service_contract"));
        if (!value || strcmp(value, filter->service_contract) != 0)
            return false; /* raw-return-ok:predicate-negative-match */
    }
    if (filter->transport[0]) {
        value = json_get_str(json_get(record, "transport"));
        if (!value || strcmp(value, filter->transport) != 0)
            return false; /* raw-return-ok:predicate-negative-match */
    }
    if (filter->endpoint_kind[0]) {
        value = json_get_str(json_get(record, "endpoint_kind"));
        if (!value || strcmp(value, filter->endpoint_kind) != 0)
            return false; /* raw-return-ok:predicate-negative-match */
    }
    if (filter->valid[0]) {
        bool want = strcmp(filter->valid, "true") == 0;
        if (json_get_bool(json_get(record, "endpoint_hint_valid")) != want)
            return false; /* raw-return-ok:predicate-negative-match */
    }
    if (filter->endpoint_only[0]) {
        bool want = strcmp(filter->endpoint_only, "true") == 0;
        if (json_get_bool(json_get(record, "is_endpoint_hint")) != want)
            return false; /* raw-return-ok:predicate-negative-match */
    }
    return true;
}

static void api_name_service_copy_str_field(struct json_value *out,
                                            const struct json_value *base,
                                            const char *key)
{
    const char *value = json_get_str(json_get(base, key));

    if (value)
        json_push_kv_str(out, key, value);
}

static void api_name_service_copy_int_field(struct json_value *out,
                                            const struct json_value *base,
                                            const char *key)
{
    const struct json_value *value = json_get(base, key);

    if (value)
        json_push_kv_int(out, key, json_get_int(value));
}

static void api_name_service_routing_plan_json(
    struct json_value *out,
    int endpoint_count,
    int valid_endpoint_count,
    int invalid_endpoint_count,
    bool supports_onion,
    bool supports_direct_p2p,
    bool supports_bootstrap)
{
    const char *preferred = "none";
    const char *fallback = "inspect_service_catalog";

    if (supports_direct_p2p) {
        preferred = "p2p";
        fallback = supports_onion ? "onion" : "bootstrap";
    } else if (supports_bootstrap) {
        preferred = "p2p_or_onion";
        fallback = supports_onion ? "onion" : "inspect_bootstrap_status";
    } else if (supports_onion) {
        preferred = "onion";
        fallback = "direct_p2p_if_directory_advertises_it";
    }

    json_set_object(out);
    json_push_kv_str(out, "schema", "zcl.names.service_routing_plan.v1");
    json_push_kv_str(out, "strategy",
                     "prefer_valid_direct_p2p_then_bootstrap_then_onion");
    json_push_kv_str(out, "preferred_transport", preferred);
    json_push_kv_str(out, "fallback_transport", fallback);
    json_push_kv_int(out, "endpoint_count", endpoint_count);
    json_push_kv_int(out, "valid_endpoint_count", valid_endpoint_count);
    json_push_kv_int(out, "invalid_endpoint_count", invalid_endpoint_count);
    json_push_kv_bool(out, "chain_verified", true);
    json_push_kv_bool(out, "requires_runtime_probe", endpoint_count > 0);
    json_push_kv_str(out, "next_action",
                     "run_selected_service_runtime_probe_before_connecting");
}

static void api_name_service_accumulate_transport(
    const struct json_value *record,
    bool *supports_onion,
    bool *supports_direct_p2p,
    bool *supports_bootstrap)
{
    const char *transport;

    if (!record)
        return;

    transport = json_get_str(json_get(record, "transport"));
    if (transport && strcmp(transport, "p2p") == 0) {
        if (supports_direct_p2p)
            *supports_direct_p2p = true;
    } else if (transport && strcmp(transport, "onion") == 0) {
        if (supports_onion)
            *supports_onion = true;
    } else if (transport && strcmp(transport, "p2p_or_onion") == 0) {
        if (supports_bootstrap)
            *supports_bootstrap = true;
    }
}

static void api_name_service_filtered_directory_json(
    const struct json_value *base,
    const struct api_name_service_filter *filter,
    struct json_value *out)
{
    const struct json_value *base_records;
    struct json_value filters = {0};
    struct json_value records = {0};
    struct json_value endpoints = {0};
    struct json_value plan = {0};
    int service_count = 0;
    int endpoint_count = 0;
    int valid_endpoint_count = 0;
    int invalid_endpoint_count = 0;
    bool supports_onion = false;
    bool supports_direct_p2p = false;
    bool supports_bootstrap = false;

    json_set_object(out);
    api_name_service_copy_str_field(out, base, "schema");
    api_name_service_copy_int_field(out, base, "schema_version");
    api_name_service_copy_str_field(out, base, "source");
    api_name_service_copy_str_field(out, base, "transport_model");
    api_name_service_copy_str_field(out, base, "base_layer");
    api_name_service_copy_str_field(out, base, "routing_policy");
    api_name_service_copy_str_field(out, base, "service_contract_route");
    api_name_service_copy_str_field(out, base, "operation_contract_route");
    api_name_service_copy_str_field(out, base, "runtime_probe_schema");
    api_name_service_copy_str_field(out, base, "runtime_probe_contract_field");
    api_name_service_copy_str_field(out, base, "runtime_probe_policy");
    json_push_kv_str(out, "filter_model",
                     "server-side exact-match filters over service, "
                     "service_contract, transport, endpoint_kind, valid, "
                     "and endpoint_only");
    json_set_object(&filters);
    api_name_service_filters_json(&filters, filter);
    json_push_kv(out, "filters", &filters);
    json_free(&filters);

    json_set_array(&records);
    json_set_array(&endpoints);
    base_records = json_get(base, "records");
    if (base_records && base_records->type == JSON_ARR) {
        for (size_t i = 0; i < json_size(base_records); i++) {
            const struct json_value *record = json_at(base_records, i);
            struct json_value copy = {0};
            bool is_endpoint;
            bool valid_endpoint;

            if (!api_name_service_record_matches_filter(record, filter))
                continue;

            service_count++;
            api_name_service_accumulate_transport(record, &supports_onion,
                                                  &supports_direct_p2p,
                                                  &supports_bootstrap);
            json_copy(&copy, record);
            json_push_back(&records, &copy);
            json_free(&copy);

            is_endpoint = json_get_bool(json_get(record, "is_endpoint_hint"));
            if (!is_endpoint)
                continue;

            endpoint_count++;
            valid_endpoint =
                json_get_bool(json_get(record, "endpoint_hint_valid"));
            if (valid_endpoint)
                valid_endpoint_count++;
            else
                invalid_endpoint_count++;

            json_copy(&copy, record);
            json_push_back(&endpoints, &copy);
            json_free(&copy);
        }
    }

    json_push_kv_bool(out, "has_services", service_count > 0);
    json_push_kv_int(out, "service_record_count", service_count);
    json_push_kv_int(out, "endpoint_count", endpoint_count);
    json_push_kv_int(out, "valid_endpoint_count", valid_endpoint_count);
    json_push_kv_int(out, "invalid_endpoint_count", invalid_endpoint_count);
    json_push_kv_bool(out, "supports_onion", supports_onion);
    json_push_kv_bool(out, "supports_direct_p2p", supports_direct_p2p);
    json_push_kv_bool(out, "supports_bootstrap", supports_bootstrap);

    json_set_object(&plan);
    api_name_service_routing_plan_json(&plan, endpoint_count,
                                       valid_endpoint_count,
                                       invalid_endpoint_count,
                                       supports_onion,
                                       supports_direct_p2p,
                                       supports_bootstrap);
    json_push_kv(out, "routing_plan", &plan);
    json_free(&plan);

    json_push_kv(out, "records", &records);
    json_free(&records);
    json_push_kv(out, "endpoints", &endpoints);
    json_free(&endpoints);

    api_name_service_copy_str_field(out, base, "name");
    api_name_service_copy_str_field(out, base, "name_route");
    api_name_service_copy_str_field(out, base, "self_route");
    api_name_service_copy_str_field(out, base, "operation_id");
    api_name_service_copy_str_field(out, base, "operation_route");
    api_name_service_copy_str_field(out, base, "parent_schema");
    api_name_service_copy_str_field(out, base, "next_action");
}

bool api_name_service_directory_path(const char *name, const char *path,
                                     struct json_value *result,
                                     char *err, size_t err_len)
{
    struct json_value base = {0};
    struct api_name_service_filter filter = {0};

    if (!result)
        LOG_FAIL("name",
                 "api_name_service_directory_path called with NULL result");

    if (!api_name_service_directory(name, &base)) {
        json_free(&base);
        return false; /* raw-return-ok:source-directory-missing */
    }

    api_name_service_filter_from_query(path, &filter);
    if (!filter.active) {
        json_copy(result, &base);
        json_free(&base);
        return true;
    }

    if (!api_name_service_filter_validate(&filter, err, err_len)) {
        json_free(&base);
        return false; /* raw-return-ok:filter-validation-error */
    }

    api_name_service_filtered_directory_json(&base, &filter, result);
    json_free(&base);
    return true;
}

static void api_name_service_filter_error_json(struct json_value *out,
                                               const char *message)
{
    struct json_value allowed = {0};

    json_set_object(out);
    json_push_kv_str(out, "schema", ZCL_REST_ERROR_SCHEMA);
    json_push_kv_str(out, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(out, "error", "invalid_name_service_filter");
    json_push_kv_str(out, "message",
                     message && message[0] ? message :
                     "invalid name service filter");

    json_set_object(&allowed);
    json_push_kv_str(&allowed, "service",
                     "letters,digits,underscore,dash,dot");
    json_push_kv_str(&allowed, "service_contract",
                     "letters,digits,underscore,dash,dot");
    json_push_kv_str(&allowed, "transport",
                     "p2p,onion,p2p_or_onion,unspecified,none");
    json_push_kv_str(&allowed, "endpoint_kind",
                     "letters,digits,underscore,dash,dot");
    json_push_kv_str(&allowed, "valid", "true,false");
    json_push_kv_str(&allowed, "endpoint_only", "true,false");
    json_push_kv(out, "allowed_filters", &allowed);
    json_free(&allowed);
    json_push_kv_str(out, "example",
                     "/api/v1/names/alice/services?"
                     "transport=p2p&valid=true&endpoint_only=true");
}

size_t api_serve_name_service_directory(const char *name, const char *path,
                                        const char *freshness,
                                        uint8_t *response,
                                        size_t response_max)
{
    struct json_value jr = {0};
    char err[192] = {0};

    if (api_name_service_directory_path(name, path, &jr,
                                        err, sizeof(err))) {
        api_json_add_freshness(&jr, freshness ? freshness : "znam_projection",
                               -1);
        size_t n = api_json_ok(response, response_max, &jr);
        json_free(&jr);
        return n;
    }

    json_free(&jr);
    if (err[0]) {
        json_init(&jr);
        api_name_service_filter_error_json(&jr, err);
        size_t n = api_json_status(response, response_max,
                                   "400 Bad Request", &jr);
        json_free(&jr);
        return n;
    }

    return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Name service directory not found");
}
