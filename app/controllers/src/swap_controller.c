/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Atomic Swap RPC controller.
 * Cross-chain: ZCL <-> BTC, LTC, DOGE via HTLC. */

#include "platform/time_compat.h"
#include "script/htlc.h"
#include "znam/znam.h"
#include "models/swap_contract.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "views/format_helpers.h"
#include "rpc/server.h"
#include "models/database.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include "util/log_macros.h"

/* ── Context ────────────────────────────────────────────────────── */

static struct node_db *g_swap_ndb = NULL;

void rpc_swap_set_state(struct node_db *ndb)
{
    g_swap_ndb = ndb;
}

/* ── Helpers ────────────────────────────────────────────────────── */

static const char *state_name(enum swap_state s)
{
    switch (s) {
    case SWAP_PENDING:  return "pending";
    case SWAP_FUNDED:   return "funded";
    case SWAP_REDEEMED: return "redeemed";
    case SWAP_REFUNDED: return "refunded";
    case SWAP_EXPIRED:  return "expired";
    default: return "unknown";
    }
}

static void swap_to_json(const struct swap_contract *swap, struct json_value *obj)
{
    json_set_object(obj);
    json_push_kv_str(obj, "swap_id", swap->swap_id);
    json_push_kv_str(obj, "role",
                     swap->role == SWAP_INITIATOR ? "initiator" : "participant");
    json_push_kv_str(obj, "state", state_name(swap->state));

    const struct swap_chain_params *cp = swap_get_chain_params(swap->chain);
    json_push_kv_str(obj, "chain", cp ? cp->ticker : "?");

    char hex[513]; /* large enough for 256-byte redeem script */
    HexStr(swap->secret_hash, 32, false, hex, sizeof(hex));
    json_push_kv_str(obj, "secret_hash", hex);

    if (swap->has_secret) {
        HexStr(swap->secret, 32, false, hex, sizeof(hex));
        json_push_kv_str(obj, "secret", hex);
    }

    double amt = swap->amount / 100000000.0;
    json_push_kv_real(obj, "amount", amt);
    json_push_kv_int(obj, "amount_zatoshi", swap->amount);
    json_push_kv_int(obj, "locktime", swap->locktime);
    json_push_kv_str(obj, "my_address", swap->my_address);
    json_push_kv_str(obj, "counter_address", swap->counter_address);
    json_push_kv_str(obj, "p2sh_address", swap->p2sh_address);

    HexStr(swap->redeem_script, swap->redeem_script_len, false, hex, sizeof(hex));
    json_push_kv_str(obj, "redeem_script_hex", hex);
    json_push_kv_int(obj, "redeem_script_size", (int64_t)swap->redeem_script_len);

    json_push_kv_int(obj, "created_at", swap->created_at);
}

/* ── swap_chains ────────────────────────────────────────────────── */

static bool rpc_swap_chains(const struct json_value *params, bool help,
                            struct json_value *result)
{
    if (help) {
        json_set_str(result, "swap_chains\n\nList supported chains for atomic swaps.\n");
        return true;
    }
    (void)params;

    json_set_array(result);
    for (int i = 0; i <= SWAP_CHAIN_DOGE; i++) {
        const struct swap_chain_params *cp = swap_get_chain_params((enum swap_chain)i);
        struct json_value e = {0};
        json_set_object(&e);
        json_push_kv_str(&e, "name", cp->name);
        json_push_kv_str(&e, "ticker", cp->ticker);
        json_push_back(result, &e);
        json_free(&e);
    }
    return true;
}

/* ── swap_initiate ──────────────────────────────────────────────── */

static bool rpc_swap_initiate(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || !params || json_size(params) < 4) {
        json_set_str(result,
            "swap_initiate \"my_address\" \"counter_address\" amount locktime_blocks [\"chain\"]\n"
            "\nInitiate an atomic swap. Generates secret, builds HTLC contract.\n"
            "\nArguments:\n"
            "1. my_address      (string) Your address (refund path)\n"
            "2. counter_address (string) Counterparty address (claim path)\n"
            "3. amount          (number) Amount in coins\n"
            "4. locktime_blocks (number) Lock duration in blocks\n"
            "5. chain           (string, optional) Chain: zcl, btc, ltc, doge (default: zcl)\n"
            "\nResult: swap contract with secret, secret_hash, P2SH address.\n");
        return true;
    }

    const char *my_addr = json_get_str(json_at(params, 0));
    const char *counter_addr = json_get_str(json_at(params, 1));
    double amount = 0;
    const struct json_value *amt_val = json_at(params, 2);
    if (amt_val) {
        amount = json_get_real(amt_val);
        if (amount == 0.0) amount = (double)json_get_int(amt_val);
    }
    int64_t locktime = json_get_int(json_at(params, 3));

    enum swap_chain chain = SWAP_CHAIN_ZCL;
    if (json_size(params) >= 5) {
        const char *cstr = json_get_str(json_at(params, 4));
        int c = swap_parse_chain(cstr);
        if (c >= 0) chain = (enum swap_chain)c;
    }

    if (!my_addr || !counter_addr || amount <= 0 || locktime <= 0) {
        json_set_str(result, "Invalid arguments");
        return false;
    }

    /* Generate secret */
    uint8_t secret[32], secret_hash[32];
    htlc_generate_secret(secret, secret_hash);

    struct htlc_params hp;
    memset(&hp, 0, sizeof(hp));
    memcpy(hp.secret_hash, secret_hash, 32);
    hp.locktime = (uint32_t)locktime;

    /* Decode real pubkey hashes from addresses */
    if (!htlc_address_to_pkh(counter_addr, chain, hp.recipient_pkh)) {
        json_set_str(result, "Cannot decode counter_address (invalid base58check for this chain)");
        return false;
    }
    if (!htlc_address_to_pkh(my_addr, chain, hp.refunder_pkh)) {
        json_set_str(result, "Cannot decode my_address (invalid base58check for this chain)");
        return false;
    }

    /* Build redeem script */
    uint8_t script[256];
    size_t script_len = htlc_build_script(&hp, script, sizeof(script));
    if (script_len == 0) {
        json_set_str(result, "Failed to build HTLC script");
        return false;
    }

    /* Compute P2SH address */
    char p2sh_addr[64];
    if (!htlc_p2sh_address(script, script_len, chain, p2sh_addr, sizeof(p2sh_addr))) {
        json_set_str(result, "Failed to compute P2SH address");
        return false;
    }

    /* Build swap contract */
    struct swap_contract swap;
    memset(&swap, 0, sizeof(swap));
    swap_compute_id(my_addr, counter_addr, secret_hash, swap.swap_id);
    swap.role = SWAP_INITIATOR;
    swap.state = SWAP_PENDING;
    swap.chain = chain;
    memcpy(swap.secret_hash, secret_hash, 32);
    memcpy(swap.secret, secret, 32);
    swap.has_secret = true;
    swap.amount = (int64_t)(amount * 100000000.0);
    swap.locktime = (uint32_t)locktime;
    snprintf(swap.my_address, sizeof(swap.my_address), "%s", my_addr);
    snprintf(swap.counter_address, sizeof(swap.counter_address), "%s", counter_addr);
    memcpy(swap.redeem_script, script, script_len);
    swap.redeem_script_len = script_len;
    snprintf(swap.p2sh_address, sizeof(swap.p2sh_address), "%s", p2sh_addr);
    swap.created_at = (int64_t)platform_time_wall_time_t();

    /* Persist */
    if (g_swap_ndb)
        db_swap_save(g_swap_ndb, &swap);

    /* Return contract */
    swap_to_json(&swap, result);

    const struct swap_chain_params *cp = swap_get_chain_params(chain);
    printf("swap: initiated %s swap for %.8f %s (locktime %u blocks)\n",
           cp->ticker, amount, cp->ticker, (unsigned)locktime);
    printf("swap: P2SH address: %s\n", p2sh_addr);
    printf("swap: Fund this address to activate the swap.\n");

    return true;
}

/* ── swap_participate ───────────────────────────────────────────── */

static bool rpc_swap_participate(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    if (help || !params || json_size(params) < 5) {
        json_set_str(result,
            "swap_participate \"my_address\" \"counter_address\" amount locktime_blocks \"secret_hash\" [\"chain\"]\n"
            "\nParticipate in an atomic swap (create counter-HTLC).\n"
            "\nArguments:\n"
            "1. my_address      (string) Your address (refund path)\n"
            "2. counter_address (string) Initiator's address (claim path)\n"
            "3. amount          (number) Amount in coins\n"
            "4. locktime_blocks (number) Lock duration (should be SHORTER than initiator)\n"
            "5. secret_hash     (string) 64-char hex secret hash from initiator\n"
            "6. chain           (string, optional) Chain: zcl, btc, ltc, doge (default: zcl)\n");
        return true;
    }

    const char *my_addr = json_get_str(json_at(params, 0));
    const char *counter_addr = json_get_str(json_at(params, 1));
    double amount = 0;
    const struct json_value *amt_val = json_at(params, 2);
    if (amt_val) {
        amount = json_get_real(amt_val);
        if (amount == 0.0) amount = (double)json_get_int(amt_val);
    }
    int64_t locktime = json_get_int(json_at(params, 3));
    const char *hash_hex = json_get_str(json_at(params, 4));

    enum swap_chain chain = SWAP_CHAIN_ZCL;
    if (json_size(params) >= 6) {
        const char *cstr = json_get_str(json_at(params, 5));
        int c = swap_parse_chain(cstr);
        if (c >= 0) chain = (enum swap_chain)c;
    }

    if (!my_addr || !counter_addr || !zcl_is_hex_string(hash_hex, 64)) {
        json_set_str(result, "Invalid arguments");
        return false;
    }

    /* Parse secret hash */
    uint8_t secret_hash[32];
    if (ParseHex(hash_hex, secret_hash, 32) != 32) {
        json_set_str(result, "Invalid secret_hash (64-char hex)");
        return false;
    }

    struct htlc_params hp;
    memset(&hp, 0, sizeof(hp));
    memcpy(hp.secret_hash, secret_hash, 32);
    hp.locktime = (uint32_t)locktime;

    if (!htlc_address_to_pkh(counter_addr, chain, hp.recipient_pkh)) {
        json_set_str(result, "Cannot decode counter_address");
        return false;
    }
    if (!htlc_address_to_pkh(my_addr, chain, hp.refunder_pkh)) {
        json_set_str(result, "Cannot decode my_address");
        return false;
    }

    uint8_t script[256];
    size_t script_len = htlc_build_script(&hp, script, sizeof(script));
    if (script_len == 0) {
        json_set_str(result, "Failed to build HTLC script");
        LOG_FAIL("swap", "swap_participate: htlc_build_script failed");
    }

    char p2sh_addr[64];
    htlc_p2sh_address(script, script_len, chain, p2sh_addr, sizeof(p2sh_addr));

    struct swap_contract swap;
    memset(&swap, 0, sizeof(swap));
    swap_compute_id(my_addr, counter_addr, secret_hash, swap.swap_id);
    swap.role = SWAP_PARTICIPANT;
    swap.state = SWAP_PENDING;
    swap.chain = chain;
    memcpy(swap.secret_hash, secret_hash, 32);
    swap.amount = (int64_t)(amount * 100000000.0);
    swap.locktime = (uint32_t)locktime;
    snprintf(swap.my_address, sizeof(swap.my_address), "%s", my_addr);
    snprintf(swap.counter_address, sizeof(swap.counter_address), "%s", counter_addr);
    memcpy(swap.redeem_script, script, script_len);
    swap.redeem_script_len = script_len;
    snprintf(swap.p2sh_address, sizeof(swap.p2sh_address), "%s", p2sh_addr);
    swap.created_at = (int64_t)platform_time_wall_time_t();

    if (g_swap_ndb)
        db_swap_save(g_swap_ndb, &swap);

    swap_to_json(&swap, result);
    return true;
}

/* ── swap_list ──────────────────────────────────────────────────── */

static bool rpc_swap_list(const struct json_value *params, bool help,
                          struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "swap_list [state]\n"
            "\nList atomic swap contracts.\n"
            "\nArguments:\n"
            "1. state (string, optional) Filter: pending, funded, redeemed, refunded\n");
        return true;
    }

    int state_filter = -1;
    if (params && json_size(params) > 0) {
        const char *s = json_get_str(json_at(params, 0));
        if (s) {
            if (strcmp(s, "pending") == 0) state_filter = SWAP_PENDING;
            else if (strcmp(s, "funded") == 0) state_filter = SWAP_FUNDED;
            else if (strcmp(s, "redeemed") == 0) state_filter = SWAP_REDEEMED;
            else if (strcmp(s, "refunded") == 0) state_filter = SWAP_REFUNDED;
        }
    }

    json_set_array(result);
    if (!g_swap_ndb) return true;

    struct swap_contract swaps[50];
    int count = db_swap_list(g_swap_ndb, swaps, 50, state_filter);

    for (int i = 0; i < count; i++) {
        struct json_value e = {0};
        swap_to_json(&swaps[i], &e);
        json_push_back(result, &e);
        json_free(&e);
    }
    return true;
}

/* ── REST API ───────────────────────────────────────────────────── */

bool api_swap_list(struct json_value *result)
{
    return rpc_swap_list(NULL, false, result);
}

bool api_swap_chains(struct json_value *result)
{
    return rpc_swap_chains(NULL, false, result);
}

/* ── Registration ───────────────────────────────────────────────── */

void register_swap_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "swap", "swap_chains",      rpc_swap_chains,      true },
        { "swap", "swap_initiate",    rpc_swap_initiate,    true },
        { "swap", "swap_participate", rpc_swap_participate, true },
        { "swap", "swap_list",        rpc_swap_list,        true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
