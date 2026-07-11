/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP diagnostics controller: low-level introspection primitives split
 * out of ops_controller.c so each file stays focused.
 *
 *   zcl_sql                — SELECT-only passthrough to node.db
 *   zcl_node_log           — reverse-scan node.log on the server side
 *   zcl_state              — generic *_dump_state_json dispatcher
 *   zcl_state_catalog      — machine-readable zcl_state subsystem catalog
 *   zcl_probe_zclassicd    — drift check against local zclassicd
 *   zcl_profile            — per-thread /proc CPU sampler
 *   zcl_replay_dump        — MCP request/response replay buffer
 *   zcl_replay_exec        — re-execute a recorded MCP request
 */

#include "platform/time_compat.h"
#include "../controllers.h"
#include "../router.h"
#include "../rpc_client.h"
#include "../replay.h"
#include "../rpc_params.h"

#include "controllers/diagnostics_controller.h"
#include "controllers/diagnostics_native_handlers.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── zcl_sql ─────────────────────────────────────────────────── */

/* SELECT-only SQL passthrough to node.db. Marked destructive in middleware
 * not because it mutates (it can't) but because arbitrary scans against a
 * 100M-row table can be expensive. */
static int h_zcl_sql(const struct mcp_request *req,
                     struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_sql_body(req->args, &e);
    if (!body) {
        res->error = (e.status == ZCL_NATIVE_BODY_INTERNAL)
                         ? MCP_ERR_INTERNAL : MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message), "%s", e.message);
    }
    res->body = body;
    return 0;
}

/* ── zcl_node_log ────────────────────────────────────────────── */

/* Reverse-scan node.log via getnodelog RPC. Server-side regex match + level
 * filter. Timestamp filtering is exact for lines that carry a supported
 * timestamp; legacy undated lines remain eligible and are counted in the
 * result metadata. Bounded memory: chunks the live log and stops at
 * max_lines. */
static int h_zcl_node_log(const struct mcp_request *req,
                          struct mcp_response *res)
{
    struct zcl_native_body_err e = { 0 };
    char *body = zcl_native_node_log_body(req->args, &e);
    if (!body) {
        res->error = (e.status == ZCL_NATIVE_BODY_INTERNAL)
                         ? MCP_ERR_INTERNAL : MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message), "%s", e.message);
    }
    res->body = body;
    return 0;
}

/* ── zcl_state ───────────────────────────────────────────────── */

/* Generic in-process state dump. Dispatches by `subsystem` to the owning
 * module's `*_dump_state_json` function via the `dumpstate` RPC method.
 * Adding a new subsystem is one dispatcher line in
 * app/controllers/src/diagnostics_controller.c plus one dump function in
 * the owning module — no further MCP plumbing required. */
static int h_zcl_state(const struct mcp_request *req, struct mcp_response *res)
{
    const char *sub = json_get_str(json_get(req->args, "subsystem"));
    const char *key = json_get_str_or(req->args, "key", NULL);

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_str(&p, sub ? sub : "");
    if (key && key[0])
        mcp_params_push_str(&p, key);
    char *pjson = mcp_params_to_json(&p);

    char *out = pjson ? mcp_node_rpc("dumpstate", pjson) : NULL;
    free(pjson);
    return mcp_return_rpc_body_ctx(res, out, "dumpstate", "mcp.diag",
                                    "subsystem=%s", sub ? sub : "(null)");
}

static int h_zcl_state_catalog(const struct mcp_request *req,
                               struct mcp_response *res)
{
    (void)req;
    return mcp_return_rpc_body(res, mcp_node_rpc("statecatalog", NULL),
                               "statecatalog", "mcp.diag");
}

/* ── zcl_conditions ──────────────────────────────────────────── */

/* Self-heal condition engine dump. A fixed `zcl_state subsystem=
 * condition_engine`: dispatches dumpstate with a literal param so
 * DEFINE_PT (which passes NULL params) can't express it. */
static int h_zcl_conditions(const struct mcp_request *req,
                            struct mcp_response *res)
{
    (void)req;
    return mcp_return_rpc_body(res,
                               mcp_node_rpc("dumpstate",
                                            "[\"condition_engine\"]"),
                               "dumpstate", "mcp.conditions");
}

/* ── zcl_probe_zclassicd ─────────────────────────────────────── */

/* Drift detection against the local zclassicd (legacy C++ impl). Picks a
 * random height if none supplied, fans through to the `probezclassicd`
 * RPC, returns the raw RPC body. */
static int h_zcl_probe_zclassicd(const struct mcp_request *req,
                                 struct mcp_response *res)
{
    const struct json_value *h_val = json_get(req->args, "height");
    int height = -1;
    if (h_val) {
        if (h_val->type == JSON_INT)
            height = (int)json_get_int(h_val);
        else if (h_val->type == JSON_STR)
            height = atoi(json_get_str(h_val));
    }

    if (height < 0) {
        char *tip_s = mcp_node_rpc("getblockcount", NULL);
        int tip = tip_s ? atoi(tip_s) : 0;
        free(tip_s);
        int max_h = tip - 100;
        if (max_h <= 0) {
            res->error = MCP_ERR_HANDLER_FAILED;
            snprintf(res->error_message, sizeof(res->error_message),
                     "node not synced: tip=%d", tip);
            LOG_ERR("mcp.diag", "probe_zclassicd: tip too low (%d)", tip);
            /* MUST return: rand_r() % (unsigned)max_h below is a modulo-by-zero
             * SIGFPE when max_h==0 (tip==100, the cold-start window). */
            return -1; // raw-return-ok:logged-node-not-synced
        }
        unsigned seed = (unsigned)platform_time_wall_time_t() ^ (unsigned)getpid();
        height = (int)(rand_r(&seed) % (unsigned)max_h);
    }

    struct mcp_params p;
    mcp_params_init(&p);
    mcp_params_push_int(&p, height);
    char *pjson = mcp_params_to_json(&p);
    char *out = pjson ? mcp_node_rpc("probezclassicd", pjson) : NULL;
    free(pjson);
    return mcp_return_rpc_body_ctx(res, out, "probezclassicd", "mcp.diag",
                                    "h=%d", height);
}

/* ── zcl_profile — per-thread CPU sampler ─────────────────────
 *
 * Reads /proc/self/task/<tid>/stat for every live thread, sleeps
 * `duration_ms`, reads again, diffs utime + stime, sorts descending,
 * returns the top N. "Why is this node slow at 3am" — an operator runs
 * one MCP call and gets a hot-thread list without gdb/perf/strace.
 *
 * Blocking: the handler sleeps the calling MCP worker for duration_ms.
 * Clamped to 10 seconds max so a runaway caller can't wedge the stdio
 * loop forever.
 */
#define PROFILE_MAX_THREADS 256

struct profile_sample {
    int      tid;
    char     name[16];
    uint64_t utime;   /* clock ticks */
    uint64_t stime;
};

/* Parse utime (field 14) and stime (field 15) from /proc/<pid>/stat.
 * The `comm` field at position 2 can contain spaces and parens, so we
 * skip everything up to the LAST ')' before counting tokens. */
static bool parse_task_stat(const char *buf, uint64_t *utime, uint64_t *stime)
{
    const char *p = strrchr(buf, ')');
    if (!p) return false;
    p++;
    int fields_to_skip = 11; /* state(3) -> utime(14) is 11 steps */
    for (int i = 0; i < fields_to_skip; i++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    char *end = NULL;
    *utime = strtoull(p, &end, 10);
    if (end == p) return false;
    p = end;
    while (*p == ' ') p++;
    *stime = strtoull(p, &end, 10);
    return end != p;
}

static size_t read_task_snapshot(struct profile_sample *out, size_t cap)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < cap) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;

        char path[128];
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[1024];
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (r == 0) continue;
        buf[r] = '\0';

        uint64_t u = 0, s = 0;
        if (!parse_task_stat(buf, &u, &s)) continue;

        out[n].tid = tid;
        out[n].utime = u;
        out[n].stime = s;

        snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        FILE *cf = fopen(path, "r");
        out[n].name[0] = '\0';
        if (cf) {
            if (fgets(out[n].name, sizeof(out[n].name), cf)) {
                size_t L = strlen(out[n].name);
                if (L > 0 && out[n].name[L - 1] == '\n')
                    out[n].name[L - 1] = '\0';
            }
            fclose(cf);
        }
        n++;
    }
    closedir(d);
    return n;
}

struct profile_delta {
    int      tid;
    char     name[16];
    int64_t  utime_ticks;
    int64_t  stime_ticks;
    int64_t  total_ticks;
};

static int profile_delta_cmp(const void *a, const void *b)
{
    const struct profile_delta *pa = a;
    const struct profile_delta *pb = b;
    if (pb->total_ticks != pa->total_ticks)
        return (pb->total_ticks > pa->total_ticks) ? 1 : -1;
    return 0;
}

static int h_zcl_profile(const struct mcp_request *req,
                          struct mcp_response *res)
{
    int64_t duration_ms = json_get_int_or(req->args, "duration_ms", 1000);
    int64_t top_n       = json_get_int_or(req->args, "top_n",       10);
    if (duration_ms < 100)   duration_ms = 100;
    if (duration_ms > 10000) duration_ms = 10000;
    if (top_n < 1)           top_n = 1;
    if (top_n > 64)          top_n = 64;

    /* Stack-local scratch, NOT static: the middleware runs each handler on
     * a detached worker thread and abandons it on timeout, so a retried
     * profile call can run concurrently with the abandoned one. Sharing a
     * function-static buffer would race both invocations over the same
     * memory. ~10 KB each (256 * ~40 B) — safe on the worker thread stack. */
    struct profile_sample s1[PROFILE_MAX_THREADS];
    struct profile_sample s2[PROFILE_MAX_THREADS];
    size_t n1 = read_task_snapshot(s1, PROFILE_MAX_THREADS);
    if (n1 == 0) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "failed to read /proc/self/task (no threads found)");
        LOG_ERR("mcp.diag", "profile: read_task_snapshot returned 0 (pre-sample)");
        return 0;
    }

    struct timespec ts = {
        .tv_sec  = duration_ms / 1000,
        .tv_nsec = (duration_ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);

    size_t n2 = read_task_snapshot(s2, PROFILE_MAX_THREADS);
    if (n2 == 0) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "failed to read /proc/self/task (no threads found post-sample)");
        LOG_ERR("mcp.diag", "profile: read_task_snapshot returned 0 (post-sample)");
        return 0;
    }

    /* Stack-local (see s1/s2 above): ~12 KB, no shared mutable static. */
    struct profile_delta deltas[PROFILE_MAX_THREADS];
    size_t nd = 0;
    for (size_t i = 0; i < n2; i++) {
        const struct profile_sample *a = NULL;
        for (size_t j = 0; j < n1; j++) {
            if (s1[j].tid == s2[i].tid) { a = &s1[j]; break; }
        }
        if (!a) continue;
        int64_t du = (int64_t)s2[i].utime - (int64_t)a->utime;
        int64_t dss = (int64_t)s2[i].stime - (int64_t)a->stime;
        if (du < 0) du = 0;
        if (dss < 0) dss = 0;
        deltas[nd].tid = s2[i].tid;
        snprintf(deltas[nd].name, sizeof(deltas[nd].name), "%s", s2[i].name);
        deltas[nd].utime_ticks = du;
        deltas[nd].stime_ticks = dss;
        deltas[nd].total_ticks = du + dss;
        nd++;
    }

    qsort(deltas, nd, sizeof(deltas[0]), profile_delta_cmp);

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    struct json_value root = {0};
    json_set_object(&root);
    json_push_kv_int(&root, "duration_ms", duration_ms);
    json_push_kv_int(&root, "sampled_threads", (int64_t)nd);

    struct json_value top = {0};
    json_set_array(&top);
    size_t emit = (nd < (size_t)top_n) ? nd : (size_t)top_n;
    for (size_t i = 0; i < emit; i++) {
        int64_t user_ms = deltas[i].utime_ticks * 1000 / clk_tck;
        int64_t sys_ms  = deltas[i].stime_ticks * 1000 / clk_tck;
        struct json_value item = {0};
        json_set_object(&item);
        json_push_kv_int(&item, "tid", deltas[i].tid);
        json_push_kv_str(&item, "name", deltas[i].name);
        json_push_kv_int(&item, "user_ms", user_ms);
        json_push_kv_int(&item, "sys_ms", sys_ms);
        json_push_kv_real(&item, "cpu_pct",
                          100.0 * (double)(user_ms + sys_ms) /
                              (double)duration_ms);
        json_push_back(&top, &item);
        json_free(&item);
    }
    json_push_kv(&root, "top_threads", &top);
    json_free(&top);

    size_t need = json_write(&root, NULL, 0);
    char *out = zcl_malloc(need + 1, "profile_body");
    if (!out) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "malloc failed for profile response");
        LOG_ERR("mcp.diag", "malloc failed for profile body (%zu bytes)",
                need + 1);
        json_free(&root);
        return 0;
    }
    json_write(&root, out, need + 1);
    json_free(&root);
    res->body = out;
    return 0;
}

/* ── Replay recorder handlers ───────────────────────────────── */

static int h_zcl_replay_dump(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const struct json_value *cnt = json_get(req->args, "count");
    size_t count = cnt ? (size_t)json_get_int(cnt) : 0;
    char *out = mcp_replay_dump(count);
    if (!out) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay dump returned null");
        LOG_ERR("mcp.diag", "mcp_replay_dump returned null (count=%zu)", count);
    }
    res->body = out;
    return 0;
}

static int h_zcl_replay_exec(const struct mcp_request *req,
                               struct mcp_response *res)
{
    const struct json_value *idx_v = json_get(req->args, "index");
    if (!idx_v) {
        snprintf(res->error_message, sizeof(res->error_message),
                 "index is required");
        res->error = MCP_ERR_MISSING_PARAM;
        LOG_ERR("mcp.diag", "replay_exec: index param missing");
        return 0;
    }
    int64_t idx = json_get_int(idx_v);
    size_t total = mcp_replay_count();
    if (idx < 0 || (size_t)idx >= total) {
        snprintf(res->error_message, sizeof(res->error_message),
                 "index %lld out of range [0, %zu)",
                 (long long)idx, total);
        res->error = MCP_ERR_OUT_OF_RANGE;
        LOG_ERR("mcp.diag", "replay_exec: index %lld out of range [0, %zu)",
                (long long)idx, total);
        return 0;
    }

    char *dump = mcp_replay_dump(0);
    if (!dump) {
        res->error = MCP_ERR_INTERNAL;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay dump failed during exec");
        LOG_ERR("mcp.diag", "replay_exec: mcp_replay_dump returned null");
        return 0;
    }

    struct json_value arr;
    if (!json_read(&arr, dump, strlen(dump)) || arr.type != JSON_ARR ||
        (size_t)idx >= arr.num_children) {
        free(dump);
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay parse failed");
        res->error = MCP_ERR_INTERNAL;
        LOG_ERR("mcp.diag", "replay_exec: json parse failed for index %lld",
                (long long)idx);
        return 0;
    }

    const struct json_value *entry = &arr.children[idx];
    const struct json_value *tv = json_get(entry, "tool");
    const char *tool = tv ? json_get_str(tv) : NULL;
    if (!tool || !tool[0]) {
        json_free(&arr);
        free(dump);
        snprintf(res->error_message, sizeof(res->error_message),
                 "entry has no tool name");
        res->error = MCP_ERR_INTERNAL;
        LOG_ERR("mcp.diag", "replay_exec: entry %lld has no tool name",
                (long long)idx);
        return 0;
    }

    /* Least-privilege: replay re-dispatches through mcp_router_dispatch(),
     * which bypasses the auth-tier and destructive rate-limit checks that
     * only mcp_middleware_dispatch() applies. Re-executing a destructive
     * tool here would let a normal-tier (or unauthenticated) caller trigger
     * an action the destructive tier is meant to gate — and would skip the
     * destructive bucket entirely. Refuse to replay any destructive tool;
     * the operator must invoke it directly with the correct credential. */
    const struct mcp_tool_route *route = mcp_router_find(tool);
    if (route && (route->flags & MCP_TOOL_FLAG_DESTRUCTIVE)) {
        /* Format the message and log BEFORE freeing arr: `tool` points into
         * the parsed JSON that json_free() releases. */
        snprintf(res->error_message, sizeof(res->error_message),
                 "refusing to replay destructive tool '%s'; invoke it "
                 "directly with the destructive credential", tool);
        res->error = MCP_ERR_AUTH_REQUIRED;
        LOG_ERR("mcp.diag",
                "replay_exec: refused destructive tool=%s at index %lld",
                tool, (long long)idx);
        json_free(&arr);
        free(dump);
        return 0;
    }

    char *result = mcp_router_dispatch(tool, NULL);
    json_free(&arr);
    free(dump);

    if (!result) {
        res->error = MCP_ERR_HANDLER_FAILED;
        snprintf(res->error_message, sizeof(res->error_message),
                 "replay re-dispatch failed: tool=%s", tool);
        LOG_ERR("mcp.diag", "replay_exec re-dispatch failed: tool=%s", tool);
    }
    res->body = result;
    return 0;
}

/* ── Route table ─────────────────────────────────────────────── */

static const struct mcp_param_spec p_sql[] = {
    { "sql",   MCP_PARAM_STR, true,
      "SELECT-only query (no DDL, no semicolons, auto-LIMIT)",
      0, 0, 1, 1024, NULL, NULL },
    { "limit", MCP_PARAM_INT, false,
      "Row cap (1..100; auto-appended if SQL lacks LIMIT)",
      1, 100, 0, 0, NULL, "10" },
};
static const struct mcp_param_spec p_node_log[] = {
    { "pattern",    MCP_PARAM_STR, true,
      "POSIX-extended regex matched against each log line",
      0, 0, 1, 256, NULL, NULL },
    { "since_secs", MCP_PARAM_INT, false,
      "Filter timestamped lines to the last N seconds; 0 disables it. "
      "Undated legacy lines remain eligible and are counted.",
      0, 86400, 0, 0, NULL, "300" },
    { "max_lines",  MCP_PARAM_INT, false,
      "Cap on returned lines",
      1, 500, 0, 0, NULL, "50" },
    { "level",      MCP_PARAM_STR, false,
      "Level filter",
      0, 0, 0, 16, "all,info,warn,error,fatal", "\"all\"" },
};

/* p_state.subsystem.enum_csv + description are derived from the live
 * g_dumpers registry at mcp_register_diagnostics() time. Hence non-const:
 * we patch the pointers once at boot. New subsystems added to
 * app/controllers/src/diagnostics_controller.c auto-propagate to the MCP
 * schema with no further plumbing. */
/* Sized for the full registry: an undersized buffer silently truncates the
 * schema enum mid-subsystem-name. */
static char g_state_subsystems_csv[2048];
static char g_state_subsystem_desc[2304];
static struct mcp_param_spec p_state[] = {
    { "subsystem", MCP_PARAM_STR, true,
      "Subsystem name (filled at register-time from g_dumpers registry)",
      0, 0, 1, 64, NULL, NULL },
    { "key", MCP_PARAM_STR, false,
      "Subsystem-specific key (block_index: height or hex hash)",
      0, 0, 0, 128, NULL, NULL },
};
static const struct mcp_param_spec p_probe_zclassicd[] = {
    { "height", MCP_PARAM_INT, false,
      "Block height to probe (omit for random in [0, tip-100])",
      0, 0x7fffffff, 0, 0, NULL, NULL },
};
static const struct mcp_param_spec p_profile[] = {
    { "duration_ms", MCP_PARAM_INT, false,
      "Sample window in ms (clamped to [100, 10000])",
      100, 10000, 0, 0, NULL, "1000" },
    { "top_n", MCP_PARAM_INT, false,
      "Max threads returned, sorted by CPU (clamped to [1, 64])",
      1, 64, 0, 0, NULL, "10" },
};
static const struct mcp_param_spec p_replay_dump[] = {
    { "count", MCP_PARAM_INT, false,
      "Number of most recent entries to return (0 = all)",
      0, MCP_REPLAY_RING_SIZE, 0, 0, NULL, "0" },
};
static const struct mcp_param_spec p_replay_exec[] = {
    { "index", MCP_PARAM_INT, true,
      "Index into the replay buffer (0 = oldest)",
      0, MCP_REPLAY_RING_SIZE - 1, 0, 0, NULL, NULL },
};

static const struct mcp_tool_route k_routes[] = {
    { "zcl_state", "ops",
      "Generic target-node state dump over native dumpstate RPC. See "
      "params.subsystem.x-advisoryEnum for values known to this proxy; "
      "zcl_state_catalog on the target is authoritative. "
      "For block_index, pass `key`=height or hex hash. New subsystems "
      "plug in via *_dump_state_json (see CLAUDE.md).",
      p_state, PARAM_COUNT(p_state), h_zcl_state,
      MCP_TOOL_FLAG_ADVISORY_ENUMS, NULL },
    { "zcl_state_catalog", "ops",
      "Machine-readable zcl_state catalog generated from the native "
      "diagnostics registry: subsystem names, descriptions, key hints, "
      "cost, freshness, owner shape, and native/MCP drill-down commands.",
      NULL, 0, h_zcl_state_catalog, 0, NULL },
    { "zcl_conditions", "ops",
      "Self-heal condition engine state: registered conditions, active "
      "flags, remedy attempts, outcomes, clear counts, and thresholds.",
      NULL, 0, h_zcl_conditions, 0, NULL },
    { "zcl_probe_zclassicd", "ops",
      "Drift detection: ask the local zclassicd (independent ZClassic "
      "impl) for getblockhash(H) and compare to our block_index. Picks a "
      "random height if `height` is omitted. Returns {height, our_hash, "
      "their_hash, match}.",
      p_probe_zclassicd,
      PARAM_COUNT(p_probe_zclassicd),
      h_zcl_probe_zclassicd, 0, NULL },
    { "zcl_node_log", "ops",
      "Reverse-scan node.log server-side with regex + level filter. Avoids "
      "downloading the 56 MB log just to grep. since_secs applies to "
      "timestamped lines; undated legacy lines remain eligible and are "
      "reported in metadata. Returns newest matches first.",
      p_node_log, PARAM_COUNT(p_node_log),
      h_zcl_node_log, 0, NULL },
    { "zcl_sql", "ops",
      "SELECT-only SQL passthrough to node.db. Hard validation + 2s timeout. "
      "Marked destructive (rate-gated) because arbitrary scans can be costly.",
      p_sql, PARAM_COUNT(p_sql), h_zcl_sql, 0, NULL },
    { "zcl_profile", "ops",
      "Per-thread CPU sampler: reads /proc/self/task/*/stat before "
      "and after `duration_ms`, returns top N threads by CPU delta "
      "with name, user_ms, sys_ms, cpu_pct. For diagnosing slow "
      "nodes without attaching gdb.",
      p_profile, PARAM_COUNT(p_profile), h_zcl_profile,
      /* duration_ms sleeps that long per call — clamp to 100ms so
       * the full self_test sweep doesn't balloon by a second.
       * Its longer dispatch budget lives in middleware's
       * k_long_running_tools table, not a per-route field. */
      .self_test_args = "{\"duration_ms\":100,\"top_n\":3}" },
    { "zcl_replay_dump", "ops",
      "Dump the MCP request/response replay buffer (last 100 calls). "
      "Shows tool name, args, response, timestamp, duration, error status.",
      p_replay_dump, PARAM_COUNT(p_replay_dump),
      h_zcl_replay_dump, 0, NULL },
    { "zcl_replay_exec", "ops",
      "Re-execute a previously recorded MCP request by index from the "
      "replay buffer. Useful for debugging and regression testing. "
      "Destructive tools are refused; invoke those directly.",
      p_replay_exec, PARAM_COUNT(p_replay_exec),
      h_zcl_replay_exec, MCP_TOOL_FLAG_DESTRUCTIVE, NULL },
};

void mcp_register_diagnostics(void)
{
    /* Derive advisory p_state.subsystem hints from this proxy's g_dumpers
     * registry so
     * adding a new *_dump_state_json subsystem in
     * app/controllers/src/diagnostics_controller.c automatically updates
     * the MCP-visible examples with zero further plumbing. The target
     * statecatalog remains authoritative. */
    diagnostics_subsystems_csv(g_state_subsystems_csv,
                               sizeof(g_state_subsystems_csv));
    snprintf(g_state_subsystem_desc, sizeof(g_state_subsystem_desc),
             "Subsystem name; proxy-known examples: %s. The target "
             "zcl_state_catalog is authoritative.",
             g_state_subsystems_csv);
    p_state[0].enum_csv    = g_state_subsystems_csv;
    p_state[0].description = g_state_subsystem_desc;

    for (size_t i = 0; i < PARAM_COUNT(k_routes); i++)
        mcp_router_register_required(&k_routes[i]);
}
