/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP ops controller: status/health/aggregate dashboards.
 *   Core:     zcl_status, zcl_health, zcl_kpi, zcl_events, zcl_rpc,
 *             zcl_mirror_status, zcl_filemanifest, zcl_syncdiag,
 *             zcl_self_heal_stats
 *   Mempool/mining: zcl_getmempoolinfo, zcl_getrawmempool, zcl_getmininginfo
 *   Performance: zcl_benchmark, zcl_dbstats
 *
 * Low-level diagnostic primitives (zcl_sql, zcl_state, zcl_node_log,
 * zcl_profile, zcl_probe_zclassicd, zcl_replay_*)
 * live in diagnostics_controller.c. */

#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../rpc_params.h"

#include "controllers/agent_controller.h"
#include "controllers/status_native_handlers.h"
#include "controllers/status_native_helpers.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "rpc/protocol.h"
#include "sim/postmortem.h"
#include "sim/seed_tape.h"
#include "util/clientversion.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/process_block.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_PT(h_zcl_getmempoolinfo, "getmempoolinfo", "mcp.ops")
DEFINE_PT(h_zcl_mempool_inspect, "getmempoolfeestats", "mcp.ops")
DEFINE_PT(h_zcl_getrawmempool,  "getrawmempool",  "mcp.ops")
DEFINE_PT(h_zcl_getmininginfo,  "getmininginfo",  "mcp.ops")
DEFINE_PT(h_zcl_benchmark,      "benchmark",      "mcp.ops")
DEFINE_PT(h_zcl_dbstats,        "db_info",        "mcp.ops")
DEFINE_PT(h_zcl_milestone,      "milestone",      "mcp.ops")
DEFINE_PT(h_zcl_refold_status,  "refold",         "mcp.ops")
DEFINE_PT(h_zcl_agent,          "agent",          "mcp.ops")
DEFINE_PT(h_zcl_agent_map,      "agentmap",       "mcp.ops")
DEFINE_PT(h_zcl_agent_lanes,    "agentlanes",     "mcp.ops")
DEFINE_PT(h_zcl_agent_contracts,"agentcontracts", "mcp.ops")
DEFINE_PT(h_zcl_agent_interface,"agentinterface", "mcp.ops")
DEFINE_PT(h_zcl_agent_ops,      "agentops",       "mcp.ops")
DEFINE_PT(h_zcl_app_protocols,  "appprotocols",   "mcp.ops")

static int h_zcl_agent_build(const struct mcp_request *req,
                             struct mcp_response *res)
{
    (void)req;

    struct json_value params;
    struct json_value result;
    json_init(&params);
    json_set_array(&params);
    json_init(&result);

    if (!rpc_agent_build(&params, false, &result)) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "agentbuild contract generation failed");
        LOG_ERR("mcp.ops", "agentbuild contract generation failed");
        json_free(&result);
        json_free(&params);
        return 0;
    }

    res->body = zcl_json_value_to_body(&result, "mcp.agentbuild.body");
    if (!res->body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "agentbuild response allocation failed");
        LOG_ERR("mcp.ops", "agentbuild response allocation failed");
    }

    json_free(&result);
    json_free(&params);
    return 0;
}

static int h_zcl_agent_dev_status(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    (void)req;

    struct json_value params;
    struct json_value result;
    json_init(&params);
    json_set_array(&params);
    json_init(&result);

    if (!rpc_agent_dev_status(&params, false, &result)) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "agentdevstatus contract generation failed");
        LOG_ERR("mcp.ops", "agentdevstatus contract generation failed");
        json_free(&result);
        json_free(&params);
        return 0;
    }

    res->body = zcl_json_value_to_body(&result, "mcp.agentdevstatus.body");
    if (!res->body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "agentdevstatus response allocation failed");
        LOG_ERR("mcp.ops", "agentdevstatus response allocation failed");
    }

    json_free(&result);
    json_free(&params);
    return 0;
}

/* Thin MCP wrapper for the re-homed transport-neutral body functions
 * (app/controllers/src/status_native_handlers.c). The body function owns
 * the composition and every LOG_* line; this maps its typed failure back
 * onto the handler's historical MCP error code + message so the MCP surface
 * stays byte-identical. */
static int ops_native_wrap(struct mcp_response *res, char *body,
                           const struct zcl_native_body_err *e)
{
    if (!body) {
        res->error = (e->status == ZCL_NATIVE_BODY_INTERNAL)
                         ? MCP_ERR_INTERNAL : MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message), "%s",
                 e->message);
    }
    res->body = body;
    return 0;
}


/* ── Handlers ───────────────────────────────────────────────── */

static int h_zcl_status(const struct mcp_request *req, struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_status_body(req->args, &e);
    return ops_native_wrap(res, body, &e);
}

static int h_zcl_operator_summary_compat(const struct mcp_request *req,
                                         struct mcp_response *res)
{
    (void)req;

    char *chain  = mcp_node_rpc("getblockchaininfo", NULL);
    char *peers  = mcp_node_rpc("getpeerinfo", NULL);
    char *diag   = mcp_node_rpc("getsyncdiag", NULL);
    char *dl     = mcp_node_rpc("downloadstats", NULL);
    char *mirror = mcp_node_rpc("getmirrorstatus", NULL);
    char *health = mcp_node_rpc("healthcheck", NULL);
    char *agent  = mcp_node_rpc("agent", NULL);
    char *bl     = mcp_node_rpc("dumpstate", "[\"blocker\"]");

    struct json_value chain_j, peers_j, diag_j, dl_j, mirror_j, health_j,
                      agent_j;
    bool chain_ok = status_parse_rpc_json(&chain_j, chain, JSON_OBJ);
    bool peers_ok = status_parse_rpc_json(&peers_j, peers, JSON_ARR) &&
                    status_peer_array_is_valid(&peers_j);
    bool diag_ok = status_parse_rpc_json(&diag_j, diag, JSON_OBJ);
    bool dl_ok = status_parse_rpc_json(&dl_j, dl, JSON_OBJ);
    bool mirror_ok = status_parse_rpc_json(&mirror_j, mirror, JSON_OBJ);
    bool health_ok = status_parse_rpc_json(&health_j, health, JSON_OBJ);
    bool agent_ok = status_parse_rpc_json(&agent_j, agent, JSON_OBJ);
    struct json_value blocker_summary_j, blocker_dominant_j, blocker_error_j;
    json_init(&blocker_summary_j);
    json_init(&blocker_dominant_j);
    json_init(&blocker_error_j);
    bool blockers_ok = status_build_blocker_summary(
        bl, false, &blocker_summary_j, &blocker_dominant_j,
        &blocker_error_j);
    long long active_blockers = blockers_ok
        ? status_json_int(&blocker_summary_j, "active_count", 0) : 0;
    long long permanent_blockers = blockers_ok
        ? status_json_int(&blocker_summary_j, "permanent_count", 0) : 0;
    long long resource_blockers = blockers_ok
        ? status_json_int(&blocker_summary_j, "resource_count", 0) : 0;
    const char *target_dominant_id = blockers_ok
        ? status_json_str(&blocker_dominant_j, "id", "") : "";

    int peer_total = 0, peer_inbound = 0, peer_outbound = 0, peer_ready = 0;
    long long peer_max_height = 0;
    bool peer_max_height_known = false;
    bool peer_direction_known = false;
    bool peer_ready_known = false;
    if (peers_ok) {
        struct peer_survey ps;
        status_peer_survey(&peers_j, &ps);
        peer_total = ps.total;
        peer_inbound = ps.inbound;
        peer_outbound = ps.outbound;
        peer_ready = ps.ready;
        peer_max_height = ps.max_height;
        peer_max_height_known = ps.max_height_known;
        peer_direction_known = ps.direction_known;
        peer_ready_known = ps.ready_known;
    }

    const struct json_value *health_chain =
        health_ok ? json_get(&health_j, "chain_advance") : NULL;
    const struct json_value *health_evidence =
        health_ok ? json_get(&health_j, "chain_evidence") : NULL;
    const struct json_value *checks =
        health_ok ? json_get(&health_j, "checks") : NULL;
    const struct json_value *condition_engine =
        checks ? json_get(checks, "condition_engine") : NULL;
    if ((!condition_engine || condition_engine->type != JSON_OBJ) && health_ok)
        condition_engine = json_get(&health_j, "condition_engine");
    const struct json_value *watchdog =
        diag_ok ? json_get(&diag_j, "watchdog") : NULL;

    int64_t chain_rpc_height = 0;
    int64_t diag_chain_height = 0;
    int64_t health_local_height = 0;
    int64_t evidence_tip = 0;
    int64_t chain_header_height = 0;
    int64_t diag_header_height = 0;
    bool chain_height_known =
        chain_ok && status_read_height(&chain_j, "blocks",
                                       &chain_rpc_height);
    bool diag_chain_known =
        diag_ok && status_read_height(&diag_j, "chain_height",
                                      &diag_chain_height);
    bool health_local_known =
        health_chain && status_read_height(health_chain, "local_height",
                                           &health_local_height);
    bool evidence_tip_known =
        health_evidence && status_read_height(health_evidence, "active_tip",
                                              &evidence_tip);
    bool chain_header_known =
        chain_ok && status_read_height(&chain_j, "best_header_height",
                                       &chain_header_height);
    bool diag_header_known =
        diag_ok && status_read_height(&diag_j, "best_header_height",
                                      &diag_header_height);

    bool indexed_height_known = diag_chain_known || health_local_known ||
                                evidence_tip_known;
    int64_t indexed_height = 0;
    if (diag_chain_known)
        indexed_height = diag_chain_height;
    if (health_local_known)
        indexed_height = status_max_ll(indexed_height, health_local_height);
    if (evidence_tip_known)
        indexed_height = status_max_ll(indexed_height, evidence_tip);
    if (!indexed_height_known && chain_height_known) {
        indexed_height = chain_rpc_height;
        indexed_height_known = true;
    }

    /* Served H* must come from an H*-owned contract. Active/indexed tips in
     * cached health evidence are useful corroboration, but cannot be relabeled
     * as the node's published served frontier. */
    bool height_known = chain_height_known || diag_chain_known;
    int64_t height = chain_height_known ? chain_rpc_height
                                        : diag_chain_height;

    bool header_height_known = chain_header_known || diag_header_known;
    int64_t header_height = 0;
    if (chain_header_known)
        header_height = chain_header_height;
    if (diag_header_known)
        header_height = status_max_ll(header_height, diag_header_height);

    /* Peer and legacy-mirror heights are advisory availability signals. The
     * locally validated header frontier alone defines the authoritative sync
     * target used by this summary. */
    long long target_height = header_height;
    bool frontier_evidence_known = height_known && indexed_height_known &&
                                   header_height_known;
    bool frontier_evidence_consistent = frontier_evidence_known &&
        height <= indexed_height && indexed_height <= header_height;
    bool index_gap_known = frontier_evidence_consistent;
    long long index_gap = target_height > indexed_height
        ? target_height - indexed_height : 0;
    bool gap_known = frontier_evidence_consistent;
    bool served_gap_known = gap_known;
    long long gap = target_height > height ? target_height - height : 0;
    long long served_gap = gap;

    const char *sync_state =
        diag_ok ? status_json_str(&diag_j, "sync_state", "") : "";
    if (!sync_state[0] && health_ok)
        sync_state = status_json_str(&health_j, "sync_state", "");
    if (!sync_state[0] && dl_ok)
        sync_state = status_json_str(&dl_j, "sync_state", "");
    if (!sync_state[0])
        sync_state = "unknown";

    bool healthy = false;
    bool serving = false;
    bool health_operator_needed = false;
    bool healthy_known =
        health_ok && status_read_bool(&health_j, "healthy", &healthy);
    bool serving_known =
        health_ok && status_read_bool(&health_j, "serving", &serving);
    bool health_operator_needed_known =
        checks && status_read_bool(checks, "operator_needed",
                                   &health_operator_needed);
    bool blocker_operator_needed =
        permanent_blockers > 0 || resource_blockers > 0;
    bool operator_needed = health_operator_needed ||
                           blocker_operator_needed;
    bool operator_needed_known = operator_needed ||
        (blockers_ok && health_operator_needed_known);
    const char *health_blocking_reason =
        checks ? status_json_str(checks, "blocking_reason", "") : "";
    const char *operator_needed_detail =
        checks ? status_json_str(checks, "operator_needed_detail", "") : "";
    long long active_conditions =
        condition_engine ? status_json_int(condition_engine, "active_count", 0)
                         : 0;
    if (active_conditions == 0 && watchdog)
        active_conditions = status_json_int(watchdog, "active_conditions", 0);
    long long unresolved_conditions =
        condition_engine ? status_json_int(condition_engine,
                                           "unresolved_count", 0)
                         : 0;

    int64_t in_flight = 0;
    int64_t queued = 0;
    bool in_flight_known =
        dl_ok && status_read_nonnegative_int(&dl_j, "in_flight",
                                             &in_flight);
    bool queued_known =
        dl_ok && status_read_nonnegative_int(&dl_j, "queued", &queued);
    bool download_counts_known = in_flight_known && queued_known;
    bool download_work_active = in_flight > 0 || queued > 0;

    const char *mirror_blocker = "";
    const char *mirror_detail = "";
    bool mirror_enabled = false, mirror_running = false, mirror_reachable = false;
    bool mirror_contract_trusted = false;
    bool mirror_blocker_active = false;
    bool mirror_operator_action_required = false;
    if (mirror_ok) {
        mirror_enabled = status_json_bool(&mirror_j, "mirror_enabled", false);
        mirror_running = status_json_bool(&mirror_j, "mirror_running", false);
        mirror_reachable = status_json_bool(&mirror_j, "reachable", false) ||
                           status_json_bool(&mirror_j, "mirror_reachable",
                                            false);
        const struct json_value *contract =
            json_get(&mirror_j, "mirror_contract");
        if (contract && contract->type == JSON_OBJ) {
            mirror_contract_trusted = true;
            mirror_blocker_active =
                status_json_bool(contract, "blocker_active", false);
            mirror_operator_action_required =
                status_json_bool(contract, "operator_action_required",
                                 mirror_blocker_active);
            if (mirror_blocker_active) {
                mirror_blocker =
                    status_json_str(contract, "blocker_code", "");
                if (!mirror_blocker[0])
                    mirror_blocker =
                        status_json_str(&mirror_j, "active_error_code", "");
                if (!mirror_blocker[0])
                    mirror_blocker =
                        status_json_str(&mirror_j, "activation_blocker", "");
                mirror_detail =
                    status_json_str(&mirror_j, "active_error_detail", "");
                if (!mirror_detail[0])
                    mirror_detail =
                        status_json_str(&mirror_j, "last_error", "");
            }
        } else {
            mirror_blocker =
                status_json_str(&mirror_j, "active_error_code", "");
            if (!mirror_blocker[0])
                mirror_blocker =
                    status_json_str(&mirror_j, "activation_blocker", "");
            mirror_detail =
                status_json_str(&mirror_j, "active_error_detail", "");
            if (!mirror_detail[0])
                mirror_detail =
                    status_json_str(&mirror_j, "last_error", "");
            mirror_blocker_active = mirror_blocker[0] != '\0';
            mirror_operator_action_required = mirror_blocker_active;
        }
    } else if (health_ok) {
        mirror_enabled = status_json_bool(&health_j,
                                          "mirror_monitor_running", false);
        mirror_running = mirror_enabled;
        mirror_reachable = status_json_bool(&health_j,
                                            "mirror_reachable", false);
        mirror_blocker = status_json_str(&health_j,
                                         "mirror_active_error_code", "");
        mirror_detail = status_json_str(&health_j,
                                        "mirror_active_error_detail", "");
        mirror_blocker_active = mirror_blocker[0] != '\0';
        mirror_operator_action_required = mirror_blocker_active;
    }

    const char *status = "unknown";
    const char *primary_blocker = "unknown";
    const char *next_action = "zcl_status";
    const char *next_tool = "zcl_status";
    const char *next_tool2 = "";
    char primary_blocker_buf[192] = {0};
    if (blocker_operator_needed) {
        status = "operator_needed";
        if (target_dominant_id[0]) {
            snprintf(primary_blocker_buf, sizeof(primary_blocker_buf),
                     "typed_blocker:%s", target_dominant_id);
            primary_blocker = primary_blocker_buf;
        } else {
            primary_blocker = "typed_blocker_operator_needed";
        }
        next_action = "inspect authoritative target-node blockers";
        next_tool = "zcl_blockers";
        next_tool2 = "zcl_state";
    } else if (health_operator_needed) {
        status = "operator_needed";
        primary_blocker = "operator_needed";
        if (operator_needed_detail[0]) {
            snprintf(primary_blocker_buf, sizeof(primary_blocker_buf),
                     "operator_needed:%s", operator_needed_detail);
            primary_blocker = primary_blocker_buf;
        } else if (health_blocking_reason[0]) {
            primary_blocker = health_blocking_reason;
        }
        next_action = "inspect active conditions and operator-needed detail";
        next_tool = "zcl_conditions";
        next_tool2 = "zcl_node_log";
    } else if (serving_known && !serving) {
        status = "blocked";
        primary_blocker = health_blocking_reason[0]
            ? health_blocking_reason : "not_serving";
        next_action = "inspect health and typed blockers";
        next_tool = "zcl_health";
        next_tool2 = "zcl_blockers";
    } else if (active_blockers > 0) {
        status = "degraded";
        if (target_dominant_id[0]) {
            snprintf(primary_blocker_buf, sizeof(primary_blocker_buf),
                     "typed_blocker:%s", target_dominant_id);
            primary_blocker = primary_blocker_buf;
        } else {
            primary_blocker = "typed_blocker_active";
        }
        next_action = "inspect authoritative target-node blockers";
        next_tool = "zcl_blockers";
    } else if (active_conditions > 0 || unresolved_conditions > 0) {
        status = "degraded";
        primary_blocker = "condition_active";
        next_action = "inspect active self-heal conditions";
        next_tool = "zcl_conditions";
    } else if (peers_ok && peer_total == 0) {
        status = "blocked";
        primary_blocker = "no_peers";
        next_action = "connect or inspect peers";
        next_tool = "zcl_peers";
    } else if (healthy_known && !healthy) {
        status = "degraded";
        primary_blocker = "healthcheck_unhealthy";
        next_action = "inspect health checks";
        next_tool = "zcl_health";
    } else if (gap_known && gap > 0 && peers_ok &&
               !download_counts_known) {
        status = "degraded";
        primary_blocker = "download_state_unavailable";
        next_action = "restore target download telemetry";
        next_tool = "zcl_syncdiag";
        next_tool2 = "zcl_node_log";
    } else if (gap_known && gap > 0 && peers_ok &&
               download_counts_known) {
        status = download_work_active ? "catching_up" : "degraded";
        primary_blocker = download_work_active
            ? "chain_gap"
            : "download_queue_idle";
        next_action = download_work_active
            ? "wait for gap-fill and recheck"
            : "inspect sync diagnostics and recent download/gap-fill logs";
        next_tool = "zcl_syncdiag";
        next_tool2 = "zcl_node_log";
    } else if (!chain_ok && !health_ok && !blockers_ok) {
        status = "rpc_unavailable";
        primary_blocker = "rpc_unavailable";
        next_action = "check MCP RPC cookie and node RPC reachability";
        next_tool = "zcl_health";
    } else if (frontier_evidence_known && !frontier_evidence_consistent) {
        status = "degraded";
        primary_blocker = "chain_evidence_inconsistent";
        next_action = "inspect contradictory served/index/header frontiers";
        next_tool = "zcl_status";
        next_tool2 = "zcl_syncdiag";
    } else if (!height_known || !header_height_known || !gap_known ||
               !served_gap_known || !index_gap_known) {
        status = "degraded";
        primary_blocker = "chain_evidence_unavailable";
        next_action = "restore served-height and validated-header evidence";
        next_tool = "zcl_status";
        next_tool2 = "zcl_syncdiag";
    } else if (!health_ok || !healthy_known || !serving_known ||
               !health_operator_needed_known) {
        status = "degraded";
        primary_blocker = "health_state_unavailable";
        next_action = "restore target health telemetry";
        next_tool = "zcl_health";
        next_tool2 = "zcl_node_log";
    } else if (!peers_ok) {
        status = "degraded";
        primary_blocker = "peer_state_unavailable";
        next_action = "restore target peer telemetry";
        next_tool = "zcl_peers";
        next_tool2 = "zcl_node_log";
    } else if (strcmp(sync_state, "unknown") == 0) {
        status = "degraded";
        primary_blocker = "sync_state_unavailable";
        next_action = "restore target sync telemetry";
        next_tool = "zcl_syncdiag";
        next_tool2 = "zcl_node_log";
    } else if (strcmp(sync_state, "at_tip") != 0) {
        status = "degraded";
        primary_blocker = "sync_not_at_tip";
        next_action = "inspect target sync state and progress";
        next_tool = "zcl_syncdiag";
        next_tool2 = "zcl_node_log";
    } else if (!blockers_ok) {
        status = "degraded";
        primary_blocker = "blocker_state_unavailable";
        next_action = "restore target blocker telemetry before declaring healthy";
        next_tool = "zcl_state";
        next_tool2 = "zcl_health";
    } else {
        /* Every legacy child is individually valid, but the target cannot
         * prove that the eight responses came from one process generation or
         * one frontier.  Never manufacture a green verdict from an
         * intrinsically non-coherent compatibility capture. */
        status = "degraded";
        primary_blocker = "compatibility_snapshot_non_atomic";
        next_action = "upgrade target for native operatorsnapshot support";
        next_tool = "zcl_operator_snapshot";
    }

    char height_text[32], indexed_text[32], target_text[32];
    char gap_text[32], served_gap_text[32], index_gap_text[32];
    char peer_text[32], ready_peer_text[32];
    status_format_int_if_known(height_text, sizeof(height_text),
                               height_known, height);
    status_format_int_if_known(indexed_text, sizeof(indexed_text),
                               indexed_height_known, indexed_height);
    status_format_int_if_known(target_text, sizeof(target_text),
                               header_height_known, target_height);
    status_format_int_if_known(gap_text, sizeof(gap_text), gap_known, gap);
    status_format_int_if_known(served_gap_text, sizeof(served_gap_text),
                               served_gap_known, served_gap);
    status_format_int_if_known(index_gap_text, sizeof(index_gap_text),
                               index_gap_known, index_gap);
    status_format_int_if_known(peer_text, sizeof(peer_text), peers_ok,
                               peer_total);
    status_format_int_if_known(ready_peer_text, sizeof(ready_peer_text),
                               peer_ready_known, peer_ready);
    char summary[512];
    snprintf(summary, sizeof(summary),
             "%s: height=%s indexed=%s target=%s gap=%s "
             "served_gap=%s index_gap=%s sync=%s peers=%s "
             "ready_peers=%s primary=%s",
             status, height_text, indexed_text, target_text, gap_text,
             served_gap_text, index_gap_text, sync_state, peer_text,
             ready_peer_text, primary_blocker);

    struct json_value root, peer_obj, mirror_obj, download_obj, raw;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_str(&root, "schema", "zcl.operator_summary.v1");
    json_push_kv_int(&root, "schema_version", 1);
    json_push_kv_str(&root, "api_version", "v1");
    json_push_kv_str(&root, "execution_locus", "composite");
    json_push_kv_str(&root, "source_rpc", "legacy_multi_rpc_compat");
    json_push_kv_bool(&root, "atomic", false);
    json_push_kv_bool(&root, "compatibility_fallback", true);
    json_push_kv_bool(&root, "verdict_complete", false);
    json_push_kv_str(&root, "capture_model", "multi_rpc_compat");
    json_push_kv_str(&root, "status", status);
    json_push_kv_bool(&root, "healthy", strcmp(status, "healthy") == 0);
    status_push_bool_if_known(&root, "serving", serving_known, serving);
    json_push_kv_str(&root, "summary", summary);
    status_push_int_if_known(&root, "height", height_known, height);
    status_push_int_if_known(&root, "served_height", height_known, height);
    status_push_int_if_known(&root, "indexed_height", indexed_height_known,
                             indexed_height);
    status_push_int_if_known(&root, "chain_rpc_height", chain_height_known,
                             chain_rpc_height);
    status_push_int_if_known(&root, "header_height", header_height_known,
                             header_height);
    status_push_int_if_known(&root, "target_height", header_height_known,
                             target_height);
    json_push_kv_str(&root, "target_height_source",
                     header_height_known
                         ? "target_node.validated_header_tip"
                         : "unavailable");
    status_push_int_if_known(&root, "gap", gap_known, gap);
    status_push_int_if_known(&root, "served_gap", served_gap_known,
                             served_gap);
    status_push_int_if_known(&root, "index_gap", index_gap_known,
                             index_gap);
    status_push_bool_if_known(&root, "chain_evidence_consistent",
                              frontier_evidence_known,
                              frontier_evidence_consistent);
    json_push_kv_str(&root, "sync_state", sync_state);
    json_push_kv_str(&root, "primary_blocker", primary_blocker);
    json_push_kv_str(&root, "blocking_reason", health_blocking_reason);
    if (operator_needed_detail[0])
        json_push_kv_str(&root, "operator_needed_detail",
                         operator_needed_detail);
    json_push_kv_str(&root, "next_action", next_action);
    json_push_kv_str(&root, "next_tool", next_tool);
    status_push_string_array(&root, "recommended_tools", next_tool,
                             next_tool2);
    status_push_bool_if_known(&root, "operator_needed",
                              operator_needed_known, operator_needed);
    json_push_kv_int(&root, "active_conditions", active_conditions);
    json_push_kv_int(&root, "unresolved_conditions", unresolved_conditions);
    const struct json_value *operator_lane =
        agent_ok ? json_get(&agent_j, "operator_lane") : NULL;
    if (operator_lane && operator_lane->type == JSON_OBJ) {
        status_push_lane_safety_fields(&root, operator_lane);
        json_push_kv(&root, "operator_lane", operator_lane);
    }

    json_init(&peer_obj);
    json_set_object(&peer_obj);
    json_push_kv_bool(&peer_obj, "known", peers_ok);
    json_push_kv_bool(&peer_obj, "direction_known",
                      peer_direction_known);
    json_push_kv_bool(&peer_obj, "ready_known", peer_ready_known);
    status_push_int_if_known(&peer_obj, "total", peers_ok, peer_total);
    status_push_int_if_known(&peer_obj, "inbound", peer_direction_known,
                             peer_inbound);
    status_push_int_if_known(&peer_obj, "outbound", peer_direction_known,
                             peer_outbound);
    status_push_int_if_known(&peer_obj, "ready", peer_ready_known,
                             peer_ready);
    status_push_int_if_known(&peer_obj, "max_height",
                             peer_max_height_known,
                             peer_max_height);
    json_push_kv_bool(&peer_obj, "max_height_known",
                      peer_max_height_known);
    if (peers_ok && !peer_direction_known)
        status_push_json_error(&peer_obj, "direction",
                               "peer entry missing boolean inbound field",
                               NULL);
    if (peers_ok && !peer_ready_known)
        status_push_json_error(&peer_obj, "ready",
                               "peer entry missing string state field",
                               NULL);
    if (peers_ok && !peer_max_height_known)
        status_push_json_error(&peer_obj, "max_height",
                               "no connected peer supplied a valid height claim",
                               NULL);
    json_push_kv_str(&peer_obj, "max_height_trust",
                     "untrusted_peer_advertisement");
    json_push_kv(&root, "peers", &peer_obj);
    json_free(&peer_obj);

    json_init(&download_obj);
    json_set_object(&download_obj);
    json_push_kv_bool(&download_obj, "known", download_counts_known);
    status_push_int_if_known(&download_obj, "in_flight", in_flight_known,
                             in_flight);
    status_push_int_if_known(&download_obj, "queued", queued_known, queued);
    if (dl_ok)
        json_push_kv_str(&download_obj, "sync_state",
                         status_json_str(&dl_j, "sync_state", ""));
    json_push_kv(&root, "download", &download_obj);
    json_free(&download_obj);

    json_init(&mirror_obj);
    json_set_object(&mirror_obj);
    json_push_kv_bool(&mirror_obj, "enabled", mirror_enabled);
    json_push_kv_bool(&mirror_obj, "running", mirror_running);
    json_push_kv_bool(&mirror_obj, "reachable", mirror_reachable);
    json_push_kv_bool(&mirror_obj, "contract_trusted",
                      mirror_contract_trusted);
    json_push_kv_bool(&mirror_obj, "blocker_active",
                      mirror_blocker_active);
    json_push_kv_bool(&mirror_obj, "operator_action_required",
                      mirror_operator_action_required);
    json_push_kv_str(&mirror_obj, "blocker", mirror_blocker);
    json_push_kv_str(&mirror_obj, "detail", mirror_detail);
    json_push_kv(&root, "mirror", &mirror_obj);
    json_free(&mirror_obj);

    bool blocker_fields_attached = status_push_built_blocker_summary(
        &root, &blocker_summary_j, &blocker_dominant_j,
        &blocker_error_j, blockers_ok);

    if (!chain_ok)
        status_push_rpc_parse_error(&root, "chain", chain,
                                    "getblockchaininfo returned invalid data");
    if (!peers_ok)
        status_push_rpc_parse_error(&root, "peers", peers,
                                    "getpeerinfo returned invalid data");
    if (!diag_ok)
        status_push_rpc_parse_error(&root, "syncdiag", diag,
                                    "getsyncdiag returned invalid data");
    if (!download_counts_known)
        status_push_rpc_parse_error(&root, "download", dl,
                                    "downloadstats missing valid counters");
    if (!health_ok)
        status_push_rpc_parse_error(&root, "health", health,
                                    "healthcheck returned invalid data");
    if (!frontier_evidence_known)
        status_push_json_error(&root, "chain_evidence",
                               "served, indexed, or validated header unavailable",
                               NULL);
    else if (frontier_evidence_known && !frontier_evidence_consistent)
        status_push_json_error(
            &root, "chain_evidence",
            "frontier order must satisfy served H* <= indexed <= header",
            NULL);

    json_init(&raw);
    json_set_object(&raw);
    status_push_rpc_json(&raw, "chain", chain, "getblockchaininfo");
    status_push_rpc_json(&raw, "peers", peers, "getpeerinfo");
    status_push_rpc_json(&raw, "syncdiag", diag, "getsyncdiag");
    status_push_rpc_json(&raw, "downloadstats", dl, "downloadstats");
    status_push_rpc_json(&raw, "mirror", mirror, "getmirrorstatus");
    status_push_rpc_json(&raw, "health", health, "healthcheck");
    status_push_rpc_json(&raw, "agent", agent, "agent");
    status_push_rpc_json(&raw, "blockers", bl, "dumpstate blocker");
    json_push_kv(&root, "raw", &raw);
    json_free(&raw);

    char *out = blocker_fields_attached
        ? zcl_json_value_to_body(&root, "operator_summary_body") : NULL;
    json_free(&root);
    json_free(&chain_j);
    json_free(&peers_j);
    json_free(&diag_j);
    json_free(&dl_j);
    json_free(&mirror_j);
    json_free(&health_j);
    json_free(&agent_j);
    json_free(&blocker_error_j);
    json_free(&blocker_dominant_j);
    json_free(&blocker_summary_j);
    free(chain); free(peers); free(diag); free(dl); free(mirror); free(health);
    free(agent); free(bl);
    if (!out)
        return mcp_res_set_oom(res, 0, "mcp.ops", "operator summary response");
    res->body = out;
    return 0;
}

static bool status_rpc_exact_method_not_found(const char *raw)
{
    struct json_value root;
    json_init(&root);
    if (!raw || !json_read(&root, raw, strlen(raw)) ||
        root.type != JSON_OBJ) {
        json_free(&root);
        return false;
    }
    /* A mixed error+snapshot is malformed, not an old-target signal. */
    if (json_get(&root, "schema") || json_get(&root, "summary") ||
        json_get(&root, "capture") || json_get(&root, "chain") ||
        json_get(&root, "blockers")) {
        json_free(&root);
        return false;
    }
    const struct json_value *wrapped = json_get(&root, "error");
    const struct json_value *error =
        wrapped && wrapped->type == JSON_OBJ ? wrapped : &root;
    const struct json_value *code = json_get(error, "code");
    const struct json_value *message = json_get(error, "message");
    bool exact = code && code->type == JSON_INT &&
                 code->val.i == RPC_METHOD_NOT_FOUND &&
                 message && message->type == JSON_STR;
    json_free(&root);
    return exact;
}

static bool status_read_nullable_nonnegative(
    const struct json_value *object, const char *key,
    bool *known_out, int64_t *value_out)
{
    const struct json_value *value = json_get(object, key);
    if (!value)
        return false;
    if (value->type == JSON_NULL) {
        *known_out = false;
        *value_out = 0;
        return true;
    }
    if (value->type != JSON_INT || value->val.i < 0)
        return false;
    *known_out = true;
    *value_out = value->val.i;
    return true;
}

struct status_native_frontier_view {
    bool height_known;
    bool binding_known;
    bool status_known;
    bool validity_sufficient;
    bool failure_free;
    int64_t height;
};

struct status_native_chain_view {
    bool all_known;
    bool ordered;
    bool bindings_known;
    bool gap_known;
    bool consistent;
    bool validity_known;
    bool validity_ok;
    int64_t gap;
};

struct status_native_peer_view {
    bool known;
    bool stale;
    bool direction_known;
    bool ready_known;
};

static bool status_is_hex64(const char *value)
{
    if (!value || strlen(value) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static bool status_is_nonzero_hex64(const char *value)
{
    if (!status_is_hex64(value))
        return false;
    for (size_t i = 0; i < 64; i++)
        if (value[i] != '0')
            return true;
    return false;
}

static bool status_read_native_frontier(
    const struct json_value *frontier,
    struct status_native_frontier_view *out)
{
    if (!frontier || frontier->type != JSON_OBJ || !out)
        return false;
    bool height_known = false;
    bool binding_known = false;
    bool status_known = false;
    bool validity_sufficient = false;
    bool failure_free = false;
    if (!status_read_bool(frontier, "height_known", &height_known) ||
        !status_read_bool(frontier, "binding_known", &binding_known) ||
        !status_read_bool(frontier, "status_known", &status_known) ||
        !status_read_bool(frontier, "validity_sufficient",
                          &validity_sufficient) ||
        !status_read_bool(frontier, "failure_free", &failure_free))
        return false;
    bool height_value_known = false;
    int64_t height = 0;
    if (!status_read_nullable_nonnegative(frontier, "height",
                                          &height_value_known, &height) ||
        height_value_known != height_known || height > INT_MAX)
        return false;
    const struct json_value *hash = json_get(frontier, "hash");
    const struct json_value *work = json_get(frontier, "chain_work");
    bool block_status_known = false;
    int64_t block_status = 0;
    if (!status_read_nullable_nonnegative(frontier, "block_status",
                                          &block_status_known,
                                          &block_status) ||
        block_status_known != status_known || block_status > UINT32_MAX)
        return false;
    const char *source = json_get_str(json_get(frontier, "source"));
    const char *authority = json_get_str(json_get(frontier, "authority"));
    if (!hash || !work || !source || !source[0] ||
        !authority || !authority[0])
        return false;
    if (binding_known) {
        if (!height_known || hash->type != JSON_STR ||
            work->type != JSON_STR ||
            !status_is_nonzero_hex64(json_get_str(hash)) ||
            !status_is_nonzero_hex64(json_get_str(work)))
            return false;
    } else if (hash->type != JSON_NULL || work->type != JSON_NULL) {
        return false;
    }
    *out = (struct status_native_frontier_view) {
        .height_known = height_known,
        .binding_known = binding_known,
        .status_known = status_known,
        .validity_sufficient = validity_sufficient,
        .failure_free = failure_free,
        .height = height,
    };
    return true;
}

static bool status_validate_native_chain_component(
    const struct json_value *root,
    const struct json_value *summary,
    bool stable,
    bool complete,
    struct status_native_chain_view *view_out)
{
    const struct json_value *chain = json_get(root, "chain");
    const char *chain_status = json_get_str(json_get(chain, "status"));
    const char *authority = json_get_str(json_get(chain, "authority"));
    const char *trust = json_get_str(json_get(chain, "trust"));
    if (!chain || chain->type != JSON_OBJ || !chain_status ||
        !authority || strcmp(authority, "local_consensus_validation") != 0 ||
        !trust || strcmp(trust, "authoritative") != 0)
        return false;

    struct status_native_frontier_view served = {0};
    struct status_native_frontier_view indexed = {0};
    struct status_native_frontier_view header = {0};
    if (!status_read_native_frontier(json_get(chain, "served"), &served) ||
        !status_read_native_frontier(json_get(chain, "indexed"), &indexed) ||
        !status_read_native_frontier(json_get(chain, "validated_header"),
                                     &header))
        return false;

    bool summary_served_known = false;
    bool summary_indexed_known = false;
    bool summary_header_known = false;
    bool summary_target_known = false;
    bool summary_height_known = false;
    int64_t summary_served = 0;
    int64_t summary_indexed = 0;
    int64_t summary_header = 0;
    int64_t summary_target = 0;
    int64_t summary_height = 0;
    if (!status_read_nullable_nonnegative(summary, "served_height",
                                          &summary_served_known,
                                          &summary_served) ||
        !status_read_nullable_nonnegative(summary, "indexed_height",
                                          &summary_indexed_known,
                                          &summary_indexed) ||
        !status_read_nullable_nonnegative(summary, "header_height",
                                          &summary_header_known,
                                          &summary_header) ||
        !status_read_nullable_nonnegative(summary, "target_height",
                                          &summary_target_known,
                                          &summary_target) ||
        !status_read_nullable_nonnegative(summary, "height",
                                          &summary_height_known,
                                          &summary_height))
        return false;
    if (summary_served_known != served.height_known ||
        summary_indexed_known != indexed.height_known ||
        summary_header_known != header.height_known ||
        summary_height_known != served.height_known ||
        (served.height_known &&
         (summary_served != served.height || summary_height != served.height)) ||
        (indexed.height_known && summary_indexed != indexed.height) ||
        (header.height_known && summary_header != header.height) ||
        summary_target_known != header.height_known ||
        (header.height_known && summary_target != header.height))
        return false;

    const char *target_source = json_get_str(json_get(
        summary, "target_height_source"));
    if (!target_source ||
        strcmp(target_source, header.height_known
            ? "target_node.validated_header_tip" : "unavailable") != 0)
        return false;

    bool all_known = served.height_known && indexed.height_known &&
                     header.height_known;
    bool ordered = all_known && served.height <= indexed.height &&
                   indexed.height <= header.height;
    bool authority_known = false, durable_known = false;
    bool authority_match = false, ancestry_known = false;
    bool served_ancestor = false, indexed_ancestor = false;
    bool work_known = false, work_monotone = false;
    bool validity_known = false, validity_sufficient = false;
    bool failure_free = false;
    if (!status_read_bool(chain, "authority_pair_known", &authority_known) ||
        !status_read_bool(chain, "durable_authority_known", &durable_known) ||
        !status_read_bool(chain, "authority_matches_served",
                          &authority_match) ||
        !status_read_bool(chain, "ancestry_known", &ancestry_known) ||
        !status_read_bool(chain, "served_ancestor_indexed",
                          &served_ancestor) ||
        !status_read_bool(chain, "indexed_ancestor_header",
                          &indexed_ancestor) ||
        !status_read_bool(chain, "work_known", &work_known) ||
        !status_read_bool(chain, "work_monotone", &work_monotone) ||
        !status_read_bool(chain, "validity_known", &validity_known) ||
        !status_read_bool(chain, "validity_sufficient",
                          &validity_sufficient) ||
        !status_read_bool(chain, "failure_free", &failure_free))
        return false;
    bool bindings_known = served.binding_known && indexed.binding_known &&
                          header.binding_known;
    bool chain_consistent = all_known && ordered && bindings_known &&
        authority_known && durable_known && authority_match &&
        ancestry_known && served_ancestor && indexed_ancestor &&
        work_known && work_monotone && validity_known &&
        validity_sufficient && failure_free;
    const struct json_value *summary_consistent = json_get(
        summary, "chain_evidence_consistent");
    const struct json_value *root_consistent = json_get(chain, "consistent");
    if (all_known) {
        if (!summary_consistent || summary_consistent->type != JSON_BOOL ||
            summary_consistent->val.b != chain_consistent ||
            !root_consistent || root_consistent->type != JSON_BOOL ||
            root_consistent->val.b != chain_consistent)
            return false;
    } else if (!summary_consistent || summary_consistent->type != JSON_NULL ||
               !root_consistent || root_consistent->type != JSON_NULL) {
        return false;
    }

    bool summary_gap_known = false;
    bool summary_served_gap_known = false;
    bool summary_index_gap_known = false;
    bool root_gap_known = false;
    bool root_index_gap_known = false;
    int64_t summary_gap = 0;
    int64_t summary_served_gap = 0;
    int64_t summary_index_gap = 0;
    int64_t root_gap = 0;
    int64_t root_index_gap = 0;
    if (!status_read_nullable_nonnegative(summary, "gap",
                                          &summary_gap_known, &summary_gap) ||
        !status_read_nullable_nonnegative(summary, "served_gap",
                                          &summary_served_gap_known,
                                          &summary_served_gap) ||
        !status_read_nullable_nonnegative(summary, "index_gap",
                                          &summary_index_gap_known,
                                          &summary_index_gap) ||
        !status_read_nullable_nonnegative(chain, "gap",
                                          &root_gap_known, &root_gap) ||
        !status_read_nullable_nonnegative(chain, "index_gap",
                                          &root_index_gap_known,
                                          &root_index_gap))
        return false;
    bool gaps_expected = stable && chain_consistent;
    if (summary_gap_known != gaps_expected ||
        summary_served_gap_known != gaps_expected ||
        summary_index_gap_known != gaps_expected ||
        root_gap_known != gaps_expected ||
        root_index_gap_known != gaps_expected)
        return false;
    if (gaps_expected &&
        (summary_gap != header.height - served.height ||
         summary_served_gap != summary_gap ||
         summary_index_gap != header.height - indexed.height ||
         root_gap != summary_gap || root_index_gap != summary_index_gap))
        return false;

    const char *expected_status = !all_known || !bindings_known ? "partial"
        : !stable ? "unstable" : chain_consistent ? "ok" : "error";
    if (strcmp(chain_status, expected_status) != 0)
        return false;
    if (complete && (!stable || !chain_consistent))
        return false;
    if (view_out) {
        *view_out = (struct status_native_chain_view) {
            .all_known = all_known,
            .ordered = ordered,
            .bindings_known = bindings_known,
            .gap_known = gaps_expected,
            .consistent = chain_consistent,
            .validity_known = validity_known,
            .validity_ok = validity_sufficient && failure_free,
            .gap = summary_gap,
        };
    }
    return true;
}

static bool status_validate_native_peer_component(
    const struct json_value *root,
    const struct json_value *summary,
    bool complete,
    bool healthy,
    struct status_native_peer_view *view_out)
{
    const struct json_value *root_peers = json_get(root, "peers");
    const struct json_value *peers = json_get(summary, "peers");
    if (!root_peers || root_peers->type != JSON_OBJ ||
        !peers || peers->type != JSON_OBJ)
        return false;
    bool known = false, stale = false, direction_known = false;
    bool ready_known = false, max_known = false;
    if (!status_read_bool(peers, "known", &known) ||
        !status_read_bool(peers, "stale", &stale) ||
        !status_read_bool(peers, "direction_known", &direction_known) ||
        !status_read_bool(peers, "ready_known", &ready_known) ||
        !status_read_bool(peers, "max_height_known", &max_known))
        return false;
    bool root_known = false, root_stale = false;
    bool root_direction_known = false, root_ready_known = false;
    bool root_max_known = false;
    if (!status_read_bool(root_peers, "known", &root_known) ||
        !status_read_bool(root_peers, "stale", &root_stale) ||
        !status_read_bool(root_peers, "direction_known",
                          &root_direction_known) ||
        !status_read_bool(root_peers, "ready_known", &root_ready_known) ||
        !status_read_bool(root_peers, "advertised_max_height_known",
                          &root_max_known) ||
        root_known != known || root_stale != stale ||
        root_direction_known != direction_known ||
        root_ready_known != ready_known || root_max_known != max_known)
        return false;

    bool total_known = false, inbound_known = false, outbound_known = false;
    bool ready_value_known = false, max_value_known = false;
    bool root_total_known = false, root_inbound_known = false;
    bool root_outbound_known = false, root_ready_value_known = false;
    bool root_max_value_known = false;
    int64_t total = 0, inbound = 0, outbound = 0, ready = 0, max_height = 0;
    int64_t root_total = 0, root_inbound = 0, root_outbound = 0;
    int64_t root_ready = 0, root_max_height = 0;
    if (!status_read_nullable_nonnegative(peers, "total", &total_known,
                                          &total) ||
        !status_read_nullable_nonnegative(peers, "inbound", &inbound_known,
                                          &inbound) ||
        !status_read_nullable_nonnegative(peers, "outbound", &outbound_known,
                                          &outbound) ||
        !status_read_nullable_nonnegative(peers, "ready",
                                          &ready_value_known, &ready) ||
        !status_read_nullable_nonnegative(peers, "max_height",
                                          &max_value_known, &max_height) ||
        !status_read_nullable_nonnegative(root_peers, "total",
                                          &root_total_known, &root_total) ||
        !status_read_nullable_nonnegative(root_peers, "inbound",
                                          &root_inbound_known, &root_inbound) ||
        !status_read_nullable_nonnegative(root_peers, "outbound",
                                          &root_outbound_known,
                                          &root_outbound) ||
        !status_read_nullable_nonnegative(root_peers, "ready",
                                          &root_ready_value_known,
                                          &root_ready) ||
        !status_read_nullable_nonnegative(root_peers,
                                          "advertised_max_height",
                                          &root_max_value_known,
                                          &root_max_height))
        return false;
    if (total_known != known || root_total_known != known ||
        inbound_known != direction_known ||
        outbound_known != direction_known ||
        root_inbound_known != direction_known ||
        root_outbound_known != direction_known ||
        ready_value_known != ready_known ||
        root_ready_value_known != ready_known ||
        max_value_known != max_known || root_max_value_known != max_known ||
        (known && total != root_total) ||
        (direction_known &&
         (inbound != root_inbound || outbound != root_outbound ||
          inbound + outbound != total)) ||
        (ready_known && (ready != root_ready || ready > total)) ||
        (max_known && max_height != root_max_height))
        return false;

    const struct json_value *generation = json_get(peers, "generation");
    const struct json_value *root_generation = json_get(root_peers,
                                                        "generation");
    const char *root_status = json_get_str(json_get(root_peers, "status"));
    const char *expected_status = !known ? "error" : stale ? "stale"
        : !direction_known || !ready_known ? "partial" : "ok";
    const char *authority = json_get_str(json_get(root_peers, "authority"));
    const char *height_trust = json_get_str(json_get(
        root_peers, "peer_height_trust"));
    const char *summary_height_trust = json_get_str(json_get(
        peers, "max_height_trust"));
    const struct json_value *age = json_get(root_peers, "age_seconds");
    if (!generation || generation->type != JSON_INT || generation->val.i < 0 ||
        !root_generation || root_generation->type != JSON_INT ||
        root_generation->val.i != generation->val.i ||
        !root_status || strcmp(root_status, expected_status) != 0 ||
        !authority || strcmp(authority, "live_connman_snapshot") != 0 ||
        !height_trust || strcmp(height_trust,
                                "untrusted_peer_advertisement") != 0 ||
        !summary_height_trust || strcmp(summary_height_trust,
                                "untrusted_peer_advertisement") != 0 ||
        !age || age->type != JSON_INT || age->val.i < -1)
        return false;
    if (complete && (!known || stale || !direction_known || !ready_known))
        return false;
    if (healthy && (!known || stale || total <= 0 || !ready_known ||
                    ready <= 0))
        return false;
    if (view_out) {
        *view_out = (struct status_native_peer_view) {
            .known = known,
            .stale = stale,
            .direction_known = direction_known,
            .ready_known = ready_known,
        };
    }
    return true;
}

static bool status_validate_native_runtime_components(
    const struct json_value *root,
    const struct json_value *summary,
    bool complete,
    bool healthy,
    bool hard_blocker)
{
    const struct json_value *download = json_get(root, "download");
    const struct json_value *summary_download = json_get(summary, "download");
    bool download_known = false;
    if (!download || download->type != JSON_OBJ ||
        !summary_download || summary_download->type != JSON_OBJ ||
        !status_read_bool(summary_download, "known", &download_known))
        return false;
    const char *download_status = json_get_str(json_get(download, "status"));
    const char *capture_model = json_get_str(json_get(download,
                                                       "capture_model"));
    if (!download_status || strcmp(download_status,
                                   download_known ? "ok" : "error") != 0 ||
        !capture_model || strcmp(capture_model, "single_leaf_lock") != 0)
        return false;
    static const char *const root_counter_keys[] = {
        "requested", "received", "timed_out", "in_flight", "queued",
    };
    int64_t root_counters[5] = {0};
    for (size_t i = 0; i < 5; i++) {
        bool known = false;
        if (!status_read_nullable_nonnegative(download, root_counter_keys[i],
                                              &known, &root_counters[i]) ||
            known != download_known)
            return false;
    }
    bool summary_in_flight_known = false, summary_queued_known = false;
    int64_t summary_in_flight = 0, summary_queued = 0;
    if (!status_read_nullable_nonnegative(summary_download, "in_flight",
                                          &summary_in_flight_known,
                                          &summary_in_flight) ||
        !status_read_nullable_nonnegative(summary_download, "queued",
                                          &summary_queued_known,
                                          &summary_queued) ||
        summary_in_flight_known != download_known ||
        summary_queued_known != download_known ||
        (download_known &&
         (summary_in_flight != root_counters[3] ||
          summary_queued != root_counters[4])))
        return false;
    const char *sync_state = json_get_str(json_get(summary, "sync_state"));
    const char *download_sync = json_get_str(json_get(summary_download,
                                                       "sync_state"));
    if (!sync_state || !sync_state[0] || !download_sync ||
        strcmp(sync_state, download_sync) != 0 ||
        (complete && (!download_known || strcmp(sync_state, "unknown") == 0)))
        return false;

    const struct json_value *conditions = json_get(root, "conditions");
    const char *condition_status = json_get_str(json_get(conditions, "status"));
    const char *condition_model = json_get_str(json_get(
        conditions, "capture_model"));
    int64_t registered = 0, active = 0, unresolved = 0, critical = 0;
    if (!conditions || conditions->type != JSON_OBJ ||
        !condition_status || strcmp(condition_status, "ok") != 0 ||
        !condition_model || strcmp(condition_model,
            "single_registry_pass_per_condition_atomic_fields") != 0 ||
        !status_read_nonnegative_int(conditions, "registered_count",
                                     &registered) ||
        !status_read_nonnegative_int(conditions, "active_count", &active) ||
        !status_read_nonnegative_int(conditions, "unresolved_count",
                                     &unresolved) ||
        !status_read_nonnegative_int(conditions,
                                     "unresolved_critical_count", &critical) ||
        active > registered || unresolved > active || critical > unresolved)
        return false;
    int64_t summary_active = 0, summary_unresolved = 0;
    if (!status_read_nonnegative_int(summary, "active_conditions",
                                     &summary_active) ||
        !status_read_nonnegative_int(summary, "unresolved_conditions",
                                     &summary_unresolved) ||
        summary_active != active || summary_unresolved != unresolved ||
        (healthy && (active != 0 || unresolved != 0)))
        return false;

    const struct json_value *latch = json_get(root, "operator_latch");
    const char *latch_status = json_get_str(json_get(latch, "status"));
    const struct json_value *since = json_get(latch, "since_unix");
    const struct json_value *detail = json_get(latch, "detail");
    bool latch_active = false, read_only = false, operator_needed = false;
    if (!latch || latch->type != JSON_OBJ ||
        !latch_status || strcmp(latch_status, "ok") != 0 ||
        !status_read_bool(latch, "active", &latch_active) ||
        !status_read_bool(latch, "read_only_capture", &read_only) ||
        !read_only || !since || since->type != JSON_INT || since->val.i < 0 ||
        !detail || detail->type != JSON_STR ||
        !status_read_bool(summary, "operator_needed", &operator_needed) ||
        operator_needed != (latch_active || hard_blocker || critical > 0) ||
        (healthy && operator_needed))
        return false;
    return true;
}

static bool status_native_status_is_known(const char *status)
{
    return status &&
        (strcmp(status, "healthy") == 0 ||
         strcmp(status, "catching_up") == 0 ||
         strcmp(status, "degraded") == 0 ||
         strcmp(status, "blocked") == 0 ||
         strcmp(status, "operator_needed") == 0);
}

static bool status_native_invariant_is(
    const struct json_value *invariants,
    const char *name,
    const char *expected_status)
{
    const struct json_value *invariant = json_get(invariants, name);
    const char *status = json_get_str(json_get(invariant, "status"));
    const char *detail = json_get_str(json_get(invariant, "detail"));
    return invariant && invariant->type == JSON_OBJ && status &&
           strcmp(status, expected_status) == 0 && detail && detail[0];
}

/* Snapshot trust is content-addressed.  Git object IDs are optional trace
 * metadata and therefore deliberately absent from this acceptance predicate. */
static bool status_native_source_id_valid(const struct json_value *value)
{
    if (!value || value->type != JSON_STR)
        return false;
    const char *source_id = json_get_str(value);
    if (!source_id || strlen(source_id) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        if (!((source_id[i] >= '0' && source_id[i] <= '9') ||
              (source_id[i] >= 'a' && source_id[i] <= 'f')))
            return false;
    }
    return true;
}

static bool status_validate_native_operator_snapshot(
    const char *raw, struct json_value *root_out, const char **reason_out)
{
    const char *reason = "invalid JSON object";
    json_init(root_out);
    if (!raw || !json_read(root_out, raw, strlen(raw)) ||
        root_out->type != JSON_OBJ)
        goto fail;
    if (status_json_is_rpc_error(root_out)) {
        reason = "target returned an RPC error";
        goto fail;
    }

    const struct json_value *schema = json_get(root_out, "schema");
    const struct json_value *version = json_get(root_out, "schema_version");
    const struct json_value *api = json_get(root_out, "api_version");
    const struct json_value *locus = json_get(root_out, "execution_locus");
    const struct json_value *producer = json_get(root_out, "producer");
    const struct json_value *authority = json_get(root_out, "authority");
    const struct json_value *trust = json_get(root_out, "trust");
    const struct json_value *source_id = json_get(root_out,
                                                  "source_id_sha256");
    const struct json_value *network = json_get(root_out, "network");
    const struct json_value *pid = json_get(root_out, "process_id");
    const struct json_value *instance = json_get(root_out, "node_instance_id");
    const struct json_value *identity_at = json_get(
        root_out, "identity_initialized_at_unix_us");
    const struct json_value *sequence = json_get(root_out, "snapshot_sequence");
    const struct json_value *status = json_get(root_out, "status");
    const struct json_value *healthy = json_get(root_out, "healthy");
    const struct json_value *complete = json_get(root_out, "verdict_complete");
    const struct json_value *primary = json_get(root_out, "primary_blocker");
    const struct json_value *next = json_get(root_out, "next_action");
    if (schema && schema->type == JSON_STR &&
        strcmp(json_get_str(schema), "zcl.operator_snapshot.v1") == 0 &&
        version && version->type == JSON_INT && version->val.i == 1) {
        reason = "legacy zcl.operator_snapshot.v1 is untrusted; upgrade the "
                 "target for the v2 exact source-identity contract";
        goto fail;
    }
    if (!schema || schema->type != JSON_STR ||
        strcmp(json_get_str(schema), "zcl.operator_snapshot.v2") != 0 ||
        !version || version->type != JSON_INT || version->val.i != 2 ||
        !api || api->type != JSON_STR || strcmp(json_get_str(api), "v2") != 0 ||
        !locus || locus->type != JSON_STR ||
        strcmp(json_get_str(locus), "target_node") != 0 ||
        !producer || producer->type != JSON_STR ||
        strcmp(json_get_str(producer),
               "event_operator_snapshot_controller") != 0 ||
        !authority || authority->type != JSON_STR ||
        strcmp(json_get_str(authority), "target_node_internal_state") != 0 ||
        !trust || trust->type != JSON_STR ||
        strcmp(json_get_str(trust), "target_owned_evidence") != 0 ||
        !status_native_source_id_valid(source_id) ||
        !network || network->type != JSON_STR || !json_get_str(network)[0] ||
        !pid || pid->type != JSON_INT || pid->val.i <= 0 ||
        !instance || instance->type != JSON_STR || !json_get_str(instance)[0] ||
        !identity_at || identity_at->type != JSON_INT || identity_at->val.i < 0 ||
        !sequence || sequence->type != JSON_INT || sequence->val.i <= 0 ||
        !status || status->type != JSON_STR ||
        !status_native_status_is_known(json_get_str(status)) ||
        !healthy || healthy->type != JSON_BOOL ||
        healthy->val.b != (strcmp(json_get_str(status), "healthy") == 0) ||
        !complete || complete->type != JSON_BOOL ||
        !primary || primary->type != JSON_STR ||
        !next || next->type != JSON_STR) {
        reason = "missing or invalid snapshot identity/verdict fields";
        goto fail;
    }

    const struct json_value *capture = json_get(root_out, "capture");
    const struct json_value *model = json_get(capture, "model");
    const struct json_value *started = json_get(capture, "started_at_unix_us");
    const struct json_value *completed = json_get(capture,
                                                   "completed_at_unix_us");
    const struct json_value *duration = json_get(capture, "duration_us");
    const struct json_value *skew = json_get(
        capture, "component_skew_upper_bound_us");
    const struct json_value *attempts = json_get(capture, "attempts");
    const struct json_value *stable = json_get(capture,
                                               "critical_frontier_stable");
    const struct json_value *inputs = json_get(capture,
                                               "verdict_inputs_complete");
    const struct json_value *partial = json_get(capture, "partial");
    const struct json_value *linear = json_get(capture,
                                               "globally_linearizable");
    if (!capture || capture->type != JSON_OBJ ||
        !model || model->type != JSON_STR || strcmp(json_get_str(model),
            "single_target_bounded_component_snapshots") != 0 ||
        !started || started->type != JSON_INT || started->val.i < 0 ||
        !completed || completed->type != JSON_INT ||
        completed->val.i < started->val.i ||
        !duration || duration->type != JSON_INT || duration->val.i < 0 ||
        completed->val.i - started->val.i != duration->val.i ||
        !skew || skew->type != JSON_INT || skew->val.i != duration->val.i ||
        !attempts || attempts->type != JSON_INT || attempts->val.i < 1 ||
        attempts->val.i > 2 ||
        !stable || stable->type != JSON_BOOL ||
        (!stable->val.b && attempts->val.i != 2) ||
        !inputs || inputs->type != JSON_BOOL ||
        inputs->val.b != complete->val.b ||
        !partial || partial->type != JSON_BOOL ||
        partial->val.b == complete->val.b ||
        !linear || linear->type != JSON_BOOL || linear->val.b) {
        reason = "invalid capture window/completeness contract";
        goto fail;
    }

    const struct json_value *summary = json_get(root_out, "summary");
    const struct json_value *summary_schema = json_get(summary, "schema");
    const struct json_value *summary_version = json_get(summary, "schema_version");
    const struct json_value *summary_api = json_get(summary, "api_version");
    const struct json_value *summary_locus = json_get(summary, "execution_locus");
    const struct json_value *summary_source = json_get(summary, "source_rpc");
    const struct json_value *summary_source_id = json_get(
        summary, "source_id_sha256");
    const struct json_value *summary_network = json_get(summary, "network");
    const struct json_value *summary_status = json_get(summary, "status");
    const struct json_value *summary_healthy = json_get(summary, "healthy");
    const struct json_value *summary_complete = json_get(summary,
                                                         "verdict_complete");
    const struct json_value *summary_atomic = json_get(summary, "atomic");
    const struct json_value *summary_fallback = json_get(
        summary, "compatibility_fallback");
    if (!summary || summary->type != JSON_OBJ ||
        !summary_schema || summary_schema->type != JSON_STR ||
        strcmp(json_get_str(summary_schema), "zcl.operator_summary.v2") != 0 ||
        !summary_version || summary_version->type != JSON_INT ||
        summary_version->val.i != 2 ||
        !summary_api || summary_api->type != JSON_STR ||
        strcmp(json_get_str(summary_api), "v2") != 0 ||
        !summary_locus || summary_locus->type != JSON_STR ||
        strcmp(json_get_str(summary_locus), "target_node") != 0 ||
        !summary_source || summary_source->type != JSON_STR ||
        strcmp(json_get_str(summary_source), "operatorsnapshot") != 0 ||
        !status_native_source_id_valid(summary_source_id) ||
        strcmp(json_get_str(summary_source_id), json_get_str(source_id)) != 0 ||
        !summary_network || summary_network->type != JSON_STR ||
        strcmp(json_get_str(summary_network), json_get_str(network)) != 0 ||
        !summary_status || summary_status->type != JSON_STR ||
        strcmp(json_get_str(summary_status), json_get_str(status)) != 0 ||
        !summary_healthy || summary_healthy->type != JSON_BOOL ||
        summary_healthy->val.b != healthy->val.b ||
        !summary_complete || summary_complete->type != JSON_BOOL ||
        summary_complete->val.b != complete->val.b ||
        !summary_atomic || summary_atomic->type != JSON_BOOL ||
        summary_atomic->val.b || !summary_fallback ||
        summary_fallback->type != JSON_BOOL || summary_fallback->val.b ||
        json_get_int(json_get(summary, "process_id")) != pid->val.i ||
        strcmp(json_get_str(json_get(summary, "node_instance_id")),
               json_get_str(instance)) != 0 ||
        json_get_int(json_get(summary, "snapshot_sequence")) != sequence->val.i ||
        json_get_int(json_get(summary, "capture_started_at_unix_us")) !=
            started->val.i ||
        json_get_int(json_get(summary, "capture_completed_at_unix_us")) !=
            completed->val.i ||
        json_get_int(json_get(summary, "component_skew_upper_bound_us")) !=
            skew->val.i ||
        json_get_bool(json_get(summary, "critical_frontier_stable")) !=
            stable->val.b ||
        strcmp(json_get_str(json_get(summary, "primary_blocker")),
               json_get_str(primary)) != 0 ||
        strcmp(json_get_str(json_get(summary, "next_action")),
               json_get_str(next)) != 0) {
        reason = "native summary projection disagrees with snapshot identity";
        goto fail;
    }

    struct status_native_chain_view chain_view = {0};
    if (!status_validate_native_chain_component(
            root_out, summary, stable->val.b, complete->val.b, &chain_view)) {
        reason = "chain component disagrees with summary or proof invariants";
        goto fail;
    }

    const struct json_value *blockers = json_get(root_out, "blockers");
    const struct json_value *summary_blockers = json_get(summary, "blockers");
    const struct json_value *entries = json_get(blockers, "blockers");
    bool blockers_known = false;
    if (!blockers || blockers->type != JSON_OBJ ||
        !summary_blockers || summary_blockers->type != JSON_OBJ ||
        !status_json_equal(blockers, summary_blockers) ||
        !entries || entries->type != JSON_ARR ||
        !status_read_bool(blockers, "known", &blockers_known) ||
        !blockers_known || !status_blocker_counts_match(blockers, entries)) {
        reason = "malformed or contradictory typed blocker snapshot";
        goto fail;
    }
    int64_t active = json_get_int(json_get(blockers, "active_count"));
    bool hard_blocker = json_get_int(json_get(blockers, "permanent_count")) > 0 ||
                        json_get_int(json_get(blockers, "resource_count")) > 0;

    struct status_native_peer_view peer_view = {0};
    if (!status_validate_native_peer_component(
            root_out, summary, complete->val.b, healthy->val.b, &peer_view)) {
        reason = "peer component disagrees with summary or readiness contract";
        goto fail;
    }
    if (!status_validate_native_runtime_components(
            root_out, summary, complete->val.b, healthy->val.b,
            hard_blocker)) {
        reason = "runtime components disagree with summary or operator policy";
        goto fail;
    }

    const struct json_value *invariants = json_get(root_out, "invariants");
    const char *frontier_status = !chain_view.all_known ? "unknown"
        : chain_view.ordered ? "pass" : "fail";
    const char *lineage_status = !chain_view.bindings_known ? "unknown"
        : chain_view.consistent ? "pass" : "fail";
    const char *peer_direction_status = !peer_view.direction_known
        ? "unknown" : "pass";
    const char *validity_status = !chain_view.validity_known ? "unknown"
        : chain_view.validity_ok ? "pass" : "fail";
    if (!invariants || invariants->type != JSON_OBJ ||
        !status_native_invariant_is(invariants, "critical_frontier_stable",
                                    stable->val.b ? "pass" : "fail") ||
        !status_native_invariant_is(invariants, "frontier_order",
                                    frontier_status) ||
        !status_native_invariant_is(invariants, "chain_lineage_and_work",
                                    lineage_status) ||
        !status_native_invariant_is(invariants, "frontier_validity",
                                    validity_status) ||
        !status_native_invariant_is(invariants, "blocker_counts", "pass") ||
        !status_native_invariant_is(invariants, "peer_direction_sum",
                                    peer_direction_status)) {
        reason = "invariant projection disagrees with captured components";
        goto fail;
    }

    const char *sync_state = json_get_str(json_get(summary, "sync_state"));
    if (healthy->val.b &&
        (!complete->val.b || !stable->val.b || !chain_view.consistent ||
         !chain_view.gap_known || chain_view.gap != 0 || active != 0 ||
         !sync_state || strcmp(sync_state, "at_tip") != 0)) {
        reason = "healthy verdict lacks complete supporting evidence";
        goto fail;
    }
    if (reason_out)
        *reason_out = NULL;
    return true;

fail:
    if (reason_out)
        *reason_out = reason;
    return false;
}

static int status_native_snapshot_error(struct mcp_response *res,
                                        char *raw,
                                        const char *reason)
{
    free(raw);
    res->error = MCP_ERR_HANDLER_FAILED;
    snprintf(res->error_message, sizeof(res->error_message),
             "operatorsnapshot rejected: %s",
             reason ? reason : "invalid target response");
    LOG_ERR("mcp.ops", "operatorsnapshot rejected: %s",
            reason ? reason : "invalid target response");
    return 0;
}

static int h_zcl_operator_snapshot(const struct mcp_request *req,
                                   struct mcp_response *res)
{
    (void)req;
    char *raw = mcp_node_rpc("operatorsnapshot", NULL);
    if (status_rpc_exact_method_not_found(raw))
        return status_native_snapshot_error(
            res, raw, "target does not support operatorsnapshot");
    struct json_value root;
    const char *reason = NULL;
    if (!status_validate_native_operator_snapshot(raw, &root, &reason)) {
        json_free(&root);
        return status_native_snapshot_error(res, raw, reason);
    }
    json_free(&root);
    res->body = raw;
    return 0;
}

static int h_zcl_operator_summary(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    char *raw = mcp_node_rpc("operatorsnapshot", NULL);
    if (status_rpc_exact_method_not_found(raw)) {
        free(raw);
        return h_zcl_operator_summary_compat(req, res);
    }
    struct json_value root;
    const char *reason = NULL;
    if (!status_validate_native_operator_snapshot(raw, &root, &reason)) {
        json_free(&root);
        return status_native_snapshot_error(res, raw, reason);
    }
    const struct json_value *summary = json_get(&root, "summary");
    char *body = zcl_json_value_to_body((struct json_value *)summary,
                                    "native_operator_summary_body");
    json_free(&root);
    free(raw);
    if (!body)
        return mcp_res_set_oom(res, 0, "mcp.ops",
                               "native operator summary response");
    res->body = body;
    return 0;
}

static int h_zcl_health(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    return mcp_return_rpc_body(res, mcp_node_rpc("healthcheck", NULL),
                                "healthcheck", "mcp.ops");
}

static int h_zcl_agent_impact(const struct mcp_request *req,
                              struct mcp_response *res)
{
    const struct json_value *files = json_get(req->args, "files");
    char *params = NULL;
    if (files && files->type == JSON_ARR) {
        params = zcl_json_value_to_body((struct json_value *)files,
                                    "agent_impact_params");
        if (!params)
            return mcp_res_set_oom(res, 0, "mcp.ops", "agent impact params");
    }

    char *body = mcp_node_rpc("agentimpact", params ? params : "[]");
    free(params);
    return mcp_return_rpc_body(res, body, "agentimpact", "mcp.ops");
}

static int h_zcl_agent_deploy_guard(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p,
        json_get_str_or(req->args, "action", "canonical-deploy"));
    char *params = mcp_params_to_json(&p);
    char *body = params ? mcp_node_rpc("agentdeployguard", params) : NULL;
    free(params);
    return mcp_return_rpc_body(res, body, "agentdeployguard", "mcp.ops");
}

static int h_zcl_agent_copy_prove(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, json_get_str_or(req->args, "slug", ""));
    mcp_params_push_str(&p, json_get_str_or(req->args, "src", ""));
    mcp_params_push_str(&p, json_get_str_or(req->args, "args", ""));
    mcp_params_push_int(&p, json_get_int_or(req->args, "expect_climb_past", -1));
    mcp_params_push_int(&p, json_get_int_or(req->args, "deadline_secs", 180));
    mcp_params_push_bool(&p, json_get_bool_or(req->args, "full", false));
    mcp_params_push_bool(&p, json_get_bool_or(req->args, "no_run", false));
    char *params = mcp_params_to_json(&p);
    char *body = params ? mcp_node_rpc("agentcopyprove", params) : NULL;
    free(params);
    return mcp_return_rpc_body(res, body, "agentcopyprove", "mcp.ops");
}

static int h_zcl_agent_test(const struct mcp_request *req,
                            struct mcp_response *res)
{
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, json_get_str_or(req->args, "kind", ""));
    mcp_params_push_str(&p, json_get_str_or(req->args, "name", ""));
    char *params = mcp_params_to_json(&p);
    char *body = params ? mcp_node_rpc("agenttest", params) : NULL;
    free(params);
    return mcp_return_rpc_body(res, body, "agenttest", "mcp.ops");
}

static int h_zcl_agent_liveness(const struct mcp_request *req,
                                struct mcp_response *res)
{
    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, json_get_str_or(req->args, "mode", "brief"));
    char *params = mcp_params_to_json(&p);
    char *body = params ? mcp_node_rpc("agentliveness", params) : NULL;
    free(params);
    return mcp_return_rpc_body(res, body, "agentliveness", "mcp.ops");
}

static int h_zcl_agent_diagnose(const struct mcp_request *req,
                                struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_agent_diagnose_body(req->args, &e);
    return ops_native_wrap(res, body, &e);
}

DEFINE_PT(h_zcl_mirror_status, "getmirrorstatus",       "mcp.ops")
DEFINE_PT(h_zcl_filemanifest,  "getfilemanifeststatus", "mcp.ops")

static int h_zcl_events(const struct mcp_request *req, struct mcp_response *res)
{
    char params[64];
    snprintf(params, sizeof(params), "[%lld]",
             (long long)json_get_int_or(req->args, "count", 20));
    return mcp_return_rpc_body(res, mcp_node_rpc("eventlog", params),
                                "eventlog", "mcp.ops");
}

static int h_zcl_timeline(const struct mcp_request *req,
                          struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_timeline_body(req->args, &e);
    return ops_native_wrap(res, body, &e);
}

static int h_zcl_rpc(const struct mcp_request *req, struct mcp_response *res)
{
    const char *m = json_get_str(json_get(req->args, "method"));
    return mcp_return_rpc_body(res,
                                mcp_node_rpc(m, json_get_str_or(req->args, "params", NULL)),
                                m ? m : "(null)", "mcp.ops");
}

/* zcl_kpi — single call that returns every subsystem KPI. Used by
 * operators to take the pulse of the node in one shot. Each nested
 * field is the raw result of the corresponding RPC, so field shapes
 * remain stable over time — we just add new top-level fields. */
static int h_zcl_kpi(const struct mcp_request *req, struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_kpi_body(req->args, &e);
    return ops_native_wrap(res, body, &e);
}

static int h_zcl_self_heal_stats(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    (void)req;
    struct self_heal_scan_stats stats;
    process_block_self_heal_stats_snapshot(&stats);

    char *out = zcl_malloc(512, "self_heal_stats_body");
    if (!out)
        return mcp_res_set_oom(res, 512, "mcp.ops", "self-heal stats response");

    snprintf(out, 512,
        "{"
        "\"tx_index_hits\":%llu,"
        "\"scan_hits\":%llu,"
        "\"scan_exhausted\":%llu,"
        "\"scan_blocks_checked_total\":%llu,"
        "\"scan_depth_limit\":%d"
        "}",
        (unsigned long long)stats.tx_index_hits,
        (unsigned long long)stats.scan_hits,
        (unsigned long long)stats.scan_exhausted,
        (unsigned long long)stats.scan_blocks_checked_total,
        process_block_self_heal_scan_depth_limit());
    res->body = out;
    return 0;
}

/* ── zcl_syncdiag ─────────────────────────────────────────────── */

/* Combines getsyncdiag (watchdog, header counters, chain/header heights)
 * with download queue stats and peer max height into a single response
 * for diagnosing sync issues without multiple tool calls. */
static int h_zcl_syncdiag(const struct mcp_request *req,
                          struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_syncdiag_body(req->args, &e);
    return ops_native_wrap(res, body, &e);
}

/* zcl_rebuild_recent — bounded recovery: fetch the recent block range from
 * the legacy advisory source and connect it through local consensus
 * validation, reorging off any stale local fork. Destructive (mutates the
 * live chainstate) but never wipes the UTXO set and never bypasses
 * validation. */
static int h_zcl_rebuild_recent(const struct mcp_request *req,
                                struct mcp_response *res)
{
    const struct json_value *fh = json_get(req->args, "from_height");
    char params[64];
    if (fh)
        snprintf(params, sizeof(params), "[%lld]",
                 (long long)json_get_int(fh));
    else
        snprintf(params, sizeof(params), "[]");
    return mcp_return_rpc_body(res, mcp_node_rpc("rebuild_recent", params),
                                "rebuild_recent", "mcp.ops");
}

/* zcl_blockers — dedicated MCP tool returning the typed blocker
 * registry.
 *
 * The generic `zcl_state subsystem=blocker` primitive already exposes
 * the JSON dump, but operators expect a top-level tool name for "show
 * me what's actively blocking the node right now." It preserves the target
 * state fields and adds target provenance plus the derived dominant entry;
 * no subsystem= argument is needed. */
static int h_zcl_blockers(const struct mcp_request *req,
                          struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_blockers_body(req->args, &e);
    return ops_native_wrap(res, body, &e);
}

/* ── Phase 6b postmortem capsules ───────────────────────────── */

static int h_zcl_postmortem_list(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_postmortem_list_body(req->args, &e);
    return ops_native_wrap(res, body, &e);
}

static int h_zcl_postmortem_replay(const struct mcp_request *req,
                                   struct mcp_response *res)
{
    const char *path = json_get_str_or(req->args, "path", NULL);
    if (!path || !*path) {
        res->error = MCP_ERR_MISSING_PARAM;
        snprintf(res->error_message, sizeof(res->error_message),
                 "path is required");
        LOG_WARN("mcp.ops", "postmortem_replay: %s", res->error_message);
        return 0;
    }

    int64_t limit_i = json_get_int_or(req->args, "limit", 100);
    if (limit_i < 1) limit_i = 1;
    if (limit_i > 1000) limit_i = 1000;
    size_t limit = (size_t)limit_i;

    seed_tape_t *tape = postmortem_load(path);
    if (!tape) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "failed to load postmortem tape from %s", path);
        LOG_ERR("mcp.ops", "failed to load postmortem tape path=%s", path);
        return 0;
    }

    enum { POSTMORTEM_REPLAY_PAYLOAD_CAP = 65536 };
    uint8_t *payload = zcl_malloc(POSTMORTEM_REPLAY_PAYLOAD_CAP,
                                  "mcp.postmortem.payload");
    if (!payload) {
        seed_tape_close(tape);
        return mcp_res_set_oom(res, POSTMORTEM_REPLAY_PAYLOAD_CAP, "mcp.ops",
                               "postmortem replay payload");
    }

    struct json_value root, events;
    json_init(&root);   json_set_object(&root);
    json_init(&events); json_set_array(&events);
    json_push_kv_str(&root, "path", path);
    json_push_kv_int(&root, "limit", (int64_t)limit);

    size_t returned = 0;
    int rc = 0;
    for (; returned < limit; returned++) {
        uint8_t type = 0;
        size_t payload_len = 0;
        rc = seed_tape_next_event(tape, &type, payload,
                                  POSTMORTEM_REPLAY_PAYLOAD_CAP,
                                  &payload_len);
        if (rc == -ENOENT) break;
        if (rc != 0) break;

        char *hex = zcl_malloc(payload_len * 2 + 1,
                               "mcp.postmortem.payload_hex");
        if (!hex) {
            rc = -ENOMEM;
            break;
        }
        HexStr(payload, payload_len, false, hex, payload_len * 2 + 1);

        struct json_value item;
        json_init(&item); json_set_object(&item);
        json_push_kv_int(&item, "index", (int64_t)returned);
        json_push_kv_int(&item, "type", (int64_t)type);
        json_push_kv_int(&item, "payload_len", (int64_t)payload_len);
        json_push_kv_str(&item, "payload_hex", hex);
        json_push_back(&events, &item);
        json_free(&item);
        free(hex);
    }

    free(payload);
    seed_tape_close(tape);

    if (rc != 0 && rc != -ENOENT) {
        json_free(&events);
        json_free(&root);
        res->error = (rc == -ENOMEM) ? MCP_ERR_INTERNAL : MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "postmortem replay failed for %s (rc=%d)", path, rc);
        LOG_ERR("mcp.ops", "postmortem replay failed path=%s rc=%d", path, rc);
        return 0;
    }

    json_push_kv_int(&root, "returned", (int64_t)returned);
    json_push_kv_bool(&root, "truncated", returned == limit);
    json_push_kv(&root, "events", &events);

    char *body = zcl_json_value_to_body(&root, "mcp.postmortem.replay.body");
    json_free(&events);
    json_free(&root);
    if (!body)
        return mcp_res_set_oom(res, 0, "mcp.ops", "postmortem replay response");
    res->body = body;
    return 0;
}

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_events[] = {
    { "count", MCP_PARAM_INT, false, "Number of events",
      1, 1000, 0, 0, NULL, "20" },
};
static const struct mcp_param_spec p_timeline[] = {
    { "category", MCP_PARAM_STR, false,
      "Timeline category: all, tcp, peer, message, sync, snapshot, chain, validation, condition, oracle, mirror, boot, db, wallet, mempool, disk, mcp, net",
      0, 0, 0, 32,
      "all,tcp,peer,message,sync,snapshot,chain,validation,condition,oracle,mirror,boot,db,wallet,mempool,disk,mcp,net",
      "\"all\"" },
    { "count", MCP_PARAM_INT, false, "Number of events",
      1, 1000, 0, 0, NULL, "50" },
    { "scan_count", MCP_PARAM_INT, false,
      "Bounded number of retained category events to scan before filters",
      1, 65536, 0, 0, NULL, "1000" },
    { "since_secs", MCP_PARAM_INT, false,
      "Only include events from the last N seconds",
      1, 604800, 0, 0, NULL, NULL },
    { "since_us", MCP_PARAM_INT, false,
      "Only include events with timestamp_us >= this absolute value",
      0, 0, 0, 0, NULL, NULL },
    { "peer", MCP_PARAM_INT, false, "Exact peer id filter",
      0, 1000000, 0, 0, NULL, NULL },
    { "height", MCP_PARAM_INT, false,
      "Height token filter over typed event payloads",
      0, 0, 0, 0, NULL, NULL },
    { "reducer_stage", MCP_PARAM_STR, false,
      "Reducer stage/payload substring filter",
      0, 0, 0, 64, NULL, NULL },
    { "condition", MCP_PARAM_STR, false,
      "Condition name/payload substring filter",
      0, 0, 0, 96, NULL, NULL },
    { "deploy", MCP_PARAM_STR, false,
      "Deploy/build payload substring filter",
      0, 0, 0, 96, NULL, NULL },
    { "lane", MCP_PARAM_STR, false,
      "Operator lane payload substring filter",
      0, 0, 0, 64, NULL, NULL },
};
static const struct mcp_param_spec p_rpc[] = {
    { "method", MCP_PARAM_STR, true,  "RPC method name",
      0, 0, 1, 128, NULL, NULL },
    { "params", MCP_PARAM_STR, false, "JSON params array",
      0, 0, 0, 0, NULL, "\"[]\"" },
};
static const struct mcp_param_spec p_agent_impact[] = {
    { "files", MCP_PARAM_ARRAY, false, "Changed repository file paths",
      0, 0, 0, 0, NULL, "[]" },
};
static const struct mcp_param_spec p_agent_deploy_guard[] = {
    { "action", MCP_PARAM_STR, false,
      "Action to evaluate: canonical-deploy, canonical-restart, deploy, or restart",
      0, 0, 0, 64, NULL, "\"canonical-deploy\"" },
};
static const struct mcp_param_spec p_agent_copy_prove[] = {
    { "slug", MCP_PARAM_STR, true,
      "Run label; lowercase alnum + '-', <= 64 chars",
      0, 0, 1, 64, NULL, NULL },
    { "src", MCP_PARAM_STR, false,
      "Source datadir to copy FROM; empty uses the script's own default "
      "(never a caller-chosen destination — the copy target is always "
      "chosen by the script)",
      0, 0, 0, 900, NULL, "\"\"" },
    { "args", MCP_PARAM_STR, false,
      "Space-separated extra node flags passed through to the copy's node",
      0, 0, 0, 2000, NULL, "\"\"" },
    { "expect_climb_past", MCP_PARAM_INT, false,
      "H* CLIMB gate height the copy must climb strictly past; -1 = unset",
      -1, 100000000, 0, 0, NULL, "-1" },
    { "deadline_secs", MCP_PARAM_INT, false,
      "How long the detached background run watches the tip before "
      "writing its final status; clamped to 1..3600",
      1, 3600, 0, 0, NULL, "180" },
    { "full", MCP_PARAM_BOOL, false,
      "Copy the whole datadir (blocks/ + snapshot) instead of the light "
      "cursor set",
      0, 0, 0, 0, NULL, "false" },
    { "no_run", MCP_PARAM_BOOL, false,
      "Snapshot + manifest only; do not launch the node",
      0, 0, 0, 0, NULL, "false" },
};
static const struct mcp_param_spec p_agent_test[] = {
    { "kind", MCP_PARAM_STR, true,
      "Test surface kind: \"test_group\" (build/bin/test_parallel --only=name) "
      "or \"scenario\" (build/bin/zclassic23-chaos --scenario=tools/sim/"
      "scenarios/name.scenario)",
      0, 0, 1, 20, "test_group,scenario", NULL },
    { "name", MCP_PARAM_STR, true,
      "Test group name (must be a compiled test_parallel group) or "
      "scenario basename (must exist under tools/sim/scenarios/); "
      "lowercase alnum + '_', <= 64 chars",
      0, 0, 1, 64, NULL, NULL },
};
static const struct mcp_param_spec p_agent_liveness[] = {
    { "mode", MCP_PARAM_STR, false,
      "Detail mode: brief/compact/summary returns bounded counts; full embeds availability methods, supervisor domains, and quality lanes",
      0, 0, 0, 16, "full,brief,compact,summary", "\"brief\"" },
};
static const struct mcp_param_spec p_agent_diagnose[] = {
    { "mode", MCP_PARAM_STR, false,
      "Detail mode: brief/compact/summary returns decision fields; full embeds drill-down payloads",
      0, 0, 0, 16, "full,brief,compact,summary", "\"brief\"" },
};
static const struct mcp_param_spec p_service_catalog[] = {
    { "name", MCP_PARAM_STR, false,
      "Optional service name to return one service contract",
      0, 0, 0, 64, NULL, "\"\"" },
};
static const struct mcp_param_spec p_service_operations[] = {
    { "operation_id", MCP_PARAM_STR, false,
      "Optional service.operation id to return one operation contract",
      0, 0, 0, 128, NULL, "\"\"" },
    { "service", MCP_PARAM_STR, false,
      "Optional service filter, for example bootstrap or znam_names",
      0, 0, 0, 64, NULL, "\"\"" },
    { "write_safety", MCP_PARAM_STR, false,
      "Optional safety filter",
      0, 0, 0, 40,
      "public_read_only,operator_private,operator_private_destructive",
      "\"\"" },
    { "preferred_interface", MCP_PARAM_STR, false,
      "Optional preferred interface filter",
      0, 0, 0, 32, "rest,mcp,rpc,native_or_planned", "\"\"" },
    { "status", MCP_PARAM_STR, false,
      "Optional operation status filter",
      0, 0, 0, 32, "active,in_progress", "\"\"" },
    { "surface", MCP_PARAM_STR, false,
      "Optional callable surface filter",
      0, 0, 0, 16, "rest,mcp,rpc", "\"\"" },
};
static const struct mcp_param_spec p_proof_bundle[] = {
    { "anchor_datadir", MCP_PARAM_STR, false,
      "Optional anchor mint datadir; defaults to ZCL_ANCHOR_MINT_DATADIR or ~/.zclassic-c23-anchor-mint",
      0, 0, 0, 512, NULL, "\"\"" },
};

static int h_zcl_service_catalog(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const char *name = json_get_str_or(req ? req->args : NULL, "name", "");
    if (!name || !name[0])
        return mcp_return_rpc_body(res, mcp_node_rpc("servicecatalog", NULL),
                                   "servicecatalog", "mcp.ops");

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, name);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("servicecatalog", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "servicecatalog", "mcp.ops",
                                   "name=%s", name);
}

static int h_zcl_service_operations(const struct mcp_request *req,
                                    struct mcp_response *res)
{
    const char *operation_id =
        json_get_str_or(req ? req->args : NULL, "operation_id", "");
    const char *service =
        json_get_str_or(req ? req->args : NULL, "service", "");
    const char *write_safety =
        json_get_str_or(req ? req->args : NULL, "write_safety", "");
    const char *preferred_interface =
        json_get_str_or(req ? req->args : NULL, "preferred_interface", "");
    const char *status =
        json_get_str_or(req ? req->args : NULL, "status", "");
    const char *surface =
        json_get_str_or(req ? req->args : NULL, "surface", "");
    bool has_filter =
        (service && service[0]) || (write_safety && write_safety[0]) ||
        (preferred_interface && preferred_interface[0]) ||
        (status && status[0]) || (surface && surface[0]);

    if (!operation_id || !operation_id[0]) {
        if (!has_filter)
            return mcp_return_rpc_body(
                res, mcp_node_rpc("serviceoperations", NULL),
                "serviceoperations", "mcp.ops");

        struct mcp_params p;
        mcp_params_init(&p);
        char kv[160];
#define PUSH_FILTER_KV(key_, value_) do { \
    if ((value_) && (value_)[0]) { \
        snprintf(kv, sizeof(kv), "%s=%s", key_, value_); \
        mcp_params_push_str(&p, kv); \
    } \
} while (0)
        PUSH_FILTER_KV("service", service);
        PUSH_FILTER_KV("write_safety", write_safety);
        PUSH_FILTER_KV("preferred_interface", preferred_interface);
        PUSH_FILTER_KV("status", status);
        PUSH_FILTER_KV("surface", surface);
#undef PUSH_FILTER_KV
        char *params = mcp_params_to_json(&p);
        char *out = params ? mcp_node_rpc("serviceoperations", params) : NULL;
        free(params);
        return mcp_return_rpc_body_ctx(res, out, "serviceoperations",
                                       "mcp.ops", "filtered=true");
    }

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, operation_id);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("serviceoperations", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "serviceoperations", "mcp.ops",
                                   "operation_id=%s", operation_id);
}

static int h_zcl_proof_bundle_mcp(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    const char *anchor_datadir =
        json_get_str_or(req ? req->args : NULL, "anchor_datadir", "");
    if (!anchor_datadir || !anchor_datadir[0])
        return mcp_return_rpc_body(res, mcp_node_rpc("proofbundle", NULL),
                                   "proofbundle", "mcp.ops");

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, anchor_datadir);
    char *params = mcp_params_to_json(&p);
    char *out = params ? mcp_node_rpc("proofbundle", params) : NULL;
    free(params);
    return mcp_return_rpc_body_ctx(res, out, "proofbundle", "mcp.ops",
                                   "anchor_datadir=%s", anchor_datadir);
}

struct agent_mcp_binding {
    const char *method;
    const struct mcp_param_spec *params;
    size_t num_params;
    mcp_handler_fn handler;
    uint32_t flags;
    const char *self_test_args;
};

static const struct agent_mcp_binding k_agent_mcp_bindings[] = {
    { "agent", NULL, 0, h_zcl_agent, 0, NULL },
    { "agentmap", NULL, 0, h_zcl_agent_map, 0, NULL },
    { "agentlanes", NULL, 0, h_zcl_agent_lanes, 0, NULL },
    { "agentliveness", p_agent_liveness, PARAM_COUNT(p_agent_liveness),
      h_zcl_agent_liveness, 0, "{\"mode\":\"brief\"}" },
    { "agentimpact", p_agent_impact, PARAM_COUNT(p_agent_impact),
      h_zcl_agent_impact, 0,
      "{\"files\":[\"app/controllers/src/event_controller.c\","
      "\"tools/mcp/controllers/ops_controller.c\"]}" },
    { "agentcontracts", NULL, 0, h_zcl_agent_contracts, 0, NULL },
    { "agentbuild", NULL, 0, h_zcl_agent_build, 0, NULL },
    { "agentdevstatus", NULL, 0, h_zcl_agent_dev_status, 0, NULL },
    { "proofbundle", p_proof_bundle, PARAM_COUNT(p_proof_bundle),
      h_zcl_proof_bundle_mcp, 0, "{\"anchor_datadir\":\"/tmp\"}" },
    { "agentinterface", NULL, 0, h_zcl_agent_interface, 0, NULL },
    { "agentops", NULL, 0, h_zcl_agent_ops, 0, NULL },
    { "appprotocols", NULL, 0, h_zcl_app_protocols, 0, NULL },
    { "servicecatalog", p_service_catalog, PARAM_COUNT(p_service_catalog),
      h_zcl_service_catalog, 0, "{\"name\":\"bootstrap\"}" },
    { "serviceoperations", p_service_operations,
      PARAM_COUNT(p_service_operations), h_zcl_service_operations, 0,
      "{\"operation_id\":\"bootstrap.read_bootstrap_status\"}" },
    { "agentdiagnose", p_agent_diagnose, PARAM_COUNT(p_agent_diagnose),
      h_zcl_agent_diagnose, 0, "{\"mode\":\"brief\"}" },
    { "agentdeployguard", p_agent_deploy_guard,
      PARAM_COUNT(p_agent_deploy_guard), h_zcl_agent_deploy_guard, 0,
      "{\"action\":\"canonical-deploy\"}" },
    { "agentcopyprove", p_agent_copy_prove, PARAM_COUNT(p_agent_copy_prove),
      h_zcl_agent_copy_prove,
      /* Spawns a node process + copies GBs of datadir; rate-gated like
       * every other tool that fires off real work (zcl_send,
       * zcl_invalidateblock, ...). self_test always skips destructive
       * tools (see mcp_router_dispatch guard above), so no example args
       * are needed here. */
      MCP_TOOL_FLAG_DESTRUCTIVE, NULL },
    { "agenttest", p_agent_test, PARAM_COUNT(p_agent_test),
      h_zcl_agent_test,
      /* Spawns build/bin/test_parallel or build/bin/zclassic23-chaos as a
       * real background process, same rate-gating rationale as
       * agentcopyprove above. self_test always skips destructive tools. */
      MCP_TOOL_FLAG_DESTRUCTIVE, NULL },
};

static struct mcp_tool_route
    g_agent_mcp_routes[PARAM_COUNT(k_agent_mcp_bindings)];

static void register_agent_mcp_routes(void)
{
    for (size_t i = 0; i < PARAM_COUNT(k_agent_mcp_bindings); i++) {
        const struct agent_mcp_binding *b = &k_agent_mcp_bindings[i];
        const struct agent_contract *c = agent_contract_lookup(b->method);
        if (!c || !c->mcp_tool || !c->purpose) {
            fprintf(stderr,
                    "[mcp.ops] FATAL: missing agent contract for method=%s\n",
                    b->method ? b->method : "(null)");
            abort();
        }
        g_agent_mcp_routes[i] = (struct mcp_tool_route) {
            c->mcp_tool,
            "ops",
            c->purpose,
            b->params,
            b->num_params,
            b->handler,
            b->flags,
            b->self_test_args,
        };
        mcp_router_register_required(&g_agent_mcp_routes[i]);
    }
}

static const struct mcp_param_spec p_postmortem_list[] = {
    { "dir", MCP_PARAM_STR, false, "Capsule directory",
      0, 0, 0, 512, NULL, NULL },
    { "limit", MCP_PARAM_INT, false, "Max capsules to return",
      1, 100, 0, 0, NULL, "20" },
};
static const struct mcp_param_spec p_postmortem_replay[] = {
    { "path", MCP_PARAM_STR, true, "Capsule path",
      0, 0, 1, 512, NULL, NULL },
    { "limit", MCP_PARAM_INT, false, "Max injected events to return",
      1, 1000, 0, 0, NULL, "100" },
};
static const struct mcp_param_spec p_rebuild_recent[] = {
    { "from_height", MCP_PARAM_INT, false,
      "Start height (default: active_tip - 10, floored at 0)",
      0, INT32_MAX, 0, 0, NULL, NULL },
};
static const struct mcp_tool_route k_routes[] = {
    { "zcl_status", "ops",
      "Composite target-node status: served H* height, peers, sync state, "
      "onion address, "
      "bg-validation progress, health checks, and chain advance source "
      "scoring. Target-owned fields are fetched over native RPC and carry "
      "execution-locus/source metadata. The single command to check if "
      "everything is working.",
      NULL, 0, h_zcl_status, 0, NULL },
    { "zcl_operator_summary", "ops",
      "Target-owned fail-closed operator summary projected by native "
      "operatorsnapshot. Older targets use an explicitly non-atomic, "
      "never-healthy multi-RPC compatibility path.",
      NULL, 0, h_zcl_operator_summary, 0, NULL },
    { "zcl_operator_snapshot", "ops",
      "Exact native zcl.operator_snapshot.v2 payload: bounded target-owned "
      "component snapshots, capture coherence, height/hash/work evidence, "
      "typed blockers, invariants, and the native summary projection.",
      NULL, 0, h_zcl_operator_snapshot, 0, NULL },
    { "zcl_milestone", "ops",
      "Node-computed ASCII and JSON progress toward the next version "
      "milestone, including systems/goals/subgoals bars and MVP criteria.",
      NULL, 0, h_zcl_milestone, 0, NULL },
    { "zcl_refold_status", "ops",
      "Self-verified UTXO anchor rebuild readiness: compiled checkpoint, "
      "candidate anchor snapshot path, full SHA3/count verification status, "
      "and next copy-proof action.",
      NULL, 0, h_zcl_refold_status, 0, NULL },
    { "zcl_health", "ops",
      "Health check: pass/fail, chain height, peers, sync, onion.",
      NULL, 0, h_zcl_health, 0, NULL },
    { "zcl_mirror_status", "ops",
      "Canonical zclassic23/zclassicd mirror lockstep status: both "
      "heights and hashes, lag, reachability, running state, and "
      "catch-up counters.",
      NULL, 0, h_zcl_mirror_status, 0, NULL },
    { "zcl_kpi", "ops",
      "One-shot KPI dashboard: height, peer_count, sync, validation, "
      "health, mempool, wallet, chain, network — every subsystem in "
      "one response. Invalid peer evidence yields null/known=false, not "
      "a synthetic count. The flagship operator tool for debugging.",
      NULL, 0, h_zcl_kpi, 0, NULL },
    { "zcl_self_heal_stats", "ops",
      "Self-heal UTXO recovery counters: tx-index hits, bounded scan "
      "hits/exhaustion, total scanned blocks, and active scan depth.",
      NULL, 0, h_zcl_self_heal_stats, 0, NULL },
    { "zcl_blockers", "ops",
      "Target-node typed blocker registry: active blockers by class "
      "{permanent,transient,dependency,resource}, deadlines, escape "
      "actions, fire counts, retry budgets. Preserves the "
      "`zcl_state subsystem=blocker`.state fields and adds target "
      "provenance plus a derived dominant entry. PERMANENT>0 is always "
      "an operator escalation event — typed-PERMANENT means we will not "
      "auto-retry.",
      NULL, 0, h_zcl_blockers, 0, NULL },
    { "zcl_postmortem_list", "ops",
      "List recent postmortem crash capsules: path, crash time, signal, "
      "capsule bytes, and embedded seed-tape bytes. Defaults to the "
      "operator postmortem directory.",
      p_postmortem_list, PARAM_COUNT(p_postmortem_list),
      h_zcl_postmortem_list, 0, NULL },
    { "zcl_postmortem_replay", "ops",
      "Load a postmortem crash capsule and return the recorded injected "
      "seed-tape events as bounded replay metadata with hex payloads.",
      p_postmortem_replay, PARAM_COUNT(p_postmortem_replay),
      h_zcl_postmortem_replay, 0, NULL },
    { "zcl_getmempoolinfo", "ops",
      "Mempool size, bytes, usage.",
      NULL, 0, h_zcl_getmempoolinfo, 0, NULL },
    { "zcl_mempool_inspect", "ops",
      "Mempool fee-rate (zat/byte) and age histograms. Power-user "
      "signal for transaction fee construction and congestion diagnosis.",
      NULL, 0, h_zcl_mempool_inspect, 0, NULL },
    { "zcl_getrawmempool", "ops",
      "Array of txids currently in the mempool.",
      NULL, 0, h_zcl_getrawmempool, 0, NULL },
    { "zcl_getmininginfo", "ops",
      "Mining stats: hashrate, difficulty, current block, pooled tx.",
      NULL, 0, h_zcl_getmininginfo, 0, NULL },
    { "zcl_benchmark", "ops",
      "Hash / malloc / hash160 throughput (sha256d, malloc-4K, hash160 "
      "ops/sec).",
      NULL, 0, h_zcl_benchmark, 0, NULL },
    { "zcl_dbstats", "ops",
      "Database health: table counts, SQLite page stats, sizes.",
      NULL, 0, h_zcl_dbstats, 0, NULL },
    { "zcl_filemanifest", "ops",
      "File service status: chunks, SHA3 hashes, total size.",
      NULL, 0, h_zcl_filemanifest, 0, NULL },
    { "zcl_events", "ops",
      "Recent event log: sync events, peer connections, blocks.",
      p_events, PARAM_COUNT(p_events), h_zcl_events, 0, NULL },
    { "zcl_timeline", "ops",
      "Versioned semantic event timeline by category with seq cursors; "
      "supports bounded server-side filters; prefer this over client-side "
      "jq/string filtering of zcl_events.",
      p_timeline, PARAM_COUNT(p_timeline), h_zcl_timeline, 0,
      "{\"category\":\"sync\",\"count\":50,\"since_secs\":3600}" },
    { "zcl_rpc", "ops",
      "Call any RPC method directly. 85+ commands available.",
      p_rpc, PARAM_COUNT(p_rpc), h_zcl_rpc,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE /* arbitrary RPC — skip in self_test */ },
    { "zcl_syncdiag", "ops",
      "Deep sync diagnostics: sync state, chain height, best header "
      "height, peer max height, header gap, watchdog status and "
      "escalation level, header batch counters, download queue size "
      "and in-flight count. The single tool for diagnosing sync stalls.",
      NULL, 0, h_zcl_syncdiag, 0, NULL },
    { "zcl_rebuild_recent", "ops",
      "Bounded recovery: fetch the recent block range from the legacy "
      "advisory source and connect each block through local consensus "
      "validation, reorging off any stale local fork. "
      "NOT a reindex — does not wipe the UTXO set, does not replay from "
      "genesis, does not bypass validation; the range is capped. "
      "Idempotent: a no-op when already at tip. Destructive because it "
      "mutates the live chainstate.",
      p_rebuild_recent, PARAM_COUNT(p_rebuild_recent),
      h_zcl_rebuild_recent,
      .flags = MCP_TOOL_FLAG_DESTRUCTIVE },
};

void mcp_register_ops(void)
{
    register_agent_mcp_routes();
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register_required(&k_routes[i]);
}
