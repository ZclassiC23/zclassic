/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: native read-only list/show adapters for transaction discovery.
 */

#include "command/native_command.h"

#include "controllers/transaction_type_catalog.h"
#include "json/json.h"

#include <string.h>

static void tt_csv_array(const char *csv, struct json_value *out)
{
    json_set_array(out);
    if (!csv || !csv[0])
        return;
    const char *cursor = csv;
    while (*cursor) {
        const char *end = strchr(cursor, ',');
        size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (len > 0 && len < ZCL_COMMAND_MAX_PATH) {
            char value[ZCL_COMMAND_MAX_PATH];
            memcpy(value, cursor, len);
            value[len] = 0;
            struct json_value item;
            json_init(&item);
            json_set_str(&item, value);
            (void)json_push_back(out, &item);
            json_free(&item);
        }
        if (!end)
            break;
        cursor = end + 1;
    }
}

static bool tt_command_contract(
    const struct zcl_command_registry *registry, const char *role,
    const char *path, struct json_value *contracts,
    bool *all_ready, bool *needs_money, bool *needs_owner)
{
    if (!path || !path[0])
        return false;
    const struct zcl_command_spec *spec =
        zcl_command_registry_find(registry, path, NULL);
    if (!spec) {
        *all_ready = false;
        return false;
    }
    struct json_value item;
    json_init(&item);
    json_set_object(&item);
    (void)json_push_kv_str(&item, "role", role);
    (void)json_push_kv_str(&item, "command", spec->path);
    (void)json_push_kv_str(&item, "availability",
                           zcl_command_availability_name(spec->availability));
    (void)json_push_kv_str(&item, "summary", spec->summary);
    (void)json_push_kv_str(&item, "semantics", spec->semantics);
    (void)json_push_kv_str(&item, "input_schema", spec->input_schema);
    (void)json_push_kv_str(&item, "output_schema", spec->output_schema);
    struct json_value keys;
    json_init(&keys);
    tt_csv_array(spec->input_keys, &keys);
    (void)json_push_kv(&item, "allowed_keys", &keys);
    json_free(&keys);
    (void)json_push_kv_str(&item, "example", spec->example);
    (void)json_push_kv_str(&item, "effect",
                           zcl_command_effect_name(spec->effect));
    (void)json_push_kv_str(&item, "risk", zcl_command_risk_name(spec->risk));
    (void)json_push_kv_str(&item, "authority",
                           zcl_command_authority_name(spec->authority));
    (void)json_push_kv_str(&item, "confirmation",
                           zcl_command_confirmation_name(spec->confirmation));
    (void)json_push_back(contracts, &item);
    json_free(&item);
    if (spec->availability != ZCL_COMMAND_READY)
        *all_ready = false;
    if (spec->risk == ZCL_COMMAND_RISK_WALLET)
        *needs_money = true;
    if (spec->authority == ZCL_COMMAND_AUTH_OWNER)
        *needs_owner = true;
    return true;
}

static bool tt_is_primary_command(
    const struct zcl_transaction_type_contract *type,
    const char *path, size_t len)
{
    const char *primary[] = { type->builder_command, type->commit_command,
                              type->inspect_command };
    for (size_t i = 0; i < sizeof(primary) / sizeof(primary[0]); i++)
        if (primary[i] && strlen(primary[i]) == len &&
            memcmp(primary[i], path, len) == 0)
            return true;
    return false;
}

static size_t tt_component_contracts(
    const struct zcl_command_registry *registry,
    const struct zcl_transaction_type_contract *type,
    struct json_value *contracts, bool *all_ready,
    bool *needs_money, bool *needs_owner)
{
    const char *cursor = type->component_commands_csv;
    size_t count = 0;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, ',');
        size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (len > 0 && len < ZCL_COMMAND_MAX_PATH &&
            !tt_is_primary_command(type, cursor, len)) {
            char path[ZCL_COMMAND_MAX_PATH];
            memcpy(path, cursor, len);
            path[len] = 0;
            count += tt_command_contract(registry, "component", path,
                contracts, all_ready, needs_money, needs_owner) ? 1u : 0u;
        }
        if (!end)
            break;
        cursor = end + 1;
    }
    return count;
}

static const char *tt_agent_decision(
    const struct zcl_transaction_type_contract *type)
{
    if (strcmp(type->availability, "ready") == 0)
        return "discover_schema_then_plan_then_request_owner_commit";
    if (strcmp(type->availability, "process_only") == 0)
        return "receive_validate_and_inspect_only";
    if (strcmp(type->availability, "contained") == 0)
        return "refuse_outside_declared_network_policy";
    return "refuse_not_implemented";
}

void zcl_native_handle_transaction_types_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    if (!zcl_transaction_types_index_json(&reply->data))
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "CATALOG_FAILED",
                               "render", false, false,
                               "transaction type catalog could not be rendered",
                               "");
}

void zcl_native_handle_transaction_type_show(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *id = json_get_str(json_get(request->input, "type"));
    if (!id || !id[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_TYPE",
                               "normalize", false, false,
                               "type is required", "");
        return;
    }
    if (!zcl_transaction_type_show_json(id, &reply->data)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "UNKNOWN_TYPE",
                               "resolve", false, false,
                               "no such semantic transaction type", id);
        (void)zcl_command_reply_add_next(reply, "app.transaction-types.list",
                                         "{}", "list transaction types");
    }
}

void zcl_native_handle_transaction_type_guide(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *id = json_get_str(json_get(request->input, "type"));
    if (!id || !id[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_TYPE",
                               "normalize", false, false,
                               "type is required", "");
        return;
    }
    const struct zcl_transaction_type_contract *type =
        zcl_transaction_type_find(id);
    if (!type) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED, "UNKNOWN_TYPE",
                               "resolve", false, false,
                               "no such semantic transaction type", id);
        (void)zcl_command_reply_add_next(reply, "app.transaction-types.list",
                                         "{}", "list transaction types");
        return;
    }
    const struct zcl_command_registry *registry = request->context
        ? request->context->registry : NULL;
    if (!registry) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "REGISTRY_UNAVAILABLE", "join", true, false,
                               "live command registry is unavailable", id);
        return;
    }

    json_set_object(&reply->data);
    (void)json_push_kv_str(&reply->data, "schema",
                           ZCL_TRANSACTION_TYPE_GUIDE_SCHEMA);
    struct json_value type_json;
    json_init(&type_json);
    (void)zcl_transaction_type_show_json(id, &type_json);
    (void)json_push_kv(&reply->data, "transaction_type", &type_json);
    json_free(&type_json);
    (void)json_push_kv_str(&reply->data, "agent_decision",
                           tt_agent_decision(type));

    bool all_ready = true;
    bool needs_money = type->commit_command[0] != 0;
    bool needs_owner = false;
    size_t count = 0;
    struct json_value contracts;
    json_init(&contracts);
    json_set_array(&contracts);
    count += tt_command_contract(registry, "builder", type->builder_command,
        &contracts, &all_ready, &needs_money, &needs_owner) ? 1u : 0u;
    count += tt_command_contract(registry, "commit", type->commit_command,
        &contracts, &all_ready, &needs_money, &needs_owner) ? 1u : 0u;
    count += tt_component_contracts(registry, type, &contracts, &all_ready,
                                    &needs_money, &needs_owner);
    count += tt_command_contract(registry, "inspect", type->inspect_command,
        &contracts, &all_ready, &needs_money, &needs_owner) ? 1u : 0u;
    (void)json_push_kv(&reply->data, "command_contracts", &contracts);
    json_free(&contracts);
    (void)json_push_kv_int(&reply->data, "command_contract_count",
                           (int64_t)count);
    bool can_execute = strcmp(type->availability, "ready") == 0 &&
        type->builder_command[0] && all_ready;
    (void)json_push_kv_bool(&reply->data, "can_execute", can_execute);
    (void)json_push_kv_bool(&reply->data, "money_snapshot_required",
                            needs_money);
    (void)json_push_kv_bool(&reply->data, "owner_authorization_required",
                            needs_owner);
    (void)json_push_kv_str(&reply->data, "money_snapshot_command",
        needs_money ? "metaverse.agent.money" : "");
    (void)json_push_kv_str(&reply->data, "proof_command",
                           "make transaction-lab-proof");
    (void)json_push_kv_str(&reply->data, "focused_test_group",
                           type->test_group);

    static const char *const checklist[] = {
        "reject planned, contained-network, stale, unknown, or conflicted state",
        "discover current schemas; never infer flags or wallet scope",
        "obtain a current identity-bound money snapshot before any spend",
        "plan first and preserve outputs, fee, expiry, snapshot, and idempotency",
        "commit only after explicit owner authorization",
        "inspect txid or operation state before recording redacted evidence",
    };
    struct json_value safety;
    json_init(&safety);
    json_set_array(&safety);
    for (size_t i = 0; i < sizeof(checklist) / sizeof(checklist[0]); i++) {
        struct json_value item;
        json_init(&item);
        json_set_str(&item, checklist[i]);
        (void)json_push_back(&safety, &item);
        json_free(&item);
    }
    (void)json_push_kv(&reply->data, "safety_checklist", &safety);
    json_free(&safety);
}
