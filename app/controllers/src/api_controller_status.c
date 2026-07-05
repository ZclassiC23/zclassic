/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Compact public REST status endpoint for dashboards, website checks, and
 * MCP-friendly clients that do not need the full diagnostic tree. */

#include "controllers/api_controller.h"
#include "controllers/agent_controller.h"
#include "api_controller_internal.h"
#include "config/runtime.h"
#include "json/json.h"
#include "jobs/reducer_frontier.h"
#include "jobs/tip_finalize_stage.h"
#include "net/download.h"
#include "services/anchor_selfmint.h"
#include "services/node_health_service.h"
#include "storage/progress_store.h"
#include "sync/sync_state.h"
#include "util/clientversion.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct milestone_criterion {
    int id;
    const char *key;
    const char *title;
    const char *status;
    bool strict_pass;
    int units;
    const char *next;
};

static const struct milestone_criterion k_mvp_criteria[] = {
    { 1, "single_binary_install",
      "Single-binary install on clean Ubuntu/Debian",
      "pass", true, 2, "keep ci-install-linger green" },
    { 2, "tor_onion_bootstrap",
      "Tor onion bootstrap in <60s",
      "pass", true, 2, "keep mvp-onion-local green" },
    { 3, "cold_start_sync",
      "Cold-start sync to tip in <10 min",
      "partial", false, 1, "seed coins frontier on snapshot cold path" },
    { 4, "shielded_receive",
      "Receive shielded payment end-to-end",
      "pass", true, 2, "keep shielded payment proof green" },
    { 5, "store_flow",
      "List and sell file via store",
      "partial", false, 1, "prove live buyer over onion/file transfer" },
    { 6, "seven_day_soak",
      "7-day soak with zero operator intervention",
      "partial", false, 1, "complete clean 168h soak window" },
    { 7, "kill9_recovery",
      "Recover from kill -9 in <2 min",
      "pass", true, 2, "keep crash bootstrap and peer-tip proofs green" },
    { 8, "consensus_parity",
      "Consensus parity with zclassicd",
      "partial", false, 1, "prove exact parity over the soak window" },
};

static void api_ascii_bar(char out[13], int done, int total)
{
    int filled = 0;
    if (total > 0)
        filled = (done * 10 + total / 2) / total;
    if (filled < 0)
        filled = 0;
    if (filled > 10)
        filled = 10;

    out[0] = '[';
    for (int i = 0; i < 10; i++)
        out[i + 1] = i < filled ? '#' : '-';
    out[11] = ']';
    out[12] = '\0';
}

static void api_push_progress_bar(struct json_value *bars,
                                  struct json_value *ascii,
                                  const char *name,
                                  int done,
                                  int total,
                                  const char *summary)
{
    char bar[13];
    char line[192];
    int percent = total > 0 ? (done * 100) / total : 0;
    api_ascii_bar(bar, done, total);
    snprintf(line, sizeof(line), "%s %s %d/%d %s",
             name, bar, done, total, summary ? summary : "");

    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "bar", bar);
    json_push_kv_str(&obj, "line", line);
    json_push_kv_int(&obj, "done", done);
    json_push_kv_int(&obj, "total", total);
    json_push_kv_int(&obj, "percent", percent);
    json_push_kv_str(&obj, "summary", summary ? summary : "");
    json_push_kv(bars, name, &obj);
    json_push_kv_str(ascii, name, line);
    json_free(&obj);
}

int64_t api_served_tip_height(void)
{
    if (reducer_frontier_provable_tip_is_published()) {
        int served = reducer_frontier_provable_tip_cached();
        if (served >= 0)
            return served;
    }

    sqlite3 *db = progress_store_db();
    int durable_height = 0;
    uint8_t durable_hash[32];
    if (db && tip_finalize_stage_resolve_durable_tip(
            db, &durable_height, durable_hash) && durable_height >= 0)
        return durable_height;

    return 0;
}

void api_milestone_status_json(struct json_value *result)
{
    struct node_health_snapshot health = {0};
    node_health_collect(&health, g_api_ctx.node_db ?
        g_api_ctx.node_db : app_runtime_node_db(),
        g_api_ctx.main_state);

    int served_height = (int)api_served_tip_height();
    int indexed_height = health.tip_height;
    int target = indexed_height > served_height ? indexed_height
                                                : served_height;
    if (health.header_height > target)
        target = health.header_height;
    if (health.peer_best_height > target)
        target = health.peer_best_height;
    int gap = target > served_height ? target - served_height : 0;

    int systems_done = 0;
    int systems_total = 6;
    bool onion_ok = health.tor_enabled && health.tor_ready &&
                    health.onion_service_ready;
    if (health.serving)
        systems_done++;
    if (health.healthy)
        systems_done++;
    if (served_height > 0)
        systems_done++;
    if (gap <= 1)
        systems_done++;
    if (health.has_peers)
        systems_done++;
    if (onion_ok)
        systems_done++;

    int strict_pass = 0;
    int partial_units = 0;
    int max_units = 0;
    size_t criteria_count = sizeof(k_mvp_criteria) / sizeof(k_mvp_criteria[0]);
    for (size_t i = 0; i < criteria_count; i++) {
        if (k_mvp_criteria[i].strict_pass)
            strict_pass++;
        partial_units += k_mvp_criteria[i].units;
        max_units += 2;
    }

    json_set_object(result);
    json_push_kv_str(result, "schema", ZCL_MILESTONE_STATUS_SCHEMA);
    json_push_kv_str(result, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(result, "milestone", "v1 MVP");
    json_push_kv_str(result, "source", "zclassic23");
    json_push_kv_str(result, "rule",
                     "MVP is achieved only when MRS reaches 8/8");
    json_push_kv_bool(result, "complete",
                      strict_pass == (int)criteria_count);
    json_push_kv_int(result, "mvp_readiness_score", strict_pass);
    json_push_kv_int(result, "target_score", (int64_t)criteria_count);
    json_push_kv_int(result, "partial_progress_units", partial_units);
    json_push_kv_int(result, "partial_progress_units_total", max_units);

    struct json_value live;
    json_init(&live);
    json_set_object(&live);
    json_push_kv_bool(&live, "healthy", health.healthy);
    json_push_kv_bool(&live, "serving", health.serving);
    json_push_kv_int(&live, "served_height", served_height);
    json_push_kv_int(&live, "indexed_height", indexed_height);
    json_push_kv_int(&live, "header_height", health.header_height);
    json_push_kv_int(&live, "peer_best_height", health.peer_best_height);
    json_push_kv_int(&live, "target_height", target);
    json_push_kv_int(&live, "gap", gap);
    json_push_kv_int(&live, "peers", (int64_t)health.peer_count);
    json_push_kv_bool(&live, "tor_enabled", health.tor_enabled);
    json_push_kv_bool(&live, "onion_ready", onion_ok);
    json_push_kv_str(&live, "sync_state",
                     sync_state_name(health.sync_state));
    json_push_kv(result, "live", &live);
    json_free(&live);

    struct json_value bars;
    struct json_value ascii;
    json_init(&bars);
    json_init(&ascii);
    json_set_object(&bars);
    json_set_object(&ascii);
    api_push_progress_bar(&bars, &ascii, "systems", systems_done,
                          systems_total, "live node runtime checks");
    api_push_progress_bar(&bars, &ascii, "goals", strict_pass,
                          (int)criteria_count, "strict MVP MRS");
    api_push_progress_bar(&bars, &ascii, "subgoals", partial_units,
                          max_units, "full plus partial proof units");
    json_push_kv(result, "bars", &bars);
    json_push_kv(result, "ascii", &ascii);
    json_free(&bars);
    json_free(&ascii);

    struct json_value criteria;
    json_init(&criteria);
    json_set_array(&criteria);
    for (size_t i = 0; i < criteria_count; i++) {
        const struct milestone_criterion *c = &k_mvp_criteria[i];
        struct json_value item;
        json_init(&item);
        json_set_object(&item);
        json_push_kv_int(&item, "id", c->id);
        json_push_kv_str(&item, "key", c->key);
        json_push_kv_str(&item, "title", c->title);
        json_push_kv_str(&item, "status", c->status);
        json_push_kv_bool(&item, "strict_pass", c->strict_pass);
        json_push_kv_int(&item, "progress_units", c->units);
        json_push_kv_int(&item, "progress_units_total", 2);
        json_push_kv_str(&item, "next", c->next);
        json_push_back(&criteria, &item);
        json_free(&item);
    }
    json_push_kv(result, "criteria", &criteria);
    json_free(&criteria);

    struct json_value next;
    json_init(&next);
    json_set_array(&next);
    for (size_t i = 0; i < criteria_count; i++) {
        if (k_mvp_criteria[i].strict_pass)
            continue;
        struct json_value s;
        json_init(&s);
        json_set_str(&s, k_mvp_criteria[i].next);
        json_push_back(&next, &s);
        json_free(&s);
    }
    json_push_kv(result, "next", &next);
    json_free(&next);
}

size_t api_serve_milestone(uint8_t *response, size_t response_max)
{
    if (!response || response_max == 0)
        return 0;

    struct json_value body;
    json_init(&body);
    api_milestone_status_json(&body);
    api_json_add_freshness(&body, "operator_status", -1);
    size_t n = api_json_ok(response, response_max, &body);
    json_free(&body);
    return n;
}

void api_refold_status_json(struct json_value *result)
{
    struct anchor_snapshot_status st = {0};
    bool status_ok = anchor_selfmint_snapshot_status(g_api_ctx.datadir, &st);

    json_set_object(result);
    json_push_kv_str(result, "schema", ZCL_REFOLD_STATUS_SCHEMA);
    json_push_kv_str(result, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(result, "source", "zclassic23");
    json_push_kv_str(result, "purpose",
                     "sovereign refold anchor readiness");
    json_push_kv_bool(result, "ready_for_refold",
                      status_ok && st.verified);
    json_push_kv_str(result, "primary_blocker",
                     status_ok && st.verified
                         ? "none"
                         : "missing_verified_anchor_snapshot");
    json_push_kv_str(result, "next_action",
                     status_ok ? st.next_action
                               : "check anchor snapshot status internals");

    struct json_value checkpoint;
    json_init(&checkpoint);
    json_set_object(&checkpoint);
    json_push_kv_bool(&checkpoint, "present", status_ok &&
                      st.checkpoint_present);
    json_push_kv_int(&checkpoint, "height", st.checkpoint_height);
    json_push_kv_int(&checkpoint, "utxo_count",
                     (int64_t)st.checkpoint_utxo_count);
    json_push_kv_int(&checkpoint, "total_supply",
                     st.checkpoint_total_supply);
    json_push_kv_str(&checkpoint, "sha3", st.checkpoint_sha3_hex);
    json_push_kv_str(&checkpoint, "block_hash",
                     st.checkpoint_block_hash_hex);
    json_push_kv(result, "checkpoint", &checkpoint);
    json_free(&checkpoint);

    struct json_value snap;
    json_init(&snap);
    json_set_object(&snap);
    json_push_kv_bool(&snap, "path_resolved", status_ok &&
                      st.path_resolved);
    json_push_kv_str(&snap, "source", status_ok ? st.path_source : "");
    json_push_kv_str(&snap, "path", status_ok ? st.path : "");
    json_push_kv_bool(&snap, "stat_present", status_ok &&
                      st.stat_present);
    json_push_kv_int(&snap, "stat_size",
                     status_ok ? st.stat_size : 0);
    json_push_kv_bool(&snap, "header_read", status_ok && st.header_read);
    json_push_kv_int(&snap, "height", st.snapshot_height);
    json_push_kv_int(&snap, "count", (int64_t)st.snapshot_count);
    json_push_kv_int(&snap, "total_supply",
                     st.snapshot_total_supply);
    json_push_kv_str(&snap, "sha3", st.snapshot_sha3_hex);
    json_push_kv_str(&snap, "block_hash", st.snapshot_block_hash_hex);
    json_push_kv_bool(&snap, "height_match", status_ok &&
                      st.height_match);
    json_push_kv_bool(&snap, "count_match", status_ok &&
                      st.count_match);
    json_push_kv_bool(&snap, "sha3_match", status_ok && st.sha3_match);
    json_push_kv_bool(&snap, "block_hash_match", status_ok &&
                      st.block_hash_match);
    json_push_kv_bool(&snap, "verified", status_ok && st.verified);
    json_push_kv_str(&snap, "verification",
                     status_ok ? st.verification : "status_error");
    if (!status_ok || st.error[0])
        json_push_kv_str(&snap, "error", status_ok ? st.error
                                                    : "status unavailable");
    json_push_kv(result, "anchor_snapshot", &snap);
    json_free(&snap);

    struct json_value commands;
    json_init(&commands);
    json_set_object(&commands);
    json_push_kv_str(&commands, "native", "zclassic23 refold");
    json_push_kv_str(&commands, "rest", "/api/v1/refold");
    json_push_kv_str(&commands, "mcp", "zcl_refold_status");
    json_push_kv_str(&commands, "copy_proof",
                     "make repro-on-copy SLUG=soak-refold "
                     "REPRO_SRC=$HOME/.zclassic-c23-soak REPRO_FULL=1 "
                     "CLIMB_PAST=<checkpoint_height> "
                     "ARGS='-refold-from-anchor -nobgvalidation "
                     "-paramsdir=$$HOME/.zcash-params'");
    json_push_kv(result, "commands", &commands);
    json_free(&commands);
}

size_t api_serve_refold_status(uint8_t *response, size_t response_max)
{
    if (!response || response_max == 0)
        return 0;

    struct json_value body;
    json_init(&body);
    api_refold_status_json(&body);
    api_json_add_freshness(&body, "anchor_snapshot", -1);
    size_t n = api_json_ok(response, response_max, &body);
    json_free(&body);
    return n;
}

size_t api_serve_unsupported_version(const char *requested_version,
                                     uint8_t *response,
                                     size_t response_max)
{
    const char *requested = requested_version ? requested_version : "unknown";

    struct json_value body;
    json_init(&body);
    json_set_object(&body);
    json_push_kv_str(&body, "schema", ZCL_REST_ERROR_SCHEMA);
    json_push_kv_str(&body, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(&body, "error", "unsupported_api_version");
    json_push_kv_str(&body, "requested_version", requested);

    struct json_value supported;
    json_init(&supported);
    json_set_array(&supported);
    struct json_value version;
    json_init(&version);
    json_set_str(&version, ZCL_REST_API_VERSION);
    json_push_back(&supported, &version);
    json_free(&version);
    json_push_kv(&body, "supported_versions", &supported);
    json_free(&supported);

    json_push_kv_str(&body, "base_path", ZCL_REST_API_BASE_PATH);
    json_push_kv_str(&body, "index", ZCL_REST_API_BASE_PATH);

    size_t n = api_json_status(response, response_max, "400 Bad Request",
                               &body);
    json_free(&body);
    return n;
}

/* Route: /api/status and /api/node/summary */
size_t api_serve_node_summary(uint8_t *response, size_t response_max)
{
    struct node_health_snapshot health = {0};
    node_health_collect(&health, g_api_ctx.node_db ?
        g_api_ctx.node_db : app_runtime_node_db(),
        g_api_ctx.main_state);

    struct download_manager *dm = msg_get_download_mgr();
    uint64_t dl_req = 0, dl_recv = 0, dl_tout = 0;
    uint64_t dl_inflight = 0, dl_queued = 0;
    dl_get_stats(dm, &dl_req, &dl_recv, &dl_tout, &dl_inflight, &dl_queued);

    struct api_freshness_meta freshness;
    api_freshness_prepare(&freshness, "served_tip", health.tip_height);
    int64_t height = freshness.served_height;
    int64_t indexed_height = freshness.indexed_height;
    int64_t target = indexed_height > height ? indexed_height : height;
    if (health.header_height > target)
        target = health.header_height;
    if (health.peer_best_height > target)
        target = health.peer_best_height;
    int64_t gap = target > height ? target - height : 0;
    int64_t index_gap = indexed_height > height ? indexed_height - height : 0;

    const char *status = "healthy";
    const char *primary = "none";
    const char *next_endpoint = "/api/v1/agent";
    bool material_gap = gap > 1;

    const char *summary = "node healthy at served frontier";
    bool operator_needed = false;

    if (!health.serving) {
        status = "blocked";
        primary = health.blocking_reason[0] ? health.blocking_reason
                                            : "not_serving";
        next_endpoint = "/api/v1/health";
        summary = health.blocking_reason[0]
            ? "node has an active health blocker"
            : "node is not serving";
        operator_needed = true;
    } else if (!health.has_peers) {
        status = "blocked";
        primary = "no_peers";
        next_endpoint = "/api/v1/peers";
        summary = "node has no connected peers";
        operator_needed = true;
    } else if (material_gap && (dl_inflight > 0 || dl_queued > 0)) {
        status = "catching_up";
        primary = "chain_gap";
        next_endpoint = "/api/v1/downloadstats";
        summary = "node is downloading blocks toward the best known tip";
    } else if (material_gap) {
        status = "degraded";
        primary = "download_queue_idle";
        next_endpoint = "/api/v1/downloadstats";
        summary = "node is behind the best known tip without active downloads";
        operator_needed = true;
    } else if (!health.healthy) {
        status = "degraded";
        primary = "healthcheck_unhealthy";
        next_endpoint = "/api/v1/health";
        summary = "node health checks are degraded";
        operator_needed = health.warning_count > 0;
    }

    struct json_value body;
    json_init(&body);
    json_set_object(&body);
    json_push_kv_str(&body, "schema", ZCL_PUBLIC_STATUS_SCHEMA);
    json_push_kv_str(&body, "api_version", ZCL_REST_API_VERSION);
    json_push_kv_str(&body, "build_commit", zcl_build_commit());
    json_push_kv_str(&body, "status", status);
    json_push_kv_bool(&body, "healthy", health.healthy);
    json_push_kv_bool(&body, "serving", health.serving);
    json_push_kv_bool(&body, "operator_needed", operator_needed);
    json_push_kv_bool(&body, "operator_latch_recovered",
                      health.operator_latch_recovered);
    json_push_kv_str(&body, "summary", summary);
    json_push_kv_str(&body, "primary_blocker", primary);
    json_push_kv_str(&body, "next_endpoint", next_endpoint);
    agent_push_operator_lane_json(&body, "operator_lane");
    json_push_kv_int(&body, "height", height);
    api_freshness_push_json(&body, &freshness);
    json_push_kv_int(&body, "header_height", health.header_height);
    json_push_kv_int(&body, "peer_best_height", health.peer_best_height);
    json_push_kv_int(&body, "target_height", target);
    json_push_kv_int(&body, "gap", gap);
    json_push_kv_int(&body, "index_gap", index_gap);
    json_push_kv_str(&body, "sync_state",
                     sync_state_name(health.sync_state));

    struct json_value peers;
    json_init(&peers);
    json_set_object(&peers);
    json_push_kv_int(&peers, "total", (int64_t)health.peer_count);
    json_push_kv_bool(&peers, "has_peers", health.has_peers);
    json_push_kv_int(&peers, "magicbean",
                     (int64_t)health.magicbean_peer_count);
    json_push_kv_int(&peers, "zclassic23",
                     (int64_t)health.zclassic_c23_peer_count);
    json_push_kv(&body, "peers", &peers);
    json_free(&peers);

    struct json_value download;
    json_init(&download);
    json_set_object(&download);
    json_push_kv_int(&download, "requested", (int64_t)dl_req);
    json_push_kv_int(&download, "received", (int64_t)dl_recv);
    json_push_kv_int(&download, "timed_out", (int64_t)dl_tout);
    json_push_kv_int(&download, "in_flight", (int64_t)dl_inflight);
    json_push_kv_int(&download, "queued", (int64_t)dl_queued);
    json_push_kv(&body, "download", &download);
    json_free(&download);

    struct json_value services;
    json_init(&services);
    json_set_object(&services);
    json_push_kv_bool(&services, "tor_enabled", health.tor_enabled);
    json_push_kv_bool(&services, "tor_ready", health.tor_ready);
    json_push_kv_bool(&services, "onion_service_ready",
                      health.onion_service_ready);
    json_push_kv(&body, "services", &services);
    json_free(&services);

    static const char *recommended[] = {
        "/api/v1/agent",
        "/api/v1/health",
        "/api/v1/node/status",
        "/api/v1/hodl",
        "/api/v1/factoids",
    };
    struct json_value endpoints;
    json_init(&endpoints);
    json_set_array(&endpoints);
    for (size_t i = 0; i < sizeof(recommended) / sizeof(recommended[0]); i++) {
        struct json_value item;
        json_init(&item);
        json_set_str(&item, recommended[i]);
        json_push_back(&endpoints, &item);
        json_free(&item);
    }
    json_push_kv(&body, "recommended_endpoints", &endpoints);
    json_free(&endpoints);

    size_t n = api_json_ok(response, response_max, &body);
    json_free(&body);
    return n;
}
