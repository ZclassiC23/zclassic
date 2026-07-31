/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The `ops.telemetry.network.*` controller — four leaves, one job each.
 *
 * This file is deliberately the thinnest layer in the domain. It picks a
 * snapshot and a view, hands both to telemetry_render(), and attaches the
 * document. It reads no global, decides no health, and names no field: the
 * field names live once in util/telemetry/network_fields.def, the values come
 * from network_dump_state_fill(), and the verdict comes from the one render
 * layer every domain shares. If anything here starts to look like logic, it
 * belongs in the collector.
 *
 * The four leaves are four VIEWS of one snapshot, not four data sources:
 *
 *   summary    every group at the summary tier — the connectivity posture
 *   peers      the `peers` group at full detail
 *   tor        the `tor` group at full detail
 *   transport  the `transport` group at full detail
 *
 * Health is evaluated over the WHOLE snapshot on every one of them (see
 * telemetry_render.c invariant 2), so `tor` cannot report ok while the peer
 * floor is broken. The group filter prunes what is DISPLAYED, never what is
 * judged.
 *
 * NEXT-COMMAND RULE, stated because breaking it destroys the entire reply: a
 * `next[]` entry naming the command currently being served makes
 * push_next_array set ok=false and serialize_reply return 0, which the CLI
 * misreports as RESPONSE_BUDGET_EXCEEDED over an empty document. Every next[]
 * below points at a SIBLING or at `ops state`, never at its own path.
 */

#include "command/native_command.h"

#include "json/json.h"
#include "kernel/command_registry.h"
#include "services/network_telemetry.h"
#include "util/telemetry_render.h"
#include "util/telemetry_snapshots.h"

/* Take the snapshot, render it, attach it. `only_group` is NULL for the
 * whole-domain view. */
static void tn_render(struct zcl_command_reply *reply,
                      enum telemetry_view view, const char *only_group)
{
    /* Zero-init is load-bearing: TELEMETRY_UNSET == 0, so a leaf the collector
     * forgets renders as a counted provider defect instead of a plausible 0. */
    struct network_snapshot snap = {0};
    if (!network_dump_state_fill(&snap)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "FILL_FAILED",
                               "execute", false, false,
                               "the network telemetry snapshot could not be "
                               "collected", "network");
        return;
    }
    struct json_value doc;
    json_init(&doc);
    if (!telemetry_render(&g_network_schema, &snap, view, only_group, &doc)) {
        json_free(&doc);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "RENDER_FAILED",
                               "serialize", false, false,
                               "the network telemetry document could not be "
                               "rendered", "network");
        return;
    }
    /* The reply arrives holding an empty object; replace it wholesale rather
     * than nesting the document under a key, so the telemetry schema IS the
     * reply body. Freeing first is what keeps that from leaking. */
    json_free(&reply->data);
    reply->data = doc; /* ownership moves to the reply */
}

void zcl_native_handle_telemetry_network_summary(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    if (!reply)
        return;
    tn_render(reply, TLV_SUMMARY, NULL);
    if (reply->status != ZCL_COMMAND_STATUS_PASSED)
        return;
    (void)zcl_command_reply_add_next(
        reply, "ops.telemetry.network.peers", "{}",
        "the full peer aggregate behind the summary counts");
}

void zcl_native_handle_telemetry_network_peers(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    if (!reply)
        return;
    tn_render(reply, TLV_FULL, "peers");
    if (reply->status != ZCL_COMMAND_STATUS_PASSED)
        return;
    /* The aggregate stops here by construction — the peer set is unbounded and
     * the table has no array leaf — so the per-peer listing has to be named. */
    /* `explain` is deliberately NOT passed here even though ops.state accepts
     * it: the registry's input validator types every key it does not name
     * explicitly as a string, and `explain` is not in that list, so a JSON
     * `true` fails validation — which invalidates the whole next[] array and
     * turns this reply into a 296-byte total loss. Verified by the byte count
     * this leaf's test prints. */
    (void)zcl_command_reply_add_next(
        reply, "ops.state", "{\"subsystem\":\"connman\"}",
        "per-peer detail and the addnode dial ledger, which this aggregate "
        "deliberately does not carry");
}

void zcl_native_handle_telemetry_network_tor(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    if (!reply)
        return;
    tn_render(reply, TLV_FULL, "tor");
    if (reply->status != ZCL_COMMAND_STATUS_PASSED)
        return;
    (void)zcl_command_reply_add_next(
        reply, "ops.state", "{\"subsystem\":\"explorer\"}",
        "the clearnet/onion serving posture this onion state feeds");
}

void zcl_native_handle_telemetry_network_transport(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    if (!reply)
        return;
    tn_render(reply, TLV_FULL, "transport");
    if (reply->status != ZCL_COMMAND_STATUS_PASSED)
        return;
    (void)zcl_command_reply_add_next(
        reply, "ops.state", "{\"subsystem\":\"transport\"}",
        "the per-connection transport mode behind these counts");
}
