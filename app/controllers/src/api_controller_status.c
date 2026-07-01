/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Compact public REST status endpoint for dashboards, website checks, and
 * MCP-friendly clients that do not need the full diagnostic tree. */

#include "controllers/api_controller.h"
#include "api_controller_internal.h"
#include "config/runtime.h"
#include "jobs/reducer_frontier.h"
#include "net/download.h"
#include "services/node_health_service.h"
#include "sync/sync_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int api_status_served_height(void)
{
    if (!reducer_frontier_provable_tip_is_published())
        return 0;

    int served = reducer_frontier_provable_tip_cached();
    return served >= 0 ? served : 0;
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

    int indexed_height = health.tip_height;
    int height = api_status_served_height();
    int target = indexed_height > height ? indexed_height : height;
    if (health.header_height > target)
        target = health.header_height;
    if (health.peer_best_height > target)
        target = health.peer_best_height;
    int gap = target > height ? target - height : 0;
    int index_gap = indexed_height > height ? indexed_height - height : 0;

    const char *status = "healthy";
    const char *primary = "none";
    const char *next_endpoint = "/api/status";
    const char *summary = "node healthy at current tip";
    bool operator_needed = false;

    if (!health.serving) {
        status = "blocked";
        primary = "not_serving";
        next_endpoint = "/api/health";
        summary = "node is not serving";
        operator_needed = true;
    } else if (!health.has_peers) {
        status = "blocked";
        primary = "no_peers";
        next_endpoint = "/api/peers";
        summary = "node has no connected peers";
        operator_needed = true;
    } else if (gap > 0 && (dl_inflight > 0 || dl_queued > 0)) {
        status = "catching_up";
        primary = "chain_gap";
        next_endpoint = "/api/downloadstats";
        summary = "node is downloading blocks toward the best known tip";
    } else if (gap > 0) {
        status = "degraded";
        primary = "download_queue_idle";
        next_endpoint = "/api/downloadstats";
        summary = "node is behind the best known tip without active downloads";
        operator_needed = true;
    } else if (!health.healthy) {
        status = "degraded";
        primary = "healthcheck_unhealthy";
        next_endpoint = "/api/health";
        summary = "node health checks are degraded";
        operator_needed = health.warning_count > 0;
    }

    char body[4096];
    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"schema\":\"zcl.public_status.v1\","
        "\"status\":\"%s\","
        "\"healthy\":%s,"
        "\"serving\":%s,"
        "\"operator_needed\":%s,"
        "\"summary\":\"%s\","
        "\"primary_blocker\":\"%s\","
        "\"next_endpoint\":\"%s\","
        "\"height\":%d,"
        "\"served_height\":%d,"
        "\"indexed_height\":%d,"
        "\"header_height\":%d,"
        "\"peer_best_height\":%d,"
        "\"target_height\":%d,"
        "\"gap\":%d,"
        "\"index_gap\":%d,"
        "\"sync_state\":\"%s\","
        "\"peers\":{"
          "\"total\":%zu,"
          "\"has_peers\":%s,"
          "\"magicbean\":%zu,"
          "\"zclassic23\":%zu"
        "},"
        "\"download\":{"
          "\"requested\":%llu,"
          "\"received\":%llu,"
          "\"timed_out\":%llu,"
          "\"in_flight\":%llu,"
          "\"queued\":%llu"
        "},"
        "\"services\":{"
          "\"tor_enabled\":%s,"
          "\"tor_ready\":%s,"
          "\"onion_service_ready\":%s"
        "},"
        "\"recommended_endpoints\":["
          "\"/api/status\","
          "\"/api/health\","
          "\"/api/node/status\","
          "\"/api/hodl\","
          "\"/api/factoids\""
        "]"
        "}",
        status,
        health.healthy ? "true" : "false",
        health.serving ? "true" : "false",
        operator_needed ? "true" : "false",
        summary,
        primary,
        next_endpoint,
        height,
        height,
        indexed_height,
        health.header_height,
        health.peer_best_height,
        target,
        gap,
        index_gap,
        sync_state_name(health.sync_state),
        health.peer_count,
        health.has_peers ? "true" : "false",
        health.magicbean_peer_count,
        health.zclassic_c23_peer_count,
        (unsigned long long)dl_req,
        (unsigned long long)dl_recv,
        (unsigned long long)dl_tout,
        (unsigned long long)dl_inflight,
        (unsigned long long)dl_queued,
        health.tor_enabled ? "true" : "false",
        health.tor_ready ? "true" : "false",
        health.onion_service_ready ? "true" : "false");
    if (body_len < 0 || body_len >= (int)sizeof(body))
        return api_json_error(response, response_max, JSON_500_HEADERS,
                              "Status response too large");

    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n\r\n"
        "%s",
        body_len, body);
}
