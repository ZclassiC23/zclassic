/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: native read-only list/show adapters for transaction discovery.
 */

#include "command/native_command.h"

#include "controllers/transaction_type_catalog.h"
#include "json/json.h"

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
