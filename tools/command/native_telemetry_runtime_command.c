/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * `ops.telemetry.runtime.{services,threads,resources}` — the three leaves over
 * the typed `runtime` snapshot.
 *
 * These handlers are deliberately almost empty. They pick a group and a view
 * and call telemetry_render(); they name no field, decide no health, and build
 * no document. Every field name in this domain lives in ONE place
 * (util/telemetry/runtime_fields.def) and every verdict comes from that table's
 * rules through the single evaluator, so there is nothing here for a rename or
 * a threshold change to drift away from.
 *
 * WHY THREE HANDLERS OVER ONE SNAPSHOT. The collector fills the whole domain on
 * every call and the render layer's `completeness` is a property of the whole
 * snapshot, not of the slice the caller asked to see. A per-command partial
 * fill would report every leaf outside the caller's group as a provider
 * defect, which is the opposite of what those counters are for. So: fill
 * everything, render one group, and let `leaves_total` vs `leaves_rendered`
 * state the difference.
 *
 * NODE-DOWN IS A TRANSPORT FAILURE, NOT TELEMETRY. When nothing answers, the
 * leaf fails closed with a retryable NODE_UNAVAILABLE and points at
 * core.status. Returning a document whose every leaf is unavailable would be
 * technically honest and operationally useless, and it would also be a far
 * larger reply than the healthy one.
 */

#include "command/native_command.h"

#include "json/json.h"
#include "kernel/command_registry.h"
#include "services/runtime_telemetry.h"
#include "util/telemetry_render.h"
#include "util/telemetry_snapshots.h"

#include <string.h>

/* The one body all three leaves share. `group` names the field table group to
 * render; it is the only thing that differs between them. */
static void telemetry_runtime_leaf(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply,
                                   const char *group)
{
    if (!request || !reply)
        return;

    /* A fresh CLI process has no RPC client until this runs. */
    zcl_native_bridge_ensure_rpc();

    /* Zero-initialized, so any leaf the collector forgets renders as a counted
     * provider defect rather than a plausible zero. */
    struct runtime_snapshot snap = {0};
    const char *why = "";
    if (!runtime_dump_state_fill(&snap, &why)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "runtime telemetry is read live from the "
                               "running node and no dumpstate subsystem "
                               "answered", why);
        /* Never point a next[] at the command being served: push_next_array
         * rejects a self-reference and drops the WHOLE envelope. */
        (void)zcl_command_reply_add_next(reply, "core.status", "{}",
                                         "confirm the node is running");
        return;
    }

    enum telemetry_view view = telemetry_view_parse(request->view, NULL, NULL);
    if (!telemetry_render(&g_runtime_schema, &snap, view, group,
                          &reply->data)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "RENDER_FAILED",
                               "serialize", false, false,
                               "the runtime telemetry snapshot could not be "
                               "rendered into a complete document", group);
        return;
    }
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

void zcl_native_handle_ops_telemetry_runtime_services(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    telemetry_runtime_leaf(request, reply, "services");
}

void zcl_native_handle_ops_telemetry_runtime_threads(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    telemetry_runtime_leaf(request, reply, "threads");
}

void zcl_native_handle_ops_telemetry_runtime_resources(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    telemetry_runtime_leaf(request, reply, "resources");
}
