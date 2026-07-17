/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP RPC client: speaks HTTP+JSON-RPC to the local zclassic23 node.
 * See rpc_client.h for the public API. */

#include "controllers/rpc_client.h"

#include "json/json.h"
#include "rpc/server.h"
#include "rpc/httpserver.h"

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

/* Selectable transport backend. Defaults to the out-of-process HTTP path
 * so the existing -mcp proxy is byte-for-byte unchanged unless the
 * in-process mode explicitly flips it via mcp_rpc_client_use_inprocess(). */
typedef char *(*mcp_rpc_backend_fn)(const char *method,
                                    const char *params_json);
static mcp_rpc_backend_fn g_rpc_backend = mcp_node_rpc_http;

static char g_cookie[256];
static int g_port = 18232;
static char g_datadir[512];

#ifdef ZCL_TESTING
static mcp_node_rpc_test_fn g_test_rpc_hook;

void mcp_rpc_client_set_test_hook(mcp_node_rpc_test_fn fn)
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
 * node restart that rotates the cookie is picked up by the long-lived MCP
 * server.  Returns false (and clears any stale cookie) if the file is
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
 * Heap-allocated like every other mcp_node_rpc return value; caller frees.
 * Naming the cookie path turns a cryptic 401 into an actionable message
 * ("is the node running? is -datadir correct?"). */
static char *cookie_error_body(void)
{
    char *out = zcl_malloc(768, "mcp rpc cookie error json");
    if (!out) return NULL;
    snprintf(out, 768,
        "{\"error\":{\"code\":-32603,\"message\":"
        "\"cannot read RPC auth cookie at %s/.cookie — is the node "
        "running and is the MCP -datadir correct? (proceeding would "
        "send an empty credential and 401)\"}}",
        g_datadir[0] ? g_datadir : "(unset datadir)");
    return out;
}

void mcp_rpc_client_init(const char *datadir, int rpc_port)
{
    snprintf(g_datadir, sizeof(g_datadir), "%s", datadir ? datadir : "");
    g_port = rpc_port;
}

const char *mcp_rpc_client_datadir(void)
{
    return g_datadir;
}

char *mcp_node_rpc_http(const char *method, const char *params_json)
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
     * SIGPIPE and kill the MCP process. Treat any short/failed write as
     * fatal for this request — a truncated POST yields a bogus reply. */
    if (send(sock, header, (size_t)hlen, MSG_NOSIGNAL) != (ssize_t)hlen ||
        send(sock, body,   (size_t)blen, MSG_NOSIGNAL) != (ssize_t)blen) {
        close(sock);
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"failed to send request to node\"}}");
    }

    size_t cap = 65536, len = 0;
    char *buf = zcl_malloc(cap, "mcp rpc response buf");
    if (!buf) { close(sock); return NULL; }
    for (;;) {
        if (len + 4096 > cap) {
            size_t newcap = cap * 2;
            char *tmp = zcl_realloc(buf, newcap, "mcp rpc response buf");
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
            char *out = zcl_malloc(4096, "mcp rpc error json");
            if (!out) { json_free(&v); free(buf); return NULL; }
            json_write(err, out, 4096);
            json_free(&v);
            free(buf);
            return out;
        }
        if (res) {
            char *out = zcl_malloc(cap, "mcp rpc result json");
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

/* In-process backend. Instead of opening a socket and POSTing JSON-RPC to
 * 127.0.0.1, call the node's own rpc_table_execute() — the SAME function
 * lib/rpc/src/httpserver.c invokes — on the live dispatch table, then
 * project the result into the identical malloc'd JSON shape mcp_node_rpc_http
 * returns: the bare "result" value on success, or the "error" object on
 * failure.  We build the full JSON-RPC envelope with the same helper the
 * HTTP server uses (rpc_http_test_build_response_envelope) and extract from
 * it exactly the way mcp_node_rpc_http parses the wire reply, so the bytes
 * handed back to every controller are byte-for-byte what the proxy returns. */
char *mcp_node_rpc_inproc(const char *method, const char *params_json)
{
    const char *mname = method ? method : "(null)";
    const struct rpc_table *table = rpc_http_active_table();
    if (!table) {
        /* The in-process path needs a fully booted node (rpc_http_start
         * has run and wired the table). Mirror the HTTP "cannot reach the
         * node" envelope so downstream parsing is unaffected. (We log via
         * fprintf, not LOG_FAIL, because LOG_FAIL returns false — wrong for
         * a pointer-returning function — and we must still hand back a
         * proper error body.) */
        // obs-ok:inproc-rpc-not-ready (best-effort stderr; returns a JSON -32603 error envelope)
        fprintf(stderr, "[mcp] %s:%d %s(): in-process RPC for '%s' before "
                "rpc_table is live\n", __FILE__, __LINE__, __func__, mname);
        return strdup("{\"error\":{\"code\":-32603,"
                      "\"message\":\"node not ready: RPC table not live "
                      "(in-process MCP requires a booted node)\"}}");
    }

    /* Parse params_json (NULL/empty → []) into a JSON array value, exactly
     * the shape rpc_table_execute receives from the HTTP request parser. */
    struct json_value params;
    json_init(&params);
    if (params_json && params_json[0]) {
        if (!json_read(&params, params_json, strlen(params_json))) {
            json_free(&params);
            // obs-ok:inproc-rpc-bad-params (best-effort stderr; returns a JSON -32602 error envelope)
            fprintf(stderr, "[mcp] %s:%d %s(): in-process RPC '%s' bad "
                    "params JSON\n", __FILE__, __LINE__, __func__, mname);
            return strdup("{\"error\":{\"code\":-32602,"
                          "\"message\":\"invalid params JSON\"}}");
        }
    } else {
        json_set_array(&params);
    }

    struct json_value result;
    json_init(&result);
    bool rpc_ok = rpc_table_execute(table, method, &params, &result);

    /* Reuse the HTTP server's envelope builder so the success/error
     * projection (including the "method" key it appends to error objects)
     * is identical to the wire path. id is irrelevant to the extracted
     * result/error, but the builder requires one. */
    struct json_value id;
    json_init(&id);
    json_set_int(&id, 1);

    struct json_value envelope;
    json_init(&envelope);
    bool built = rpc_http_test_build_response_envelope(
        rpc_ok, method, &result, &id, &envelope);

    char *out = NULL;
    if (built) {
        const struct json_value *err = json_get(&envelope, "error");
        const struct json_value *res = json_get(&envelope, "result");
        /* Same precedence as mcp_node_rpc_http: a non-null error wins. */
        const struct json_value *pick =
            (err && err->type != JSON_NULL) ? err : res;
        if (pick) {
            size_t need = json_write(pick, NULL, 0) + 1;
            out = zcl_malloc(need, "mcp inproc rpc json");
            if (out)
                json_write(pick, out, need);
            else
                // obs-ok:inproc-rpc-alloc-fail (best-effort stderr; returns NULL, caller emits its own error)
                fprintf(stderr, "[mcp] %s:%d %s(): in-process RPC '%s' "
                        "result alloc failed\n", __FILE__, __LINE__,
                        __func__, mname);
        }
    }

    json_free(&envelope);
    json_free(&id);
    json_free(&result);
    json_free(&params);

    if (!out) {
        /* Never hand a controller NULL on a path the HTTP backend would
         * have answered — always set an error body. */
        out = strdup("{\"error\":{\"code\":-32603,"
                     "\"message\":\"in-process RPC failed to build "
                     "response\"}}");
    }
    return out;
}

void mcp_rpc_client_use_inprocess(void)
{
    g_rpc_backend = mcp_node_rpc_inproc;
}

/* Public entry every controller and the diagnostics dumper call. Routes to
 * the selected backend; the ZCL_TESTING hook still wins so the controller
 * equivalence tests can stub the node out regardless of transport. */
char *mcp_node_rpc(const char *method, const char *params_json)
{
#ifdef ZCL_TESTING
    if (g_test_rpc_hook)
        return g_test_rpc_hook(method, params_json);
#endif
    return g_rpc_backend(method, params_json);
}
