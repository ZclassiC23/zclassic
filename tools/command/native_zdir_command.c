/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the `core zdir` tree — the operator's way to put a
 * node on the on-chain directory, and to take it back off.
 *
 * This is the surface that closes the gap zdir/zdir.h named: the ZDIR codec
 * and the onion_directory fold were both wired, but nothing composed,
 * funded, signed or broadcast a record, so on a network where no other tool
 * writes ZDIR records the projection read empty forever.
 *
 * Writes (register/deregister) take TWO paths, exactly like
 * core.identity.anchor:
 *   1. LIVE NODE — dispatch zdir_register / zdir_deregister over JSON-RPC.
 *      params is a JSON-RPC ARRAY whose [0] is the --input-style object; a
 *      bare object is NOT a valid params and the dispatcher would never see
 *      the fields.
 *   2. OFFLINE — build the same `ZDIR` OP_RETURN locally and return
 *      op_return_hex plus the exact next step, so an operator with a cold
 *      wallet can include it themselves.
 * Nothing here broadcasts on its own: path 1 hands the decision to the
 * node's wallet, path 2 hands the bytes to the operator. Neither path runs
 * unless an operator types the command — no boot path, no timer, no
 * background service reaches this file.
 *
 * Pre-flight reads run against <datadir>/node.db READONLY BEFORE either
 * path — the core.storage.query.offline pattern — so a refusal is named and
 * free rather than a spent fee: deregister needs an existing, still-active
 * row, and any mutation of a row that records no owner is refused outright
 * because the fold would refuse it too.
 *
 * NO TRANSFER LEAF. ZDIR command byte 3 is reserved for TRANSFER and
 * zdir_parse deliberately rejects it, because a parsed-but-unhandled
 * command would be a silent stub. Handing a hostname to another operator is
 * expressed as `core zdir deregister` by the current owner followed by
 * `core zdir register` from the new one. */

#include "command/native_command.h"

#include "controllers/rpc_client.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "models/database.h"
#include "models/onion_directory.h"
#include "net/onion_peer_merge.h"
#include "zdir/zdir.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── small helpers ────────────────────────────────────────────────── */

static const char *zdc_input_str(const struct json_value *input,
                                 const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

/* Explicit input.datadir wins, else the CLI's --datadir. */
static const char *zdc_datadir(const struct zcl_command_request *request)
{
    const char *dd = zdc_input_str(request->input, "datadir");
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

/* Open <datadir>/node.db READONLY (no CREATE) behind an ad-hoc node_db.
 * A missing node.db is NOT an error here: registering a hostname on a host
 * with no folded chain is a legitimate offline operation. Returns the handle
 * or NULL; nothing is reported either way. */
static sqlite3 *zdc_open_db_quiet(const char *datadir, struct node_db *ndb_out)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/node.db", datadir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return NULL;  // raw-return-ok:optional-preflight-open
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;  // raw-return-ok:optional-preflight-open
    }
    (void)sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 2000);
    memset(ndb_out, 0, sizeof(*ndb_out));
    ndb_out->db = db;
    ndb_out->open = true;
    return db;
}

/* Exactly 64 hex chars decoding to a non-zero 32-byte key. */
static bool zdc_parse_key(const char *hex, uint8_t out[ZDIR_PUBKEY_LEN])
{
    if (!hex || strlen(hex) != 64 || !IsHex(hex))
        return false;
    if (ParseHex(hex, out, ZDIR_PUBKEY_LEN) != ZDIR_PUBKEY_LEN)
        return false;
    for (int i = 0; i < ZDIR_PUBKEY_LEN; i++)
        if (out[i])
            return true;
    return false;   /* all-zero is the unset sentinel, never a usable key */
}

/* A node_rpc_call body that is an error, not a result — the transport's own
 * {"error":{...}} envelope, an extracted {code,message}, or a bare string
 * (the RPC handler's own message). Verbatim shape of zidc_rpc_body_error in
 * native_identity_command.c. */
static bool zdc_rpc_body_error(const struct json_value *v, char *msg,
                               size_t msg_size)
{
    const char *m = NULL;
    if (v->type == JSON_STR) {
        m = json_get_str(v);
    } else if (v->type == JSON_OBJ) {
        const struct json_value *err = json_get(v, "error");
        if (err && err->type != JSON_NULL) {
            const struct json_value *em =
                err->type == JSON_OBJ ? json_get(err, "message") : NULL;
            m = (em && em->type == JSON_STR) ? json_get_str(em)
                                             : "node RPC error";
        } else {
            const struct json_value *code = json_get(v, "code");
            const struct json_value *msg_v = json_get(v, "message");
            if (code && code->type == JSON_INT && msg_v &&
                msg_v->type == JSON_STR)
                m = json_get_str(msg_v);
        }
    }
    if (m && msg)
        snprintf(msg, msg_size, "%s", m);
    return m != NULL;
}

/* Try the live node. Returns true and fills reply on success; otherwise
 * returns false with rpc_err carrying why (empty when no node answered). */
static bool zdc_try_rpc(const char *method, const char *params,
                        struct zcl_command_reply *reply, char *rpc_err,
                        size_t rpc_err_size)
{
    rpc_err[0] = '\0';
    zcl_native_bridge_ensure_rpc();
    char *rpc_result = node_rpc_call(method, params);
    if (!rpc_result)
        return false;

    struct json_value body;
    bool parsed = json_read(&body, rpc_result, strlen(rpc_result));
    bool error_body = parsed &&
                      zdc_rpc_body_error(&body, rpc_err, rpc_err_size);
    if (parsed && body.type == JSON_OBJ && !error_body) {
        char via[96];
        snprintf(via, sizeof(via), "node_rpc %s", method);
        json_push_kv_str(&body, "via", via);
        json_copy(&reply->data, &body);
        json_free(&body);
        free(rpc_result);
        reply->status = ZCL_COMMAND_STATUS_PASSED;
        reply->exit_code = ZCL_COMMAND_EXIT_OK;
        return true;
    }
    if (!error_body)
        snprintf(rpc_err, rpc_err_size, "%s",
                 parsed ? "node RPC returned an unexpected body"
                        : "node RPC returned an unparseable body");
    json_free(&body);
    free(rpc_result);
    return false;
}

/* The shared offline tail: emit the script the operator must include. */
static void zdc_offline_reply(struct zcl_command_reply *reply,
                              const char *command, const uint8_t *script,
                              size_t script_len, const char *rpc_err)
{
    char hex[ZDIR_SCRIPT_MAX * 2 + 2];
    HexStr(script, script_len, false, hex, sizeof(hex));
    json_push_kv_str(&reply->data, "command", command);
    if (rpc_err && rpc_err[0])
        json_push_kv_str(&reply->data, "node_rpc_error", rpc_err);
    json_push_kv_str(&reply->data, "op_return_hex", hex);
    json_push_kv_int(&reply->data, "op_return_size", (int64_t)script_len);
    json_push_kv_str(&reply->data, "status", "ready");
    /* Two different situations reach this tail and they must not be reported
     * with one sentence. "Nothing answered" is a node you have not started;
     * "the node refused" is a node that answered and could not compose the
     * transaction — most often an unfunded wallet. Telling an operator with
     * a running node to "start the node" sends them to debug the wrong
     * thing, so the message follows node_rpc_error. */
    json_push_kv_str(&reply->data, "note",
                     (rpc_err && rpc_err[0])
                         ? "the node answered but could not compose the "
                           "transaction — see node_rpc_error (an unfunded "
                           "wallet is the usual cause). The OP_RETURN below "
                           "is still correct: fund the wallet and re-run, or "
                           "include it as vout[0] of a transaction you sign "
                           "yourself"
                         : "no live node answered — start the node and re-run "
                           "to compose+broadcast with the node wallet, or "
                           "include this OP_RETURN as vout[0] of any "
                           "transaction you sign yourself");
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* Read --hostname, holding it to the ONE v3 onion rule the node has
 * (onion_hostname_valid). Fails the reply with a named code and returns
 * NULL when it is absent or not a v3 onion. */
static const char *zdc_require_hostname(const struct json_value *input,
                                        struct zcl_command_reply *reply,
                                        const char *path)
{
    const char *hostname = zdc_input_str(input, "hostname");
    if (!hostname || !hostname[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_HOSTNAME",
                               "normalize", false, false,
                               "give --hostname=<56 base32 chars>.onion — the "
                               "exact Tor v3 hostname this node serves", path);
        return NULL;
    }
    if (!onion_hostname_valid(hostname)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_HOSTNAME",
                               "normalize", false, false,
                               "hostname must be a Tor v3 onion: exactly 56 "
                               "base32 characters followed by \".onion\" — v2 "
                               "onions and bare hostnames are refused",
                               hostname);
        return NULL;
    }
    return hostname;
}

/* ── core.zdir.register ───────────────────────────────────────────── */

void zcl_native_handle_core_zdir_register(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *path = "core.zdir.register";

    const char *hostname = zdc_require_hostname(request->input, reply, path);
    if (!hostname)
        return;

    const char *pubkey_hex = zdc_input_str(request->input, "pubkey");
    bool have_key = pubkey_hex && pubkey_hex[0];
    uint8_t key[ZDIR_PUBKEY_LEN];
    if (have_key && !zdc_parse_key(pubkey_hex, key)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_PUBKEY_HEX",
                               "normalize", false, false,
                               "pubkey must be exactly 64 hex characters and "
                               "not all-zero — omit it entirely to register "
                               "the hostname unbound", pubkey_hex);
        return;
    }

    /* Pre-flight: a hostname whose row records no owner can never be
     * mutated by anyone, so re-registering it would spend a fee on a record
     * the fold refuses. A missing node.db is not an error — a first
     * registration from a host with no folded chain is legitimate. */
    const char *datadir = zdc_datadir(request);
    char owner[ONION_DIRECTORY_ADDRESS_MAX] = "";
    char status[ONION_DIRECTORY_STATUS_MAX] = "";
    bool known = false;
    if (datadir) {
        struct node_db ndb;
        sqlite3 *db = zdc_open_db_quiet(datadir, &ndb);
        if (db) {
            struct db_onion_directory prev;
            memset(&prev, 0, sizeof(prev));
            known = db_onion_directory_find(&ndb, hostname, &prev);
            if (known) {
                snprintf(owner, sizeof(owner), "%s", prev.owner_address);
                snprintf(status, sizeof(status), "%s", prev.status);
            }
            sqlite3_close(db);
            if (known && owner[0] == '\0') {
                zcl_command_reply_fail(
                    reply, ZCL_COMMAND_STATUS_FAILED,
                    ZCL_COMMAND_EXIT_FAILED, "NOT_OWNER", "execute", false,
                    false,
                    "this hostname already has a directory row that records "
                    "no owner address, so no signer can ever prove ownership "
                    "of it — the row is permanently immutable by design and "
                    "a re-registration would be refused by every node",
                    hostname);
                return;
            }
        }
    }

    char params[256];
    if (have_key)
        snprintf(params, sizeof(params),
                 "[{\"hostname\":\"%s\",\"pubkey\":\"%s\"}]", hostname,
                 pubkey_hex);
    else
        snprintf(params, sizeof(params), "[{\"hostname\":\"%s\"}]", hostname);
    char rpc_err[256] = {0};
    if (zdc_try_rpc("zdir_register", params, reply, rpc_err, sizeof(rpc_err)))
        return;

    uint8_t script[ZDIR_SCRIPT_MAX];
    size_t script_len = zdir_build_register(script, sizeof(script), hostname,
                                            have_key ? key : NULL);
    if (script_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "OP_RETURN_BUILD_FAILED", "execute", false,
                               false, "zdir_build_register rejected the "
                               "hostname or key", hostname);
        return;
    }
    json_push_kv_str(&reply->data, "hostname", hostname);
    if (have_key)
        json_push_kv_str(&reply->data, "pubkey", pubkey_hex);
    if (known) {
        json_push_kv_str(&reply->data, "owner_address", owner);
        json_push_kv_str(&reply->data, "prior_status", status);
    }
    zdc_offline_reply(reply, "register", script, script_len, rpc_err);
}

/* ── core.zdir.deregister ─────────────────────────────────────────── */

void zcl_native_handle_core_zdir_deregister(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *path = "core.zdir.deregister";

    const char *hostname = zdc_require_hostname(request->input, reply, path);
    if (!hostname)
        return;

    const char *datadir = zdc_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default — "
                               "deregister must read the directory row first "
                               "to know who owns it", path);
        return;
    }
    struct node_db ndb;
    sqlite3 *db = zdc_open_db_quiet(datadir, &ndb);
    if (!db) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNAVAILABLE", "execute", true, false,
                               "node.db not found or unreadable at datadir — "
                               "check --datadir, or boot the node once to "
                               "create it", datadir);
        return;
    }
    struct db_onion_directory prev;
    memset(&prev, 0, sizeof(prev));
    bool found = db_onion_directory_find(&ndb, hostname, &prev);
    sqlite3_close(db);

    if (!found) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "NOT_REGISTERED",
                               "execute", false, false,
                               "this hostname has no directory row on the "
                               "chain this node has folded — there is "
                               "nothing to retire", hostname);
        return;
    }
    if (strcmp(prev.status, ONION_DIRECTORY_STATUS_RETIRED) == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "ALREADY_RETIRED",
                               "execute", false, false,
                               "this hostname is already retired — "
                               "deregistering it again would spend a fee to "
                               "say nothing", hostname);
        return;
    }
    if (prev.owner_address[0] == '\0') {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "NOT_OWNER",
                               "execute", false, false,
                               "the directory row records no owner address, "
                               "so no signer can ever prove ownership of it "
                               "— this hostname is permanently immutable by "
                               "design", hostname);
        return;
    }

    char params[256];
    snprintf(params, sizeof(params), "[{\"hostname\":\"%s\"}]", hostname);
    char rpc_err[256] = {0};
    if (zdc_try_rpc("zdir_deregister", params, reply, rpc_err,
                    sizeof(rpc_err)))
        return;

    uint8_t script[ZDIR_SCRIPT_MAX];
    size_t script_len = zdir_build_deregister(script, sizeof(script),
                                              hostname);
    if (script_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "OP_RETURN_BUILD_FAILED", "execute", false,
                               false, "zdir_build_deregister rejected the "
                               "hostname", hostname);
        return;
    }
    json_push_kv_str(&reply->data, "hostname", hostname);
    json_push_kv_str(&reply->data, "owner_address", prev.owner_address);
    zdc_offline_reply(reply, "deregister", script, script_len, rpc_err);
}
