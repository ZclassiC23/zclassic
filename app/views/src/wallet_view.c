/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "views/wallet_view.h"
#include "views/format_helpers.h"
#include "wallet/wallet.h"
#include "models/chain_snapshot.h"
#include "wallet/keystore.h"
#include "keys/key_io.h"
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "json/json.h"
#include "script/standard.h"
#include "models/wallet_key.h"
#include "encoding/utilstrencodings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

void wallet_view_utxo(struct json_value *out,
                      const struct coin_entry *coin,
                      const struct wallet *w)
{
    (void)w;
    json_set_object(out);

    char txid_hex[65];
    uint256_get_hex(&coin->wtx->tx.hash, txid_hex);
    json_push_kv_str(out, "txid", txid_hex);
    json_push_kv_int(out, "vout", coin->i);

    const struct tx_out *txout = &coin->wtx->tx.vout[coin->i];

    struct tx_destination dest;
    if (script_extract_destination(&txout->script_pub_key, &dest)) {
        const struct chain_params *cp = chain_params_get();
        size_t pk_len = 0, sc_len = 0;
        const unsigned char *pk_pfx =
            chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
        const unsigned char *sc_pfx =
            chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_len);
        char addr[128];
        encode_destination(&dest, pk_pfx, pk_len, sc_pfx, sc_len,
                          addr, sizeof(addr));
        json_push_kv_str(out, "address", addr);
    }

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), txout->value);
    json_push_kv_real(out, "amount", strtod(amt, NULL));
    json_push_kv_int(out, "confirmations", coin->depth);
    json_push_kv_bool(out, "spendable", coin->spendable);
    json_push_kv_bool(out, "solvable", coin->solvable);
}

void wallet_view_tx(struct json_value *out,
                    const struct wallet_tx *wtx,
                    const struct wallet *w)
{
    (void)w;
    json_set_object(out);

    char txid_hex[65];
    uint256_get_hex(&wtx->tx.hash, txid_hex);
    json_push_kv_str(out, "txid", txid_hex);
    json_push_kv_int(out, "confirmations", wtx->confirms);
    json_push_kv_int(out, "time", wtx->time_received);
    json_push_kv_int(out, "timereceived", wtx->time_received);

    if (wtx->from_me)
        json_push_kv_str(out, "category", "send");
    else if (wtx->confirms > 0)
        json_push_kv_str(out, "category", "receive");
    else
        json_push_kv_str(out, "category", "orphan");
}

void wallet_view_info(struct json_value *out,
                      const struct wallet *w,
                      int64_t balance,
                      int64_t unconfirmed)
{
    json_set_object(out);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), balance);
    json_push_kv_real(out, "balance", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), unconfirmed);
    json_push_kv_real(out, "unconfirmed_balance", strtod(amt, NULL));
    json_push_kv_int(out, "txcount", (int64_t)w->num_wallet_tx);
    json_push_kv_int(out, "keypoolsize", (int64_t)w->key_pool_size);
}

void wallet_view_balance(struct json_value *out,
                         int64_t balance,
                         int64_t unconfirmed,
                         int64_t immature)
{
    json_set_object(out);
    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), balance);
    json_push_kv_real(out, "balance", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), unconfirmed);
    json_push_kv_real(out, "unconfirmed", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), immature);
    json_push_kv_real(out, "immature", strtod(amt, NULL));
}

void wallet_view_key_entry(struct json_value *out,
                           const struct db_wallet_key *key,
                           const char *address,
                           int unspent_count,
                           int64_t balance)
{
    (void)key;
    json_set_object(out);
    json_push_kv_str(out, "address", address);

    char pkh[41];
    HexStr(key->pubkey_hash, 20, false, pkh, sizeof(pkh));
    json_push_kv_str(out, "pubkey_hash", pkh);
    json_push_kv_int(out, "unspent_count", unspent_count);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), balance);
    json_push_kv_real(out, "balance", strtod(amt, NULL));
}

void wallet_view_utxo_trace(struct json_value *out,
                            const char *txid_hex, uint32_t vout,
                            const char *status,
                            int64_t value, int height,
                            const char *spent_by,
                            bool in_wallet, bool in_chainstate)
{
    json_set_object(out);
    json_push_kv_str(out, "txid", txid_hex);
    json_push_kv_int(out, "vout", vout);
    json_push_kv_str(out, "status", status);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), value);
    json_push_kv_real(out, "value", strtod(amt, NULL));
    json_push_kv_int(out, "height", height);
    if (spent_by)
        json_push_kv_str(out, "spent_by", spent_by);
    json_push_kv_bool(out, "in_wallet", in_wallet);
    json_push_kv_bool(out, "in_chainstate", in_chainstate);
}

void wallet_view_flow_entry(struct json_value *out,
                            const char *txid_hex,
                            const char *category,
                            int64_t amount, int64_t fee,
                            int height, int64_t running_balance)
{
    json_set_object(out);
    json_push_kv_str(out, "txid", txid_hex);
    json_push_kv_str(out, "category", category);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), amount);
    json_push_kv_real(out, "amount", strtod(amt, NULL));
    if (fee != 0) {
        zcl_format_zcl(amt, sizeof(amt), -fee);
        json_push_kv_real(out, "fee", strtod(amt, NULL));
    }
    json_push_kv_int(out, "height", height);
    zcl_format_zcl(amt, sizeof(amt), running_balance);
    json_push_kv_real(out, "running_balance", strtod(amt, NULL));
}

void wallet_view_reconcile_summary(struct json_value *out,
    int verified, int phantom, int spent_on_chain, int mismatched, int fixed,
    int64_t balance_before, int64_t balance_after)
{
    json_set_object(out);
    json_push_kv_int(out, "verified", verified);
    json_push_kv_int(out, "phantom", phantom);
    json_push_kv_int(out, "spent_on_chain", spent_on_chain);
    json_push_kv_int(out, "value_mismatch", mismatched);
    json_push_kv_int(out, "fixed", fixed);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), balance_before);
    json_push_kv_real(out, "balance_before", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), balance_after);
    json_push_kv_real(out, "balance_after", strtod(amt, NULL));
}

void wallet_view_purge_summary(struct json_value *out,
    int utxos_deleted, int txs_deleted, int64_t amount_purged,
    int64_t balance_before, int64_t balance_after)
{
    json_set_object(out);
    json_push_kv_int(out, "utxos_deleted", utxos_deleted);
    json_push_kv_int(out, "txs_deleted", txs_deleted);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), amount_purged);
    json_push_kv_real(out, "amount_purged", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), balance_before);
    json_push_kv_real(out, "balance_before", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), balance_after);
    json_push_kv_real(out, "balance_after", strtod(amt, NULL));
}

void wallet_view_replay_summary(struct json_value *out,
    int utxos_found, int txs_found,
    int64_t new_balance, int64_t old_balance)
{
    json_set_object(out);
    json_push_kv_int(out, "utxos_found", utxos_found);
    json_push_kv_int(out, "txs_found", txs_found);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), new_balance);
    json_push_kv_real(out, "new_balance", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), old_balance);
    json_push_kv_real(out, "old_balance", strtod(amt, NULL));
}

void wallet_view_sync_summary(struct json_value *out,
    int synced, int already_correct, int marked_spent,
    int64_t balance_before, int64_t balance_after)
{
    json_set_object(out);
    json_push_kv_int(out, "utxos_unmarked_spent", synced);
    json_push_kv_int(out, "already_correct", already_correct);
    json_push_kv_int(out, "marked_spent", marked_spent);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), balance_before);
    json_push_kv_real(out, "balance_before", strtod(amt, NULL));
    zcl_format_zcl(amt, sizeof(amt), balance_after);
    json_push_kv_real(out, "balance_after", strtod(amt, NULL));
}

void wallet_view_legacy_import(struct json_value *out,
                           const struct chain_snapshot *snap,
                           int keys_recovered, int keys_total,
                           int64_t balance)
{
    json_set_object(out);

    struct json_value src_info = {0};
    json_set_object(&src_info);
    json_push_kv_str(&src_info, "directory",
                     snap->src_dir ? snap->src_dir : "");
    json_push_kv_bool(&src_info, "valid", snap->src_valid);
    json_push_kv_int(&src_info, "block_files", snap->src_block_files);

    char sz[32];
    snprintf(sz, sizeof(sz), "%" PRId64, snap->src_blocks_bytes / 1048576);
    json_push_kv_str(&src_info, "blocks_mb", sz);
    snprintf(sz, sizeof(sz), "%" PRId64, snap->src_chainstate_bytes / 1048576);
    json_push_kv_str(&src_info, "chainstate_mb", sz);
    json_push_kv(out, "source", &src_info);
    json_free(&src_info);

    struct json_value copy_info = {0};
    json_set_object(&copy_info);
    json_push_kv_bool(&copy_info, "blocks", snap->copy_blocks_ok);
    json_push_kv_bool(&copy_info, "index", snap->copy_index_ok);
    json_push_kv_bool(&copy_info, "chainstate", snap->copy_chainstate_ok);
    json_push_kv(out, "copy_status", &copy_info);
    json_free(&copy_info);

    struct json_value wallet_info = {0};
    json_set_object(&wallet_info);
    json_push_kv_int(&wallet_info, "keys_recovered", keys_recovered);
    json_push_kv_int(&wallet_info, "keys_total", keys_total);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), balance);
    json_push_kv_str(&wallet_info, "balance", amt);
    json_push_kv(out, "wallet", &wallet_info);
    json_free(&wallet_info);
}

void wallet_view_chain_coin(struct json_value *out,
                            uint32_t vout, int64_t value,
                            bool available, const char *address,
                            bool in_wallet)
{
    json_set_object(out);
    json_push_kv_int(out, "vout", vout);

    char amt[32];
    zcl_format_zcl(amt, sizeof(amt), value);
    json_push_kv_real(out, "value", strtod(amt, NULL));
    json_push_kv_str(out, "status", available ? "unspent" : "spent");
    if (address)
        json_push_kv_str(out, "address", address);
    json_push_kv_bool(out, "in_wallet", in_wallet);
}

/* ── HTML view render functions ─────────────────────────────── */

size_t wv_render_pulse(uint8_t *buf, size_t max, const struct wv_pulse *d) {
    return (size_t)snprintf((char *)buf, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"height\":%d,\"balance\":%" PRId64 ",\"shielded\":%" PRId64
        ",\"speed_balance\":%" PRId64
        ",\"t_utxos\":%d,\"z_notes\":%d"
        ",\"peers\":%d,\"sync\":\"%s\",\"mempool\":%d}",
        d->height, d->balance, d->shielded, d->speed_balance,
        d->t_utxos, d->z_notes,
        d->peers, d->sync, d->mempool);
}
