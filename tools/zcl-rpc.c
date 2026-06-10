/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcl-rpc: lightweight RPC client for zclassic23.
 * Reads cookie auth, sends JSON-RPC, prints result.
 * Usage: zcl-rpc <method> [param1] [param2] ... */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int rpc_call(const char *host, int port, const char *cookie,
                    const char *method, const char *params_json,
                    char *out, size_t out_len)
{
    char body[8192];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"method\":\"%s\",\"params\":[%s],\"id\":1}",
        method, params_json ? params_json : "");

    /* Write body to temp file to avoid shell quoting issues */
    char tmpf[] = "/tmp/zcl-rpc-XXXXXX";
    int tfd = mkstemp(tmpf);
    if (tfd < 0) return -1;
    write(tfd, body, strlen(body));
    close(tfd);

    char cmd[16384];
    snprintf(cmd, sizeof(cmd),
        "curl -s --max-time 30 --user \"%s\" "
        "-d @%s -H 'content-type:text/plain;' "
        "http://%s:%d/ 2>/dev/null",
        cookie, tmpf, host, port);

    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    size_t total = fread(out, 1, out_len - 1, p);
    out[total] = '\0';
    pclose(p);
    unlink(tmpf);
    return (int)total;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: zcl-rpc <method> [params...]\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  zcl-rpc getblockcount\n");
        fprintf(stderr, "  zcl-rpc getbalance\n");
        fprintf(stderr, "  zcl-rpc sendtoaddress '\"t1addr...\", 0.1'\n");
        return 1;
    }

    /* Read cookie */
    const char *home = getenv("HOME");
    char cookie_path[512];
    const char *datadir = getenv("ZCL_DATADIR");
    if (datadir)
        snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", datadir);
    else if (home)
        snprintf(cookie_path, sizeof(cookie_path), "%s/.zclassic-c23/.cookie", home);
    else
        snprintf(cookie_path, sizeof(cookie_path), ".zclassic-c23/.cookie");

    char cookie[256] = "";

    /* Try cookie auth first (C23 node uses cookies) */
    {
        FILE *cf = fopen(cookie_path, "r");
        if (cf) {
            if (fgets(cookie, sizeof(cookie), cf)) {
                char *nl = strchr(cookie, '\n');
                if (nl) *nl = '\0';
            }
            fclose(cf);
        }
    }

    /* Fall back to rpcuser:rpcpassword from conf (zclassicd) */
    if (cookie[0] == '\0')
    {
        char conf_path[512];
        if (datadir)
            snprintf(conf_path, sizeof(conf_path), "%s/zclassic.conf", datadir);
        else if (home)
            snprintf(conf_path, sizeof(conf_path), "%s/.zclassic-c23/zclassic.conf", home);
        else
            snprintf(conf_path, sizeof(conf_path), "zclassic.conf");

        FILE *f = fopen(conf_path, "r");
        if (f) {
            char user[128] = "", pass[128] = "";
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "rpcuser=", 8) == 0) {
                    snprintf(user, sizeof(user), "%s", line + 8);
                    char *nl = strchr(user, '\n'); if (nl) *nl = '\0';
                }
                if (strncmp(line, "rpcpassword=", 12) == 0) {
                    snprintf(pass, sizeof(pass), "%s", line + 12);
                    char *nl = strchr(pass, '\n'); if (nl) *nl = '\0';
                }
            }
            fclose(f);
            if (user[0] && pass[0])
                snprintf(cookie, sizeof(cookie), "%s:%s", user, pass);
        }
    }

    if (cookie[0] == '\0') {
        fprintf(stderr, "No auth found (no cookie, no rpcuser/rpcpassword)\n");
        return 1;
    }

    /* Build params JSON from remaining args */
    char params[4096] = "";
    if (argc > 2) {
        /* If single arg with commas, pass as-is */
        if (argc == 3) {
            snprintf(params, sizeof(params), "%s", argv[2]);
        } else {
            /* Multiple args — join with commas */
            size_t off = 0;
            for (int i = 2; i < argc && off < sizeof(params) - 2; i++) {
                if (i > 2) params[off++] = ',';
                off += (size_t)snprintf(params + off, sizeof(params) - off, "%s", argv[i]);
            }
        }
    }

    /* Override the default RPC port via env. Matches the documentation
     * in README.md (Environment variables section). atoi() returns 0
     * for unparseable values; we keep the literal default in that
     * case rather than connecting to port 0. */
    int port = 18232;
    const char *port_env = getenv("ZCL_RPCPORT");
    if (port_env) {
        int parsed = atoi(port_env);
        if (parsed > 0 && parsed <= 65535) port = parsed;
    }

    char response[1024 * 1024];
    int n = rpc_call("127.0.0.1", port, cookie, argv[1], params,
                     response, sizeof(response));
    if (n < 0) {
        fprintf(stderr, "Connection failed (port %d)\n", port);
        return 1;
    }

    /* If we got "Unauthorized", retry with conf auth */
    if (n == 0 || strstr(response, "Unauthorized")) {
        char conf_path2[512];
        if (datadir)
            snprintf(conf_path2, sizeof(conf_path2), "%s/zclassic.conf", datadir);
        else if (home)
            snprintf(conf_path2, sizeof(conf_path2), "%s/.zclassic-c23/zclassic.conf", home);
        else
            snprintf(conf_path2, sizeof(conf_path2), "zclassic.conf");
        FILE *f2 = fopen(conf_path2, "r");
        if (f2) {
            char user2[128] = "", pass2[128] = "", line2[256];
            while (fgets(line2, sizeof(line2), f2)) {
                if (strncmp(line2, "rpcuser=", 8) == 0)
                    { snprintf(user2, sizeof(user2), "%s", line2+8); char *nl=strchr(user2,'\n'); if(nl)*nl='\0'; }
                if (strncmp(line2, "rpcpassword=", 12) == 0)
                    { snprintf(pass2, sizeof(pass2), "%s", line2+12); char *nl=strchr(pass2,'\n'); if(nl)*nl='\0'; }
            }
            fclose(f2);
            if (user2[0] && pass2[0]) {
                char fallback[256];
                snprintf(fallback, sizeof(fallback), "%s:%s", user2, pass2);
                n = rpc_call("127.0.0.1", port, fallback, argv[1], params,
                             response, sizeof(response));
            }
        }
    }

    if (n > 0)
        printf("%s\n", response);
    return n > 0 ? 0 : 1;
}
