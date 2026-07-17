/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM-seed controller: the native command handlers for
 * `ops.debug.rom_seed.*` — operator visibility and control over free-tier
 * P2P delivery of the ROM/sync artifacts (docs/ROM_DELIVERY.md). Each
 * handler parses its input, delegates to net/rom_seed_policy.h or
 * net/rom_seed_ledger.h, and renders one bounded JSON document into
 * reply->data (parse -> authorize -> call one service, per the controller
 * shape). Every failure path sets a structured error body.
 *
 * Nested under `ops.debug` rather than a new top-level `ops.*` branch —
 * the top-level ops menu is within its ~1600-byte listing budget
 * (ZCL_COMMAND_BRANCH_BUDGET, proven by test_branch_menus_shallow) already
 * with no room for another direct child; see the identical precedent
 * comment above `ops.debug.dash` in config/commands/ops.def. Named
 * `ops.debug.rom_seed.*` rather than colliding with `ops.debug.rom` — that
 * path is the unrelated ROM *compile*-fold telemetry leaf
 * (app/jobs/src/rom_compile_status.c). The underscore form also matches
 * the sibling seed/fetch lanes' dumpstate subsystem names (`rom_seed`,
 * `rom_fetch`), so the three surfaces read as one family without
 * colliding. */

#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "net/rom_seed_policy.h"
#include "net/rom_seed_ledger.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"

#include <string.h>

/* Every leaf in this file takes empty input (the registry rejects unknown
 * keys before the handler runs — see zcl_command_registry_input_validate)
 * and every operation it performs (policy read, enable/disable, ledger
 * rollup over a possibly-empty table) always succeeds, so there is no
 * failure path here today to route through zcl_command_reply_fail. A
 * future filtered variant (e.g. --artifact_id) would add one. */

/* Render the policy+counters body (identical shape to
 * rom_seed_policy_dump_state_json) plus this leaf's ledger/registry
 * sections into `reply->data`. Shared by status/enable/disable so the
 * mutating leaves return the exact same settled-state shape
 * account.suspend/unsuspend return after their mutation. */
static void rs_render_status(struct zcl_command_reply *reply)
{
    struct json_value policy_body;
    json_init(&policy_body);
    (void)rom_seed_policy_dump_state_json(&policy_body, NULL);
    json_copy(&reply->data, &policy_body);
    json_free(&policy_body);

    rom_seed_ledger_t *ledger = rom_seed_ledger_global();
    struct json_value ledger_obj;
    json_init(&ledger_obj);
    json_set_object(&ledger_obj);
    (void)json_push_kv_bool(&ledger_obj, "available", ledger != NULL);
    (void)json_push_kv_int(&ledger_obj, "rows",
                           ledger ? rom_seed_ledger_row_count(ledger) : 0);
    (void)json_push_kv(&reply->data, "ledger", &ledger_obj);
    json_free(&ledger_obj);
}

/* ── ops.rom_seed.status ────────────────────────────────────────────── */
void zcl_native_handle_rom_seed_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    rs_render_status(reply);
}

/* Shared body for enable/disable: flip the policy, render the settled
 * state. rom_seed_policy_set_enabled always succeeds (no bounds to
 * violate on a bool). */
static void rs_set_enabled(struct zcl_command_reply *reply, bool enabled)
{
    (void)rom_seed_policy_set_enabled(enabled);
    rs_render_status(reply);
}

/* ── ops.rom_seed.enable ────────────────────────────────────────────── */
void zcl_native_handle_rom_seed_enable(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    rs_set_enabled(reply, true);
}

/* ── ops.rom_seed.disable ───────────────────────────────────────────── */
void zcl_native_handle_rom_seed_disable(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    rs_set_enabled(reply, false);
}

/* ── ops.rom_seed.artifacts ─────────────────────────────────────────── */
/* Lists every artifact this node has ever served, per-artifact stats
 * folded from the local serve log. The seed engine's artifact REGISTRY
 * (dumpstate `rom_seed` — names, digests, prices-are-N/A-for-free-tier) is
 * a sibling lane's surface, not wired into this build; the `registry`
 * section here is an honest "not available" until that lane lands rather
 * than a guess at its shape. */
#define RS_ARTIFACTS_MAX 64

void zcl_native_handle_rom_seed_artifacts(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    struct json_value registry_obj;
    json_init(&registry_obj);
    json_set_object(&registry_obj);
    (void)json_push_kv_bool(&registry_obj, "available", false);
    (void)json_push_kv_str(&registry_obj, "note",
        "artifact registry (dumpstate rom_seed) is not wired into this "
        "build; showing serve-log-derived stats only");
    (void)json_push_kv(&reply->data, "registry", &registry_obj);
    json_free(&registry_obj);

    rom_seed_ledger_t *ledger = rom_seed_ledger_global();
    struct json_value stats_arr;
    json_init(&stats_arr);
    json_set_array(&stats_arr);
    int64_t count = 0;

    if (ledger) {
        uint8_t ids[RS_ARTIFACTS_MAX][ROM_SEED_LEDGER_ARTIFACT_ID_LEN];
        size_t n = rom_seed_ledger_distinct_artifacts(ledger, ids,
                                                       RS_ARTIFACTS_MAX);
        for (size_t i = 0; i < n; i++) {
            struct rom_seed_ledger_artifact_stats st;
            if (!rom_seed_ledger_artifact_stats(ledger, ids[i], &st))
                continue;
            char hex[2 * ROM_SEED_LEDGER_ARTIFACT_ID_LEN + 1];
            HexStr(ids[i], ROM_SEED_LEDGER_ARTIFACT_ID_LEN, false, hex,
                  sizeof(hex));
            struct json_value item;
            json_init(&item);
            json_set_object(&item);
            (void)json_push_kv_str(&item, "artifact_id", hex);
            (void)json_push_kv_int(&item, "total_bytes_served",
                                   (int64_t)st.total_bytes_served);
            (void)json_push_kv_int(&item, "total_chunks_served",
                                   (int64_t)st.total_chunks_served);
            (void)json_push_kv_int(&item, "distinct_peers",
                                   (int64_t)st.distinct_peers);
            (void)json_push_kv_int(&item, "sessions", (int64_t)st.sessions);
            (void)json_push_kv_int(&item, "last_served_unix",
                                   st.last_served_unix);
            (void)json_push_back(&stats_arr, &item);
            json_free(&item);
            count++;
        }
    }
    (void)json_push_kv(&reply->data, "seed_stats", &stats_arr);
    json_free(&stats_arr);
    (void)json_push_kv_int(&reply->data, "seed_stats_count", count);
    (void)json_push_kv_bool(&reply->data, "ledger_available", ledger != NULL);
}
