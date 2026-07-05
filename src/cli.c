/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zclassic-cli — Native C23 RPC client for zclassic23 node.
 * No curl, no Python, no external dependencies.
 * Reads cookie auth automatically, sends JSON-RPC over HTTP. */

#include "rpc/client.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static char g_cookie[256];
static int g_port = 18232;
static char g_host[64] = "127.0.0.1";

static bool read_cookie(const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/.cookie", datadir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(g_cookie, 1, sizeof(g_cookie) - 1, f);
    fclose(f);
    g_cookie[n] = 0;
    /* Strip newline */
    char *nl = strchr(g_cookie, '\n');
    if (nl) *nl = 0;
    return n > 0;
}

/* Base64 encode for HTTP Basic auth */
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

/* Send JSON-RPC request and return response body.
 * Caller must free() the returned string. */
static char *rpc_call(const char *body, size_t body_len)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to %s:%d\n", g_host, g_port);
        close(sock);
        return NULL;
    }

    /* Build HTTP request */
    char auth_b64[512];
    base64_encode(g_cookie, strlen(g_cookie), auth_b64);

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST / HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", g_host, auth_b64, body_len);

    /* Send header + body */
    send(sock, header, (size_t)hlen, 0);
    send(sock, body, body_len, 0);

    /* Read response */
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(sock); return NULL; }

    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { close(sock); return NULL; }
        }
        ssize_t n = recv(sock, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sock);
    buf[len] = 0;

    /* Skip HTTP headers — find \r\n\r\n */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) return buf;
    body_start += 4;

    /* Move body to start of buffer */
    size_t blen = len - (size_t)(body_start - buf);
    memmove(buf, body_start, blen + 1);
    return buf;
}

int main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    char datadir[512];
    if (home)
        snprintf(datadir, sizeof(datadir), "%s/.zclassic-c23", home);
    else
        snprintf(datadir, sizeof(datadir), ".zclassic-c23");

    /* Parse options before method */
    int arg_start = 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0) {
            snprintf(datadir, sizeof(datadir), "%s", argv[i] + 9);
            arg_start = i + 1;
        } else if (strncmp(argv[i], "-rpcport=", 9) == 0) {
            g_port = atoi(argv[i] + 9);
            arg_start = i + 1;
        } else if (strncmp(argv[i], "-rpcconnect=", 12) == 0) {
            snprintf(g_host, sizeof(g_host), "%s", argv[i] + 12);
            arg_start = i + 1;
        } else {
            break;
        }
    }

    if (arg_start >= argc) {
        fprintf(stderr, "Usage: zclassic-cli [options] <method> [params...]\n");
        fprintf(stderr, "\nExamples:\n");
        fprintf(stderr, "  zclassic-cli getblockcount\n");
        fprintf(stderr, "  zclassic-cli getbalance\n");
        fprintf(stderr, "  zclassic-cli z_gettotalbalance\n");
        fprintf(stderr, "  zclassic-cli chainview 100 5\n");
        fprintf(stderr, "  zclassic-cli z_sendmany \"zs1...\" '[{\"address\":\"zs1...\",\"amount\":0.001}]'\n");
        fprintf(stderr, "  zclassic-cli sendtoaddress t1... 0.01\n");
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, "  -datadir=<dir>       Data directory (default: ~/.zclassic-c23)\n");
        fprintf(stderr, "  -rpcport=<port>      RPC port (default: 18232)\n");
        fprintf(stderr, "  -rpcconnect=<host>   RPC host (default: 127.0.0.1)\n");
        return 1;
    }

    if (!read_cookie(datadir)) {
        fprintf(stderr, "Cannot read cookie from %s/.cookie\n"
                "Is the node running?\n", datadir);
        return 1;
    }

    const char *method = argv[arg_start];
    const char **params = (const char **)&argv[arg_start + 1];
    int nparams = argc - arg_start - 1;

    /* Convert string params to proper JSON types using the convert table */
    struct json_value json_params;
    if (!rpc_convert_values(method, params, (size_t)nparams, &json_params)) {
        fprintf(stderr, "Failed to parse parameters\n");
        return 1;
    }

    /* Build JSON-RPC request */
    char params_buf[32768];
    json_write(&json_params, params_buf, sizeof(params_buf));
    json_free(&json_params);

    char body[65536];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"cli\",\"method\":\"%s\",\"params\":%s}",
        method, params_buf);

    char *response = rpc_call(body, (size_t)blen);
    if (!response) {
        fprintf(stderr, "RPC call failed\n");
        return 1;
    }

    int rc = rpc_cli_print_json_result(response, stdout, stderr);
    free(response);
    return rc;
}
