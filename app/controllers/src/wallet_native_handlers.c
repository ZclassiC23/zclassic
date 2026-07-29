/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Registry handlers for the MUTATING core.wallet.* leaves — the ones that
 * create keys, reveal keys, move funds, rescan, or write a backup.
 *
 * The read bodies that compose a query RPC live in
 * wallet_native_read_bodies.c. They were split out when this file passed the
 * size ceiling: the two halves are different risk classes, and the leaves here
 * are the ones that carry ZCL_COMMAND_CONFIRM_PLAN_COMMIT.
 *
 * The trampolines at the end still bind the read bodies, which is why the
 * shared header is included from both files. */

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
 * a plan_token, a confirm hint, and the exact input that commits it.
 *
 * The commit input travels as DATA, not as a next-action. A next-action naming
 * this same leaf cannot be serialized at all: push_next_array() rejects a next
 * whose path equals the current leaf's (lib/kernel/src/command_registry.c),
 * which is a deliberate guard against a command that only ever points at
 * itself. Emitting one anyway failed the whole reply, so every plan leg here —
 * send, shielded-send, export-key — answered RESPONSE_BUDGET_EXCEEDED instead
 * of a plan, and the plan/commit flow could not be driven from the typed
 * interface at all. The caller needs the committing input, not a link; it is
 * `commit_input` below, and re-running this leaf with it executes the plan. */
static void wnh_emit_plan(struct zcl_command_reply *reply, const char *path,
                          const char *action, const char *token,
                          const char *commit_input)
{
    (void)path; /* the commit re-runs THIS leaf; see the note above */
    (void)json_push_kv_str(&reply->data, "stage", "plan");
    (void)json_push_kv_str(&reply->data, "action", action);
    (void)json_push_kv_bool(&reply->data, "committed", false);
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    (void)json_push_kv_str(
        &reply->data, "confirm_hint",
        "re-run this same command with the commit_input below to execute");
    (void)json_push_kv_str(&reply->data, "commit_input", commit_input);
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

void zcl_native_handle_wallet_shielded_address(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    (void)request;
    struct json_value body;
    if (!wnh_call_rpc(reply, "z_getnewaddress", NULL, &body))
        return;
    const char *addr = wnh_string_result(&body);
    if (!addr) {
        json_free(&body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_ADDRESS",
                 "z_getnewaddress did not return a shielded address",
                 "z_getnewaddress");
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

    /* The scan is synchronous (see core.def: MODE_JOB is aspirational —
     * rpc_rescanblockchain blocks until the scan completes), so by the time
     * we are here it has FINISHED. Report what it actually covered and
     * found; never an unconditional "started". */
    const struct json_value *cov = json_get(&body, "coverage_ok");
    const struct json_value *blk = json_get(&body, "blocker");
    bool coverage_ok = !cov || cov->type != JSON_BOOL || json_get_bool(cov);

    if (!coverage_ok) {
        /* A rescan that could not read the blocks it was asked to scan is a
         * failure, not a success with a small number in it. Surface the
         * counts alongside the typed name so an agent can act without a
         * second call. */
        const char *code = (blk && blk->type == JSON_STR)
                             ? json_get_str(blk) : "RESCAN_INCOMPLETE_COVERAGE";
        char msg[384];
        const struct json_value *scanned = json_get(&body, "blocks_scanned");
        const struct json_value *indexed = json_get(&body, "blocks_indexed");
        const struct json_value *missing = json_get(&body, "blocks_missing_data");
        snprintf(msg, sizeof(msg),
                 "rescan read %lld of %lld indexed blocks (%lld have no block "
                 "body on this node); the result does NOT mean the wallet is "
                 "empty — this node cannot see those blocks' transactions",
                 scanned ? (long long)json_get_int(scanned) : 0LL,
                 indexed ? (long long)json_get_int(indexed) : 0LL,
                 missing ? (long long)json_get_int(missing) : 0LL);
        (void)json_push_kv(&reply->data, "result", &body);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, code, msg,
                 "core.wallet.rescan");
        /* AFTER wnh_fail: zcl_command_reply_fail() overwrites `mutated`.
         * A failed rescan still wrote — it advanced best_block_height and
         * folded in whatever the blocks it COULD read contained. */
        reply->error.mutated = true;
        json_free(&body);
        return;
    }

    (void)json_push_kv(&reply->data, "result", &body);
    (void)json_push_kv_bool(&reply->data, "completed", true);
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
