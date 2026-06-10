/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "views/format_helpers.h"
#include "controllers/mining_controller.h"
#include "controllers/strong_params.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "mining/miner.h"
#include "primitives/block.h"
#include "script/script.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "services/chain_activation_service.h"
#include "chain/subsidy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/util.h"  /* LogPrintf */

struct mining_context {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    struct coins_view_cache *coins_tip;
};

static struct mining_context g_mining_ctx = {0};

static struct mining_context *mining_ctx(void)
{
    return &g_mining_ctx;
}

static bool mining_submit_mined_block(struct block *block)
{
    struct validation_state state;
    validation_state_init(&state);
    bool ok = reducer_ingest_block(boot_activation_controller(), block,
                                   REDUCER_SRC_MINED, true, &state);
    if (!ok) {
        /* Surface WHY the reducer rejected a locally-mined block. Without
         * this, `generate` returns an empty result array with no clue why
         * the tip didn't advance (the validation_state was dropped on the
         * floor). Log the reject reason + the block hash for the operator. */
        char msg[MAX_REJECT_REASON + 64];
        format_state_message(&state, msg, sizeof(msg));
        struct uint256 h;
        block_get_hash(block, &h);
        char hex[65];
        uint256_get_hex(&h, hex);
        LogPrintf("[mining] submit of mined block %s REJECTED by reducer: %s\n",
                  hex, msg[0] ? msg : "(no reason set)");
    }
    return ok;
}

void rpc_mining_set_state(struct main_state *ms, struct tx_mempool *mp,
                           struct coins_view_cache *coins_tip)
{
    struct mining_context *ctx = mining_ctx();
    ctx->main_state = ms;
    ctx->mempool = mp;
    ctx->coins_tip = coins_tip;
}

static bool rpc_getmininginfo(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct mining_context *ctx = mining_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getmininginfo\n"
        "Returns mining-related information.");

    if (!ctx->main_state) {
        json_set_str(result, "Mining state not initialized");
        LOG_FAIL("mining", "getmininginfo: ctx->main_state is NULL");
    }

    const struct chain_params *cp = chain_params_get();
    struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);

    json_set_object(result);
    json_push_kv_int(result, "blocks", tip ? tip->nHeight : 0);
    json_push_kv_int(result, "currentblocksize",
                      (int64_t)ctx->main_state->nLastBlockSize);
    json_push_kv_int(result, "currentblocktx",
                      (int64_t)ctx->main_state->nLastBlockTx);

    double difficulty = 0.0;
    if (tip) {
        int shift = (tip->nBits >> 24) & 0xff;
        double diff = (double)(0x0000ff & (tip->nBits >> 16));
        while (shift < 29) { diff *= 256.0; shift++; }
        while (shift > 29) { diff /= 256.0; shift--; }
        if (diff != 0.0)
            difficulty = (double)0x00ffff / diff;
    }
    json_push_kv_real(result, "difficulty", difficulty);

    json_push_kv_str(result, "chain", cp->strNetworkID);
    json_push_kv_bool(result, "generate", false);

    return true;
}

static bool rpc_generate(const struct json_value *params, bool help,
                          struct json_value *result)
{
    struct mining_context *ctx = mining_ctx();
    RPC_HELP(help, result,
        "generate numblocks\n"
        "Mine blocks immediately (regtest only).\n"
        "Arguments:\n"
        "1. numblocks (numeric, required) How many blocks to generate");

    struct rpc_params p;
    rpc_params_init(&p, params);
    int64_t num_blocks = rpc_require_int(&p, 0, "numblocks");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }
    if (num_blocks <= 0 || num_blocks > 1000) {
        json_set_str(result, "Invalid number of blocks");
        return false;
    }

    const struct chain_params *cp = chain_params_get();

    /* generate is an on-demand miner for regtest. On mainnet/testnet the
     * Equihash parameters make an in-process solve impractical (and mining
     * goes through real workers/peers), so we refuse — matching zcashd's
     * "regtest mode only" contract via fMineBlocksOnDemand. */
    if (!cp->fMineBlocksOnDemand) {
        json_set_str(result,
                     "Error: generate is for regtest only "
                     "(this network is not mine-blocks-on-demand)");
        return false;
    }

    struct script coinbase_script;
    coinbase_script.size = 0;

    json_set_array(result);

    for (int64_t i = 0; i < num_blocks; i++) {
        struct block_template *tmpl = create_new_block(
            &coinbase_script, ctx->main_state, ctx->coins_tip, ctx->mempool, cp);
        if (!tmpl) break;

        struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);
        unsigned int extra_nonce = 0;
        increment_extra_nonce(&tmpl->block, tip, &extra_nonce);

        /* Solve the Equihash PoW so the block passes the reducer's
         * stateless check_block(check_pow=true) gate. Without this the
         * block carries an empty solution and is rejected at intake, so
         * the tip never advances. Fast for regtest/testnet (small N,K). */
        int new_height = (tip ? tip->nHeight : 0) + 1;
        if (!mine_block_pow(&tmpl->block, new_height, cp, 0)) {
            block_template_free(tmpl);
            free(tmpl);
            break;
        }

        if (mining_submit_mined_block(&tmpl->block)) {
            struct uint256 hash;
            block_get_hash(&tmpl->block, &hash);
            char hex[65];
            uint256_get_hex(&hash, hex);
            struct json_value v = {0};
            json_set_str(&v, hex);
            json_push_back(result, &v);
            json_free(&v);
        }

        block_template_free(tmpl);
        free(tmpl);
    }

    return true;
}

static bool rpc_submitblock(const struct json_value *params, bool help,
                              struct json_value *result)
{
    RPC_HELP(help, result,
        "submitblock \"hexdata\"\n"
        "Attempts to submit new block to network.\n"
        "Arguments:\n"
        "1. \"hexdata\" (string, required) The hex-encoded block data");

    struct rpc_params p;
    rpc_params_init(&p, params);
    const char *hex = rpc_require_str(&p, 0, "hexdata");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }
    size_t hex_len = strlen(hex);
    size_t bin_len = hex_len / 2;
    unsigned char *bin = zcl_malloc(bin_len, "submitblock_bin");
    if (!bin) LOG_FAIL("mining", "malloc failed for submitblock hex decode (%zu bytes)", bin_len);

    size_t parsed = ParseHex(hex, bin, bin_len);
    if (parsed == 0) {
        free(bin);
        json_set_str(result, "Block decode failed");
        return false;
    }

    struct byte_stream s;
    stream_init_from_data(&s, bin, parsed);

    struct block blk;
    block_init(&blk);
    if (!block_deserialize(&blk, &s)) {
        block_free(&blk);
        stream_free(&s);
        free(bin);
        json_set_str(result, "Block decode failed");
        return false;
    }
    stream_free(&s);
    free(bin);

    struct validation_state state;
    validation_state_init(&state);

    /* submitblock intake: the synchronous reducer_ingest_block drives the
     * staged Job pipeline and fills the validation_state. force=true mirrors
     * the locally-requested relay-pre-filter-skipping semantics submitblock
     * already had. The verdict in `state` flows into format_state_message
     * below, so the RPC still returns null on accept / the reject reason on
     * reject. */
    bool ok = reducer_ingest_block(boot_activation_controller(), &blk,
                                   REDUCER_SRC_SUBMIT, true, &state);
    block_free(&blk);

    if (!ok) {
        char msg[512];
        format_state_message(&state, msg, sizeof(msg));
        if (msg[0])
            json_set_str(result, msg);
        else
            json_set_str(result, "rejected");
        return false;
    }

    json_set_null(result);
    return true;
}

static bool rpc_getblocktemplate(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct mining_context *ctx = mining_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getblocktemplate ( \"jsonrequestobject\" )\n"
        "Returns data needed to construct a block to work on.");

    const struct chain_params *cp = chain_params_get();
    struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);
    if (!tip) {
        json_set_str(result, "No tip available");
        return false;
    }

    struct script coinbase_script;
    coinbase_script.size = 0;

    struct block_template *tmpl = create_new_block(
        &coinbase_script, ctx->main_state, ctx->coins_tip, ctx->mempool, cp);
    if (!tmpl) {
        json_set_str(result, "Could not create block template");
        return false;
    }

    json_set_object(result);

    json_push_kv_int(result, "version", tmpl->block.header.nVersion);

    char prev_hex[65];
    uint256_get_hex(&tmpl->block.header.hashPrevBlock, prev_hex);
    json_push_kv_str(result, "previousblockhash", prev_hex);

    /* Transactions (skip coinbase at index 0) */
    struct json_value txs = {0};
    json_set_array(&txs);
    for (size_t i = 1; i < tmpl->block.num_vtx; i++) {
        struct json_value txobj = {0};
        json_set_object(&txobj);

        char *hex = zcl_malloc(2 * 1024 * 1024, "template_tx_hex");
        if (hex) {
            size_t hlen = encode_hex_tx(&tmpl->block.vtx[i], hex,
                                        2 * 1024 * 1024);
            hex[hlen] = '\0';
            json_push_kv_str(&txobj, "data", hex);
            free(hex);
        }

        transaction_compute_hash(&tmpl->block.vtx[i]);
        char txid[65];
        uint256_get_hex(&tmpl->block.vtx[i].hash, txid);
        json_push_kv_str(&txobj, "hash", txid);

        if (tmpl->tx_fees)
            json_push_kv_int(&txobj, "fee", tmpl->tx_fees[i]);
        if (tmpl->tx_sig_ops)
            json_push_kv_int(&txobj, "sigops",
                             (int64_t)tmpl->tx_sig_ops[i]);

        json_push_back(&txs, &txobj);
        json_free(&txobj);
    }
    json_push_kv(result, "transactions", &txs);
    json_free(&txs);

    /* Coinbase */
    struct json_value coinbase_obj = {0};
    json_set_object(&coinbase_obj);
    char *cb_hex = zcl_malloc(2 * 1024 * 1024, "template_cb_hex");
    if (cb_hex && tmpl->block.num_vtx > 0) {
        size_t hlen = encode_hex_tx(&tmpl->block.vtx[0], cb_hex,
                                    2 * 1024 * 1024);
        cb_hex[hlen] = '\0';
        json_push_kv_str(&coinbase_obj, "data", cb_hex);
    }
    free(cb_hex);
    json_push_kv(result, "coinbasetxn", &coinbase_obj);
    json_free(&coinbase_obj);

    /* Target and bits */
    char bits_str[16];
    snprintf(bits_str, sizeof(bits_str), "%08x", tmpl->block.header.nBits);
    json_push_kv_str(result, "bits", bits_str);

    json_push_kv_int(result, "height", tip->nHeight + 1);
    json_push_kv_int(result, "curtime", (int64_t)tmpl->block.header.nTime);
    json_push_kv_int(result, "mintime",
                     block_index_get_median_time_past(tip) + 1);
    json_push_kv_int(result, "sizelimit", MAX_BLOCK_SIZE);
    json_push_kv_int(result, "sigoplimit", 20000);

    char finalsapling[65];
    uint256_get_hex(&tmpl->block.header.hashFinalSaplingRoot, finalsapling);
    json_push_kv_str(result, "finalsaplingroothash", finalsapling);

    block_template_free(tmpl);
    free(tmpl);
    return true;
}

static bool rpc_getblocksubsidy(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct mining_context *ctx = mining_ctx();
    RPC_HELP(help, result,
        "getblocksubsidy height\n"
        "Returns block subsidy reward of block at given height.");

    struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);
    int default_height = tip ? tip->nHeight : 0;

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    int height = (int)rpc_permit_int(&p, 0, "height", default_height);
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }

    const struct chain_params *cp = chain_params_get();

    int64_t subsidy = get_block_subsidy(height, &cp->consensus);

    json_set_object(result);
    json_push_kv_real(result, "miner", (double)subsidy / (double)ZATOSHI_PER_ZCL);

    return true;
}

void register_mining_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "mining", "getmininginfo",     rpc_getmininginfo,    true },
        { "mining", "generate",          rpc_generate,         true },
        { "mining", "submitblock",       rpc_submitblock,      true },
        { "mining", "getblocktemplate",  rpc_getblocktemplate, true },
        { "mining", "getblocksubsidy",   rpc_getblocksubsidy,  true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
