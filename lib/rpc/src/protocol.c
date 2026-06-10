/* Copyright (c) 2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/protocol.h"
#include "core/random.h"
#include "encoding/utilstrencodings.h"
#include "util/util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char COOKIEAUTH_USER[] = "__cookie__";
static const char COOKIEAUTH_FILE[] = ".cookie";

size_t json_rpc_request(const char *method,
                        const struct json_value *params,
                        const struct json_value *id,
                        char *out, size_t out_size)
{
    struct json_value req;
    json_init(&req);
    json_set_object(&req);
    json_push_kv_str(&req, "method", method);
    json_push_kv(&req, "params", params);
    json_push_kv(&req, "id", id);
    size_t n = json_write(&req, out, out_size > 1 ? out_size - 2 : 0);
    if (n + 1 < out_size) { out[n] = '\n'; out[n + 1] = '\0'; }
    json_free(&req);
    return n + 1;
}

static void json_rpc_reply_obj(struct json_value *reply,
                               const struct json_value *result,
                               const struct json_value *error,
                               const struct json_value *id)
{
    json_init(reply);
    json_set_object(reply);
    if (!json_is_null(error)) {
        struct json_value null_val;
        json_init(&null_val);
        json_push_kv(reply, "result", &null_val);
    } else {
        json_push_kv(reply, "result", result);
    }
    json_push_kv(reply, "error", error);
    json_push_kv(reply, "id", id);
}

size_t json_rpc_reply(const struct json_value *result,
                      const struct json_value *error,
                      const struct json_value *id,
                      char *out, size_t out_size)
{
    struct json_value reply;
    json_rpc_reply_obj(&reply, result, error, id);
    size_t n = json_write(&reply, out, out_size > 1 ? out_size - 2 : 0);
    if (n + 1 < out_size) { out[n] = '\n'; out[n + 1] = '\0'; }
    json_free(&reply);
    return n + 1;
}

void json_rpc_error(struct json_value *out, int code, const char *message)
{
    json_init(out);
    json_set_object(out);
    json_push_kv_int(out, "code", code);
    json_push_kv_str(out, "message", message);
}

void json_rpc_error_full(struct json_value *out, int code,
                         const char *message, const char *method)
{
    json_init(out);
    json_set_object(out);
    json_push_kv_int(out, "code", code);
    json_push_kv_str(out, "message", message);
    if (method)
        json_push_kv_str(out, "method", method);
}

size_t json_rpc_error_response(char *buf, size_t buflen, int code,
                               const char *message, const char *method,
                               const char *id_json)
{
    if (!buf || buflen == 0) return 0;
    size_t pos = 0;
    int n;
    if (method) {
        n = snprintf(buf + pos, buflen - pos,
                     "{\"result\":null,\"error\":{\"code\":%d,"
                     "\"message\":\"%s\",\"method\":\"%s\"},\"id\":%s}",
                     code, message ? message : "",
                     method, id_json ? id_json : "null");
    } else {
        n = snprintf(buf + pos, buflen - pos,
                     "{\"result\":null,\"error\":{\"code\":%d,"
                     "\"message\":\"%s\"},\"id\":%s}",
                     code, message ? message : "",
                     id_json ? id_json : "null");
    }
    if (n < 0) return 0;
    return (size_t)n < buflen ? (size_t)n : buflen - 1;
}

void get_auth_cookie_file(char *out, size_t out_size)
{
    const char *custom = GetArg("-rpccookiefile", COOKIEAUTH_FILE);
    if (custom[0] == '/') {
        snprintf(out, out_size, "%s", custom);
    } else {
        char datadir[1024];
        GetDataDir(false, datadir, sizeof(datadir));
        snprintf(out, out_size, "%s/%s", datadir, custom);
    }
}

bool generate_auth_cookie(char *cookie_out, size_t cookie_size)
{
    unsigned char rand_pwd[32];
    GetRandBytes(rand_pwd, 32);

    char b64[64];
    EncodeBase64(rand_pwd, 32, b64, sizeof(b64));

    char cookie[128];
    snprintf(cookie, sizeof(cookie), "%s:%s", COOKIEAUTH_USER, b64);

    char filepath[1024];
    get_auth_cookie_file(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "w");
    if (!f) {
        LogPrintf("Unable to open cookie authentication file %s for writing\n",
                  filepath);
        return false;
    }
    fputs(cookie, f);
    fclose(f);
    LogPrintf("Generated RPC authentication cookie %s\n", filepath);

    if (cookie_out)
        snprintf(cookie_out, cookie_size, "%s", cookie);
    return true;
}

bool get_auth_cookie(char *cookie_out, size_t cookie_size)
{
    char filepath[1024];
    get_auth_cookie_file(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "r");
    if (!f)
        return false;

    char cookie[256];
    if (!fgets(cookie, (int)sizeof(cookie), f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    size_t len = strlen(cookie);
    while (len > 0 && (cookie[len - 1] == '\n' || cookie[len - 1] == '\r'))
        cookie[--len] = '\0';

    if (cookie_out)
        snprintf(cookie_out, cookie_size, "%s", cookie);
    return true;
}

void delete_auth_cookie(void)
{
    char filepath[1024];
    get_auth_cookie_file(filepath, sizeof(filepath));
    unlink(filepath);
}
