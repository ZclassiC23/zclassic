/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/wallet_controller_internal.h"
#include "controllers/wallet_shielded_controller.h"
#include "controllers/wallet_diagnostic_controller.h"
#include "controllers/wallet_rescan_controller.h"
void rpc_wallet_set_state(struct wallet *w, struct main_state *ms,
                          const char *datadir, struct wallet_sqlite *wdb,
                          struct tx_mempool *mempool,
                          struct connman *connman)
{
    wallet_rpc_context_set_base(w, ms, datadir, wdb, mempool, connman);
}

void rpc_wallet_set_node_db(struct node_db *ndb)
{
    wallet_rpc_context_set_node_db(ndb);
}

void rpc_wallet_set_coins_tip(struct coins_view_cache *tip)
{
    wallet_rpc_context_set_coins_tip(tip);
}


static bool rpc_getnewaddress(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getnewaddress\n"
        "Returns a new ZClassic address for receiving payments.");

    ENSURE_WALLET(result);
    if (!ctx->wallet_db || !ctx->wallet_db->open) {
        json_set_str(result,
            "Error: wallet durability backend unavailable; address not created");
        LOG_FAIL("wallet",
                 "getnewaddress: refusing RAM-only receive address");
    }

    bool direct_generated = wallet_has_hd(ctx->wallet);
    if (!direct_generated &&
        wallet_key_pool_persisted_size(ctx->wallet) == 0) {
        if (!wallet_top_up_key_pool(ctx->wallet, DEFAULT_KEYPOOL_SIZE)) {
            json_set_str(result, "Error: keypool top-up failed");
            LOG_FAIL("wallet", "getnewaddress: keypool top-up failed");
        }
        int64_t pool_generation =
            wallet_key_pool_generation_ceiling(ctx->wallet);
        if (ctx->wallet_db) {
            struct zcl_result topup_flush = wallet_flush_from_context(ctx);
            if (!topup_flush.ok) {
                json_set_str(result,
                    "Error: wallet persistence failed. No unpersisted "
                    "keypool address was returned.");
                LOG_FAIL("wallet",
                         "getnewaddress: keypool durability failed "
                         "(code=%d): %s",
                         topup_flush.code, topup_flush.message);
            }
        }
        wallet_key_pool_mark_persisted_through(
            ctx->wallet, pool_generation);
    }

    char addr[128];
    struct key_id generated_kid;
    if (!wallet_get_new_address_ex(ctx->wallet, addr, sizeof(addr),
                                   &generated_kid)) {
        json_set_str(result, "Error: no durable keypool address available");
        LOG_FAIL("wallet", "getnewaddress: durable keypool ran out");
    }

    /* Persist the fresh key to wallet_keys BEFORE handing the address
     * to the user. If the flush fails, roll back the exact returned key ID
     * so concurrent key generation cannot make us remove a different key.
     * Never return an address we cannot persist: a
     * receive to an unsaved key would lose funds on the next restart. */
    if (ctx->wallet_db && direct_generated) {
        struct zcl_result fr = wallet_flush_from_context(ctx);
        if (!fr.ok) {
            bool removed = wallet_remove_key(ctx->wallet, &generated_kid);
            json_set_str(result,
                "Error: wallet persistence failed. New address NOT saved. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet", "getnewaddress: wallet_sqlite_flush_r failed "
                                "(exact_key_removed=%d, code=%d): %s",
                                (int)removed, fr.code, fr.message);
        }

    }

    /* Success: kick the JSON backup writer so the mirror follows. */
    if (ctx->wallet_db)
        wallet_backup_service_on_key_change();

    json_set_str(result, addr);
    return true;
}

static bool rpc_getbalance(const struct json_value *params, bool help,
                            struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getbalance\n"
        "Returns the total available balance.");

    ENSURE_WALLET(result);

    /* Use SQLite model layer as authoritative source. Spendable balance
     * EXCLUDES immature coinbase (matches listunspent + the coin selector),
     * so the reported number is what the user can actually send. */
    int64_t balance = wallet_ctx_db_ready(ctx)
        ? db_wallet_utxo_spendable_balance(ctx->node_db, NULL)
        : wallet_get_balance(ctx->wallet);
    char buf[32];
    format_amount(balance, buf, sizeof(buf));
    json_set_str(result, buf);
    return true;
}

static bool rpc_getunconfirmedbalance(const struct json_value *params,
                                       bool help, struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getunconfirmedbalance\n"
        "Returns the unconfirmed balance.");

    ENSURE_WALLET(result);

    int64_t balance = wallet_get_unconfirmed_balance(ctx->wallet);
    char buf[32];
    format_amount(balance, buf, sizeof(buf));
    json_set_str(result, buf);
    return true;
}

static bool rpc_getwalletinfo(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getwalletinfo\n"
        "Returns wallet state info.");

    ENSURE_WALLET(result);

    json_set_object(result);
    char bal[32], ubal[32], ibal[32], fee[32];
    /* Spendable balance EXCLUDES immature coinbase (matches listunspent + the
     * coin selector). Emit money fields as strings to preserve full precision
     * (the JSON real serializer uses %.8g, which rounds large balances). */
    int64_t balance = wallet_ctx_db_ready(ctx)
        ? db_wallet_utxo_spendable_balance(ctx->node_db, NULL)
        : wallet_get_balance(ctx->wallet);
    format_amount(balance, bal, sizeof(bal));
    format_amount(wallet_get_unconfirmed_balance(ctx->wallet), ubal, sizeof(ubal));
    format_amount(wallet_get_immature_balance(ctx->wallet), ibal, sizeof(ibal));
    format_amount(ctx->wallet->default_fee, fee, sizeof(fee));
    json_push_kv_str(result, "balance", bal);
    json_push_kv_str(result, "unconfirmed_balance", ubal);
    json_push_kv_str(result, "immature_balance", ibal);
    json_push_kv_int(result, "txcount", (int64_t)wallet_history_count());
    json_push_kv_int(result, "keypoolsize", (int64_t)ctx->wallet->key_pool_size);
    json_push_kv_str(result, "paytxfee", fee);

    /* Persistence health block. Aggregates the canary status + a live
     * count query so operators and tooling can see at a glance whether
     * the wallet storage is healthy.
     *
     *   healthy = open && canary_ok && !mismatch
     *
     * A false value here means the persistence-abort paths would fire
     * on the next restart — surface it before the user sends funds to
     * an address that won't survive reboot. */
    sqlite3 *wallet_sqlite_handle = (ctx->wallet_db && ctx->wallet_db->open)
                                      ? ctx->wallet_db->db
                                      : NULL;
    struct wallet_persistence_health h = wallet_persistence_get_health(
        wallet_sqlite_handle, (int)ctx->wallet->keystore.num_keys);

    struct json_value persistence = {0};
    json_init(&persistence);
    json_set_object(&persistence);
    json_push_kv_bool(&persistence, "healthy",
                       h.open && h.canary_ok && !h.mismatch);
    json_push_kv_bool(&persistence, "open",              h.open);
    json_push_kv_bool(&persistence, "canary_ok",         h.canary_ok);
    json_push_kv_int (&persistence, "canary_last_ok_ts", h.canary_last_ok_ts);
    json_push_kv_int (&persistence, "row_count",         h.row_count);
    json_push_kv_int (&persistence, "keystore_count",    h.keystore_count);
    json_push_kv_bool(&persistence, "mismatch",          h.mismatch);
    json_push_kv_int (&persistence, "corrupt_rows",      h.corrupt_rows);
    json_push_kv_str (&persistence, "last_error",        h.last_error);
    json_push_kv(result, "persistence", &persistence);
    return true;
}

static bool rpc_listunspent(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "listunspent ( minconf maxconf )\n"
        "Returns array of unspent transaction outputs.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int min_conf = (int)rpc_permit_int(&p, 0, "minconf", 1);
    int max_conf = (int)rpc_permit_int(&p, 1, "maxconf", 9999999);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "listunspent: invalid params"); }

    ENSURE_WALLET(result);
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "listunspent", "Chainstate lookup"))
        return false;


    int tip = active_chain_height(&ctx->main_state->chain_active);

    json_set_array(result);

    /* SQLite model layer — authoritative UTXO source */
    if (wallet_ctx_db_ready(ctx)) {
        struct db_wallet_utxo utxos[4096];
        int n = db_wallet_utxo_list_unspent(ctx->node_db, utxos, 4096);
        for (int i = 0; i < n; i++) {
            int h = utxos[i].height;
            /* Fix height=0: look up real height from global UTXO index */
            if (h <= 0) {
                struct db_utxo global;
                if (db_utxo_find(ctx->node_db, utxos[i].txid,
                                  utxos[i].vout, &global)) {
                    h = global.height;
                    db_utxo_free(&global);
                }
            }
            int confs = (h > 0) ? tip - h + 1 : 0;
            if (confs < min_conf || confs > max_conf)
                continue;
            if (utxos[i].is_coinbase && confs < 100)
                continue;

            struct json_value entry = {0};
            json_init(&entry);
            json_set_object(&entry);

            struct uint256 txid_u;
            memcpy(txid_u.data, utxos[i].txid, 32);
            char txid_hex[65];
            uint256_get_hex(&txid_u, txid_hex);
            json_push_kv_str(&entry, "txid", txid_hex);
            json_push_kv_int(&entry, "vout", (int64_t)utxos[i].vout);

            /* Decode address from script */
            if (utxos[i].script && utxos[i].script_len > 0 &&
                utxos[i].script_len <= MAX_SCRIPT_SIZE) {
                struct script sc;
                script_init(&sc);
                memcpy(sc.data, utxos[i].script, utxos[i].script_len);
                sc.size = utxos[i].script_len;
                struct tx_destination dest;
                if (script_extract_destination(&sc, &dest)) {
                    char addr[128];
                    if (wallet_encode_destination(&dest, addr, sizeof(addr)))
                        json_push_kv_str(&entry, "address", addr);
                }
            }

            char amt_buf[32];
            format_amount(utxos[i].value, amt_buf, sizeof(amt_buf));
            json_push_kv_real(&entry, "amount", strtod(amt_buf, NULL));
            json_push_kv_int(&entry, "confirmations", (int64_t)confs);
            json_push_kv_bool(&entry, "spendable", true);
            json_push_kv_bool(&entry, "solvable", true);

            json_push_back(result, &entry);
            json_free(&entry);
            db_wallet_utxo_free(&utxos[i]);
        }
        return true;
    }

    /* Fallback: in-memory wallet */
    struct coin_entry coins[4096];
    size_t num_coins = 0;
    wallet_available_coins(ctx->wallet, coins, &num_coins, 4096,
                           min_conf > 0, false);
    for (size_t i = 0; i < num_coins; i++) {
        if (coins[i].depth < min_conf || coins[i].depth > max_conf)
            continue;

        struct json_value entry = {0};
        json_init(&entry);
        json_set_object(&entry);

        char txid[65];
        uint256_get_hex(&coins[i].wtx->tx.hash, txid);
        json_push_kv_str(&entry, "txid", txid);
        json_push_kv_int(&entry, "vout", (int64_t)coins[i].i);

        const struct tx_out *out = &coins[i].wtx->tx.vout[coins[i].i];
        struct tx_destination dest;
        if (script_extract_destination(&out->script_pub_key, &dest)) {
            char addr[128];
            if (wallet_encode_destination(&dest, addr, sizeof(addr)))
                json_push_kv_str(&entry, "address", addr);
        }

        char amt_buf[32];
        format_amount(out->value, amt_buf, sizeof(amt_buf));
        json_push_kv_real(&entry, "amount", strtod(amt_buf, NULL));
        json_push_kv_int(&entry, "confirmations", (int64_t)coins[i].depth);
        json_push_kv_bool(&entry, "spendable", coins[i].spendable);
        json_push_kv_bool(&entry, "solvable", coins[i].solvable);

        json_push_back(result, &entry);
        json_free(&entry);
    }

    return true;
}

static bool rpc_sendtoaddress(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "sendtoaddress \"address\" amount\n"
        "Send an amount to a given address.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 2, 2);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    int64_t amount = rpc_require_amount(&p, 1, "amount");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "sendtoaddress: invalid params"); }

    ENSURE_WALLET(result);

    if (amount <= 0) {
        json_set_str(result, "Invalid amount");
        LOG_FAIL("wallet", "sendtoaddress: invalid amount %lld", (long long)amount);
    }

    struct tx_destination dest;
    if (!wallet_decode_address(addr_str, &dest)) {
        json_set_str(result, "Invalid address");
        LOG_FAIL("wallet", "sendtoaddress: invalid address %s", addr_str);
    }

    struct wallet_tx wtx;
    int64_t fee = 0;
    const char *error = NULL;
    if (!wallet_create_transaction(ctx->wallet, &dest, amount,
                                    &wtx, &fee, &error)) {
        json_set_str(result, error ? error : "Transaction creation failed");
        LOG_FAIL("wallet", "sendtoaddress: create tx failed: %s", error ? error : "unknown");
    }

    /* Persist the wallet keystore (which now holds the freshly-minted change
     * key) to disk BEFORE broadcasting. The change key is RAM-only until this
     * flush; a flush failure (disk full / I/O error) AFTER broadcast would put
     * a tx on the wire that pays change to a key absent from disk — permanently
     * unspendable on restart. Treat a pre-broadcast flush failure as a hard
     * error and abort the send (write-ahead the key, like zclassicd). */
    if (ctx->wallet_db) {
        struct zcl_result fr = wallet_flush_from_context(ctx);
        if (!fr.ok) {
            transaction_free(&wtx.tx);
            json_set_str(result, "Cannot persist change key before broadcast — send aborted");
            LOG_FAIL("wallet", "sendtoaddress: pre-broadcast key flush failed "
                               "(code=%d): %s", fr.code, fr.message);
        }
    }

    struct zcl_result commit = wallet_commit_from_context(ctx, &wtx);
    if (!commit.ok) {
        json_set_str(result, commit.message);
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet", "sendtoaddress: commit transaction failed "
                           "(code=%d): %s", commit.code, commit.message);
    }

    struct zcl_result persisted =
        wallet_persist_commit_before_relay(ctx, &wtx);
    if (!persisted.ok) {
        json_set_str(result, persisted.message);
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet", "sendtoaddress: pre-relay durability failed "
                           "(code=%d): %s", persisted.code,
                           persisted.message);
    }

    if (wallet_ctx_db_ready(ctx))
        node_db_sync_wallet_tx(ctx->node_db, &wtx.tx, ctx->wallet, 0);

    /* Relay to peers */
    if (ctx->connman)
        connman_relay_transaction(ctx->connman, &wtx.tx.hash);

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);
    transaction_free(&wtx.tx);
    return true;
}

/* ── Direct C API for wallet view controller ──────────────── */

bool wallet_direct_sendtoaddress(const char *address, int64_t amount_sat,
                                  char *txid_out, size_t txid_out_size,
                                  char *error_out, size_t error_out_size)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)txid_out_size; /* always 65 bytes for hex txid */
    if (!ctx->wallet) {
        snprintf(error_out, error_out_size, "Wallet not loaded");
        LOG_FAIL("wallet", "direct_sendtoaddress: wallet not loaded");
    }
    if (amount_sat <= 0) {
        snprintf(error_out, error_out_size, "Invalid amount");
        LOG_FAIL("wallet", "direct_sendtoaddress: invalid amount %lld", (long long)amount_sat);
    }

    struct tx_destination dest;
    if (!wallet_decode_address(address, &dest)) {
        snprintf(error_out, error_out_size, "Invalid address");
        LOG_FAIL("wallet", "direct_sendtoaddress: invalid address %s", address);
    }

    struct wallet_tx wtx;
    int64_t fee = 0;
    const char *err = NULL;
    if (!wallet_create_transaction(ctx->wallet, &dest, amount_sat, &wtx, &fee, &err)) {
        snprintf(error_out, error_out_size, "%s", err ? err : "Transaction creation failed");
        LOG_FAIL("wallet", "direct_sendtoaddress: create tx failed: %s", err ? err : "unknown");
    }

    /* Persist the change key BEFORE broadcast (see rpc_sendtoaddress): abort
     * the send if the keystore flush fails, so we never broadcast a tx whose
     * RAM-only change key isn't durable. */
    if (ctx->wallet_db) {
        struct zcl_result fr = wallet_flush_from_context(ctx);
        if (!fr.ok) {
            transaction_free(&wtx.tx);
            snprintf(error_out, error_out_size,
                     "Cannot persist change key before broadcast — send aborted");
            LOG_FAIL("wallet", "direct_sendtoaddress: pre-broadcast key flush "
                               "failed (code=%d): %s", fr.code, fr.message);
        }
    }

    struct zcl_result commit = wallet_commit_from_context(ctx, &wtx);
    if (!commit.ok) {
        snprintf(error_out, error_out_size, "%s", commit.message);
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet", "direct_sendtoaddress: commit transaction failed "
                           "(code=%d): %s", commit.code, commit.message);
    }

    struct zcl_result persisted =
        wallet_persist_commit_before_relay(ctx, &wtx);
    if (!persisted.ok) {
        snprintf(error_out, error_out_size, "%s", persisted.message);
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet", "direct_sendtoaddress: pre-relay durability failed "
                           "(code=%d): %s", persisted.code,
                           persisted.message);
    }

    if (wallet_ctx_db_ready(ctx))
        node_db_sync_wallet_tx(ctx->node_db, &wtx.tx, ctx->wallet, 0);
    if (ctx->connman)
        connman_relay_transaction(ctx->connman, &wtx.tx.hash);

    uint256_get_hex(&wtx.tx.hash, txid_out);
    transaction_free(&wtx.tx);
    return true;
}

static bool rpc_rescanblockchain(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "rescanblockchain ( start_height stop_height )\n"
        "\nRescan the local blockchain for wallet transactions.\n"
        "\nArguments:\n"
        "1. start_height  (numeric, optional, default=0) Block height to start\n"
        "2. stop_height   (numeric, optional, default=tip) Block height to stop\n"
        "\nResult:\n"
        "{\n"
        "  \"start_height\": n,\n"
        "  \"stop_height\": n\n"
        "}\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int start_height = (int)rpc_permit_int(&p, 0, "start_height", 0);
    int stop_height = (int)rpc_permit_int(&p, 1, "stop_height", -1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "rescanblockchain: invalid params"); }

    ENSURE_WALLET(result);

    if (!ctx->main_state) {
        json_set_str(result, "Chain state not initialized");
        LOG_FAIL("wallet", "rescanblockchain: chain state not initialized");
    }
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "rescanblockchain",
            "Chainstate lookup"))
        return false;

    int tip = active_chain_height(&ctx->main_state->chain_active);
    if (stop_height < 0 || stop_height > tip)
        stop_height = tip;
    if (start_height < 0)
        start_height = 0;

    if (start_height > tip) {
        json_set_str(result, "start_height exceeds chain tip");
        LOG_FAIL("wallet", "rescanblockchain: start_height %d exceeds tip %d", start_height, tip);
    }

    wallet_rescan(ctx->wallet, &ctx->main_state->chain_active,
                  start_height, stop_height, ctx->datadir);

    json_set_object(result);
    json_push_kv_int(result, "start_height", start_height);
    json_push_kv_int(result, "stop_height", stop_height);
    return true;
}

static bool rpc_keypoolrefill(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "keypoolrefill ( newsize )\nFills the keypool.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    unsigned int new_size = (unsigned int)rpc_permit_int(&p, 0, "newsize", DEFAULT_KEYPOOL_SIZE);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "keypoolrefill: invalid params"); }

    ENSURE_WALLET(result);
    if (!ctx->wallet_db || !ctx->wallet_db->open) {
        json_set_str(result,
            "Error: wallet durability backend unavailable; keypool unchanged");
        LOG_FAIL("wallet", "keypoolrefill: refusing RAM-only keypool");
    }

    if (!wallet_top_up_key_pool(ctx->wallet, new_size)) {
        json_set_str(result, "Error refilling keypool");
        LOG_FAIL("wallet", "keypoolrefill: failed to refill keypool (size=%u)", new_size);
    }
    int64_t pool_generation =
        wallet_key_pool_generation_ceiling(ctx->wallet);

    /* Flush the fresh keypool entries. If persistence fails the
     * keypool indices still point into the keystore, but the on-disk
     * rows won't exist — on next restart the node would hand out a
     * pre-existing address twice. Log and error; canary will flag
     * the daemon as unhealthy and operator can intervene. */
    if (ctx->wallet_db) {
        struct zcl_result fr = wallet_flush_from_context(ctx);
        if (!fr.ok) {
            json_set_str(result,
                "Error: keypool refilled in memory but persistence flush failed. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet", "keypoolrefill: wallet_sqlite_flush_r failed "
                                "(new_size=%u, code=%d): %s",
                                new_size, fr.code, fr.message);
        }
        wallet_key_pool_mark_persisted_through(
            ctx->wallet, pool_generation);
        wallet_backup_service_on_keypool_topup();
    } else {
        wallet_key_pool_mark_persisted_through(
            ctx->wallet, pool_generation);
    }

    json_set_null(result);
    return true;
}


void register_wallet_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "wallet", "getnewaddress",        rpc_getnewaddress,        false },
        { "wallet", "getbalance",           rpc_getbalance,           false },
        { "wallet", "getunconfirmedbalance", rpc_getunconfirmedbalance, false },
        { "wallet", "getwalletinfo",        rpc_getwalletinfo,        false },
        { "wallet", "listunspent",          rpc_listunspent,          false },
        { "wallet", "sendtoaddress",        rpc_sendtoaddress,        false },
        { "wallet", "dumpprivkey",          rpc_dumpprivkey,          false },
        { "wallet", "importprivkey",        rpc_importprivkey,        false },
        { "wallet", "importaddress",       rpc_importaddress,        false },
        { "wallet", "keypoolrefill",        rpc_keypoolrefill,        false },
        { "wallet", "listtransactions",     rpc_listtransactions,     false },
        { "wallet", "gettransaction",       rpc_gettransaction,       false },
        { "wallet", "rescanblockchain",     rpc_rescanblockchain,     false },
        { "wallet", "sendmany",             rpc_sendmany,             false },
        { "wallet", "createmultisig",       rpc_createmultisig,       false },
        { "wallet", "addmultisigaddress",   rpc_addmultisigaddress,   false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);

    /* Register shielded and diagnostic sub-controllers */
    register_wallet_shielded_rpc_commands(t);
    register_wallet_diagnostic_rpc_commands(t);
    register_wallet_rescan_rpc_commands(t);
}
