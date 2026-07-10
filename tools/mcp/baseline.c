/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP metrics baseline — implementation. See baseline.h for the
 * contract.
 *
 * Capture-at-source vs render-and-diff:
 *
 * metrics.c holds its counters in several bounded internal tables
 * (per-(tool,code) counters, per-tool latency histograms, peer-offence
 * buckets, the RPC middleware snapshot, the consensus-reject
 * (kind,reason) table, and a handful of atomic node-level gauges).
 * There are two ways a baseline module could snapshot that state:
 *
 *   (a) Capture-at-source: walk the same structs metrics.c owns
 *       (struct counter_entry, struct tool_hist, ...) directly.
 *   (b) Capture-the-render: call the ALREADY-PUBLIC
 *       mcp_metrics_render_prometheus() and treat its text output as
 *       the snapshot, diffing generically.
 *
 * (a) means baseline.c has to know, line for line, which internal
 * tables exist and how to walk each one — the same enumeration
 * knowledge mcp_metrics_render_prometheus() already encodes, just
 * duplicated in a second file. Every future metric added to
 * metrics.c (a new gauge, a new bounded table) would need a matching
 * hand-written addition here or it silently stops being diffable.
 * That is exactly the kind of duplicated-knowledge drift the
 * project's DRY law flags.
 *
 * (b) costs one small, fully generic Prometheus-line parser
 * (parse_metric_lines below) that knows nothing about individual
 * metric names — only the wire shape "name{labels} value" that EVERY
 * line render_prometheus emits already follows. Any metric that
 * exists today, or is added to metrics.c next month, is automatically
 * captured and diffable with zero changes to this file. That is the
 * "does not duplicate metric-enumeration knowledge" option, so it is
 * the one implemented here. lib/json is still used, just for building
 * the *output* envelope (list/diff bodies) rather than for parsing
 * the capture — the capture format is text because that is what the
 * single existing source of truth (mcp_metrics_render_prometheus)
 * already emits; wrapping it in a parallel JSON exporter would be
 * exactly the duplicated enumeration this design avoids.
 */

#include "platform/time_compat.h"
#include "baseline.h"
#include "metrics.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mcp_baseline_entry {
    char     label[MCP_BASELINE_LABEL_MAX];
    uint64_t timestamp_us;
    uint64_t seq;              /* monotonic, breaks timestamp ties */
    char     text[MCP_BASELINE_SNAPSHOT_CAP];
    size_t   text_len;
    bool     used;
};

static struct mcp_baseline_entry g_ring[MCP_BASELINE_RING_SIZE];
static size_t              g_head;   /* next write slot (mod RING_SIZE) */
static uint64_t            g_seq;    /* monotonic capture counter */
static pthread_mutex_t     g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_us(void)
{
    struct timespec ts;
    platform_time_realtime_timespec(&ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

void mcp_baseline_init(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_ring, 0, sizeof(g_ring));
    g_head = 0;
    g_seq = 0;
    pthread_mutex_unlock(&g_lock);
}

bool mcp_baseline_set(const char *label, char *out_label,
                      size_t out_label_cap, uint64_t *out_timestamp_us)
{
    pthread_mutex_lock(&g_lock);

    struct mcp_baseline_entry *e = &g_ring[g_head % MCP_BASELINE_RING_SIZE];
    g_seq++;

    if (label && label[0])
        snprintf(e->label, sizeof(e->label), "%s", label);
    else
        snprintf(e->label, sizeof(e->label), "b%llu",
                 (unsigned long long)g_seq);

    e->timestamp_us = now_us();
    e->seq = g_seq;
    e->text_len = mcp_metrics_render_prometheus(e->text, sizeof(e->text));
    e->used = true;

    if (out_label && out_label_cap)
        snprintf(out_label, out_label_cap, "%s", e->label);
    if (out_timestamp_us)
        *out_timestamp_us = e->timestamp_us;

    char label_copy[MCP_BASELINE_LABEL_MAX];
    snprintf(label_copy, sizeof(label_copy), "%s", e->label);
    bool ok = e->text_len > 0;

    g_head++;
    pthread_mutex_unlock(&g_lock);

    if (!ok)
        LOG_FAIL("mcp.baseline",
                 "metrics render returned empty for label=%s", label_copy);
    return true;
}

size_t mcp_baseline_count(void)
{
    pthread_mutex_lock(&g_lock);
    size_t n = 0;
    for (size_t i = 0; i < MCP_BASELINE_RING_SIZE; i++)
        if (g_ring[i].used) n++;
    pthread_mutex_unlock(&g_lock);
    return n;
}

int mcp_baseline_find(const char *label)
{
    if (!label || !label[0]) return -1;  // raw-return-ok:sentinel
    pthread_mutex_lock(&g_lock);
    int best = -1;
    uint64_t best_seq = 0;
    for (size_t i = 0; i < MCP_BASELINE_RING_SIZE; i++) {
        if (!g_ring[i].used) continue;
        if (strcmp(g_ring[i].label, label) != 0) continue;
        if (best < 0 || g_ring[i].seq > best_seq) {
            best = (int)i;
            best_seq = g_ring[i].seq;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return best;  // raw-return-ok:sentinel
}

int mcp_baseline_find_nearest_before(uint64_t since_us)
{
    pthread_mutex_lock(&g_lock);
    int best = -1;
    uint64_t best_ts = 0, best_seq = 0;
    for (size_t i = 0; i < MCP_BASELINE_RING_SIZE; i++) {
        if (!g_ring[i].used) continue;
        if (g_ring[i].timestamp_us > since_us) continue;
        if (best < 0 || g_ring[i].timestamp_us > best_ts ||
            (g_ring[i].timestamp_us == best_ts && g_ring[i].seq > best_seq)) {
            best = (int)i;
            best_ts = g_ring[i].timestamp_us;
            best_seq = g_ring[i].seq;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return best;  // raw-return-ok:sentinel
}

int mcp_baseline_latest(void)
{
    pthread_mutex_lock(&g_lock);
    int best = -1;
    uint64_t best_seq = 0;
    for (size_t i = 0; i < MCP_BASELINE_RING_SIZE; i++) {
        if (!g_ring[i].used) continue;
        if (best < 0 || g_ring[i].seq > best_seq) {
            best = (int)i;
            best_seq = g_ring[i].seq;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return best;  // raw-return-ok:sentinel
}

char *mcp_baseline_list_json(void)
{
    pthread_mutex_lock(&g_lock);

    size_t idxs[MCP_BASELINE_RING_SIZE];
    size_t n = 0;
    for (size_t i = 0; i < MCP_BASELINE_RING_SIZE; i++)
        if (g_ring[i].used) idxs[n++] = i;

    /* Insertion sort by seq ascending (oldest first) — n <= 16. */
    for (size_t i = 1; i < n; i++) {
        size_t key = idxs[i];
        uint64_t key_seq = g_ring[key].seq;
        size_t j = i;
        while (j > 0 && g_ring[idxs[j - 1]].seq > key_seq) {
            idxs[j] = idxs[j - 1];
            j--;
        }
        idxs[j] = key;
    }

    uint64_t t_now = now_us();

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t k = 0; k < n; k++) {
        struct mcp_baseline_entry *e = &g_ring[idxs[k]];
        struct json_value item;
        json_init(&item);
        json_set_object(&item);
        json_push_kv_str(&item, "label", e->label);
        json_push_kv_int(&item, "timestamp_us", (int64_t)e->timestamp_us);
        int64_t age_s = t_now > e->timestamp_us
            ? (int64_t)((t_now - e->timestamp_us) / 1000000ULL) : 0;
        json_push_kv_int(&item, "age_seconds", age_s);
        json_push_kv_int(&item, "bytes", (int64_t)e->text_len);
        json_push_back(&arr, &item);
        json_free(&item);
    }
    pthread_mutex_unlock(&g_lock);

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_int(&root, "count", (int64_t)n);
    json_push_kv_int(&root, "ring_size", MCP_BASELINE_RING_SIZE);
    json_push_kv(&root, "baselines", &arr);
    json_free(&arr);

    size_t need = json_write(&root, NULL, 0);
    char *out = zcl_malloc(need + 1, "baseline_list_json");
    if (!out) {
        json_free(&root);
        LOG_NULL("mcp.baseline",
                 "malloc failed for baseline list response (%zu bytes)",
                 need + 1);
    }
    json_write(&root, out, need + 1);
    json_free(&root);
    return out;
}

/* ── Generic Prometheus-line parsing + diff ─────────────────────── */

struct metric_kv {
    const char *key;
    size_t      key_len;
    double      value;
};

#define MCP_BASELINE_MAX_METRIC_LINES 4096
#define MCP_BASELINE_METRIC_KEY_MAX   320

/* Split "name{labels} value" / "name value" lines. Skips blank lines
 * and "#"-prefixed HELP/TYPE comment lines. Knows nothing about which
 * metric names exist — see the file header for why that matters. */
static size_t parse_metric_lines(const char *text, size_t text_len,
                                  struct metric_kv *out, size_t out_cap)
{
    size_t n = 0;
    const char *p = text;
    const char *end = text + text_len;

    while (p < end && n < out_cap) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (line_len > 0 && p[0] != '#') {
            long sp = -1;
            for (size_t i = line_len; i > 0; i--) {
                if (p[i - 1] == ' ') { sp = (long)(i - 1); break; }
            }
            if (sp > 0) {
                const char *val = p + sp + 1;
                size_t val_len = line_len - (size_t)sp - 1;
                if (val_len > 0 && val_len < 40) {
                    char valbuf[40];
                    memcpy(valbuf, val, val_len);
                    valbuf[val_len] = '\0';
                    char *endptr = NULL;
                    double v = strtod(valbuf, &endptr);
                    if (endptr != valbuf) {
                        out[n].key = p;
                        out[n].key_len = (size_t)sp;
                        out[n].value = v;
                        n++;
                    }
                }
            }
        }
        p = nl ? nl + 1 : end;
    }
    return n;
}

/* Emit v as JSON_INT when it round-trips exactly (the overwhelming
 * majority of Prometheus lines here are integer counters/gauges);
 * fall back to JSON_REAL for genuine fractional values (rss_mb,
 * histogram _sum). Keeps "delta":42 instead of "delta":42.00000000. */
static void push_number(struct json_value *obj, const char *key, double v)
{
    if (fabs(v) < 1e15 && v == (double)(int64_t)v)
        json_push_kv_int(obj, key, (int64_t)v);
    else
        json_push_kv_real(obj, key, v);
}

static void push_metric_delta(struct json_value *arr, const char *key,
                              size_t key_len, double from, double to,
                              double delta)
{
    char keybuf[MCP_BASELINE_METRIC_KEY_MAX];
    size_t n = key_len < sizeof(keybuf) - 1 ? key_len : sizeof(keybuf) - 1;
    memcpy(keybuf, key, n);
    keybuf[n] = '\0';

    struct json_value item;
    json_init(&item);
    json_set_object(&item);
    json_push_kv_str(&item, "metric", keybuf);
    push_number(&item, "from", from);
    push_number(&item, "to", to);
    push_number(&item, "delta", delta);
    json_push_back(arr, &item);
    json_free(&item);
}

char *mcp_baseline_diff_json(int idx)
{
    if (idx < 0 || idx >= MCP_BASELINE_RING_SIZE) return NULL;  // raw-return-ok:sentinel

    pthread_mutex_lock(&g_lock);
    struct mcp_baseline_entry *e = &g_ring[idx];
    if (!e->used) {
        pthread_mutex_unlock(&g_lock);
        return NULL;  // raw-return-ok:sentinel
    }

    char label_copy[MCP_BASELINE_LABEL_MAX];
    snprintf(label_copy, sizeof(label_copy), "%s", e->label);
    uint64_t base_ts = e->timestamp_us;
    size_t base_len = e->text_len;

    char *base_text = zcl_malloc(base_len + 1, "baseline_diff_base_copy");
    if (!base_text) {
        pthread_mutex_unlock(&g_lock);
        LOG_NULL("mcp.baseline",
                 "malloc failed copying baseline text (%zu bytes) label=%s",
                 base_len + 1, label_copy);
    }
    memcpy(base_text, e->text, base_len);
    base_text[base_len] = '\0';
    pthread_mutex_unlock(&g_lock);

    /* The "now" side of the diff: render the live snapshot fresh. */
    char *curr_text = zcl_malloc(MCP_BASELINE_SNAPSHOT_CAP, "baseline_diff_curr");
    if (!curr_text) {
        free(base_text);
        LOG_NULL("mcp.baseline",
                 "malloc failed for live metrics snapshot (%d bytes)",
                 MCP_BASELINE_SNAPSHOT_CAP);
    }
    size_t curr_len = mcp_metrics_render_prometheus(curr_text,
                                                     MCP_BASELINE_SNAPSHOT_CAP);

    struct metric_kv *base_kv = zcl_malloc(
        sizeof(struct metric_kv) * MCP_BASELINE_MAX_METRIC_LINES,
        "baseline_diff_base_kv");
    struct metric_kv *curr_kv = zcl_malloc(
        sizeof(struct metric_kv) * MCP_BASELINE_MAX_METRIC_LINES,
        "baseline_diff_curr_kv");
    if (!base_kv || !curr_kv) {
        free(base_text); free(curr_text); free(base_kv); free(curr_kv);
        LOG_NULL("mcp.baseline",
                 "malloc failed for metric-line scratch arrays");
    }

    size_t base_n = parse_metric_lines(base_text, base_len, base_kv,
                                       MCP_BASELINE_MAX_METRIC_LINES);
    size_t curr_n = parse_metric_lines(curr_text, curr_len, curr_kv,
                                       MCP_BASELINE_MAX_METRIC_LINES);

    struct json_value changed;
    json_init(&changed);
    json_set_array(&changed);
    size_t changed_count = 0;

    /* Pass 1: every baseline metric — matched (changed if delta != 0)
     * or dropped (present at baseline time, absent now). */
    for (size_t i = 0; i < base_n; i++) {
        bool found = false;
        for (size_t j = 0; j < curr_n; j++) {
            if (base_kv[i].key_len == curr_kv[j].key_len &&
                memcmp(base_kv[i].key, curr_kv[j].key,
                       base_kv[i].key_len) == 0) {
                found = true;
                double delta = curr_kv[j].value - base_kv[i].value;
                if (fabs(delta) > 1e-9) {
                    push_metric_delta(&changed, base_kv[i].key,
                                      base_kv[i].key_len,
                                      base_kv[i].value, curr_kv[j].value,
                                      delta);
                    changed_count++;
                }
                break;
            }
        }
        if (!found && fabs(base_kv[i].value) > 1e-9) {
            push_metric_delta(&changed, base_kv[i].key, base_kv[i].key_len,
                              base_kv[i].value, 0.0, -base_kv[i].value);
            changed_count++;
        }
    }

    /* Pass 2: metrics that exist now but didn't at baseline time
     * (e.g. a tool called for the first time since the baseline). */
    for (size_t j = 0; j < curr_n; j++) {
        bool found = false;
        for (size_t i = 0; i < base_n; i++) {
            if (curr_kv[j].key_len == base_kv[i].key_len &&
                memcmp(curr_kv[j].key, base_kv[i].key,
                       curr_kv[j].key_len) == 0) {
                found = true;
                break;
            }
        }
        if (!found && fabs(curr_kv[j].value) > 1e-9) {
            push_metric_delta(&changed, curr_kv[j].key, curr_kv[j].key_len,
                              0.0, curr_kv[j].value, curr_kv[j].value);
            changed_count++;
        }
    }

    free(base_kv);
    free(curr_kv);
    free(base_text);
    free(curr_text);

    uint64_t t_now = now_us();
    int64_t age_s = t_now > base_ts
        ? (int64_t)((t_now - base_ts) / 1000000ULL) : 0;

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    json_push_kv_str(&root, "label", label_copy);
    json_push_kv_int(&root, "baseline_timestamp_us", (int64_t)base_ts);
    json_push_kv_int(&root, "age_seconds", age_s);
    json_push_kv_int(&root, "changed_count", (int64_t)changed_count);
    json_push_kv(&root, "changed", &changed);
    json_free(&changed);

    size_t need = json_write(&root, NULL, 0);
    char *out = zcl_malloc(need + 1, "baseline_diff_json");
    if (!out) {
        json_free(&root);
        LOG_NULL("mcp.baseline",
                 "malloc failed for baseline diff response (%zu bytes)",
                 need + 1);
    }
    json_write(&root, out, need + 1);
    json_free(&root);
    return out;
}
