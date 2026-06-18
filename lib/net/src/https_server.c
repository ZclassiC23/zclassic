/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Public HTTPS server — serves the block explorer.
 * Uses OpenSSL for TLS. HTTP port redirects to HTTPS.
 * Bounded worker pool prevents unbounded detached thread growth.
 *
 * Listens on high ports (8443/8080) to avoid needing root or setcap on the node.
 * For public 443/80 access, a tiny capped userspace forwarder maps the ports
 * (the node stays unprivileged). See tools/zcl_portfwd.c,
 * deploy/systemd/zcl-portfwd.service, and docs/BLOCK_EXPLORER_HOSTING.md. */

#include "net/https_server.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/time.h>
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include "mcp/metrics.h"

static SSL_CTX *g_ssl_ctx = NULL;
static int g_https_fd = -1;
static int g_http_fd = -1;
static pthread_t g_https_thread;
static pthread_t g_http_thread;
static pthread_t g_worker_threads[16];
static unsigned g_worker_threads_started = 0;
static bool g_https_thread_started = false;
static bool g_http_thread_started = false;
static _Atomic bool g_running = false;
static char g_hostname[256] = "";
static pthread_mutex_t g_https_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_client_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_client_queue_cv = PTHREAD_COND_INITIALIZER;

/* Connection limit — prevents OOM under heavy load.
 * Each connection mallocs 512KB for response buffer. */
#define MAX_HTTPS_CONNECTIONS 64
static _Atomic int g_active_connections = 0;
#define HTTPS_CLIENT_QUEUE_CAP 128

struct client_arg {
    int fd;
    bool tls;
};

static struct client_arg g_client_queue[HTTPS_CLIENT_QUEUE_CAP];
static size_t g_client_queue_head = 0;
static size_t g_client_queue_tail = 0;
static size_t g_client_queue_len = 0;

static void close_client_arg(struct client_arg *ca)
{
    if (!ca)
        return;
    if (ca->fd >= 0)
        close(ca->fd);
    ca->fd = -1;
}

static bool client_queue_push(const struct client_arg *ca)
{
    bool ok = false;

    pthread_mutex_lock(&g_client_queue_mutex);
    if (g_client_queue_len < HTTPS_CLIENT_QUEUE_CAP) {
        g_client_queue[g_client_queue_tail] = *ca;
        g_client_queue_tail =
            (g_client_queue_tail + 1U) % HTTPS_CLIENT_QUEUE_CAP;
        g_client_queue_len++;
        ok = true;
    }
    pthread_cond_signal(&g_client_queue_cv);
    pthread_mutex_unlock(&g_client_queue_mutex);
    return ok;
}

static bool client_queue_pop(struct client_arg *ca)
{
    pthread_mutex_lock(&g_client_queue_mutex);
    while (g_client_queue_len == 0 && atomic_load(&g_running))
        pthread_cond_wait(&g_client_queue_cv, &g_client_queue_mutex);

    if (g_client_queue_len == 0) {
        pthread_mutex_unlock(&g_client_queue_mutex);
        return false;
    }

    *ca = g_client_queue[g_client_queue_head];
    g_client_queue_head = (g_client_queue_head + 1U) % HTTPS_CLIENT_QUEUE_CAP;
    g_client_queue_len--;
    pthread_mutex_unlock(&g_client_queue_mutex);
    return true;
}

static void client_queue_close_all(void)
{
    pthread_mutex_lock(&g_client_queue_mutex);
    while (g_client_queue_len > 0) {
        close_client_arg(&g_client_queue[g_client_queue_head]);
        g_client_queue_head = (g_client_queue_head + 1U) % HTTPS_CLIENT_QUEUE_CAP;
        g_client_queue_len--;
    }
    g_client_queue_head = 0;
    g_client_queue_tail = 0;
    pthread_mutex_unlock(&g_client_queue_mutex);
}

/* ── HTTP helpers ─────────────────────────────────────────── */

/* CRLF-trimming line reader shared by the TLS and plaintext request paths.
 * The byte-at-a-time buffering, max-length termination, and \r-stripping are
 * identical on both transports; only the single-byte read primitive differs,
 * so it is supplied as a closure (read one byte into *c; return <=0 on EOF/error). */
typedef int (*read_byte_fn)(void *src, char *c);

static int ssl_read_byte(void *src, char *c) { return SSL_read((SSL *)src, c, 1); }
static int fd_read_byte(void *src, char *c)  { return (int)read(*(int *)src, c, 1); }

static bool read_line(void *src, read_byte_fn rb, char *buf, size_t max)
{
    size_t pos = 0;
    while (pos < max - 1) {
        char c;
        int r = rb(src, &c);
        if (r <= 0) return false;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return true;
}

static bool ssl_read_line(SSL *ssl, char *buf, size_t max)
{
    return read_line(ssl, ssl_read_byte, buf, max);
}

static bool plain_read_line(int fd, char *buf, size_t max)
{
    return read_line(&fd, fd_read_byte, buf, max);
}

/* ── HTTPS handler ────────────────────────────────────────── */

static void handle_https_client(SSL *ssl)
{
    char line[4096];
    if (!ssl_read_line(ssl, line, sizeof(line)))
        return;

    char method[16] = "", path[2048] = "";
    if (sscanf(line, "%15s %2047s", method, path) != 2)
        return;

    /* Read remaining headers (discard) */
    while (ssl_read_line(ssl, line, sizeof(line))) {
        if (line[0] == '\0') break;
    }

    /* Only serve GET requests to explorer routes */
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        const char *resp =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "Only GET is supported.\n";
        SSL_write(ssl, resp, (int)strlen(resp));
        return;
    }

    /* Redirect root to explorer */
    if (strcmp(path, "/") == 0) {
        const char *resp =
            "HTTP/1.1 302 Found\r\n"
            "Location: /explorer\r\n"
            "Connection: close\r\n\r\n";
        SSL_write(ssl, resp, (int)strlen(resp));
        return;
    }

    /* Prometheus /metrics endpoint on HTTPS */
    if (strcmp(path, "/metrics") == 0) {
        size_t cap = 131072;
        char *mbuf = zcl_malloc(cap, "https_metrics_buf");
        if (!mbuf) return;
        size_t n = mcp_metrics_render_prometheus(mbuf, cap);
        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Connection: close\r\n"
            "Content-Length: %zu\r\n\r\n", n);
        SSL_write(ssl, hdr, hlen);
        size_t written = 0;
        while (written < n) {
            size_t chunk = n - written;
            if (chunk > 16384) chunk = 16384;
            int w = SSL_write(ssl, mbuf + written, (int)chunk);
            if (w <= 0) break;
            written += (size_t)w;
        }
        free(mbuf);
        return;
    }

    /* Operator-private API gate — this clearnet 0.0.0.0 listener is
     * untrusted ingress, and the API router cannot authenticate (it
     * never sees headers or peer identity — see api_handle_request),
     * so the gate lives here. The onion service exposes no /api;
     * wallet_gui/zcl-browser call explorer_handle_request in-process
     * and are unaffected. Deliberately no Access-Control-Allow-Origin
     * header on the refusal. */
    if (strncmp(path, "/api", 4) == 0) {
        extern bool api_route_is_operator_private(const char *path);
        if (api_route_is_operator_private(path)) {
            const char *resp =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n\r\n"
                "{\"error\":\"operator-private endpoint: "
                "not served on the public listener\"}";
            SSL_write(ssl, resp, (int)strlen(resp));
            return;
        }
    }

    /* Explorer + API routes — call the explorer handler (which delegates /api/) */
    if (strncmp(path, "/explorer", 9) == 0 ||
        strncmp(path, "/api", 4) == 0) {
        extern size_t explorer_handle_request(const char *, const char *,
            const unsigned char *, size_t, unsigned char *, size_t);

        unsigned char *buf = zcl_malloc(512 * 1024, "https_resp_buf"); /* 512 KB response buffer */
        if (!buf) return;

        size_t n = explorer_handle_request(method, path, NULL, 0, buf, 512 * 1024);
        if (n > 0) {
            /* Write in chunks — SSL_write may not accept large buffers at once */
            size_t written = 0;
            while (written < n) {
                size_t chunk = n - written;
                if (chunk > 16384) chunk = 16384;
                int w = SSL_write(ssl, buf + written, (int)chunk);
                if (w <= 0) break;
                written += (size_t)w;
            }
        } else {
            const char *resp =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n\r\n"
                "Not found.\n";
            SSL_write(ssl, resp, (int)strlen(resp));
        }
        free(buf);
        return;
    }

    /* Anything else → 404 */
    const char *resp =
        "HTTP/1.1 302 Found\r\n"
        "Location: /explorer\r\n"
        "Connection: close\r\n\r\n";
    SSL_write(ssl, resp, (int)strlen(resp));
}

static void handle_https_client_fd(int fd)
{
    atomic_fetch_add(&g_active_connections, 1);

    SSL *ssl = SSL_new(g_ssl_ctx);
    if (!ssl) {
        close(fd);
        atomic_fetch_sub(&g_active_connections, 1);
        return;
    }

    SSL_set_fd(ssl, fd);

    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(fd);
        atomic_fetch_sub(&g_active_connections, 1);
        return;
    }

    handle_https_client(ssl);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    atomic_fetch_sub(&g_active_connections, 1);
}

static void *https_listen_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_https_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running && errno != EINVAL)
                perror("https accept");
            continue;
        }

        /* Reject if too many concurrent connections (prevents OOM) */
        if (atomic_load(&g_active_connections) >= MAX_HTTPS_CONNECTIONS) {
            const char *busy = "HTTP/1.1 503 Service Unavailable\r\n"
                "Retry-After: 5\r\nConnection: close\r\n\r\n";
            write(client_fd, busy, strlen(busy));
            close(client_fd);
            continue;
        }

        /* Slowloris protection: 15s timeout for HTTPS requests.
         * Heavy pages (HODL, stats) are pre-cached so serve instantly. */
        struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct client_arg ca = {
            .fd = client_fd,
            .tls = true,
        };
        if (!client_queue_push(&ca)) {
            const char *busy = "HTTP/1.1 503 Service Unavailable\r\n"
                "Retry-After: 5\r\nConnection: close\r\n\r\n";
            write(client_fd, busy, strlen(busy));
            close(client_fd);
        }
    }
    return NULL;
}

/* ── HTTP redirect handler (port 80) ─────────────────────── */

static void handle_http_client_fd(int fd)
{
    /* Read the request line to get the path */
    char line[4096];
    if (!plain_read_line(fd, line, sizeof(line))) {
        close(fd);
        return;
    }

    char method[16] = "", path[2048] = "";
    sscanf(line, "%15s %2047s", method, path);

    /* Drain headers, capturing the request Host for a generic redirect. */
    char req_host[256] = "";
    while (plain_read_line(fd, line, sizeof(line))) {
        if (line[0] == '\0') break;
        if (req_host[0] == '\0' &&
            strncasecmp(line, "Host:", 5) == 0) {
            const char *v = line + 5;
            while (*v == ' ' || *v == '\t') v++;
            snprintf(req_host, sizeof(req_host), "%s", v);
        }
    }

    /* ACME challenge passthrough for cert renewal */
    if (strncmp(path, "/.well-known/acme-challenge/", 28) == 0) {
        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "/var/www/html%s", path);
        /* Sanitize */
        if (strstr(filepath, "..") == NULL) {
            FILE *f = fopen(filepath, "r");
            if (f) {
                char body[4096];
                size_t n = fread(body, 1, sizeof(body), f);
                fclose(f);
                char hdr[512];
                int hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n\r\n", n);
                (void)write(fd, hdr, (size_t)hlen);
                (void)write(fd, body, n);
                close(fd);
                return;
            }
        }
    }

    /* Redirect everything to HTTPS. Prefer the operator-configured servername
     * (-httpsdomain); else echo the request's own Host header so the redirect
     * works on any domain without a hardcoded host. */
    const char *redir_host = g_hostname[0] ? g_hostname :
                             (req_host[0] ? req_host : NULL);
    char resp[4096];
    int n;
    if (redir_host)
        n = snprintf(resp, sizeof(resp),
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: https://%s%s\r\n"
            "Connection: close\r\n\r\n",
            redir_host, path);
    else
        /* No host known: relative redirect keeps the browser's authority. */
        n = snprintf(resp, sizeof(resp),
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: %s\r\n"
            "Connection: close\r\n\r\n",
            path);
    (void)write(fd, resp, (size_t)n);
    close(fd);
}

static void *http_listen_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_http_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running && errno != EINVAL)
                perror("http accept");
            continue;
        }

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct client_arg ca = {
            .fd = client_fd,
            .tls = false,
        };
        if (!client_queue_push(&ca)) {
            const char *busy = "HTTP/1.1 503 Service Unavailable\r\n"
                "Retry-After: 5\r\nConnection: close\r\n\r\n";
            write(client_fd, busy, strlen(busy));
            close(client_fd);
        }
    }
    return NULL;
}

static void *https_worker_fn(void *arg)
{
    (void)arg;

    while (atomic_load(&g_running)) {
        struct client_arg ca;

        if (!client_queue_pop(&ca))
            break;
        if (ca.fd < 0)
            continue;
        if (ca.tls)
            handle_https_client_fd(ca.fd);
        else
            handle_http_client_fd(ca.fd);
    }

    return NULL;
}

/* ── Bind helper ──────────────────────────────────────────── */

static int bind_port(uint16_t port, bool any_addr)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) LOG_ERR("https", "socket() failed: %s", strerror(errno));

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = any_addr ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        LOG_ERR("https", "bind port %u failed: %s", port, strerror(errno));
    }
    if (listen(fd, 32) < 0) {
        close(fd);
        LOG_ERR("https", "listen on port %u failed: %s", port, strerror(errno));
    }
    return fd;
}

/* ── Public API ───────────────────────────────────────────── */

bool https_server_start_on_port(const char *cert_path, const char *key_path,
                                const char *hostname, int https_port, int http_port)
{
    unsigned started_workers = 0;

    signal(SIGPIPE, SIG_IGN);

    pthread_mutex_lock(&g_https_state_mutex);
    if (atomic_load(&g_running) || g_https_thread_started) {
        pthread_mutex_unlock(&g_https_state_mutex);
        return true;
    }

    if (hostname)
        snprintf(g_hostname, sizeof(g_hostname), "%s", hostname);

    /* Init OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    g_ssl_ctx = SSL_CTX_new(method);
    if (!g_ssl_ctx) {
        ERR_print_errors_fp(stderr);
        pthread_mutex_unlock(&g_https_state_mutex);
        LOG_FAIL("https", "SSL_CTX_new failed");
    }

    /* Set minimum TLS 1.2 */
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(g_ssl_ctx, cert_path) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
        pthread_mutex_unlock(&g_https_state_mutex);
        LOG_FAIL("https", "failed to load cert: %s", cert_path);
    }
    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
        pthread_mutex_unlock(&g_https_state_mutex);
        LOG_FAIL("https", "failed to load private key: %s", key_path);
    }
    if (!SSL_CTX_check_private_key(g_ssl_ctx)) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
        pthread_mutex_unlock(&g_https_state_mutex);
        LOG_FAIL("https", "cert/key mismatch: cert=%s key=%s", cert_path, key_path);
    }

    /* Bind HTTPS port (iptables redirects 443→default 8443) */
    g_https_fd = bind_port(https_port, true);
    if (g_https_fd < 0) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
        pthread_mutex_unlock(&g_https_state_mutex);
        LOG_FAIL("https", "cannot bind HTTPS port %d", https_port);
    }

    /* Bind HTTP port for redirect */
    g_http_fd = bind_port(http_port, true);
    if (g_http_fd < 0) {
        fprintf(stderr, "HTTPS: cannot bind port %d, HTTP redirect won't work\n",  // obs-ok:bind-failure-non-fatal
                http_port);
        /* Non-fatal — continue with HTTPS only */
    }

    atomic_store(&g_running, true);
    g_client_queue_head = 0;
    g_client_queue_tail = 0;
    g_client_queue_len = 0;
    atomic_store(&g_active_connections, 0);

    for (unsigned i = 0; i < (sizeof(g_worker_threads) / sizeof(g_worker_threads[0])); i++) {
        if (thread_registry_spawn_ex("zcl_https_wkr", https_worker_fn,
                                      NULL, &g_worker_threads[i]) != 0) {
            fprintf(stderr, "HTTPS: worker thread failed\n");  // obs-ok:thread-spawn-fallback-logged
            break;
        }
        started_workers++;
    }
    g_worker_threads_started = started_workers;
    if (g_worker_threads_started == 0) {
        atomic_store(&g_running, false);
        close(g_https_fd);
        g_https_fd = -1;
        if (g_http_fd >= 0) {
            close(g_http_fd);
            g_http_fd = -1;
        }
        if (g_ssl_ctx) {
            SSL_CTX_free(g_ssl_ctx);
            g_ssl_ctx = NULL;
        }
        pthread_mutex_unlock(&g_https_state_mutex);
        LOG_FAIL("https", "no worker threads could be started");
    }

    if (thread_registry_spawn_ex("zcl_https_listen", https_listen_fn, NULL,
                                  &g_https_thread) != 0) {
        close(g_https_fd);
        g_https_fd = -1;
        atomic_store(&g_running, false);
        pthread_cond_broadcast(&g_client_queue_cv);
        pthread_mutex_unlock(&g_https_state_mutex);
        for (unsigned i = 0; i < g_worker_threads_started; i++)
            pthread_join(g_worker_threads[i], NULL);
        g_worker_threads_started = 0;
        if (g_ssl_ctx) {
            SSL_CTX_free(g_ssl_ctx);
            g_ssl_ctx = NULL;
        }
        LOG_FAIL("https", "thread_registry_spawn_ex failed for HTTPS listen thread");
    }
    g_https_thread_started = true;

    if (g_http_fd >= 0) {
        if (thread_registry_spawn_ex("zcl_http_listen", http_listen_fn, NULL,
                                      &g_http_thread) != 0) {
            fprintf(stderr, "HTTPS: HTTP redirect thread failed\n");  // obs-ok:thread-spawn-fallback-logged
            close(g_http_fd);
            g_http_fd = -1;
        } else {
            g_http_thread_started = true;
        }
    }
    pthread_mutex_unlock(&g_https_state_mutex);

    printf("HTTPS server listening on 0.0.0.0:%d (TLS)\n", https_port);
    if (g_http_fd >= 0)
        printf("HTTP redirect on 0.0.0.0:%d -> https://%s\n", http_port, g_hostname);

    return true;
}

bool https_server_start(const char *cert_path, const char *key_path,
                         const char *hostname)
{
    return https_server_start_on_port(cert_path, key_path, hostname, 8443, 8080);
}

void https_server_stop(void)
{
    pthread_t https_thread;
    pthread_t http_thread;
    pthread_t worker_threads[sizeof(g_worker_threads) / sizeof(g_worker_threads[0])];
    unsigned worker_threads_started = 0;
    bool have_https_thread = false;
    bool have_http_thread = false;
    int https_fd = -1;
    int http_fd = -1;

    pthread_mutex_lock(&g_https_state_mutex);
    if (!atomic_load(&g_running) && !g_https_thread_started &&
        !g_http_thread_started && g_worker_threads_started == 0) {
        pthread_mutex_unlock(&g_https_state_mutex);
        return;
    }
    atomic_store(&g_running, false);
    https_fd = g_https_fd;
    http_fd = g_http_fd;
    g_https_fd = -1;
    g_http_fd = -1;
    if (g_https_thread_started) {
        https_thread = g_https_thread;
        g_https_thread_started = false;
        have_https_thread = true;
    }
    if (g_http_thread_started) {
        http_thread = g_http_thread;
        g_http_thread_started = false;
        have_http_thread = true;
    }
    worker_threads_started = g_worker_threads_started;
    for (unsigned i = 0; i < worker_threads_started; i++)
        worker_threads[i] = g_worker_threads[i];
    g_worker_threads_started = 0;
    pthread_mutex_unlock(&g_https_state_mutex);

    if (https_fd >= 0) {
        shutdown(https_fd, SHUT_RDWR);
        close(https_fd);
    }
    if (http_fd >= 0) {
        shutdown(http_fd, SHUT_RDWR);
        close(http_fd);
    }
    pthread_cond_broadcast(&g_client_queue_cv);
    client_queue_close_all();

    if (have_https_thread)
        pthread_join(https_thread, NULL);
    if (have_http_thread)
        pthread_join(http_thread, NULL);
    for (unsigned i = 0; i < worker_threads_started; i++)
        pthread_join(worker_threads[i], NULL);
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    printf("HTTPS server stopped.\n");
}

/* ── Deferred HTTPS start (after IBD completes) ──────────── */

static char g_deferred_cert[1024];
static char g_deferred_key[1024];
static char g_deferred_host[256];
static _Atomic bool g_deferred_pending = false;

void https_deferred_set(const char *cert, const char *key, const char *hostname)
{
    strncpy(g_deferred_cert, cert, sizeof(g_deferred_cert) - 1);
    strncpy(g_deferred_key, key, sizeof(g_deferred_key) - 1);
    if (hostname && hostname[0])
        snprintf(g_deferred_host, sizeof(g_deferred_host), "%s", hostname);
    else
        g_deferred_host[0] = '\0';
    atomic_store(&g_deferred_pending, true);
    printf("HTTPS: deferred start queued (will start when synced)\n");
}

void https_deferred_check(void)
{
    if (atomic_load(&g_deferred_pending) && !g_running) {
        atomic_store(&g_deferred_pending, false);
        printf("HTTPS: starting deferred server (node synced)\n");
        /* hostname NULL when the operator did not set -httpsdomain; with a
         * single cert the presented cert is the same regardless of SNI. */
        https_server_start(g_deferred_cert, g_deferred_key,
                           g_deferred_host[0] ? g_deferred_host : NULL);
    }
}
