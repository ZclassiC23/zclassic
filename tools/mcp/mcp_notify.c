/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP event push out-channel implementation. See mcp_notify.h for the
 * contract and the 4.1 source-swap seam. */

#include "mcp_notify.h"
#include "rpc_client.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ── Operator-class allow-list ──────────────────────────────────
 * Only events that demand operator attention are pushed. Everything
 * else stays poll-only via zcl_events so we never flood the agent
 * with routine block-connect / peer-handshake chatter. These strings
 * are event_type_name() outputs (lib/event/src/event.c). The set is
 * the named operator signals plus the loudest integrity alarms: a
 * suspected fork, a violated anchor, a halted chain, a lag-SLO breach,
 * a needed operator action, a failed boot validation, a critical disk,
 * a UTXO drift, a coins-flush failure, an oracle disagreement, a
 * crash, and a detected condition. Add a string here to push a new class.
 *
 * "condition.detected" (EV_CONDITION_DETECTED) is the generic
 * "name=... severity=..." vehicle the framework condition-engine
 * healers already emit on every new detection episode
 * (lib/framework/src/condition.c) AND the vehicle the metrics
 * threshold-alert rule table uses (tools/mcp/metrics.c
 * mcp_metrics_evaluate_alert_rules(), payload name prefixed
 * "metric_alert."). Both producers are edge-triggered — one push per
 * detection episode, not per poll — so allow-listing the type does not
 * reintroduce the routine-chatter flood this list exists to avoid. */
static const char *const k_operator_events[] = {
    "condition.operator_needed",   /* EV_OPERATOR_NEEDED */
    "condition.detected",          /* EV_CONDITION_DETECTED */
    "oracle.chain_halted",         /* EV_CHAIN_HALTED */
    "oracle.fork_suspected",       /* EV_FORK_SUSPECTED */
    "oracle.anchor_panic",         /* EV_ANCHOR_PANIC */
    "oracle.disagree",             /* EV_ORACLE_DISAGREE */
    "mirror.lag_slo_breach",       /* EV_LAG_SLO_BREACH */
    "peer.floor_breach",           /* EV_PEER_FLOOR_BREACH */
    "boot.validation_failed",      /* EV_BOOT_VALIDATION_FAILED */
    "disk.critical",               /* EV_DISK_CRITICAL */
    "chain.utxo_drift_detected",   /* EV_UTXO_DRIFT_DETECTED */
    "chain.coins_flush_fail",      /* EV_COINS_FLUSH_FAILED */
    "sys.crash",                   /* EV_CRASH */
};

bool mcp_notify_is_operator_event(const char *event_type)
{
    if (!event_type) return false;
    for (size_t i = 0; i < sizeof(k_operator_events) /
                           sizeof(k_operator_events[0]); i++)
        if (strcmp(event_type, k_operator_events[i]) == 0)
            return true;
    return false;
}

/* MCP "logging" severity for an operator event. A halted chain, a
 * violated anchor, a fatal lag breach, a crash, or a failed boot
 * validation are errors; the rest are warnings worth surfacing. The
 * agent renders level to decide how loudly to react.
 *
 * This is a per-TYPE bucket, not per-payload: "condition.detected" and
 * "mirror.lag_slo_breach" both carry a finer-grained severity string
 * inside their own payload ("severity=warning|critical|..."), but every
 * instance of the type still gets the same top-level MCP level here
 * (warning) — same precedent as mirror.lag_slo_breach below, which
 * stays "warning" even for its own payload's "severity=critical|fatal".
 * The agent can still branch on the embedded field for finer handling. */
static const char *severity_for(const char *event_type)
{
    if (strcmp(event_type, "oracle.chain_halted") == 0 ||
        strcmp(event_type, "oracle.anchor_panic") == 0 ||
        strcmp(event_type, "sys.crash") == 0 ||
        strcmp(event_type, "boot.validation_failed") == 0 ||
        strcmp(event_type, "chain.coins_flush_fail") == 0 ||
        strcmp(event_type, "chain.utxo_drift_detected") == 0)
        return "error";
    return "warning";
}

/* Append `s` to out[*pos] with JSON string-body escaping (no quotes).
 * Stops before out_sz; updates *pos. */
static void append_escaped(char *out, size_t out_sz, size_t *pos,
                           const char *s)
{
    if (!s) return;
    for (; *s && *pos + 2 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  out[(*pos)++] = '\\'; out[(*pos)++] = '"';  break;
        case '\\': out[(*pos)++] = '\\'; out[(*pos)++] = '\\'; break;
        case '\n': out[(*pos)++] = '\\'; out[(*pos)++] = 'n';  break;
        case '\r': out[(*pos)++] = '\\'; out[(*pos)++] = 'r';  break;
        case '\t': out[(*pos)++] = '\\'; out[(*pos)++] = 't';  break;
        default:
            if (c < 0x20) {
                /* Control char -> \u00XX (needs 6 bytes). */
                if (*pos + 6 >= out_sz) return;
                static const char hexd[] = "0123456789abcdef";
                out[(*pos)++] = '\\'; out[(*pos)++] = 'u';
                out[(*pos)++] = '0';  out[(*pos)++] = '0';
                out[(*pos)++] = hexd[(c >> 4) & 0xf];
                out[(*pos)++] = hexd[c & 0xf];
            } else {
                out[(*pos)++] = (char)c;
            }
        }
    }
}

size_t mcp_notify_build_frame(char *out, size_t out_sz,
                              const char *event_type, int64_t seq,
                              const char *data, int64_t ts_us, uint32_t peer)
{
    if (!out || out_sz < 64 || !event_type) return 0;

    /* MCP notifications/message: a JSON-RPC notification (no id).
     * params.level/logger follow the MCP logging shape; params.data
     * carries the structured event so the agent can branch on type. */
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_sz - pos,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
        "\"params\":{\"level\":\"%s\",\"logger\":\"zcl23.event\","
        "\"data\":{\"type\":\"", severity_for(event_type));
    if (pos >= out_sz) return 0;

    append_escaped(out, out_sz, &pos, event_type);
    pos += (size_t)snprintf(out + pos, out_sz - pos,
        "\",\"seq\":%lld,\"ts\":%lld,\"peer\":%u,\"data\":\"",
        (long long)seq, (long long)ts_us, peer);
    if (pos >= out_sz) return 0;

    append_escaped(out, out_sz, &pos, data ? data : "");
    pos += (size_t)snprintf(out + pos, out_sz - pos, "\"}}}");
    if (pos >= out_sz) return 0;
    out[pos] = '\0';
    return pos;
}

/* ── Module state (watermark + counters) ───────────────────────── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t  g_high_water = -1;     /* last seq examined; -1 = nothing */
static bool     g_primed     = false;  /* watermark anchored to "now" yet */
static _Atomic uint64_t g_emitted;

void mcp_notify_reset(int64_t prime_to)
{
    pthread_mutex_lock(&g_lock);
    g_high_water = prime_to;
    g_primed = (prime_to >= 0);
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_emitted, 0);
}

uint64_t mcp_notify_total_emitted(void)
{
    return atomic_load(&g_emitted);
}

int mcp_notify_consider_snapshot(const char *eventlog_body,
                                 mcp_notify_sink_fn sink, void *sink_ctx)
{
    if (!eventlog_body || !sink) return 0;

    struct json_value root;
    if (!json_read(&root, eventlog_body, strlen(eventlog_body)))
        return 0;

    const struct json_value *events = json_get(&root, "events");
    if (!events || events->type != JSON_ARR) {
        json_free(&root);
        return 0;
    }

    int emitted = 0;
    pthread_mutex_lock(&g_lock);

    /* First snapshot ever: anchor the watermark to the newest seq so the
     * agent is not handed a backlog of pre-connection history. From the
     * next snapshot on, only strictly-newer entries are pushed. */
    if (!g_primed) {
        for (size_t i = 0; i < events->num_children; i++) {
            const struct json_value *sv =
                json_get(&events->children[i], "seq");
            if (sv && sv->type == JSON_INT && sv->val.i > g_high_water)
                g_high_water = sv->val.i;
        }
        g_primed = true;
        pthread_mutex_unlock(&g_lock);
        json_free(&root);
        return 0;
    }

    int64_t new_high = g_high_water;
    for (size_t i = 0; i < events->num_children; i++) {
        const struct json_value *ev = &events->children[i];
        const struct json_value *sv = json_get(ev, "seq");
        if (!sv || sv->type != JSON_INT) continue;
        int64_t seq = sv->val.i;
        if (seq <= g_high_water) continue;          /* already examined */
        if (seq > new_high) new_high = seq;

        const char *type = json_get_str(json_get(ev, "type"));
        if (!mcp_notify_is_operator_event(type)) continue;

        const char *data = json_get_str(json_get(ev, "data"));
        const struct json_value *tsv = json_get(ev, "ts");
        const struct json_value *pv  = json_get(ev, "peer");
        int64_t ts = (tsv && tsv->type == JSON_INT) ? tsv->val.i : 0;
        uint32_t peer = (pv && pv->type == JSON_INT) ? (uint32_t)pv->val.i : 0;

        char frame[1024];
        size_t n = mcp_notify_build_frame(frame, sizeof(frame),
                                          type, seq, data, ts, peer);
        if (n > 0) {
            sink(frame, sink_ctx);
            emitted++;
            atomic_fetch_add(&g_emitted, 1);
        }
    }
    g_high_water = new_high;
    pthread_mutex_unlock(&g_lock);

    json_free(&root);
    return emitted;
}

/* ── Background notifier thread ─────────────────────────────────── */

static pthread_t        g_thread;
static bool             g_running;          /* guarded by g_run_lock */
static _Atomic bool     g_stop;
static pthread_mutex_t  g_run_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_run_cv   = PTHREAD_COND_INITIALIZER;

static mcp_notify_fetch_fn g_fetch;
static void               *g_fetch_ctx;
static mcp_notify_sink_fn  g_sink;
static void               *g_sink_ctx;
static unsigned            g_poll_ms = 750;

static void *notify_thread_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        char *body = g_fetch ? g_fetch(g_fetch_ctx) : NULL;
        if (body) {
            mcp_notify_consider_snapshot(body, g_sink, g_sink_ctx);
            free(body);
        }
        /* Sleep on the condvar so stop() wakes us immediately rather
         * than waiting out a full poll interval. */
        pthread_mutex_lock(&g_run_lock);
        if (!atomic_load(&g_stop)) {
            struct timespec deadline;
            platform_time_realtime_timespec(&deadline);
            deadline.tv_sec  += g_poll_ms / 1000;
            deadline.tv_nsec += (long)(g_poll_ms % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec  += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_run_cv, &g_run_lock, &deadline);
        }
        pthread_mutex_unlock(&g_run_lock);
    }
    return NULL;
}

bool mcp_notify_start(mcp_notify_fetch_fn fetch, void *fetch_ctx,
                      mcp_notify_sink_fn sink, void *sink_ctx,
                      unsigned poll_ms)
{
    if (!fetch || !sink) return false;

    pthread_mutex_lock(&g_run_lock);
    if (g_running) {
        pthread_mutex_unlock(&g_run_lock);
        LOG_WARN("mcp.notify", "notifier already running");
        return false;
    }
    g_fetch = fetch;       g_fetch_ctx = fetch_ctx;
    g_sink  = sink;        g_sink_ctx  = sink_ctx;
    g_poll_ms = poll_ms ? poll_ms : 750;
    atomic_store(&g_stop, false);
    /* Watermark unprimed: the first fetch anchors it to the live tip. */
    mcp_notify_reset(-1);

    /* raw-pthread-ok: long-lived notifier joined in mcp_notify_stop() */
    if (pthread_create(&g_thread, NULL, notify_thread_main, NULL) != 0) {
        pthread_mutex_unlock(&g_run_lock);
        LOG_WARN("mcp.notify", "pthread_create failed for notifier");
        return false;
    }
    g_running = true;
    pthread_mutex_unlock(&g_run_lock);
    LOG_INFO("mcp.notify",
             "event push out-channel started (poll=%ums, %zu operator classes)",
             g_poll_ms, sizeof(k_operator_events) / sizeof(k_operator_events[0]));
    return true;
}

void mcp_notify_stop(void)
{
    pthread_mutex_lock(&g_run_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_run_lock);
        return;
    }
    atomic_store(&g_stop, true);
    pthread_cond_broadcast(&g_run_cv);
    pthread_mutex_unlock(&g_run_lock);

    pthread_join(g_thread, NULL);

    pthread_mutex_lock(&g_run_lock);
    g_running = false;
    pthread_mutex_unlock(&g_run_lock);
}

/* ── Live source: the eventlog RPC over the proxy's HTTP client ────
 * This is the 4.1 swap point. Today the proxy has no in-process event
 * ring, so the only event source is a poll of the eventlog RPC. When
 * the MCP loop is hosted on a node thread (4.1), replace this fetch
 * with a direct event_observe()/async subscription that pushes ring
 * entries into mcp_notify_consider_snapshot() without a poll. */
char *mcp_notify_eventlog_fetch(void *ctx)
{
    (void)ctx;
    /* Pull a generous window each poll so a burst between polls is not
     * missed; the seq watermark de-dups. */
    return mcp_node_rpc("eventlog", "[256]");
}
