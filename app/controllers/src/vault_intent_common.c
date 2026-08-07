/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: shared canonical parsing and binding for durable vault intents. */

#include "controllers/vault_intent_controller.h"

#include "base/serialize_le.h"
#include "chain/chain.h"
#include "controllers/wallet_helpers.h"
#include "core/amount.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "models/vault_intent.h"
#include "util/log_macros.h"
#include "validation/main_state.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static void vic_hex(const uint8_t in[32], char out[65])
{
    HexStr(in, 32, false, out, 65);
}

static void vic_chain_hash_hex(const uint8_t in[32], char out[65])
{
    struct uint256 hash;
    memcpy(hash.data, in, sizeof(hash.data));
    uint256_get_hex(&hash, out);
}

static void vic_amount_text(int64_t amount, char out[32])
{
    (void)snprintf(out, 32, "%lld.%08lld",
                   (long long)(amount / COIN),
                   (long long)(amount % COIN));
}

bool vault_intent_chain_confirmation(struct main_state *ms,
                                     const uint8_t block_hash[32],
                                     int32_t *height_out,
                                     int32_t *confirmations_out)
{
    if (!ms || !block_hash || !height_out || !confirmations_out)
        LOG_FAIL("vault_intent", "chain confirmation: NULL argument");

    struct uint256 hash;
    memcpy(hash.data, block_hash, sizeof(hash.data));
    bool found = false;
    int32_t height = -1;
    int32_t confirmations = 0;

    /* Wallet confirmation counters can lag tip advancement. Resolve the
     * recorded block hash against the canonical active chain instead. */
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = block_map_find(&ms->map_block_index, &hash);
    int tip = active_chain_height(&ms->chain_active);
    if (bi && bi->nHeight >= 0 && bi->nHeight <= tip &&
        active_chain_at(&ms->chain_active, bi->nHeight) == bi) {
        height = bi->nHeight;
        confirmations = tip - bi->nHeight + 1;
        found = confirmations > 0;
    }
    zcl_mutex_unlock(&ms->cs_main);

    if (!found)
        return false;
    *height_out = height;
    *confirmations_out = confirmations;
    return true;
}

/* Amounts are text only. This is deliberately not the permissive legacy RPC
 * amount parser: floats can never cross this boundary. */
bool vault_intent_parse_zcl_amount(const char *s, int64_t *out)
{
    if (!s || !out || !s[0] || s[0] == '-' || s[0] == '+')
        return false; // raw-return-ok:predicate
    int64_t whole = 0, frac = 0;
    unsigned decimals = 0;
    bool dot = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '.' && !dot) { dot = true; continue; }
        if (*p < '0' || *p > '9') return false; // raw-return-ok:predicate
        int digit = *p - '0';
        if (!dot) {
            if (whole > (INT64_MAX - digit) / 10)
                return false; // raw-return-ok:predicate
            whole = whole * 10 + digit;
        } else {
            if (++decimals > 8) return false; // raw-return-ok:predicate
            frac = frac * 10 + digit;
        }
    }
    if (dot && decimals == 0) return false; // raw-return-ok:predicate
    while (decimals++ < 8) frac *= 10;
    if (whole > (INT64_MAX - frac) / COIN)
        return false; // raw-return-ok:predicate
    int64_t amount = whole * COIN + frac;
    if (amount <= 0 || !MoneyRange(amount))
        return false; // raw-return-ok:predicate
    *out = amount;
    return true;
}

bool vault_intent_idempotency_key_valid(const char *key)
{
    if (!key || !key[0] || strlen(key) > VAULT_INTENT_IDEMPOTENCY_MAX)
        return false;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        if (*p < 0x20 || *p > 0x7e)
            return false;
    return true;
}

void vault_intent_digest_payload(const uint8_t *raw, size_t len,
                                 const struct vault_intent_row *row,
                                 uint8_t out[32])
{
    uint8_t height_le[4], expiry_le[8];
    zcl_write_i32_le(height_le, row->anchor_height);
    zcl_write_i64_le(expiry_le, row->expires_at);
    struct sha3_256_ctx c;
    sha3_256_init(&c);
    sha3_256_write(&c, (const uint8_t *)"vault-intent-v1", 15);
    sha3_256_write(&c, raw, len);
    sha3_256_write(&c, height_le, sizeof(height_le));
    sha3_256_write(&c, row->anchor_hash, 32);
    sha3_256_write(&c, expiry_le, sizeof(expiry_le));
    sha3_256_write(&c, (const uint8_t *)row->wallet_scope,
                   strlen(row->wallet_scope));
    sha3_256_write(&c, (const uint8_t *)row->wallet_instance_id,
                   strlen(row->wallet_instance_id));
    sha3_256_write(&c, (const uint8_t *)row->wallet_genesis,
                   strlen(row->wallet_genesis));
    sha3_256_write(&c, row->snapshot_root, 32);
    uint8_t money[2][8];
    zcl_write_i64_le(money[0], row->recipient_value_zat);
    zcl_write_i64_le(money[1], row->max_fee_zat);
    sha3_256_write(&c, (const uint8_t *)money, sizeof(money));
    sha3_256_finalize(&c, out);
}

void vault_intent_render_row(struct wallet_rpc_context *ctx,
                             struct json_value *out,
                             const struct vault_intent_row *row)
{
    char id[65]; vic_hex(row->plan_id, id);
    (void)json_push_kv_str(out, "plan_id", id);
    (void)json_push_kv_str(out, "state",
                           vault_intent_state_name(row->state));
    (void)json_push_kv_int(out, "created_at", row->created_at);
    (void)json_push_kv_int(out, "expires_at", row->expires_at);
    if (row->wallet_scope[0]) {
        char root[65], recipient[32], fee[32], reserved[32];
        vic_hex(row->snapshot_root, root);
        vic_amount_text(row->recipient_value_zat, recipient);
        vic_amount_text(row->max_fee_zat, fee);
        vic_amount_text(row->reserved_zat, reserved);
        (void)json_push_kv_str(out, "wallet_scope", row->wallet_scope);
        (void)json_push_kv_str(out, "wallet_instance_id",
                               row->wallet_instance_id);
        (void)json_push_kv_str(out, "network_genesis", row->wallet_genesis);
        (void)json_push_kv_str(out, "money_snapshot_root", root);
        (void)json_push_kv_str(out, "recipient_value", recipient);
        (void)json_push_kv_str(out, "maximum_fee", fee);
        (void)json_push_kv_str(out, "reserved", reserved);
    }
    if (row->has_txid) {
        char txid[65]; vic_chain_hash_hex(row->txid, txid);
        (void)json_push_kv_str(out, "txid", txid);
    } else {
        struct json_value none; json_init(&none); json_set_null(&none);
        (void)json_push_kv(out, "txid", &none); json_free(&none);
    }
    const bool confirmation_known =
        (row->state == VAULT_INTENT_CONFIRMED ||
         row->state == VAULT_INTENT_FINALIZED ||
         row->state == VAULT_INTENT_REORGED) &&
        row->confirm_height >= 0;
    int64_t confirmations = 0;
    if (ctx && ctx->main_state && confirmation_known &&
        row->state != VAULT_INTENT_REORGED) {
        int tip = active_chain_height(&ctx->main_state->chain_active);
        if (tip >= row->confirm_height)
            confirmations = (int64_t)tip - row->confirm_height + 1;
    }
    (void)json_push_kv_int(out, "confirmations", confirmations);
    if (confirmation_known) {
        (void)json_push_kv_int(out, "confirmed_height", row->confirm_height);
        if (row->has_confirm_hash) {
            char block_hash[65];
            vic_chain_hash_hex(row->confirm_hash, block_hash);
            (void)json_push_kv_str(out, "confirmed_block_hash", block_hash);
        }
    }
    if (row->error_code[0])
        (void)json_push_kv_str(out, "error_code", row->error_code);
}
