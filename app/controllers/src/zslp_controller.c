/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZSLP token controller — token operations + shielded payments. */

#include "controllers/zslp_controller.h"
#include "zslp/slp.h"
#include "core/uint256.h"
#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "keys/key_io.h"
#include "chain/chainparams.h"
#include "script/standard.h"
#include "validation/sighash.h"
#include "consensus/upgrades.h"
#include "support/cleanse.h"
#include "validation/txmempool.h"
#include "primitives/transaction.h"
#include "config/runtime.h"
#include "models/zslp.h"
#include "services/zslp_command_service.h"
#include "services/zslp_service.h"
#include "rpc/server.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sqlite3.h>
#include "util/log_macros.h"

struct zslp_context {
    const char *datadir;
};

static struct zslp_context g_zslp_ctx = {0};

static struct zslp_context *zslp_ctx(void)
{
    return &g_zslp_ctx;
}

static struct wallet *zslp_wallet(void)
{
    return app_runtime_wallet();
}

static struct tx_mempool *zslp_mempool(void)
{
    return app_runtime_mempool();
}

static bool zslp_wallet_admission(struct wallet_tx_admission *out)
{
    if (!out)
        LOG_FAIL("zslp", "wallet_admission: NULL output");
    *out = (struct wallet_tx_admission) {
        .mempool = app_runtime_mempool(),
        .coins_tip = app_runtime_coins_tip(),
        .main_state = app_runtime_main_state(),
        .params = chain_params_get(),
    };
    if (!out->mempool || !out->coins_tip || !out->main_state)
        LOG_FAIL("zslp", "wallet_admission: runtime validation context incomplete");
    return true;
}

static const char *zslp_effective_datadir(const char *datadir)
{
    return datadir ? datadir : zslp_ctx()->datadir;
}

static bool zslp_open_runtime_db(const char *datadir, sqlite3 **db_out,
                                 bool *owns_db)
{
    struct zcl_result r =
        zslp_service_open_db(zslp_effective_datadir(datadir), db_out, owns_db);
    if (!r.ok)
        LOG_FAIL("zslp", "open_runtime_db: %s", r.message);
    return true;
}

static bool zslp_require_token_key(const char *token_key, struct json_value *result)
{
    if (token_key && zslp_service_validate_token_key(token_key).ok)
        return true;
    if (result)
        json_set_str(result, "token_id must be alphanumeric or 64-char hex");
    return false;
}

static bool zslp_require_address(const char *addr, bool strict_chain_addr,
                                 struct json_value *result)
{
    if (addr && zslp_service_validate_recipient_addr(addr, strict_chain_addr).ok)
        return true;
    if (result)
        json_set_str(result, "address is invalid");
    return false;
}

/* ── Token creation (GENESIS) ────────────────────────────── */

const char *zslp_create_token(const char *datadir,
                               const char *ticker,
                               const char *name,
                               uint8_t decimals,
                               uint64_t initial_supply)
{
    const char *effective_datadir = zslp_effective_datadir(datadir);
    struct zslp_token_create_request req = {
        .ticker = ticker,
        .name = name,
        .decimals = decimals,
        .initial_supply = initial_supply
    };
    const char *validation_error = NULL;
    if (!effective_datadir || !ticker || !name)
        LOG_NULL("zslp", "create_token: missing required param (datadir=%p ticker=%p name=%p)",
                 (const void *)effective_datadir, (const void *)ticker, (const void *)name);

    validation_error = zslp_service_validate_create_request(&req);
    if (validation_error)
        LOG_NULL("zslp", "create_token: %s", validation_error);

    /* Build the GENESIS OP_RETURN script */
    uint8_t script[256];
    size_t slen = slp_build_genesis(script, sizeof(script),
        ticker, name, "", NULL, decimals, 2, /* mint baton at vout 2 */
        initial_supply);

    if (slen == 0)
        LOG_NULL("zslp", "create_token: failed to build GENESIS script");

    printf("ZSLP GENESIS: ticker=%s name=%s decimals=%d supply=%llu "
           "script=%zu bytes\n",
           ticker, name, decimals, (unsigned long long)initial_supply, slen);

    /* Build transaction:
     *   vout[0]: OP_RETURN with GENESIS script (value=0)
     *   vout[1]: dust output to our address (receives initial supply)
     *   vout[2]: dust output to our address (mint baton)
     * Sign and broadcast via wallet. */
    char *broadcast_txid = NULL; /* set if we successfully broadcast */
    struct wallet *wallet = zslp_wallet();
    struct tx_mempool *mempool = zslp_mempool();

    if (!wallet || !mempool) {
        /* No wallet available (test mode) — skip on-chain broadcast,
         * just track in SQLite below. */
        goto store_sqlite;
    }

    struct wallet_tx wtx;
    int64_t fee_paid = 0;
    const char *tx_error = NULL;
    if (!zslp_command_build_genesis_base_tx(wallet, &wtx,
                                            &fee_paid, &tx_error).ok)
        LOG_NULL("zslp", "create_token: tx build failed: %s",
                 tx_error ? tx_error : "unknown");

    struct wallet_tx_admission admission;
    if (!zslp_wallet_admission(&admission) ||
        !zslp_command_commit_with_op_return(wallet, &wtx, &admission,
                                            script, slen).ok) {
        LOG_WARN("zslp", "zslp: commit failed");
        transaction_free(&wtx.tx);
        return NULL;
    }

    static char bc_txid[128];
    uint256_get_hex(&wtx.tx.hash, bc_txid);
    broadcast_txid = bc_txid;
    printf("ZSLP GENESIS broadcast: token_id=%s\n", broadcast_txid);

store_sqlite:
    ;
    static char result[128];
    if (!zslp_command_finalize_genesis(effective_datadir, broadcast_txid, &req,
                                       result).ok)
        LOG_NULL("zslp", "finalize_genesis failed for ticker=%s", ticker);
    return result;
}

/* ── Token balance ───────────────────────────────────────── */

uint64_t zslp_balance(const char *datadir,
                       const char *token_id_hex,
                       const char *addr)
{
    sqlite3 *db = NULL;
    bool owns_db = false;
    if (!zslp_effective_datadir(datadir) ||
        !zslp_service_validate_token_key(token_id_hex).ok ||
        !zslp_service_validate_recipient_addr(addr, false).ok) return 0;

    /* Scan the SQLite token_balances table */
    if (!zslp_open_runtime_db(datadir, &db, &owns_db))
        return 0;

    uint64_t bal = zslp_service_get_balance(db, token_id_hex, addr);
    zslp_service_close_db(db, owns_db);
    return bal;
}

/* ── Shielded payment address ────────────────────────────── */

bool zslp_generate_payment_address(const char *datadir,
                                    char *z_addr_out, size_t max)
{
    if (!zslp_effective_datadir(datadir))
        LOG_FAIL("zslp", "generate_payment_address: datadir not initialized");
    struct zcl_result r =
        zslp_payment_generate_address(zslp_wallet(), z_addr_out, max);
    if (!r.ok)
        LOG_FAIL("zslp", "generate_payment_address: %s", r.message);
    return true;
}

/* ── Payment detection ───────────────────────────────────── */

int64_t zslp_check_payment(const char *datadir,
                            const char *z_addr,
                            int64_t min_amount)
{
    if (!zslp_effective_datadir(datadir))
        return 0;
    return zslp_payment_check_received(zslp_effective_datadir(datadir),
                                       z_addr, min_amount);
}

/* ── Token mint ──────────────────────────────────────────── */

bool zslp_mint(const char *datadir,
                const char *token_id_hex,
                const char *recipient_addr,
                uint64_t amount)
{
    bool strict_chain_addr = (zslp_wallet() != NULL && zslp_mempool() != NULL);
    struct zslp_token_transfer_request req = {
        .token_id = token_id_hex,
        .recipient_addr = recipient_addr,
        .amount = amount,
        .strict_chain_addr = strict_chain_addr
    };
    const char *validation_error = NULL;
    if (!zslp_effective_datadir(datadir) ||
        !zslp_service_validate_token_key(token_id_hex).ok ||
        !recipient_addr)
        LOG_FAIL("zslp", "mint: invalid params (datadir=%p token=%p recipient=%p)",
                 (const void *)zslp_effective_datadir(datadir),
                 (const void *)token_id_hex, (const void *)recipient_addr);

    validation_error = zslp_service_validate_transfer_request(&req);
    if (validation_error)
        LOG_FAIL("zslp", "mint: %s", validation_error);
    if (!zslp_command_credit_transfer(zslp_effective_datadir(datadir), &req).ok)
        LOG_FAIL("zslp", "mint: balance update failed for token=%s",
                 token_id_hex ? token_id_hex : "?");

    /* Build and broadcast ZSLP MINT transaction on-chain */
    struct wallet *wallet = zslp_wallet();
    struct tx_mempool *mempool = zslp_mempool();
    if (!wallet || !mempool) {
        /* No wallet (test mode) — balances already updated above */
        return true;
    }

    struct uint256 token_id;
    uint256_set_hex(&token_id, token_id_hex);
    if (uint256_is_null(&token_id))
        LOG_FAIL("zslp", "mint: invalid token_id for broadcast: %s",
                 token_id_hex ? token_id_hex : "(null)");

    uint8_t op_script[256];
    size_t slen = slp_build_mint(op_script, sizeof(op_script),
        &token_id, 0, amount);
    if (slen == 0)
        LOG_FAIL("zslp", "mint: failed to build MINT script");

    struct wallet_tx wtx;
    int64_t fee_paid = 0;
    const char *tx_error = NULL;
    if (!zslp_command_build_send_base_tx(wallet, recipient_addr, &wtx,
                                         &fee_paid, &tx_error).ok)
        LOG_FAIL("zslp", "mint: tx build failed: %s",
                 tx_error ? tx_error : "unknown");

    struct wallet_tx_admission admission;
    if (!zslp_wallet_admission(&admission) ||
        !zslp_command_commit_with_op_return(wallet, &wtx, &admission,
                                            op_script, slen).ok) {
        LOG_WARN("zslp", "zslp: mint commit failed");
        transaction_free(&wtx.tx);
        return false;
    }

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    printf("ZSLP MINT broadcast: token=%s amount=%llu to=%s txid=%s\n",
           token_id_hex, (unsigned long long)amount, recipient_addr, txid);
    return true;
}

/* ── Token send ──────────────────────────────────────────── */

bool zslp_send(const char *datadir,
                const char *token_id_hex,
                const char *to_addr,
                uint64_t amount)
{
    struct wallet *wallet = zslp_wallet();
    struct tx_mempool *mempool = zslp_mempool();
    struct zslp_token_transfer_request req = {
        .token_id = token_id_hex,
        .recipient_addr = to_addr,
        .amount = amount,
        .strict_chain_addr = (wallet != NULL && mempool != NULL)
    };
    const char *validation_error = NULL;
    if (!zslp_effective_datadir(datadir) ||
        !zslp_service_validate_token_key(token_id_hex).ok ||
        !to_addr)
        LOG_FAIL("zslp", "send: invalid params (datadir=%p token=%p to=%p)",
                 (const void *)zslp_effective_datadir(datadir),
                 (const void *)token_id_hex, (const void *)to_addr);
    validation_error = zslp_service_validate_transfer_request(&req);
    if (validation_error)
        LOG_FAIL("zslp", "send: %s", validation_error);

    if (!wallet || !mempool) {
        /* No wallet (test mode) — just update SQLite balances */
        struct zcl_result r =
            zslp_command_credit_transfer(zslp_effective_datadir(datadir), &req);
        if (!r.ok)
            LOG_FAIL("zslp", "send: balance update failed: %s", r.message);
        return true;
    }

    /* Build SEND OP_RETURN script */
    struct uint256 token_id;
    uint256_set_hex(&token_id, token_id_hex);
    if (uint256_is_null(&token_id))
        LOG_FAIL("zslp", "send: invalid token_id: %s",
                 token_id_hex ? token_id_hex : "(null)");

    uint64_t quantities[1] = { amount };
    uint8_t op_script[256];
    size_t slen = slp_build_send(op_script, sizeof(op_script),
        &token_id, quantities, 1);
    if (slen == 0)
        LOG_FAIL("zslp", "send: failed to build SEND script");

    struct wallet_tx wtx;
    int64_t fee_paid = 0;
    const char *tx_error = NULL;
    if (!zslp_command_build_send_base_tx(wallet, to_addr, &wtx,
                                         &fee_paid, &tx_error).ok)
        LOG_FAIL("zslp", "send: tx build failed: %s",
                 tx_error ? tx_error : "unknown");

    struct wallet_tx_admission admission;
    if (!zslp_wallet_admission(&admission) ||
        !zslp_command_commit_with_op_return(wallet, &wtx, &admission,
                                            op_script, slen).ok) {
        LOG_WARN("zslp", "zslp: commit failed");
        transaction_free(&wtx.tx);
        return false;
    }

    /* Update balances in SQLite */
    if (!zslp_command_credit_transfer(zslp_effective_datadir(datadir), &req).ok)
        LOG_FAIL("zslp", "send: balance update failed for token=%s",
                 token_id_hex ? token_id_hex : "?");

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    printf("ZSLP SEND broadcast: token=%s amount=%llu to=%s txid=%s\n",
           token_id_hex, (unsigned long long)amount, to_addr, txid);
    return true;
}

/* ── RPC handlers ────────────────────────────────────────── */

void zslp_rpc_set_datadir(const char *datadir)
{
    zslp_ctx()->datadir = datadir;
}

static bool zslp_parse_amount(const struct json_value *value, uint64_t *amount_out)
{
    int64_t raw;
    if (!value || !amount_out)
        LOG_FAIL("zslp", "parse_amount: null value=%p or amount_out=%p",
                 (const void *)value, (const void *)amount_out);
    raw = json_get_int(value);
    if (raw <= 0)
        LOG_FAIL("zslp", "parse_amount: non-positive amount %lld", (long long)raw);
    *amount_out = (uint64_t)raw;
    return true;
}

static bool zslp_parse_token_param(const struct json_value *params, size_t index,
                                   const char **token_id,
                                   struct json_value *result);
static bool zslp_parse_addr_param(const struct json_value *params, size_t index,
                                  bool strict_chain_addr, const char **addr,
                                  struct json_value *result);

static void zslp_render_token_json(struct json_value *out,
                                   const struct db_zslp_token_info *token)
{
    json_set_object(out);
    json_push_kv_str(out, "token_id", token->token_id);
    json_push_kv_str(out, "ticker", token->ticker);
    json_push_kv_str(out, "name", token->name);
    json_push_kv_int(out, "decimals", token->decimals);
    json_push_kv_int(out, "genesis_height", token->genesis_height);
    json_push_kv_int(out, "total_minted", token->total_minted);
}

static void zslp_render_transfer_json(struct json_value *out,
                                      const struct db_zslp_transfer_info *xfer)
{
    json_set_object(out);
    json_push_kv_str(out, "txid", xfer->txid);
    json_push_kv_str(out, "token_id", xfer->token_id);
    json_push_kv_int(out, "block_height", xfer->block_height);
    json_push_kv_int(out, "tx_type", xfer->tx_type);
    json_push_kv_int(out, "amount", xfer->amount);
    json_push_kv_int(out, "vout", xfer->vout);
    if (xfer->to_addr_hex[0])
        json_push_kv_str(out, "to_addr_hex", xfer->to_addr_hex);
}

static bool zslp_parse_create_request(const struct json_value *params,
                                      struct zslp_token_create_request *req,
                                      struct json_value *result)
{
    const char *validation_error;

    req->ticker = json_get_str(json_at(params, 0));
    req->name = json_get_str(json_at(params, 1));
    req->decimals = (uint8_t)json_get_int(json_at(params, 2));
    req->initial_supply = 0;
    if (!req->ticker || !req->name) {
        json_set_str(result, "invalid parameters");
        return false;
    }
    validation_error = zslp_service_validate_create_request(req);
    if (validation_error && strcmp(validation_error,
            "initial supply exceeds maximum") != 0) {
        json_set_str(result, validation_error);
        return false;
    }
    if (!zslp_parse_amount(json_at(params, 3), &req->initial_supply)) {
        json_set_str(result, "supply must be a positive integer");
        return false;
    }
    validation_error = zslp_service_validate_create_request(req);
    if (validation_error) {
        json_set_str(result, validation_error);
        return false;
    }
    return true;
}

static bool zslp_parse_transfer_request(const struct json_value *params,
                                        bool strict_chain_addr,
                                        struct zslp_token_transfer_request *req,
                                        struct json_value *result)
{
    const char *validation_error;

    if (!zslp_parse_token_param(params, 0, &req->token_id, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (!zslp_parse_addr_param(params, 1, strict_chain_addr,
                               &req->recipient_addr, result))
        return false;
    if (!zslp_parse_amount(json_at(params, 2), &req->amount)) {
        json_set_str(result, "amount must be a positive integer");
        return false;
    }
    req->strict_chain_addr = strict_chain_addr;
    validation_error = zslp_service_validate_transfer_request(req);
    if (validation_error) {
        json_set_str(result, validation_error);
        return false;
    }
    return true;
}

static bool zslp_parse_token_param(const struct json_value *params, size_t index,
                                   const char **token_id,
                                   struct json_value *result)
{
    *token_id = json_get_str(json_at(params, index));
    return zslp_require_token_key(*token_id, result);
}

static bool zslp_parse_addr_param(const struct json_value *params, size_t index,
                                  bool strict_chain_addr, const char **addr,
                                  struct json_value *result)
{
    *addr = json_get_str(json_at(params, index));
    return zslp_require_address(*addr, strict_chain_addr, result);
}

static bool zslp_rpc_require_context(struct json_value *result)
{
    if (!zslp_effective_datadir(NULL)) {
        json_set_str(result, "zslp runtime/datadir not initialized");
        return false;
    }
    return true;
}

/* zslp_createtoken "ticker" "name" decimals supply */
static bool rpc_zslp_createtoken(const struct json_value *params,
                                   bool help, struct json_value *result)
{
    struct zslp_token_create_request req;
    if (help || !params || json_size(params) < 4) {
        json_set_str(result,
            "zslp_createtoken \"ticker\" \"name\" decimals supply");
        return !help;
    }
    if (!zslp_rpc_require_context(result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (!zslp_parse_create_request(params, &req, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)

    const char *token_id = zslp_create_token(NULL, req.ticker, req.name,
        req.decimals, req.initial_supply);
    if (token_id)
        json_set_str(result, token_id);
    else {
        json_set_str(result, "token creation failed");
        return false;
    }
    return true;
}

/* zslp_send "token_id" "address" amount */
static bool rpc_zslp_send(const struct json_value *params,
                             bool help, struct json_value *result)
{
    struct zslp_token_transfer_request req;
    bool strict_chain_addr;
    if (help || !params || json_size(params) < 3) {
        json_set_str(result,
            "zslp_send \"token_id\" \"address\" amount");
        return !help;
    }
    if (!zslp_rpc_require_context(result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    strict_chain_addr = (zslp_wallet() != NULL && zslp_mempool() != NULL);
    if (!zslp_parse_transfer_request(params, strict_chain_addr, &req, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)

    bool ok = zslp_send(NULL, req.token_id, req.recipient_addr, req.amount);
    json_set_bool(result, ok);
    return ok;
}

/* zslp_balance "token_id" "address" */
static bool rpc_zslp_balance(const struct json_value *params,
                               bool help, struct json_value *result)
{
    const char *token_id = NULL;
    const char *addr = NULL;
    if (help || !params || json_size(params) < 2) {
        json_set_str(result,
            "zslp_balance \"token_id\" \"address\"");
        return !help;
    }

    if (!zslp_rpc_require_context(result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (!zslp_parse_token_param(params, 0, &token_id, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (!zslp_parse_addr_param(params, 1, false, &addr, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)

    uint64_t bal = zslp_balance(NULL, token_id, addr);
    json_set_int(result, (int64_t)bal);
    return true;
}

/* zslp_mint "token_id" "address" amount */
static bool rpc_zslp_mint(const struct json_value *params,
                             bool help, struct json_value *result)
{
    struct zslp_token_transfer_request req;
    bool strict_chain_addr;
    if (help || !params || json_size(params) < 3) {
        json_set_str(result,
            "zslp_mint \"token_id\" \"address\" amount");
        return !help;
    }
    if (!zslp_rpc_require_context(result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    strict_chain_addr = (zslp_wallet() != NULL && zslp_mempool() != NULL);
    if (!zslp_parse_transfer_request(params, strict_chain_addr, &req, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)

    bool ok = zslp_mint(NULL, req.token_id, req.recipient_addr, req.amount);
    json_set_bool(result, ok);
    return ok;
}

static bool rpc_zslp_gettoken(const struct json_value *params,
                              bool help, struct json_value *result)
{
    sqlite3 *db = NULL;
    bool owns_db = false;
    const char *token_id = NULL;
    struct db_zslp_token_info token;

    if (help || !params || json_size(params) < 1) {
        json_set_str(result, "zslp_gettoken \"token_id\"");
        return !help;
    }
    if (!zslp_rpc_require_context(result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (!zslp_parse_token_param(params, 0, &token_id, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (!zslp_open_runtime_db(NULL, &db, &owns_db)) {
        json_set_str(result, "zslp database unavailable");
        return false;
    }
    if (!zslp_service_get_token(db, token_id, &token).ok) {
        zslp_service_close_db(db, owns_db);
        json_set_str(result, "token not found");
        return false;
    }
    zslp_service_close_db(db, owns_db);

    zslp_render_token_json(result, &token);
    return true;
}

static bool rpc_zslp_listtokens(const struct json_value *params,
                                bool help, struct json_value *result)
{
    sqlite3 *db = NULL;
    bool owns_db = false;
    struct db_zslp_token_info tokens[64];
    int count = 0;
    int64_t limit = 50;

    if (help) {
        json_set_str(result, "zslp_listtokens ( limit )");
        return false;
    }
    if (!zslp_rpc_require_context(result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (params && json_size(params) > 0) {
        limit = json_get_int(json_at(params, 0));
        if (limit <= 0 || limit > 64) {
            json_set_str(result, "limit must be between 1 and 64");
            return false;
        }
    }
    if (!zslp_open_runtime_db(NULL, &db, &owns_db)) {
        json_set_str(result, "zslp database unavailable");
        return false;
    }
    count = zslp_service_list_tokens(db, tokens, (size_t)limit);
    zslp_service_close_db(db, owns_db);

    json_set_array(result);
    for (int i = 0; i < count; i++) {
        struct json_value entry = {0};
        json_init(&entry);
        zslp_render_token_json(&entry, &tokens[i]);
        json_push_back(result, &entry);
    }
    return true;
}

static bool rpc_zslp_listtransfers(const struct json_value *params,
                                   bool help, struct json_value *result)
{
    sqlite3 *db = NULL;
    bool owns_db = false;
    const char *token_id = NULL;
    struct db_zslp_transfer_info transfers[64];
    int count = 0;
    int64_t limit = 50;

    if (help || !params || json_size(params) < 1) {
        json_set_str(result, "zslp_listtransfers \"token_id\" ( limit )");
        return !help;
    }
    if (!zslp_rpc_require_context(result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (!zslp_parse_token_param(params, 0, &token_id, result))
        return false; // raw-return-ok:RPC error body already set via json_set_str(result,...)
    if (json_size(params) > 1) {
        limit = json_get_int(json_at(params, 1));
        if (limit <= 0 || limit > 64) {
            json_set_str(result, "limit must be between 1 and 64");
            return false;
        }
    }
    if (!zslp_open_runtime_db(NULL, &db, &owns_db)) {
        json_set_str(result, "zslp database unavailable");
        return false;
    }
    count = zslp_service_list_transfers(db, token_id, transfers, (size_t)limit);
    zslp_service_close_db(db, owns_db);

    json_set_array(result);
    for (int i = 0; i < count; i++) {
        struct json_value entry = {0};
        json_init(&entry);
        zslp_render_transfer_json(&entry, &transfers[i]);
        json_push_back(result, &entry);
    }
    return true;
}

void register_zslp_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "zslp", "zslp_createtoken", rpc_zslp_createtoken, false },
        { "zslp", "zslp_gettoken",    rpc_zslp_gettoken,    true  },
        { "zslp", "zslp_listtokens",  rpc_zslp_listtokens,  true  },
        { "zslp", "zslp_listtransfers", rpc_zslp_listtransfers, true },
        { "zslp", "zslp_send",       rpc_zslp_send,         false },
        { "zslp", "zslp_balance",    rpc_zslp_balance,      true  },
        { "zslp", "zslp_mint",       rpc_zslp_mint,         false },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
