/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the `core identity` tree — the chain-rooted answer
 * to "whose master key is this, and is it still live?"
 * (docs/spec/sovereign-identity-layer.md).
 *
 * Reads (resolve/list) open <datadir>/node.db READONLY — the
 * core.storage.query.offline pattern — and answer straight out of the
 * zid_identities projection (app/models/src/zid_identity.c), which is
 * fed from confirmed chain data by both identity feeds
 * (app/models/src/explorer_index_zid.c). So a stopped or copied datadir
 * answers exactly like a running one, with no RPC.
 *
 * Writes (anchor/rotate/revoke) take TWO paths, like core.epoch.anchor:
 *   1. LIVE NODE — dispatch identity_anchor / identity_rotate /
 *      identity_revoke over JSON-RPC. params is a JSON-RPC ARRAY whose
 *      [0] is the --input-style object; a bare object is NOT a valid
 *      params and the dispatcher would never see the fields.
 *   2. OFFLINE — build the same `ZID\0` OP_RETURN locally and return
 *      op_return_hex plus the exact next step, so an operator with a
 *      cold wallet can include it themselves.
 * Nothing here broadcasts on its own: path 1 hands the decision to the
 * node's wallet, path 2 hands the bytes to the operator.
 *
 * Pre-flight reads run against the projection BEFORE either path, so a
 * refusal is named and free rather than a spent fee: rotate/revoke need
 * an existing, non-revoked row; rotate needs old != new. */

#include "command/native_command.h"

#include "controllers/rpc_client.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "models/database.h"
#include "models/zid_identity.h"
#include "zid/zid_anchor.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZID_CMD_LIST_DEFAULT 25
#define ZID_CMD_LIST_CAP     100

/* ── small helpers ────────────────────────────────────────────────── */

static const char *zidc_input_str(const struct json_value *input,
                                  const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

static int64_t zidc_input_int(const struct json_value *input, const char *key,
                              int64_t def)
{
    const struct json_value *v = json_get(input, key);
    if (!v) return def;
    if (v->type == JSON_INT) return json_get_int(v);
    if (v->type == JSON_STR) {
        const char *s = json_get_str(v);
        return (s && s[0]) ? strtoll(s, NULL, 10) : def;
    }
    return def;
}

/* Explicit input.datadir wins, else the CLI's --datadir. */
static const char *zidc_datadir(const struct zcl_command_request *request)
{
    const char *dd = zidc_input_str(request->input, "datadir");
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

/* The read-only open lives in tools/command/native_node_db_ro.c; these
 * leaves call zcl_native_node_db_require_readonly directly.
 *
 * The one caller that still needs a local wrapper is the anchor preflight:
 * a missing node.db is NOT an error there, because anchoring a brand new
 * key on a host with no folded chain is a legitimate offline operation.
 * That is a decision about ABSENT only — an UNREADABLE node.db is still a
 * failure to look, and must not be silently treated as "the key has no
 * prior anchor". Returns the handle, or NULL with *fatal_out set when the
 * caller must refuse rather than proceed. */
static sqlite3 *zidc_open_db_preflight(const char *datadir,
                                       struct node_db *ndb_out,
                                       bool *fatal_out)
{
    sqlite3 *db = NULL;
    *fatal_out = false;
    enum zcl_node_db_ro_status st =
        zcl_native_node_db_open_readonly(datadir, &db, ndb_out, NULL, 0);
    if (st == ZCL_NODE_DB_RO_OK)
        return db;
    if (st != ZCL_NODE_DB_RO_ABSENT)
        *fatal_out = true;
    return NULL;  // raw-return-ok:optional-preflight-open
}

/* Exactly 64 hex chars decoding to a non-zero 32-byte key. */
static bool zidc_parse_key(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64 || !IsHex(hex))
        return false;
    if (ParseHex(hex, out, 32) != 32)
        return false;
    for (int i = 0; i < 32; i++)
        if (out[i])
            return true;
    return false;   /* all-zero is the unset sentinel, never an identity */
}

static void zidc_row_json(const struct zid_identity *r,
                          struct json_value *obj)
{
    json_set_object(obj);
    char hex[65];
    HexStr(r->master_pubkey, 32, false, hex, sizeof(hex));
    json_push_kv_str(obj, "pubkey", hex);
    json_push_kv_str(obj, "name", r->name);
    HexStr(r->anchor_txid, 32, false, hex, sizeof(hex));
    json_push_kv_str(obj, "anchor_txid", hex);
    json_push_kv_int(obj, "anchor_height", r->anchor_height);
    json_push_kv_int(obj, "updated_height", r->updated_height);
    json_push_kv_str(obj, "status", r->status);
    if (r->has_successor) {
        HexStr(r->successor_pubkey, 32, false, hex, sizeof(hex));
        json_push_kv_str(obj, "successor", hex);
    } else {
        json_push_kv_str(obj, "successor", "");
    }
    json_push_kv_str(obj, "source", r->source);
    json_push_kv_str(obj, "owner_address", r->owner_address);
}

/* A node_rpc_call body that is an error, not a result — the transport's
 * own {"error":{...}} envelope, an extracted {code,message}, or a bare
 * string (the RPC handler's own message). Verbatim shape of
 * ep_rpc_body_error in native_epoch_command.c. */
static bool zidc_rpc_body_error(const struct json_value *v, char *msg,
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
static bool zidc_try_rpc(const char *method, const char *params,
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
                      zidc_rpc_body_error(&body, rpc_err, rpc_err_size);
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
static void zidc_offline_reply(struct zcl_command_reply *reply,
                               const char *command, const uint8_t *script,
                               size_t script_len, const char *rpc_err)
{
    char hex[ZID_ANCHOR_SCRIPT_MAX * 2 + 2];
    HexStr(script, script_len, false, hex, sizeof(hex));
    json_push_kv_str(&reply->data, "command", command);
    if (rpc_err && rpc_err[0])
        json_push_kv_str(&reply->data, "node_rpc_error", rpc_err);
    json_push_kv_str(&reply->data, "op_return_hex", hex);
    json_push_kv_int(&reply->data, "op_return_size", (int64_t)script_len);
    json_push_kv_str(&reply->data, "status", "ready");
    json_push_kv_str(&reply->data, "note",
                     "no live node answered — start the node and re-run to "
                     "compose+broadcast with the node wallet, or include "
                     "this OP_RETURN as vout[0] of any transaction you sign "
                     "yourself");
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* Resolve the datadir or fail the reply. */
static const char *zidc_require_datadir(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply, const char *path)
{
    const char *datadir = zidc_datadir(request);
    if (!datadir)
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               path);
    return datadir;
}

/* Read --pubkey into `key`, failing the reply with a named code when it is
 * absent or malformed. */
static bool zidc_require_pubkey(const struct json_value *input,
                                const char *field,
                                struct zcl_command_reply *reply,
                                const char *path, uint8_t key[32])
{
    const char *hex = zidc_input_str(input, field);
    if (!hex || !hex[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "give --%s=<64 hex chars> (the ed25519 master public key)",
                 field);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               strcmp(field, "pubkey") == 0
                                   ? "MISSING_PUBKEY" : "MISSING_NEW_PUBKEY",
                               "normalize", false, false, msg, path);
        return false;
    }
    if (!zidc_parse_key(hex, key)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_PUBKEY_HEX",
                               "normalize", false, false,
                               "pubkey must be exactly 64 hex characters and "
                               "not all-zero — an all-zero key is the unset "
                               "sentinel, never a usable ed25519 point",
                               path);
        return false;
    }
    return true;
}

/* ── core.identity.resolve ────────────────────────────────────────── */

void zcl_native_handle_core_identity_resolve(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *path = "core.identity.resolve";

    const char *pubkey_hex = zidc_input_str(request->input, "pubkey");
    const char *name = zidc_input_str(request->input, "name");
    bool have_pubkey = pubkey_hex && pubkey_hex[0];
    bool have_name = name && name[0];
    if (!have_pubkey && !have_name) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_SELECTOR",
                               "normalize", false, false,
                               "give --pubkey=<64hex> or --name=<znam name> "
                               "— resolve needs exactly one selector", path);
        return;
    }
    if (have_pubkey && have_name) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "AMBIGUOUS_SELECTOR",
                               "normalize", false, false,
                               "give --pubkey OR --name, never both — they "
                               "can disagree and there is no rule for which "
                               "wins", path);
        return;
    }

    uint8_t key[32];
    if (have_pubkey && !zidc_parse_key(pubkey_hex, key)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_PUBKEY_HEX",
                               "normalize", false, false,
                               "pubkey must be exactly 64 hex characters and "
                               "not all-zero", path);
        return;
    }

    const char *datadir = zidc_require_datadir(request, reply, path);
    if (!datadir)
        return;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!zcl_native_node_db_require_readonly(datadir, reply,
                                             "the identity projection",
                                             &db, &ndb))
        return;

    struct zid_identity row;
    bool found = have_pubkey ? db_zid_identity_find(&ndb, key, &row)
                             : db_zid_identity_find_by_name(&ndb, name, &row);
    zcl_native_node_db_close_readonly(&db, &ndb);

    if (!found) {
        if (have_pubkey)
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_FAILED,
                                   "KEY_NOT_ANCHORED", "execute", false,
                                   false,
                                   "this master key has no anchor on the "
                                   "chain this node has folded — it may be "
                                   "unanchored, or anchored above the "
                                   "node's current height", pubkey_hex);
        else
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_FAILED, "NAME_NOT_FOUND",
                                   "execute", false, false,
                                   "no ZNAM name anchors an identity under "
                                   "that name — check the spelling, or the "
                                   "name may set no `zid` text record",
                                   name);
        return;
    }

    struct json_value obj = {0};
    zidc_row_json(&row, &obj);
    json_copy(&reply->data, &obj);
    json_free(&obj);
    json_push_kv_str(&reply->data, "datadir", datadir);
    json_push_kv_bool(&reply->data, "found", true);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* ── core.identity.list ───────────────────────────────────────────── */

void zcl_native_handle_core_identity_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *path = "core.identity.list";

    int64_t limit = zidc_input_int(request->input, "limit",
                                   ZID_CMD_LIST_DEFAULT);
    int64_t offset = zidc_input_int(request->input, "offset", 0);
    if (limit < 1) limit = 1;
    if (limit > ZID_CMD_LIST_CAP) limit = ZID_CMD_LIST_CAP;
    if (offset < 0) offset = 0;

    const char *datadir = zidc_require_datadir(request, reply, path);
    if (!datadir)
        return;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!zcl_native_node_db_require_readonly(datadir, reply,
                                             "the identity projection",
                                             &db, &ndb))
        return;

    struct zid_identity rows[ZID_CMD_LIST_CAP];
    int n = db_zid_identity_list(&ndb, rows, (int)limit, (int)offset);
    int64_t total = db_zid_identity_count(&ndb);
    int64_t active = db_zid_identity_count_by_status(
        &ndb, ZID_IDENTITY_STATUS_ACTIVE);
    int64_t rotated = db_zid_identity_count_by_status(
        &ndb, ZID_IDENTITY_STATUS_ROTATED);
    int64_t revoked = db_zid_identity_count_by_status(
        &ndb, ZID_IDENTITY_STATUS_REVOKED);
    zcl_native_node_db_close_readonly(&db, &ndb);

    json_push_kv_str(&reply->data, "datadir", datadir);
    json_push_kv_int(&reply->data, "limit", limit);
    json_push_kv_int(&reply->data, "offset", offset);
    json_push_kv_int(&reply->data, "total", total);
    json_push_kv_int(&reply->data, "active", active);
    json_push_kv_int(&reply->data, "rotated", rotated);
    json_push_kv_int(&reply->data, "revoked", revoked);

    struct json_value arr = {0};
    json_set_array(&arr);
    for (int i = 0; i < n; i++) {
        struct json_value e = {0};
        zidc_row_json(&rows[i], &e);
        json_push_back(&arr, &e);
        json_free(&e);
    }
    json_push_kv(&reply->data, "identities", &arr);
    json_free(&arr);
    json_push_kv_int(&reply->data, "count", n);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* ── core.identity.anchor ─────────────────────────────────────────── */

void zcl_native_handle_core_identity_anchor(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *path = "core.identity.anchor";

    uint8_t key[32];
    if (!zidc_require_pubkey(request->input, "pubkey", reply, path, key))
        return;
    const char *pubkey_hex = zidc_input_str(request->input, "pubkey");

    /* Pre-flight: a dead or superseded key is never re-anchored, and
     * finding that out costs nothing while broadcasting costs a fee.
     * An ABSENT node.db is NOT fatal here — anchoring a brand new key on a
     * host with no folded chain is a legitimate offline operation. An
     * UNREADABLE one IS fatal: skipping the preflight because the lookup
     * failed would spend a fee re-anchoring a key that may already be
     * revoked, on the strength of a check that never ran. */
    const char *datadir = zidc_datadir(request);
    if (datadir) {
        struct node_db ndb;
        bool db_fatal = false;
        sqlite3 *db = zidc_open_db_preflight(datadir, &ndb, &db_fatal);
        if (db_fatal) {
            zcl_command_reply_fail(
                reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
                "NODE_DB_UNREADABLE", "execute", true, false,
                "node.db is present at this datadir but would not open "
                "read-only, so the already-revoked pre-flight could not "
                "run — refusing rather than spending a fee on an unchecked "
                "anchor; check permissions, or pass a datadir you can read",
                datadir);
            return;
        }
        if (db) {
            struct zid_identity prev;
            memset(&prev, 0, sizeof(prev));
            bool dead = db_zid_identity_find(&ndb, key, &prev) &&
                        strcmp(prev.status,
                               ZID_IDENTITY_STATUS_ACTIVE) != 0;
            char status[ZID_IDENTITY_STATUS_MAX];
            snprintf(status, sizeof(status), "%s", prev.status);
            zcl_native_node_db_close_readonly(&db, &ndb);
            if (dead) {
                zcl_command_reply_fail(
                    reply, ZCL_COMMAND_STATUS_FAILED,
                    ZCL_COMMAND_EXIT_FAILED, "ALREADY_REVOKED", "execute",
                    false, false,
                    strcmp(status, ZID_IDENTITY_STATUS_REVOKED) == 0
                        ? "this key is revoked — a dead key is never "
                          "re-anchored; anchor a fresh key instead"
                        : "this key has already been rotated away — anchor "
                          "its successor, not the superseded key",
                    pubkey_hex);
                return;
            }
        }
    }

    char params[192];
    snprintf(params, sizeof(params), "[{\"pubkey\":\"%s\"}]", pubkey_hex);
    char rpc_err[256] = {0};
    if (zidc_try_rpc("identity_anchor", params, reply, rpc_err,
                     sizeof(rpc_err)))
        return;

    uint8_t script[ZID_ANCHOR_SCRIPT_MAX];
    size_t script_len = zid_anchor_build_anchor(script, sizeof(script), key);
    if (script_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "OP_RETURN_BUILD_FAILED", "execute", false,
                               false, "zid_anchor_build_anchor rejected the "
                               "key", pubkey_hex);
        return;
    }
    json_push_kv_str(&reply->data, "pubkey", pubkey_hex);
    zidc_offline_reply(reply, "anchor", script, script_len, rpc_err);
}

/* ── core.identity.rotate ─────────────────────────────────────────── */

void zcl_native_handle_core_identity_rotate(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *path = "core.identity.rotate";

    uint8_t old_key[32], new_key[32];
    if (!zidc_require_pubkey(request->input, "pubkey", reply, path, old_key))
        return;
    if (!zidc_require_pubkey(request->input, "new_pubkey", reply, path,
                             new_key))
        return;
    const char *old_hex = zidc_input_str(request->input, "pubkey");
    const char *new_hex = zidc_input_str(request->input, "new_pubkey");

    if (memcmp(old_key, new_key, 32) == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "SELF_ROTATE",
                               "normalize", false, false,
                               "pubkey and new_pubkey are the same key — a "
                               "rotation to itself says nothing and the "
                               "overlay refuses it on both sides", old_hex);
        return;
    }

    const char *datadir = zidc_require_datadir(request, reply, path);
    if (!datadir)
        return;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!zcl_native_node_db_require_readonly(datadir, reply,
                                             "the identity projection",
                                             &db, &ndb))
        return;
    struct zid_identity prev;
    bool found = db_zid_identity_find(&ndb, old_key, &prev);
    zcl_native_node_db_close_readonly(&db, &ndb);

    if (!found) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "KEY_NOT_ANCHORED",
                               "execute", false, false,
                               "the key you are rotating away from has no "
                               "anchor row — anchor it first with `core "
                               "identity anchor`", old_hex);
        return;
    }
    if (strcmp(prev.status, ZID_IDENTITY_STATUS_REVOKED) == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "ALREADY_REVOKED",
                               "execute", false, false,
                               "this key is revoked — a dead key is never "
                               "rotated; anchor a fresh key instead",
                               old_hex);
        return;
    }
    if (prev.owner_address[0] == '\0') {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "NOT_OWNER",
                               "execute", false, false,
                               "the anchor row records no owner address, so "
                               "no signer can ever prove ownership of it — "
                               "this identity is permanently immutable by "
                               "design", old_hex);
        return;
    }

    char params[320];
    snprintf(params, sizeof(params),
             "[{\"pubkey\":\"%s\",\"new_pubkey\":\"%s\"}]", old_hex, new_hex);
    char rpc_err[256] = {0};
    if (zidc_try_rpc("identity_rotate", params, reply, rpc_err,
                     sizeof(rpc_err)))
        return;

    uint8_t script[ZID_ANCHOR_SCRIPT_MAX];
    size_t script_len = zid_anchor_build_rotate(script, sizeof(script),
                                                old_key, new_key);
    if (script_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "OP_RETURN_BUILD_FAILED", "execute", false,
                               false, "zid_anchor_build_rotate rejected the "
                               "key pair", old_hex);
        return;
    }
    json_push_kv_str(&reply->data, "pubkey", new_hex);
    json_push_kv_str(&reply->data, "old_pubkey", old_hex);
    json_push_kv_str(&reply->data, "owner_address", prev.owner_address);
    zidc_offline_reply(reply, "rotate", script, script_len, rpc_err);
}

/* ── core.identity.revoke ─────────────────────────────────────────── */

void zcl_native_handle_core_identity_revoke(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *path = "core.identity.revoke";

    uint8_t key[32];
    if (!zidc_require_pubkey(request->input, "pubkey", reply, path, key))
        return;
    const char *pubkey_hex = zidc_input_str(request->input, "pubkey");

    const char *datadir = zidc_require_datadir(request, reply, path);
    if (!datadir)
        return;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!zcl_native_node_db_require_readonly(datadir, reply,
                                             "the identity projection",
                                             &db, &ndb))
        return;
    struct zid_identity prev;
    bool found = db_zid_identity_find(&ndb, key, &prev);
    zcl_native_node_db_close_readonly(&db, &ndb);

    if (!found) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "KEY_NOT_ANCHORED",
                               "execute", false, false,
                               "this key has no anchor row — there is "
                               "nothing on-chain to revoke", pubkey_hex);
        return;
    }
    if (strcmp(prev.status, ZID_IDENTITY_STATUS_REVOKED) == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "ALREADY_REVOKED",
                               "execute", false, false,
                               "this key is already revoked — revoking it "
                               "again would spend a fee to say nothing",
                               pubkey_hex);
        return;
    }
    if (prev.owner_address[0] == '\0') {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "NOT_OWNER",
                               "execute", false, false,
                               "the anchor row records no owner address, so "
                               "no signer can ever prove ownership of it — "
                               "this identity is permanently immutable by "
                               "design", pubkey_hex);
        return;
    }

    char params[192];
    snprintf(params, sizeof(params), "[{\"pubkey\":\"%s\"}]", pubkey_hex);
    char rpc_err[256] = {0};
    if (zidc_try_rpc("identity_revoke", params, reply, rpc_err,
                     sizeof(rpc_err)))
        return;

    uint8_t script[ZID_ANCHOR_SCRIPT_MAX];
    size_t script_len = zid_anchor_build_revoke(script, sizeof(script), key);
    if (script_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "OP_RETURN_BUILD_FAILED", "execute", false,
                               false, "zid_anchor_build_revoke rejected the "
                               "key", pubkey_hex);
        return;
    }
    json_push_kv_str(&reply->data, "pubkey", pubkey_hex);
    json_push_kv_str(&reply->data, "owner_address", prev.owner_address);
    zidc_offline_reply(reply, "revoke", script, script_len, rpc_err);
}
