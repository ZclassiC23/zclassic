/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the `core epoch` tree — the Bounded Node keystone
 * (docs/spec/sovereign-identity-layer.md). The OP_RETURN catalog projection
 * (app/models/src/op_return_index.c, folded forward by the backfill
 * service) maintains an incremental digest-chain over EVERY OP_RETURN the
 * chain has ever carried; anchoring that digest in a ZANC anchor commits
 * the entire overlay state (ZNAM/ZSLP/ZANC/ZID) in one ~40-byte tx.
 *
 * v1 discipline: NO background service, NO auto-broadcast. Anchoring
 * spends fees, so it is an operator decision, triggered only by
 * `core epoch anchor`. Label semantics: an epoch anchor is a ZANC anchor
 * labeled "zepoch@<H>" where H is the catalog cursor height the digest
 * was read at; an anchor counts as covering the current epoch E =
 * tip/1000 when its label height H >= E*1000. These semantics match
 * zepoch_status_dump_state_json (dumpstate zepoch) exactly.
 *
 * Reads open <datadir>/node.db READONLY (the core.storage.query.offline
 * pattern) so status/verify also answer for a stopped or copied datadir.
 * `core epoch anchor` prefers the LIVE node: it dispatches the wallet
 * compose+broadcast through the anchor_publish RPC (which itself falls
 * back to op_return_hex when no wallet is loaded); with no live node it
 * builds the same OP_RETURN locally and returns op_return_hex with a
 * next-step hint. */

#include "command/native_command.h"

#include "controllers/rpc_client.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "models/database.h"
#include "models/op_return_index.h"
#include "models/zanc.h"
#include "zanc/zanc.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EP_LOG "core.epoch"

/* ── small helpers ────────────────────────────────────────────────── */

static const char *ep_input_str(const struct json_value *input,
                                const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

/* Resolve the target datadir: explicit input.datadir wins, else the CLI's
 * --datadir (core.node.bootstatus precedent). NULL when neither is set. */
static const char *ep_datadir(const struct zcl_command_request *request)
{
    const char *dd = ep_input_str(request->input, "datadir");
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

/* Open <datadir>/node.db READONLY (no CREATE) + ad-hoc node_db wrapper.
 * On failure, reply is filled and false returned. */
static bool ep_open_catalog(const char *datadir,
                            struct zcl_command_reply *reply,
                            sqlite3 **db_out, struct node_db *ndb_out)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/node.db", datadir);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               "DATADIR_PATH_TOO_LONG", "normalize", false,
                               false, "datadir path too long", datadir);
        return false;
    }
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNAVAILABLE", "execute", true, false,
                               "node.db not found or unreadable at datadir — "
                               "check --datadir, or boot the node once to "
                               "create it", path);
        return false;
    }
    (void)sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 2000);
    *db_out = db;
    memset(ndb_out, 0, sizeof(*ndb_out));
    ndb_out->db = db;
    ndb_out->open = true;
    return true;
}

/* Current catalog state: cursor height + digest + row count. */
static bool ep_read_catalog(struct node_db *ndb, int32_t *cursor_out,
                            uint8_t digest_out[32], int64_t *rows_out)
{
    if (!op_return_index_get_cursor(ndb, cursor_out, digest_out))
        return false;
    *rows_out = op_return_index_count(ndb);
    return true;
}

/* Latest zepoch anchor with label height >= min_height (labels are
 * "zepoch@<H>", newest-first list scan). Returns true and fills *out. */
static bool ep_find_anchor(struct node_db *ndb, int32_t min_height,
                           struct zanc_anchor *out)
{
    struct zanc_anchor rows[100];
    int n = db_zanc_list(ndb, rows, 100);
    bool found = false;
    int32_t best = -1;
    for (int i = 0; i < n; i++) {
        if (strncmp(rows[i].label, "zepoch@", 7) != 0)
            continue;
        const char *hstr = rows[i].label + 7;
        if (!hstr[0])
            continue;
        char *end = NULL;
        long h = strtol(hstr, &end, 10);
        if (!end || *end != '\0' || h < min_height)
            continue;
        if (!found || h > best) {
            best = (int32_t)h;
            *out = rows[i];
            found = true;
        }
    }
    return found;
}

static void ep_anchor_json(const struct zanc_anchor *a, int32_t cursor,
                           struct json_value *obj)
{
    json_set_object(obj);
    json_push_kv_str(obj, "label", a->label);
    char hex[65];
    HexStr(a->txid, 32, false, hex, sizeof(hex));
    json_push_kv_str(obj, "txid", hex);
    json_push_kv_int(obj, "height", a->height);
    json_push_kv_int(obj, "confirmations",
                     cursor >= a->height ? (int64_t)(cursor - a->height + 1)
                                         : 0);
    HexStr(a->digest, 32, false, hex, sizeof(hex));
    json_push_kv_str(obj, "digest", hex);
}

/* ── core.epoch.status ─────────────────────────────────────────────── */

void zcl_native_handle_core_epoch_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = ep_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               "core.epoch.status");
        return;
    }

    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!ep_open_catalog(datadir, reply, &db, &ndb))
        return;

    int32_t cursor = -1;
    uint8_t digest[32] = {0};
    int64_t rows = 0;
    bool have = ep_read_catalog(&ndb, &cursor, digest, &rows);

    json_push_kv_str(&reply->data, "datadir", datadir);
    if (!have) {
        sqlite3_close(db);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "CATALOG_CURSOR_UNREADABLE", "execute", true,
                               false,
                               "catalog cursor unreadable — the op_return "
                               "backfill has not folded anything yet; boot "
                               "the node and let the backfill advance",
                               datadir);
        return;
    }

    json_push_kv_int(&reply->data, "tip_height", cursor);
    char hex[65];
    HexStr(digest, 32, false, hex, sizeof(hex));
    json_push_kv_str(&reply->data, "catalog_digest", hex);
    json_push_kv_int(&reply->data, "catalog_rows", rows);

    int64_t epoch = cursor >= 0 ? cursor / 1000 : -1;
    json_push_kv_int(&reply->data, "epoch", epoch);
    json_push_kv_int(&reply->data, "epoch_start", epoch >= 0 ? epoch * 1000 : -1);
    json_push_kv_int(&reply->data, "blocks_into_epoch",
                     epoch >= 0 ? (int64_t)cursor - epoch * 1000 : -1);

    struct zanc_anchor a;
    bool anchored = epoch >= 0 &&
                    ep_find_anchor(&ndb, (int32_t)(epoch * 1000), &a);
    json_push_kv_bool(&reply->data, "anchored", anchored);
    if (anchored) {
        struct json_value aj = {0};
        ep_anchor_json(&a, cursor, &aj);
        json_push_kv(&reply->data, "anchor", &aj);
        json_free(&aj);
        json_push_kv_bool(&reply->data, "digest_match",
                          memcmp(a.digest, digest, 32) == 0);
    } else {
        json_push_kv_str(&reply->data, "next",
                         "no zepoch anchor in the current epoch — run "
                         "`zclassic23 core epoch anchor` to commit the "
                         "catalog digest on-chain (operator decision; "
                         "spends a fee)");
    }
    sqlite3_close(db);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* ── core.epoch.anchor ─────────────────────────────────────────────── */

void zcl_native_handle_core_epoch_anchor(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = ep_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               "core.epoch.anchor");
        return;
    }

    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!ep_open_catalog(datadir, reply, &db, &ndb))
        return;

    int32_t cursor = -1;
    uint8_t digest[32] = {0};
    int64_t rows = 0;
    bool have = ep_read_catalog(&ndb, &cursor, digest, &rows);
    sqlite3_close(db);

    if (!have || cursor < 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "CATALOG_EMPTY", "execute", true, false,
                               "the OP_RETURN catalog has no folded digest "
                               "yet — boot the node and let the op_return "
                               "backfill advance, then re-run",
                               datadir);
        return;
    }

    char digest_hex[65];
    HexStr(digest, 32, false, digest_hex, sizeof(digest_hex));
    char label[ZANC_LABEL_MAX + 1];
    snprintf(label, sizeof(label), "zepoch@%d", (int)cursor);

    /* Live-node path: anchor_publish composes + broadcasts when the node
     * has a wallet loaded, and itself returns op_return_hex when not. */
    char params[256];
    snprintf(params, sizeof(params),
             "{\"digest\":\"%s\",\"hash_type\":\"sha3\",\"label\":\"%s\"}",
             digest_hex, label);
    zcl_native_bridge_ensure_rpc();
    char *rpc_result = node_rpc_call("anchor_publish", params);
    if (rpc_result) {
        struct json_value body;
        if (json_read(&body, rpc_result, strlen(rpc_result))) {
            json_push_kv_str(&body, "via", "node_rpc anchor_publish");
            json_push_kv_int(&body, "catalog_height", cursor);
            json_copy(&reply->data, &body);
            json_free(&body);
            free(rpc_result);
            reply->status = ZCL_COMMAND_STATUS_PASSED;
            reply->exit_code = ZCL_COMMAND_EXIT_OK;
            return;
        }
        free(rpc_result);
    }

    /* Offline / no-live-node path: build the same OP_RETURN locally. */
    uint8_t script[128];
    size_t script_len = zanc_build_anchor(script, sizeof(script),
                                          ZANC_HASH_SHA3_256, digest, label);
    if (script_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "OP_RETURN_BUILD_FAILED", "execute", false,
                               false, "zanc_build_anchor rejected the digest/"
                               "label", label);
        return;
    }

    json_push_kv_str(&reply->data, "hash_type", "sha3");
    json_push_kv_str(&reply->data, "digest", digest_hex);
    json_push_kv_str(&reply->data, "label", label);
    json_push_kv_int(&reply->data, "catalog_height", cursor);
    char hex[257];
    HexStr(script, script_len, false, hex, sizeof(hex));
    json_push_kv_str(&reply->data, "op_return_hex", hex);
    json_push_kv_int(&reply->data, "op_return_size", (int64_t)script_len);
    json_push_kv_str(&reply->data, "status", "ready");
    json_push_kv_str(&reply->data, "note",
                     "no live node answered — start the node and re-run "
                     "`core epoch anchor` to compose+broadcast with the "
                     "node wallet, or include this OP_RETURN manually as "
                     "vout[0] of any transaction");
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* ── core.epoch.verify ─────────────────────────────────────────────── */

void zcl_native_handle_core_epoch_verify(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = ep_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               "core.epoch.verify");
        return;
    }

    int64_t want_height = -1;
    const struct json_value *hv = json_get(request->input, "height");
    if (hv && hv->type == JSON_INT)
        want_height = json_get_int(hv);

    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!ep_open_catalog(datadir, reply, &db, &ndb))
        return;

    int32_t cursor = -1;
    uint8_t digest[32] = {0};
    int64_t rows = 0;
    bool have = ep_read_catalog(&ndb, &cursor, digest, &rows);

    json_push_kv_str(&reply->data, "datadir", datadir);
    (void)rows;

    if (want_height >= 0) {
        /* Lookup-only: the catalog digest-chain folds forward and is only
         * recomputable at the tip, never at a historical height. */
        char label[ZANC_LABEL_MAX + 1];
        snprintf(label, sizeof(label), "zepoch@%d", (int)want_height);
        struct zanc_anchor rows_[100];
        int n = db_zanc_list(&ndb, rows_, 100);
        bool found = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(rows_[i].label, label) == 0) {
                struct json_value aj = {0};
                ep_anchor_json(&rows_[i], cursor, &aj);
                json_push_kv(&reply->data, "anchor", &aj);
                json_free(&aj);
                found = true;
                break;
            }
        }
        json_push_kv_int(&reply->data, "height", want_height);
        json_push_kv_bool(&reply->data, "anchored", found);
        json_push_kv_str(&reply->data, "note",
                         "lookup-only: digest recompute is tip-only — the "
                         "catalog digest-chain cannot be re-derived at a "
                         "historical height, so no digest comparison is "
                         "made here");
        sqlite3_close(db);
        reply->status = ZCL_COMMAND_STATUS_PASSED;
        reply->exit_code = ZCL_COMMAND_EXIT_OK;
        return;
    }

    if (!have || cursor < 0) {
        sqlite3_close(db);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "CATALOG_EMPTY", "execute", true, false,
                               "no folded catalog digest at the tip — boot "
                               "the node and let the backfill advance",
                               datadir);
        return;
    }

    /* Tip path: fresh-read digest, latest zepoch anchor, compare. */
    char hex[65];
    HexStr(digest, 32, false, hex, sizeof(hex));
    json_push_kv_int(&reply->data, "tip_height", cursor);
    json_push_kv_str(&reply->data, "catalog_digest", hex);
    int64_t epoch = cursor / 1000;
    json_push_kv_int(&reply->data, "epoch", epoch);

    struct zanc_anchor a;
    bool anchored = ep_find_anchor(&ndb, (int32_t)(epoch * 1000), &a);
    json_push_kv_bool(&reply->data, "anchored", anchored);
    if (anchored) {
        struct json_value aj = {0};
        ep_anchor_json(&a, cursor, &aj);
        json_push_kv(&reply->data, "anchor", &aj);
        json_free(&aj);
        bool match = memcmp(a.digest, digest, 32) == 0;
        json_push_kv_bool(&reply->data, "digest_match", match);
        if (!match)
            json_push_kv_str(&reply->data, "note",
                             "digest mismatch — the catalog has advanced "
                             "past the anchored digest (or the anchor "
                             "committed different bytes); re-anchor with "
                             "`core epoch anchor` if this epoch is not yet "
                             "committed");
    } else {
        json_push_kv_str(&reply->data, "next",
                         "no zepoch anchor in the current epoch — run "
                         "`zclassic23 core epoch anchor` to commit");
    }
    sqlite3_close(db);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}
