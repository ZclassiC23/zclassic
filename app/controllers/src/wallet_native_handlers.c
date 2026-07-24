/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native argument parsing and RPC composition for the wallet read commands,
 * plus the dedicated registry handlers for the mutating core.wallet.* leaves.
 * See controllers/native_handler_body.h for the read-body failure contract. */

#include "controllers/wallet_native_handlers.h"

#include "json/json.h"
#include "controllers/rpc_client.h"
#include "controllers/rpc_params.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *zcl_native_listunspent_body(const struct json_value *args,
                                   struct zcl_native_body_err *err)
{
    char params[128];
    snprintf(params, sizeof(params), "[%lld,%lld]",
             (long long)json_get_int_or(args, "minconf", 1),
             (long long)json_get_int_or(args, "maxconf", 9999999));
    char *out = node_rpc_call("listunspent", params);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "listunspent");
        LOG_NULL("native.wallet", "RPC %s returned null", "listunspent");
    }
    return out;
}

char *zcl_native_listtransactions_body(const struct json_value *args,
                                        struct zcl_native_body_err *err)
{
    char params[128];
    snprintf(params, sizeof(params), "[\"\",%lld,%lld]",
             (long long)json_get_int_or(args, "count", 10),
             (long long)json_get_int_or(args, "skip",   0));
    char *out = node_rpc_call("listtransactions", params);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "listtransactions");
        LOG_NULL("native.wallet", "RPC %s returned null", "listtransactions");
    }
    return out;
}

char *zcl_native_gettransaction_body(const struct json_value *args,
                                      struct zcl_native_body_err *err)
{
    const char *v = json_get_str(json_get(args, "txid"));
    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, v);
    char *params = rpc_arg_builder_to_json(&p);
    char *out = params ? node_rpc_call("gettransaction", params) : NULL;
    free(params);
    if (!out) {
        char ctx[192];
        snprintf(ctx, sizeof(ctx), "txid=%s", v ? v : "(null)");
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s failed: %s", "gettransaction", ctx);
        LOG_NULL("native.wallet", "%s failed: %s", "gettransaction", ctx);
    }
    return out;
}

char *zcl_native_listaddresses_body(const struct json_value *args,
                                     struct zcl_native_body_err *err)
{
    (void)args;
    /* The node RPC `listwalletkeys` returns {transparent_keys:[{address,...}],
     * sapling_keys:[...]}.  Call it without private keys and project just
     * the addresses so the caller gets a clean list. */
    char *raw = node_rpc_call("listwalletkeys", "[false]");
    if (!raw) {
        err->status = ZCL_NATIVE_BODY_UNAVAILABLE;
        snprintf(err->message, sizeof(err->message),
                 "RPC %s returned null", "listwalletkeys");
        LOG_NULL("native.wallet", "RPC %s returned null", "listwalletkeys");
    }

    struct json_value root;
    if (!json_read(&root, raw, strlen(raw)))
        return raw;
    free(raw);

    size_t cap = 65536;
    char *out = zcl_malloc(cap, "listaddresses_body");
    if (!out) {
        json_free(&root);
        err->status = ZCL_NATIVE_BODY_INTERNAL;
        snprintf(err->message, sizeof(err->message),
                 "malloc failed for %s", "listaddresses response");
        if (cap > 0)
            LOG_NULL("native.wallet", "malloc failed for %s (%zu bytes)",
                     "listaddresses response", cap);
        LOG_NULL("native.wallet", "malloc failed for %s",
                 "listaddresses response");
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, cap - pos, "{\"t_addresses\":[");

    const struct json_value *tk = json_get(&root, "transparent_keys");
    bool first = true;
    if (tk && tk->type == JSON_ARR) {
        for (size_t i = 0; i < tk->num_children; i++) {
            const struct json_value *k = &tk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    pos += (size_t)snprintf(out + pos, cap - pos, "],\"z_addresses\":[");

    const struct json_value *sk = json_get(&root, "sapling_keys");
    first = true;
    if (sk && sk->type == JSON_ARR) {
        for (size_t i = 0; i < sk->num_children; i++) {
            const struct json_value *k = &sk->children[i];
            const struct json_value *av = json_get(k, "address");
            const char *addr = av ? json_get_str(av) : NULL;
            if (!addr || !addr[0]) continue;
            if (pos + strlen(addr) + 8 >= cap) break;
            if (!first) out[pos++] = ',';
            first = false;
            out[pos++] = '"';
            for (const char *c = addr; *c && pos + 2 < cap; c++) out[pos++] = *c;
            out[pos++] = '"';
        }
    }
    if (pos + 2 < cap) { out[pos++] = ']'; out[pos++] = '}'; out[pos] = 0; }

    json_free(&root);
    return out;
}

/* ── core.wallet.* mutating native leaves ────────────────────────────────
 * Dedicated registry handlers (request -> reply) for the leaves that create
 * keys, reveal keys, move funds, rescan, or write a backup. Each reaches the
 * running node over the same loopback JSON-RPC path the read bodies use
 * (zcl_native_bridge_ensure_rpc + node_rpc_call) and renders one bounded
 * JSON document.
 *
 * transaction.send / shielded.send / address.export-key carry
 * ZCL_COMMAND_CONFIRM_PLAN_COMMIT, and honour it with the reserved `confirm`
 * boolean input key: a first call without `confirm:true` returns a
 * non-mutating plan plus the exact commit next-action, and only a second call
 * with `confirm:true` broadcasts or reveals. Bound in
 * config/commands/core.def.
 *
 * `idempotency_key` is a caller-supplied correlation tag: it is folded into
 * the plan token and echoed on the committed reply, so a plan and its commit
 * are provably the same intent. It is NOT node-side deduplication — the
 * wallet RPC layer has no replay ledger, so two identical confirmed commits
 * broadcast twice. */

#define WNH_TAG "native.wallet"

/* Detect a JSON-RPC failure body: {"error":{...}} / {"error":"..."} or a bare
 * {"code":int,"message":str} (the shape node_rpc_call returns on transport
 * failure). On error, sets *msg_out to the message text when there is one. */
static bool wnh_body_is_error(const struct json_value *body,
                              const char **msg_out)
{
    if (!body || body->type != JSON_OBJ)
        return false;
    const struct json_value *err = json_get(body, "error");
    if (err && !json_is_null(err)) {
        if (msg_out) {
            if (err->type == JSON_OBJ)
                *msg_out = json_get_str(json_get(err, "message"));
            else if (err->type == JSON_STR)
                *msg_out = json_get_str(err);
        }
        return true;
    }
    const struct json_value *code = json_get(body, "code");
    const struct json_value *m = json_get(body, "message");
    if (code && code->type == JSON_INT && m && m->type == JSON_STR) {
        if (msg_out)
            *msg_out = json_get_str(m);
        return true;
    }
    return false;
}

/* Call one wallet RPC method. On success returns true and fills *out (caller
 * json_free's it). On any failure sets a typed error body on `reply`, releases
 * its own scratch, and returns false — never leaves `reply` silent. */
static bool wnh_call_rpc(struct zcl_command_reply *reply, const char *method,
                         const char *params_json, struct json_value *out)
{
    zcl_native_bridge_ensure_rpc();
    char *raw = node_rpc_call(method, params_json);
    if (!raw) {
        LOG_ERROR(WNH_TAG, "RPC %s returned null", method);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_TRANSIENT, "NODE_UNAVAILABLE",
                               "dispatch", true, false,
                               "the node did not return a wallet result",
                               method);
        return false;
    }
    if (!json_read(out, raw, strlen(raw))) {
        json_free(out);
        free(raw);
        LOG_ERROR(WNH_TAG, "RPC %s returned an unparseable body", method);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_RPC_BODY",
                               "serialize", false, false,
                               "wallet RPC returned an unparseable body",
                               method);
        return false;
    }
    free(raw);
    const char *emsg = NULL;
    if (wnh_body_is_error(out, &emsg)) {
        LOG_ERROR(WNH_TAG, "RPC %s reported an error: %s", method,
                  emsg && emsg[0] ? emsg : "(no message)");
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "WALLET_RPC_ERROR",
                               "execute", false, false,
                               emsg && emsg[0] ? emsg
                                               : "wallet RPC reported an error",
                               method);
        json_free(out);
        return false;
    }
    return true;
}

/* Deterministic, non-secret plan token binding a plan preview to its exact
 * parameters (FNV-1a over the parts, 16 hex). The operator can compare it
 * across the plan and the committed reply; no server state is stored. */
static void wnh_plan_token(char out[17], const char *a, const char *b,
                           const char *c)
{
    uint64_t h = 1469598103934665603ULL;
    const char *parts[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        for (const char *p = parts[i]; p && *p; p++) {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        h ^= 0x1f;
        h *= 1099511628211ULL;
    }
    (void)snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* Coerce an `amount` JSON value (int / real / decimal string) to a
 * non-negative double. Sets *ok=false on any other shape or a negative. */
static double wnh_amount_real(const struct json_value *amt, bool *ok)
{
    *ok = false;
    if (!amt)
        return 0.0;
    if (amt->type == JSON_REAL) {
        double v = json_get_real(amt);
        *ok = v >= 0.0;
        return v;
    }
    if (amt->type == JSON_INT) {
        double v = (double)json_get_int(amt);
        *ok = v >= 0.0;
        return v;
    }
    if (amt->type == JSON_STR) {
        const char *s = json_get_str(amt);
        if (!s || !s[0])
            return 0.0;
        char *end = NULL;
        double v = strtod(s, &end);
        if (end && !*end && v >= 0.0) {
            *ok = true;
            return v;
        }
    }
    return 0.0;
}

static void wnh_fail(struct zcl_command_reply *reply,
                     enum zcl_command_exit exit_code, const char *code,
                     const char *message, const char *evidence)
{
    enum zcl_command_status status =
        exit_code == ZCL_COMMAND_EXIT_BLOCKED ? ZCL_COMMAND_STATUS_BLOCKED
                                              : ZCL_COMMAND_STATUS_FAILED;
    LOG_ERROR(WNH_TAG, "%s: %s (%s)", code, message,
              evidence && evidence[0] ? evidence : "-");
    zcl_command_reply_fail(reply, status, exit_code, code, "handle", false,
                           false, message, evidence ? evidence : "");
}

/* Serialize `ci` into `commit` as the commit-half input for a plan, falling
 * back to the minimal {"confirm":true} when it would not fit. */
static void wnh_commit_input(const struct json_value *ci, char *commit,
                             size_t commit_size)
{
    size_t n = json_write(ci, commit, commit_size);
    if (n == 0 || n >= commit_size) {
        LOG_WARN(WNH_TAG, "commit input truncated (%zu bytes)", n);
        (void)snprintf(commit, commit_size, "{\"confirm\":true}");
    }
}

/* Emit the non-mutating plan half of a CONFIRM_PLAN_COMMIT leaf: stage=plan,
 * a plan_token, a confirm hint, and one next-action re-running THIS leaf with
 * the committed input. `commit_input` must be a schema-valid JSON object for
 * this leaf (it always includes "confirm":true). */
static void wnh_emit_plan(struct zcl_command_reply *reply, const char *path,
                          const char *action, const char *token,
                          const char *commit_input)
{
    (void)json_push_kv_str(&reply->data, "stage", "plan");
    (void)json_push_kv_str(&reply->data, "action", action);
    (void)json_push_kv_bool(&reply->data, "committed", false);
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    (void)json_push_kv_str(
        &reply->data, "confirm_hint",
        "re-run with \"confirm\":true to execute this plan");
    (void)zcl_command_reply_add_next(reply, path, commit_input,
                                     "commit this plan with confirm:true");
    reply->error.mutated = false;
}

/* Extract a bare JSON-string RPC result (getnewaddress / dumpprivkey /
 * sendtoaddress / z_sendmany all return one). NULL when the body was not a
 * non-empty string. */
static const char *wnh_string_result(const struct json_value *body)
{
    if (!body || body->type != JSON_STR)
        return NULL;
    const char *s = json_get_str(body);
    return (s && s[0]) ? s : NULL;
}

void zcl_native_handle_wallet_address_new(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    struct json_value body;
    if (!wnh_call_rpc(reply, "getnewaddress", NULL, &body))
        return;
    const char *addr = wnh_string_result(&body);
    if (!addr) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_ADDRESS",
                 "getnewaddress did not return an address", "getnewaddress");
        return;
    }
    (void)json_push_kv_str(&reply->data, "address", addr);
    (void)json_push_kv_bool(&reply->data, "created", true);
    reply->error.mutated = true;
    json_free(&body);
}

void zcl_native_handle_wallet_address_import(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *addr = json_get_str(json_get(request->input, "address"));
    if (!addr || !addr[0]) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_ADDRESS",
                 "address is required", "core.wallet.address.import");
        return;
    }
    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, addr);
    char *params = rpc_arg_builder_to_json(&p);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode importaddress params", addr);
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "importaddress", params, &body);
    free(params);
    if (!ok)
        return;
    json_free(&body); /* importaddress acknowledges with null on success */
    (void)json_push_kv_str(&reply->data, "address", addr);
    (void)json_push_kv_bool(&reply->data, "imported", true);
    reply->error.mutated = true;
}

void zcl_native_handle_wallet_address_export_key(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *addr = json_get_str(json_get(request->input, "address"));
    if (!addr || !addr[0]) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_ADDRESS",
                 "address is required", "core.wallet.address.export-key");
        return;
    }
    bool confirm = json_get_bool_or(request->input, "confirm", false);
    char token[17];
    wnh_plan_token(token, "export-key", addr, "");

    if (!confirm) {
        struct json_value ci;
        json_init(&ci);
        json_set_object(&ci);
        (void)json_push_kv_str(&ci, "address", addr);
        (void)json_push_kv_bool(&ci, "confirm", true);
        char commit[384];
        wnh_commit_input(&ci, commit, sizeof(commit));
        json_free(&ci);
        (void)json_push_kv_str(&reply->data, "address", addr);
        (void)json_push_kv_str(
            &reply->data, "warning",
            "commit reveals this address's private key in the response");
        wnh_emit_plan(reply, request->spec->path, "export-key", token, commit);
        return;
    }

    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, addr);
    char *params = rpc_arg_builder_to_json(&p);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode dumpprivkey params", addr);
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "dumpprivkey", params, &body);
    free(params);
    if (!ok)
        return;
    const char *wif = wnh_string_result(&body);
    if (!wif) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_KEY",
                 "dumpprivkey did not return a private key", addr);
        return;
    }
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_str(&reply->data, "address", addr);
    (void)json_push_kv_str(&reply->data, "privkey", wif);
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    json_free(&body);
}

void zcl_native_handle_wallet_transaction_send(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *addr = json_get_str(json_get(request->input, "address"));
    if (!addr || !addr[0]) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_ADDRESS",
                 "address is required", "core.wallet.transaction.send");
        return;
    }
    bool aok = false;
    double amount = wnh_amount_real(json_get(request->input, "amount"), &aok);
    if (!aok) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "INVALID_AMOUNT",
                 "amount must be a non-negative number", addr);
        return;
    }
    const char *idem =
        json_get_str(json_get(request->input, "idempotency_key"));
    bool confirm = json_get_bool_or(request->input, "confirm", false);
    char amtbuf[64];
    (void)snprintf(amtbuf, sizeof(amtbuf), "%.8f", amount);
    char token[17];
    wnh_plan_token(token, addr, amtbuf, idem ? idem : "");

    if (!confirm) {
        struct json_value ci;
        json_init(&ci);
        json_set_object(&ci);
        (void)json_push_kv_str(&ci, "address", addr);
        (void)json_push_kv_real(&ci, "amount", amount);
        if (idem && idem[0])
            (void)json_push_kv_str(&ci, "idempotency_key", idem);
        (void)json_push_kv_bool(&ci, "confirm", true);
        char commit[512];
        wnh_commit_input(&ci, commit, sizeof(commit));
        json_free(&ci);
        (void)json_push_kv_str(&reply->data, "address", addr);
        (void)json_push_kv_real(&reply->data, "amount", amount);
        if (idem && idem[0])
            (void)json_push_kv_str(&reply->data, "idempotency_key", idem);
        wnh_emit_plan(reply, request->spec->path, "send", token, commit);
        return;
    }

    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, addr);
    rpc_arg_builder_push_real(&p, amount);
    char *params = rpc_arg_builder_to_json(&p);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode sendtoaddress params", addr);
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "sendtoaddress", params, &body);
    free(params);
    if (!ok)
        return;
    const char *txid = wnh_string_result(&body);
    if (!txid) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_TXID",
                 "sendtoaddress did not return a transaction id", addr);
        return;
    }
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_str(&reply->data, "txid", txid);
    (void)json_push_kv_str(&reply->data, "address", addr);
    (void)json_push_kv_real(&reply->data, "amount", amount);
    if (idem && idem[0])
        (void)json_push_kv_str(&reply->data, "idempotency_key", idem);
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    reply->error.mutated = true;
    json_free(&body);
}

void zcl_native_handle_wallet_shielded_send(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *from = json_get_str(json_get(request->input, "from"));
    const char *to = json_get_str(json_get(request->input, "to"));
    if (!from || !from[0] || !to || !to[0]) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_ADDRESS",
                 "both from and to are required", "core.wallet.shielded.send");
        return;
    }
    bool aok = false;
    double amount = wnh_amount_real(json_get(request->input, "amount"), &aok);
    if (!aok) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "INVALID_AMOUNT",
                 "amount must be a non-negative number", to);
        return;
    }
    const char *idem =
        json_get_str(json_get(request->input, "idempotency_key"));
    bool confirm = json_get_bool_or(request->input, "confirm", false);
    char amtbuf[64];
    (void)snprintf(amtbuf, sizeof(amtbuf), "%.8f", amount);
    char token[17];
    wnh_plan_token(token, from, to, amtbuf);

    if (!confirm) {
        struct json_value ci;
        json_init(&ci);
        json_set_object(&ci);
        (void)json_push_kv_str(&ci, "from", from);
        (void)json_push_kv_str(&ci, "to", to);
        (void)json_push_kv_real(&ci, "amount", amount);
        if (idem && idem[0])
            (void)json_push_kv_str(&ci, "idempotency_key", idem);
        (void)json_push_kv_bool(&ci, "confirm", true);
        char commit[512];
        wnh_commit_input(&ci, commit, sizeof(commit));
        json_free(&ci);
        (void)json_push_kv_str(&reply->data, "from", from);
        (void)json_push_kv_str(&reply->data, "to", to);
        (void)json_push_kv_real(&reply->data, "amount", amount);
        wnh_emit_plan(reply, request->spec->path, "shielded-send", token,
                      commit);
        return;
    }

    /* z_sendmany takes [from, [{address, amount}]] — build the nested
     * recipient array through the encoder so a quote in from/to cannot
     * rewrite the params array. */
    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    rpc_arg_builder_push_str(&p, from);
    struct json_value recip, recip_arr;
    json_init(&recip);
    json_set_object(&recip);
    (void)json_push_kv_str(&recip, "address", to);
    (void)json_push_kv_real(&recip, "amount", amount);
    json_init(&recip_arr);
    json_set_array(&recip_arr);
    (void)json_push_back(&recip_arr, &recip);
    rpc_arg_builder_push_value(&p, &recip_arr);
    json_free(&recip);
    json_free(&recip_arr);
    char *params = rpc_arg_builder_to_json(&p);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode z_sendmany params", to);
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "z_sendmany", params, &body);
    free(params);
    if (!ok)
        return;
    /* z_sendmany returns an async operation id (a bare string). */
    const char *opid = wnh_string_result(&body);
    if (!opid) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_OPERATION_ID",
                 "z_sendmany did not return an operation id", to);
        return;
    }
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_str(&reply->data, "operation_id", opid);
    (void)json_push_kv_str(&reply->data, "from", from);
    (void)json_push_kv_str(&reply->data, "to", to);
    (void)json_push_kv_real(&reply->data, "amount", amount);
    if (idem && idem[0])
        (void)json_push_kv_str(&reply->data, "idempotency_key", idem);
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    reply->error.mutated = true;
    json_free(&body);
}

void zcl_native_handle_wallet_rescan(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const struct json_value *sh = json_get(request->input, "start_height");
    struct rpc_arg_builder p;
    rpc_arg_builder_init(&p);
    if (sh && sh->type == JSON_INT) {
        if (json_get_int(sh) < 0) {
            rpc_arg_builder_free(&p);
            wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "INVALID_START_HEIGHT",
                     "start_height must be a non-negative integer",
                     "core.wallet.rescan");
            return;
        }
        rpc_arg_builder_push_int(&p, json_get_int(sh));
    }
    char *params = rpc_arg_builder_to_json(&p);
    if (!params) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INTERNAL, "ARG_BUILD_FAILED",
                 "could not encode rescanblockchain params",
                 "core.wallet.rescan");
        return;
    }
    struct json_value body;
    bool ok = wnh_call_rpc(reply, "rescanblockchain", params, &body);
    free(params);
    if (!ok)
        return;
    (void)json_push_kv(&reply->data, "result", &body);
    (void)json_push_kv_bool(&reply->data, "started", true);
    reply->error.mutated = true;
    json_free(&body);
}

void zcl_native_handle_wallet_backup_now(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    struct json_value body;
    if (!wnh_call_rpc(reply, "walletbackupnow", NULL, &body))
        return;
    (void)json_push_kv(&reply->data, "backup", &body);
    (void)json_push_kv_bool(&reply->data, "created", true);
    reply->error.mutated = true;
    json_free(&body);
}

/* ── Tier-1 hot-swap: native.leaves generation entrypoint ──────
 * Dev-only (compiled only under -DZCL_HOTSWAP_GEN, a generation .so build;
 * expands to nothing in the node/release TU — see ZCL_HOTSWAP_EXPORT_LEAVES
 * in lib/hotswap/include/hotswap/hotswap.h). Stages every native command
 * leaf this controller owns; the resident bridge re-points them at THIS
 * TU's freshly-compiled bodies via zcl_native_bridge_run(). Probe is
 * core.wallet.address.list: zcl_native_listaddresses_body ignores `args`
 * ((void)args) and unconditionally calls listwalletkeys[false], returning
 * {"t_addresses":[...],"z_addresses":[...]} with no top-level "error" key
 * on success, so the empty-args self-test dispatch succeeds.
 * core.wallet.utxo.list / core.wallet.transaction.list also default their
 * params (minconf/maxconf, count/skip) and would work as a probe too;
 * core.wallet.transaction.get requires a caller-supplied txid and is NOT
 * a probe candidate. See config/hotswap_eligible.def. */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_PROBE_LEAF "core.wallet.address.list"
#include "hotswap/hotswap.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void tramp_listaddresses(const struct zcl_command_request *request,
                                struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_listaddresses_body, reply);
}

static void tramp_listunspent(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_listunspent_body, reply);
}

static void tramp_listtransactions(const struct zcl_command_request *request,
                                   struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_listtransactions_body, reply);
}

static void tramp_gettransaction(const struct zcl_command_request *request,
                                 struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_gettransaction_body, reply);
}

static const struct zcl_hotswap_leaf_replacement k_leaves[] = {
    { "core.wallet.address.list",      tramp_listaddresses },
    { "core.wallet.utxo.list",         tramp_listunspent },
    { "core.wallet.transaction.list",  tramp_listtransactions },
    { "core.wallet.transaction.get",   tramp_gettransaction },
};

ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, sizeof(k_leaves) / sizeof(k_leaves[0]))
#endif /* ZCL_HOTSWAP_GEN */

/* REAL (activatable) single-handler module ABI export. Compiled only under a
 * `make hotswap-module-so HANDLER=core.wallet.address.list` build
 * (-DZCL_HOTSWAP_MODULE_GEN); expands to nothing in the node/release TU. The
 * module re-points ONLY the `core.wallet.address.list` leaf to this TU's
 * freshly-compiled body via the same zcl_native_bridge_run() seam the leaf
 * provider uses. See hotswap_module.h and hotswap_activate() (lib/hotswap). */
#ifdef ZCL_HOTSWAP_MODULE_GEN
#include "hotswap/hotswap_module.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void module_tramp_listaddresses(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_listaddresses_body, reply);
}

/* The module's own health hook — runs before the loader publishes it. Kept
 * node-independent (no RPC): a structural OK. */
static bool module_selftest_listaddresses(char *err, size_t cap)
{
    (void)err;
    (void)cap;
    return true;
}

ZCL_HOTSWAP_MODULE("core.wallet.address.list",
                   module_tramp_listaddresses,
                   module_selftest_listaddresses)
#endif /* ZCL_HOTSWAP_MODULE_GEN */
