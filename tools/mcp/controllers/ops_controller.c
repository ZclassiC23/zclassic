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
    if (p) {
        pc = status_count_json_objects(p);
        /* Count inbound vs outbound */
        const char *sp = p;
        while ((sp = strstr(sp, "\"inbound\"")) != NULL) {
            sp += strlen("\"inbound\"");
            while (*sp == ' ' || *sp == ':') sp++;
            if (strncmp(sp, "true", 4) == 0)
                inbound++;
            else
                outbound++;
        }
        /* Count by client type via subver */
        sp = p;
        while ((sp = strstr(sp, "\"subver\"")) != NULL) {
            sp += strlen("\"subver\"");
            while (*sp == ' ' || *sp == ':' || *sp == '"') sp++;
            if (strstr(sp, "ZClassic-C23") != NULL &&
                (strchr(sp, '"') == NULL || strstr(sp, "ZClassic-C23") < strchr(sp, '"')))
                zcl23_cnt++;
            else if (strstr(sp, "MagicBean") != NULL &&
                     (strchr(sp, '"') == NULL || strstr(sp, "MagicBean") < strchr(sp, '"')))
                magicbean_cnt++;
        }
    }

    /* Extract header_height from getblockchaininfo best_header_height */
    int header_height = (int)status_extract_json_int(ci, "best_header_height", 0);

    /* Extract max peer starting_height from getpeerinfo */
    int max_peer_height = 0;
    if (p) {
        const char *sp = p;
        while ((sp = strstr(sp, "\"startingheight\"")) != NULL) {
            sp += strlen("\"startingheight\"");
            while (*sp == ' ' || *sp == ':') sp++;
            int sh = atoi(sp);
            if (sh > max_peer_height) max_peer_height = sh;
        }
    }

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

static int h_zcl_health(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    return mcp_return_rpc_body(res, mcp_node_rpc("healthcheck", NULL),
                                "healthcheck", "mcp.ops");
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

/* zcl_rebuild_recent — bounded recovery: fetch the canonical recent
 * block range from the authoritative local zclassicd and connect it
 * through the normal validated accept path, reorging off any stale
 * local fork. Destructive (mutates the live chainstate) but never wipes
 * the UTXO set and never bypasses validation. */
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
static const struct mcp_param_spec p_rpc[] = {
    { "method", MCP_PARAM_STR, true,  "RPC method name",
      0, 0, 1, 128, NULL, NULL },
    { "params", MCP_PARAM_STR, false, "JSON params array",
      0, 0, 0, 0, NULL, "\"[]\"" },
};
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
      "Bounded recovery: fetch the canonical recent block range from the "
      "authoritative local zclassicd and connect each block through the "
      "normal validated accept path, reorging off any stale local fork. "
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
    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register(&k_routes[i]);
}
