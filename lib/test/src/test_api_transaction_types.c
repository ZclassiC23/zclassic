/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Contract tests for the semantic transaction-type resource.
 */

#include "test/api_test_fixtures.h"

#include "config/command_catalog.h"
#include "controllers/transaction_type_catalog.h"
#include "kernel/command_registry.h"

#include <string.h>

static bool command_exists(const struct zcl_command_registry *registry,
                           const char *path)
{
    return !path || !path[0] ||
           zcl_command_registry_find(registry, path, NULL) != NULL;
}

static bool component_commands_exist(
    const struct zcl_command_registry *registry, const char *csv)
{
    if (!csv || !csv[0])
        return true;
    const char *cursor = csv;
    while (*cursor) {
        const char *start = cursor;
        while (*cursor && *cursor != ',')
            cursor++;
        size_t len = (size_t)(cursor - start);
        char path[128];
        if (len == 0 || len >= sizeof(path))
            return false;
        memcpy(path, start, len);
        path[len] = '\0';
        if (!command_exists(registry, path))
            return false;
        if (*cursor == ',')
            cursor++;
    }
    return true;
}

int api_transaction_type_focused_tests(void)
{
    int failures = 0;

    printf("api: transaction type registry references real native leaves... ");
    {
        const struct zcl_command_registry *registry = zcl_command_catalog();
        size_t count = 0;
        const struct zcl_transaction_type_contract *types =
            zcl_transaction_type_catalog(&count);
        bool ok = registry && types && count >= 30;
        for (size_t i = 0; ok && i < count; i++) {
            ok = types[i].id && types[i].id[0] &&
                 types[i].family && types[i].family[0] &&
                 types[i].availability && types[i].availability[0] &&
                 types[i].lab_case_id &&
                 strcmp(types[i].lab_case_id, types[i].id) == 0 &&
                 types[i].proof_level && types[i].proof_level[0] &&
                 command_exists(registry, types[i].builder_command) &&
                 command_exists(registry, types[i].commit_command) &&
                 command_exists(registry, types[i].inspect_command) &&
                 component_commands_exist(
                     registry, types[i].component_commands_csv);
            for (size_t j = 0; ok && j < i; j++)
                ok = strcmp(types[i].id, types[j].id) != 0;
        }
        ok = ok && zcl_command_registry_find(
            registry, "app.transaction-types.list", NULL) != NULL;
        ok = ok && zcl_command_registry_find(
            registry, "app.transaction-types.show", NULL) != NULL;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: transaction type collection separates readiness and proof... ");
    {
        static uint8_t response[262144];
        size_t n = api_handle_request("GET", "/api/v1/transaction-types",
                                      NULL, 0, response, sizeof(response));
        const char *body = api_test_body(response, n, sizeof(response));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          ZCL_TRANSACTION_TYPES_INDEX_SCHEMA) == 0;
        const struct json_value *types = json_get(&root, "transaction_types");
        int64_t count = json_get_int(json_get(&root,
                                              "transaction_type_count"));
        ok = ok && types && types->type == JSON_ARR &&
             count == (int64_t)json_size(types) && count >= 30;
        ok = ok && count == json_get_int(json_get(&root, "ready_count")) +
                               json_get_int(json_get(&root,
                                                     "process_only_count")) +
                               json_get_int(json_get(&root,
                                                     "contained_count")) +
                               json_get_int(json_get(&root, "planned_count"));
        ok = ok && count == 34 &&
             json_get_int(json_get(&root, "demonstrated_count")) == 34 &&
             json_get_int(json_get(&root, "blocked_count")) == 0 &&
             json_get_int(json_get(&root, "chain_confirmed_count")) == 22 &&
             json_get_int(json_get(&root,
                                   "mainnet_live_proven_count")) == 0 &&
             json_get_int(json_get(&root, "proof_test_group_count")) == 19 &&
             json_get_bool(json_get(&root, "fully_demonstrated"));
        const struct json_value *transparent =
            api_test_find_str_field(types, "id", "transparent_t_to_t");
        const struct json_value *coinbase =
            api_test_find_str_field(types, "id", "coinbase_reward");
        const struct json_value *store =
            api_test_find_str_field(types, "id", "store_shielded_payment");
        const struct json_value *market =
            api_test_find_str_field(types, "id", "market_purchase");
        const struct json_value *znam =
            api_test_find_str_field(types, "id", "znam_register");
        const struct json_value *blog =
            api_test_find_str_field(types, "id", "blog_anchor");
        ok = ok && transparent &&
             strcmp(json_get_str(json_get(transparent, "builder_command")),
                    "core.wallet.transaction.send") == 0 &&
             strcmp(json_get_str(json_get(transparent, "proof_level")),
                    "simnet_confirmed") == 0;
        ok = ok && coinbase &&
             strcmp(json_get_str(json_get(coinbase, "availability")),
                    "process_only") == 0;
        ok = ok && store &&
             strcmp(json_get_str(json_get(store, "network_policy")),
                    "isolated_non_mainnet_only") == 0;
        ok = ok && market &&
             strcmp(json_get_str(json_get(market, "availability")),
                    "ready") == 0 &&
             strcmp(json_get_str(json_get(market, "proof_level")),
                    "projection_verified") == 0;
        ok = ok && znam &&
             strcmp(json_get_str(json_get(znam, "proof_level")),
                    "simnet_confirmed") == 0;
        ok = ok && blog &&
             strcmp(json_get_str(json_get(blog, "availability")),
                    "contained") == 0 &&
             strcmp(json_get_str(json_get(blog, "proof_level")),
                    "simnet_confirmed") == 0 &&
             strcmp(json_get_str(json_get(blog, "builder_command")),
                    "app.blog.anchor") == 0;
        ok = ok && strstr(body, "private_key") == NULL &&
             strstr(body, "grant_token") == NULL &&
             strstr(body, "/home/") == NULL;
        json_free(&root);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: native transaction list fits its declared response budget... ");
    {
        const struct zcl_command_registry *registry = zcl_command_catalog();
        const struct zcl_command_spec *spec = registry ?
            zcl_command_registry_find(registry,
                                      "app.transaction-types.list", NULL) : NULL;
        struct zcl_command_context context = {
            .registry = registry,
            .granted_capabilities = ~(uint64_t)0,
            .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
        };
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        char output[ZCL_COMMAND_LIST_BUDGET + 1];
        enum zcl_command_exit exit_code = ZCL_COMMAND_EXIT_INTERNAL;
        size_t n = spec ? zcl_command_registry_execute_json(
            registry, spec, &context, &input, false, spec->path, "normal",
            0, 0, NULL, output, sizeof(output) - 1, &exit_code) : 0;
        json_free(&input);
        output[n < sizeof(output) ? n : sizeof(output) - 1] = '\0';
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && n <= ZCL_COMMAND_LIST_BUDGET &&
                  exit_code == ZCL_COMMAND_EXIT_OK &&
                  json_read(&root, output, n) &&
                  json_get_bool(json_get(&root, "ok")) &&
                  strcmp(json_get_str(json_get(&root, "data_schema")),
                         ZCL_TRANSACTION_TYPES_INDEX_SCHEMA) == 0;
        json_free(&root);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: one-call transaction guide joins exact live command contracts... ");
    {
        const struct zcl_command_registry *registry = zcl_command_catalog();
        const struct zcl_command_spec *spec = registry ?
            zcl_command_registry_find(registry,
                                      "app.transaction-types.guide", NULL)
            : NULL;
        struct zcl_command_context context = {
            .registry = registry,
            .granted_capabilities = ~(uint64_t)0,
            .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
        };
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        json_push_kv_str(&input, "type", "znam_register");
        char output[ZCL_COMMAND_LIST_BUDGET + 1];
        enum zcl_command_exit exit_code = ZCL_COMMAND_EXIT_INTERNAL;
        size_t n = spec ? zcl_command_registry_execute_json(
            registry, spec, &context, &input, false, spec->path, "normal",
            0, 0, NULL, output, sizeof(output) - 1, &exit_code) : 0;
        json_free(&input);
        output[n < sizeof(output) ? n : sizeof(output) - 1] = 0;
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && n <= ZCL_COMMAND_LIST_BUDGET &&
            exit_code == ZCL_COMMAND_EXIT_OK && json_read(&root, output, n) &&
            json_get_bool(json_get(&root, "ok"));
        const struct json_value *data = json_get(&root, "data");
        const struct json_value *type = data ?
            json_get(data, "transaction_type") : NULL;
        const struct json_value *contracts = data ?
            json_get(data, "command_contracts") : NULL;
        const struct json_value *builder = contracts ?
            api_test_find_str_field(contracts, "role", "builder") : NULL;
        ok = ok && data && type && contracts && builder &&
            strcmp(json_get_str(json_get(data, "schema")),
                   ZCL_TRANSACTION_TYPE_GUIDE_SCHEMA) == 0 &&
            strcmp(json_get_str(json_get(type, "id")), "znam_register") == 0 &&
            json_get_bool(json_get(data, "can_execute")) &&
            json_get_bool(json_get(data, "money_snapshot_required")) &&
            json_get_bool(json_get(data, "owner_authorization_required")) &&
            json_size(contracts) == 3 &&
            strcmp(json_get_str(json_get(builder, "command")),
                   "app.names.register") == 0 &&
            json_get(builder, "allowed_keys") &&
            json_size(json_get(builder, "allowed_keys")) > 0 &&
            strlen(json_get_str(json_get(builder, "input_schema"))) > 0 &&
            strlen(json_get_str(json_get(builder, "example"))) > 0 &&
            strstr(output, "private_key") == NULL &&
            strstr(output, "grant_token") == NULL;
        json_free(&root);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: transaction type member is exact and unknown ids 404... ");
    {
        static uint8_t response[262144];
        size_t n = api_handle_request(
            "GET", "/api/v1/transaction-types/zcode_release_anchor",
            NULL, 0, response, sizeof(response));
        const char *body = api_test_body(response, n, sizeof(response));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          ZCL_TRANSACTION_TYPE_SCHEMA) == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "id")),
                          "zcode_release_anchor") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "chain_encoding")),
                          "op_return_zanc_zcode_domain_root") == 0;
        ok = ok && api_test_array_has_str(json_get(&root,
                                                    "component_commands"),
                                           "zcode.release.prove");
        ok = ok && strcmp(json_get_str(json_get(&root, "evidence_status")),
                          "demonstrated") == 0;
        ok = ok && !json_get_bool(json_get(&root,
                                           "mainnet_live_proven"));
        json_free(&root);

        n = api_handle_request(
            "GET", "/api/v1/transaction-types/blog_anchor",
            NULL, 0, response, sizeof(response));
        body = api_test_body(response, n, sizeof(response));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok &&
             strcmp(json_get_str(json_get(&root, "availability")),
                    "contained") == 0 &&
             strcmp(json_get_str(json_get(&root, "proof_level")),
                    "simnet_confirmed") == 0 &&
             strcmp(json_get_str(json_get(&root, "builder_command")),
                    "app.blog.anchor") == 0 &&
             api_test_array_has_str(
                 json_get(&root, "supplemental_test_groups"),
                 "test_native_api_contract") &&
             api_test_array_has_str(
                 json_get(&root, "supplemental_test_groups"),
                 "test_simnet") &&
             api_test_array_has_str(
                 json_get(&root, "supplemental_test_groups"),
                 "test_transaction_intent");
        json_free(&root);

        n = api_handle_request(
            "GET", "/api/v1/transaction-types/market_purchase",
            NULL, 0, response, sizeof(response));
        body = api_test_body(response, n, sizeof(response));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "evidence_status")),
                          "demonstrated") == 0 &&
             strcmp(json_get_str(json_get(&root, "proof_level")),
                    "projection_verified") == 0 &&
             !json_get_bool(json_get(&root, "mainnet_live_proven"));
        json_free(&root);

        n = api_handle_request(
            "GET", "/api/v1/transaction-types/htlc_redeem",
            NULL, 0, response, sizeof(response));
        body = api_test_body(response, n, sizeof(response));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok &&
             strcmp(json_get_str(json_get(&root, "proof_level")),
                    "simnet_confirmed") == 0 &&
             strcmp(json_get_str(json_get(&root, "test_group")),
                    "test_swap_settlement") == 0 &&
             api_test_array_has_str(
                 json_get(&root, "supplemental_test_groups"),
                 "test_simnet_contract");
        json_free(&root);

        n = api_handle_request("GET",
                               "/api/v1/transaction-types/not_real",
                               NULL, 0, response, sizeof(response));
        response[n < sizeof(response) ? n : sizeof(response) - 1] = '\0';
        ok = ok && strstr((char *)response,
                          "HTTP/1.1 404 Not Found") != NULL;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
