/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unified AI/operator liveness contract. This composes existing C-owned
 * contracts instead of re-implementing lane, quality, or supervisor logic. */

#include "controllers/agent_background_quality.h"
#include "controllers/agent_controller.h"
#include "controllers/strong_params.h"

#include "json/json.h"
#include "rpc/server.h"
#include "util/clientversion.h"
#include "util/supervisor.h"

#include <stdint.h>
#include <string.h>

struct agent_liveness_supervisor_counts {
    int64_t child_count;
    int64_t stale_child_count;
    int64_t stall_fires_total;
    int64_t restart_count_total;
};

static void agent_liveness_push_str(struct json_value *arr, const char *s)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, s ? s : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static void agent_liveness_count_children(
    const struct json_value *children,
    struct agent_liveness_supervisor_counts *counts)
{
    if (!children || children->type != JSON_ARR || !counts)
        return;
    for (size_t i = 0; i < json_size(children); i++) {
        const struct json_value *child = json_at(children, i);
        int64_t deadline_secs =
            json_get_int(json_get(child, "deadline_secs"));
        int64_t age_us =
            json_get_int(json_get(child, "last_tick_age_us"));
        int64_t stall_fires =
            json_get_int(json_get(child, "stall_fires"));
        int64_t restarts =
            json_get_int(json_get(child, "restart_count"));

        counts->child_count++;
        counts->stall_fires_total += stall_fires;
        counts->restart_count_total += restarts;
        if (deadline_secs > 0 && age_us > deadline_secs * 1000000LL)
            counts->stale_child_count++;
    }
}

static struct agent_liveness_supervisor_counts
agent_liveness_supervisor_counts(const struct json_value *supervisor)
{
    struct agent_liveness_supervisor_counts counts = {0};
    const struct json_value *domains = json_get(supervisor, "domains");
    const struct json_value *orphans = json_get(supervisor, "root_orphans");

    if (domains && domains->type == JSON_ARR) {
        for (size_t i = 0; i < json_size(domains); i++) {
            const struct json_value *domain = json_at(domains, i);
            agent_liveness_count_children(json_get(domain, "children"),
                                          &counts);
        }
    }
    agent_liveness_count_children(orphans, &counts);
    return counts;
}

static void agent_liveness_push_drilldowns(struct json_value *out,
                                           bool attention_needed)
{
    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    agent_liveness_push_str(&arr, "zcl_agent");
    agent_liveness_push_str(&arr, "zcl_agent_lanes");
    agent_liveness_push_str(&arr, "zcl_state {\"subsystem\":\"supervisor\"}");
    agent_liveness_push_str(&arr, "zcl_agent_build");
    agent_liveness_push_str(&arr, "make quality-linger-status");
    if (attention_needed) {
        agent_liveness_push_str(&arr,
            "zcl_node_log pattern=\"stall|stale|fail|restart|quality\" max_lines=120");
    }
    json_push_kv(out, "recommended_drilldowns", &arr);
    json_free(&arr);
}

static void agent_liveness_push_summary(struct json_value *out,
                                        const struct json_value *lane,
                                        const struct json_value *runtime,
                                        const struct json_value *quality,
                                        const struct json_value *supervisor)
{
    struct agent_liveness_supervisor_counts sup =
        agent_liveness_supervisor_counts(supervisor);
    bool supervisor_running = json_get_bool(json_get(supervisor, "running"));
    bool supervisor_thread_alive =
        json_get_bool(json_get(supervisor, "thread_alive"));
    bool rpc_running = json_get_bool(json_get(runtime, "rpc_running"));
    bool https_running = json_get_bool(json_get(runtime, "https_running"));
    bool fs_running = json_get_bool(json_get(runtime, "fs_running"));
    int64_t quality_failed =
        json_get_int(json_get(quality, "failed_count"));
    int64_t quality_running =
        json_get_int(json_get(quality, "running_count"));
    int64_t quality_valid =
        json_get_int(json_get(quality, "status_files_valid"));
    int64_t quality_configured =
        json_get_int(json_get(quality, "lanes_configured"));
    const char *quality_summary =
        json_get_str(json_get(quality, "summary"));
    const char *lane_name =
        json_get_str(json_get(lane, "lane"));
    bool runtime_active = supervisor_running || supervisor_thread_alive ||
                          rpc_running || https_running || fs_running;
    bool attention_needed = quality_failed > 0 ||
                            sup.stale_child_count > 0 ||
                            sup.stall_fires_total > 0 ||
                            (supervisor_running && !supervisor_thread_alive);
    const char *overall = attention_needed ? "attention_needed" :
        (runtime_active ? "active" : "static_or_offline_context");
    const char *next_action = attention_needed
        ? "inspect_agent_liveness_drilldowns"
        : (runtime_active ? "monitor_liveness_and_quality_lanes"
                          : "probe_target_runtime_or_start_dev_lane");

    struct json_value summary;
    json_init(&summary);
    json_set_object(&summary);
    json_push_kv_str(&summary, "lane", lane_name);
    json_push_kv_bool(&summary, "runtime_active", runtime_active);
    json_push_kv_bool(&summary, "attention_needed", attention_needed);
    json_push_kv_bool(&summary, "rpc_running", rpc_running);
    json_push_kv_bool(&summary, "https_running", https_running);
    json_push_kv_bool(&summary, "fs_running", fs_running);
    json_push_kv_bool(&summary, "supervisor_running", supervisor_running);
    json_push_kv_bool(&summary, "supervisor_thread_alive",
                      supervisor_thread_alive);
    json_push_kv_int(&summary, "supervisor_child_count", sup.child_count);
    json_push_kv_int(&summary, "supervisor_stale_child_count",
                     sup.stale_child_count);
    json_push_kv_int(&summary, "supervisor_stall_fires_total",
                     sup.stall_fires_total);
    json_push_kv_int(&summary, "supervisor_restart_count_total",
                     sup.restart_count_total);
    json_push_kv_str(&summary, "background_quality_summary",
                     quality_summary);
    json_push_kv_int(&summary, "background_quality_lanes_configured",
                     quality_configured);
    json_push_kv_int(&summary, "background_quality_status_files_valid",
                     quality_valid);
    json_push_kv_int(&summary, "background_quality_running_count",
                     quality_running);
    json_push_kv_int(&summary, "background_quality_failed_count",
                     quality_failed);
    json_push_kv_str(&summary, "overall_liveness", overall);
    json_push_kv_str(&summary, "agent_next_action", next_action);
    json_push_kv(out, "liveness_summary", &summary);
    json_free(&summary);

    json_push_kv_str(out, "overall_liveness", overall);
    json_push_kv_str(out, "agent_next_action", next_action);
    agent_liveness_push_drilldowns(out, attention_needed);
}

bool rpc_agent_liveness(const struct json_value *params, bool help,
                        struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agentliveness\n"
        "\nReturn one AI/operator liveness contract that composes lane identity,\n"
        "runtime listeners, supervisor children, and background quality lanes.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_liveness.v1\", "
        "\"liveness_summary\":{...} }\n");

    struct json_value supervisor_state, quality_status;
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_liveness.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_str(result, "native_command", "zclassic23 agentliveness");
    json_push_kv_str(result, "mcp_tool", "zcl_agent_liveness");
    json_push_kv_str(result, "semantics",
                     "read-only composition of current_runtime_lane, runtime_services, supervisor_state, and background_quality_status");

    agent_push_operator_lane_json(result, "current_runtime_lane");
    agent_push_runtime_services_json(result, "runtime_services");

    json_init(&supervisor_state);
    if (!supervisor_dump_state_json(&supervisor_state, NULL)) {
        json_free(&supervisor_state);
        json_set_object(&supervisor_state);
        json_push_kv_str(&supervisor_state, "status", "unavailable");
    }
    json_push_kv(result, "supervisor_state", &supervisor_state);

    json_init(&quality_status);
    agent_build_background_quality_status(&quality_status);
    json_push_kv(result, "background_quality_status", &quality_status);

    agent_liveness_push_summary(
        result,
        json_get(result, "current_runtime_lane"),
        json_get(result, "runtime_services"),
        &quality_status,
        &supervisor_state);

    json_free(&quality_status);
    json_free(&supervisor_state);
    return true;
}
