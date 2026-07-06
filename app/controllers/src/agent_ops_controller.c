/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Compact no-jq operator surface for AI agents. This is intentionally
 * opinionated: it returns the common answers directly instead of forcing the
 * caller to filter larger discovery payloads. */

#include "controllers/agent_controller.h"
#include "controllers/agent_background_quality.h"
#include "controllers/strong_params.h"

#include "json/json.h"
#include "rpc/server.h"
#include "util/clientversion.h"

#include <stdint.h>

static void agent_ops_push_command(struct json_value *arr, const char *name,
                                   const char *native, const char *mcp,
                                   const char *returns)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "name", name);
    json_push_kv_str(&obj, "native", native);
    json_push_kv_str(&obj, "mcp", mcp);
    json_push_kv_str(&obj, "returns", returns);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static void agent_ops_push_work(struct json_value *arr, int64_t rank,
                                const char *name, const char *why,
                                const char *first_slice,
                                const char *proof)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_int(&obj, "rank", rank);
    json_push_kv_str(&obj, "name", name);
    json_push_kv_str(&obj, "why", why);
    json_push_kv_str(&obj, "first_slice", first_slice);
    json_push_kv_str(&obj, "proof", proof);
    json_push_back(arr, &obj);
    json_free(&obj);
}

static void agent_ops_push_quality_summary(struct json_value *out)
{
    struct json_value q;
    json_init(&q);
    agent_build_background_quality_status(&q);
    json_push_kv_str(out, "background_quality_schema",
                     json_get_str(json_get(&q, "schema")));
    json_push_kv_str(out, "background_quality_summary",
                     json_get_str(json_get(&q, "summary")));
    json_push_kv_int(out, "background_quality_lanes_configured",
                     json_get_int(json_get(&q, "lanes_configured")));
    json_push_kv_int(out, "background_quality_status_files_present",
                     json_get_int(json_get(&q, "status_files_present")));
    json_push_kv_int(out, "background_quality_status_files_valid",
                     json_get_int(json_get(&q, "status_files_valid")));
    json_push_kv_str(out, "background_quality_next_action",
                     json_get_str(json_get(&q, "agent_next_action")));
    json_free(&q);
}

bool rpc_agent_ops(const struct json_value *params, bool help,
                   struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "agentops\n"
        "\nReturn the compact no-jq AI/operator command center contract:\n"
        "direct fields, drill-down commands, and the top next architecture\n"
        "work items.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_ops.v1\", \"no_jq_required\":true, "
        "\"top_next_work\":[...] }\n");

    struct json_value commands, api_rules, review, gaps, work;
    const struct agent_contract *ops_contract = agent_contract_lookup("agentops");
    const struct agent_contract *agent_contract = agent_contract_lookup("agent");
    const struct agent_contract *liveness_contract =
        agent_contract_lookup("agentliveness");
    const struct agent_contract *catalog_contract =
        agent_contract_lookup("statecatalog");
    const struct agent_contract *timeline_contract =
        agent_contract_lookup("timeline");
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.agent_ops.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_bool(result, "no_jq_required", true);
    json_push_kv_str(result, "purpose",
                     "one compact agent-ready view of API shape, service architecture, and next work");
    json_push_kv_str(result, "preferred_transport", "mcp");
    json_push_kv_str(result, "native_command",
                     ops_contract ? ops_contract->native_command : "");
    json_push_kv_str(result, "mcp_tool",
                     ops_contract ? ops_contract->mcp_tool : "");
    json_push_kv_str(result, "live_status_command",
                     agent_contract ? agent_contract->native_command : "");
    json_push_kv_str(result, "live_status_tool",
                     agent_contract ? agent_contract->mcp_tool : "");
    json_push_kv_str(result, "liveness_command",
                     liveness_contract ? liveness_contract->native_command : "");
    json_push_kv_str(result, "liveness_tool",
                     liveness_contract ? liveness_contract->mcp_tool : "");
    json_push_kv_str(result, "diagnostics_catalog_command",
                     catalog_contract ? catalog_contract->native_command : "");
    json_push_kv_str(result, "diagnostics_catalog_tool",
                     catalog_contract ? catalog_contract->mcp_tool : "");
    json_push_kv_str(result, "diagnostics_drilldown_command",
                     "zclassic23 dumpstate <subsystem> [key]");
    json_push_kv_str(result, "diagnostics_drilldown_tool", "zcl_state");
    json_push_kv_str(result, "timeline_command",
                     timeline_contract ? timeline_contract->native_command : "");
    json_push_kv_str(result, "timeline_tool",
                     timeline_contract ? timeline_contract->mcp_tool : "");
    json_push_kv_str(result, "refold_plain_english",
                     "Rebuild the UTXO/trust anchor from zclassic23's own verified block history, then cut over so the node no longer depends on a borrowed snapshot seed.");

    agent_push_operator_lane_json(result, "current_runtime_lane");
    agent_push_runtime_build_json(result, "runtime_build");
    agent_push_runtime_availability_json(result, "runtime_availability");
    agent_ops_push_quality_summary(result);

    json_init(&api_rules);
    json_set_array(&api_rules);
    agent_push_contract_command_json(&api_rules, "no_jq_contract",
                                     "agentops",
                                     "compact top-level fields for common agent decisions");
    agent_push_contract_command_json(&api_rules, "live_status", "agent",
                                     "serving state, heights, peers, blockers, lane, readiness");
    agent_push_contract_command_json(&api_rules, "unified_liveness",
                                     "agentliveness",
                                     "lane, listener, supervisor, and background quality liveness");
    agent_ops_push_command(&api_rules, "state_drilldown",
                           "zclassic23 dumpstate <subsystem>", "zcl_state",
                           "one subsystem state object with description");
    agent_push_contract_command_json(&api_rules, "state_catalog",
                                     "statecatalog",
                                     "catalog of zcl_state subsystems, key hints, cost, and owners");
    agent_ops_push_command(&api_rules, "log_search",
                           "zclassic23 getnodelog <pattern>", "zcl_node_log",
                           "bounded server-side log search");
    agent_push_contract_command_json(&api_rules, "semantic_timeline",
                                     "timeline",
                                     "versioned event timeline with bounded filters and seq cursors");
    agent_push_contract_command_json(&api_rules, "test_routing",
                                     "agentimpact",
                                     "changed files mapped to focused tests and risk");
    json_push_kv(result, "direct_commands", &api_rules);
    json_free(&api_rules);

    json_init(&commands);
    json_set_array(&commands);
    agent_push_contract_command_json(&commands, "deploy_guard",
                                     "agentdeployguard",
                                     "allow/refuse before any restart or deploy");
    agent_push_contract_command_json(&commands, "lanes", "agentlanes",
                                     "canonical/soak/dev safety contracts");
    agent_push_contract_command_json(&commands, "liveness",
                                     "agentliveness",
                                     "current lane/service/supervisor/quality rollup");
    agent_push_contract_command_json(&commands, "build_loop", "agentbuild",
                                     "fast-ci, background quality lanes, reproducibility");
    agent_push_contract_command_json(&commands, "mirror",
                                     "getmirrorstatus",
                                     "advisory mirror lag/blocker contract");
    json_push_kv(result, "drilldowns", &commands);
    json_free(&commands);

    json_init(&review);
    json_set_object(&review);
    json_push_kv_str(&review, "architecture_center",
                     "progress.kv fact log plus reducer stages; projections and API are derived views");
    json_push_kv_str(&review, "best_existing_primitive",
                     "diagnostics_registry + zcl_state: one table maps subsystem names to C dumpers");
    json_push_kv_str(&review, "main_dry_problem",
                     "native CLI, live RPC, MCP, REST, and helper scripts still expose overlapping shapes");
    json_push_kv_str(&review, "api_direction",
                     "one C-owned JSON builder per contract; transports proxy it without reshaping");
    json_push_kv_str(&review, "preferred_payload",
                     "versioned JSON with direct decision fields and explicit drill-down commands");
    json_push_kv(result, "architecture_review", &review);
    json_free(&review);

    json_init(&gaps);
    json_set_array(&gaps);
    agent_ops_push_work(&gaps, 1, "runtime_identity_everywhere",
        "Agents must know whether a payload came from source HEAD, dev, soak, or canonical.",
        "add build/lane identity to every first-call compact response",
        "syncdiag_rpc + mcp_controllers + api");
    agent_ops_push_work(&gaps, 2, "state_catalog_schema",
        "The first catalog now exists; next it should be kept rich enough for automated routing.",
        "extend catalog metadata as new dumpers need cost, freshness, key, and owner hints",
        "statecatalog RPC + MCP catalog tests");
    agent_ops_push_work(&gaps, 3, "timeline_query",
        "Agents still stitch together logs, SQL, events, and condition detail to answer what happened.",
        "ship and extend zcl.timeline.v1 before adding more bespoke log readers",
        "event + mcp_controllers + syncdiag_rpc");
    json_push_kv(result, "api_gaps", &gaps);
    json_free(&gaps);

    json_init(&work);
    json_set_array(&work);
    agent_ops_push_work(&work, 1, "finish_self_verified_utxo_anchor_rebuild",
        "It replaces the borrowed snapshot seed with a UTXO anchor rebuilt from zclassic23's own verified block history.",
        "copy-prove -refold-from-anchor artifact and cutover gates",
        "copy fixture, refold tests, parity checks, live H* climb");
    agent_ops_push_work(&work, 2, "dry_agent_contract_registry",
        "The first-call contracts now exist, but their method/schema/tool names still appear in several tables.",
        "move repeated agent command metadata into one C-owned registry",
        "syncdiag_rpc + mcp_controllers + make_lint_gates");
    agent_ops_push_work(&work, 3, "promote_diagnostics_catalog",
        "Every subsystem should be semantically discoverable without source search.",
        "keep zcl.state_catalog.v1 complete as dumpers grow and route agents through it first",
        "statecatalog RPC + MCP route + docs tests");
    agent_ops_push_work(&work, 4, "extend_semantic_timeline_durability",
        "The event-ring timeline is semantic now; longer root-cause windows need durable event_log/node.log references.",
        "extend zcl.timeline.v1 toward durable event_log/node.log references",
        "event + mcp_controllers + syncdiag_rpc");
    agent_ops_push_work(&work, 5, "harden_agent_liveness_slos",
        "zcl.agent_liveness.v1 now composes lanes, supervisor, and quality; the next step is explicit SLO thresholds.",
        "add production SLO thresholds and alert semantics to the liveness summary",
        "supervisor production tree + lane health + background quality tests");
    json_push_kv(result, "top_next_work", &work);
    json_free(&work);
    return true;
}
