/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Blockchain controller — public surface, shared context, MMR/MMB
 * commitment globals, RPC command registration. The route handlers live
 * in the sibling files:
 *
 *   blockchain_controller_blocks.c       — block accessor RPCs
 *   blockchain_controller_chain.c        — chain info + commitments
 *   blockchain_controller_admin.c        — reindex / import
 *
 * See blockchain_controller_internal.h for cross-sibling declarations. */

#include "platform/time_compat.h"
#include "controllers/blockchain_controller.h"
#include "blockchain_controller_internal.h"
#include "chain/chain.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "core/uint256.h"
#include "json/json.h"
#include "models/database.h"
#include "primitives/block.h"
#include "util/log_macros.h"
#include "validation/main_state.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Shared controller context ──────────────────────────── */

static struct blockchain_context g_blockchain_ctx = {0};

struct blockchain_context *blockchain_ctx(void)
{
    return &g_blockchain_ctx;
}

void rpc_blockchain_set_state(struct main_state *ms, struct tx_mempool *mp,
                               const char *datadir)
{
    g_blockchain_ctx.main_state = ms;
    g_blockchain_ctx.mempool = mp;
    g_blockchain_ctx.datadir = datadir;
}

void rpc_blockchain_set_coins_db(struct coins_view_db *cvdb,
                                  struct coins_view_cache *coins_tip)
{
    g_blockchain_ctx.coins_db = cvdb;
    g_blockchain_ctx.coins_tip = coins_tip;
}

void rpc_blockchain_set_node_db(struct node_db *ndb)
{
    g_blockchain_ctx.node_db = ndb;
}

/* ── Global MMR ────────────────────────────────────────── */

static struct mmr g_mmr;
static bool g_mmr_initialized = false;

void rpc_blockchain_mmr_append(const uint8_t block_hash[32])
{
    if (!g_mmr_initialized) {
        mmr_init(&g_mmr);
        g_mmr_initialized = true;
    }
    mmr_append(&g_mmr, block_hash);
}

struct mmr *rpc_blockchain_get_mmr(void) { return &g_mmr; }

bool rpc_blockchain_mmr_initialized(void) { return g_mmr_initialized; }

void rpc_blockchain_mmr_init_from_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;

    uint8_t buf[MMR_SERIALIZED_MAX];
    size_t len = 0;
    if (node_db_state_get(ndb, "mmr_state", buf, sizeof(buf), &len) &&
        len >= 12 && mmr_deserialize(&g_mmr, buf, len)) {
        g_mmr_initialized = true;
    }
    if (!g_mmr_initialized) {
        mmr_init(&g_mmr);
        g_mmr_initialized = true;
    }
}

void rpc_blockchain_mmr_catchup(struct main_state *ms)
{
    if (!g_mmr_initialized || !ms) return;
    int chain_height = active_chain_height(&ms->chain_active);
    int mmr_height = (int)g_mmr.num_leaves - 1;

    if (mmr_height >= chain_height) return;

    int start = mmr_height + 1;
    int64_t t0 = (int64_t)platform_time_wall_time_t();
    for (int h = start; h <= chain_height; h++) {
        const struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (bi && bi->phashBlock)
            mmr_append(&g_mmr, bi->phashBlock->data);
    }
    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("MMR catchup: %d → %d (%d blocks, %llds)\n",
           start, chain_height, chain_height - start + 1, (long long)elapsed);
}

void rpc_blockchain_mmr_save(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !g_mmr_initialized) return;
    uint8_t buf[MMR_SERIALIZED_MAX];
    size_t len = mmr_serialize(&g_mmr, buf, sizeof(buf));
    if (len == 0) return;
    if (!node_db_state_set(ndb, "mmr_state", buf, len))
        LOG_WARN("blockchain", "MMR state save failed");
}

/* ── Global MMB (Merkle Mountain Belt) ────────────────── */

static struct mmb g_mmb;
static bool g_mmb_initialized = false;

void rpc_blockchain_mmb_append(const struct mmb_leaf *leaf)
{
    if (!g_mmb_initialized) {
        mmb_init(&g_mmb);
        g_mmb_initialized = true;
    }
    mmb_append(&g_mmb, leaf);
}

struct mmb *rpc_blockchain_get_mmb(void) { return &g_mmb; }

void rpc_blockchain_mmb_init_from_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;

    uint8_t buf[MMB_SERIALIZED_MAX];
    size_t len = 0;
    if (node_db_state_get(ndb, "mmb_state", buf, sizeof(buf), &len) &&
        len >= 13 && mmb_deserialize(&g_mmb, buf, len)) {
        g_mmb_initialized = true;
    }
    if (!g_mmb_initialized) {
        mmb_init(&g_mmb);
        g_mmb_initialized = true;
    }
    printf("MMB: loaded %llu leaves, %u peaks\n",
           (unsigned long long)g_mmb.num_leaves, g_mmb.num_mountains);
}

void rpc_blockchain_mmb_catchup(struct main_state *ms)
{
    if (!g_mmb_initialized || !ms) return;
    int chain_height = active_chain_height(&ms->chain_active);
    int mmb_height = (int)g_mmb.num_leaves - 1;

    if (mmb_height >= chain_height) return;

    int start = mmb_height + 1;
    int64_t t0 = (int64_t)platform_time_wall_time_t();
    for (int h = start; h <= chain_height; h++) {
        const struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (bi && bi->phashBlock) {
            struct mmb_leaf leaf;
            mmb_leaf_from_block(&leaf,
                bi->phashBlock->data,
                bi->nHeight, bi->nTime, bi->nBits,
                bi->hashFinalSaplingRoot.data,
                (const uint8_t *)bi->nChainWork.pn);
            mmb_append(&g_mmb, &leaf);
        }
    }
    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("MMB catchup: %d → %d (%d blocks, %llds, %u peaks)\n",
           start, chain_height, chain_height - start + 1,
           (long long)elapsed, g_mmb.num_mountains);
}

void rpc_blockchain_mmb_save(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !g_mmb_initialized) return;
    uint8_t buf[MMB_SERIALIZED_MAX];
    size_t len = mmb_serialize(&g_mmb, buf, sizeof(buf));
    if (len == 0) return;
    if (!node_db_state_set(ndb, "mmb_state", buf, len))
        LOG_WARN("blockchain", "MMB state save failed");
}

/* ── Commitment MMR (UTXO state binding) ─────────────── */

static struct mmr g_commitment_mmr;
static bool g_commitment_mmr_initialized = false;

struct mmr *rpc_blockchain_get_commitment_mmr(void)
{
    if (!g_commitment_mmr_initialized) {
        mmr_init(&g_commitment_mmr);
        g_commitment_mmr_initialized = true;
    }
    return &g_commitment_mmr;
}

void rpc_blockchain_commitment_mmr_init_from_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;

    uint8_t buf[MMR_SERIALIZED_MAX];
    size_t len = 0;
    if (node_db_state_get(ndb, "commitment_mmr_state", buf, sizeof(buf),
                          &len)) {
        if (len >= 12 && mmr_deserialize(&g_commitment_mmr, buf, len)) {
            g_commitment_mmr_initialized = true;
            printf("Commitment MMR loaded: %llu leaves\n",
                   (unsigned long long)g_commitment_mmr.num_leaves);
        }
    }
    if (!g_commitment_mmr_initialized) {
        mmr_init(&g_commitment_mmr);
        g_commitment_mmr_initialized = true;
    }
}

void rpc_blockchain_commitment_mmr_save(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !g_commitment_mmr_initialized) return;
    uint8_t buf[MMR_SERIALIZED_MAX];
    size_t len = mmr_serialize(&g_commitment_mmr, buf, sizeof(buf));
    if (len == 0) return;
    if (!node_db_state_set(ndb, "commitment_mmr_state", buf, len))
        LOG_WARN("blockchain", "commitment MMR state save failed");
}

void rpc_blockchain_maybe_commit(int32_t height,
                                  const uint8_t block_hash[32],
                                  const uint8_t xor_accumulator[32],
                                  uint64_t utxo_count)
{
    if (height <= 0 || height % MMR_COMMITMENT_INTERVAL != 0)
        return;

    /* Skip commitment during deferred proof validation IBD — the MMR will be built
     * from scratch once we pass deferred proof validation height. */
    extern _Atomic int g_deferred_proof_validation_below_height;
    if (g_deferred_proof_validation_below_height >= 0 && height <= g_deferred_proof_validation_below_height)
        return;

    if (!g_commitment_mmr_initialized) {
        mmr_init(&g_commitment_mmr);
        g_commitment_mmr_initialized = true;
    }

    /* Use the incremental XOR accumulator (O(1)) instead of the O(N)
     * SHA3 full-table scan that was killing sync performance. */
    struct mmr_commitment c = { .height = height };
    memcpy(c.block_hash, block_hash, 32);
    memcpy(c.utxo_root, xor_accumulator, 32);
    memset(c.data_root, 0, 32);

    mmr_append_commitment(&g_commitment_mmr, &c);

    if (height % 10000 == 0) {
        uint8_t root[32];
        mmr_root(&g_commitment_mmr, root);
        printf("MMR commitment: h=%d utxos=%llu root=",
               height, (unsigned long long)utxo_count);
        for (int i = 0; i < 8; i++) printf("%02x", root[i]);
        printf("... (%u leaves)\n",
               (unsigned)g_commitment_mmr.num_leaves);
    }
}

/* ── SHA3 UTXO commitment RPC ──────────────────────────── */

/* ── RPC command registration ─────────────────────────── */

void register_blockchain_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "blockchain", "getblockcount",     rpc_getblockcount,     true },
        { "blockchain", "getbestblockhash",  rpc_getbestblockhash,  true },
        { "blockchain", "getchaintip",       rpc_getchaintip,       true },
        { "blockchain", "getdifficulty",     rpc_getdifficulty,     true },
        { "blockchain", "getblockhash",      rpc_getblockhash,      true },
        { "blockchain", "getblockheader",    rpc_getblockheader,    true },
        { "blockchain", "getblock",          rpc_getblock,          true },
        { "blockchain", "getblockchaininfo", rpc_getblockchaininfo, true },
        { "blockchain", "getmempoolinfo",    rpc_getmempoolinfo,    true },
        { "blockchain", "getmempoolfeestats", rpc_getmempoolfeestats, true },
        { "blockchain", "gettxoutsetinfo",      rpc_gettxoutsetinfo,      true },
        /* HODL wave commands in hodl_controller.c */
        { "blockchain", "reindexchainstate",    rpc_reindexchainstate,     false },
        { "blockchain", "importchainstate",     rpc_importchainstate,       false },
        { "blockchain", "invalidateblock",      rpc_invalidateblock,       false },
        { "blockchain", "reconsiderblock",      rpc_reconsiderblock,       false },
        { "blockchain", "getutxocommitment",   rpc_getutxocommitment,     true },
        { "blockchain", "getutxoaudit",        rpc_getutxoaudit,          true },
        { "blockchain", "getmmrroot",          rpc_getmmrroot,            true },
        { "blockchain", "getcommitmentmmr",   rpc_getcommitmentmmr,     true },
        { "blockchain", "auditchain",          rpc_auditchain,            true },
        { "blockchain", "verifycheckpoint",    rpc_verifycheckpoint,      true },
        { "blockchain", "getdataintegrity",    rpc_getdataintegrity,      true },
        { "blockchain", "rebuildsaplingtree", rpc_rebuildsaplingtree,    false },
        /* Long-poll waitfor* (additive, read-only; ok_safe_mode=true) */
        { "blockchain", "waitforheight",       rpc_waitforheight,         true },
        { "blockchain", "waitforhalt",         rpc_waitforhalt,           true },
        { "blockchain", "waitforblocker",      rpc_waitforblocker,        true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
