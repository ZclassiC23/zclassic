/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Small nested contracts used by zcl.public_status.v1. Keeping them here
 * avoids turning the first-call agent summary into a catch-all formatting file.
 */

#include "controllers/agent_operator_contracts.h"

#include "json/json.h"
#include "services/legacy_mirror_sync_service.h"

#include <string.h>

static bool agent_mirror_operator_action_required(
    const struct legacy_mirror_sync_stats *mirror)
{
    if (!mirror)
        return false;
    return legacy_mirror_sync_blocker_is_active(mirror) &&
           legacy_mirror_sync_blocker_should_surface(mirror, false);
}

static bool agent_operator_latch_is_mirror_disagreement(const char *detail)
{
    if (!detail || !detail[0])
        return false;
    return strstr(detail, "chain_advance_hash-disagreement") ||
           strstr(detail, "hash-disagreement") ||
           strstr(detail, "mirror.divergence_located");
}

bool agent_operator_latch_suppressed_by_mirror(
    bool active,
    const char *detail,
    const struct legacy_mirror_sync_stats *mirror)
{
    return active && mirror && mirror->enabled &&
           agent_operator_latch_is_mirror_disagreement(detail) &&
           !agent_mirror_operator_action_required(mirror);
}

void agent_push_operator_latch_contract_json(
    struct json_value *out,
    const struct agent_operator_latch_contract_view *view)
{
    if (!out || !view)
        return;

    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", "zcl.operator_latch.v1");
    json_push_kv_int(&obj, "schema_version", 1);
    json_push_kv_bool(&obj, "active", view->active);
    json_push_kv_bool(&obj, "operator_action_required",
                      view->operator_action_required);
    json_push_kv_bool(&obj, "recovered_this_call",
                      view->recovered_this_call);
    json_push_kv_bool(&obj, "suppressed_by_mirror_contract",
                      view->suppressed_by_mirror_contract);
    json_push_kv_int(&obj, "since_unix", view->since_unix);
    json_push_kv_str(&obj, "detail", view->detail);
    json_push_kv_str(&obj, "state_tool",
                     "zcl_state subsystem=condition_engine");
    json_push_kv_str(&obj, "native_state_command",
                     "zclassic23 dumpstate condition_engine");
    json_push_kv_str(&obj, "semantics",
                     "active means EV_OPERATOR_NEEDED is latched; mirror "
                     "classification is advisory and never clears the "
                     "operator-action requirement on an observation path");
    json_push_kv(out, "operator_latch", &obj);
    json_free(&obj);
}

void agent_push_condition_summary_contract_json(
    struct json_value *out,
    const struct agent_condition_summary_contract_view *view)
{
    if (!out || !view)
        return;

    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", "zcl.condition_engine_summary.v1");
    json_push_kv_int(&obj, "schema_version", 1);
    json_push_kv_int(&obj, "active_count", view->active_count);
    json_push_kv_int(&obj, "unresolved_count", view->unresolved_count);
    json_push_kv_int(&obj, "unresolved_critical_count",
                     view->unresolved_critical_count);
    json_push_kv_str(&obj, "state_tool",
                     "zcl_state subsystem=condition_engine");
    json_push_kv_str(&obj, "native_state_command",
                     "zclassic23 dumpstate condition_engine");
    json_push_kv_str(&obj, "semantics",
                     "summary only; use state_tool for the registered "
                     "condition list, attempts, thresholds, and detail");
    json_push_kv(out, "conditions", &obj);
    json_free(&obj);
}
