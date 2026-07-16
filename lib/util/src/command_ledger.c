/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Command interaction ledger — the agent flight recorder (Phase D).
 * Contract + design invariants: util/command_ledger.h.
 *
 * The hot path (command_ledger_sink) takes NO lock: each record renders to
 * well under PIPE_BUF, so an O_APPEND write() is kernel-atomic even across
 * concurrent fresh-process CLI writers. Rotation (rare) is serialized by an
 * advisory flock on a sidecar lock file; the in-process append fd is a fixed
 * descriptor number kept in sync with the current file via dup2, so a
 * concurrent same-process writer never writes into a rotated-away inode.
 */

#define _GNU_SOURCE
#include "util/command_ledger.h"

#include "json/json.h"
#include "kernel/command_registry.h"
#include "platform/clock.h"
#include "util/log_json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define LEDGER_DEFAULT_MAX_BYTES (8 * 1024 * 1024) /* 8 MiB cap -> rotate */
#define LEDGER_LINE_MAX 1024                       /* one record, < PIPE_BUF */
#define LEDGER_P99_SCAN_BYTES (256 * 1024)         /* tail scanned for p99 */
#define LEDGER_QUERY_SCAN_BYTES (4 * 1024 * 1024)  /* tail scanned for query */
#define LEDGER_P99_MAX_SAMPLES 4096U
#define LEDGER_SUMMARY_MAX_LEAVES 64U
#define LEDGER_SUMMARY_MAX_SAMPLES 256U

/* ── Module state (single ledger per process) ───────────────────────────── */
static int g_fd = -1;      /* fixed append fd (kept current via dup2) */
static int g_lockfd = -1;  /* advisory rotation lock */
static char g_path[600];   /* current ndjson */
static char g_path1[608];  /* rotated .1 */
static char g_gap[620];    /* durable retention-gap marker */
static char g_lockpath[620];
static bool g_installed;
static int64_t g_max_bytes = LEDGER_DEFAULT_MAX_BYTES;
static _Atomic uint64_t g_records;   /* records this process appended */
static _Atomic uint64_t g_rotations; /* rotations this process performed */

/* ── Small helpers ──────────────────────────────────────────────────────── */
static const char *transport_name(enum zcl_cmd_transport t)
{
    switch (t) {
    case ZCL_CMD_TRANSPORT_NATIVE: return "native";
    case ZCL_CMD_TRANSPORT_RPC:    return "rpc";
    case ZCL_CMD_TRANSPORT_REST:   return "rest";
    case ZCL_CMD_TRANSPORT_MCP:    return "mcp";
    }
    return "native";
}

static bool ledger_retention_gap(void)
{
    return g_installed && g_gap[0] && access(g_gap, F_OK) == 0;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int64_t percentile_us(int64_t *v, size_t n, int pct)
{
    if (n == 0)
        return 0;
    qsort(v, n, sizeof(int64_t), cmp_i64);
    size_t idx = (size_t)((n - 1) * (size_t)pct / 100);
    return v[idx];
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */
void command_ledger_test_set_cap(int64_t max_bytes)
{
    g_max_bytes = max_bytes > 0 ? max_bytes : LEDGER_DEFAULT_MAX_BYTES;
}

void command_ledger_uninstall(void)
{
    zcl_command_registry_set_ledger_sink(NULL);
    zcl_command_registry_set_latency_source(NULL);
    if (g_fd >= 0)
        close(g_fd);
    if (g_lockfd >= 0)
        close(g_lockfd);
    g_fd = -1;
    g_lockfd = -1;
    g_installed = false;
}

bool command_ledger_install(const char *datadir)
{
    if (!datadir || !datadir[0])
        LOG_FAIL("command_ledger", "null/empty datadir; ledger not installed");

    /* A re-install re-points at a fresh datadir (used by tests). */
    command_ledger_uninstall();

    char dir[560];
    int n = snprintf(dir, sizeof(dir), "%s/telemetry", datadir);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        LOG_FAIL("command_ledger", "datadir too long: %zu", strlen(datadir));

    /* Best-effort mkdir of the datadir then telemetry/ (EEXIST is success). */
    if (mkdir(datadir, 0700) != 0 && errno != EEXIST)
        LOG_FAIL("command_ledger", "mkdir datadir failed: %s", strerror(errno));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        LOG_FAIL("command_ledger", "mkdir telemetry failed: %s",
                 strerror(errno));

    (void)snprintf(g_path, sizeof(g_path), "%s/command_ledger.ndjson", dir);
    (void)snprintf(g_path1, sizeof(g_path1), "%s.1", g_path);
    (void)snprintf(g_gap, sizeof(g_gap), "%s/command_ledger.gap", dir);
    (void)snprintf(g_lockpath, sizeof(g_lockpath), "%s/command_ledger.lock",
                   dir);

    g_fd = open(g_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (g_fd < 0)
        LOG_FAIL("command_ledger", "open ledger failed: %s", strerror(errno));
    g_lockfd = open(g_lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_lockfd < 0) {
        close(g_fd);
        g_fd = -1;
        LOG_FAIL("command_ledger", "open lock failed: %s", strerror(errno));
    }

    g_installed = true;
    zcl_command_registry_set_ledger_sink(command_ledger_sink);
    zcl_command_registry_set_latency_source(command_ledger_p99);
    return true;
}

/* ── Rotation (rare; flock-serialized) ──────────────────────────────────── */
static void ledger_rotate(void)
{
    if (g_lockfd < 0 || g_fd < 0)
        return;
    if (flock(g_lockfd, LOCK_EX) != 0)
        return; /* best-effort: skip rotation this time */

    struct stat st;
    bool cur_big = stat(g_path, &st) == 0 && st.st_size >= g_max_bytes;
    if (cur_big) {
        /* Overwriting an existing .1 drops a whole generation -> durable gap. */
        struct stat s1;
        if (stat(g_path1, &s1) == 0) {
            int gf = open(g_gap, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (gf >= 0)
                close(gf);
        }
        (void)rename(g_path, g_path1);
        atomic_fetch_add_explicit(&g_rotations, 1, memory_order_relaxed);
    }
    /* Re-sync the fixed append fd to the current file (self-heals when another
     * process rotated it out from under us). dup2 is an atomic in-process
     * swap, so concurrent writers keep the same fd number. */
    int fresh = open(g_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fresh >= 0) {
        (void)dup2(fresh, g_fd);
        close(fresh);
    }
    flock(g_lockfd, LOCK_UN);
}

/* ── Sink (hot path) ────────────────────────────────────────────────────── */
void command_ledger_sink(const struct zcl_command_ledger_record *r)
{
    if (!r || !g_installed || g_fd < 0)
        return;

    char leaf[ZCL_COMMAND_MAX_PATH];
    char code[80];
    log_json_escape(leaf, sizeof(leaf), r->leaf ? r->leaf : "");
    log_json_escape(code, sizeof(code), r->code);

    char line[LEDGER_LINE_MAX];
    int len = snprintf(
        line, sizeof(line),
        "{\"schema\":\"zcl.cmd_ledger.v1\",\"ts_unix_ms\":%lld,\"seq\":%llu,"
        "\"leaf\":\"%s\",\"transport\":\"%s\",\"input_bytes\":%lld,"
        "\"output_bytes\":%lld,\"budget_bytes\":%lld,\"budget_exceeded\":%s,"
        "\"elapsed_us\":%lld,\"budget_ms\":%lld,\"latency_class\":\"%s\","
        "\"ok\":%s,\"code\":\"%s\",\"request_id\":\"%s\"}\n",
        (long long)r->ts_unix_ms, (unsigned long long)r->seq, leaf,
        transport_name(r->transport), (long long)r->input_bytes,
        (long long)r->output_bytes, (long long)r->budget_bytes,
        r->budget_exceeded ? "true" : "false", (long long)r->elapsed_us,
        (long long)r->budget_ms, zcl_command_latency_name(r->latency_class),
        r->ok ? "true" : "false", code, r->request_id);
    if (len <= 0 || (size_t)len >= sizeof(line))
        LOG_WARN("command_ledger", "record render overflow (leaf=%s)", leaf);

    /* Rotate BEFORE writing when the current file is at the cap, so the file
     * never grows more than one record past g_max_bytes. */
    struct stat st;
    if (fstat(g_fd, &st) == 0 && st.st_size + (off_t)len > g_max_bytes)
        ledger_rotate();

    ssize_t w = write(g_fd, line, (size_t)len); /* raw-io-ok:command-ledger */
    if (w != (ssize_t)len)
        LOG_WARN("command_ledger", "short/failed append: %zd/%d (%s)", w, len,
                 strerror(errno));
    else
        atomic_fetch_add_explicit(&g_records, 1, memory_order_relaxed);

    /* One structured dispatch line, mirroring the per-request event the MCP
     * router emitted (tools/mcp/router.c). Goes to stderr via LogPrintStr, so
     * it never pollutes a CLI's stdout result document. */
    log_jsonf(LOG_JSON_INFO, "command_dispatch",
              "\"leaf\":\"%s\",\"transport\":\"%s\",\"ok\":%s,\"code\":\"%s\","
              "\"elapsed_us\":%lld,\"budget_ms\":%lld,\"budget_exceeded\":%s,"
              "\"input_bytes\":%lld,\"output_bytes\":%lld,\"seq\":%llu,"
              "\"request_id\":\"%s\"",
              leaf, transport_name(r->transport), r->ok ? "true" : "false",
              code, (long long)r->elapsed_us, (long long)r->budget_ms,
              r->budget_exceeded ? "true" : "false", (long long)r->input_bytes,
              (long long)r->output_bytes, (unsigned long long)r->seq,
              r->request_id);
}

/* ── Bounded tail scan (shared by p99 / summary / tail) ─────────────────── */
/* Read up to `want` bytes from the END of `path`. Returns a malloc'd buffer
 * (caller frees) with *len bytes and *partial set when the read started
 * mid-file (so the first line may be truncated and must be skipped). */
static char *read_tail(const char *path, size_t want, size_t *len,
                       bool *partial)
{
    *len = 0;
    *partial = false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    off_t size = st.st_size;
    off_t off = (off_t)want < size ? size - (off_t)want : 0;
    size_t n = (size_t)(size - off);
    char *buf = zcl_malloc(n + 1, "command_ledger tail scan");
    if (!buf) {
        close(fd);
        return NULL;
    }
    ssize_t got = pread(fd, buf, n, off);
    close(fd);
    if (got <= 0) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    *len = (size_t)got;
    *partial = off > 0;
    return buf;
}

typedef void (*ledger_visit_fn)(const char *raw, size_t rawlen,
                                const struct json_value *rec, void *ctx);

/* Scan the older (.1) then the current file, oldest -> newest, invoking
 * `visit` on every well-formed record line. */
static void ledger_scan(size_t want, ledger_visit_fn visit, void *ctx)
{
    const char *paths[2] = { g_path1, g_path };
    for (int fi = 0; fi < 2; fi++) {
        size_t len = 0;
        bool partial = false;
        char *buf = read_tail(paths[fi], want, &len, &partial);
        if (!buf)
            continue;
        size_t i = 0;
        if (partial) {
            while (i < len && buf[i] != '\n')
                i++;
            if (i < len)
                i++;
        }
        while (i < len) {
            size_t start = i;
            while (i < len && buf[i] != '\n')
                i++;
            size_t line_len = i - start;
            if (i < len)
                i++;
            if (line_len == 0 || line_len >= LEDGER_LINE_MAX)
                continue;
            struct json_value rec;
            json_init(&rec);
            if (json_read(&rec, buf + start, line_len) && rec.type == JSON_OBJ)
                visit(buf + start, line_len, &rec, ctx);
            json_free(&rec);
        }
        free(buf);
    }
}

static int64_t rec_i64(const struct json_value *rec, const char *k)
{
    return json_get_int(json_get(rec, k));
}
static const char *rec_str(const struct json_value *rec, const char *k)
{
    const char *s = json_get_str(json_get(rec, k));
    return s ? s : "";
}

/* ── p99 (latency source) ───────────────────────────────────────────────── */
struct p99_ctx {
    const char *leaf;
    int64_t window_lo_ms; /* < 0 == no lower bound */
    int64_t *samp;
    size_t cap;
    size_t n;
};
static void p99_visit(const char *raw, size_t rawlen,
                      const struct json_value *rec, void *vctx)
{
    (void)raw;
    (void)rawlen;
    struct p99_ctx *c = vctx;
    if (c->n >= c->cap)
        return;
    if (strcmp(rec_str(rec, "leaf"), c->leaf) != 0)
        return;
    if (c->window_lo_ms >= 0 && rec_i64(rec, "ts_unix_ms") < c->window_lo_ms)
        return;
    c->samp[c->n++] = rec_i64(rec, "elapsed_us");
}

bool command_ledger_p99(const char *leaf, int64_t window_s, int64_t *p99_us,
                        uint32_t *samples)
{
    if (p99_us)
        *p99_us = 0;
    if (samples)
        *samples = 0;
    if (!g_installed || !leaf || !leaf[0])
        return false;
    int64_t *buf = zcl_malloc(LEDGER_P99_MAX_SAMPLES * sizeof(int64_t),
                              "command_ledger p99 samples");
    if (!buf)
        LOG_FAIL("command_ledger", "p99 sample alloc failed");
    struct p99_ctx c = {
        .leaf = leaf,
        .window_lo_ms = window_s > 0 ? clock_now_wall_ms() - window_s * 1000
                                     : -1,
        .samp = buf,
        .cap = LEDGER_P99_MAX_SAMPLES,
        .n = 0,
    };
    ledger_scan(LEDGER_P99_SCAN_BYTES, p99_visit, &c);
    bool have = c.n > 0;
    if (have) {
        if (p99_us)
            *p99_us = percentile_us(buf, c.n, 99);
        if (samples)
            *samples = (uint32_t)c.n;
    }
    free(buf);
    return have;
}

/* ── Summary ────────────────────────────────────────────────────────────── */
struct leaf_agg {
    char leaf[ZCL_COMMAND_MAX_PATH];
    uint64_t calls;
    uint64_t errors;
    uint64_t budget_exceeded;
    uint64_t output_bytes_sum;
    int64_t elapsed[LEDGER_SUMMARY_MAX_SAMPLES];
    size_t nsamp;
};
struct summary_ctx {
    struct leaf_agg *aggs;
    size_t nleaves;
    const char *filter; /* NULL/"" == all */
    int64_t window_lo_ms;
    int64_t oldest_ms;
    int64_t newest_ms;
    uint64_t total_calls;
};
static void summary_visit(const char *raw, size_t rawlen,
                          const struct json_value *rec, void *vctx)
{
    (void)raw;
    (void)rawlen;
    struct summary_ctx *c = vctx;
    const char *leaf = rec_str(rec, "leaf");
    if (!leaf[0])
        return;
    if (c->filter && c->filter[0] && strcmp(leaf, c->filter) != 0)
        return;
    int64_t ts = rec_i64(rec, "ts_unix_ms");
    if (c->window_lo_ms >= 0 && ts < c->window_lo_ms)
        return;
    if (c->oldest_ms == 0 || ts < c->oldest_ms)
        c->oldest_ms = ts;
    if (ts > c->newest_ms)
        c->newest_ms = ts;
    c->total_calls++;
    struct leaf_agg *a = NULL;
    for (size_t i = 0; i < c->nleaves; i++)
        if (strcmp(c->aggs[i].leaf, leaf) == 0) {
            a = &c->aggs[i];
            break;
        }
    if (!a) {
        if (c->nleaves >= LEDGER_SUMMARY_MAX_LEAVES)
            return; /* distinct-leaf cap reached; drop the tail */
        a = &c->aggs[c->nleaves++];
        (void)snprintf(a->leaf, sizeof(a->leaf), "%s", leaf);
    }
    a->calls++;
    if (!json_get_bool(json_get(rec, "ok")))
        a->errors++;
    if (json_get_bool(json_get(rec, "budget_exceeded")))
        a->budget_exceeded++;
    a->output_bytes_sum += (uint64_t)rec_i64(rec, "output_bytes");
    if (a->nsamp < LEDGER_SUMMARY_MAX_SAMPLES)
        a->elapsed[a->nsamp++] = rec_i64(rec, "elapsed_us");
}

bool command_ledger_summary(int64_t window_s, const char *leaf_filter, int top,
                            struct json_value *out)
{
    if (!out)
        return false;
    if (top < 1)
        top = 10;
    if (top > 50)
        top = 50;
    struct summary_ctx c;
    memset(&c, 0, sizeof(c));
    c.aggs = zcl_calloc(LEDGER_SUMMARY_MAX_LEAVES, sizeof(struct leaf_agg),
                        "command_ledger summary aggs");
    if (!c.aggs)
        LOG_FAIL("command_ledger", "summary agg alloc failed");
    c.filter = leaf_filter;
    c.window_lo_ms =
        window_s > 0 ? clock_now_wall_ms() - window_s * 1000 : -1;

    if (g_installed)
        ledger_scan(LEDGER_QUERY_SCAN_BYTES, summary_visit, &c);

    /* Rank leaves by call count (descending), tie-broken by name. */
    for (size_t i = 0; i < c.nleaves; i++)
        for (size_t j = i + 1; j < c.nleaves; j++)
            if (c.aggs[j].calls > c.aggs[i].calls ||
                (c.aggs[j].calls == c.aggs[i].calls &&
                 strcmp(c.aggs[j].leaf, c.aggs[i].leaf) < 0)) {
                struct leaf_agg tmp = c.aggs[i];
                c.aggs[i] = c.aggs[j];
                c.aggs[j] = tmp;
            }

    struct json_value leaves;
    json_init(&leaves);
    json_set_array(&leaves);
    size_t emit = c.nleaves < (size_t)top ? c.nleaves : (size_t)top;
    bool ok = true;
    for (size_t i = 0; ok && i < emit; i++) {
        struct leaf_agg *a = &c.aggs[i];
        struct json_value o;
        json_init(&o);
        json_set_object(&o);
        double err_rate = a->calls ? (double)a->errors / (double)a->calls : 0.0;
        uint64_t avg_out = a->calls ? a->output_bytes_sum / a->calls : 0;
        ok = json_push_kv_str(&o, "leaf", a->leaf) &&
             json_push_kv_int(&o, "calls", (int64_t)a->calls) &&
             json_push_kv_int(&o, "errors", (int64_t)a->errors) &&
             json_push_kv_real(&o, "error_rate", err_rate) &&
             json_push_kv_int(&o, "p50_us",
                              percentile_us(a->elapsed, a->nsamp, 50)) &&
             json_push_kv_int(&o, "p99_us",
                              percentile_us(a->elapsed, a->nsamp, 99)) &&
             json_push_kv_int(&o, "avg_output_bytes", (int64_t)avg_out) &&
             json_push_kv_int(&o, "budget_exceeded_count",
                              (int64_t)a->budget_exceeded) &&
             json_push_back(&leaves, &o);
        json_free(&o);
    }
    ok = ok && json_push_kv_str(out, "schema", "zcl.cmd_ledger_summary.v1") &&
         json_push_kv_bool(out, "installed", g_installed) &&
         json_push_kv_int(out, "window_s", window_s) &&
         json_push_kv_int(out, "window_start_ms", c.oldest_ms) &&
         json_push_kv_int(out, "window_end_ms", c.newest_ms) &&
         json_push_kv_int(out, "total_calls", (int64_t)c.total_calls) &&
         json_push_kv_int(out, "distinct_leaves", (int64_t)c.nleaves) &&
         json_push_kv_bool(out, "retention_gap", ledger_retention_gap()) &&
         json_push_kv(out, "leaves", &leaves);
    json_free(&leaves);
    free(c.aggs);
    return ok;
}

/* ── Tail ───────────────────────────────────────────────────────────────── */
struct tail_ctx {
    char *lines;   /* n * LEDGER_LINE_MAX ring of raw record bytes */
    size_t linecap;
    size_t cap;    /* == n */
    const char *filter;
    size_t count;  /* live entries */
    size_t head;   /* oldest slot */
};
static void tail_visit(const char *raw, size_t rawlen,
                       const struct json_value *rec, void *vctx)
{
    struct tail_ctx *c = vctx;
    if (c->filter && c->filter[0] &&
        strcmp(rec_str(rec, "leaf"), c->filter) != 0)
        return;
    if (rawlen >= c->linecap)
        return;
    size_t slot = (c->head + c->count) % c->cap;
    if (c->count == c->cap)
        c->head = (c->head + 1) % c->cap; /* overwrite the oldest */
    else
        c->count++;
    memcpy(c->lines + slot * c->linecap, raw, rawlen);
    c->lines[slot * c->linecap + rawlen] = '\0';
}

bool command_ledger_tail(int n, const char *leaf_filter, struct json_value *out)
{
    if (!out)
        return false;
    if (n < 1)
        n = 20;
    if (n > 200)
        n = 200;
    struct tail_ctx c;
    memset(&c, 0, sizeof(c));
    c.cap = (size_t)n;
    c.linecap = LEDGER_LINE_MAX;
    c.filter = leaf_filter;
    c.lines = zcl_calloc(c.cap, c.linecap, "command_ledger tail ring");
    if (!c.lines)
        LOG_FAIL("command_ledger", "tail ring alloc failed");
    if (g_installed)
        ledger_scan(LEDGER_QUERY_SCAN_BYTES, tail_visit, &c);

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    bool ok = true;
    /* Emit newest-first: walk the ring from the most-recent entry backward. */
    for (size_t k = 0; ok && k < c.count; k++) {
        size_t slot = (c.head + c.count - 1 - k) % c.cap;
        const char *raw = c.lines + slot * c.linecap;
        struct json_value rec;
        json_init(&rec);
        if (json_read(&rec, raw, strlen(raw)) && rec.type == JSON_OBJ)
            ok = json_push_back(&arr, &rec);
        json_free(&rec);
    }
    ok = ok && json_push_kv_str(out, "schema", "zcl.cmd_ledger_tail.v1") &&
         json_push_kv_bool(out, "installed", g_installed) &&
         json_push_kv_int(out, "count", (int64_t)c.count) &&
         json_push_kv_bool(out, "retention_gap", ledger_retention_gap()) &&
         json_push_kv(out, "records", &arr);
    json_free(&arr);
    free(c.lines);
    return ok;
}

/* ── dumpstate ──────────────────────────────────────────────────────────── */
bool command_ledger_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    int64_t cur_bytes = 0, one_bytes = 0, oldest = 0, newest = 0;
    struct stat st;
    if (g_installed && stat(g_path, &st) == 0)
        cur_bytes = (int64_t)st.st_size;
    if (g_installed && stat(g_path1, &st) == 0)
        one_bytes = (int64_t)st.st_size;

    if (g_installed) {
        /* oldest = first line of .1 (or current if no .1); newest = last line
         * of current. Bounded head/tail reads only. */
        size_t len = 0;
        bool partial = false;
        char *head =
            read_tail(g_path1[0] && one_bytes > 0 ? g_path1 : g_path,
                      LEDGER_LINE_MAX * 2, &len, &partial);
        if (head) {
            char *nl = memchr(head, '\n', len);
            if (nl) {
                struct json_value rec;
                json_init(&rec);
                if (json_read(&rec, head, (size_t)(nl - head)))
                    oldest = rec_i64(&rec, "ts_unix_ms");
                json_free(&rec);
            }
            free(head);
        }
        char *tail = read_tail(g_path, LEDGER_LINE_MAX * 2, &len, &partial);
        if (tail) {
            size_t e = len;
            while (e > 0 && tail[e - 1] == '\n')
                e--;
            size_t s = e;
            while (s > 0 && tail[s - 1] != '\n')
                s--;
            if (e > s) {
                struct json_value rec;
                json_init(&rec);
                if (json_read(&rec, tail + s, e - s))
                    newest = rec_i64(&rec, "ts_unix_ms");
                json_free(&rec);
            }
            free(tail);
        }
    }

    int64_t avg = 200; /* approx bytes/record for the count estimate */
    int64_t approx_records = (cur_bytes + one_bytes) / avg;
    return json_push_kv_str(out, "schema", "zcl.cmd_ledger_state.v1") &&
           json_push_kv_bool(out, "installed", g_installed) &&
           json_push_kv_str(out, "path", g_installed ? g_path : "") &&
           json_push_kv_int(out, "current_bytes", cur_bytes) &&
           json_push_kv_int(out, "rotated_bytes", one_bytes) &&
           json_push_kv_int(out, "max_bytes", g_max_bytes) &&
           json_push_kv_int(out, "approx_records", approx_records) &&
           json_push_kv_int(out, "records_appended",
                            (int64_t)atomic_load_explicit(
                                &g_records, memory_order_relaxed)) &&
           json_push_kv_int(out, "rotations",
                            (int64_t)atomic_load_explicit(
                                &g_rotations, memory_order_relaxed)) &&
           json_push_kv_int(out, "oldest_ts_unix_ms", oldest) &&
           json_push_kv_int(out, "newest_ts_unix_ms", newest) &&
           json_push_kv_bool(out, "retention_gap", ledger_retention_gap());
}
