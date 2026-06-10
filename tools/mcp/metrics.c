/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP Metrics — implementation.  See metrics.h for the contract.
 */

#include "mcp/metrics.h"
#include "event/event.h"
#include "net/peer_scoring.h"
#include "rpc/http_middleware.h"
#include "util/blocker.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Bucket upper bounds in microseconds — matches Prometheus "le" labels.
 * Last bucket (+Inf) is implicit via the _count total. */
static const int64_t k_bucket_us[MCP_METRICS_HIST_BUCKETS] = {
    1000,       /* 1   ms */
    5000,       /* 5   ms */
    25000,      /* 25  ms */
    100000,     /* 100 ms */
    500000,     /* 500 ms */
    2000000,    /* 2000 ms */
};

static const char *k_bucket_le_label[MCP_METRICS_HIST_BUCKETS] = {
    "0.001", "0.005", "0.025", "0.1", "0.5", "2.0",
};

struct counter_entry {
    char     tool[48];
    char     code[24];
    uint64_t count;
};

struct tool_hist {
    char     tool[48];
    uint64_t buckets[MCP_METRICS_HIST_BUCKETS];  /* le bucket counts (non-cumulative) */
    uint64_t overflow;                           /* > last bucket */
    uint64_t total_count;                        /* _count */
    uint64_t total_us;                           /* running sum (for _sum) */
};

static struct counter_entry g_counters[MCP_METRICS_MAX_COUNTERS];
static size_t               g_counter_count;

/* +1 slot reserved for the "__other__" overflow bucket so that when we
 * hit MCP_METRICS_MAX_TOOLS we can still create the fold-in entry. */
static struct tool_hist     g_hists[MCP_METRICS_MAX_TOOLS + 1];
static size_t               g_hist_count;

static uint64_t             g_total_requests;
static uint64_t             g_total_errors;

/* Peer scoring counters.  Bucketed by offence kind so cardinality is
 * bounded by the allowlist below; anything not on the list goes into
 * "other".  Bans are a single counter — splitting them by kind would
 * be misleading because peer_misbehaving() bans on cumulative score,
 * not on the offence that crossed the threshold. */
#define MCP_METRICS_PEER_KINDS 12
static const char *const k_peer_kind_names[MCP_METRICS_PEER_KINDS] = {
    "timeout",
    "invalid_message",
    "flood",
    "invalid_header",
    "invalid_block",
    "unrequested",
    "offer_rejected",
    "invalid_payload",
    "invalid_chunk",
    "invalid_proof",
    "protocol_violation",
    "other",
};
static uint64_t             g_peer_offences[MCP_METRICS_PEER_KINDS];
static uint64_t             g_peer_offences_total;
static uint64_t             g_peer_bans_total;

/* ── Node-level gauges (set by caller, read by prometheus render) ── */
static _Atomic int64_t  g_node_block_height;
static _Atomic int64_t  g_node_peer_count;
static _Atomic int64_t  g_node_rss_mb_x100;   /* fixed-point: RSS_MB * 100 */
static _Atomic int64_t  g_node_utxo_count;
static _Atomic int64_t  g_node_uptime_seconds;
/* Seconds since last EV_BLOCK_CONNECTED, fed by
 * sync_monitor_tip_advance_age() via the lib/metrics tick.
 * Negative means "not yet observed" (cold boot); emitted as -1 so
 * PromQL queries can distinguish bootstrap from a real stall via
 * `> 0` guards. */
static _Atomic int64_t  g_node_tip_advance_age = -1;

/* Mirror lag SLO breach gauges. Set by the metrics tick from the live
 * legacy_mirror_sync_stats snapshot. Pre-bootstrap defaults: lag=-1
 * (mirror not yet attached); seconds=0 (no breach). */
static _Atomic int64_t  g_mirror_lag_blocks = -1;
static _Atomic int64_t  g_mirror_lag_breach_seconds = 0;
static _Atomic int64_t  g_mirror_lag_critical_seconds = 0;

/* Peer-kind gauges. Set by the metrics tick from node_health_snapshot
 * peer classification (subver_is_magicbean / subver_is_zcl23). These
 * are the "Magic Bean reporting" signal — track how many zclassicd-era
 * peers and native zclassic23 peers are connected to us over time. */
static _Atomic int64_t  g_node_magicbean_peer_count;
static _Atomic int64_t  g_node_zcl23_peer_count;

/* Consensus reject registry — bounded (kind, reason) → count table.
 * `kind` is "tx" or "block"; reason is a kebab-case string emitted by
 * the REJECT_IF/UNLESS macros in lib/validation/src/check_*.c.  Beyond
 * `MCP_METRICS_MAX_REJECT_REASONS` distinct (kind, reason) pairs we fold
 * into the per-kind overflow counters so cardinality stays bounded.
 * The per-kind totals are incremented unconditionally — they stay
 * consistent with the sum of slot counts + overflow counts. */
struct reject_reason_slot {
    char     reason[48];
    char     kind[8];  /* "tx" or "block" */
    uint64_t count;
};
static struct reject_reason_slot g_reject_slots[MCP_METRICS_MAX_REJECT_REASONS];
static size_t                    g_reject_slot_count;
static uint64_t                  g_reject_total_tx;
static uint64_t                  g_reject_total_block;
static uint64_t                  g_reject_overflow_tx;
static uint64_t                  g_reject_overflow_block;

static pthread_mutex_t      g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool                 g_observer_installed = false;

/* Sync state gauge (set atomically alongside node gauges) */
static _Atomic int          g_node_sync_state;
static const char          *g_node_sync_state_name = "unknown";

/* ── Parsers ────────────────────────────────────────────────── */

/* Extract a field value from a "key=value ..." payload.  Writes up to
 * cap-1 chars into out.  Stops at space or end.  Returns true if found. */
static bool parse_kv(const char *payload, size_t len, const char *key,
                     char *out, size_t cap)
{
    if (cap == 0) return false;
    out[0] = '\0';
    size_t klen = strlen(key);
    for (size_t i = 0; i < len; ) {
        /* Skip leading spaces */
        while (i < len && payload[i] == ' ') i++;
        /* Match key= */
        if (i + klen + 1 <= len &&
            strncmp(payload + i, key, klen) == 0 &&
            payload[i + klen] == '=') {
            size_t j = i + klen + 1;
            size_t o = 0;
            while (j < len && payload[j] != ' ' && o + 1 < cap) {
                out[o++] = payload[j++];
            }
            out[o] = '\0';
            return true;
        }
        /* Skip token */
        while (i < len && payload[i] != ' ') i++;
    }
    return false;
}

/* ── Registry ops ───────────────────────────────────────────── */

static int hist_slot(const char *tool)
{
    for (size_t i = 0; i < g_hist_count; i++)
        if (strncmp(g_hists[i].tool, tool, sizeof(g_hists[i].tool)) == 0)
            return (int)i;
    if (g_hist_count >= MCP_METRICS_MAX_TOOLS) {
        /* Fold into "__other__" */
        for (size_t i = 0; i < g_hist_count; i++)
            if (strcmp(g_hists[i].tool, "__other__") == 0) return (int)i;
        /* Create it */
        size_t idx = g_hist_count++;
        memset(&g_hists[idx], 0, sizeof(g_hists[idx]));
        snprintf(g_hists[idx].tool, sizeof(g_hists[idx].tool), "%s", "__other__");
        return (int)idx;
    }
    size_t idx = g_hist_count++;
    memset(&g_hists[idx], 0, sizeof(g_hists[idx]));
    snprintf(g_hists[idx].tool, sizeof(g_hists[idx].tool), "%s", tool);
    return (int)idx;
}

static int counter_slot(const char *tool, const char *code)
{
    for (size_t i = 0; i < g_counter_count; i++)
        if (strncmp(g_counters[i].tool, tool, sizeof(g_counters[i].tool)) == 0 &&
            strncmp(g_counters[i].code, code, sizeof(g_counters[i].code)) == 0)
            return (int)i;
    if (g_counter_count >= MCP_METRICS_MAX_COUNTERS) return -1;
    size_t idx = g_counter_count++;
    snprintf(g_counters[idx].tool, sizeof(g_counters[idx].tool), "%s", tool);
    snprintf(g_counters[idx].code, sizeof(g_counters[idx].code), "%s", code);
    g_counters[idx].count = 0;
    return (int)idx;
}

static void record_locked(const char *tool, const char *code, int64_t dur_us)
{
    if (!tool || !tool[0]) tool = "__missing__";
    if (!code || !code[0]) code = "UNKNOWN";

    int ci = counter_slot(tool, code);
    if (ci >= 0) g_counters[ci].count++;

    g_total_requests++;
    if (strcmp(code, "OK") != 0) g_total_errors++;

    if (dur_us >= 0) {
        int hi = hist_slot(tool);
        if (hi >= 0) {
            struct tool_hist *h = &g_hists[hi];
            bool placed = false;
            for (int b = 0; b < MCP_METRICS_HIST_BUCKETS; b++) {
                if (dur_us <= k_bucket_us[b]) {
                    h->buckets[b]++;
                    placed = true;
                    break;
                }
            }
            if (!placed) h->overflow++;
            h->total_count++;
            h->total_us += (uint64_t)dur_us;
        }
    }
}

void mcp_metrics_record(const char *tool, const char *code, int64_t dur_us)
{
    pthread_mutex_lock(&g_lock);
    record_locked(tool, code, dur_us);
    pthread_mutex_unlock(&g_lock);
}

void mcp_metrics_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_counter_count = 0;
    g_hist_count = 0;
    g_total_requests = 0;
    g_total_errors = 0;
    memset(g_counters, 0, sizeof(g_counters));
    memset(g_hists, 0, sizeof(g_hists));
    memset(g_peer_offences, 0, sizeof(g_peer_offences));
    g_peer_offences_total = 0;
    g_peer_bans_total = 0;
    memset(g_reject_slots, 0, sizeof(g_reject_slots));
    g_reject_slot_count = 0;
    g_reject_total_tx = 0;
    g_reject_total_block = 0;
    g_reject_overflow_tx = 0;
    g_reject_overflow_block = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ── Node-level gauge setter ────────────────────────────────── */

void mcp_metrics_set_node_gauges(int64_t block_height, int64_t peer_count,
                                 double rss_mb, int64_t utxo_count,
                                 int64_t uptime_seconds)
{
    atomic_store(&g_node_block_height, block_height);
    atomic_store(&g_node_peer_count, peer_count);
    atomic_store(&g_node_rss_mb_x100, (int64_t)(rss_mb * 100.0));
    atomic_store(&g_node_utxo_count, utxo_count);
    atomic_store(&g_node_uptime_seconds, uptime_seconds);
}

void mcp_metrics_set_tip_advance_age(int64_t seconds)
{
    atomic_store(&g_node_tip_advance_age, seconds);
}

void mcp_metrics_set_mirror_lag(int64_t lag_blocks,
                                int64_t breach_seconds,
                                int64_t critical_seconds)
{
    atomic_store(&g_mirror_lag_blocks, lag_blocks);
    atomic_store(&g_mirror_lag_breach_seconds, breach_seconds);
    atomic_store(&g_mirror_lag_critical_seconds, critical_seconds);
}

void mcp_metrics_set_peer_kinds(int64_t magicbean_count,
                                int64_t zcl23_count)
{
    atomic_store(&g_node_magicbean_peer_count, magicbean_count);
    atomic_store(&g_node_zcl23_peer_count, zcl23_count);
}

/* ── Peer scoring counters ──────────────────────────────────── */

static int peer_kind_slot(const char *kind)
{
    if (!kind || !*kind) return MCP_METRICS_PEER_KINDS - 1; /* "other" */
    for (int i = 0; i < MCP_METRICS_PEER_KINDS; i++) {
        if (strcmp(k_peer_kind_names[i], kind) == 0) return i;
    }
    return MCP_METRICS_PEER_KINDS - 1; /* "other" */
}

void mcp_metrics_record_peer_offence(const char *kind)
{
    pthread_mutex_lock(&g_lock);
    g_peer_offences[peer_kind_slot(kind)]++;
    g_peer_offences_total++;
    pthread_mutex_unlock(&g_lock);
}

void mcp_metrics_record_peer_ban(void)
{
    pthread_mutex_lock(&g_lock);
    g_peer_bans_total++;
    pthread_mutex_unlock(&g_lock);
}

uint64_t mcp_metrics_peer_offences_total(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_peer_offences_total;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_peer_offences_for_kind(const char *kind)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_peer_offences[peer_kind_slot(kind)];
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_peer_bans_total(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_peer_bans_total;
    pthread_mutex_unlock(&g_lock);
    return v;
}

/* ── Consensus reject counters ──────────────────────────────── */

/* Find an existing (kind, reason) slot or create a new one.  Returns
 * -1 when the table is full so the caller can fall back to overflow.
 * Must be called with g_lock held. */
static int reject_slot_locked(const char *kind, const char *reason)
{
    for (size_t i = 0; i < g_reject_slot_count; i++) {
        if (strcmp(g_reject_slots[i].kind, kind) == 0 &&
            strcmp(g_reject_slots[i].reason, reason) == 0)
            return (int)i;
    }
    if (g_reject_slot_count >= MCP_METRICS_MAX_REJECT_REASONS)
        return -1;
    size_t idx = g_reject_slot_count++;
    memset(&g_reject_slots[idx], 0, sizeof(g_reject_slots[idx]));
    snprintf(g_reject_slots[idx].reason,
             sizeof(g_reject_slots[idx].reason), "%s", reason);
    snprintf(g_reject_slots[idx].kind,
             sizeof(g_reject_slots[idx].kind), "%s", kind);
    return (int)idx;
}

void mcp_metrics_record_consensus_reject(const char *kind, const char *reason)
{
    /* Normalise inputs so the table is predictable regardless of
     * observer call path. */
    bool is_block = (kind && strcmp(kind, "block") == 0);
    const char *k = is_block ? "block" : "tx";
    if (!reason || !*reason) reason = "unknown";

    pthread_mutex_lock(&g_lock);
    int idx = reject_slot_locked(k, reason);
    if (idx >= 0) {
        g_reject_slots[idx].count++;
    } else if (is_block) {
        g_reject_overflow_block++;
    } else {
        g_reject_overflow_tx++;
    }
    if (is_block) g_reject_total_block++;
    else          g_reject_total_tx++;
    pthread_mutex_unlock(&g_lock);
}

uint64_t mcp_metrics_consensus_rejects_total(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_reject_total_tx + g_reject_total_block;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_consensus_rejects_for_kind(const char *kind)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = (kind && strcmp(kind, "block") == 0)
                   ? g_reject_total_block : g_reject_total_tx;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_consensus_rejects_tracked_reasons(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = (uint64_t)g_reject_slot_count;
    pthread_mutex_unlock(&g_lock);
    return v;
}

/* ── Event observer ─────────────────────────────────────────── */

static void mcp_request_observer(enum event_type type, uint32_t peer_id,
                                  const void *payload, uint32_t payload_len,
                                  void *ctx)
{
    (void)peer_id; (void)ctx;
    if (type != EV_MCP_REQUEST) return;
    if (!payload || payload_len == 0) return;

    char tool[48], code[24], dur_str[24];
    if (!parse_kv(payload, payload_len, "tool", tool, sizeof(tool))) return;
    if (!parse_kv(payload, payload_len, "code", code, sizeof(code))) return;

    int64_t dur = -1;
    if (parse_kv(payload, payload_len, "dur_us", dur_str, sizeof(dur_str)))
        dur = strtoll(dur_str, NULL, 10);

    mcp_metrics_record(tool, code, dur);
}

/* Extract the offence kind from an EV_PEER_MISBEHAVE / EV_PEER_BANNED
 * payload.  The shapes used by net.c are:
 *   misbehave: "+10=50 invalid_message: bad header"
 *   banned:    "score=100 invalid_block: bad consensus"
 * In both cases the kind is the FIRST whitespace-separated word that
 * (a) is not the score header (no '=' inside) and (b) optionally has
 * a trailing ':'.  We strip the colon and return the bare name. */
static void parse_peer_kind(const char *payload, size_t payload_len,
                             char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    size_t i = 0;
    /* Skip the score header token (the one with '=') */
    while (i < payload_len) {
        size_t tok_start = i;
        while (i < payload_len && payload[i] != ' ') i++;
        size_t tok_len = i - tok_start;
        bool has_eq = false;
        for (size_t j = tok_start; j < tok_start + tok_len; j++) {
            if (payload[j] == '=') { has_eq = true; break; }
        }
        if (!has_eq && tok_len > 0) {
            size_t copy_len = tok_len;
            /* Strip a trailing ':' so we get the bare kind name. */
            if (copy_len > 0 && payload[tok_start + copy_len - 1] == ':')
                copy_len--;
            if (copy_len >= cap) copy_len = cap - 1;
            memcpy(out, payload + tok_start, copy_len);
            out[copy_len] = '\0';
            return;
        }
        while (i < payload_len && payload[i] == ' ') i++;
    }
}

static void mcp_peer_observer(enum event_type type, uint32_t peer_id,
                               const void *payload, uint32_t payload_len,
                               void *ctx)
{
    (void)peer_id; (void)ctx;
    if (type == EV_PEER_MISBEHAVE) {
        char kind[32] = {0};
        if (payload && payload_len > 0)
            parse_peer_kind(payload, payload_len, kind, sizeof(kind));
        mcp_metrics_record_peer_offence(kind);
    } else if (type == EV_PEER_BANNED) {
        mcp_metrics_record_peer_ban();
    }
}

static void mcp_consensus_observer(enum event_type type, uint32_t peer_id,
                                    const void *payload, uint32_t payload_len,
                                    void *ctx)
{
    (void)peer_id; (void)ctx;
    const char *kind;
    if      (type == EV_CONSENSUS_REJECT_TX)    kind = "tx";
    else if (type == EV_CONSENSUS_REJECT_BLOCK) kind = "block";
    else return;

    char reason[48] = {0};
    if (payload && payload_len > 0)
        parse_kv(payload, payload_len, "reason", reason, sizeof(reason));
    mcp_metrics_record_consensus_reject(kind, reason);
}

void mcp_metrics_set_sync_state(int state, const char *name)
{
    atomic_store(&g_node_sync_state, state);
    g_node_sync_state_name = name ? name : "unknown";
}

void mcp_metrics_init(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_observer_installed) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    event_observe(EV_MCP_REQUEST, mcp_request_observer, NULL);
    event_observe(EV_PEER_MISBEHAVE, mcp_peer_observer, NULL);
    event_observe(EV_PEER_BANNED, mcp_peer_observer, NULL);
    event_observe(EV_CONSENSUS_REJECT_TX, mcp_consensus_observer, NULL);
    event_observe(EV_CONSENSUS_REJECT_BLOCK, mcp_consensus_observer, NULL);
    g_observer_installed = true;
    pthread_mutex_unlock(&g_lock);
}

/* ── Introspection ──────────────────────────────────────────── */

size_t mcp_metrics_counter_count(void)
{
    pthread_mutex_lock(&g_lock);
    size_t n = g_counter_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

uint64_t mcp_metrics_get(const char *tool, const char *code)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = 0;
    for (size_t i = 0; i < g_counter_count; i++) {
        if (strcmp(g_counters[i].tool, tool) == 0 &&
            strcmp(g_counters[i].code, code) == 0) {
            v = g_counters[i].count;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_total_requests(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_total_requests;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t mcp_metrics_total_errors(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_total_errors;
    pthread_mutex_unlock(&g_lock);
    return v;
}

/* ── Prometheus text format ─────────────────────────────────── */

__attribute__((format(printf, 4, 5)))
static size_t append(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    if (pos >= cap) return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    if ((size_t)n >= cap - pos) return cap - 1;
    return pos + (size_t)n;
}

size_t mcp_metrics_render_prometheus(char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    pthread_mutex_lock(&g_lock);

    size_t pos = 0;
    pos = append(buf, cap, pos,
        "# HELP zcl_mcp_requests_total MCP requests labeled by tool + code\n"
        "# TYPE zcl_mcp_requests_total counter\n");
    for (size_t i = 0; i < g_counter_count; i++) {
        pos = append(buf, cap, pos,
            "zcl_mcp_requests_total{tool=\"%s\",code=\"%s\"} %llu\n",
            g_counters[i].tool, g_counters[i].code,
            (unsigned long long)g_counters[i].count);
    }

    pos = append(buf, cap, pos,
        "# HELP zcl_mcp_request_duration_seconds Histogram of handler durations\n"
        "# TYPE zcl_mcp_request_duration_seconds histogram\n");
    for (size_t i = 0; i < g_hist_count; i++) {
        const struct tool_hist *h = &g_hists[i];
        uint64_t cumulative = 0;
        for (int b = 0; b < MCP_METRICS_HIST_BUCKETS; b++) {
            cumulative += h->buckets[b];
            pos = append(buf, cap, pos,
                "zcl_mcp_request_duration_seconds_bucket{tool=\"%s\",le=\"%s\"} %llu\n",
                h->tool, k_bucket_le_label[b], (unsigned long long)cumulative);
        }
        cumulative += h->overflow;
        pos = append(buf, cap, pos,
            "zcl_mcp_request_duration_seconds_bucket{tool=\"%s\",le=\"+Inf\"} %llu\n",
            h->tool, (unsigned long long)cumulative);
        pos = append(buf, cap, pos,
            "zcl_mcp_request_duration_seconds_sum{tool=\"%s\"} %.6f\n",
            h->tool, (double)h->total_us / 1000000.0);
        pos = append(buf, cap, pos,
            "zcl_mcp_request_duration_seconds_count{tool=\"%s\"} %llu\n",
            h->tool, (unsigned long long)h->total_count);
    }

    pos = append(buf, cap, pos,
        "# HELP zcl_mcp_requests_summary_total Aggregate MCP request counts\n"
        "# TYPE zcl_mcp_requests_summary_total counter\n"
        "zcl_mcp_requests_summary_total{kind=\"total\"} %llu\n"
        "zcl_mcp_requests_summary_total{kind=\"error\"} %llu\n",
        (unsigned long long)g_total_requests,
        (unsigned long long)g_total_errors);

    pos = append(buf, cap, pos,
        "# HELP zcl_peer_offences_total Peer scoring offences observed since boot\n"
        "# TYPE zcl_peer_offences_total counter\n");
    for (int i = 0; i < MCP_METRICS_PEER_KINDS; i++) {
        pos = append(buf, cap, pos,
            "zcl_peer_offences_total{kind=\"%s\"} %llu\n",
            k_peer_kind_names[i], (unsigned long long)g_peer_offences[i]);
    }
    pos = append(buf, cap, pos,
        "zcl_peer_offences_total{kind=\"all\"} %llu\n",
        (unsigned long long)g_peer_offences_total);

    pos = append(buf, cap, pos,
        "# HELP zcl_peer_bans_total Peers banned (score crossed threshold)\n"
        "# TYPE zcl_peer_bans_total counter\n"
        "zcl_peer_bans_total %llu\n",
        (unsigned long long)g_peer_bans_total);

    /* ── HTTP RPC middleware block ───────────────────────────── */
    struct rpc_http_middleware *rpc_mw = rpc_http_middleware_get_global();
    struct rpc_http_stats_snapshot snap;
    rpc_http_middleware_stats_snapshot(rpc_mw, &snap);

    pos = append(buf, cap, pos,
        "# HELP zcl_rpc_requests_total HTTP RPC middleware decisions by result\n"
        "# TYPE zcl_rpc_requests_total counter\n"
        "zcl_rpc_requests_total{result=\"allowed\"} %llu\n"
        "zcl_rpc_requests_total{result=\"rate_limited_global\"} %llu\n"
        "zcl_rpc_requests_total{result=\"rate_limited_per_ip\"} %llu\n"
        "zcl_rpc_requests_total{result=\"banned\"} %llu\n",
        (unsigned long long)snap.allowed,
        (unsigned long long)snap.rate_limited_global,
        (unsigned long long)snap.rate_limited_per_ip,
        (unsigned long long)snap.banned_rejected);

    pos = append(buf, cap, pos,
        "# HELP zcl_rpc_auth_failures_total HTTP RPC auth (401) failures\n"
        "# TYPE zcl_rpc_auth_failures_total counter\n"
        "zcl_rpc_auth_failures_total %llu\n",
        (unsigned long long)snap.auth_failures);

    pos = append(buf, cap, pos,
        "# HELP zcl_rpc_bans_issued_total IP bans issued by HTTP RPC middleware\n"
        "# TYPE zcl_rpc_bans_issued_total counter\n"
        "zcl_rpc_bans_issued_total %llu\n",
        (unsigned long long)snap.bans_issued);

    pos = append(buf, cap, pos,
        "# HELP zcl_rpc_bans_active Currently-active HTTP RPC IP bans\n"
        "# TYPE zcl_rpc_bans_active gauge\n"
        "zcl_rpc_bans_active %llu\n",
        (unsigned long long)snap.active_bans);

    pos = append(buf, cap, pos,
        "# HELP zcl_rpc_tracked_ips IPs currently in the per-IP bucket table\n"
        "# TYPE zcl_rpc_tracked_ips gauge\n"
        "zcl_rpc_tracked_ips %llu\n",
        (unsigned long long)snap.tracked_ips);

    /* ── Consensus reject block ─────────────────────────────── */
    pos = append(buf, cap, pos,
        "# HELP zcl_consensus_rejects_total Consensus rejects by kind + reason\n"
        "# TYPE zcl_consensus_rejects_total counter\n");
    for (size_t i = 0; i < g_reject_slot_count; i++) {
        pos = append(buf, cap, pos,
            "zcl_consensus_rejects_total{kind=\"%s\",reason=\"%s\"} %llu\n",
            g_reject_slots[i].kind,
            g_reject_slots[i].reason,
            (unsigned long long)g_reject_slots[i].count);
    }
    pos = append(buf, cap, pos,
        "zcl_consensus_rejects_total{kind=\"tx\",reason=\"__other__\"} %llu\n"
        "zcl_consensus_rejects_total{kind=\"block\",reason=\"__other__\"} %llu\n"
        "zcl_consensus_rejects_total{kind=\"tx\",reason=\"all\"} %llu\n"
        "zcl_consensus_rejects_total{kind=\"block\",reason=\"all\"} %llu\n"
        "zcl_consensus_rejects_total{kind=\"all\",reason=\"all\"} %llu\n",
        (unsigned long long)g_reject_overflow_tx,
        (unsigned long long)g_reject_overflow_block,
        (unsigned long long)g_reject_total_tx,
        (unsigned long long)g_reject_total_block,
        (unsigned long long)(g_reject_total_tx + g_reject_total_block));

    /* ── Node-level gauges ───────────────────────────────────── */
    int64_t bh = atomic_load(&g_node_block_height);
    int64_t pc = atomic_load(&g_node_peer_count);
    int64_t rss100 = atomic_load(&g_node_rss_mb_x100);
    int64_t uc = atomic_load(&g_node_utxo_count);
    int64_t up = atomic_load(&g_node_uptime_seconds);
    int ss = atomic_load(&g_node_sync_state);
    const char *ssn = g_node_sync_state_name;

    pos = append(buf, cap, pos,
        "# HELP zcl_block_height Current best chain height\n"
        "# TYPE zcl_block_height gauge\n"
        "zcl_block_height %lld\n",
        (long long)bh);

    pos = append(buf, cap, pos,
        "# HELP zcl_peer_count Connected P2P peers\n"
        "# TYPE zcl_peer_count gauge\n"
        "zcl_peer_count %lld\n",
        (long long)pc);

    int64_t mb_pc = atomic_load(&g_node_magicbean_peer_count);
    int64_t z23_pc = atomic_load(&g_node_zcl23_peer_count);
    pos = append(buf, cap, pos,
        "# HELP zcl_magicbean_peer_count Connected peers identifying as /MagicBean:.../ (zclassicd or compatible)\n"
        "# TYPE zcl_magicbean_peer_count gauge\n"
        "zcl_magicbean_peer_count %lld\n"
        "# HELP zcl_zclassic_c23_peer_count Connected peers identifying as ZClassic-C23 (NODE_ZCL23 services or subver tag)\n"
        "# TYPE zcl_zclassic_c23_peer_count gauge\n"
        "zcl_zclassic_c23_peer_count %lld\n",
        (long long)mb_pc, (long long)z23_pc);

    pos = append(buf, cap, pos,
        "# HELP zcl_rss_mb Resident set size in megabytes\n"
        "# TYPE zcl_rss_mb gauge\n"
        "zcl_rss_mb %.2f\n",
        (double)rss100 / 100.0);

    pos = append(buf, cap, pos,
        "# HELP zcl_utxo_count Number of unspent transaction outputs\n"
        "# TYPE zcl_utxo_count gauge\n"
        "zcl_utxo_count %lld\n",
        (long long)uc);

    pos = append(buf, cap, pos,
        "# HELP zcl_sync_state Sync state (0=idle, 5=at_tip)\n"
        "# TYPE zcl_sync_state gauge\n"
        "zcl_sync_state{name=\"%s\"} %d\n",
        ssn ? ssn : "unknown", ss);

    pos = append(buf, cap, pos,
        "# HELP zcl_uptime_seconds Node uptime in seconds\n"
        "# TYPE zcl_uptime_seconds gauge\n"
        "zcl_uptime_seconds %lld\n",
        (long long)up);

    int64_t tage = atomic_load(&g_node_tip_advance_age);
    pos = append(buf, cap, pos,
        "# HELP zcl_tip_advance_age_seconds Seconds since last EV_BLOCK_CONNECTED (-1 pre-bootstrap)\n"
        "# TYPE zcl_tip_advance_age_seconds gauge\n"
        "zcl_tip_advance_age_seconds %lld\n",
        (long long)tage);

    int64_t mlag = atomic_load(&g_mirror_lag_blocks);
    int64_t mbreach = atomic_load(&g_mirror_lag_breach_seconds);
    int64_t mcrit = atomic_load(&g_mirror_lag_critical_seconds);
    pos = append(buf, cap, pos,
        "# HELP zcl_mirror_lag_blocks zclassic23 block-height lag behind zclassicd (-1 pre-bootstrap)\n"
        "# TYPE zcl_mirror_lag_blocks gauge\n"
        "zcl_mirror_lag_blocks %lld\n"
        "# HELP zcl_mirror_lag_breach_seconds Seconds spent above the breach SLO (0 when under)\n"
        "# TYPE zcl_mirror_lag_breach_seconds gauge\n"
        "zcl_mirror_lag_breach_seconds %lld\n"
        "# HELP zcl_mirror_lag_critical_seconds Seconds spent above the critical SLO (0 when under)\n"
        "# TYPE zcl_mirror_lag_critical_seconds gauge\n"
        "zcl_mirror_lag_critical_seconds %lld\n",
        (long long)mlag, (long long)mbreach, (long long)mcrit);

    /* ── Typed blocker block (Round 6 C5) ──────────────────────
     *
     * Live counts per class + escape-dispatch total. The blocker
     * primitive's own JSON dumper is exposed via the zcl_blockers
     * MCP tool; Prometheus gets the numeric gauges so dashboards can
     * alert on class-level pressure (e.g. permanent>0 is always an
     * operator-escalation event). */
    int active_total = blocker_count_active();
    int active_perm  = blocker_count_by_class(BLOCKER_PERMANENT);
    int active_trans = blocker_count_by_class(BLOCKER_TRANSIENT);
    int active_dep   = blocker_count_by_class(BLOCKER_DEPENDENCY);
    int active_res   = blocker_count_by_class(BLOCKER_RESOURCE);
    int escape_total = blocker_escape_dispatched_count();
    pos = append(buf, cap, pos,
        "# HELP zcl_blockers_active Currently-active typed blockers per class\n"
        "# TYPE zcl_blockers_active gauge\n"
        "zcl_blockers_active{class=\"permanent\"} %d\n"
        "zcl_blockers_active{class=\"transient\"} %d\n"
        "zcl_blockers_active{class=\"dependency\"} %d\n"
        "zcl_blockers_active{class=\"resource\"} %d\n"
        "zcl_blockers_active{class=\"all\"} %d\n"
        "# HELP zcl_blocker_escape_dispatched_total Edge-triggered escape dispatches since boot\n"
        "# TYPE zcl_blocker_escape_dispatched_total counter\n"
        "zcl_blocker_escape_dispatched_total %d\n",
        active_perm, active_trans, active_dep, active_res, active_total,
        escape_total);

    if (pos < cap) buf[pos] = '\0';
    pthread_mutex_unlock(&g_lock);
    return pos;
}

size_t mcp_metrics_peer_report_json(char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    pthread_mutex_lock(&g_lock);

    size_t pos = 0;
    pos = append(buf, cap, pos,
        "{\"config\":{"
            "\"ban_threshold\":%d,"
            "\"ban_hours\":%d,"
            "\"decay_per_min\":%d"
        "},\"offences\":{",
        peer_scoring_ban_threshold(),
        peer_scoring_ban_hours(),
        peer_scoring_decay_rate());

    for (int i = 0; i < MCP_METRICS_PEER_KINDS; i++) {
        pos = append(buf, cap, pos,
            "%s\"%s\":%llu",
            i == 0 ? "" : ",",
            k_peer_kind_names[i],
            (unsigned long long)g_peer_offences[i]);
    }

    pos = append(buf, cap, pos,
        "},\"offences_total\":%llu,\"bans_total\":%llu}",
        (unsigned long long)g_peer_offences_total,
        (unsigned long long)g_peer_bans_total);

    if (pos < cap) buf[pos] = '\0';
    pthread_mutex_unlock(&g_lock);
    return pos;
}

size_t mcp_metrics_rpc_report_json(char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;

    /* NB: we do NOT hold g_lock here — the RPC middleware has its own
     * mutex and we want the snapshot to be consistent with itself, not
     * with the MCP counter registry. */
    struct rpc_http_middleware *mw = rpc_http_middleware_get_global();
    struct rpc_http_stats_snapshot snap;
    rpc_http_middleware_stats_snapshot(mw, &snap);
    const char *server_state = mw ? "active" : "inactive";

    size_t pos = 0;
    pos = append(buf, cap, pos,
        "{\"rpc_server\":\"%s\","
         "\"config\":{"
            "\"global_rps\":%d,"
            "\"global_burst\":%d,"
            "\"per_ip_rps\":%d,"
            "\"per_ip_burst\":%d,"
            "\"auth_fail_threshold\":%d,"
            "\"ban_seconds\":%d"
         "},\"stats\":{"
            "\"allowed\":%llu,"
            "\"rate_limited_global\":%llu,"
            "\"rate_limited_per_ip\":%llu,"
            "\"banned_rejected\":%llu,"
            "\"bans_issued\":%llu,"
            "\"auth_failures\":%llu"
         "},\"tracked_ips\":%zu,\"active_bans\":%zu}",
        server_state,
        snap.global_rps, snap.global_burst,
        snap.per_ip_rps, snap.per_ip_burst,
        snap.auth_fail_threshold, snap.ban_seconds,
        (unsigned long long)snap.allowed,
        (unsigned long long)snap.rate_limited_global,
        (unsigned long long)snap.rate_limited_per_ip,
        (unsigned long long)snap.banned_rejected,
        (unsigned long long)snap.bans_issued,
        (unsigned long long)snap.auth_failures,
        snap.tracked_ips, snap.active_bans);

    if (pos < cap) buf[pos] = '\0';
    return pos;
}

size_t mcp_metrics_consensus_report_json(char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    pthread_mutex_lock(&g_lock);

    size_t pos = 0;
    pos = append(buf, cap, pos,
        "{\"totals\":{"
            "\"tx\":%llu,"
            "\"block\":%llu,"
            "\"all\":%llu"
         "},\"overflow\":{"
            "\"tx\":%llu,"
            "\"block\":%llu"
         "},\"tracked_reasons\":%zu,\"capacity\":%d,\"by_reason\":[",
        (unsigned long long)g_reject_total_tx,
        (unsigned long long)g_reject_total_block,
        (unsigned long long)(g_reject_total_tx + g_reject_total_block),
        (unsigned long long)g_reject_overflow_tx,
        (unsigned long long)g_reject_overflow_block,
        g_reject_slot_count,
        MCP_METRICS_MAX_REJECT_REASONS);

    for (size_t i = 0; i < g_reject_slot_count; i++) {
        pos = append(buf, cap, pos,
            "%s{\"kind\":\"%s\",\"reason\":\"%s\",\"count\":%llu}",
            i == 0 ? "" : ",",
            g_reject_slots[i].kind,
            g_reject_slots[i].reason,
            (unsigned long long)g_reject_slots[i].count);
    }
    pos = append(buf, cap, pos, "]}");

    if (pos < cap) buf[pos] = '\0';
    pthread_mutex_unlock(&g_lock);
    return pos;
}
