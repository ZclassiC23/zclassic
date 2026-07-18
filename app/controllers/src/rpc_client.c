/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Loopback RPC client: speaks HTTP+JSON-RPC to the local zclassic23 node.
 * Native command handlers and the tools/command CLI are its consumers.
 * See controllers/rpc_client.h for the public API. */

#include "controllers/rpc_client.h"

#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "util/safe_alloc.h"

static char g_cookie[256];
static int g_port = 18232;
static char g_datadir[512];

#ifdef ZCL_TESTING
static node_rpc_test_fn g_test_rpc_hook;

void node_rpc_client_set_test_hook(node_rpc_test_fn fn)
{
    g_test_rpc_hook = fn;
}
#endif

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const char *in, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint8_t a = (uint8_t)in[i], b = (uint8_t)in[i+1], c = (uint8_t)in[i+2];
        out[j++] = b64[a >> 2];
        out[j++] = b64[((a & 3) << 4) | (b >> 4)];
        out[j++] = b64[((b & 0xf) << 2) | (c >> 6)];
        out[j++] = b64[c & 0x3f];
    }
    if (i < len) {
        uint8_t a = (uint8_t)in[i];
        out[j++] = b64[a >> 2];
        if (i + 1 < len) {
            uint8_t b2 = (uint8_t)in[i+1];
            out[j++] = b64[((a & 3) << 4) | (b2 >> 4)];
            out[j++] = b64[(b2 & 0xf) << 2];
        } else {
            out[j++] = b64[(a & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = 0;
}

/* Re-read the RPC auth cookie from <datadir>/.cookie on EVERY call so a
 * node restart that rotates the cookie is picked up by long-lived callers.
 * Returns false (and clears any stale cookie) if the file is
 * missing/unreadable/empty — callers MUST NOT proceed to send a request
 * with an empty credential, because the node then answers 401 Unauthorized
 * with no hint that the real cause is a cookie problem.  That silent 401
 * masquerades as a generic auth failure on every tool. */
static bool read_cookie(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.cookie", g_datadir);
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Drop any previously-loaded cookie so we never send a stale
         * credential after the file goes away (e.g. datadir unmounted). */
        g_cookie[0] = 0;
        return false;
    }
    size_t n = fread(g_cookie, 1, sizeof(g_cookie) - 1, f);
    fclose(f);
    g_cookie[n] = 0;
    char *nl = strchr(g_cookie, '\n');
    if (nl) *nl = 0;
    if (g_cookie[0] == 0)
        return false;
    return n > 0;
}

/* Build a self-describing error body for the "no usable auth cookie" case.
 * Heap-allocated like every other node_rpc_call return value; caller frees.
 * Naming the cookie path turns a cryptic 401 into an actionable message
 * ("is the node running? is -datadir correct?"). */
static char *cookie_error_body(void)
{
    char *out = zcl_malloc(768, "rpc cookie error json");
    if (!out) return NULL;
    snprintf(out, 768,
        "{\"error\":{\"code\":-32603,\"message\":"
        "\"cannot read RPC auth cookie at %s/.cookie — is the node "
        "running and is the selected datadir correct? (proceeding would "
        "send an empty credential and 401)\"}}",
        g_datadir[0] ? g_datadir : "(unset datadir)");
    return out;
}

void node_rpc_client_init(const char *datadir, int rpc_port)
{
    snprintf(g_datadir, sizeof(g_datadir), "%s", datadir ? datadir : "");
    g_port = rpc_port;
}

const char *node_rpc_client_datadir(void)
{
    return g_datadir;
}

char *node_rpc_call_http(const char *method, const char *params_json)
{
    /* Fail fast with an actionable message rather than sending an empty
     * credential that the node would reject with a cryptic 401. */
    if (!read_cookie())
        return cookie_error_body();

    char body[8192];
    int blen;
    if (params_json && params_json[0])
        blen = snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
            method, params_json);
    else
        blen = snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":[]}",
            method);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"socket failed\"}}");

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"cannot connect to node\"}}");
    }

    char auth_b64[512];
    base64_encode(g_cookie, strlen(g_cookie), auth_b64);

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", auth_b64, blen);

    /* MSG_NOSIGNAL: a peer reset mid-send must return EPIPE, not raise
     * SIGPIPE and kill the caller. Treat any short/failed write as
     * fatal for this request — a truncated POST yields a bogus reply. */
    if (send(sock, header, (size_t)hlen, MSG_NOSIGNAL) != (ssize_t)hlen ||
        send(sock, body,   (size_t)blen, MSG_NOSIGNAL) != (ssize_t)blen) {
        close(sock);
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"failed to send request to node\"}}");
    }

    size_t cap = 65536, len = 0;
    char *buf = zcl_malloc(cap, "rpc response buf");
    if (!buf) { close(sock); return NULL; }
    for (;;) {
        if (len + 4096 > cap) {
            size_t newcap = cap * 2;
            char *tmp = zcl_realloc(buf, newcap, "rpc response buf");
            if (!tmp) { free(buf); close(sock); return NULL; }
            buf = tmp;
            cap = newcap;
        }
        ssize_t n = recv(sock, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sock);
    buf[len] = 0;

    /* Skip HTTP headers */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t bslen = len - (size_t)(body_start - buf);
        memmove(buf, body_start, bslen + 1);
    }

    /* Extract "result" from {"result":...,"error":null,"id":1} */
    struct json_value v;
    if (json_read(&v, buf, strlen(buf))) {
        const struct json_value *res = json_get(&v, "result");
        const struct json_value *err = json_get(&v, "error");
        if (err && err->type != JSON_NULL) {
            char *out = zcl_malloc(4096, "rpc error json");
            if (!out) { json_free(&v); free(buf); return NULL; }
            json_write(err, out, 4096);
            json_free(&v);
            free(buf);
            return out;
        }
        if (res) {
            char *out = zcl_malloc(cap, "rpc result json");
            if (!out) { json_free(&v); free(buf); return NULL; }
            json_write(res, out, cap);
            json_free(&v);
            free(buf);
            return out;
        }
        json_free(&v);
    }
    return buf;
}

/* Public entry every controller and the diagnostics dumper call. Routes to
 * the HTTP backend; the test hook lets controller tests stub the node. */
char *node_rpc_call(const char *method, const char *params_json)
{
#ifdef ZCL_TESTING
    if (g_test_rpc_hook)
        return g_test_rpc_hook(method, params_json);
#endif
    return node_rpc_call_http(method, params_json);
}
