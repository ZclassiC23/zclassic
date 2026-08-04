/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: deterministic typed composition and inspection for generic ZANC
 * digest anchors. Funding, signing, and broadcast remain separate owner-only
 * raw-transaction commands; this file never reads a wallet or moves funds. */

#include "command/native_command.h"

#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "script/standard.h"
#include "zanc/zanc.h"

#include <string.h>

#define ZCL_CORE_ANCHOR_COMPOSE_SCHEMA "zcl.core_anchor_compose.v1"
#define ZCL_CORE_ANCHOR_INSPECT_SCHEMA "zcl.core_anchor_inspect.v1"

static bool anchor_digest_parse(const char *hex, uint8_t out[ZANC_DIGEST_LEN])
{
    return hex && strlen(hex) == ZANC_DIGEST_LEN * 2 && IsHex(hex) &&
           ParseHex(hex, out, ZANC_DIGEST_LEN) == ZANC_DIGEST_LEN;
}

static bool anchor_hash_type_parse(const char *name, uint8_t *out)
{
    if (!name || !name[0] || strcmp(name, "sha3") == 0 ||
        strcmp(name, "sha3-256") == 0) {
        *out = ZANC_HASH_SHA3_256;
        return true;
    }
    if (strcmp(name, "sha2") == 0 || strcmp(name, "sha2-256") == 0) {
        *out = ZANC_HASH_SHA2_256;
        return true;
    }
    return false;
}

static void anchor_fail(struct zcl_command_reply *reply, const char *code,
                        const char *message, const char *evidence)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "normalize", false,
                           false, message, evidence ? evidence : "");
}

void zcl_native_handle_core_anchor_compose(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *digest_hex = json_get_str(json_get(request->input, "digest"));
    const char *hash_name = json_get_str(json_get(request->input, "hash_type"));
    const char *label = json_get_str(json_get(request->input, "label"));
    uint8_t digest[ZANC_DIGEST_LEN];
    uint8_t hash_type = 0;
    if (!anchor_digest_parse(digest_hex, digest)) {
        anchor_fail(reply, "INVALID_DIGEST",
                    "digest must be exactly 64 hexadecimal characters",
                    "digest");
        return;
    }
    if (!anchor_hash_type_parse(hash_name, &hash_type)) {
        anchor_fail(reply, "INVALID_HASH_TYPE",
                    "hash_type must be sha2, sha2-256, sha3, or sha3-256",
                    hash_name);
        return;
    }
    if (label && !zanc_label_valid(label, strlen(label))) {
        anchor_fail(reply, "INVALID_LABEL",
                    "label must be at most 32 bytes of printable UTF-8",
                    "label");
        return;
    }

    uint8_t script[MAX_OP_RETURN_RELAY];
    const size_t script_len = zanc_build_anchor(
        script, sizeof(script), hash_type, digest, label);
    if (script_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "ANCHOR_BUILD_FAILED", "compose", false,
                               false,
                               "the canonical ZANC codec rejected the anchor",
                               "zanc_build_anchor");
        return;
    }

    char script_hex[MAX_OP_RETURN_RELAY * 2 + 1];
    HexStr(script, script_len, false, script_hex, sizeof(script_hex));
    (void)json_push_kv_str(&reply->data, "schema",
                           ZCL_CORE_ANCHOR_COMPOSE_SCHEMA);
    (void)json_push_kv_str(&reply->data, "hash_type",
                           zanc_hash_type_name(hash_type));
    (void)json_push_kv_str(&reply->data, "digest", digest_hex);
    (void)json_push_kv_str(&reply->data, "label", label ? label : "");
    (void)json_push_kv_str(&reply->data, "op_return_hex", script_hex);
    (void)json_push_kv_int(&reply->data, "op_return_size",
                           (int64_t)script_len);
    (void)json_push_kv_str(&reply->data, "next_command",
                           "core.wallet.transaction.raw.create");
    struct json_value next_input;
    json_init(&next_input);
    json_set_object(&next_input);
    (void)json_push_kv_str(&next_input, "op_return_hex", script_hex);
    (void)json_push_kv(&reply->data, "next_input_fragment", &next_input);
    json_free(&next_input);
    (void)json_push_kv_str(&reply->data, "authority_note",
        "composition_only_no_wallet_read_signing_or_broadcast_authority");
}

void zcl_native_handle_core_anchor_inspect(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *script_hex = json_get_str(
        json_get(request->input, "op_return_hex"));
    if (!script_hex || !script_hex[0] || !IsHex(script_hex) ||
        (strlen(script_hex) & 1U) != 0 ||
        strlen(script_hex) > MAX_OP_RETURN_RELAY * 2) {
        anchor_fail(reply, "INVALID_OP_RETURN_HEX",
                    "op_return_hex must be even hexadecimal within the 223-byte relay cap",
                    "op_return_hex");
        return;
    }
    uint8_t script[MAX_OP_RETURN_RELAY];
    const size_t script_len = ParseHex(script_hex, script, sizeof(script));
    struct zanc_message message;
    if (script_len == 0 || !zanc_parse(script, script_len, &message)) {
        anchor_fail(reply, "INVALID_ZANC_ANCHOR",
                    "op_return_hex is not one canonical ZANC anchor",
                    "zanc_parse");
        return;
    }

    char digest_hex[ZANC_DIGEST_LEN * 2 + 1];
    HexStr(message.digest, ZANC_DIGEST_LEN, false, digest_hex,
           sizeof(digest_hex));
    (void)json_push_kv_str(&reply->data, "schema",
                           ZCL_CORE_ANCHOR_INSPECT_SCHEMA);
    (void)json_push_kv_bool(&reply->data, "valid", true);
    (void)json_push_kv_int(&reply->data, "version", message.version);
    (void)json_push_kv_str(&reply->data, "hash_type",
                           zanc_hash_type_name(message.hash_type));
    (void)json_push_kv_str(&reply->data, "digest", digest_hex);
    (void)json_push_kv_str(&reply->data, "label", message.label);
    (void)json_push_kv_int(&reply->data, "op_return_size",
                           (int64_t)script_len);
    (void)json_push_kv_str(&reply->data, "authority_note",
                           "inspection_only_no_wallet_or_chain_mutation");
}
