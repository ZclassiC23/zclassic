/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * API controller unit tests — routing, input validation, edge cases,
 * and supply calculation correctness.
 *
 * The cases live in test_api_*.c, one file per API area, and share the
 * fixtures declared in test/api_test_fixtures.h. This file only runs them,
 * in the order they were written in: the areas exercise process-global
 * controller state (api_set_state, the progress store, the error ring), so
 * the sequence is part of the contract. */

#include "test/api_test_fixtures.h"

int test_api(void)
{
    int failures = 0;

    failures += api_query_filters_focused_tests();
    failures += api_controller_supervision_focused_tests();
    failures += api_http_contract_focused_tests();
    failures += api_znam_routes_focused_tests();
    failures += api_rest_index_focused_tests();
    failures += api_catalog_focused_tests();
    failures += api_transaction_type_focused_tests();
    failures += api_openapi_focused_tests();
    failures += api_route_table_focused_tests();
    failures += api_status_focused_tests();
    failures += api_health_gate_focused_tests();
    failures += api_supply_focused_tests();
    failures += api_resource_reads_focused_tests();
    failures += api_access_focused_tests();

    return failures;
}
