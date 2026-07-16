/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the `dev.usage` tree — the agent's self-review surface
 * over the command interaction ledger (Phase D). Both are READ-ONLY: they only
 * fold the durable, content-free zcl.cmd_ledger.v1 records that the kernel sink
 * (util/command_ledger) appended for every prior dispatch, and never touch node
 * or wallet state. Read-only, so they compile into both builds; the catalog
 * binds them as READY only in a dev catalog (COMPAT in release).
 */

#include "command/native_command.h"

#include "json/json.h"
#include "kernel/command_registry.h"
#include "util/command_ledger.h"

/* Small shared failure projection: the ledger producer returned no document. */
static void usage_fail(struct zcl_command_reply *reply, const char *code,
                       const char *leaf)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INTERNAL, code, "produce", false,
                           false,
                           "command interaction ledger query could not be "
                           "produced",
                           leaf);
    (void)zcl_command_reply_add_next(
        reply, "discover.describe", "{\"path\":\"dev.usage\"}",
        "inspect the command-ledger self-review surface");
}

void zcl_native_handle_dev_usage_summary(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const struct json_value *in = request ? request->input : NULL;
    int64_t window_s = json_get_int(json_get(in, "window_s")); /* 0 == all */
    const char *leaf = json_get_str(json_get(in, "leaf"));     /* NULL == all */
    const struct json_value *top_v = json_get(in, "top");
    int top = top_v ? (int)json_get_int(top_v) : 0;            /* 0 == default */

    if (!command_ledger_summary(window_s, leaf, top, &reply->data)) {
        usage_fail(reply, "LEDGER_SUMMARY_FAILED", "dev.usage.summary");
        return;
    }
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

void zcl_native_handle_dev_usage_tail(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const struct json_value *in = request ? request->input : NULL;
    const struct json_value *n_v = json_get(in, "n");
    int n = n_v ? (int)json_get_int(n_v) : 0;                  /* 0 == default */
    const char *leaf = json_get_str(json_get(in, "leaf"));     /* NULL == all */

    if (!command_ledger_tail(n, leaf, &reply->data)) {
        usage_fail(reply, "LEDGER_TAIL_FAILED", "dev.usage.tail");
        return;
    }
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}
