/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "platform/time_compat.h"
#include "rpc/httpserver.h"
#include "rpc/http_middleware.h"
#include <sys/stat.h>
#include "rpc/rpc_timeout.h"
#include "net/ws_events.h"
#include "json/json.h"
#include "rpc/protocol.h"
#include "core/random.h"
#include "encoding/utilstrencodings.h"
#include "mcp/metrics.h"
#include "support/cleanse.h"
#include "util/log_macros.h"
#include "util/trace.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

#define RPC_HTTP_WORKERS 4
#define RPC_HTTP_QUEUE_CAP 64

/* Upper bound on a single serialized JSON-RPC response body. Generous
 * enough for the largest legitimate responses (gettxoutsetinfo, a full
 * listunspent / getrawmempool true) while bounding the one-shot
 * allocation an authenticated client can drive. A response that would
 * exceed this returns a proper RPC error envelope instead of a partial
 * or out-of-band send. */
#define RPC_HTTP_MAX_RESP_BYTES ((size_t)128 * 1024 * 1024)

/* ── Connection abstraction for plain + TLS ───────────────────────── */

struct rpc_conn {
    int   fd;
    SSL  *ssl;  /* NULL for plain-text connections */
};

static int g_listen_fd = -1;
static const struct rpc_table *g_table = NULL;
static pthread_t g_listen_thread;
static bool g_listen_thread_started = false;
/* g_running coordinates the listener + worker threads with rpc_http_stop().
 * volatile is *not* a thread-synchronization primitive in C (it's for MMIO
 * and signal handlers); reads/writes across threads need atomic semantics
 * for memory ordering. The shutdown path also broadcasts g_client_queue_cond
 * after flipping this, so workers in dequeue_client wake; this atomic is
 * belt-and-suspenders for any other read sites. */
static _Atomic bool g_running = false;
static struct rpc_http_middleware g_middleware;
static bool g_middleware_active = false;
static struct rpc_timeout_mgr g_rpc_timeout;
static bool g_rpc_timeout_active = false;
static char g_rpc_user[128];
static char g_rpc_password[128];
static char g_rpc_password_prev[128];  /* previous cookie — valid until next rotation */
static char g_cookie_file[1024];
static bool g_auth_required = false;
static bool g_cookie_mode = false;     /* true when using generated cookie (not explicit user/pass) */
static pthread_mutex_t g_cookie_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_cookie_rotate_thread;
static bool g_cookie_rotate_started = false;
static int g_cookie_rotate_sec = 86400; /* default 24h, env ZCL_RPC_COOKIE_ROTATE_SEC */
/* Prometheus /metrics HTTP endpoint. Off by default; an operator
 * sets ZCL_METRICS_HTTP_ENABLE=1 to expose the same text that
 * `zcl_metrics` returns via MCP. Gated behind the same RPC Basic-auth
 * cookie the wallet endpoints use. Prometheus `scrape_configs`
 * supports `basic_auth: { username_file: ..., password_file: ... }` —
 * point
 * those at the two halves of `~/.zclassic-c23/.cookie` and the
 * scraper authenticates exactly like `zclassic-cli`. Previously the
 * endpoint was open by design, exposing peer counts / tx volume /
 * mempool size to anyone who could reach the TLS listener — usable
 * for network fingerprinting. The HTTP middleware (rate-limit + ban
 * + loopback bypass) still runs first, unchanged. */
static bool g_metrics_http_enable = false;
static pthread_t g_worker_threads[RPC_HTTP_WORKERS];
static size_t g_workers_started = 0;
static struct rpc_conn g_client_queue[RPC_HTTP_QUEUE_CAP];
static size_t g_client_queue_head = 0;
static size_t g_client_queue_tail = 0;
static size_t g_client_queue_count = 0;
static pthread_mutex_t g_client_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_client_queue_cond = PTHREAD_COND_INITIALIZER;

/* ── TLS state ────────────────────────────────────────────────────── */
static SSL_CTX *g_tls_ctx = NULL;
static int g_tls_listen_fd = -1;
static pthread_t g_tls_listen_thread;
static bool g_tls_listen_thread_started = false;
static uint16_t g_tls_port = 0;

bool rpc_http_test_build_response_envelope(bool rpc_ok,
                                           const char *method,
                                           struct json_value *rpc_result,
                                           const struct json_value *id,
                                           struct json_value *response)
{
    struct json_value null_err = {0};
    struct json_value null_res = {0};
    bool ok = true;

    if (!rpc_result || !id || !response)
        return false;

    json_init(response);
    json_set_object(response);

    if (rpc_ok) {
        ok = ok && json_push_kv(response, "result", rpc_result);
        json_set_null(&null_err);
        ok = ok && json_push_kv(response, "error", &null_err);
        json_free(&null_err);
    } else {
        json_set_null(&null_res);
        ok = ok && json_push_kv(response, "result", &null_res);
        json_free(&null_res);
        if (rpc_result->type == JSON_OBJ)
            ok = ok && json_push_kv_str(rpc_result, "method",
                                        method ? method : "");
        ok = ok && json_push_kv(response, "error", rpc_result);
    }

    ok = ok && json_push_kv(response, "id", id);
    return ok;
}

/* Constant-time comparison to prevent timing attacks on RPC credentials.
 * Always compares all bytes of the shorter string; returns 0 on match. */
static int constant_time_strcmp(const char *a, size_t alen,
                                 const char *b, size_t blen)
{
    unsigned int diff = (unsigned int)(alen ^ blen);
    size_t n = alen < blen ? alen : blen;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned int)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return diff == 0 ? 0 : 1;
}

static bool check_auth(const char *auth_header)
{
    if (!g_auth_required) return true;
    if (!auth_header) return false;

    while (*auth_header == ' ') auth_header++;
    if (strncmp(auth_header, "Basic ", 6) != 0) return false;
    const char *b64 = auth_header + 6;
    while (*b64 == ' ') b64++;

    unsigned char decoded[512];
    size_t dlen = DecodeBase64(b64, decoded, sizeof(decoded) - 1, NULL);
    decoded[dlen] = '\0';

    char expected[512];
    pthread_mutex_lock(&g_cookie_mutex);
    snprintf(expected, sizeof(expected), "%s:%s", g_rpc_user, g_rpc_password);
    size_t elen = strlen(expected);
    bool ok = (constant_time_strcmp((const char *)decoded, dlen,
                                    expected, elen) == 0);

    /* Accept previous cookie during rotation transition window */
    if (!ok && g_cookie_mode && g_rpc_password_prev[0]) {
        snprintf(expected, sizeof(expected), "%s:%s",
                 g_rpc_user, g_rpc_password_prev);
        elen = strlen(expected);
        ok = (constant_time_strcmp((const char *)decoded, dlen,
                                   expected, elen) == 0);
    }
    pthread_mutex_unlock(&g_cookie_mutex);

    memory_cleanse(expected, sizeof(expected));
    memory_cleanse(decoded, sizeof(decoded));
    return ok;
}

/* ── I/O wrappers: plain fd or SSL ─────────────────────────────── */

static ssize_t conn_read(const struct rpc_conn *c, void *buf, size_t len)
{
    if (c->ssl)
        return SSL_read(c->ssl, buf, (int)len);
    return read(c->fd, buf, len);
}

static ssize_t conn_write(const struct rpc_conn *c, const void *buf, size_t len)
{
    if (c->ssl)
        return SSL_write(c->ssl, buf, (int)len);
    return write(c->fd, buf, len);
}

/* Cap request-header count so an endless-header stream (a slowloris variant)
 * cannot pin a server thread reading header lines forever. Legitimate
 * RPC/metrics/WebSocket clients send well under this. */
#define HTTP_MAX_REQUEST_HEADERS 512

static bool read_line(const struct rpc_conn *c, char *buf, size_t buflen)
{
    size_t pos = 0;
    while (pos < buflen - 1) {
        char ch;
        ssize_t r = conn_read(c, &ch, 1);
        if (r <= 0) return false;
        if (ch == '\n') {
            if (pos > 0 && buf[pos - 1] == '\r')
                pos--;
            buf[pos] = '\0';
            return true;
        }
        buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return true;
}

static bool read_exact(const struct rpc_conn *c, char *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t r = conn_read(c, buf + total, len - total);
        if (r <= 0) return false;
        total += (size_t)r;
    }
    return true;
}

static void send_response_with_type(const struct rpc_conn *c, int status_code,
                                     const char *status_text,
                                     const char *content_type,
                                     const char *body, size_t body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);
    (void)conn_write(c, header, (size_t)hlen);
    if (body_len > 0)
        (void)conn_write(c, body, body_len);
}

static void send_response(const struct rpc_conn *c, int status_code,
                            const char *status_text,
                            const char *body, size_t body_len)
{
    send_response_with_type(c, status_code, status_text,
                            "application/json", body, body_len);
}

static bool enqueue_client(struct rpc_conn conn)
{
    bool ok = false;

    pthread_mutex_lock(&g_client_queue_mutex);
    if (g_client_queue_count < RPC_HTTP_QUEUE_CAP) {
        g_client_queue[g_client_queue_tail] = conn;
        g_client_queue_tail =
            (g_client_queue_tail + 1) % RPC_HTTP_QUEUE_CAP;
        g_client_queue_count++;
        ok = true;
        pthread_cond_signal(&g_client_queue_cond);
    }
    pthread_mutex_unlock(&g_client_queue_mutex);

    return ok;
}

static struct rpc_conn dequeue_client(void)
{
    struct rpc_conn conn = { .fd = -1, .ssl = NULL };

    pthread_mutex_lock(&g_client_queue_mutex);
    /* Timed wait so a worker never blocks past a shutdown that skipped
     * the cond_broadcast (e.g., an abort path that bypasses
     * rpc_http_stop). 2 s wake is invisible under load — the cond is
     * signaled on each enqueue — and bounded under shutdown. */
    while (g_client_queue_count == 0 && g_running) {
        struct timespec deadline;
        platform_time_realtime_timespec(&deadline);
        deadline.tv_sec += 2;
        pthread_cond_timedwait(&g_client_queue_cond, &g_client_queue_mutex,
                               &deadline);
    }

    if (g_client_queue_count > 0) {
        conn = g_client_queue[g_client_queue_head];
        g_client_queue_head =
            (g_client_queue_head + 1) % RPC_HTTP_QUEUE_CAP;
        g_client_queue_count--;
    }
    pthread_mutex_unlock(&g_client_queue_mutex);

    return conn;
}

static void conn_close(struct rpc_conn *c)
{
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

/* Two-pass serialization of a JSON-RPC response. json_write() returns
 * the FULL required length regardless of the buffer it is handed (it
 * only writes where pos < buflen, and a zero-length buffer is never
 * dereferenced), so we size first with a zero-length probe, reject
 * anything past RPC_HTTP_MAX_RESP_BYTES, then allocate exactly len+1
 * and write the body. This replaces the old fixed 4 MiB buffer whose
 * unclamped length was fed straight to write() — for a response larger
 * than the buffer that read (len - 4 MiB) bytes past the allocation and
 * shipped adjacent heap memory to the client (crash/DoS + info-leak).
 *
 * On success: *out_buf owns a heap buffer the caller must free(), and
 * *out_len is the exact body length to send. Returns false (with
 * *out_buf == NULL) on OOM or when the response exceeds the cap; the
 * caller sends a proper RPC error envelope instead.
 *
 * Exposed (non-static) so the regression test exercises the exact same
 * sizing path the HTTP response uses — same convention as
 * rpc_http_test_build_response_envelope above. */
bool rpc_http_test_serialize_response(const struct json_value *response,
                                      char **out_buf, size_t *out_len)
{
    if (!response || !out_buf || !out_len) {
        if (out_buf) *out_buf = NULL;
        if (out_len) *out_len = 0;
        return false;
    }
    *out_buf = NULL;
    *out_len = 0;

    /* Pass 1: size the body without writing (zero-length buffer). */
    size_t need = json_write(response, NULL, 0);
    if (need > RPC_HTTP_MAX_RESP_BYTES) {
        LOG_FAIL("rpc", "response too large: %zu > %zu bytes", need,
                 RPC_HTTP_MAX_RESP_BYTES);
        return false;
    }

    char *buf = zcl_malloc(need + 1, "http_resp_buf");
    if (!buf) {
        LOG_FAIL("rpc", "response buffer alloc failed: %zu bytes", need + 1);
        return false;
    }

    /* Pass 2: write exactly into a buffer sized to hold the whole body.
     * json_write writes the NUL only when pos < buflen; need+1 guarantees
     * room for it, and the returned length is the body length to send. */
    size_t wrote = json_write(response, buf, need + 1);
    if (wrote != need) {
        /* Should be impossible (the two passes serialize the same value),
         * but never ship a length that disagrees with what we wrote. Free
         * before logging since LOG_FAIL returns. */
        free(buf);
        LOG_FAIL("rpc", "response size mismatch: sized %zu wrote %zu", need,
                 wrote);
    }

    *out_buf = buf;
    *out_len = wrote;
    return true;
}

static void handle_client(struct rpc_conn conn)
{
    struct trace_span *rpc_span = trace_start("rpc.dispatch");
    int client_fd = conn.fd;

    /* Set socket timeout to prevent slowloris attacks.
     * 5 seconds to send complete request — generous for local RPC,
     * fatal for attackers trying to hold connections open. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Look up the source IP via getpeername() so we can drive the
     * middleware (rate limit + ban check) without changing the queue
     * shape. */
    uint32_t client_ip_be = 0x0100007Fu; /* fall back to 127.0.0.1 */
    {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        if (getpeername(client_fd, (struct sockaddr *)&peer, &peer_len) == 0
            && peer.sin_family == AF_INET) {
            client_ip_be = peer.sin_addr.s_addr;
        }
    }

    /* Register this request with the timeout watchdog. The watchdog
     * will shutdown() our fd if the dispatch phase runs past
     * ZCL_RPC_TIMEOUT_MS — our in-flight read/write then fails and
     * we unwind cleanly. slot == -1 means table full or module
     * disabled; either way we just proceed without tracking. */
    int tmo_slot = -1;
    if (g_rpc_timeout_active) {
        tmo_slot = rpc_timeout_register(&g_rpc_timeout, client_fd, client_ip_be);
    }

    /* Pre-flight: ban + rate limit BEFORE we touch the request.
     * The middleware is loopback-aware and will exempt 127.0.0.0/8
     * from ban + per-IP buckets. */
    if (g_middleware_active) {
        enum rpc_http_decision d =
            rpc_http_middleware_check(&g_middleware, client_ip_be);
        if (d == RPC_HTTP_BANNED) {
            char errbuf[256];
            size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                -32003, "IP banned", NULL, NULL);
            send_response(&conn, 403, "Forbidden", errbuf, elen);
            goto done;
        }
        if (d == RPC_HTTP_RATE_LIMITED_GLOBAL ||
            d == RPC_HTTP_RATE_LIMITED_PER_IP) {
            char errbuf[256];
            size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                -32005, "Rate limit exceeded", NULL, NULL);
            send_response(&conn, 429, "Too Many Requests",
                          errbuf, elen);
            goto done;
        }
    }

    char method[16];
    char path[256];
    char line[4096];

    if (!read_line(&conn, line, sizeof(line)))
        goto done;

    if (sscanf(line, "%15s %255s", method, path) != 2)
        goto done;

    /* GET /metrics serves Prometheus text when enabled via
     * ZCL_METRICS_HTTP_ENABLE=1. Auth required — same Basic-auth
     * cookie the wallet endpoints use. Scrapers point
     * `basic_auth.password_file` at the cookie and authenticate as
     * `zclassic-cli` does. Rate-limit + ban middleware has already
     * run for this connection above. */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/metrics") == 0) {
        if (!g_metrics_http_enable) {
            /* Drain request headers so the socket closes cleanly. */
            int drain_hdrs = 0;
            while (read_line(&conn, line, sizeof(line))) {
                if (line[0] == '\0') break;
                if (++drain_hdrs > HTTP_MAX_REQUEST_HEADERS) goto done;
            }
            const char *msg = "metrics endpoint disabled "
                              "(set ZCL_METRICS_HTTP_ENABLE=1)";
            send_response_with_type(&conn, 404, "Not Found",
                                    "text/plain; charset=utf-8",
                                    msg, strlen(msg));
            goto done;
        }

        char metrics_auth[512] = {0};
        int metrics_hdrs = 0;
        while (read_line(&conn, line, sizeof(line))) {
            if (line[0] == '\0') break;
            if (++metrics_hdrs > HTTP_MAX_REQUEST_HEADERS) goto done;
            if (strncasecmp(line, "Authorization:", 14) == 0)
                snprintf(metrics_auth, sizeof(metrics_auth),
                         "%s", line + 14);
        }

        if (!check_auth(metrics_auth[0] ? metrics_auth : NULL)) {
            if (g_middleware_active)
                rpc_http_middleware_record_auth_fail(&g_middleware,
                                                     client_ip_be);
            const char *msg = "authentication required";
            send_response_with_type(&conn, 401, "Unauthorized",
                                    "text/plain; charset=utf-8",
                                    msg, strlen(msg));
            goto done;
        }
        if (g_middleware_active)
            rpc_http_middleware_record_success(&g_middleware, client_ip_be);

        size_t cap = 131072;
        char *buf = zcl_malloc(cap, "http_read_buf");
        if (!buf) {
            const char *oom = "out of memory";
            send_response_with_type(&conn, 500, "Internal Server Error",
                                    "text/plain; charset=utf-8",
                                    oom, strlen(oom));
            goto done;
        }
        size_t n = mcp_metrics_render_prometheus(buf, cap);
        /* Prometheus exposition format 0.0.4 */
        send_response_with_type(&conn, 200, "OK",
            "text/plain; version=0.0.4; charset=utf-8",
            buf, n);
        free(buf);
        goto done;
    }

    /* WebSocket event stream at GET /events.
     * Check for WebSocket upgrade request before rejecting non-POST.
     * The path may include a query string: /events?domain=chain,peer */
    if (strcmp(method, "GET") == 0 &&
        (strncmp(path, "/events", 7) == 0 &&
         (path[7] == '\0' || path[7] == '?'))) {
        /* Read headers looking for WebSocket upgrade fields */
        char ws_key[128] = {0};
        char ws_auth[512] = {0};
        bool is_upgrade = false;
        int ws_hdrs = 0;
        while (read_line(&conn, line, sizeof(line))) {
            if (line[0] == '\0') break;
            if (++ws_hdrs > HTTP_MAX_REQUEST_HEADERS) goto done;
            if (strncasecmp(line, "Upgrade:", 8) == 0 &&
                strstr(line + 8, "websocket"))
                is_upgrade = true;
            if (strncasecmp(line, "Authorization:", 14) == 0)
                snprintf(ws_auth, sizeof(ws_auth), "%s", line + 14);
            if (strncasecmp(line, "Sec-WebSocket-Key:", 18) == 0) {
                const char *p = line + 18;
                while (*p == ' ') p++;
                snprintf(ws_key, sizeof(ws_key), "%s", p);
                /* Trim trailing whitespace */
                size_t kl = strlen(ws_key);
                while (kl > 0 && (ws_key[kl-1] == '\r' ||
                       ws_key[kl-1] == '\n' || ws_key[kl-1] == ' '))
                    ws_key[--kl] = '\0';
            }
        }
        if (is_upgrade && ws_key[0]) {
            /* Same Basic-auth gate as the JSON-RPC path: the event
             * stream exposes chain/peer/wallet activity, so an
             * unauthenticated upgrade is an information leak. */
            if (!check_auth(ws_auth[0] ? ws_auth : NULL)) {
                if (g_middleware_active)
                    rpc_http_middleware_record_auth_fail(&g_middleware,
                                                         client_ip_be);
                char errbuf[256];
                size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                    HTTP_UNAUTHORIZED, "Unauthorized", NULL, NULL);
                send_response(&conn, 401, "Unauthorized", errbuf, elen);
                goto done;
            }
            if (g_middleware_active)
                rpc_http_middleware_record_success(&g_middleware,
                                                   client_ip_be);
            /* ws_events owns a raw fd only — handing it a TLS socket
             * would write the 101 handshake plaintext beneath the TLS
             * stream and orphan the SSL object on the early return.
             * Refuse over SSL instead; done: SSL_frees via conn_close. */
            if (conn.ssl) {
                char errbuf[256];
                size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                    RPC_INVALID_REQUEST,
                    "WebSocket /events not supported on the TLS listener",
                    NULL, NULL);
                send_response(&conn, 501, "Not Implemented", errbuf, elen);
                goto done;
            }
            const char *query = strchr(path, '?');
            if (ws_events_upgrade(client_fd, path, ws_key, query)) {
                /* fd is now owned by ws_events — do NOT close it */
                if (tmo_slot >= 0)
                    rpc_timeout_unregister(&g_rpc_timeout, tmo_slot);
                return;  /* skip done: label which closes fd */
            }
            /* Upgrade failed — fall through to 503 */
            char errbuf[256];
            size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                -32006, "WebSocket capacity full (max 100)", NULL, NULL);
            send_response(&conn, 503, "Service Unavailable",
                          errbuf, elen);
            goto done;
        }
        /* Not a WebSocket upgrade — fall through to reject */
    }

    /* No blog over clearnet. Blog is Tor-only via dynhost.
     * Clearnet serves ONLY authenticated RPC (POST). */
    if (strcmp(method, "POST") != 0) {
        char errbuf[256];
        size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
            RPC_INVALID_REQUEST, "Method not allowed", NULL, NULL);
        send_response(&conn, 405, "Method Not Allowed", errbuf, elen);
        goto done;
    }

    size_t content_length = 0;
    char auth_value[512] = {0};
    int post_hdrs = 0;
    while (read_line(&conn, line, sizeof(line))) {
        if (line[0] == '\0') break;
        if (++post_hdrs > HTTP_MAX_REQUEST_HEADERS) goto done;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *endp = NULL;
            long v = strtol(line + 15, &endp, 10);
            if (endp == line + 15 || v < 0 || v > 10 * 1024 * 1024)
                content_length = 0;
            else
                content_length = (size_t)v;
        }
        if (strncasecmp(line, "Authorization:", 14) == 0)
            snprintf(auth_value, sizeof(auth_value), "%s", line + 14);
    }

    if (!check_auth(auth_value[0] ? auth_value : NULL)) {
        if (g_middleware_active)
            rpc_http_middleware_record_auth_fail(&g_middleware, client_ip_be);
        char errbuf[256];
        size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
            HTTP_UNAUTHORIZED, "Unauthorized", NULL, NULL);
        send_response(&conn, 401, "Unauthorized", errbuf, elen);
        goto done;
    }
    if (g_middleware_active)
        rpc_http_middleware_record_success(&g_middleware, client_ip_be);

    if (content_length == 0 || content_length > 10 * 1024 * 1024) {
        if (content_length > 10 * 1024 * 1024) {
            char errbuf[256];
            size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                RPC_INVALID_REQUEST, "Request body too large", NULL, NULL);
            send_response(&conn, 413, "Payload Too Large", errbuf, elen);
        }
        goto done;
    }

    char *body = zcl_malloc(content_length + 1, "http_body");
    if (!body) goto done;

    if (!read_exact(&conn, body, content_length)) {
        free(body);
        goto done;
    }
    body[content_length] = '\0';

    struct json_value request;
    json_init(&request);
    if (!json_read(&request, body, content_length)) {
        free(body);
        json_free(&request);
        char errbuf[256];
        size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
            RPC_PARSE_ERROR, "Parse error", NULL, NULL);
        send_response(&conn, 200, "OK", errbuf, elen);
        goto done;
    }
    free(body);

    struct json_request req;
    json_request_init(&req);
    if (!json_request_parse(&req, &request)) {
        json_free(&request);
        json_request_free(&req);
        char errbuf[256];
        size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
            RPC_INVALID_REQUEST, "Invalid request", NULL, NULL);
        send_response(&conn, 200, "OK", errbuf, elen);
        goto done;
    }
    json_free(&request);

    /* Now that we know the method name, label the timeout slot so
     * EV_RPC_TIMEOUT carries useful context if the watchdog kills us
     * during dispatch. */
    if (tmo_slot >= 0) {
        rpc_timeout_set_method(&g_rpc_timeout, tmo_slot, req.method);
    }
    trace_attr_str(rpc_span, "method", req.method);

    struct json_value result;
    json_init(&result);
    bool rpc_ok = rpc_table_execute(g_table, req.method, &req.params, &result);

    /* Build standard JSON-RPC response:
     *   success: {result: <data>, error: null, id: <id>}
     *   failure: {result: null, error: {code, message, method}, id: <id>}
     * Route through the shared helper so the HTTP path and the
     * regression test exercise the same stack-init discipline. */
    struct json_value response;
    json_init(&response);
    bool response_ok = rpc_http_test_build_response_envelope(
        rpc_ok, req.method, &result, &req.id, &response);

    char *resp_buf = NULL;
    size_t resp_len = 0;
    if (response_ok &&
        rpc_http_test_serialize_response(&response, &resp_buf, &resp_len)) {
        /* Body sized and written by the same value: send exactly the
         * bytes we wrote — never an unclamped length past the buffer. */
        send_response(&conn, 200, "OK", resp_buf, resp_len);
        free(resp_buf);
    } else {
        char errbuf[256];
        size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
            RPC_OUT_OF_MEMORY, "Internal error: response too large or "
            "out of memory", req.method, NULL);
        send_response(&conn, 500, "Internal Server Error",
                      errbuf, elen);
    }

    json_free(&result);
    json_free(&response);
    json_request_free(&req);

done:
    if (tmo_slot >= 0) {
        rpc_timeout_unregister(&g_rpc_timeout, tmo_slot);
    }
    trace_end(rpc_span);
    conn_close(&conn);
}

static void *rpc_worker_thread_fn(void *arg)
{
    (void)arg;

    while (g_running) {
        struct rpc_conn conn = dequeue_client();
        if (conn.fd < 0)
            continue;
        handle_client(conn);
    }

    return NULL;
}

static void *listen_thread_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running)
                perror("accept");
            continue;
        }
        struct rpc_conn conn = { .fd = client_fd, .ssl = NULL };
        if (!enqueue_client(conn)) {
            struct rpc_conn tmp = { .fd = client_fd, .ssl = NULL };
            char errbuf[256];
            size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                RPC_INTERNAL_ERROR, "RPC server busy", NULL, NULL);
            send_response(&tmp, 503, "Service Unavailable",
                          errbuf, elen);
            close(client_fd);
        }
    }
    return NULL;
}

/* ── TLS listener thread ──────────────────────────────────────────── */

static void *tls_listen_thread_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_tls_listen_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running)
                perror("tls accept");
            continue;
        }

        /* Bound the inline handshake: SSL_accept runs on the accept
         * loop, so a slowloris client that never finishes the handshake
         * would stall ALL new TLS RPC connections. Blocking-socket
         * timeouts make its internal reads/writes fail after 5 s and
         * fall into the existing drop path below. */
        struct timeval hs_tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &hs_tv, sizeof(hs_tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &hs_tv, sizeof(hs_tv));

        /* Perform TLS handshake */
        SSL *ssl = SSL_new(g_tls_ctx);
        if (!ssl) {
            fprintf(stderr, "RPC TLS: SSL_new failed\n");  // obs-ok:helper-context-logged
            close(client_fd);
            continue;
        }
        SSL_set_fd(ssl, client_fd);
        if (SSL_accept(ssl) <= 0) {
            /* TLS handshake failure — drop silently (common with
             * port scanners and misconfigured clients) */
            SSL_free(ssl);
            close(client_fd);
            continue;
        }

        struct rpc_conn conn = { .fd = client_fd, .ssl = ssl };
        if (!enqueue_client(conn)) {
            char errbuf[256];
            size_t elen = json_rpc_error_response(errbuf, sizeof(errbuf),
                RPC_INTERNAL_ERROR, "RPC server busy", NULL, NULL);
            send_response(&conn, 503, "Service Unavailable",
                          errbuf, elen);
            conn_close(&conn);
        }
    }
    return NULL;
}

/* ── TLS initialization ───────────────────────────────────────────── */

static SSL_CTX *tls_init(const char *cert_path, const char *key_path)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx)
        LOG_NULL("rpc", "TLS: SSL_CTX_new failed");

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
        SSL_CTX_free(ctx);
        LOG_NULL("rpc", "TLS: failed to load certificate: %s", cert_path);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        LOG_NULL("rpc", "TLS: failed to load private key: %s", key_path);
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        LOG_NULL("rpc", "TLS: cert/key mismatch");
    }

    return ctx;
}

/* ── Cookie rotation ────────────────────────────────────────────── */

void rpc_http_cookie_rotate(void)
{
    pthread_mutex_lock(&g_cookie_mutex);
    if (!g_cookie_mode || !g_auth_required) {
        pthread_mutex_unlock(&g_cookie_mutex);
        return;
    }

    /* Shift current → previous */
    memory_cleanse(g_rpc_password_prev, sizeof(g_rpc_password_prev));
    memcpy(g_rpc_password_prev, g_rpc_password, sizeof(g_rpc_password));

    /* Generate new password */
    uint64_t r1 = GetRand(UINT64_MAX);
    uint64_t r2 = GetRand(UINT64_MAX);
    snprintf(g_rpc_password, sizeof(g_rpc_password),
             "%016llx%016llx",
             (unsigned long long)r1, (unsigned long long)r2);

    /* Write new cookie to disk */
    if (g_cookie_file[0]) {
        FILE *f = fopen(g_cookie_file, "w");
        if (f) {
            fprintf(f, "%s:%s", g_rpc_user, g_rpc_password);
            fclose(f);
            chmod(g_cookie_file, 0600);
        }
    }
    pthread_mutex_unlock(&g_cookie_mutex);
    printf("RPC cookie rotated\n");
}

static void *cookie_rotate_thread_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        /* Sleep in 1-second ticks so we notice shutdown promptly */
        for (int i = 0; i < g_cookie_rotate_sec && g_running; i++)
            sleep(1);
        if (g_running)
            rpc_http_cookie_rotate();
    }
    return NULL;
}

int rpc_http_cookie_rotate_sec(void)
{
    return g_cookie_rotate_sec;
}

/* ── Server start/stop ──────────────────────────────────────────── */

const struct rpc_table *rpc_http_active_table(void)
{
    return g_table;
}

bool rpc_http_start(const struct rpc_table *table, uint16_t port,
                     const char *rpc_user, const char *rpc_password,
                     const char *datadir)
{
    if (g_running || g_listen_thread_started || g_workers_started > 0)
        return false;

    g_client_queue_head = 0;
    g_client_queue_tail = 0;
    g_client_queue_count = 0;
    g_table = table;
    g_rpc_user[0] = '\0';
    g_rpc_password[0] = '\0';
    memory_cleanse(g_rpc_password_prev, sizeof(g_rpc_password_prev));
    g_cookie_file[0] = '\0';
    g_auth_required = false;
    g_cookie_mode = false;
    if (rpc_user && rpc_password) {
        snprintf(g_rpc_user, sizeof(g_rpc_user), "%s", rpc_user);
        snprintf(g_rpc_password, sizeof(g_rpc_password), "%s", rpc_password);
        g_auth_required = true;
    } else if (datadir) {
        snprintf(g_rpc_user, sizeof(g_rpc_user), "__cookie__");
        uint64_t r1 = GetRand(UINT64_MAX);
        uint64_t r2 = GetRand(UINT64_MAX);
        snprintf(g_rpc_password, sizeof(g_rpc_password),
                 "%016llx%016llx",
                 (unsigned long long)r1, (unsigned long long)r2);
        g_auth_required = true;
        g_cookie_mode = true;

        snprintf(g_cookie_file, sizeof(g_cookie_file),
                 "%s/.cookie", datadir);
        FILE *f = fopen(g_cookie_file, "w");
        if (f) {
            fprintf(f, "%s:%s", g_rpc_user, g_rpc_password);
            fclose(f);
            /* Restrict cookie file to owner-only access (0600) to prevent
             * other users on the system from reading RPC credentials. */
            chmod(g_cookie_file, 0600);
            printf("RPC cookie written to %s\n", g_cookie_file);
        }
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return false;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    if (listen(g_listen_fd, 8) < 0) {
        perror("listen");
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    /* Rate limit + per-IP bucket + IP ban for the HTTP RPC surface.
     * Init once on first start; load env config so operators can tune
     * via ZCL_RPC_RPS / ZCL_RPC_PER_IP_RPS / ZCL_RPC_BAN_* without a
     * rebuild. */
    if (!g_middleware_active) {
        rpc_http_middleware_init(&g_middleware);
        rpc_http_middleware_load_from_env(&g_middleware);
        g_middleware_active = true;
    }
    /* Publish the global handle so metrics.c + zcl_rpc_report can read
     * the live config and stats without reaching into httpserver.c. */
    rpc_http_middleware_set_global(&g_middleware);

    /* Per-request timeout watchdog. ZCL_RPC_TIMEOUT_MS=0 disables
     * entirely so operators can opt out. Watchdog thread is dormant
     * in that case. */
    if (!g_rpc_timeout_active) {
        rpc_timeout_init(&g_rpc_timeout);
        rpc_timeout_load_from_env(&g_rpc_timeout);
        g_rpc_timeout_active = true;
    }
    rpc_timeout_set_global(&g_rpc_timeout);
    if (!rpc_timeout_start_watchdog(&g_rpc_timeout)) {
        fprintf(stderr, "RPC server: rpc_timeout watchdog start failed\n");  // obs-ok:helper-context-logged
    }

    /* Optional GET /metrics Prometheus endpoint. Accept "1", "true",
     * "yes", "on" as truthy; anything else leaves it off. */
    g_metrics_http_enable = false;
    const char *mx = getenv("ZCL_METRICS_HTTP_ENABLE");
    if (mx && *mx) {
        if (strcmp(mx, "1") == 0 ||
            strcasecmp(mx, "true") == 0 ||
            strcasecmp(mx, "yes")  == 0 ||
            strcasecmp(mx, "on")   == 0) {
            g_metrics_http_enable = true;
            printf("RPC server: GET /metrics Prometheus endpoint enabled\n");
        }
    }

    /* RPC cookie rotation. Default 24h; operators tune via
     * ZCL_RPC_COOKIE_ROTATE_SEC. Set to 0 to disable rotation. Only
     * active in cookie mode (no explicit rpcuser/rpcpassword). */
    g_cookie_rotate_sec = 86400;
    const char *rot_env = getenv("ZCL_RPC_COOKIE_ROTATE_SEC");
    if (rot_env && *rot_env) {
        int v = atoi(rot_env);
        if (v >= 0)
            g_cookie_rotate_sec = v;
    }
    g_cookie_rotate_started = false;
    if (g_cookie_mode && g_cookie_rotate_sec > 0) {
        /* Thread created after g_running is set (below) */
    }

    /* Optional TLS listener for non-loopback RPC. Set ZCL_RPC_TLS_CERT
     * and ZCL_RPC_TLS_KEY to PEM file paths. TLS listener binds to
     * 0.0.0.0 on rpcport+1 (or ZCL_RPC_TLS_PORT). Plain-text listener
     * stays on 127.0.0.1 for local tools. */
    g_tls_ctx = NULL;
    g_tls_listen_fd = -1;
    g_tls_listen_thread_started = false;
    g_tls_port = 0;
    {
        const char *cert = getenv("ZCL_RPC_TLS_CERT");
        const char *key  = getenv("ZCL_RPC_TLS_KEY");
        if (cert && cert[0] && key && key[0]) {
            g_tls_ctx = tls_init(cert, key);
            if (g_tls_ctx) {
                /* Determine TLS port */
                g_tls_port = port + 1;
                const char *tp = getenv("ZCL_RPC_TLS_PORT");
                if (tp && *tp) {
                    int v = atoi(tp);
                    if (v > 0 && v < 65536)
                        g_tls_port = (uint16_t)v;
                }

                /* Create TLS listen socket on all interfaces */
                g_tls_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (g_tls_listen_fd >= 0) {
                    int topt = 1;
                    setsockopt(g_tls_listen_fd, SOL_SOCKET, SO_REUSEADDR,
                               &topt, sizeof(topt));

                    struct sockaddr_in taddr;
                    memset(&taddr, 0, sizeof(taddr));
                    taddr.sin_family = AF_INET;
                    taddr.sin_addr.s_addr = htonl(INADDR_ANY);
                    taddr.sin_port = htons(g_tls_port);

                    if (bind(g_tls_listen_fd, (struct sockaddr *)&taddr,
                             sizeof(taddr)) < 0) {
                        fprintf(stderr, "RPC TLS: bind port %u failed: %s\n",  // obs-ok:helper-context-logged
                                g_tls_port, strerror(errno));
                        close(g_tls_listen_fd);
                        g_tls_listen_fd = -1;
                    } else if (listen(g_tls_listen_fd, 8) < 0) {
                        fprintf(stderr, "RPC TLS: listen failed: %s\n",  // obs-ok:helper-context-logged
                                strerror(errno));
                        close(g_tls_listen_fd);
                        g_tls_listen_fd = -1;
                    }
                }

                if (g_tls_listen_fd < 0) {
                    /* TLS socket failed — clean up ctx but continue
                     * with plain-text listener only */
                    SSL_CTX_free(g_tls_ctx);
                    g_tls_ctx = NULL;
                    fprintf(stderr, "RPC TLS: disabled (socket failed)\n");  // obs-ok:helper-context-logged
                }
            }
        }
    }

    g_running = true;
    printf("RPC server listening on 127.0.0.1:%u\n", port);
    if (g_tls_ctx)
        printf("RPC TLS server listening on 0.0.0.0:%u\n", g_tls_port);

    for (size_t i = 0; i < RPC_HTTP_WORKERS; i++) {
        if (thread_registry_spawn_ex("zcl_rpc_worker", rpc_worker_thread_fn,
                                      NULL, &g_worker_threads[i]) != 0) {
            perror("thread_registry_spawn_ex");
            g_running = false;
            shutdown(g_listen_fd, SHUT_RDWR);
            close(g_listen_fd);
            g_listen_fd = -1;
            pthread_mutex_lock(&g_client_queue_mutex);
            pthread_cond_broadcast(&g_client_queue_cond);
            pthread_mutex_unlock(&g_client_queue_mutex);
            for (size_t j = 0; j < i; j++)
                pthread_join(g_worker_threads[j], NULL);
            g_workers_started = 0;
            return false;
        }
        g_workers_started = i + 1;
    }

    if (thread_registry_spawn_ex("zcl_rpc_listen", listen_thread_fn, NULL,
                                  &g_listen_thread) != 0) {
        perror("thread_registry_spawn_ex");
        g_running = false;
        pthread_mutex_lock(&g_client_queue_mutex);
        pthread_cond_broadcast(&g_client_queue_cond);
        pthread_mutex_unlock(&g_client_queue_mutex);
        for (size_t i = 0; i < g_workers_started; i++)
            pthread_join(g_worker_threads[i], NULL);
        g_workers_started = 0;
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }
    g_listen_thread_started = true;

    /* Start TLS listener thread if configured */
    if (g_tls_ctx && g_tls_listen_fd >= 0) {
        if (thread_registry_spawn_ex("zcl_rpc_tls", tls_listen_thread_fn,
                                      NULL, &g_tls_listen_thread) == 0) {
            g_tls_listen_thread_started = true;
        } else {
            fprintf(stderr, "RPC TLS: listener thread start failed\n");  // obs-ok:helper-context-logged
            close(g_tls_listen_fd);
            g_tls_listen_fd = -1;
            SSL_CTX_free(g_tls_ctx);
            g_tls_ctx = NULL;
        }
    }

    /* Start cookie rotation background thread */
    if (g_cookie_mode && g_cookie_rotate_sec > 0) {
        if (thread_registry_spawn_ex("zcl_rpc_cookie", cookie_rotate_thread_fn,
                                      NULL, &g_cookie_rotate_thread) == 0) {
            g_cookie_rotate_started = true;
            printf("RPC cookie rotation: every %d seconds\n",
                   g_cookie_rotate_sec);
        }
    }

    return true;
}

void rpc_http_stop(void)
{
    g_running = false;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_tls_listen_fd >= 0) {
        shutdown(g_tls_listen_fd, SHUT_RDWR);
        close(g_tls_listen_fd);
        g_tls_listen_fd = -1;
    }
    pthread_mutex_lock(&g_client_queue_mutex);
    pthread_cond_broadcast(&g_client_queue_cond);
    pthread_mutex_unlock(&g_client_queue_mutex);
    if (g_listen_thread_started) {
        pthread_join(g_listen_thread, NULL);
        g_listen_thread_started = false;
    }
    if (g_tls_listen_thread_started) {
        pthread_join(g_tls_listen_thread, NULL);
        g_tls_listen_thread_started = false;
    }
    if (g_workers_started > 0) {
        for (size_t i = 0; i < g_workers_started; i++)
            pthread_join(g_worker_threads[i], NULL);
        g_workers_started = 0;
    }

    pthread_mutex_lock(&g_client_queue_mutex);
    while (g_client_queue_count > 0) {
        struct rpc_conn c = g_client_queue[g_client_queue_head];
        g_client_queue_head =
            (g_client_queue_head + 1) % RPC_HTTP_QUEUE_CAP;
        g_client_queue_count--;
        conn_close(&c);
    }
    g_client_queue_head = 0;
    g_client_queue_tail = 0;
    pthread_mutex_unlock(&g_client_queue_mutex);

    if (g_cookie_rotate_started) {
        pthread_join(g_cookie_rotate_thread, NULL);
        g_cookie_rotate_started = false;
    }

    if (g_cookie_file[0]) {
        unlink(g_cookie_file);
        g_cookie_file[0] = '\0';
    }
    g_table = NULL;
    g_auth_required = false;
    g_cookie_mode = false;
    memory_cleanse(g_rpc_user, sizeof(g_rpc_user));
    memory_cleanse(g_rpc_password, sizeof(g_rpc_password));
    memory_cleanse(g_rpc_password_prev, sizeof(g_rpc_password_prev));
    if (g_rpc_timeout_active) {
        rpc_timeout_stop_watchdog(&g_rpc_timeout);
        rpc_timeout_set_global(NULL);
        rpc_timeout_destroy(&g_rpc_timeout);
        g_rpc_timeout_active = false;
    }
    if (g_middleware_active) {
        rpc_http_middleware_set_global(NULL);
        rpc_http_middleware_destroy(&g_middleware);
        g_middleware_active = false;
    }
    if (g_tls_ctx) {
        SSL_CTX_free(g_tls_ctx);
        g_tls_ctx = NULL;
    }
    printf("RPC server stopped.\n");
}

bool rpc_http_is_running(void)
{
    return g_running;
}

bool rpc_http_tls_active(void)
{
    return g_tls_ctx != NULL && g_running;
}
