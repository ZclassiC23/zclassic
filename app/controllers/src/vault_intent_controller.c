/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: durable exact-input planning and idempotent transparent commits. */

#include "controllers/vault_intent_controller.h"
#include "controllers/native_handler_body.h"
#include "controllers/sovereignty_controller.h"
#include "controllers/strong_params.h"
#include "controllers/sync_controller.h"
#include "controllers/wallet_helpers.h"
#include "base/serialize_le.h"
#include "chain/chain.h"
#include "core/serialize.h"
#include "core/amount.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "models/database.h"
#include "models/vault_intent.h"
#include "models/wallet_metadata_crypto.h"
#include "net/connman.h"
#include "platform/time_compat.h"
#include "primitives/transaction.h"
#include "rpc/server.h"
#include "services/wallet_backup_service.h"
#include "support/cleanse.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "wallet/wallet.h"
#include "wallet/wallet_lock.h"
#include "wallet/wallet_sqlite.h"

#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VI_EFFECTS_MAX 50
#define VI_INPUTS_MAX 128
#define VI_ADDR_MAX 127
#define VI_TTL 600
#define VI_PROVING_LEASE 300

struct vi_effect { char to[VI_ADDR_MAX + 1]; int64_t amount; };
struct vi_input { uint8_t txid[32]; uint32_t vout; };
struct vi_payload {
    struct vi_effect effects[VI_EFFECTS_MAX];
    struct vi_input inputs[VI_INPUTS_MAX];
    size_t effects_len, inputs_len;
    int64_t fee;
};

static void vi_error(struct json_value *out, const char *code, const char *msg)
{
    json_set_object(out);
    json_push_kv_bool(out, "ok", false);
    json_push_kv_str(out, "code", code);
    json_push_kv_str(out, "message", msg);
}

static void vi_hex(const uint8_t in[32], char out[65])
{
    HexStr(in, 32, false, out, 65);
}

static bool vi_unhex(const char *s, uint8_t out[32])
{
    return s && strlen(s) == 64 && IsHex(s) && ParseHex(s, out, 32) == 32;
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

static void vi_amount_text(int64_t amount, char out[32])
{
    snprintf(out, 32, "%lld.%08lld", (long long)(amount / 100000000LL),
             (long long)(amount % 100000000LL));
}

static bool vi_encode(const struct vi_payload *p, uint8_t *out, size_t cap,
                      size_t *out_len)
{
    struct byte_stream s;
    stream_init(&s, 1024);
    bool ok = stream_write(&s, "VIP1", 4) &&
        stream_write_u8(&s, VAULT_INTENT_ROUTE_TRANSPARENT) &&
        stream_write_u8(&s, (uint8_t)p->effects_len) &&
        stream_write_u16_le(&s, (uint16_t)p->inputs_len) &&
        stream_write_i64_le(&s, p->fee);
    for (size_t i = 0; ok && i < p->effects_len; i++) {
        size_t n = strlen(p->effects[i].to);
        ok = n > 0 && n <= VI_ADDR_MAX && stream_write_u8(&s, (uint8_t)n) &&
             stream_write(&s, p->effects[i].to, n) &&
             stream_write_i64_le(&s, p->effects[i].amount);
    }
    for (size_t i = 0; ok && i < p->inputs_len; i++)
        ok = stream_write(&s, p->inputs[i].txid, 32) &&
             stream_write_u32_le(&s, p->inputs[i].vout);
    if (!ok || s.size > cap) {
        stream_free(&s);
        LOG_FAIL("vault_intent", "canonical payload encode failed");
    }
    memcpy(out, s.data, s.size);
    *out_len = s.size;
    stream_free(&s);
    return true;
}

static bool vi_decode(const uint8_t *raw, size_t len, struct vi_payload *p)
{
    if (!raw || !p) LOG_FAIL("vault_intent", "decode: NULL argument");
    memset(p, 0, sizeof(*p));
    struct byte_stream s;
    stream_init_from_data(&s, raw, len);
    uint8_t magic[4], route = 0, ne = 0;
    uint16_t ni = 0;
    bool ok = stream_read(&s, magic, 4) && memcmp(magic, "VIP1", 4) == 0 &&
        stream_read_u8(&s, &route) && route == VAULT_INTENT_ROUTE_TRANSPARENT &&
        stream_read_u8(&s, &ne) && ne > 0 && ne <= VI_EFFECTS_MAX &&
        stream_read_u16_le(&s, &ni) && ni > 0 && ni <= VI_INPUTS_MAX &&
        stream_read_i64_le(&s, &p->fee);
    p->effects_len = ne; p->inputs_len = ni;
    for (size_t i = 0; ok && i < p->effects_len; i++) {
        uint8_t n = 0;
        ok = stream_read_u8(&s, &n) && n > 0 && n <= VI_ADDR_MAX &&
             stream_read(&s, p->effects[i].to, n) &&
             stream_read_i64_le(&s, &p->effects[i].amount);
        p->effects[i].to[n] = '\0';
    }
    for (size_t i = 0; ok && i < p->inputs_len; i++)
        ok = stream_read(&s, p->inputs[i].txid, 32) &&
             stream_read_u32_le(&s, &p->inputs[i].vout);
    ok = ok && stream_remaining(&s) == 0 && p->fee >= 0;
    stream_free(&s);
    if (!ok) LOG_FAIL("vault_intent", "canonical payload decode failed");
    return true;
}

static void vi_digest(const uint8_t *raw, size_t len, int32_t height,
                      const uint8_t hash[32], int64_t expiry, uint8_t out[32])
{
    uint8_t height_le[4], expiry_le[8];
    zcl_write_i32_le(height_le, height);
    zcl_write_i64_le(expiry_le, expiry);
    struct sha3_256_ctx c;
    sha3_256_init(&c);
    sha3_256_write(&c, (const uint8_t *)"vault-intent-v1", 15);
    sha3_256_write(&c, raw, len);
    sha3_256_write(&c, height_le, sizeof(height_le));
    sha3_256_write(&c, hash, 32);
    sha3_256_write(&c, expiry_le, sizeof(expiry_le));
    sha3_256_finalize(&c, out);
}

static bool vi_ready(struct wallet_rpc_context *ctx, struct json_value *out)
{
    if (!ctx || !ctx->wallet || !ctx->wallet_db || !ctx->node_db ||
        !ctx->mempool || !ctx->coins_tip || !ctx->main_state) {
        vi_error(out, "WALLET_UNAVAILABLE", "wallet persistence context is incomplete");
        return false;
    }
    if (!wallet_lock_encrypted_at_rest()) {
        vi_error(out, "WALLET_NOT_ENCRYPTED", "encrypt wallet keys and metadata before planning");
        return false;
    }
    if (!wallet_lock_is_unlocked()) {
        vi_error(out, "WALLET_LOCKED", "unlock the wallet through stdin before planning");
        return false;
    }
    struct wallet_sqlite_health h = wallet_sqlite_get_health(
        ctx->wallet_db, (int)ctx->wallet->keystore.num_keys);
    if (!h.open || !h.canary_ok || h.mismatch) {
        vi_error(out, "WALLET_PERSISTENCE_UNHEALTHY", "wallet persistence health gate failed");
        return false;
    }
    struct wallet_backup_status backup;
    wallet_backup_status_snapshot(&backup);
    int64_t now = (int64_t)platform_time_wall_time_t();
    size_t path_len = strlen(backup.last_path);
    bool encrypted_backup = path_len > 4 &&
        strcmp(backup.last_path + path_len - 4, ".enc") == 0;
    if (backup.last_run_unix <= 0 || now < backup.last_run_unix ||
        now - backup.last_run_unix > 86400 || !encrypted_backup ||
        backup.last_tables_verified != backup.wallet_table_count) {
        vi_error(out, "ENCRYPTED_BACKUP_REQUIRED",
                 "a current-key encrypted, fully verified wallet backup under 24 hours old is required");
        return false;
    }
    char why[96] = {0};
    if (!sovereignty_guard_allow("wallet_spend", why, sizeof(why))) {
        vi_error(out, "SOVEREIGNTY_GATE", why);
        return false;
    }
    return true;
}

static bool vi_effects(const struct json_value *input, struct vi_payload *p,
                       struct json_value *out)
{
    const char *route = json_get_str(json_get(input, "route"));
    if (!route) route = "private";
    if (strcmp(route, "transparent") != 0) {
        vi_error(out, strcmp(route, "private") == 0 ?
            "PRIVACY_ROUTE_UNAVAILABLE" : "UNSUPPORTED_ROUTE",
            "privacy is never downgraded; this build currently commits only explicit transparent plans");
        return false;
    }
    const struct json_value *effects = json_get(input, "effects");
    size_t effect_count = effects ? json_size(effects) : 0;
    if (!effects || effects->type != JSON_ARR || effect_count == 0 ||
        effect_count > VI_EFFECTS_MAX) {
        vi_error(out, "INVALID_EFFECTS", "effects must contain 1..50 entries");
        return false;
    }
    int64_t total = 0;
    p->effects_len = effect_count;
    for (size_t i = 0; i < effect_count; i++) {
        const struct json_value *e = json_at(effects, i);
        const char *asset = e ? json_get_str(json_get(e, "asset")) : NULL;
        const char *to = e ? json_get_str(json_get(e, "to")) : NULL;
        int64_t amount = 0;
        struct tx_destination dest;
        if (!e || e->type != JSON_OBJ || !asset || strcmp(asset, "ZCL") ||
            !to || strlen(to) > VI_ADDR_MAX ||
            !json_get(e, "amount") || json_get(e, "amount")->type != JSON_STR ||
            !vault_intent_parse_zcl_amount(
                json_get_str(json_get(e, "amount")), &amount) ||
            !wallet_decode_address(to, &dest) || // raw-return-ok:invalid recipient is reported below
            total > INT64_MAX - amount) {
            vi_error(out, "INVALID_EFFECT", "each effect needs asset=ZCL, a transparent address, and a decimal-string amount");
            return false; // raw-return-ok:invalid_effect_reported
        }
        snprintf(p->effects[i].to, sizeof(p->effects[i].to), "%s", to);
        p->effects[i].amount = amount;
        total += amount;
    }
    return true;
}

static void vi_render_row(struct wallet_rpc_context *ctx,
                          struct json_value *out,
                          const struct vault_intent_row *row)
{
    char id[65]; vi_hex(row->plan_id, id);
    json_push_kv_str(out, "plan_id", id);
    json_push_kv_str(out, "state", vault_intent_state_name(row->state));
    json_push_kv_int(out, "created_at", row->created_at);
    json_push_kv_int(out, "expires_at", row->expires_at);
    if (row->has_txid) {
        char txid[65]; vi_hex(row->txid, txid);
        json_push_kv_str(out, "txid", txid);
    } else {
        struct json_value none; json_init(&none); json_set_null(&none);
        json_push_kv(out, "txid", &none); json_free(&none);
    }
    int64_t confirmations = 0;
    if (ctx && ctx->main_state && row->confirm_height >= 0 &&
        row->state != VAULT_INTENT_REORGED) {
        int tip = active_chain_height(&ctx->main_state->chain_active);
        if (tip >= row->confirm_height)
            confirmations = (int64_t)tip - row->confirm_height + 1;
    }
    json_push_kv_int(out, "confirmations", confirmations);
    if (row->confirm_height >= 0) {
        json_push_kv_int(out, "confirmed_height", row->confirm_height);
        if (row->has_confirm_hash) {
            char block_hash[65]; vi_hex(row->confirm_hash, block_hash);
            json_push_kv_str(out, "confirmed_block_hash", block_hash);
        }
    }
    if (row->error_code[0]) json_push_kv_str(out, "error_code", row->error_code);
}

static void vi_refresh_state(struct wallet_rpc_context *ctx,
                             struct vault_intent_row *row, int64_t now)
{
    if (!ctx || !row || !row->has_txid)
        return;
    struct uint256 txid;
    memcpy(txid.data, row->txid, 32);
    const struct wallet_tx *wtx = wallet_get_tx(ctx->wallet, &txid);
    if (wtx && wtx->confirms > 0 && !uint256_is_null(&wtx->hash_block)) {
        int tip = active_chain_height(&ctx->main_state->chain_active);
        int32_t height = tip - wtx->confirms + 1;
        enum vault_intent_state state = wtx->confirms >= 6
            ? VAULT_INTENT_FINALIZED : VAULT_INTENT_CONFIRMED;
        if (vault_intent_set_confirmation(ctx->node_db, row->plan_id, state,
                height, wtx->hash_block.data, now))
            (void)vault_intent_find(ctx->node_db, row->plan_id, row);
    } else if (row->state == VAULT_INTENT_CONFIRMED ||
               row->state == VAULT_INTENT_FINALIZED) {
        if (vault_intent_set_state(ctx->node_db, row->plan_id,
                VAULT_INTENT_REORGED, row->txid, "CONFIRMATION_REORGED", now))
            (void)vault_intent_find(ctx->node_db, row->plan_id, row);
    }
}

static bool rpc_vi_plan(const struct json_value *params, bool help,
                        struct json_value *result)
{
    RPC_HELP(help, result, "vault_intent_plan {route,effects}\n");
    const struct json_value *input = json_at(params, 0);
    struct wallet_rpc_context *ctx = wallet_rpc_context_current();
    if (!input || input->type != JSON_OBJ) {
        vi_error(result, "INVALID_INPUT", "one intent object is required");
        return true;
    }
    if (!vi_ready(ctx, result)) return true;
    struct vi_payload p; memset(&p, 0, sizeof(p));
    if (!vi_effects(input, &p, result)) return true;
    int64_t target = 0;
    for (size_t i = 0; i < p.effects_len; i++) target += p.effects[i].amount;
    p.fee = wallet_default_fee(ctx->wallet);
    if (p.fee < 0 || target > INT64_MAX - p.fee) {
        vi_error(result, "FEE_INVALID", "wallet fee is invalid"); return true;
    }
    struct coin_entry *avail = zcl_malloc(4096 * sizeof(*avail),
                                           "intent_available_coins");
    struct coin_entry chosen[VI_INPUTS_MAX];
    if (!avail) { vi_error(result, "OUT_OF_MEMORY", "coin inventory allocation failed"); return true; }
    size_t na = 0, nc = 0; int64_t value = 0;
    wallet_available_coins(ctx->wallet, avail, &na, 4096, true, false);
    bool funded = wallet_select_coins(ctx->wallet, avail, na, target + p.fee,
                                      chosen, &nc, VI_INPUTS_MAX, &value);
    if (!funded || nc == 0 || nc > VI_INPUTS_MAX) {
        free(avail);
        vi_error(result, "INSUFFICIENT_CONFIRMED_FUNDS", "confirmed transparent funds cannot satisfy effects and fee");
        return true;
    }
    p.inputs_len = nc;
    for (size_t i = 0; i < nc; i++) {
        memcpy(p.inputs[i].txid, chosen[i].wtx->tx.hash.data, 32);
        p.inputs[i].vout = (uint32_t)chosen[i].i;
    }
    free(avail);
    struct block_index *anchor = active_chain_tip(&ctx->main_state->chain_active);
    if (!anchor) { vi_error(result, "CHAIN_ANCHOR_UNAVAILABLE", "active chain has no tip"); return true; }
    uint8_t plain[WALLET_METADATA_PLAINTEXT_MAX]; size_t plen = 0;
    if (!vi_encode(&p, plain, sizeof(plain), &plen)) {
        vi_error(result, "PLAN_ENCODING_FAILED", "could not encode exact plan"); return true;
    }
    struct vault_intent_row row; memset(&row, 0, sizeof(row));
    if (RAND_bytes(row.plan_id, 32) != 1) {
        memory_cleanse(plain, sizeof(plain));
        vi_error(result, "RNG_FAILED", "could not mint plan id"); return true;
    }
    row.state = VAULT_INTENT_PLANNED;
    row.route = VAULT_INTENT_ROUTE_TRANSPARENT;
    row.created_at = (int64_t)platform_time_wall_time_t();
    row.expires_at = row.created_at + VI_TTL;
    row.updated_at = row.created_at;
    row.anchor_height = anchor->nHeight;
    memcpy(row.anchor_hash, anchor->hashBlock.data, 32);
    vi_digest(plain, plen, row.anchor_height, row.anchor_hash, row.expires_at,
              row.digest);
    bool stored = wallet_metadata_encrypt(ctx->node_db, row.plan_id, 32,
        plain, plen, row.encrypted_payload, sizeof(row.encrypted_payload),
        &row.encrypted_payload_len) && vault_intent_save(ctx->node_db, &row);
    memory_cleanse(plain, sizeof(plain));
    if (!stored) { vi_error(result, "PLAN_PERSIST_FAILED", "encrypted plan could not be persisted"); return true; }
    json_set_object(result); json_push_kv_bool(result, "ok", true);
    vi_render_row(ctx, result, &row);
    char digest[65], fee[32]; vi_hex(row.digest, digest); vi_amount_text(p.fee, fee);
    json_push_kv_str(result, "digest", digest);
    json_push_kv_str(result, "fee", fee);
    json_push_kv_int(result, "confirmation_policy", 6);
    json_push_kv_str(result, "route", "transparent");
    json_push_kv_str(result, "privacy", "PUBLIC: recipients, values, inputs, change, and transaction graph are visible");
    struct json_value effects; json_init(&effects); json_set_array(&effects);
    for (size_t i = 0; i < p.effects_len; i++) {
        struct json_value e; json_init(&e); json_set_object(&e);
        char amount[32]; vi_amount_text(p.effects[i].amount, amount);
        json_push_kv_str(&e, "asset", "ZCL");
        json_push_kv_str(&e, "to", p.effects[i].to);
        json_push_kv_str(&e, "amount", amount);
        json_push_back(&effects, &e); json_free(&e);
    }
    json_push_kv(result, "effects", &effects); json_free(&effects);
    return true;
}

static bool vi_anchor_ok(struct wallet_rpc_context *ctx,
                         const struct vault_intent_row *row)
{
    struct block_index *bi = active_chain_at(&ctx->main_state->chain_active,
                                              row->anchor_height);
    return bi && memcmp(bi->hashBlock.data, row->anchor_hash, 32) == 0;
}

static bool vi_publish(struct wallet_rpc_context *ctx, const uint8_t id[32],
                       struct wallet_tx *wtx, int64_t now,
                       struct json_value *result)
{
    if (!wallet_get_tx(ctx->wallet, &wtx->tx.hash)) {
        struct zcl_result r = wallet_commit_from_context(ctx, wtx);
        if (!r.ok) {
            vault_intent_set_state(ctx->node_db, id, VAULT_INTENT_FAILED, NULL,
                                   "MEMPOOL_REJECTED", now);
            vi_error(result, "MEMPOOL_REJECTED", r.message); return false;
        }
        r = wallet_persist_commit_before_relay(ctx, wtx);
        if (!r.ok) {
            vault_intent_set_state(ctx->node_db, id, VAULT_INTENT_FAILED, NULL,
                                   "PERSISTENCE_FAILED", now);
            vi_error(result, "PERSISTENCE_FAILED", r.message); return false;
        }
        if (wallet_ctx_db_ready(ctx))
            node_db_sync_wallet_tx(ctx->node_db, &wtx->tx, ctx->wallet, 0);
    }
    if (ctx->connman) connman_relay_transaction(ctx->connman, &wtx->tx.hash);
    if (!vault_intent_set_state(ctx->node_db, id,
            VAULT_INTENT_MEMPOOL_ACCEPTED, wtx->tx.hash.data, "", now)) {
        vi_error(result, "INTENT_STATE_FAILED", "transaction is durable but intent state update failed");
        return false;
    }
    return true;
}

static bool vi_build_prepared(struct wallet_rpc_context *ctx,
                              const struct vault_intent_row *row,
                              struct wallet_tx *wtx, struct json_value *result)
{
    uint8_t plain[WALLET_METADATA_PLAINTEXT_MAX]; size_t plen = 0;
    if (!wallet_metadata_decrypt(ctx->node_db, row->plan_id, 32,
            row->encrypted_payload, row->encrypted_payload_len,
            plain, sizeof(plain), &plen)) {
        vi_error(result, "PLAN_DECRYPT_FAILED", "encrypted plan failed authentication");
        return false;
    }
    uint8_t digest[32];
    vi_digest(plain, plen, row->anchor_height, row->anchor_hash,
              row->expires_at, digest);
    struct vi_payload p;
    bool valid = memcmp(digest, row->digest, 32) == 0 && vi_decode(plain, plen, &p);
    memory_cleanse(plain, sizeof(plain));
    if (!valid) { vi_error(result, "PLAN_TAMPERED", "plan digest or payload changed"); return false; }
    struct coin_entry *avail = zcl_malloc(4096 * sizeof(*avail),
                                           "intent_commit_coins");
    struct coin_entry selected[VI_INPUTS_MAX];
    struct tx_out outputs[VI_EFFECTS_MAX];
    if (!avail) { vi_error(result, "OUT_OF_MEMORY", "commit inventory allocation failed"); return false; }
    size_t na = 0;
    wallet_available_coins(ctx->wallet, avail, &na, 4096, true, false);
    bool found_all = true;
    for (size_t i = 0; i < p.inputs_len; i++) {
        bool found = false;
        for (size_t j = 0; j < na; j++) {
            if (avail[j].i == p.inputs[i].vout &&
                memcmp(avail[j].wtx->tx.hash.data, p.inputs[i].txid, 32) == 0) {
                selected[i] = avail[j]; found = true; break;
            }
        }
        if (!found) { found_all = false; break; }
    }
    free(avail);
    if (!found_all) { vi_error(result, "INPUT_CONFLICT", "an exact input is no longer confirmed and spendable"); return false; }
    for (size_t i = 0; i < p.effects_len; i++) {
        struct tx_destination dest; memset(&outputs[i], 0, sizeof(outputs[i]));
        if (!wallet_decode_address(p.effects[i].to, &dest)) {
            vi_error(result, "RECIPIENT_REVALIDATION_FAILED", "planned recipient no longer decodes");
            return false; // raw-return-ok:recipient_revalidation_reported
        }
        outputs[i].value = p.effects[i].amount;
        script_for_destination(&outputs[i].script_pub_key, &dest);
    }
    int64_t fee = 0; const char *why = NULL;
    if (!wallet_create_transaction_selected(ctx->wallet, selected, p.inputs_len,
            outputs, p.effects_len, wtx, &fee, &why) || fee != p.fee) {
        vi_error(result, "EXACT_BUILD_FAILED", why ? why : "fee changed since planning"); return false;
    }
    struct zcl_result flushed = wallet_flush_from_context(ctx);
    if (!flushed.ok) {
        transaction_free(&wtx->tx);
        vi_error(result, "CHANGE_KEY_PERSIST_FAILED", flushed.message); return false;
    }
    struct byte_stream s; stream_init(&s, 1024);
    bool saved = transaction_serialize(&wtx->tx, &s) &&
        s.size <= VAULT_INTENT_RAW_MAX &&
        vault_intent_store_raw(ctx->node_db, row->plan_id, s.data, s.size) &&
        vault_intent_set_state(ctx->node_db, row->plan_id,
            VAULT_INTENT_PROVING, wtx->tx.hash.data, "",
            (int64_t)platform_time_wall_time_t());
    stream_free(&s);
    if (!saved) {
        transaction_free(&wtx->tx);
        vi_error(result, "PRE_RELAY_DURABILITY_FAILED", "signed transaction could not be recorded"); return false;
    }
    return true;
}

static bool rpc_vi_commit(const struct json_value *params, bool help,
                          struct json_value *result)
{
    RPC_HELP(help, result, "vault_intent_commit {plan_id,confirm:true}\n");
    const struct json_value *in = json_at(params, 0);
    const char *hex = in ? json_get_str(json_get(in, "plan_id")) : NULL;
    uint8_t id[32];
    if (!in || !json_get_bool_or(in, "confirm", false) || !vi_unhex(hex, id)) {
        vi_error(result, "CONFIRM_REQUIRED", "plan_id and confirm:true are required"); return true;
    }
    struct wallet_rpc_context *ctx = wallet_rpc_context_current();
    if (!vi_ready(ctx, result)) return true;
    int64_t now = (int64_t)platform_time_wall_time_t();
    vault_intent_expire_due(ctx->node_db, now);
    struct vault_intent_row row;
    if (!vault_intent_find(ctx->node_db, id, &row)) {
        vi_error(result, "PLAN_NOT_FOUND", "no durable plan has that id"); return true;
    }
    if (row.state >= VAULT_INTENT_MEMPOOL_ACCEPTED &&
        row.state <= VAULT_INTENT_FINALIZED) {
        json_set_object(result); json_push_kv_bool(result, "ok", true);
        vi_render_row(ctx, result, &row); json_push_kv_bool(result, "idempotent_replay", true); return true;
    }
    if (row.state == VAULT_INTENT_EXPIRED || row.expires_at <= now) {
        vi_error(result, "PLAN_EXPIRED", "the ten-minute plan lifetime elapsed"); return true;
    }
    if (row.state == VAULT_INTENT_PROVING &&
        !vault_intent_has_raw(ctx->node_db, id)) {
        if (now - row.updated_at < VI_PROVING_LEASE) {
            vi_error(result, "COMMIT_BUSY",
                     "another commit is preparing the signed transaction");
            return true;
        }
        if (!vault_intent_reclaim_proving(ctx->node_db, id,
                                          now - VI_PROVING_LEASE, now) ||
            !vault_intent_claim_commit(ctx->node_db, id, now)) {
            vi_error(result, "COMMIT_BUSY",
                     "the expired proving lease could not be reclaimed");
            return true;
        }
        row.state = VAULT_INTENT_PROVING;
        row.updated_at = now;
    }
    if (row.state == VAULT_INTENT_PLANNED &&
        !vault_intent_claim_commit(ctx->node_db, id, now)) {
        vi_error(result, "COMMIT_BUSY", "another commit claimed this plan"); return true;
    }
    if (row.state != VAULT_INTENT_PLANNED && row.state != VAULT_INTENT_PROVING) {
        vi_error(result, "PLAN_NOT_COMMITTABLE", vault_intent_state_name(row.state)); return true;
    }
    if (!vi_anchor_ok(ctx, &row)) {
        vault_intent_set_state(ctx->node_db, id, VAULT_INTENT_REORGED, NULL,
                               "ANCHOR_REORGED", now);
        vi_error(result, "ANCHOR_REORGED", "the plan chain anchor is no longer active"); return true;
    }
    uint8_t *raw = zcl_malloc(VAULT_INTENT_RAW_MAX, "intent_raw_tx");
    if (!raw) { vi_error(result, "OUT_OF_MEMORY", "raw transaction allocation failed"); return true; }
    size_t raw_len = 0; struct wallet_tx wtx; memset(&wtx, 0, sizeof(wtx));
    if (vault_intent_load_raw(ctx->node_db, id, raw, VAULT_INTENT_RAW_MAX,
                              &raw_len)) {
        struct byte_stream s; stream_init_from_data(&s, raw, raw_len);
        bool ok = transaction_deserialize(&wtx.tx, &s) && stream_remaining(&s) == 0;
        stream_free(&s);
        if (!ok) { free(raw); transaction_free(&wtx.tx); vi_error(result, "RAW_TX_CORRUPT", "prepared transaction failed to decode"); return true; }
        wtx.time_received = now; wtx.from_me = true; wtx.used = true;
    } else if (!vi_build_prepared(ctx, &row, &wtx, result)) {
        free(raw);
        const char *code = json_get_str(json_get(result, "code"));
        if (code && strcmp(code, "INPUT_CONFLICT") == 0)
            vault_intent_set_state(ctx->node_db, id, VAULT_INTENT_CONFLICTED,
                                   NULL, "INPUT_CONFLICT", now);
        return true;
    }
    free(raw);
    if (!vi_publish(ctx, id, &wtx, now, result)) { transaction_free(&wtx.tx); return true; }
    transaction_free(&wtx.tx);
    vault_intent_find(ctx->node_db, id, &row);
    json_set_object(result); json_push_kv_bool(result, "ok", true);
    vi_render_row(ctx, result, &row); json_push_kv_bool(result, "idempotent_replay", false);
    return true;
}

static bool rpc_vi_status(const struct json_value *params, bool help,
                          struct json_value *result)
{
    RPC_HELP(help, result, "vault_intent_status {plan_id}\n");
    const struct json_value *in = json_at(params, 0);
    const char *hex = in ? json_get_str(json_get(in, "plan_id")) : NULL;
    uint8_t id[32]; struct vault_intent_row row; struct node_db *ndb = wallet_rpc_node_db();
    if (!vi_unhex(hex, id) || !ndb || !vault_intent_find(ndb, id, &row)) {
        vi_error(result, "PLAN_NOT_FOUND", "no durable plan has that id"); return true;
    }
    vault_intent_expire_due(ndb, (int64_t)platform_time_wall_time_t());
    vault_intent_find(ndb, id, &row);
    vi_refresh_state(wallet_rpc_context_current(), &row,
                     (int64_t)platform_time_wall_time_t());
    json_set_object(result); json_push_kv_bool(result, "ok", true);
    vi_render_row(wallet_rpc_context_current(), result, &row);
    return true;
}

static bool rpc_vi_list(const struct json_value *params, bool help,
                        struct json_value *result)
{
    (void)params; RPC_HELP(help, result, "vault_intent_list\n");
    struct node_db *ndb = wallet_rpc_node_db();
    if (!ndb) { vi_error(result, "DATABASE_UNAVAILABLE", "node.db is unavailable"); return true; }
    vault_intent_expire_due(ndb, (int64_t)platform_time_wall_time_t());
    struct vault_intent_row rows[100]; int n = vault_intent_list(ndb, rows, 100);
    for (int i = 0; i < n; i++)
        vi_refresh_state(wallet_rpc_context_current(), &rows[i],
                         (int64_t)platform_time_wall_time_t());
    json_set_object(result); json_push_kv_bool(result, "ok", true);
    struct json_value items; json_init(&items); json_set_array(&items);
    for (int i = 0; i < n; i++) {
        struct json_value item; json_init(&item); json_set_object(&item);
        vi_render_row(wallet_rpc_context_current(), &item, &rows[i]);
        json_push_back(&items, &item); json_free(&item);
    }
    json_push_kv(result, "intents", &items); json_free(&items);
    json_push_kv_int(result, "count", n); return true;
}

void register_vault_intent_rpc_commands(struct rpc_table *t)
{
    const struct rpc_command cmds[] = {
        { "wallet", "vault_intent_plan", rpc_vi_plan, false },
        { "wallet", "vault_intent_commit", rpc_vi_commit, false },
        { "wallet", "vault_intent_status", rpc_vi_status, false },
        { "wallet", "vault_intent_list", rpc_vi_list, false },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
