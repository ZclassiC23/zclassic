/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the `core identity` native leaves
 * (tools/command/native_identity_command.c) and for the chain-rooted
 * `--anchored` mode of `zcode release verify`
 * (tools/command/native_zcode_release_command.c).
 *
 * Every case builds a REAL on-disk fixture datadir with a real node.db,
 * seeds the zid_identities projection through the model's own write API,
 * and calls the handler function directly — the
 * test_offline_datadir_query.c harness shape, because the handlers under
 * test open real files at a caller-supplied path.
 *
 * NO NODE, NO BROADCAST. node_rpc_call is stubbed to return NULL for the
 * whole file, so every write command deterministically takes its OFFLINE
 * branch: it emits op_return_hex and stops. Nothing here composes a
 * transaction, and nothing here touches the network.
 *
 * The un-fakeable case is (6): the hex a write command hands the operator
 * is fed straight back to zid_anchor_parse, so the two halves of the
 * codec must agree byte for byte or the test fails.
 *
 * Case (9) is the point of the whole surface — it pins that "the
 * signature is valid" and "the key is anchored on-chain" are two
 * SEPARATE reported facts that never collapse into one: a tampered doc
 * signed under an anchored key still fails BAD_SIGNATURE, and a valid
 * doc under an unanchored key still fails KEY_NOT_ANCHORED. */

#include "test/test_core.h"

#include "command/native_command.h"
#include "controllers/rpc_client.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "models/database.h"
#include "models/zid_identity.h"
#include "zid/zid.h"
#include "zid/zid_anchor.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define IDC_CHECK(name, expr) do {                                    \
    printf("identity_command: %s... ", (name));                       \
    if (expr) { printf("OK\n"); }                                     \
    else { printf("FAIL\n"); failures++; }                            \
} while (0)

/* ── fixture ──────────────────────────────────────────────────────── */

/* No live node for the whole file: node_rpc_call answers NULL, so every
 * write command falls through to its offline op_return_hex branch. */
static char *idc_rpc_hook(const char *method, const char *params_json)
{
    (void)method;
    (void)params_json;
    return NULL;
}

static void idc_mk_key(uint8_t out[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++)
        out[i] = (uint8_t)(seed + i);
}

static void idc_hex32(const uint8_t b[32], char out[65])
{
    HexStr(b, 32, false, out, 65);
}

/* Build <dir>/node.db (schema comes from node_db_open) and stash one row
 * per caller request. Returns false on any failure. */
static bool idc_save_row(const char *dir, const struct zid_identity *row)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/node.db", dir);
    struct node_db ndb;
    if (!node_db_open(&ndb, path))
        return false;
    bool ok = db_zid_identity_save(&ndb, row);
    node_db_close(&ndb);
    return ok;
}

/* Create the fixture datadir and its node.db. */
static bool idc_mk_datadir(char *dir, size_t dir_size, const char *tag)
{
    test_fmt_tmpdir(dir, dir_size, "identity_command", tag);
    mkdir("./test-tmp", 0700);
    mkdir(dir, 0700);
    char path[512];
    snprintf(path, sizeof(path), "%s/node.db", dir);
    struct node_db ndb;
    if (!node_db_open(&ndb, path))
        return false;
    node_db_close(&ndb);
    return true;
}

static void idc_fill(struct zid_identity *r, const uint8_t key[32],
                     int32_t height, const char *status, const char *source,
                     const char *name, const char *owner)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->master_pubkey, key, 32);
    memset(r->anchor_txid, 0x5a, 32);
    r->anchor_height = height;
    r->updated_height = height;
    snprintf(r->status, sizeof(r->status), "%s", status);
    snprintf(r->source, sizeof(r->source), "%s", source);
    if (name) snprintf(r->name, sizeof(r->name), "%s", name);
    if (owner) snprintf(r->owner_address, sizeof(r->owner_address), "%s",
                        owner);
}

/* Run one handler against {datadir, ...} and hand the reply back. The
 * caller owns `reply` (zcl_command_reply_free) and `input` (json_free). */
static void idc_call(void (*fn)(const struct zcl_command_request *,
                                struct zcl_command_reply *),
                     struct json_value *input,
                     struct zcl_command_reply *reply, const char *schema)
{
    struct zcl_command_request request = { .input = input };
    zcl_command_reply_init(reply, schema);
    fn(&request, reply);
}

static void idc_input_open(struct json_value *input, const char *dir)
{
    json_init(input);
    json_set_object(input);
    if (dir)
        (void)json_push_kv_str(input, "datadir", dir);
}

static const char *idc_str(const struct zcl_command_reply *reply,
                           const char *key)
{
    const char *s = json_get_str(json_get(&reply->data, key));
    return s ? s : "";
}

/* ── (1)+(2)+(3)+(4) resolve ──────────────────────────────────────── */

static int t_resolve(void)
{
    int failures = 0;
    char dir[256];
    IDC_CHECK("resolve fixture: datadir",
              idc_mk_datadir(dir, sizeof(dir), "resolve"));

    uint8_t key_a[32], key_b[32], key_missing[32];
    idc_mk_key(key_a, 0x11);
    idc_mk_key(key_b, 0x40);
    idc_mk_key(key_missing, 0x90);
    char hex_a[65], hex_b[65], hex_missing[65];
    idc_hex32(key_a, hex_a);
    idc_hex32(key_b, hex_b);
    idc_hex32(key_missing, hex_missing);

    struct zid_identity row;
    /* key_a: rotated toward key_b, anchored via the ZNAM text convention. */
    idc_fill(&row, key_a, 1000, ZID_IDENTITY_STATUS_ROTATED,
             ZID_IDENTITY_SOURCE_ZNAM_TEXT, "alice", "t1owner");
    memcpy(row.successor_pubkey, key_b, 32);
    row.has_successor = true;
    row.updated_height = 1200;
    IDC_CHECK("resolve fixture: rotated row", idc_save_row(dir, &row));
    /* key_b: the live successor, anchored via the ZID overlay. */
    idc_fill(&row, key_b, 1200, ZID_IDENTITY_STATUS_ACTIVE,
             ZID_IDENTITY_SOURCE_ZID_OVERLAY, NULL, "t1owner");
    IDC_CHECK("resolve fixture: active row", idc_save_row(dir, &row));

    /* by pubkey */
    struct json_value input;
    struct zcl_command_reply reply;
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_a);
    idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
             "zcl.core_identity_resolve.v1");
    IDC_CHECK("resolve by pubkey: exit OK",
              reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("resolve by pubkey: pubkey echoed",
              strcmp(idc_str(&reply, "pubkey"), hex_a) == 0);
    IDC_CHECK("resolve by pubkey: name",
              strcmp(idc_str(&reply, "name"), "alice") == 0);
    IDC_CHECK("resolve by pubkey: status rotated",
              strcmp(idc_str(&reply, "status"), "rotated") == 0);
    IDC_CHECK("resolve by pubkey: successor is key_b",
              strcmp(idc_str(&reply, "successor"), hex_b) == 0);
    IDC_CHECK("resolve by pubkey: source znam_text",
              strcmp(idc_str(&reply, "source"), "znam_text") == 0);
    IDC_CHECK("resolve by pubkey: anchor_height",
              json_get_int(json_get(&reply.data, "anchor_height")) == 1000);
    IDC_CHECK("resolve by pubkey: updated_height",
              json_get_int(json_get(&reply.data, "updated_height")) == 1200);
    IDC_CHECK("resolve by pubkey: anchor_txid is 64 hex",
              strlen(idc_str(&reply, "anchor_txid")) == 64);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* by name */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "name", "alice");
    idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
             "zcl.core_identity_resolve.v1");
    IDC_CHECK("resolve by name: exit OK",
              reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("resolve by name: same row as by pubkey",
              strcmp(idc_str(&reply, "pubkey"), hex_a) == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* unknown key -> KEY_NOT_ANCHORED */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_missing);
    idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
             "zcl.core_identity_resolve.v1");
    IDC_CHECK("resolve unknown key: FAILED",
              reply.status == ZCL_COMMAND_STATUS_FAILED);
    IDC_CHECK("resolve unknown key: KEY_NOT_ANCHORED",
              strcmp(reply.error.code, "KEY_NOT_ANCHORED") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* unknown name -> NAME_NOT_FOUND */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "name", "nobody");
    idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
             "zcl.core_identity_resolve.v1");
    IDC_CHECK("resolve unknown name: NAME_NOT_FOUND",
              strcmp(reply.error.code, "NAME_NOT_FOUND") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* no selector -> MISSING_SELECTOR */
    idc_input_open(&input, dir);
    idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
             "zcl.core_identity_resolve.v1");
    IDC_CHECK("resolve no selector: MISSING_SELECTOR",
              strcmp(reply.error.code, "MISSING_SELECTOR") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* both selectors -> AMBIGUOUS_SELECTOR */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_a);
    (void)json_push_kv_str(&input, "name", "alice");
    idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
             "zcl.core_identity_resolve.v1");
    IDC_CHECK("resolve both selectors: AMBIGUOUS_SELECTOR",
              strcmp(reply.error.code, "AMBIGUOUS_SELECTOR") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* malformed pubkey -> BAD_PUBKEY_HEX (short, non-hex, all-zero) */
    static const char *const bad[] = {
        "deadbeef",
        "zz11111111111111111111111111111111111111111111111111111111111111",
        "0000000000000000000000000000000000000000000000000000000000000000",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        idc_input_open(&input, dir);
        (void)json_push_kv_str(&input, "pubkey", bad[i]);
        idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
                 "zcl.core_identity_resolve.v1");
        IDC_CHECK("resolve malformed pubkey: BAD_PUBKEY_HEX",
                  strcmp(reply.error.code, "BAD_PUBKEY_HEX") == 0);
        zcl_command_reply_free(&reply);
        json_free(&input);
    }

    /* no datadir at all -> MISSING_DATADIR, never a crash */
    idc_input_open(&input, NULL);
    (void)json_push_kv_str(&input, "pubkey", hex_a);
    idc_call(zcl_native_handle_core_identity_resolve, &input, &reply,
             "zcl.core_identity_resolve.v1");
    IDC_CHECK("resolve no datadir: named error, no crash",
              reply.status == ZCL_COMMAND_STATUS_FAILED &&
              reply.error.code[0] != '\0');
    zcl_command_reply_free(&reply);
    json_free(&input);

    return failures;
}

/* ── (5) list paging ──────────────────────────────────────────────── */

static int t_list(void)
{
    int failures = 0;
    char dir[256];
    IDC_CHECK("list fixture: datadir",
              idc_mk_datadir(dir, sizeof(dir), "list"));

    bool seeded = true;
    for (int i = 0; i < 5; i++) {
        uint8_t k[32];
        idc_mk_key(k, (uint8_t)(0x20 + i));
        struct zid_identity row;
        idc_fill(&row, k, 100 + i, i == 4 ? ZID_IDENTITY_STATUS_REVOKED
                                          : ZID_IDENTITY_STATUS_ACTIVE,
                 ZID_IDENTITY_SOURCE_ZID_OVERLAY, NULL, "t1owner");
        seeded = seeded && idc_save_row(dir, &row);
    }
    IDC_CHECK("list fixture: five rows", seeded);

    struct json_value input;
    struct zcl_command_reply reply;
    idc_input_open(&input, dir);
    (void)json_push_kv_int(&input, "limit", 2);
    idc_call(zcl_native_handle_core_identity_list, &input, &reply,
             "zcl.core_identity_index.v1");
    IDC_CHECK("list: exit OK", reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("list: total is 5",
              json_get_int(json_get(&reply.data, "total")) == 5);
    IDC_CHECK("list: active is 4",
              json_get_int(json_get(&reply.data, "active")) == 4);
    IDC_CHECK("list: revoked is 1",
              json_get_int(json_get(&reply.data, "revoked")) == 1);
    IDC_CHECK("list: limit caps the page",
              json_get_int(json_get(&reply.data, "count")) == 2);
    const struct json_value *arr = json_get(&reply.data, "identities");
    IDC_CHECK("list: two identities returned",
              arr && arr->type == JSON_ARR && arr->num_children == 2);
    /* newest anchor first: heights 104 then 103 */
    IDC_CHECK("list: newest anchor first",
              arr && arr->num_children == 2 &&
              json_get_int(json_get(json_at(arr, 0), "anchor_height")) == 104 &&
              json_get_int(json_get(json_at(arr, 1), "anchor_height")) == 103);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* offset walks the page */
    idc_input_open(&input, dir);
    (void)json_push_kv_int(&input, "limit", 2);
    (void)json_push_kv_int(&input, "offset", 2);
    idc_call(zcl_native_handle_core_identity_list, &input, &reply,
             "zcl.core_identity_index.v1");
    arr = json_get(&reply.data, "identities");
    IDC_CHECK("list: offset walks to height 102",
              arr && arr->num_children == 2 &&
              json_get_int(json_get(json_at(arr, 0), "anchor_height")) == 102);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* an absurd limit is clamped, never honoured */
    idc_input_open(&input, dir);
    (void)json_push_kv_int(&input, "limit", 100000);
    idc_call(zcl_native_handle_core_identity_list, &input, &reply,
             "zcl.core_identity_index.v1");
    IDC_CHECK("list: limit clamped to the cap",
              json_get_int(json_get(&reply.data, "limit")) == 100);
    zcl_command_reply_free(&reply);
    json_free(&input);

    return failures;
}

/* ── (6)+(7) the offline op_return_hex round-trip ─────────────────── */

/* Take reply.data.op_return_hex, decode it and parse it back through the
 * ZID codec. This is the un-fakeable half: the builder and the parser must
 * agree byte for byte. */
static bool idc_roundtrip(const struct zcl_command_reply *reply,
                          struct zid_anchor_message *msg)
{
    const char *hex = json_get_str(json_get(&reply->data, "op_return_hex"));
    if (!hex || !hex[0])
        return false;
    size_t len = strlen(hex);
    if ((len & 1u) != 0 || len > ZID_ANCHOR_SCRIPT_MAX * 2 || !IsHex(hex))
        return false;
    uint8_t script[ZID_ANCHOR_SCRIPT_MAX];
    int n = ParseHex(hex, script, sizeof(script));
    if (n <= 0)
        return false;
    return zid_anchor_parse(script, (size_t)n, msg);
}

static int t_write_offline(void)
{
    int failures = 0;
    char dir[256];
    IDC_CHECK("write fixture: datadir",
              idc_mk_datadir(dir, sizeof(dir), "write"));

    uint8_t key_a[32], key_b[32];
    idc_mk_key(key_a, 0x11);
    idc_mk_key(key_b, 0x40);
    char hex_a[65], hex_b[65];
    idc_hex32(key_a, hex_a);
    idc_hex32(key_b, hex_b);

    struct zid_identity row;
    idc_fill(&row, key_a, 500, ZID_IDENTITY_STATUS_ACTIVE,
             ZID_IDENTITY_SOURCE_ZID_OVERLAY, NULL, "t1owner");
    IDC_CHECK("write fixture: active row", idc_save_row(dir, &row));

    struct json_value input;
    struct zcl_command_reply reply;
    struct zid_anchor_message msg;

    /* anchor: an unanchored key is a legitimate first anchor */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_b);
    idc_call(zcl_native_handle_core_identity_anchor, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("anchor offline: exit OK",
              reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("anchor offline: status ready",
              strcmp(idc_str(&reply, "status"), "ready") == 0);
    IDC_CHECK("anchor offline: hex round-trips through zid_anchor_parse",
              idc_roundtrip(&reply, &msg));
    IDC_CHECK("anchor offline: command is ANCHOR",
              msg.command == ZID_ANCHOR_CMD_ANCHOR);
    IDC_CHECK("anchor offline: subject key is byte-identical",
              memcmp(msg.pubkey, key_b, 32) == 0);
    IDC_CHECK("anchor offline: no old key on an ANCHOR",
              !msg.has_old_pubkey);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* rotate key_a -> key_b */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_a);
    (void)json_push_kv_str(&input, "new_pubkey", hex_b);
    idc_call(zcl_native_handle_core_identity_rotate, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("rotate offline: exit OK",
              reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("rotate offline: hex round-trips",
              idc_roundtrip(&reply, &msg));
    IDC_CHECK("rotate offline: command is ROTATE",
              msg.command == ZID_ANCHOR_CMD_ROTATE);
    IDC_CHECK("rotate offline: old key preserved",
              msg.has_old_pubkey && memcmp(msg.old_pubkey, key_a, 32) == 0);
    IDC_CHECK("rotate offline: new key preserved",
              memcmp(msg.pubkey, key_b, 32) == 0);
    IDC_CHECK("rotate offline: owner echoed for the operator",
              strcmp(idc_str(&reply, "owner_address"), "t1owner") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* revoke key_a */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_a);
    idc_call(zcl_native_handle_core_identity_revoke, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("revoke offline: exit OK",
              reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("revoke offline: hex round-trips",
              idc_roundtrip(&reply, &msg));
    IDC_CHECK("revoke offline: command is REVOKE",
              msg.command == ZID_ANCHOR_CMD_REVOKE);
    IDC_CHECK("revoke offline: subject key preserved",
              memcmp(msg.pubkey, key_a, 32) == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    return failures;
}

/* ── (8) the named refusals ───────────────────────────────────────── */

static int t_write_refusals(void)
{
    int failures = 0;
    char dir[256];
    IDC_CHECK("refusal fixture: datadir",
              idc_mk_datadir(dir, sizeof(dir), "refuse"));

    uint8_t key_live[32], key_dead[32], key_absent[32], key_orphan[32];
    idc_mk_key(key_live, 0x11);
    idc_mk_key(key_dead, 0x33);
    idc_mk_key(key_absent, 0x77);
    idc_mk_key(key_orphan, 0x99);
    char hex_live[65], hex_dead[65], hex_absent[65], hex_orphan[65];
    idc_hex32(key_live, hex_live);
    idc_hex32(key_dead, hex_dead);
    idc_hex32(key_absent, hex_absent);
    idc_hex32(key_orphan, hex_orphan);

    struct zid_identity row;
    idc_fill(&row, key_live, 500, ZID_IDENTITY_STATUS_ACTIVE,
             ZID_IDENTITY_SOURCE_ZID_OVERLAY, NULL, "t1owner");
    IDC_CHECK("refusal fixture: live row", idc_save_row(dir, &row));
    idc_fill(&row, key_dead, 501, ZID_IDENTITY_STATUS_REVOKED,
             ZID_IDENTITY_SOURCE_ZID_OVERLAY, NULL, "t1owner");
    IDC_CHECK("refusal fixture: revoked row", idc_save_row(dir, &row));
    /* an anchor published from a non-P2PKH input records no owner */
    idc_fill(&row, key_orphan, 502, ZID_IDENTITY_STATUS_ACTIVE,
             ZID_IDENTITY_SOURCE_ZID_OVERLAY, NULL, NULL);
    IDC_CHECK("refusal fixture: ownerless row", idc_save_row(dir, &row));

    struct json_value input;
    struct zcl_command_reply reply;

    /* rotate to itself */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_live);
    (void)json_push_kv_str(&input, "new_pubkey", hex_live);
    idc_call(zcl_native_handle_core_identity_rotate, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("rotate self: SELF_ROTATE",
              strcmp(reply.error.code, "SELF_ROTATE") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* rotate an absent key */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_absent);
    (void)json_push_kv_str(&input, "new_pubkey", hex_live);
    idc_call(zcl_native_handle_core_identity_rotate, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("rotate absent key: KEY_NOT_ANCHORED",
              strcmp(reply.error.code, "KEY_NOT_ANCHORED") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* rotate a revoked key */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_dead);
    (void)json_push_kv_str(&input, "new_pubkey", hex_live);
    idc_call(zcl_native_handle_core_identity_rotate, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("rotate revoked key: ALREADY_REVOKED",
              strcmp(reply.error.code, "ALREADY_REVOKED") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* rotate an ownerless row — nobody can ever prove ownership */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_orphan);
    (void)json_push_kv_str(&input, "new_pubkey", hex_absent);
    idc_call(zcl_native_handle_core_identity_rotate, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("rotate ownerless row: NOT_OWNER",
              strcmp(reply.error.code, "NOT_OWNER") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* revoke an absent key / a revoked key */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_absent);
    idc_call(zcl_native_handle_core_identity_revoke, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("revoke absent key: KEY_NOT_ANCHORED",
              strcmp(reply.error.code, "KEY_NOT_ANCHORED") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_dead);
    idc_call(zcl_native_handle_core_identity_revoke, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("revoke revoked key: ALREADY_REVOKED",
              strcmp(reply.error.code, "ALREADY_REVOKED") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* re-anchoring a dead key */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_dead);
    idc_call(zcl_native_handle_core_identity_anchor, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("anchor revoked key: ALREADY_REVOKED",
              strcmp(reply.error.code, "ALREADY_REVOKED") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* every write with no pubkey at all names its own refusal */
    idc_input_open(&input, dir);
    idc_call(zcl_native_handle_core_identity_anchor, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("anchor no pubkey: MISSING_PUBKEY",
              strcmp(reply.error.code, "MISSING_PUBKEY") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "pubkey", hex_live);
    idc_call(zcl_native_handle_core_identity_rotate, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("rotate no new_pubkey: MISSING_NEW_PUBKEY",
              strcmp(reply.error.code, "MISSING_NEW_PUBKEY") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    idc_input_open(&input, dir);
    idc_call(zcl_native_handle_core_identity_revoke, &input, &reply,
             "zcl.core_identity_anchor.v1");
    IDC_CHECK("revoke no pubkey: MISSING_PUBKEY",
              strcmp(reply.error.code, "MISSING_PUBKEY") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    return failures;
}

/* ── (9) zcode release verify --anchored ──────────────────────────── */

/* Sign a release doc under `seed` and hand back its canonical wire hex
 * plus the master pubkey the signature commits to. */
static bool idc_sign_release(const uint8_t seed[32], const char *name,
                             const char *version, char *hex_out,
                             size_t hex_size, uint8_t pubkey_out[32])
{
    struct zid_release rel;
    memset(&rel, 0, sizeof(rel));
    snprintf(rel.name, sizeof(rel.name), "%s", name);
    snprintf(rel.version, sizeof(rel.version), "%s", version);
    memset(rel.manifest_root, 0x7c, 32);

    struct zid_doc doc;
    memset(&doc, 0, sizeof(doc));
    /* An expiry far past any plausible test clock: the case under test is
     * anchor status, and an expired doc would fail for the wrong reason. */
    if (!zid_release_sign(&doc, &rel, 1, 4102444800ull, seed))
        return false;
    uint8_t wire[ZID_DOC_MAX];
    size_t n = zid_doc_encode(wire, sizeof(wire), &doc);
    if (n == 0 || n * 2 + 1 > hex_size)
        return false;
    HexStr(wire, n, false, hex_out, hex_size);
    memcpy(pubkey_out, doc.master_pubkey, 32);
    return true;
}

static int t_verify_anchored(void)
{
    int failures = 0;
    char dir[256];
    IDC_CHECK("anchored fixture: datadir",
              idc_mk_datadir(dir, sizeof(dir), "anchored"));

    uint8_t seed_live[32], seed_dead[32], seed_loose[32];
    idc_mk_key(seed_live, 0x01);
    idc_mk_key(seed_dead, 0x02);
    idc_mk_key(seed_loose, 0x03);

    char doc_live[ZID_DOC_MAX * 2 + 2];
    char doc_dead[ZID_DOC_MAX * 2 + 2];
    char doc_loose[ZID_DOC_MAX * 2 + 2];
    uint8_t pub_live[32], pub_dead[32], pub_loose[32];
    IDC_CHECK("anchored fixture: sign live doc",
              idc_sign_release(seed_live, "demo", "1.0", doc_live,
                               sizeof(doc_live), pub_live));
    IDC_CHECK("anchored fixture: sign revoked-key doc",
              idc_sign_release(seed_dead, "demo", "0.9", doc_dead,
                               sizeof(doc_dead), pub_dead));
    IDC_CHECK("anchored fixture: sign unanchored-key doc",
              idc_sign_release(seed_loose, "demo", "0.1", doc_loose,
                               sizeof(doc_loose), pub_loose));

    struct zid_identity row;
    idc_fill(&row, pub_live, 2222, ZID_IDENTITY_STATUS_ACTIVE,
             ZID_IDENTITY_SOURCE_ZNAM_TEXT, "demo-publisher", "t1owner");
    IDC_CHECK("anchored fixture: anchored publisher key",
              idc_save_row(dir, &row));
    idc_fill(&row, pub_dead, 1111, ZID_IDENTITY_STATUS_REVOKED,
             ZID_IDENTITY_SOURCE_ZID_OVERLAY, NULL, "t1owner");
    IDC_CHECK("anchored fixture: revoked publisher key",
              idc_save_row(dir, &row));

    struct json_value input;
    struct zcl_command_reply reply;

    /* the honest happy path: valid signature AND an anchored, live key */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "doc", doc_live);
    (void)json_push_kv_bool(&input, "anchored", true);
    idc_call(zcl_native_handle_zcode_release_verify, &input, &reply,
             "zcl.zcode_release_verify.v1");
    IDC_CHECK("verify --anchored: exit OK",
              reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("verify --anchored: signature still reported valid",
              json_get_bool(json_get(&reply.data, "valid")));
    IDC_CHECK("verify --anchored: anchored true",
              json_get_bool(json_get(&reply.data, "anchored")));
    IDC_CHECK("verify --anchored: anchor_height",
              json_get_int(json_get(&reply.data, "anchor_height")) == 2222);
    IDC_CHECK("verify --anchored: anchor_name",
              strcmp(idc_str(&reply, "anchor_name"), "demo-publisher") == 0);
    IDC_CHECK("verify --anchored: anchor_status active",
              strcmp(idc_str(&reply, "anchor_status"), "active") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* without --anchored the handler is unchanged: no anchor fact at all */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "doc", doc_loose);
    idc_call(zcl_native_handle_zcode_release_verify, &input, &reply,
             "zcl.zcode_release_verify.v1");
    IDC_CHECK("verify without --anchored: still passes",
              reply.exit_code == ZCL_COMMAND_EXIT_OK);
    IDC_CHECK("verify without --anchored: reports no anchor fact",
              json_get(&reply.data, "anchored") == NULL);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* a valid signature by a key nobody anchored is NOT trust */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "doc", doc_loose);
    (void)json_push_kv_bool(&input, "anchored", true);
    idc_call(zcl_native_handle_zcode_release_verify, &input, &reply,
             "zcl.zcode_release_verify.v1");
    IDC_CHECK("verify --anchored unanchored key: KEY_NOT_ANCHORED",
              strcmp(reply.error.code, "KEY_NOT_ANCHORED") == 0);
    IDC_CHECK("verify --anchored unanchored key: signature was still valid",
              json_get_bool(json_get(&reply.data, "valid")));
    IDC_CHECK("verify --anchored unanchored key: anchored false",
              !json_get_bool(json_get(&reply.data, "anchored")));
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* a valid signature by a revoked key is NOT trust either */
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "doc", doc_dead);
    (void)json_push_kv_bool(&input, "anchored", true);
    idc_call(zcl_native_handle_zcode_release_verify, &input, &reply,
             "zcl.zcode_release_verify.v1");
    IDC_CHECK("verify --anchored revoked key: KEY_REVOKED",
              strcmp(reply.error.code, "KEY_REVOKED") == 0);
    IDC_CHECK("verify --anchored revoked key: anchored is still true",
              json_get_bool(json_get(&reply.data, "anchored")));
    IDC_CHECK("verify --anchored revoked key: anchor_status revoked",
              strcmp(idc_str(&reply, "anchor_status"), "revoked") == 0);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* THE separation: an anchored key does not rescue a tampered doc */
    char tampered[ZID_DOC_MAX * 2 + 2];
    snprintf(tampered, sizeof(tampered), "%s", doc_live);
    size_t tlen = strlen(tampered);
    /* flip one nibble deep inside the signed body, not in the signature */
    size_t at = tlen / 2;
    tampered[at] = (tampered[at] == 'a') ? 'b' : 'a';
    idc_input_open(&input, dir);
    (void)json_push_kv_str(&input, "doc", tampered);
    (void)json_push_kv_bool(&input, "anchored", true);
    idc_call(zcl_native_handle_zcode_release_verify, &input, &reply,
             "zcl.zcode_release_verify.v1");
    IDC_CHECK("verify --anchored tampered doc: still BAD_SIGNATURE",
              strcmp(reply.error.code, "BAD_SIGNATURE") == 0);
    IDC_CHECK("verify --anchored tampered doc: no anchor fact reported",
              json_get(&reply.data, "anchored") == NULL);
    zcl_command_reply_free(&reply);
    json_free(&input);

    /* --anchored with no datadir names its own refusal */
    idc_input_open(&input, NULL);
    (void)json_push_kv_str(&input, "doc", doc_live);
    (void)json_push_kv_bool(&input, "anchored", true);
    idc_call(zcl_native_handle_zcode_release_verify, &input, &reply,
             "zcl.zcode_release_verify.v1");
    IDC_CHECK("verify --anchored no datadir: named error, no crash",
              reply.status != ZCL_COMMAND_STATUS_PASSED &&
              reply.error.code[0] != '\0');
    zcl_command_reply_free(&reply);
    json_free(&input);

    return failures;
}

int test_identity_command(void)
{
    printf("\n=== core identity command tests ===\n");
    int failures = 0;

    node_rpc_client_set_test_hook(idc_rpc_hook);

    failures += t_resolve();
    failures += t_list();
    failures += t_write_offline();
    failures += t_write_refusals();
    failures += t_verify_anchored();

    node_rpc_client_set_test_hook(NULL);
    return failures;
}
