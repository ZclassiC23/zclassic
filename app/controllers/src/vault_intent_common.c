/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: shared canonical parsing and binding for durable vault intents. */

#include "controllers/vault_intent_controller.h"

#include "base/serialize_le.h"
#include "core/amount.h"
#include "crypto/sha3.h"
#include "models/vault_intent.h"

#include <limits.h>
#include <string.h>

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
