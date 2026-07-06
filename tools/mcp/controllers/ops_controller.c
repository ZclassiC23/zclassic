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
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "sim/postmortem.h"
#include "sim/seed_tape.h"
#include "util/blocker.h"
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
DEFINE_PT(h_zcl_agent_liveness, "agentliveness",  "mcp.ops")
DEFINE_PT(h_zcl_agent_contracts,"agentcontracts", "mcp.ops")
DEFINE_PT(h_zcl_agent_build,    "agentbuild",     "mcp.ops")
DEFINE_PT(h_zcl_agent_interface,"agentinterface", "mcp.ops")
DEFINE_PT(h_zcl_agent_ops,      "agentops",       "mcp.ops")
DEFINE_PT(h_zcl_agent_diagnose, "agentdiagnose",  "mcp.ops")

static char *json_value_to_body(struct json_value *v, const char *label);

static void status_push_raw_json(struct json_value *obj, const char *key,
                                 const char *raw)
{
    struct json_value child;
    json_init(&child);
    if (raw && json_read(&child, raw, strlen(raw))) {
        json_push_kv(obj, key, &child);
    } else {
        json_set_null(&child);
        json_push_kv(obj, key, &child);
    }
    json_free(&child);
}

static void status_push_json_error(struct json_value *obj,
                                   const char *key,
                                   const char *message,
                                   const struct json_value *error_obj)
{
    char err_key[96];
    snprintf(err_key, sizeof(err_key), "%s_error", key ? key : "rpc");
    if (error_obj && error_obj->type == JSON_OBJ) {
        json_push_kv(obj, err_key, error_obj);
        return;
    }

    struct json_value err;
    json_init(&err);
    json_set_object(&err);
    json_push_kv_str(&err, "message", message ? message : "unknown error");
    json_push_kv(obj, err_key, &err);
    json_free(&err);
}

static void status_push_rpc_json(struct json_value *obj, const char *key,
                                 const char *raw, const char *rpc_name)
{
    struct json_value child;
    json_init(&child);
    if (raw && json_read(&child, raw, strlen(raw))) {
        json_push_kv(obj, key, &child);
        json_free(&child);
        return;
    }

    json_set_null(&child);
    json_push_kv(obj, key, &child);
    json_free(&child);

    char msg[128];
    snprintf(msg, sizeof(msg), "%s RPC returned %s",
             rpc_name ? rpc_name : key,
             raw ? "invalid JSON" : "null");
    status_push_json_error(obj, key, msg, NULL);
}

static void status_push_dumpstate_json(struct json_value *obj,
                                       const char *key,
                                       const char *raw)
{
    struct json_value child;
    json_init(&child);
    if (!raw || !json_read(&child, raw, strlen(raw))) {
        struct json_value nullv;
        json_init(&nullv);
        json_set_null(&nullv);
        json_push_kv(obj, key, &nullv);
        status_push_json_error(obj, key,
                               raw ? "invalid dumpstate JSON"
                                   : "dumpstate RPC returned null",
                               NULL);
        json_free(&nullv);
        json_free(&child);
        return;
    }

    const struct json_value *error = json_get(&child, "error");
    if (error && error->type == JSON_OBJ) {
        struct json_value nullv;
        json_init(&nullv);
        json_set_null(&nullv);
        json_push_kv(obj, key, &nullv);
        status_push_json_error(obj, key, NULL, error);
        json_free(&nullv);
        json_free(&child);
        return;
    }

    const struct json_value *state = json_get(&child, "state");
    if (state && state->type == JSON_OBJ)
        json_push_kv(obj, key, state);
    else
        json_push_kv(obj, key, &child);
    json_free(&child);
}

/* Pull a top-level quoted-string field out of a raw JSON response without
 * parsing the whole document (matches the strstr style used for the numeric
 * scrapes above). Returns false if the key is absent or not a string. */
static bool status_extract_json_str(const char *json, const char *key,
                                    char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return false;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (p[i] && p[i] != '"' && i + 1 < out_size) { out[i] = p[i]; i++; }
    if (p[i] != '"') return false;
    out[i] = '\0';
    return true;
}

/* Pull a top-level numeric field out of a raw JSON response (same strstr style
 * as status_extract_json_str). Returns dflt when the key is absent. */
static long long status_extract_json_int(const char *json, const char *key,
                                         long long dflt)
{
    if (!json || !key) return dflt;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return atoll(p);
}

/* Count JSON objects in a flat array body by counting '{' — sufficient for the
 * peer arrays returned by getpeerinfo. */
static int status_count_json_objects(const char *s)
{
    int n = 0;
    if (s) for (const char *c = s; *c; c++) if (*c == '{') n++;
    return n;
}

static bool status_parse_json(struct json_value *out, const char *raw)
{
    json_init(out);
    return raw && json_read(out, raw, strlen(raw));
}

static long long status_json_int(const struct json_value *obj,
                                 const char *key,
                                 long long dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_int(v) : dflt;
}

static const char *status_json_str(const struct json_value *obj,
                                   const char *key,
                                   const char *dflt)
{
    const struct json_value *v = json_get(obj, key);
    const char *s = json_get_str(v);
    return s && s[0] ? s : dflt;
}

static bool status_json_bool(const struct json_value *obj,
                             const char *key,
                             bool dflt)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_bool(v) : dflt;
}

static bool status_peer_subver_has(const struct json_value *peer,
                                   const char *token)
{
    const char *subver = status_json_str(peer, "subver", "");
    return token && strstr(subver, token) != NULL;
}

static bool status_peer_is_zcl23(const struct json_value *peer)
{
    return status_json_bool(peer, "zclassic23", false) ||
           status_json_bool(peer, "zclassic_c23", false) ||
           status_peer_subver_has(peer, "ZClassic23") ||
           status_peer_subver_has(peer, "ZClassic-C23");
}

static bool status_peer_is_magicbean(const struct json_value *peer)
{
    return status_json_bool(peer, "magicbean", false) ||
           status_peer_subver_has(peer, "MagicBean");
}

static void status_peer_status_counts(const struct json_value *peers,
                                      int *total_out,
                                      int *inbound_out,
                                      int *outbound_out,
                                      int *zcl23_out,
                                      int *magicbean_out,
                                      int *max_height_out)
{
    int total = 0, inbound = 0, outbound = 0, zcl23 = 0, magicbean = 0;
    int max_h = 0;

    if (peers && peers->type == JSON_ARR) {
        total = (int)json_size(peers);
        for (size_t i = 0; i < json_size(peers); i++) {
            const struct json_value *peer = json_at(peers, i);
            if (!peer || peer->type != JSON_OBJ)
                continue;
            if (status_json_bool(peer, "inbound", false))
                inbound++;
            else
                outbound++;
            if (status_peer_is_zcl23(peer))
                zcl23++;
            else if (status_peer_is_magicbean(peer))
                magicbean++;
            int h = (int)status_json_int(peer, "startingheight", 0);
            if (h > max_h)
                max_h = h;
        }
    }

    if (total_out) *total_out = total;
    if (inbound_out) *inbound_out = inbound;
    if (outbound_out) *outbound_out = outbound;
    if (zcl23_out) *zcl23_out = zcl23;
    if (magicbean_out) *magicbean_out = magicbean;
    if (max_height_out) *max_height_out = max_h;
}

static long long status_max_ll(long long a, long long b)
{
    return a > b ? a : b;
}

static void status_push_string_array(struct json_value *obj,
                                     const char *key,
                                     const char *a,
                                     const char *b)
{
    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    if (a && a[0]) {
        struct json_value item;
        json_init(&item);
        json_set_str(&item, a);
        json_push_back(&arr, &item);
        json_free(&item);
    }
    if (b && b[0] && (!a || strcmp(a, b) != 0)) {
        struct json_value item;
        json_init(&item);
        json_set_str(&item, b);
        json_push_back(&arr, &item);
        json_free(&item);
    }
    json_push_kv(obj, key, &arr);
    json_free(&arr);
}

static void status_push_lane_safety_fields(
    struct json_value *root, const struct json_value *lane)
{
    if (!root || !lane || lane->type != JSON_OBJ)
        return;

    const struct json_value *safety = json_get(lane, "deployment_safety");
    json_push_kv_str(root, "operator_lane_name",
                     status_json_str(lane, "lane", "unknown"));
    json_push_kv_bool(root, "automation_restart_ok",
                      status_json_bool(lane, "automation_restart_ok", false));
    json_push_kv_bool(root, "automation_deploy_ok",
                      status_json_bool(lane, "automation_deploy_ok", false));
    json_push_kv_bool(root, "requires_operator_confirmation",
                      status_json_bool(lane,
                                       "requires_operator_confirmation",
                                       true));
    json_push_kv_str(root, "preferred_deploy_target",
                     safety ? status_json_str(safety,
                                              "preferred_deploy_target",
                                              "unknown") : "unknown");
    json_push_kv_str(root, "safe_default_action",
                     safety ? status_json_str(safety,
                                              "safe_default_action",
                                              "inspect_operator_lane")
                            : "inspect_operator_lane");
}

static void status_peer_summary(const struct json_value *peers,
                                int *total_out,
                                int *inbound_out,
                                int *outbound_out,
                                int *ready_out,
                                long long *max_height_out)
{
    int total = 0, inbound = 0, outbound = 0, ready = 0;
    long long max_h = 0;

    if (peers && peers->type == JSON_ARR) {
        total = (int)json_size(peers);
        for (size_t i = 0; i < json_size(peers); i++) {
            const struct json_value *p = json_at(peers, i);
            if (!p || p->type != JSON_OBJ)
                continue;
            if (json_get_bool(json_get(p, "inbound")))
                inbound++;
            else
                outbound++;
            const char *state = json_get_str(json_get(p, "state"));
            if (strcmp(state, "handshake_complete") == 0 ||
                strcmp(state, "active") == 0)
                ready++;
            long long h = status_json_int(p, "startingheight", 0);
            if (h > max_h)
                max_h = h;
        }
    }

    if (total_out) *total_out = total;
    if (inbound_out) *inbound_out = inbound;
    if (outbound_out) *outbound_out = outbound;
    if (ready_out) *ready_out = ready;
    if (max_height_out) *max_height_out = max_h;
}

static int blocker_status_priority(int cls)
{
    switch ((enum blocker_class)cls) {
    case BLOCKER_RESOURCE:   return 400;
    case BLOCKER_PERMANENT:  return 300;
    case BLOCKER_DEPENDENCY: return 200;
    case BLOCKER_TRANSIENT:  return 100;
    }
    return 0;
}

static void status_push_blocker_summary(struct json_value *root)
{
    struct blocker_snapshot snaps[BLOCKER_CAP];
    int n = blocker_snapshot_all(snaps, BLOCKER_CAP);
    if (n < 0) n = 0;

    int counts[4] = {0, 0, 0, 0};
    int dominant = -1;
    int dominant_prio = -1;
    for (int i = 0; i < n; i++) {
        int cls = snaps[i].class;
        if (cls >= 0 && cls < 4) counts[cls]++;

        int prio = blocker_status_priority(cls);
        if (dominant < 0 || prio > dominant_prio ||
            (prio == dominant_prio &&
             snaps[i].age_us > snaps[dominant].age_us)) {
            dominant = i;
            dominant_prio = prio;
        }
    }

    struct json_value summary;
    json_init(&summary);
    json_set_object(&summary);
    json_push_kv_int(&summary, "active_count", n);
    json_push_kv_int(&summary, "permanent_count",
                     counts[BLOCKER_PERMANENT]);
    json_push_kv_int(&summary, "transient_count",
                     counts[BLOCKER_TRANSIENT]);
    json_push_kv_int(&summary, "dependency_count",
                     counts[BLOCKER_DEPENDENCY]);
    json_push_kv_int(&summary, "resource_count",
                     counts[BLOCKER_RESOURCE]);
    json_push_kv_int(&summary, "escape_dispatched_total",
                     blocker_escape_dispatched_count());

    struct json_value dominant_json;
    json_init(&dominant_json);
    if (dominant >= 0) {
        const struct blocker_snapshot *s = &snaps[dominant];
        json_set_object(&dominant_json);
        json_push_kv_str(&dominant_json, "id", s->id);
        json_push_kv_str(&dominant_json, "owner", s->owner_subsystem);
        json_push_kv_str(&dominant_json, "class",
                         blocker_class_name((enum blocker_class)s->class));
        json_push_kv_int(&dominant_json, "age_us", s->age_us);
        json_push_kv_int(&dominant_json, "deadline_remaining_us",
                         s->deadline_remaining_us);
        json_push_kv_str(&dominant_json, "escape_action",
                         s->escape_action);
        json_push_kv_int(&dominant_json, "retry_count", s->retry_count);
        json_push_kv_int(&dominant_json, "retry_budget", s->retry_budget);
        json_push_kv_int(&dominant_json, "fire_count", s->fire_count);
        json_push_kv_str(&dominant_json, "reason", s->reason);
    } else {
        json_set_null(&dominant_json);
    }

    json_push_kv(&summary, "dominant", &dominant_json);
    json_push_kv(root, "blockers", &summary);
    json_push_kv(root, "dominant_blocker", &dominant_json);

    json_free(&dominant_json);
    json_free(&summary);
}


/* ── Handlers ───────────────────────────────────────────────── */

static int h_zcl_status(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    char *h  = mcp_node_rpc("getblockcount", NULL);
    char *p  = mcp_node_rpc("getpeerinfo", NULL);
    char *s  = mcp_node_rpc("syncstate", NULL);
    char *v  = mcp_node_rpc("validationstatus", NULL);
    char *hc = mcp_node_rpc("healthcheck", NULL);
    char *ci = mcp_node_rpc("getblockchaininfo", NULL);
    char *cac = mcp_node_rpc("dumpstate", "[\"chain_advance_coordinator\"]");
    char *rf = mcp_node_rpc("dumpstate", "[\"reducer_frontier\"]");
    char *tf = mcp_node_rpc("dumpstate", "[\"tip_finalize\"]");
    char *ce = mcp_node_rpc("dumpstate", "[\"condition_engine\"]");

    int pc = 0, inbound = 0, outbound = 0, zcl23_cnt = 0, magicbean_cnt = 0;
    int max_peer_height = 0;
    struct json_value peers_j;
    bool peers_ok = status_parse_json(&peers_j, p) &&
                    peers_j.type == JSON_ARR;
    if (peers_ok) {
        status_peer_status_counts(&peers_j, &pc, &inbound, &outbound,
                                  &zcl23_cnt, &magicbean_cnt,
                                  &max_peer_height);
    } else {
        pc = status_count_json_objects(p);
    }

    /* Extract header_height from getblockchaininfo best_header_height */
    int header_height = (int)status_extract_json_int(ci, "best_header_height", 0);

    int block_height = h ? atoi(h) : 0;
    int header_gap = max_peer_height - header_height;
    if (header_gap < 0) header_gap = 0;
    bool sync_behind = header_gap > 144;

    /* Extract memory_rss_mb and uptime_seconds from healthcheck response */
    int64_t memory_rss_mb = status_extract_json_int(hc, "memory_rss_mb", -1);
    int64_t uptime_secs   = status_extract_json_int(hc, "uptime_seconds", 0);

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_int(&root, "height", block_height);
    /* build_commit must describe the NODE. This MCP server is a separate
     * long-lived process and can be running an older binary than the node
     * it queries — stamping our own hash here can mis-report the deployed
     * node version. Scrape the node's value from its healthcheck; surface
     * ours only when it differs. */
    char node_commit[64];
    bool have_node_commit =
        status_extract_json_str(hc, "build_commit",
                                node_commit, sizeof(node_commit));
    json_push_kv_str(&root, "build_commit",
                     have_node_commit ? node_commit : zcl_build_commit());
    if (have_node_commit && strcmp(node_commit, zcl_build_commit()) != 0)
        json_push_kv_str(&root, "mcp_build_commit", zcl_build_commit());
    json_push_kv_int(&root, "header_height", header_height);
    json_push_kv_int(&root, "max_peer_height", max_peer_height);
    json_push_kv_int(&root, "header_gap", header_gap);
    json_push_kv_bool(&root, "sync_behind", sync_behind);
    json_push_kv_int(&root, "peers", pc);

    struct json_value conn;
    json_init(&conn);
    json_set_object(&conn);
    json_push_kv_int(&conn, "total", pc);
    json_push_kv_int(&conn, "inbound", inbound);
    json_push_kv_int(&conn, "outbound", outbound);
    json_push_kv_int(&conn, "zcl23", zcl23_cnt);
    json_push_kv_int(&conn, "magicbean", magicbean_cnt);
    json_push_kv(&root, "connections", &conn);
    json_free(&conn);

    json_push_kv_int(&root, "memory_rss_mb", memory_rss_mb);
    json_push_kv_int(&root, "uptime_secs", uptime_secs);
    status_push_raw_json(&root, "sync", s);
    status_push_raw_json(&root, "validation", v);
    status_push_raw_json(&root, "health", hc);
    status_push_dumpstate_json(&root, "chain_advance", cac);
    status_push_dumpstate_json(&root, "reducer_frontier", rf);
    status_push_dumpstate_json(&root, "tip_finalize", tf);
    status_push_dumpstate_json(&root, "condition_engine", ce);
    status_push_blocker_summary(&root);

    char *out = json_value_to_body(&root, "status_body");
    json_free(&root);
    json_free(&peers_j);
    free(h); free(p); free(s); free(v); free(hc); free(ci); free(cac);
    free(rf); free(tf); free(ce);
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for status response");
        LOG_ERR("mcp.ops", "malloc failed for status body");
        return -1;  // raw-return-ok:logged-oom
    }
    res->body = out;
    return 0;
}

static int h_zcl_operator_summary(const struct mcp_request *req,
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

    struct json_value chain_j, peers_j, diag_j, dl_j, mirror_j, health_j,
                      agent_j;
    bool chain_ok  = status_parse_json(&chain_j, chain) &&
                     chain_j.type == JSON_OBJ;
    bool peers_ok  = status_parse_json(&peers_j, peers) &&
                     peers_j.type == JSON_ARR;
    bool diag_ok   = status_parse_json(&diag_j, diag) &&
                     diag_j.type == JSON_OBJ;
    bool dl_ok     = status_parse_json(&dl_j, dl) &&
                     dl_j.type == JSON_OBJ;
    bool mirror_ok = status_parse_json(&mirror_j, mirror) &&
                     mirror_j.type == JSON_OBJ;
    bool health_ok = status_parse_json(&health_j, health) &&
                     health_j.type == JSON_OBJ;
    bool agent_ok  = status_parse_json(&agent_j, agent) &&
                     agent_j.type == JSON_OBJ;

    int peer_total = 0, peer_inbound = 0, peer_outbound = 0, peer_ready = 0;
    long long peer_max_height = 0;
    if (peers_ok) {
        status_peer_summary(&peers_j, &peer_total, &peer_inbound,
                            &peer_outbound, &peer_ready, &peer_max_height);
    } else {
        peer_total = status_count_json_objects(peers);
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

    long long chain_rpc_height =
        chain_ok ? status_json_int(&chain_j, "blocks", -1) : -1;
    long long diag_chain_height =
        diag_ok ? status_json_int(&diag_j, "chain_height", -1) : -1;
    long long health_local_height =
        health_chain ? status_json_int(health_chain, "local_height", -1) : -1;
    long long evidence_tip =
        health_evidence ? status_json_int(health_evidence, "active_tip", -1)
                        : -1;

    long long indexed_height = status_max_ll(diag_chain_height,
                                             health_local_height);
    indexed_height = status_max_ll(indexed_height, evidence_tip);
    if (indexed_height < 0)
        indexed_height = 0;

    long long height = chain_rpc_height >= 0 ? chain_rpc_height
                                             : indexed_height;

    long long header_height =
        chain_ok ? status_json_int(&chain_j, "best_header_height", 0) : 0;
    header_height = status_max_ll(
        header_height,
        diag_ok ? status_json_int(&diag_j, "best_header_height", 0) : 0);

    long long target_height = status_max_ll(header_height, peer_max_height);
    target_height = status_max_ll(
        target_height,
        health_chain ? status_json_int(health_chain, "target_height", 0) : 0);
    target_height = status_max_ll(
        target_height,
        mirror_ok ? status_json_int(&mirror_j, "target_height", 0) : 0);
    long long gap = target_height > indexed_height
        ? target_height - indexed_height : 0;
    long long served_gap = target_height > height ? target_height - height : 0;

    const char *sync_state =
        diag_ok ? status_json_str(&diag_j, "sync_state", "") : "";
    if (!sync_state[0] && health_ok)
        sync_state = status_json_str(&health_j, "sync_state", "");
    if (!sync_state[0] && dl_ok)
        sync_state = status_json_str(&dl_j, "sync_state", "");
    if (!sync_state[0])
        sync_state = "unknown";

    bool healthy = health_ok &&
                   status_json_bool(&health_j, "healthy", false);
    bool serving = health_ok &&
                   status_json_bool(&health_j, "serving", false);
    bool operator_needed =
        checks && status_json_bool(checks, "operator_needed", false);
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

    long long in_flight = dl_ok ? status_json_int(&dl_j, "in_flight", 0) : 0;
    long long queued = dl_ok ? status_json_int(&dl_j, "queued", 0) : 0;

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
    if (!chain_ok && !health_ok) {
        status = "rpc_unavailable";
        primary_blocker = "rpc_unavailable";
        next_action = "check MCP RPC cookie and node RPC reachability";
        next_tool = "zcl_health";
    } else if (operator_needed) {
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
    } else if (health_ok && !serving) {
        status = "blocked";
        primary_blocker = health_blocking_reason[0]
            ? health_blocking_reason : "not_serving";
        next_action = "inspect health and typed blockers";
        next_tool = "zcl_health";
        next_tool2 = "zcl_blockers";
    } else if (gap > 0 && peer_total <= 0) {
        status = "blocked";
        primary_blocker = "no_peers";
        next_action = "connect or inspect peers";
        next_tool = "zcl_peers";
    } else if (gap > 0) {
        status = (in_flight + queued) > 0 ? "catching_up" : "degraded";
        primary_blocker = (in_flight + queued) > 0
            ? "chain_gap"
            : "download_queue_idle";
        next_action = (in_flight + queued) > 0
            ? "wait for gap-fill and recheck"
            : "inspect sync diagnostics and recent download/gap-fill logs";
        next_tool = "zcl_syncdiag";
        next_tool2 = "zcl_node_log";
    } else if (active_conditions > 0 || unresolved_conditions > 0) {
        status = "degraded";
        primary_blocker = "condition_active";
        next_action = "inspect active self-heal conditions";
        next_tool = "zcl_conditions";
    } else if (health_ok && !healthy) {
        status = "degraded";
        primary_blocker = "healthcheck_unhealthy";
        next_action = "inspect health checks";
        next_tool = "zcl_health";
    } else {
        status = "healthy";
        primary_blocker = "none";
        next_action = "none";
        next_tool = "";
    }

    char summary[512];
    snprintf(summary, sizeof(summary),
             "%s: height=%lld indexed=%lld target=%lld gap=%lld "
             "served_gap=%lld sync=%s peers=%d "
             "ready_peers=%d primary=%s",
             status, height, indexed_height, target_height, gap, served_gap,
             sync_state, peer_total, peer_ready, primary_blocker);

    struct json_value root, peer_obj, mirror_obj, download_obj, raw;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_str(&root, "schema", "zcl.operator_summary.v1");
    json_push_kv_str(&root, "api_version", "v1");
    json_push_kv_str(&root, "status", status);
    json_push_kv_bool(&root, "healthy", strcmp(status, "healthy") == 0);
    json_push_kv_bool(&root, "serving", serving);
    json_push_kv_str(&root, "summary", summary);
    json_push_kv_int(&root, "height", height);
    json_push_kv_int(&root, "served_height", height);
    json_push_kv_int(&root, "indexed_height", indexed_height);
    json_push_kv_int(&root, "chain_rpc_height", chain_rpc_height);
    json_push_kv_int(&root, "header_height", header_height);
    json_push_kv_int(&root, "target_height", target_height);
    json_push_kv_int(&root, "gap", gap);
    json_push_kv_int(&root, "served_gap", served_gap);
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
    json_push_kv_bool(&root, "operator_needed", operator_needed);
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
    json_push_kv_int(&peer_obj, "total", peer_total);
    json_push_kv_int(&peer_obj, "inbound", peer_inbound);
    json_push_kv_int(&peer_obj, "outbound", peer_outbound);
    json_push_kv_int(&peer_obj, "ready", peer_ready);
    json_push_kv_int(&peer_obj, "max_height", peer_max_height);
    json_push_kv(&root, "peers", &peer_obj);
    json_free(&peer_obj);

    json_init(&download_obj);
    json_set_object(&download_obj);
    json_push_kv_int(&download_obj, "in_flight", in_flight);
    json_push_kv_int(&download_obj, "queued", queued);
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

    status_push_blocker_summary(&root);

    json_init(&raw);
    json_set_object(&raw);
    status_push_rpc_json(&raw, "chain", chain, "getblockchaininfo");
    status_push_rpc_json(&raw, "syncdiag", diag, "getsyncdiag");
    status_push_rpc_json(&raw, "downloadstats", dl, "downloadstats");
    status_push_rpc_json(&raw, "mirror", mirror, "getmirrorstatus");
    status_push_rpc_json(&raw, "health", health, "healthcheck");
    status_push_rpc_json(&raw, "agent", agent, "agent");
    json_push_kv(&root, "raw", &raw);
    json_free(&raw);

    char *out = json_value_to_body(&root, "operator_summary_body");
    json_free(&root);
    json_free(&chain_j);
    json_free(&peers_j);
    json_free(&diag_j);
    json_free(&dl_j);
    json_free(&mirror_j);
    json_free(&health_j);
    json_free(&agent_j);
    free(chain); free(peers); free(diag); free(dl); free(mirror); free(health);
    free(agent);
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for operator summary response");
        LOG_ERR("mcp.ops", "malloc failed for operator summary body");
        return -1; // raw-return-ok:logged-oom
    }
    res->body = out;
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
        params = json_value_to_body((struct json_value *)files,
                                    "agent_impact_params");
        if (!params) {
            res->error = MCP_ERR_INTERNAL;
            snprintf(res->error_message, sizeof(res->error_message),
                     "malloc failed for agent impact params");
            LOG_ERR("mcp.ops", "malloc failed for agent impact params");
            return -1; // raw-return-ok:logged-oom
        }
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
    const char *category = json_get_str_or(req->args, "category", "all");
    int64_t count = json_get_int_or(req->args, "count", 50);

    struct json_value arr, obj;
    json_init(&arr);
    json_set_array(&arr);
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "category",
                     (category && category[0]) ? category : "all");
    json_push_kv_int(&obj, "count", count);

    const char *str_filters[] = {
        "reducer_stage", "stage", "condition", "deploy", "lane",
    };
    const char *int_filters[] = {
        "scan_count", "scan", "since_us", "since_secs", "peer", "height",
    };
    for (size_t i = 0; i < sizeof(str_filters) / sizeof(str_filters[0]); i++) {
        const char *v = json_get_str(json_get(req->args, str_filters[i]));
        if (v && v[0])
            json_push_kv_str(&obj, str_filters[i], v);
    }
    for (size_t i = 0; i < sizeof(int_filters) / sizeof(int_filters[0]); i++) {
        const struct json_value *v = json_get(req->args, int_filters[i]);
        if (v && (v->type == JSON_INT || v->type == JSON_REAL))
            json_push_kv_int(&obj, int_filters[i], json_get_int(v));
    }
    json_push_back(&arr, &obj);

    size_t need = json_write(&arr, NULL, 0);
    char *params = zcl_malloc(need + 1u, "mcp timeline params");
    if (params)
        json_write(&arr, params, need + 1u);
    char *body = params ? mcp_node_rpc("timeline", params) : NULL;
    free(params);
    json_free(&obj);
    json_free(&arr);
    return mcp_return_rpc_body(res, body, "timeline", "mcp.ops");
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
    (void)req;
    char *height    = mcp_node_rpc("getblockcount",     NULL);
    char *peers     = mcp_node_rpc("getpeerinfo",       NULL);
    char *sync      = mcp_node_rpc("syncstate",         NULL);
    char *val       = mcp_node_rpc("validationstatus",  NULL);
    char *health    = mcp_node_rpc("healthcheck",       NULL);
    char *mempool   = mcp_node_rpc("getmempoolinfo",    NULL);
    char *wallet    = mcp_node_rpc("getwalletinfo",     NULL);
    char *chain     = mcp_node_rpc("getblockchaininfo", NULL);
    char *network   = mcp_node_rpc("getnetworkinfo",    NULL);

    int peer_count = status_count_json_objects(peers);

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    status_push_rpc_json(&root, "height", height, "getblockcount");
    json_push_kv_int(&root, "peer_count", peer_count);
    status_push_rpc_json(&root, "sync", sync, "syncstate");
    status_push_rpc_json(&root, "validation", val, "validationstatus");
    status_push_rpc_json(&root, "health", health, "healthcheck");
    status_push_rpc_json(&root, "mempool", mempool, "getmempoolinfo");
    status_push_rpc_json(&root, "wallet", wallet, "getwalletinfo");
    status_push_rpc_json(&root, "chain", chain, "getblockchaininfo");
    status_push_rpc_json(&root, "network", network, "getnetworkinfo");

    free(height); free(peers); free(sync); free(val); free(health);
    free(mempool); free(wallet); free(chain); free(network);
    char *out = json_value_to_body(&root, "kpi_body");
    json_free(&root);
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for KPI response");
        LOG_ERR("mcp.ops", "malloc failed for kpi body");
        return -1; // raw-return-ok:logged-oom
    }
    res->body = out;
    return 0;
}

static int h_zcl_self_heal_stats(const struct mcp_request *req,
                                  struct mcp_response *res)
{
    (void)req;
    struct self_heal_scan_stats stats;
    process_block_self_heal_stats_snapshot(&stats);

    char *out = zcl_malloc(512, "self_heal_stats_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for self-heal stats response");
        LOG_ERR("mcp.ops", "malloc failed for self-heal stats body");
        return 0;
    }

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
    (void)req;
    char *diag = mcp_node_rpc("getsyncdiag", NULL);
    char *dl   = mcp_node_rpc("downloadstats", NULL);
    char *pi   = mcp_node_rpc("getpeerinfo", NULL);

    /* Extract peer_max_height from getpeerinfo (max starting_height) */
    int peer_max_height = 0;
    if (pi) {
        /* Scan for "startingheight": N — take the maximum */
        const char *p = pi;
        while ((p = strstr(p, "\"startingheight\"")) != NULL) {
            p += strlen("\"startingheight\"");
            while (*p == ' ' || *p == ':') p++;
            int h = atoi(p);
            if (h > peer_max_height) peer_max_height = h;
        }
    }

    struct json_value root;
    struct json_value diag_json;
    json_init(&root);
    json_init(&diag_json);
    json_set_object(&root);

    if (diag && json_read(&diag_json, diag, strlen(diag)) &&
        diag_json.type == JSON_OBJ) {
        for (size_t i = 0; i < diag_json.num_children; i++) {
            const char *key = diag_json.keys[i];
            if (strcmp(key, "peer_max_height") == 0 ||
                strcmp(key, "download") == 0)
                continue;
            json_push_kv(&root, key, &diag_json.children[i]);
        }
    } else {
        json_push_kv_str(&root, "error", "getsyncdiag RPC failed");
        status_push_json_error(&root, "getsyncdiag",
            diag ? "getsyncdiag RPC returned invalid JSON"
                 : "getsyncdiag RPC returned null",
            NULL);
    }

    json_push_kv_int(&root, "peer_max_height", peer_max_height);
    status_push_rpc_json(&root, "download", dl, "downloadstats");
    json_free(&diag_json);
    free(diag); free(dl); free(pi);
    char *out = json_value_to_body(&root, "syncdiag_body");
    json_free(&root);
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for syncdiag response");
        LOG_ERR("mcp.ops", "malloc failed for syncdiag body");
        return -1; // raw-return-ok:logged-oom
    }
    res->body = out;
    return 0;
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
 * me what's actively blocking the node right now." Same JSON body, no
 * subsystem= argument, easier to script against. */
static int h_zcl_blockers(const struct mcp_request *req,
                          struct mcp_response *res)
{
    (void)req;

    struct json_value root;
    json_init(&root);
    if (!blocker_dump_state_json(&root, NULL)) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "failed to dump blocker state");
        LOG_ERR("mcp.ops", "failed to dump blocker state");
        return 0;
    }

    char *out = json_value_to_body(&root, "zcl_blockers_body");
    json_free(&root);
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for blockers body");
        LOG_ERR("mcp.ops", "malloc failed for zcl_blockers body");
        return 0;
    }
    res->body = out;
    return 0;
}

/* ── Phase 6b postmortem capsules ───────────────────────────── */

static int postmortem_default_dir(char *buf, size_t cap)
{
    const char *home = getenv("HOME");
    int n;
    if (home && *home) {
        n = snprintf(buf, cap, "%s/.zclassic-c23/postmortems", home);
    } else {
        n = snprintf(buf, cap, "./.zclassic-c23/postmortems");
    }
    if (n < 0 || (size_t)n >= cap) return -ENOSPC;
    return 0;
}

static char *json_value_to_body(struct json_value *v, const char *label)
{
    size_t need = json_write(v, NULL, 0);
    char *out = zcl_malloc(need + 1, label);
    if (!out) return NULL;
    json_write(v, out, need + 1);
    return out;
}

static int h_zcl_postmortem_list(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const char *dir = json_get_str_or(req->args, "dir", NULL);
    char default_dir[512];
    if (!dir || !*dir) {
        if (postmortem_default_dir(default_dir, sizeof(default_dir)) != 0) {
            res->error = MCP_ERR_INTERNAL;
            snprintf(res->error_message, sizeof(res->error_message),
                     "default postmortem dir path too long");
            LOG_ERR("mcp.ops", "default postmortem dir path too long");
            return 0;
        }
        dir = default_dir;
    }

    int64_t limit_i = json_get_int_or(req->args, "limit", 20);
    if (limit_i < 1) limit_i = 1;
    if (limit_i > 100) limit_i = 100;
    size_t limit = (size_t)limit_i;

    struct postmortem_summary *summaries =
        zcl_malloc(sizeof(*summaries) * limit, "mcp.postmortem.list");
    if (!summaries) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for postmortem summary list");
        LOG_ERR("mcp.ops", "malloc failed for %zu postmortem summaries",
                limit);
        return 0;
    }

    size_t count = 0;
    int rc = postmortem_list(dir, summaries, limit, &count);
    if (rc != 0) {
        free(summaries);
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "postmortem list failed for %s (rc=%d)", dir, rc);
        LOG_ERR("mcp.ops", "postmortem list failed dir=%s rc=%d", dir, rc);
        return 0;
    }

    struct json_value root, arr;
    json_init(&root); json_set_object(&root);
    json_init(&arr);  json_set_array(&arr);
    size_t returned = count < limit ? count : limit;
    json_push_kv_str(&root, "dir", dir);
    json_push_kv_int(&root, "total", (int64_t)count);
    json_push_kv_int(&root, "returned", (int64_t)returned);
    json_push_kv_int(&root, "limit", (int64_t)limit);

    for (size_t i = 0; i < returned; i++) {
        struct json_value item;
        json_init(&item); json_set_object(&item);
        json_push_kv_str(&item, "path", summaries[i].path);
        json_push_kv_int(&item, "crash_unix", summaries[i].crash_unix);
        json_push_kv_int(&item, "crash_signal",
                         (int64_t)summaries[i].crash_signal);
        json_push_kv_int(&item, "capsule_bytes",
                         (int64_t)summaries[i].capsule_bytes);
        json_push_kv_int(&item, "tape_size_bytes",
                         (int64_t)summaries[i].tape_size_bytes);
        json_push_back(&arr, &item);
        json_free(&item);
    }
    json_push_kv(&root, "capsules", &arr);

    char *body = json_value_to_body(&root, "mcp.postmortem.list.body");
    json_free(&arr);
    json_free(&root);
    free(summaries);
    if (!body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for postmortem list response");
        LOG_ERR("mcp.ops", "malloc failed for postmortem list body");
        return 0;
    }
    res->body = body;
    return 0;
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
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for postmortem replay payload");
        LOG_ERR("mcp.ops", "malloc failed for postmortem replay payload");
        return 0;
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

    char *body = json_value_to_body(&root, "mcp.postmortem.replay.body");
    json_free(&events);
    json_free(&root);
    if (!body) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for postmortem replay response");
        LOG_ERR("mcp.ops", "malloc failed for postmortem replay body");
        return 0;
    }
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
    { "agentliveness", NULL, 0, h_zcl_agent_liveness, 0, NULL },
    { "agentimpact", p_agent_impact, PARAM_COUNT(p_agent_impact),
      h_zcl_agent_impact, 0,
      "{\"files\":[\"app/controllers/src/event_controller.c\","
      "\"tools/mcp/controllers/ops_controller.c\"]}" },
    { "agentcontracts", NULL, 0, h_zcl_agent_contracts, 0, NULL },
    { "agentbuild", NULL, 0, h_zcl_agent_build, 0, NULL },
    { "agentinterface", NULL, 0, h_zcl_agent_interface, 0, NULL },
    { "agentops", NULL, 0, h_zcl_agent_ops, 0, NULL },
    { "agentdiagnose", NULL, 0, h_zcl_agent_diagnose, 0, NULL },
    { "agentdeployguard", p_agent_deploy_guard,
      PARAM_COUNT(p_agent_deploy_guard), h_zcl_agent_deploy_guard, 0,
      "{\"action\":\"canonical-deploy\"}" },
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
      "Node status: block height, peers, sync state, onion address, "
      "bg-validation progress, health checks, and chain advance source "
      "scoring. The single command to check if everything is working.",
      NULL, 0, h_zcl_status, 0, NULL },
    { "zcl_operator_summary", "ops",
      "MCP-friendly operator summary: stable top-level status, height, "
      "target height, gap, peer counts, primary blocker, next action, "
      "and recommended next tools, with raw diagnostics attached under "
      "`raw` for drill-down.",
      NULL, 0, h_zcl_operator_summary, 0, NULL },
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
      "one response. The flagship operator tool for debugging.",
      NULL, 0, h_zcl_kpi, 0, NULL },
    { "zcl_self_heal_stats", "ops",
      "Self-heal UTXO recovery counters: tx-index hits, bounded scan "
      "hits/exhaustion, total scanned blocks, and active scan depth.",
      NULL, 0, h_zcl_self_heal_stats, 0, NULL },
    { "zcl_blockers", "ops",
      "Typed blocker registry: active blockers by class "
      "{permanent,transient,dependency,resource}, deadlines, escape "
      "actions, fire counts, retry budgets. Same shape as "
      "`zcl_state subsystem=blocker` but at a top-level tool name "
      "for direct invocation. PERMANENT>0 is always an operator "
      "escalation event — typed-PERMANENT means we will not auto-retry.",
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
