/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Node / diagnostics route handlers for the REST API controller.
 *
 * These functions own node, diagnostics, event-log, and explorer factoid
 * response helpers for the route table in api_controller.c. They read
 * g_api_ctx, lock-free atomics, and node state, then write directly into the
 * caller's response buffer. */

#include "platform/time_compat.h"
#include "controllers/api_controller.h"
#include "controllers/blockchain_controller.h"
#include "controllers/file_controller.h"
#include "api_controller_internal.h"
#include "chain/mmb.h"
#include "config/boot.h"
#include "config/runtime.h"
#include "encoding/utilstrencodings.h"
#include "event/event.h"
#include "jobs/reducer_frontier.h"
#include "sync/sync_state.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/block.h"
#include "models/file_service.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "net/download.h"
#include "sapling/incremental_merkle_tree.h"
#include "services/node_health_service.h"
#include "net/snapshot_sync_contract.h"
#include "validation/contextual_check_tx.h"
#include "validation/main_state.h"
#include "views/explorer_factoids_view.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/log_macros.h"
#include "util/safe_alloc.h"

static struct snapshot_sync_service *api_snapshot_sync(bool *initialized)
{
    struct snapshot_sync_service *svc = app_runtime_snapshot_sync();

    if (svc) {
        if (initialized)
            *initialized = true;
        return svc;
    }
    if (initialized)
        *initialized = snapsync_global_initialized();
    return snapsync_global_initialized() ? snapsync_global() : NULL;
}

static bool api_json_quote_or_null(char *out, size_t out_sz, const char *src)
{
    static const char hex[] = "0123456789abcdef";
    if (!out || out_sz == 0)
        return false;
    if (!src)
        return (size_t)snprintf(out, out_sz, "null") < out_sz;

    size_t w = 0;
    if (w + 1 >= out_sz)
        return false;
    out[w++] = '"';
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default: break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (w + n >= out_sz)
                return false;
            memcpy(out + w, esc, n);
            w += n;
        } else if (c < 0x20) {
            if (w + 6 >= out_sz)
                return false;
            out[w++] = '\\';
            out[w++] = 'u';
            out[w++] = '0';
            out[w++] = '0';
            out[w++] = hex[(c >> 4) & 0x0f];
            out[w++] = hex[c & 0x0f];
        } else {
            if (w + 1 >= out_sz)
                return false;
            out[w++] = (char)c;
        }
    }
    if (w + 1 >= out_sz)
        return false;
    out[w++] = '"';
    out[w] = '\0';
    return true;
}

/* Event log — lock-free atomic reads, safe from any handler thread */
size_t api_serve_events(const char *path, uint8_t *response,
                        size_t response_max)
{
    size_t count = 200;
    const char *q = strchr(path, '?');
    if (q) {
        const char *cp = strstr(q, "count=");
        if (cp) {
            long v = strtol(cp + 6, NULL, 10);
            if (v > 0 && v <= 65536) count = (size_t)v;
        }
    }
    /* Build JSON body: {"sync_state":"...","events":[...]} */
    char *buf = zcl_malloc(524288, "api_events_buf");
    if (!buf)
        return api_json_error(response, response_max, JSON_500_HEADERS,
                          "Out of memory");
    size_t w = 0;
    w += (size_t)snprintf(buf + w, 524288 - w,
        "{\"sync_state\":\"%s\",\"events\":",
        sync_state_name(sync_get_state()));
    /* Parse ?type= filter from query string */
    const char *type_filter = NULL;
    if (q) {
        const char *tp = strstr(q, "type=");
        if (tp) {
            static char type_buf[64];
            size_t tlen = 0;
            for (const char *c = tp + 5; *c && *c != '&' && tlen < 63; c++)
                type_buf[tlen++] = *c;
            type_buf[tlen] = '\0';
            type_filter = type_buf;
        }
    }
    if (type_filter)
        w += event_dump_json_filtered(buf + w, 524288 - w, count, type_filter);
    else
        w += event_dump_json(buf + w, 524288 - w, count);
    if (w + 1 < 524288) buf[w++] = '}';

    size_t off = (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n\r\n", w);
    if (w > 524288) w = 524288;  /* cap to buf size */
    if (off + w <= response_max)
        memcpy(response + off, buf, w);
    else if (off < response_max) {
        size_t avail = response_max - off;
        if (avail > w) avail = w;
        memcpy(response + off, buf, avail);
    }
    free(buf);
    return off + w < response_max ? off + w : response_max;
}

/* Sync state — minimal monitoring endpoint */
size_t api_serve_syncstate(uint8_t *response, size_t response_max)
{
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"sync_state\":\"%s\","
        "\"utxo_replay_active\":%s,"
        "\"utxo_replay_height\":%d}",
        sync_state_name(sync_get_state()),
        atomic_load(&g_utxo_replay_active) ? "true" : "false",
        atomic_load(&g_utxo_replay_height));
}

/* Download stats — IBD progress monitoring */
size_t api_serve_downloadstats(uint8_t *response, size_t response_max)
{
    struct download_manager *dm = msg_get_download_mgr();
    uint64_t req = 0, recv = 0, tout = 0, inflight = 0, queued = 0;
    dl_get_stats(dm, &req, &recv, &tout, &inflight, &queued);
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"sync_state\":\"%s\","
        "\"requested\":%llu,\"received\":%llu,"
        "\"timed_out\":%llu,\"in_flight\":%llu,"
        "\"queued\":%llu,\"defer_proof_validation_below_height\":%d}",
        sync_state_name(sync_get_state()),
        (unsigned long long)req, (unsigned long long)recv,
        (unsigned long long)tout, (unsigned long long)inflight,
        (unsigned long long)queued, g_deferred_proof_validation_below_height);
}

/* Health check — lightweight, machine-readable */
size_t api_serve_health(uint8_t *response, size_t response_max)
{
    struct node_health_snapshot health = {0};
    node_health_collect(&health, g_api_ctx.node_db ?
        g_api_ctx.node_db : app_runtime_node_db(),
        g_api_ctx.main_state);

    char onion_json[1024];
    char last_error_json[8192];
    char last_error_type_json[512];
    char wd_name_json[512];
    char wd_trigger_json[1024];
    char wd_reason_json[1024];
    char degraded_json[1024];
    char blocking_json[1024];
    char warnings_json[1024];
    if (!api_json_quote_or_null(onion_json, sizeof(onion_json),
                                health.onion_address[0]
                                    ? health.onion_address : NULL) ||
        !api_json_quote_or_null(last_error_json, sizeof(last_error_json),
                                health.last_error[0]
                                    ? health.last_error : NULL) ||
        !api_json_quote_or_null(last_error_type_json,
                                sizeof(last_error_type_json),
                                health.last_error_type[0]
                                    ? health.last_error_type : NULL) ||
        !api_json_quote_or_null(wd_name_json, sizeof(wd_name_json),
                                health.wd_last_recovery_name) ||
        !api_json_quote_or_null(wd_trigger_json, sizeof(wd_trigger_json),
                                health.wd_last_recovery_trigger) ||
        !api_json_quote_or_null(wd_reason_json, sizeof(wd_reason_json),
                                health.wd_last_recovery_reason) ||
        !api_json_quote_or_null(degraded_json, sizeof(degraded_json),
                                health.degraded_reason[0]
                                    ? health.degraded_reason : NULL) ||
        !api_json_quote_or_null(blocking_json, sizeof(blocking_json),
                                health.blocking_reason[0]
                                    ? health.blocking_reason : NULL) ||
        !api_json_quote_or_null(warnings_json, sizeof(warnings_json),
                                health.warning_reasons[0]
                                    ? health.warning_reasons : NULL))
        return api_json_error(response, response_max, JSON_500_HEADERS,
                              "Health string too large");

    char body[16384];
    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"healthy\":%s,"
        "\"serving\":%s,"
        "\"warning_count\":%zu,"
        "\"sync_state\":\"%s\","
        "\"chain\":{"
          "\"tip_height\":%d,"
          "\"header_height\":%d,"
          "\"peer_best_height\":%d,"
          "\"tip_lag\":%d"
        "},"
        "\"database\":{"
          "\"wal_size_bytes\":%lld,"
          "\"utxo_count\":%lld"
        "},"
        "\"network\":{"
          "\"peer_count\":%zu,"
          "\"has_peers\":%s,"
          "\"tip_stale\":%s,"
          "\"tip_stale_seconds\":%lld,"
          "\"magicbean_peer_count\":%zu,"
          "\"zclassic_c23_peer_count\":%zu"
        "},"
        "\"services\":{"
          "\"tor_enabled\":%s,"
          "\"tor_ready\":%s,"
          "\"onion_service_ready\":%s,"
          "\"onion_address\":%s"
        "},"
        "\"download\":{"
          "\"requested\":%llu,"
          "\"received\":%llu,"
          "\"timed_out\":%llu,"
          "\"in_flight\":%llu,"
          "\"queued\":%llu,"
          "\"queue_backed_up\":%s"
        "},"
        "\"boot\":{\"uptime_seconds\":%lld},"
        "\"errors\":{"
          "\"total\":%d,"
          "\"last\":%s,"
          "\"last_type\":%s,"
          "\"last_age_seconds\":%lld,"
          "\"last_recent\":%s"
        "},"
        "\"watchdog\":{"
          "\"checks_run\":%d,"
          "\"recoveries\":%d,"
          "\"blocks_per_sec\":%.1f,"
          "\"escalation_level\":%d,"
          "\"last_recovery\":%s,"
          "\"last_recovery_ago_secs\":%lld,"
          "\"last_recovery_target_height\":%d,"
          "\"last_recovery_manifest_height\":%d,"
          "\"last_recovery_trigger\":%s,"
          "\"last_recovery_reason\":%s"
        "},"
        "\"status\":{"
          "\"serving\":%s,"
          "\"blocking_reason\":%s,"
          "\"warning\":%s,"
          "\"warning_count\":%zu,"
          "\"warning_reasons\":%s,"
          "\"degraded_reason\":%s"
        "}"
        "}",
        health.healthy ? "true" : "false",
        health.serving ? "true" : "false",
        health.warning_count,
        sync_state_name(health.sync_state),
        health.tip_height,
        health.header_height,
        health.peer_best_height,
        health.tip_lag,
        (long long)health.wal_size_bytes,
        (long long)health.utxo_count,
        health.peer_count,
        health.has_peers ? "true" : "false",
        health.tip_stale ? "true" : "false",
        (long long)health.tip_stale_seconds,
        health.magicbean_peer_count,
        health.zclassic_c23_peer_count,
        health.tor_enabled ? "true" : "false",
        health.tor_ready ? "true" : "false",
        health.onion_service_ready ? "true" : "false",
        onion_json,
        (unsigned long long)health.blocks_requested,
        (unsigned long long)health.blocks_received,
        (unsigned long long)health.blocks_timed_out,
        (unsigned long long)health.in_flight,
        (unsigned long long)health.queued,
        health.queue_backed_up ? "true" : "false",
        (long long)health.uptime_seconds,
        health.error_total,
        last_error_json,
        last_error_type_json,
        (long long)health.last_error_age_seconds,
        health.last_error_recent ? "true" : "false",
        health.wd_checks_run,
        health.wd_recoveries,
        health.wd_blocks_per_sec,
        health.wd_escalation_level,
        wd_name_json,
        health.wd_last_recovery_time > 0
            ? (long long)((int64_t)platform_time_wall_time_t() - health.wd_last_recovery_time)
            : 0LL,
        health.wd_last_recovery_target_height,
        health.wd_last_recovery_manifest_height,
        wd_trigger_json,
        wd_reason_json,
        health.serving ? "true" : "false",
        blocking_json,
        health.warning ? "true" : "false",
        health.warning_count,
        warnings_json,
        degraded_json);
    if (body_len < 0 || body_len >= (int)sizeof(body))
        return api_json_error(response, response_max, JSON_500_HEADERS,
                              "Health response too large");

    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        health.healthy ? "200 OK" : "503 Service Unavailable",
        body);
}

/* Route: /api/node/snapshot — snapshot sync service status */
size_t api_serve_node_snapshot(uint8_t *response, size_t response_max)
{
    bool init = false;
    struct snapshot_sync_service *svc = api_snapshot_sync(&init);
    struct snapsync_status snap_status = {0};
    snap_status.state = SNAPSYNC_IDLE;
    uint64_t received = 0, total = 0;
    double rate = 0;
    if (init) snapsync_get_status_snapshot(svc, &snap_status);
    if (init) snapsync_get_progress(svc, &received, &total, &rate);
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"state\":\"%s\","
        "\"received\":%llu,\"total\":%llu,"
        "\"rate_per_sec\":%.0f,"
        "\"percent\":%.1f,"
        "\"serving_peer\":%u,"
        "\"offered_height\":%d,"
        "\"turbo_active\":%s}",
        init ? snapsync_state_name(snap_status.state) : "not_initialized",
        (unsigned long long)received, (unsigned long long)total,
        rate,
        total > 0 ? 100.0 * (double)received / (double)total : 0,
        init ? snap_status.serving_peer_id : 0,
        init ? snap_status.offered_height : 0,
        (init && snap_status.turbo_active) ? "true" : "false");
}

/* Route: /api/node/mmb — Merkle Mountain Belt status */
size_t api_serve_node_mmb(uint8_t *response, size_t response_max)
{
    struct mmb *mb = rpc_blockchain_get_mmb();
    uint8_t root[32] = {0};
    if (mb && mb->num_leaves > 0) mmb_root(mb, root);
    char hex[65];
    HexStr(root, 32, false, hex, sizeof(hex));
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"mmb_root\":\"%s\","
        "\"num_leaves\":%llu,"
        "\"num_peaks\":%u}",
        hex,
        (unsigned long long)(mb ? mb->num_leaves : 0),
        mb ? mb->num_mountains : 0);
}

/* Route: /api/node/status — comprehensive one-stop diagnostics */
size_t api_serve_node_status(uint8_t *response, size_t response_max)
{
    enum sync_state ss = sync_get_state();
    bool snap_init = false;
    struct snapshot_sync_service *svc = api_snapshot_sync(&snap_init);
    struct snapsync_status snap_status = {0};
    snap_status.state = SNAPSYNC_IDLE;
    struct mmb *mb = rpc_blockchain_get_mmb();
    struct error_ring *er = error_ring_global();
    uint64_t snap_received = 0, snap_total = 0;
    double snap_rate = 0;
    if (snap_init) snapsync_get_status_snapshot(svc, &snap_status);
    if (snap_init) snapsync_get_progress(svc, &snap_received, &snap_total, &snap_rate);

    /* Collect health for peer/network data */
    struct node_health_snapshot health = {0};
    node_health_collect(&health, g_api_ctx.node_db ?
        g_api_ctx.node_db : app_runtime_node_db(),
        g_api_ctx.main_state);

    /* Chain tip info */
    char tip_hash_hex[65] = "null";
    char sapling_root_hex[65] = "null";
    char best_header_hex[65] = "null";
    int tip_height = -1;
    int header_height = -1;
    int64_t tip_time = 0;
    size_t sapling_tree_size = 0;

    struct main_state *ms = g_api_ctx.main_state;
    if (ms) {
        tip_height = reducer_frontier_provable_tip_cached();
        const struct block_index *tip =
            active_chain_at(&ms->chain_active, tip_height);
        if (tip) {
            tip_time = (int64_t)tip->nTime;
            if (tip->phashBlock) {
                uint256_get_hex(tip->phashBlock, tip_hash_hex);
            }
            uint256_get_hex(&tip->hashFinalSaplingRoot, sapling_root_hex);
        }
        if (ms->pindex_best_header) {
            header_height = ms->pindex_best_header->nHeight;
            if (ms->pindex_best_header->phashBlock)
                uint256_get_hex(ms->pindex_best_header->phashBlock,
                                best_header_hex);
        }
    }
    /* Read current sapling tree from node_state (authoritative source,
     * updated by sync_controller on every block connection). */
    char sapling_tree_root_hex[65] = "null";
    {
        struct node_db *ndb = g_api_ctx.node_db ?
            g_api_ctx.node_db : app_runtime_node_db();
        if (ndb && ndb->db) {
            uint8_t tree_buf[8192];
            size_t tree_len = 0;
            if (node_db_state_get(ndb, "sapling_tree",
                    tree_buf, sizeof(tree_buf), &tree_len)
                && tree_len > 0) {
                struct incremental_merkle_tree api_tree;
                sapling_tree_init(&api_tree);
                struct byte_stream tbs;
                stream_init_from_data(&tbs, tree_buf, tree_len);
                if (incremental_tree_deserialize(&api_tree, &tbs)) {
                    sapling_tree_size = incremental_tree_size(&api_tree);
                    if (sapling_tree_size > 0) {
                        struct uint256 tree_root;
                        incremental_tree_root(&api_tree, &tree_root);
                        uint256_get_hex(&tree_root,
                                        sapling_tree_root_hex);
                    }
                }
            }
        }
    }

    /* Recent errors (up to last 8) */
    char errors_json[2048];
    size_t elen = error_ring_dump_json(er, errors_json, sizeof(errors_json));
    if (elen == 0) {
        errors_json[0] = '['; errors_json[1] = ']'; errors_json[2] = '\0';
    }

    /* Download stats */
    struct download_manager *dm = msg_get_download_mgr();
    uint64_t dl_req = 0, dl_recv = 0, dl_tout = 0, dl_inflight = 0, dl_queued = 0;
    dl_get_stats(dm, &dl_req, &dl_recv, &dl_tout, &dl_inflight, &dl_queued);

    char *body = zcl_malloc(8192, "api_sync_body");
    if (!body)
        return api_json_error(response, response_max, JSON_500_HEADERS, "OOM");

    snprintf(body, 8192,
        "{"
        "\"sync\":{\"state\":\"%s\","
          "\"replay_active\":%s,\"replay_height\":%d},"
        "\"chain\":{"
          "\"tip_height\":%d,"
          "\"tip_hash\":\"%s\","
          "\"tip_time\":%lld,"
          "\"header_height\":%d,"
          "\"best_header_hash\":\"%s\","
          "\"peer_best_height\":%d,"
          "\"tip_lag\":%d,"
          "\"blocks_behind\":%d"
        "},"
        "\"sapling\":{"
          "\"tree_size\":%zu,"
          "\"tree_root\":\"%s\","
          "\"header_root\":\"%s\","
          "\"roots_match\":%s"
        "},"
        "\"network\":{"
          "\"peer_count\":%zu,"
          "\"tip_stale\":%s,"
          "\"tip_stale_seconds\":%lld"
        "},"
        "\"download\":{"
          "\"requested\":%llu,\"received\":%llu,"
          "\"timed_out\":%llu,\"in_flight\":%llu,\"queued\":%llu"
        "},"
        "\"snapshot\":{\"state\":\"%s\","
          "\"received\":%llu,\"total\":%llu,\"rate\":%.0f},"
        "\"mmb\":{\"leaves\":%llu,\"peaks\":%u},"
        "\"database\":{"
          "\"wal_size_bytes\":%lld,"
          "\"utxo_count\":%lld"
        "},"
        "\"errors\":{\"total\":%d,"
          "\"last_type\":%s%s%s,"
          "\"last_age_seconds\":%lld,"
          "\"last_recent\":%s,"
          "\"recent\":%s},"
        "\"defer_proof_validation_below_height\":%d,"
        "\"uptime_seconds\":%lld"
        "}",
        sync_state_name(ss),
        atomic_load(&g_utxo_replay_active) ? "true" : "false",
        atomic_load(&g_utxo_replay_height),
        tip_height,
        tip_hash_hex,
        (long long)tip_time,
        header_height,
        best_header_hex,
        health.peer_best_height,
        health.tip_lag,
        health.peer_best_height > tip_height ?
            health.peer_best_height - tip_height : 0,
        sapling_tree_size,
        sapling_tree_root_hex,
        sapling_root_hex,
        (sapling_tree_size > 0 &&
         strcmp(sapling_tree_root_hex, sapling_root_hex) == 0) ?
            "true" : "false",
        health.peer_count,
        health.tip_stale ? "true" : "false",
        (long long)health.tip_stale_seconds,
        (unsigned long long)dl_req, (unsigned long long)dl_recv,
        (unsigned long long)dl_tout, (unsigned long long)dl_inflight,
        (unsigned long long)dl_queued,
        snap_init ? snapsync_state_name(snap_status.state) : "not_initialized",
        (unsigned long long)(snap_init ? snap_received : 0),
        (unsigned long long)(snap_init ? snap_total : 0),
        snap_rate,
        (unsigned long long)(mb ? mb->num_leaves : 0),
        mb ? mb->num_mountains : 0,
        (long long)health.wal_size_bytes,
        (long long)health.utxo_count,
        error_ring_total(er),
        health.last_error_type[0] ? "\"" : "null",
        health.last_error_type,
        health.last_error_type[0] ? "\"" : "",
        (long long)health.last_error_age_seconds,
        health.last_error_recent ? "true" : "false",
        errors_json,
        g_deferred_proof_validation_below_height,
        (long long)health.uptime_seconds);

    size_t n = (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n%s", body);
    free(body);
    return n;
}

/* Wallet data — balance, address, activity */
size_t api_serve_wallet(uint8_t *response, size_t response_max)
{
    struct node_db *ndb = g_api_ctx.node_db ?
        g_api_ctx.node_db : app_runtime_node_db();
    if (!ndb || !ndb->db) {
        return api_json_error(response, response_max, JSON_500_HEADERS,
                          "No database");
    }

    /* Balance — use wallet_utxos (correct spent tracking). Spendable balance
     * EXCLUDES immature coinbase (matches listunspent + the coin selector). */
    int64_t transparent = db_wallet_utxo_spendable_balance(ndb, NULL);
    int64_t shielded = db_sapling_note_balance(ndb);

    /* Address — encode the first wallet key's pubkey hash */
    char address[128] = "";
    uint8_t pkh[20];
    if (db_wallet_key_first_pubkey_hash(ndb, pkh)) {
        struct tx_destination dest;
        dest.type = DEST_KEY_ID;
        memcpy(dest.id.key.id.data, pkh, 20);
        const unsigned char pk[] = {0x1C, 0xB8};
        const unsigned char sc[] = {0x1C, 0xBD};
        encode_destination(&dest, pk, 2, sc, 2,
                           address, sizeof(address));
    }

    /* Chain height + latest block time */
    int64_t height = 0, block_time = 0;
    db_block_tip_height_and_time(ndb, &height, &block_time);

    /* Activity — last 20 wallet UTXOs with timestamps */
    struct db_wallet_activity activity[20];
    int activity_count = db_wallet_utxo_recent_activity(ndb, activity, 20);

    size_t w = 0;
    char *buf = (char *)response;
    w += (size_t)snprintf(buf + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"transparent\":%lld,\"shielded\":%lld,"
        "\"address\":\"%s\",\"height\":%lld,"
        "\"block_time\":%lld,\"now\":%lld,"
        "\"activity\":[",
        (long long)transparent, (long long)shielded,
        address, (long long)height,
        (long long)block_time, (long long)platform_time_wall_time_t());

    bool first = true;
    for (int i = 0; i < activity_count && w + 100 < response_max; i++) {
        if (!first) buf[w++] = ',';
        first = false;
        w += (size_t)snprintf(buf + w, response_max - w,
            "{\"value\":%lld,\"height\":%d,\"time\":%lld}",
            (long long)activity[i].value,
            activity[i].height,
            (long long)activity[i].time);
    }

    w += (size_t)snprintf(buf + w, response_max - w, "]}");
    return w;
}

/* GET /api/files/manifest — JSON manifest of all chunks */
size_t api_serve_files_manifest(uint8_t *response, size_t response_max)
{
    struct file_manifest manifest;
    if (!file_controller_get_manifest_copy(&manifest)) {
        /* Try building on demand */
        extern void file_controller_init(const char *);
        if (g_api_ctx.datadir) {
            file_controller_init(g_api_ctx.datadir);
            file_controller_refresh_manifest();
        }
    }
    if (!file_controller_get_manifest_copy(&manifest))
        return api_json_error(response, response_max, JSON_500_HEADERS,
                          "No block files for manifest");

    /* Build JSON response */
    char *buf = zcl_malloc(131072, "api_manifest_buf");
    if (!buf)
        return api_json_error(response, response_max, JSON_500_HEADERS, "OOM");
    size_t w = 0;
    char root_hex[65];
    HexStr(manifest.root_hash, 32, false, root_hex, sizeof(root_hex));

    w += (size_t)snprintf(buf + w, 131072 - w,
        "{\"root_hash\":\"%s\","
        "\"num_chunks\":%u,"
        "\"total_bytes\":%llu,"
        "\"chunks\":[",
        root_hex, manifest.num_chunks,
        (unsigned long long)manifest.total_bytes);
    for (uint32_t i = 0; i < manifest.num_chunks && w + 256 < 131072; i++) {
        char hex[65];
        HexStr(manifest.chunks[i].sha3, 32, false, hex, sizeof(hex));
        w += (size_t)snprintf(buf + w, 131072 - w,
            "%s{\"sha3\":\"%s\",\"size\":%u,\"file\":%d,\"offset\":%llu}",
            i > 0 ? "," : "", hex, manifest.chunks[i].size,
            manifest.chunks[i].file_index,
            (unsigned long long)manifest.chunks[i].offset);
    }
    w += (size_t)snprintf(buf + w, 131072 - w, "]}");

    size_t off = (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: public, max-age=300\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n\r\n", w);
    if (off + w <= response_max)
        memcpy(response + off, buf, w);
    free(buf);
    return off + w < response_max ? off + w : response_max;
}

/* GET /api/files/:sha3hash — raw chunk bytes by SHA3 hash */
size_t api_serve_file_chunk(const char *hex, uint8_t *response,
                            size_t response_max)
{
    struct file_manifest manifest;
    uint8_t sha3[32];
    if (ParseHex(hex, sha3, 32) != 32)
        return api_json_error(response, response_max,
            JSON_404_HEADERS, "Invalid SHA3 hash");
    if (!file_controller_get_manifest_copy(&manifest))
        return api_json_error(response, response_max,
            JSON_404_HEADERS, "No manifest");
    const struct file_chunk *chunk = file_manifest_find(&manifest, sha3);
    if (!chunk)
        return api_json_error(response, response_max,
            JSON_404_HEADERS, "Chunk not found");
    uint8_t *data = NULL;
    uint32_t data_size = 0;
    if (!file_chunk_read(chunk, g_api_ctx.datadir, &data, &data_size))
        return api_json_error(response, response_max,
            JSON_500_HEADERS, "Failed to read chunk");

    size_t off = (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: public, max-age=31536000, immutable\r\n"
        "X-SHA3-256: %s\r\n"
        "Connection: close\r\n"
        "Content-Length: %u\r\n\r\n", hex, data_size);
    if (off + data_size <= response_max)
        memcpy(response + off, data, data_size);
    free(data);
    return off + data_size < response_max ? off + data_size : response_max;
}
