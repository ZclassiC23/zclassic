/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
#include "event_agent_readiness.h"

#include "json/json.h"
#include "services/node_health_service.h"

static bool chain_serving_ready(bool serving, bool has_peers,
                                bool operator_needed, int gap,
                                int log_head_gap)
{
    return serving && has_peers && !operator_needed &&
           gap <= ZCL_NODE_HEALTH_LAG_WARN_BLOCKS &&
           (log_head_gap < 0 || log_head_gap <= 1);
}

static bool index_projection_ready(bool serving, bool has_peers,
                                   bool operator_needed, int gap,
                                   int index_gap, int log_head_gap)
{
    if (index_gap < 0)
        return false;
    return chain_serving_ready(serving, has_peers, operator_needed, gap,
                               log_head_gap) &&
           index_gap <= ZCL_NODE_HEALTH_LAG_WARN_BLOCKS;
}

static const char *readiness_status(bool serving, bool has_peers,
                                    bool operator_needed, int gap,
                                    int index_gap, int log_head_gap)
{
    if (!chain_serving_ready(serving, has_peers, operator_needed, gap,
                             log_head_gap))
        return "not_serving";
    if (!index_projection_ready(serving, has_peers, operator_needed, gap,
                                index_gap, log_head_gap))
        return "serving_projection_deferred";
    return "ready";
}

static const char *readiness_next_action(bool serving, bool has_peers,
                                         bool operator_needed, int gap,
                                         int index_gap, int log_head_gap)
{
    if (operator_needed)
        return "operator_intervention_required";
    if (!chain_serving_ready(serving, has_peers, operator_needed, gap,
                             log_head_gap))
        return "restore_chain_serving";
    if (!index_projection_ready(serving, has_peers, operator_needed, gap,
                                index_gap, log_head_gap))
        return "continue_chain_ops_inspect_indexer_if_needed";
    return "none";
}

void agent_push_readiness_json(struct json_value *out, const char *key,
                               bool serving, bool has_peers,
                               bool operator_needed,
                               bool validation_pack_ok, int gap,
                               int index_gap, int log_head_gap)
{
    if (!out)
        return;

    const bool chain_ready = chain_serving_ready(serving, has_peers,
                                                 operator_needed, gap,
                                                 log_head_gap);
    const bool index_ready = index_projection_ready(serving, has_peers,
                                                    operator_needed, gap,
                                                    index_gap, log_head_gap);
    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", "zcl.agent_readiness.v1");
    json_push_kv_int(&obj, "schema_version", 1);
    json_push_kv_str(&obj, "status",
                     readiness_status(serving, has_peers, operator_needed,
                                      gap, index_gap, log_head_gap));
    json_push_kv_bool(&obj, "chain_serving_ready", chain_ready);
    json_push_kv_bool(&obj, "index_projection_ready", index_ready);
    json_push_kv_bool(&obj, "agent_work_ready",
                      chain_ready && validation_pack_ok);
    json_push_kv_bool(&obj, "operator_action_required", operator_needed);
    json_push_kv_int(&obj, "tip_gap_blocks", gap);
    json_push_kv_int(&obj, "index_gap_blocks", index_gap);
    json_push_kv_int(&obj, "reducer_log_gap_blocks", log_head_gap);
    json_push_kv_str(&obj, "next_action",
                     readiness_next_action(serving, has_peers,
                                           operator_needed, gap, index_gap,
                                           log_head_gap));
    json_push_kv_str(&obj, "semantics",
                     "chain_serving_ready excludes index projection lag; use index_projection_ready for explorer/projection freshness");
    json_push_kv(out, key && key[0] ? key : "readiness", &obj);
    json_free(&obj);
}
