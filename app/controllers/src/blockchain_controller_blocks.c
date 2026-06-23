/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Block accessor RPCs: getblockcount, getbestblockhash, getdifficulty,
 * getblockhash, getblockheader, getblock. Also defines the shared
 * block_header_to_json helper used by the chain controller siblings.
 * (Block difficulty now comes from difficulty_from_index() in chain/pow.h.)
 * See blockchain_controller_internal.h. */

#include "platform/time_compat.h"
#include "controllers/blockchain_controller.h"
#include "blockchain_controller_internal.h"
#include "controllers/strong_params.h"
#include "chain/chain.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "primitives/block.h"
#include "util/log_macros.h"
#include "validation/main_state.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

static struct block_index *rpc_provable_tip(struct blockchain_context *ctx,
                                            int32_t *out_hstar)
{
    int32_t hstar = reducer_frontier_provable_tip_cached();
    if (out_hstar)
        *out_hstar = hstar;
    if (!ctx || !ctx->main_state)
        return NULL;
    return active_chain_at(&ctx->main_state->chain_active, (int)hstar);
}

static bool rpc_block_within_provable_range(const struct block_index *bi,
                                            int32_t *out_hstar)
{
    int32_t hstar = reducer_frontier_provable_tip_cached();
    if (out_hstar)
        *out_hstar = hstar;
    return bi && bi->nHeight >= 0 && bi->nHeight <= hstar;
}

bool rpc_getblockcount(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getblockcount\nReturns the number of blocks.");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblockcount: main_state not initialized");
    }
    /* Report the PROVABLE tip (H*), not the sync-window/lookahead tip: an
     * external getblockcount must never name a height we cannot prove or that
     * rewinds under a reorg. Cached lock-free atomic refreshed once per
     * finalized advance / reorg rewind — see reducer_frontier_provable_tip_cached.
     * Equals the real tip at steady state; LOWER mid-fold / post-reorg. */
    json_set_int(result, reducer_frontier_provable_tip_cached());
    return true;
}

bool rpc_getbestblockhash(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getbestblockhash\nReturns the hash of the best block.");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getbestblockhash: main_state not initialized");
    }
    /* Match getblockcount/getblockchaininfo: the public "best block" is H*,
     * not the active sync-window tip that can run ahead mid-fold. */
    int32_t hstar = -1;
    struct block_index *tip = rpc_provable_tip(ctx, &hstar);
    if (!tip || !tip->phashBlock) {
        json_set_str(result, "No provable tip");
        LOG_FAIL("blockchain",
                 "getbestblockhash: provable tip hstar=%d unresolved",
                 hstar);
    }
    char hex[65];
    uint256_get_hex(tip->phashBlock, hex);
    json_set_str(result, hex);
    return true;
}

/* Bundle the tip identity + timing + work into one call. Power-user
 * convenience: avoids round-tripping getbestblockhash + getblockheader
 * just to check "where am I and how stale am I". */
bool rpc_getchaintip(const struct json_value *params, bool help,
                     struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getchaintip\n"
        "\nReturns the provable chain tip in one shot.\n"
        "Result: { hash, height, time, age_seconds, work, bits, difficulty }");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getchaintip: main_state not initialized");
    }
    int32_t hstar = -1;
    struct block_index *tip = rpc_provable_tip(ctx, &hstar);
    if (!tip || !tip->phashBlock) {
        json_set_str(result, "No provable tip");
        LOG_FAIL("blockchain", "getchaintip: provable tip hstar=%d unresolved",
                 hstar);
    }
    char hex[65];
    uint256_get_hex(tip->phashBlock, hex);
    char work_hex[65];
    arith_uint256_get_hex(&tip->nChainWork, work_hex);

    int64_t now = (int64_t)platform_time_wall_time_t();
    int64_t tip_time = (int64_t)tip->nTime;

    json_set_object(result);
    json_push_kv_str(result, "hash", hex);
    json_push_kv_int(result, "height", tip->nHeight);
    json_push_kv_int(result, "time", tip_time);
    json_push_kv_int(result, "age_seconds", now - tip_time);
    json_push_kv_str(result, "work", work_hex);
    json_push_kv_int(result, "bits", (int64_t)tip->nBits);
    json_push_kv_real(result, "difficulty", difficulty_from_index(tip));
    return true;
}

bool rpc_getdifficulty(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getdifficulty\nReturns proof-of-work difficulty.");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getdifficulty: main_state not initialized");
    }
    int32_t hstar = -1;
    struct block_index *tip = rpc_provable_tip(ctx, &hstar);
    if (!tip) {
        json_set_str(result, "No provable tip");
        LOG_FAIL("blockchain", "getdifficulty: provable tip hstar=%d unresolved",
                 hstar);
    }
    json_set_real(result, difficulty_from_index(tip));
    return true;
}

bool rpc_getblockhash(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result, "getblockhash height\nReturns hash of block at height.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    int height = (int)rpc_require_int(&p, 0, "height");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        LOG_FAIL("blockchain", "getblockhash: invalid params");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblockhash: main_state not initialized");
    }
    int32_t hstar = reducer_frontier_provable_tip_cached();
    if (height < 0 || height > hstar) {
        json_set_str(result, "Block height out of range");
        LOG_FAIL("blockchain",
                 "getblockhash: height %d outside provable range hstar=%d",
                 height, hstar);
    }
    struct block_index *bi = active_chain_at(&ctx->main_state->chain_active, height);
    if (!bi || !bi->phashBlock) {
        json_set_str(result, "Block height out of range");
        LOG_FAIL("blockchain", "getblockhash: height %d out of range", height);
    }
    char hex[65];
    uint256_get_hex(bi->phashBlock, hex);
    json_set_str(result, hex);
    return true;
}

void block_header_to_json(const struct block_index *bi,
                                  struct json_value *result)
{
    json_set_object(result);
    if (!bi || !bi->phashBlock)
        return;

    char hex[65];
    uint256_get_hex(bi->phashBlock, hex);
    json_push_kv_str(result, "hash", hex);

    /* confirmations = 1 + (tip height - this block's height), floored at 0.
     * Default to 1 when main_state isn't available (pre-init). */
    struct blockchain_context *ctx = blockchain_ctx();
    int64_t confirmations = 1;
    if (ctx->main_state) {
        /* Confirmations is an EXTERNAL count relative to the provable tip (H*),
         * matching getblockcount above; a block at/above H* shows 1 (or 0 only
         * if H* hasn't reached it). Cached atomic — never the window tip. */
        int tip_height = reducer_frontier_provable_tip_cached();
        int64_t c = 1 + (int64_t)tip_height - (int64_t)bi->nHeight;
        confirmations = c > 0 ? c : 0;
    }
    json_push_kv_int(result, "confirmations", confirmations);
    json_push_kv_int(result, "height", bi->nHeight);
    json_push_kv_int(result, "version", bi->nVersion);

    uint256_get_hex(&bi->hashMerkleRoot, hex);
    json_push_kv_str(result, "merkleroot", hex);

    json_push_kv_int(result, "time", (int64_t)bi->nTime);
    uint256_get_hex(&bi->nNonce, hex);
    json_push_kv_str(result, "nonce", hex);

    char bits_hex[9];
    snprintf(bits_hex, sizeof(bits_hex), "%08x", bi->nBits);
    json_push_kv_str(result, "bits", bits_hex);

    json_push_kv_real(result, "difficulty", difficulty_from_index(bi));

    if (bi->pprev && bi->pprev->phashBlock) {
        uint256_get_hex(bi->pprev->phashBlock, hex);
        json_push_kv_str(result, "previousblockhash", hex);
    }
}

bool rpc_getblockheader(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
             "getblockheader \"hash\" ( verbose )\nReturns block header.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *hash_str = rpc_require_str(&p, 0, "hash");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        LOG_FAIL("blockchain", "getblockheader: invalid params");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblockheader: main_state not initialized");
    }
    struct uint256 hash;
    uint256_set_hex(&hash, hash_str);

    struct block_index *bi = block_map_find(&ctx->main_state->map_block_index, &hash);
    if (!bi) {
        json_set_str(result, "Block not found");
        LOG_FAIL("blockchain", "getblockheader: block %s not found", hash_str);
    }
    int32_t hstar = -1;
    if (!rpc_block_within_provable_range(bi, &hstar)) {
        json_set_str(result, "Block not found");
        LOG_FAIL("blockchain",
                 "getblockheader: block %s height %d above provable hstar=%d",
                 hash_str, bi->nHeight, hstar);
    }

    block_header_to_json(bi, result);
    return true;
}

bool rpc_getblock(const struct json_value *params, bool help,
                          struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
             "getblock \"hash\" ( verbose )\nReturns block data.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *hash_str = rpc_require_str(&p, 0, "hash");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        LOG_FAIL("blockchain", "getblock: invalid params");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblock: main_state not initialized");
    }
    struct uint256 hash;
    uint256_set_hex(&hash, hash_str);

    struct block_index *bi = block_map_find(&ctx->main_state->map_block_index, &hash);
    if (!bi) {
        json_set_str(result, "Block not found");
        LOG_FAIL("blockchain", "getblock: block %s not found", hash_str);
    }
    int32_t hstar = -1;
    if (!rpc_block_within_provable_range(bi, &hstar)) {
        json_set_str(result, "Block not found");
        LOG_FAIL("blockchain",
                 "getblock: block %s height %d above provable hstar=%d",
                 hash_str, bi->nHeight, hstar);
    }

    block_header_to_json(bi, result);

    json_push_kv_int(result, "size", 0);
    json_push_kv_int(result, "tx", (int64_t)bi->nTx);

    return true;
}
