/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_AGENT_OPERATOR_CONTRACTS_H
#define ZCL_CONTROLLERS_AGENT_OPERATOR_CONTRACTS_H

#include <stdbool.h>
#include <stdint.h>

struct json_value;
struct legacy_mirror_sync_stats;

struct agent_operator_latch_contract_view {
    bool active;
    bool operator_action_required;
    bool recovered_this_call;
    bool suppressed_by_mirror_contract;
    int64_t since_unix;
    const char *detail;
};

struct agent_condition_summary_contract_view {
    int active_count;
    int unresolved_count;
};

bool agent_operator_latch_suppressed_by_mirror(
    bool active,
    const char *detail,
    const struct legacy_mirror_sync_stats *mirror);

void agent_push_operator_latch_contract_json(
    struct json_value *out,
    const struct agent_operator_latch_contract_view *view);

void agent_push_condition_summary_contract_json(
    struct json_value *out,
    const struct agent_condition_summary_contract_view *view);

#endif /* ZCL_CONTROLLERS_AGENT_OPERATOR_CONTRACTS_H */
