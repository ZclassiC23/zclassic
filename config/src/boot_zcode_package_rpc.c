/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Route package pin plan/commit through the resident store owner. */

#include "config/boot_zcode_dht.h"

#include "command/native_command.h"
#include "json/json.h"
#include "rpc/server.h"

static const struct json_value *package_rpc_input(
    const struct json_value *params)
{
    const struct json_value *first =
        params && json_size(params) ? json_at(params, 0) : NULL;
    return first && first->type == JSON_OBJ ? first : NULL;
}

/* Recovery includes orphan GC, so one-shot clients must not open the live
 * files behind the daemon's in-memory CAS view. Execute pin work here. */
static bool package_pin_rpc(const struct json_value *params, bool help,
                            struct json_value *result, bool pinned)
{
    if (help) {
        json_set_str(result,
                     pinned ? "zcode_package_pin {root,mode,plan_token?}"
                            : "zcode_package_unpin {root,mode,plan_token?}");
        return true;
    }
    const struct json_value *input = package_rpc_input(params);
    if (!input || json_get(input, "datadir")) {
        json_set_object(result);
        json_push_kv_bool(result, "ok", false);
        json_push_kv_str(result, "code", "INVALID_INPUT");
        json_push_kv_str(result, "phase", "validate");
        json_push_kv_str(result, "message",
                         "one input object without datadir is required");
        return true;
    }

    struct zcl_command_request request = { .input = input };
    struct zcl_command_reply reply;
    zcl_command_reply_init(&reply, "zcl.zcode_package_pin.v1");
    if (pinned)
        zcl_native_handle_zcode_package_pin(&request, &reply);
    else
        zcl_native_handle_zcode_package_unpin(&request, &reply);

    bool passed = reply.status == ZCL_COMMAND_STATUS_PASSED &&
                  reply.exit_code == ZCL_COMMAND_EXIT_OK;
    json_set_object(result);
    json_push_kv_bool(result, "ok", passed);
    if (passed) {
        json_push_kv(result, "data", &reply.data);
    } else {
        json_push_kv_str(result, "code", reply.error.code[0]
                                            ? reply.error.code
                                            : "PIN_REFUSED");
        json_push_kv_str(result, "phase", reply.error.phase[0]
                                             ? reply.error.phase
                                             : "execute");
        json_push_kv_str(result, "message", reply.error.message[0]
                                               ? reply.error.message
                                               : "resident pin failed");
        json_push_kv_bool(result, "retryable", reply.error.retryable);
        json_push_kv_bool(result, "mutated", reply.error.mutated);
    }
    zcl_command_reply_free(&reply);
    return true;
}

static bool package_pin(const struct json_value *params, bool help,
                        struct json_value *result)
{
    return package_pin_rpc(params, help, result, true);
}

static bool package_unpin(const struct json_value *params, bool help,
                          struct json_value *result)
{
    return package_pin_rpc(params, help, result, false);
}

void boot_zcode_package_register_rpc(struct rpc_table *table)
{
    const struct rpc_command commands[] = {
        { "zcode", "zcode_package_pin", package_pin, true },
        { "zcode", "zcode_package_unpin", package_unpin, true },
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        rpc_table_must_append(table, &commands[i]);
}
