/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Collection/index view for service operation contracts. The catalog file owns
 * operation definitions; this file owns filtering and list metadata. */

#include "api_controller_internal.h"
#include "api_controller_service_operations_internal.h"

#include "json/json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct api_service_operation_counts {
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

struct api_service_operation_filter {
    char service[64];
    char write_safety[40];
    char preferred_interface[32];
    char status[32];
    char surface[16];
    bool active;
};

static bool api_service_operation_surface_matches(
    const struct api_service_operation_contract *op,
    const char *surface)
{
    if (!surface || !surface[0])
        return true;
    if (!op)
        return false; /* raw-return-ok:predicate-null-input */
    if (strcmp(surface, "rest") == 0)
        return op->rest_method && op->rest_method[0] &&
               op->rest_route && op->rest_route[0];
    if (strcmp(surface, "mcp") == 0)
        return op->mcp_tool && op->mcp_tool[0];
    if (strcmp(surface, "rpc") == 0)
        return op->rpc_method && op->rpc_method[0];
    return false; /* raw-return-ok:predicate-negative-match */
}

static bool api_service_operation_filter_matches(
    const struct api_service_operation_contract *op,
    const struct api_service_operation_filter *filter)
{
    const char *iface;

    if (!op)
        return false; /* raw-return-ok:predicate-null-input */
    if (!filter || !filter->active)
        return true;
    if (filter->service[0] &&
        strcmp(op->service_name, filter->service) != 0)
        return false; /* raw-return-ok:predicate-negative-match */
    if (filter->write_safety[0] &&
        strcmp(api_service_operation_write_safety(op),
               filter->write_safety) != 0)
        return false; /* raw-return-ok:predicate-negative-match */
    if (filter->status[0] &&
        strcmp(op->status ? op->status : "", filter->status) != 0)
        return false; /* raw-return-ok:predicate-negative-match */
    iface = api_service_operation_agent_interface(op);
    if (filter->preferred_interface[0] &&
        strcmp(iface, filter->preferred_interface) != 0)
        return false; /* raw-return-ok:predicate-negative-match */
    if (!api_service_operation_surface_matches(op, filter->surface))
        return false; /* raw-return-ok:predicate-negative-match */
    return true;
}

static void api_service_operation_counts_add(
    struct api_service_operation_counts *counts,
    const struct api_service_operation_contract *op)
{
    const char *iface;

    if (!counts || !op)
        return;

    iface = api_service_operation_agent_interface(op);
    counts->operation_count++;
    if (op->public_read)
        counts->public_read_count++;
    if (op->operator_private)
        counts->operator_private_count++;
    if (op->destructive)
        counts->destructive_count++;
    if (op->rest_method && op->rest_method[0] &&
        op->rest_route && op->rest_route[0])
        counts->rest_callable_count++;
    if (op->mcp_tool && op->mcp_tool[0])
        counts->mcp_callable_count++;
    if (op->rpc_method && op->rpc_method[0])
        counts->rpc_callable_count++;
    if (op->status && strcmp(op->status, "active") == 0)
        counts->active_count++;
    if (op->status && strcmp(op->status, "in_progress") == 0)
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

static void api_service_operation_counts_json(
    struct json_value *out,
    const struct api_service_operation_counts *counts)
{
    if (!out || !counts)
        return;

    json_set_object(out);
    json_push_kv_int(out, "operation_count", counts->operation_count);
    json_push_kv_int(out, "public_read_count", counts->public_read_count);
    json_push_kv_int(out, "operator_private_count",
                     counts->operator_private_count);
    json_push_kv_int(out, "destructive_count", counts->destructive_count);
    json_push_kv_int(out, "rest_callable_count",
                     counts->rest_callable_count);
    json_push_kv_int(out, "mcp_callable_count", counts->mcp_callable_count);
    json_push_kv_int(out, "rpc_callable_count", counts->rpc_callable_count);
    json_push_kv_int(out, "active_count", counts->active_count);
    json_push_kv_int(out, "in_progress_count", counts->in_progress_count);
    json_push_kv_int(out, "preferred_rest_count",
                     counts->preferred_rest_count);
    json_push_kv_int(out, "preferred_mcp_count",
                     counts->preferred_mcp_count);
    json_push_kv_int(out, "preferred_rpc_count",
                     counts->preferred_rpc_count);
    json_push_kv_int(out, "preferred_native_count",
                     counts->preferred_native_count);
}

static void api_service_operation_counts_for_filter(
    const struct api_service_operation_filter *filter,
    struct api_service_operation_counts *counts)
{
    if (!counts)
        return;

    memset(counts, 0, sizeof(*counts));
    for (size_t i = 0; i < api_service_operation_count(); i++) {
        const struct api_service_operation_contract *op =
            api_service_operation_at(i);
        if (api_service_operation_filter_matches(op, filter))
            api_service_operation_counts_add(counts, op);
    }
}

static bool api_service_operation_service_seen_before(size_t index)
{
    const struct api_service_operation_contract *op =
        api_service_operation_at(index);

    if (!op)
        return true;

    for (size_t i = 0; i < index; i++) {
        const struct api_service_operation_contract *prev =
            api_service_operation_at(i);
        if (prev && strcmp(prev->service_name, op->service_name) == 0)
            return true;
    }

    return false; /* raw-return-ok:predicate-negative-match */
}

static void api_service_operation_service_facets_json(
    struct json_value *out,
    const struct api_service_operation_filter *filter)
{
    json_set_array(out);
    for (size_t i = 0; i < api_service_operation_count(); i++) {
        const struct api_service_operation_contract *op =
            api_service_operation_at(i);
        struct api_service_operation_counts counts;
        struct json_value facet;
        struct api_service_operation_filter service_filter =
            filter ? *filter : (struct api_service_operation_filter){0};
        char service_route[160];

        if (!op || api_service_operation_service_seen_before(i))
            continue;

        snprintf(service_filter.service, sizeof(service_filter.service),
                 "%s", op->service_name);
        service_filter.active = true;
        api_service_operation_counts_for_filter(&service_filter, &counts);
        if (counts.operation_count == 0)
            continue;
        snprintf(service_route, sizeof(service_route),
                 "/api/v1/service-catalog/%s", op->service_name);
        service_route[sizeof(service_route) - 1] = '\0';

        json_init(&facet);
        api_service_operation_counts_json(&facet, &counts);
        json_push_kv_str(&facet, "service", op->service_name);
        json_push_kv_str(&facet, "service_catalog_route", service_route);
        json_push_kv_str(&facet, "operation_subset_route", service_route);
        json_push_kv_str(&facet, "operation_subset_field", "operations");
        json_push_back(out, &facet);
        json_free(&facet);
    }
}

static void api_service_operation_named_count_json(
    struct json_value *out,
    const char *name,
    int64_t count)
{
    struct json_value facet;

    json_init(&facet);
    json_set_object(&facet);
    json_push_kv_str(&facet, "name", name);
    json_push_kv_int(&facet, "operation_count", count);
    json_push_back(out, &facet);
    json_free(&facet);
}

static void api_service_operation_interface_facets_json(
    struct json_value *out,
    const struct api_service_operation_counts *counts)
{
    json_set_array(out);
    if (!counts)
        return;

    api_service_operation_named_count_json(out, "rest",
                                           counts->preferred_rest_count);
    api_service_operation_named_count_json(out, "mcp",
                                           counts->preferred_mcp_count);
    api_service_operation_named_count_json(out, "rpc",
                                           counts->preferred_rpc_count);
    api_service_operation_named_count_json(out, "native_or_planned",
                                           counts->preferred_native_count);
}

static void api_service_operation_safety_facets_json(
    struct json_value *out,
    const struct api_service_operation_counts *counts)
{
    int64_t public_read_only;
    int64_t operator_private_only;

    json_set_array(out);
    if (!counts)
        return;

    public_read_only = counts->operation_count -
        counts->operator_private_count;
    operator_private_only = counts->operator_private_count -
        counts->destructive_count;

    api_service_operation_named_count_json(out, "public_read_only",
                                           public_read_only);
    api_service_operation_named_count_json(out, "operator_private",
                                           operator_private_only);
    api_service_operation_named_count_json(out, "operator_private_destructive",
                                           counts->destructive_count);
}

static bool api_service_operation_filter_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

static bool api_service_operation_filter_value_safe(const char *value)
{
    if (!value || !value[0])
        return true;
    for (const char *p = value; *p; p++) {
        if (!api_service_operation_filter_char_ok(*p))
            return false; /* raw-return-ok:filter-validation-error */
    }
    return true;
}

static bool api_service_operation_allowed_value(const char *value,
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

static bool api_service_operation_filter_validate(
    const struct api_service_operation_filter *filter,
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

    if (!api_service_operation_filter_value_safe(filter->service))
        FILTER_FAIL("service", filter->service,
                    "letters,digits,underscore,dash,dot");
    if (!api_service_operation_allowed_value(
            filter->write_safety,
            "public_read_only,operator_private,"
            "operator_private_destructive"))
        FILTER_FAIL("write_safety", filter->write_safety,
                    "public_read_only,operator_private,"
                    "operator_private_destructive");
    if (!api_service_operation_allowed_value(
            filter->preferred_interface,
            "rest,mcp,rpc,native_or_planned"))
        FILTER_FAIL("preferred_interface", filter->preferred_interface,
                    "rest,mcp,rpc,native_or_planned");
    if (!api_service_operation_allowed_value(filter->status,
                                             "active,in_progress"))
        FILTER_FAIL("status", filter->status, "active,in_progress");
    if (!api_service_operation_allowed_value(filter->surface,
                                             "rest,mcp,rpc"))
        FILTER_FAIL("surface", filter->surface, "rest,mcp,rpc");

#undef FILTER_FAIL
    return true;
}

static void api_service_operation_filter_set(
    struct api_service_operation_filter *f,
    const char *key,
    const char *value)
{
    if (!f || !key || !value || !value[0])
        return;
    if (strcmp(key, "service") == 0) {
        snprintf(f->service, sizeof(f->service), "%s", value);
        f->active = true;
    } else if (strcmp(key, "write_safety") == 0) {
        snprintf(f->write_safety, sizeof(f->write_safety), "%s", value);
        f->active = true;
    } else if (strcmp(key, "preferred_interface") == 0 ||
               strcmp(key, "interface") == 0) {
        snprintf(f->preferred_interface, sizeof(f->preferred_interface),
                 "%s", value);
        f->active = true;
    } else if (strcmp(key, "status") == 0) {
        snprintf(f->status, sizeof(f->status), "%s", value);
        f->active = true;
    } else if (strcmp(key, "surface") == 0) {
        snprintf(f->surface, sizeof(f->surface), "%s", value);
        f->active = true;
    }
}

static void api_service_operation_filter_from_query(
    const char *path,
    struct api_service_operation_filter *filter)
{
    const char *q;

    if (!path || !filter)
        return;
    q = strchr(path, '?');
    if (!q)
        return;
    q++;

    while (*q) {
        char key[40] = {0};
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
        api_service_operation_filter_set(filter, key, value);

        if (!pair_end)
            break;
        q = pair_end + 1;
    }
}

static void api_service_operation_filters_json(
    struct json_value *out,
    const struct api_service_operation_filter *filter)
{
    json_set_object(out);
    json_push_kv_bool(out, "active", filter && filter->active);
    if (!filter)
        return;
    if (filter->service[0])
        json_push_kv_str(out, "service", filter->service);
    if (filter->write_safety[0])
        json_push_kv_str(out, "write_safety", filter->write_safety);
    if (filter->preferred_interface[0])
        json_push_kv_str(out, "preferred_interface",
                         filter->preferred_interface);
    if (filter->status[0])
        json_push_kv_str(out, "status", filter->status);
    if (filter->surface[0])
        json_push_kv_str(out, "surface", filter->surface);
    json_push_kv_str(out, "semantics",
                     "exact-match filters over the static operation catalog");
}

static void api_service_operations_filtered_json(
    struct json_value *out,
    const struct api_service_operation_filter *filter)
{
    json_set_array(out);
    for (size_t i = 0; i < api_service_operation_count(); i++) {
        struct json_value op_json;
        const struct api_service_operation_contract *op =
            api_service_operation_at(i);

        if (!api_service_operation_filter_matches(op, filter))
            continue;
        json_init(&op_json);
        api_service_operation_json(&op_json, op);
        json_push_back(out, &op_json);
        json_free(&op_json);
    }
}

bool api_service_operations_filtered_index_json(
    struct json_value *out,
    const char *service,
    const char *write_safety,
    const char *preferred_interface,
    const char *status,
    const char *surface,
    char *err,
    size_t err_len)
{
    struct json_value operations;
    struct json_value summary;
    struct json_value service_facets;
    struct json_value interface_facets;
    struct json_value safety_facets;
    struct json_value filters;
    struct api_service_operation_counts counts;
    struct api_service_operation_filter filter = {0};

    if (!out)
        return false; /* raw-return-ok:builder-null-output */

    api_service_operation_filter_set(&filter, "service", service);
    api_service_operation_filter_set(&filter, "write_safety", write_safety);
    api_service_operation_filter_set(&filter, "preferred_interface",
                                     preferred_interface);
    api_service_operation_filter_set(&filter, "status", status);
    api_service_operation_filter_set(&filter, "surface", surface);
    if (!api_service_operation_filter_validate(&filter, err, err_len))
        return false; /* raw-return-ok:filter-validation-error */

    api_service_operation_counts_for_filter(&filter, &counts);

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
                     "server-side exact-match filters over service, "
                     "write_safety, preferred_interface, status, and surface");

    json_init(&filters);
    api_service_operation_filters_json(&filters, &filter);
    json_push_kv(out, "filters", &filters);
    json_free(&filters);

    json_init(&service_facets);
    api_service_operation_service_facets_json(&service_facets, &filter);

    json_init(&summary);
    api_service_operation_counts_json(&summary, &counts);
    json_push_kv_int(&summary, "service_count",
                     (int64_t)json_size(&service_facets));
    json_push_kv(out, "summary", &summary);
    json_free(&summary);

    json_push_kv(out, "service_facets", &service_facets);
    json_free(&service_facets);

    json_init(&interface_facets);
    api_service_operation_interface_facets_json(&interface_facets, &counts);
    json_push_kv(out, "preferred_interface_facets", &interface_facets);
    json_free(&interface_facets);

    json_init(&safety_facets);
    api_service_operation_safety_facets_json(&safety_facets, &counts);
    json_push_kv(out, "write_safety_facets", &safety_facets);
    json_free(&safety_facets);

    json_init(&operations);
    api_service_operations_filtered_json(&operations, &filter);
    json_push_kv(out, "operations", &operations);
    json_push_kv_int(out, "operation_count",
                     (int64_t)json_size(&operations));
    json_free(&operations);

    return true;
}

bool api_service_operations_index_json(struct json_value *out)
{
    return api_service_operations_filtered_index_json(out, NULL, NULL, NULL,
                                                     NULL, NULL, NULL, 0);
}

bool api_service_operations_index_path_json(const char *path,
                                            struct json_value *out,
                                            char *err,
                                            size_t err_len)
{
    struct api_service_operation_filter filter = {0};

    if (!out)
        return false; /* raw-return-ok:builder-null-output */
    api_service_operation_filter_from_query(path, &filter);
    return api_service_operations_filtered_index_json(
        out,
        filter.service,
        filter.write_safety,
        filter.preferred_interface,
        filter.status,
        filter.surface,
        err,
        err_len);
}

static void api_service_operation_filter_error_json(
    struct json_value *out,
    const char *message)
{
    struct json_value allowed;

    json_set_object(out);
    json_push_kv_str(out, "schema", ZCL_REST_ERROR_SCHEMA);
    json_push_kv_str(out, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(out, "error", "invalid_service_operation_filter");
    json_push_kv_str(out, "message",
                     message && message[0] ? message :
                     "invalid service operation filter");

    json_init(&allowed);
    json_set_object(&allowed);
    json_push_kv_str(&allowed, "service",
                     "letters,digits,underscore,dash,dot");
    json_push_kv_str(&allowed, "write_safety",
                     "public_read_only,operator_private,"
                     "operator_private_destructive");
    json_push_kv_str(&allowed, "preferred_interface",
                     "rest,mcp,rpc,native_or_planned");
    json_push_kv_str(&allowed, "status", "active,in_progress");
    json_push_kv_str(&allowed, "surface", "rest,mcp,rpc");
    json_push_kv(out, "allowed_filters", &allowed);
    json_free(&allowed);
    json_push_kv_str(out, "example",
                     "/api/v1/service-operations?"
                     "service=bootstrap&write_safety=public_read_only");
}

size_t api_serve_service_operations(const char *path,
                                    uint8_t *response,
                                    size_t response_max)
{
    struct json_value jr = {0};
    char err[192] = {0};

    if (api_service_operations_index_path_json(path, &jr, err, sizeof(err))) {
        api_json_add_freshness(&jr, "static", -1);
        size_t n = api_json_ok(response, response_max, &jr);
        json_free(&jr);
        return n;
    }

    json_free(&jr);
    json_init(&jr);
    api_service_operation_filter_error_json(&jr, err);
    size_t n = api_json_status(response, response_max, "400 Bad Request",
                               &jr);
    json_free(&jr);
    return n;
}
