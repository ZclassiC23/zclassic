/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The `agentsession` RPC: the node's own surface onto the agent_sessions
 * store. See controllers/agent_session_controller.h for WHY this is an RPC
 * (short version: the policy gates run in the CLI process, which has no
 * node.db, and a second writer on a live node's database is not an option).
 *
 * Shape: params are [ "<action>", { ...fields } ]. Every action answers an
 * OBJECT with an `ok` boolean and, on refusal, a `why` token drawn from the
 * service/model vocabulary — the caller maps that token straight onto a named
 * error, so a refusal never loses its reason crossing the socket. Failures
 * return false with the message in `result`, which is the RPC convention the
 * rest of the controllers use. */

#include "controllers/agent_session_controller.h"

#include "controllers/native_handler_body.h"
#include "json/json.h"
#include "models/agent_session.h"
#include "controllers/strong_params.h"
#include "rpc/server.h"
#include "services/agent_session_service.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdlib.h>
#include <string.h>

#define AGS_TAG "agentsession"

static bool ags_fail(struct json_value *result, const char *msg)
{
    json_set_str(result, msg);
    LOG_FAIL(AGS_TAG, "%s", msg);
}

/* An object answer with ok=false and a machine token — a REFUSAL, not a
 * transport failure, so the RPC itself succeeded. Keeping these apart is what
 * lets the caller tell "the policy said no" from "the node is unreachable",
 * which are opposite actions for an operator. */
static bool ags_refuse(struct json_value *result, const char *why)
{
    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", false);
    (void)json_push_kv_str(result, "why", why);
    return true;
}

static int64_t ags_int(const struct json_value *o, const char *key,
                       int64_t dflt)
{
    const struct json_value *v = json_get(o, key);
    if (!v)
        return dflt;
    if (v->type == JSON_INT)
        return json_get_int(v);
    if (v->type == JSON_STR) {
        const char *s = json_get_str(v);
        if (s && s[0]) {
            char *end = NULL;
            long long n = strtoll(s, &end, 10);
            if (end && !*end)
                return (int64_t)n;
        }
    }
    return dflt;
}

static const char *ags_str(const struct json_value *o, const char *key)
{
    const char *s = json_get_str(json_get(o, key));
    return (s && s[0]) ? s : NULL;
}

/* ── mint ──────────────────────────────────────────────────────────────── */

static bool ags_mint(const struct json_value *in, struct json_value *result)
{
    struct agent_session_mint_request req;
    memset(&req, 0, sizeof(req));
    const char *account = ags_str(in, "account");
    if (!account)
        return ags_refuse(result, "BAD_ARGS");
    (void)snprintf(req.account, sizeof(req.account), "%s", account);
    req.max_per_tx_zat = ags_int(in, "max_per_tx_zat", -1);
    req.max_per_window_zat = ags_int(in, "max_per_window_zat", -1);
    req.window_seconds = ags_int(in, "window_seconds", 0);
    req.expires_in_seconds = ags_int(in, "expires_in_seconds", 0);
    const char *allow = ags_str(in, "recipient_allowlist");
    if (allow)
        (void)snprintf(req.recipient_allowlist,
                       sizeof(req.recipient_allowlist), "%s", allow);

    char sid[AGENT_SESSION_ID_MAX + 1] = { 0 };
    char why[64] = { 0 };
    if (!agent_session_service_mint(&req, sid, why, sizeof(why)))
        return ags_refuse(result, why[0] ? why : "PERSIST_FAILED");

    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", true);
    (void)json_push_kv_str(result, "session_id", sid);
    return true;
}

/* ── list ──────────────────────────────────────────────────────────────── */

static bool ags_list(const struct json_value *in, struct json_value *result)
{
    struct db_agent_session *rows =
        zcl_malloc(sizeof(*rows) * AGENT_SESSION_LIST_MAX,
                   "agentsession rpc rows");
    if (!rows)
        return ags_fail(result, "agentsession: allocation failed");
    int n = agent_session_service_list(ags_str(in, "account"), rows,
                                       AGENT_SESSION_LIST_MAX);
    if (n < 0) {
        free(rows);
        return ags_refuse(result, "DB_UNAVAILABLE");
    }

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (int i = 0; i < n; i++) {
        /* Redact the token here for the same reason the native leaf does
         * (tools/command/native_vault_session_command.c): session_id is a
         * BEARER grant — presenting it is the whole act that makes a spend
         * run under that grant's caps, so listing sessions must never hand
         * back a usable one. This surface used to return it in full while
         * the native leaf redacted, which made the redaction cosmetic: two
         * doors onto the same rows, one locked. The cookie holder is still
         * outside the grant model by design (docs/CUSTODY_MODEL.md), so this
         * closes an inconsistency rather than a hole — but a rule that holds
         * on only one of two surfaces is not a rule. */
        char redacted[24];
        agent_session_redact_id(rows[i].session_id, redacted,
                                sizeof(redacted));
        struct json_value o;
        json_init(&o);
        json_set_object(&o);
        (void)json_push_kv_str(&o, "session_id", redacted);
        (void)json_push_kv_bool(&o, "session_id_redacted", true);
        (void)json_push_kv_str(&o, "account", rows[i].account);
        (void)json_push_kv_int(&o, "max_per_tx_zat", rows[i].max_per_tx_zat);
        (void)json_push_kv_int(&o, "max_per_window_zat",
                               rows[i].max_per_window_zat);
        (void)json_push_kv_int(&o, "window_seconds", rows[i].window_seconds);
        (void)json_push_kv_int(&o, "window_start_epoch",
                               rows[i].window_start_epoch);
        (void)json_push_kv_int(&o, "spent_in_window_zat",
                               rows[i].spent_in_window_zat);
        (void)json_push_kv_str(&o, "recipient_allowlist",
                               rows[i].recipient_allowlist);
        (void)json_push_kv_int(&o, "created_at", rows[i].created_at);
        (void)json_push_kv_int(&o, "expires_at", rows[i].expires_at);
        (void)json_push_kv_int(&o, "revoked", rows[i].revoked);
        (void)json_push_back(&arr, &o);
        json_free(&o);
    }
    free(rows);

    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", true);
    (void)json_push_kv(result, "sessions", &arr);
    (void)json_push_kv_int(result, "session_count", n);
    json_free(&arr);
    return true;
}

/* ── revoke / authorize / release ──────────────────────────────────────── */

static bool ags_revoke(const struct json_value *in, struct json_value *result)
{
    const char *sid = ags_str(in, "session_id");
    if (!sid)
        return ags_refuse(result, "BAD_ARGS");
    char why[64] = { 0 };
    if (!agent_session_service_revoke(sid, why, sizeof(why)))
        return ags_refuse(result, why[0] ? why : "PERSIST_FAILED");
    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", true);
    (void)json_push_kv_bool(result, "revoked", true);
    return true;
}

static bool ags_authorize(const struct json_value *in,
                          struct json_value *result)
{
    const char *sid = ags_str(in, "session_id");
    if (!sid)
        return ags_refuse(result, "BAD_ARGS");
    int64_t amount_zat = ags_int(in, "amount_zat", -1);
    bool commit = json_get_bool_or(in, "commit", false);
    int64_t remaining = 0;
    enum agent_session_authz v = agent_session_service_authorize(
        sid, amount_zat, ags_str(in, "recipient"), commit, &remaining);
    if (v != AGENT_SESSION_AUTHZ_OK)
        return ags_refuse(result, agent_session_authz_token(v));
    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", true);
    (void)json_push_kv_bool(result, "committed", commit);
    (void)json_push_kv_int(result, "window_remaining_zat", remaining);
    return true;
}

static bool ags_release(const struct json_value *in, struct json_value *result)
{
    const char *sid = ags_str(in, "session_id");
    if (!sid)
        return ags_refuse(result, "BAD_ARGS");
    if (!agent_session_service_release(sid, ags_int(in, "amount_zat", 0)))
        return ags_refuse(result, "POLICY_STORE");
    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", true);
    (void)json_push_kv_bool(result, "released", true);
    return true;
}

/* ── dispatch ──────────────────────────────────────────────────────────── */

static bool rpc_agentsession(const struct json_value *params, bool help,
                             struct json_value *result)
{
    RPC_HELP(help, result,
        "agentsession \"<action>\" { ...fields }\n"
        "\nThe node-side surface onto the agent_sessions store — scoped,\n"
        "revocable agent spend grants. Actions:\n"
        "  mint      {account, max_per_tx_zat, max_per_window_zat,\n"
        "             window_seconds, recipient_allowlist, expires_in_seconds}\n"
        "            -> {ok, session_id}  (the ONE time the token is returned)\n"
        "  list      {account?} -> {ok, sessions[], session_count}\n"
        "  revoke    {session_id} -> {ok, revoked}\n"
        "  authorize {session_id, amount_zat, recipient?, commit}\n"
        "            -> {ok, committed, window_remaining_zat}; the check AND\n"
        "               the window debit in one indivisible step\n"
        "  release   {session_id, amount_zat} -> {ok, released}; credit back a\n"
        "               debit whose spend never happened\n"
        "\nA refusal answers {ok:false, why:\"<TOKEN>\"} — a successful RPC\n"
        "carrying a policy decision. Only a transport/usage error fails.");

    const char *action = json_get_str(json_at(params, 0));
    const struct json_value *in = json_at(params, 1);
    if (!action || !action[0])
        return ags_fail(result, "agentsession: action is required");
    if (in && in->type != JSON_OBJ)
        return ags_fail(result, "agentsession: second param must be an object");

    if (strcmp(action, "mint") == 0)
        return ags_mint(in, result);
    if (strcmp(action, "list") == 0)
        return ags_list(in, result);
    if (strcmp(action, "revoke") == 0)
        return ags_revoke(in, result);
    if (strcmp(action, "authorize") == 0)
        return ags_authorize(in, result);
    if (strcmp(action, "release") == 0)
        return ags_release(in, result);
    return ags_fail(result, "agentsession: unknown action");
}

void register_agent_session_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmd = { "wallet", "agentsession", rpc_agentsession,
                               false };
    rpc_table_must_append(t, &cmd);
}
