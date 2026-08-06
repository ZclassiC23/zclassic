/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: typed, deterministic compose/inspect adapters for ZPAY memos. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "zid/zpay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *zpayc_input_str(const struct json_value *input,
                                   const char *key)
{
    const struct json_value *value = json_get(input, key);
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool zpayc_input_u64(const struct json_value *input, const char *key,
                            uint64_t *out)
{
    const struct json_value *value = json_get(input, key);
    if (!value || value->type != JSON_INT || json_get_int(value) < 0)
        return false;
    *out = (uint64_t)json_get_int(value);
    return true;
}

static bool zpayc_network(const char *name, uint8_t *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "mainnet") == 0) *out = ZPAY_NETWORK_MAINNET;
    else if (strcmp(name, "testnet") == 0) *out = ZPAY_NETWORK_TESTNET;
    else if (strcmp(name, "regtest") == 0) *out = ZPAY_NETWORK_REGTEST;
    else return false;
    return true;
}

static const char *zpayc_network_name(uint8_t network)
{
    if (network == ZPAY_NETWORK_MAINNET) return "mainnet";
    if (network == ZPAY_NETWORK_TESTNET) return "testnet";
    if (network == ZPAY_NETWORK_REGTEST) return "regtest";
    return "invalid";
}

static bool zpayc_message_type(const char *name, uint8_t *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "invoice") == 0) *out = ZPAY_MESSAGE_INVOICE;
    else if (strcmp(name, "payment") == 0) *out = ZPAY_MESSAGE_PAYMENT;
    else if (strcmp(name, "receipt") == 0) *out = ZPAY_MESSAGE_RECEIPT;
    else return false;
    return true;
}

static const char *zpayc_message_type_name(uint8_t message_type)
{
    if (message_type == ZPAY_MESSAGE_INVOICE) return "invoice";
    if (message_type == ZPAY_MESSAGE_PAYMENT) return "payment";
    if (message_type == ZPAY_MESSAGE_RECEIPT) return "receipt";
    return "invalid";
}

static const char *zpayc_auth_name(enum zpay_sender_authentication auth)
{
    if (auth == ZPAY_SENDER_ANONYMOUS) return "anonymous";
    if (auth == ZPAY_SENDER_ZID_VERIFIED) return "zid_verified";
    return "zid_invalid";
}

static void zpayc_fail(struct zcl_command_reply *reply, const char *code,
                       const char *field, const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "normalize", false,
                           false, detail, field ? field : "");
}

static bool zpayc_decode_field(const struct json_value *input, const char *key,
                               uint8_t *out, size_t out_len,
                               struct zcl_command_reply *reply)
{
    const char *hex = zpayc_input_str(input, key);
    if (hex && zcl_hex_decode(hex, out, out_len))
        return true;
    char detail[128];
    (void)snprintf(detail, sizeof(detail),
                   "%s must be exactly %zu hexadecimal characters", key,
                   out_len * 2);
    zpayc_fail(reply, "BAD_HEX_FIELD", key, detail);
    return false;
}

static void zpayc_push_hex(struct json_value *out, const char *key,
                           const uint8_t *value, size_t value_len)
{
    char hex[65];
    zcl_hex_encode(value, value_len, hex);
    (void)json_push_kv_str(out, key, hex);
}

void zcl_native_handle_zpay_compose(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    struct zpay_envelope envelope;
    memset(&envelope, 0, sizeof(envelope));
    const char *network = zpayc_input_str(request->input, "network");
    const char *message_type =
        zpayc_input_str(request->input, "message_type");
    const char *asset = zpayc_input_str(request->input, "asset");
    if (!zpayc_network(network, &envelope.network)) {
        zpayc_fail(reply, "BAD_NETWORK", "network",
                   "network must be mainnet, testnet, or regtest");
        return;
    }
    if (!zpayc_message_type(message_type, &envelope.message_type)) {
        zpayc_fail(reply, "BAD_MESSAGE_TYPE", "message_type",
                   "message_type must be invoice, payment, or receipt");
        return;
    }
    if (!zpayc_input_u64(request->input, "created_at", &envelope.created_at) ||
        !zpayc_input_u64(request->input, "expires_at", &envelope.expires_at)) {
        zpayc_fail(reply, "BAD_TIME", "created_at,expires_at",
                   "created_at and expires_at must be non-negative integers");
        return;
    }
    if (!asset || strlen(asset) == 0 || strlen(asset) > ZPAY_ASSET_MAX) {
        zpayc_fail(reply, "BAD_ASSET", "asset",
                   "asset must be 1..65 canonical ZPAY characters");
        return;
    }
    (void)snprintf(envelope.asset, sizeof(envelope.asset), "%s", asset);
    if (!zpayc_decode_field(request->input, "nonce", envelope.nonce,
                            sizeof(envelope.nonce), reply) ||
        !zpayc_decode_field(request->input, "request_id", envelope.request_id,
                            sizeof(envelope.request_id), reply) ||
        !zpayc_decode_field(request->input, "invoice_digest",
                            envelope.invoice_digest,
                            sizeof(envelope.invoice_digest), reply) ||
        !zpayc_decode_field(request->input, "amount_commitment",
                            envelope.amount_commitment,
                            sizeof(envelope.amount_commitment), reply))
        return;
    const char *reply_ref = zpayc_input_str(request->input, "reply_ref");
    if (reply_ref) {
        if (!zcl_hex_decode(reply_ref, envelope.reply_ref, 32)) {
            zpayc_fail(reply, "BAD_REPLY_REF", "reply_ref",
                       "reply_ref must be exactly 64 hexadecimal characters");
            return;
        }
        envelope.has_reply = true;
    }

    uint8_t memo[ZPAY_MEMO_LEN];
    if (!zpay_memo_encode(memo, &envelope, NULL)) {
        zpayc_fail(reply, "INVALID_ENVELOPE", "input",
                   "fields do not form a canonical ZPAY v1 envelope");
        return;
    }
    char memo_hex[ZPAY_MEMO_LEN * 2 + 1];
    zcl_hex_encode(memo, sizeof(memo), memo_hex);
    json_set_object(&reply->data);
    (void)json_push_kv_str(&reply->data, "schema", "zcl.app_zpay_memo.v1");
    (void)json_push_kv_str(&reply->data, "network", network);
    (void)json_push_kv_str(&reply->data, "message_type", message_type);
    (void)json_push_kv_str(&reply->data, "memo_hex", memo_hex);
    (void)json_push_kv_int(&reply->data, "memo_bytes", ZPAY_MEMO_LEN);
    (void)json_push_kv_bool(&reply->data, "has_reply", envelope.has_reply);
    (void)json_push_kv_str(&reply->data, "sender_authentication",
                           "anonymous");
    (void)json_push_kv_bool(&reply->data, "identity_signing_available", false);
    (void)json_push_kv_str(&reply->data, "commit_command",
                           "core.wallet.shielded.send");
    (void)json_push_kv_str(&reply->data, "commit_memo_field", "memo_hex");
    (void)json_push_kv_str(&reply->data, "inspect_command",
                           "app.payments.zpay.inspect");
}

void zcl_native_handle_zpay_inspect(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *memo_hex = zpayc_input_str(request->input, "memo_hex");
    const char *network = zpayc_input_str(request->input, "network");
    uint64_t now_unix = 0;
    uint8_t expected_network = 0;
    if (!memo_hex || strlen(memo_hex) != ZPAY_MEMO_LEN * 2 ||
        !zpayc_input_u64(request->input, "now_unix", &now_unix)) {
        zpayc_fail(reply, "BAD_INSPECTION_INPUT", "memo_hex,now_unix",
                   "memo_hex must be 1024 hex characters and now_unix an integer");
        return;
    }
    if (!zpayc_network(network, &expected_network)) {
        zpayc_fail(reply, "BAD_NETWORK", "network",
                   "network must be mainnet, testnet, or regtest");
        return;
    }
    uint8_t memo[ZPAY_MEMO_LEN];
    if (!zcl_hex_decode(memo_hex, memo, sizeof(memo))) {
        zpayc_fail(reply, "BAD_MEMO_HEX", "memo_hex",
                   "memo_hex must be exactly 1024 hexadecimal characters");
        return;
    }
    struct zpay_envelope envelope;
    if (!zpay_memo_decode(memo, &envelope)) {
        zpayc_fail(reply, "INVALID_ZPAY_MEMO", "memo_hex",
                   "memo is not a canonical ZPAY v1 envelope");
        return;
    }
    enum zpay_sender_authentication auth =
        zpay_memo_authenticate(memo, now_unix, &envelope);
    bool current = zpay_envelope_is_current(&envelope, expected_network,
                                             now_unix);
    json_set_object(&reply->data);
    (void)json_push_kv_str(&reply->data, "schema",
                           "zcl.app_zpay_envelope.v1");
    (void)json_push_kv_str(&reply->data, "network",
                           zpayc_network_name(envelope.network));
    (void)json_push_kv_str(&reply->data, "expected_network", network);
    (void)json_push_kv_str(&reply->data, "message_type",
                           zpayc_message_type_name(envelope.message_type));
    (void)json_push_kv_int(&reply->data, "created_at",
                           (int64_t)envelope.created_at);
    (void)json_push_kv_int(&reply->data, "expires_at",
                           (int64_t)envelope.expires_at);
    (void)json_push_kv_str(&reply->data, "asset", envelope.asset);
    zpayc_push_hex(&reply->data, "nonce", envelope.nonce,
                   sizeof(envelope.nonce));
    zpayc_push_hex(&reply->data, "request_id", envelope.request_id,
                   sizeof(envelope.request_id));
    zpayc_push_hex(&reply->data, "invoice_digest", envelope.invoice_digest,
                   sizeof(envelope.invoice_digest));
    zpayc_push_hex(&reply->data, "amount_commitment",
                   envelope.amount_commitment,
                   sizeof(envelope.amount_commitment));
    (void)json_push_kv_bool(&reply->data, "has_reply", envelope.has_reply);
    if (envelope.has_reply)
        zpayc_push_hex(&reply->data, "reply_ref", envelope.reply_ref,
                       sizeof(envelope.reply_ref));
    (void)json_push_kv_str(&reply->data, "sender_authentication",
                           zpayc_auth_name(auth));
    (void)json_push_kv_bool(&reply->data, "has_identity_doc",
                            envelope.has_identity_doc);
    if (envelope.has_identity_doc)
        zpayc_push_hex(&reply->data, "identity_pubkey",
                       envelope.identity_doc.master_pubkey,
                       sizeof(envelope.identity_doc.master_pubkey));
    (void)json_push_kv_bool(&reply->data, "current", current);
    (void)json_push_kv_bool(&reply->data, "network_matches",
                            envelope.network == expected_network);
    (void)json_push_kv_bool(&reply->data, "time_current",
                            envelope.created_at <= now_unix &&
                            now_unix < envelope.expires_at);
}
