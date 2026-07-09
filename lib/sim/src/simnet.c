/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * simnet — deterministic, RAM-only single-node chain harness.
 * See lib/sim/include/sim/simnet.h for the design contract.
 *
 * This file ASSEMBLES inputs and ASSERTS outputs against the real
 * consensus code (connect_block). It never modifies a consensus predicate:
 * a rejected block means the harness built the wrong block, not that the
 * validator is wrong.
 */

#include "sim/simnet.h"

#include "validation/connect_block.h"
#include "validation/contextual_check_tx.h"
#include "consensus/validation.h"
#include "consensus/consensus.h"          /* COINBASE_MATURITY */
#include "coins/coins.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "bloom/merkle.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "script/script.h"
#include "util/safe_alloc.h"
#include "util/log_macros.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* First mintable height. Low enough that Sapling is inactive (so the
 * all-zero hashFinalSaplingRoot check is skipped) and the subsidy dwarfs
 * our tiny coinbase outputs. */
#define SIM_BASE_HEIGHT 100

/* A single checkpoint at this height "covers" every height we mint, so
 * connect_block runs with expensive_checks=false (PoW + parallel scripts
 * skipped) — the same mechanism as test_connect_block_self_write.c. */
#define SIM_CHECKPOINT_HEIGHT 1000000

/* Value carried by every synthetic coinbase output. Well under any subsidy
 * so the "bad-cb-amount" reward check always passes. */
#define SIM_COINBASE_VALUE 1000000

/* ── Transaction builders (adapted from
 *    lib/test/src/test_connect_block_self_write.c) ─────────────────── */

/* Coinbase whose scriptSig encodes `height`, giving each coinbase a unique
 * txid (so BIP30 never trips). One output of SIM_COINBASE_VALUE. */
static struct transaction sim_make_coinbase(int height)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "simnet_cb_vin");
    uint8_t sig[6] = { 4,
                       (uint8_t)(height & 0xFF),
                       (uint8_t)((height >> 8) & 0xFF),
                       (uint8_t)((height >> 16) & 0xFF),
                       (uint8_t)((height >> 24) & 0xFF),
                       0x11 };
    script_set(&tx.vin[0].script_sig, sig, sizeof(sig));
    uint256_set_null(&tx.vin[0].prevout.hash);
    tx.vin[0].prevout.n = 0xFFFFFFFF;
    tx.vin[0].sequence = 0xFFFFFFFF;
    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "simnet_cb_vout");
    tx.vout[0].value = SIM_COINBASE_VALUE;
    uint8_t pk[] = {0x76, 0xa9, 0x14};
    script_set(&tx.vout[0].script_pub_key, pk, sizeof(pk));
    transaction_compute_hash(&tx);
    return tx;
}

/* Transparent spend of `in_txid`:`in_n` producing one output of
 * `out_value`. expensive_checks=false skips script verification, so a
 * placeholder scriptSig suffices. */
static struct transaction sim_make_spend(const struct uint256 *in_txid,
                                         uint32_t in_n, int64_t out_value)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "simnet_spend_vin");
    tx.vin[0].prevout.hash = *in_txid;
    tx.vin[0].prevout.n = in_n;
    uint8_t sig[] = {0x00, 0x00};
    script_set(&tx.vin[0].script_sig, sig, sizeof(sig));
    tx.vin[0].sequence = 0xFFFFFFFF;
    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "simnet_spend_vout");
    tx.vout[0].value = out_value;
    uint8_t pk[] = {0x76, 0xa9, 0x14};
    script_set(&tx.vout[0].script_pub_key, pk, sizeof(pk));
    transaction_compute_hash(&tx);
    return tx;
}

static void sim_free_tx(struct transaction *tx)
{
    free(tx->vin);
    free(tx->vout);
    tx->vin = NULL;
    tx->vout = NULL;
}

/* Assemble `txs[0..ntx)` into a block at `height`, drive it through the
 * REAL connect_block, and on success advance the in-RAM tip. Frees each
 * tx's vin/vout (and the block's vtx copy array) on every path — the caller
 * hands ownership of the built txs to this helper. */
static bool sim_mint_block(struct simnet *s, struct transaction *txs,
                           size_t ntx, int height)
{
    if (!s || !s->initialized || ntx == 0)
        LOG_FAIL("simnet", "invalid mint request (ntx=%zu)", ntx);

    struct uint256 *txids = zcl_malloc(ntx * sizeof(*txids), "simnet_txids");
    if (!txids) {
        for (size_t i = 0; i < ntx; i++) sim_free_tx(&txs[i]);
        LOG_FAIL("simnet", "OOM allocating %zu txids", ntx);
    }

    struct block blk;
    memset(&blk, 0, sizeof(blk));
    blk.num_vtx = ntx;
    blk.vtx = zcl_calloc(ntx, sizeof(struct transaction), "simnet_blk_vtx");
    if (!blk.vtx) {
        free(txids);
        for (size_t i = 0; i < ntx; i++) sim_free_tx(&txs[i]);
        LOG_FAIL("simnet", "OOM allocating %zu vtx", ntx);
    }
    for (size_t i = 0; i < ntx; i++) {
        blk.vtx[i] = txs[i];
        txids[i] = txs[i].hash;
    }

    blk.header.nVersion = 4;
    blk.header.hashPrevBlock = s->tip.hashBlock;
    blk.header.hashMerkleRoot = compute_merkle_root(txids, ntx);
    free(txids);

    struct uint256 block_hash;
    block_header_get_hash(&blk.header, &block_hash);

    struct block_index pindex;
    block_index_init(&pindex);
    pindex.nHeight = height;
    pindex.phashBlock = &block_hash;
    pindex.pprev = &s->tip;

    struct validation_state vs;
    validation_state_init(&vs);

    bool ok = connect_block(&blk, &vs, &pindex, &s->view, &s->params,
                            false /* just_check */);

    free(blk.vtx);
    for (size_t i = 0; i < ntx; i++)
        sim_free_tx(&txs[i]);

    if (!ok)
        LOG_FAIL("simnet", "connect_block rejected height %d: %s",
                 height, vs.reject_reason);

    /* Advance the tip in RAM. tip.phashBlock keeps pointing at
     * tip.hashBlock (a fixed address), so links stay valid across mints. */
    s->tip.hashBlock = block_hash;
    s->tip.phashBlock = &s->tip.hashBlock;
    s->tip.nHeight = height;
    s->tip.pprev = NULL;
    s->tip.has_chain_sprout_value = false;
    s->tip.has_chain_sapling_value = false;
    arith_uint256_set_u64(&s->tip.nChainWork, (uint64_t)height + 1);
    s->tip_height = height;
    return true;
}

bool simnet_init(struct simnet *s)
{
    if (!s)
        LOG_FAIL("simnet", "NULL simnet");

    memset(s, 0, sizeof(*s));

    /* Keep the BIP30 self-write branch live and deterministic (no deferred
     * proof window) — mirrors the connect_block test harness. */
    atomic_store(&g_deferred_proof_validation_below_height, -1);

    /* Value-copy the chain params, then install a single checkpoint that
     * covers every height we mint (expensive_checks=false). */
    s->params = *chain_params_get();
    memset(&s->cpentry, 0, sizeof(s->cpentry));
    s->cpentry.height = SIM_CHECKPOINT_HEIGHT;
    memset(s->cpentry.hash.data, 0x01, 32);
    s->params.checkpointData.entries = &s->cpentry;
    s->params.checkpointData.nEntries = 1;

    /* Synthetic base tip, one below the first mintable height. */
    block_index_init(&s->tip);
    s->tip.nHeight = SIM_BASE_HEIGHT - 1;
    memset(s->tip.hashBlock.data, 0x33, 32);
    s->tip.phashBlock = &s->tip.hashBlock;
    s->tip.pprev = NULL;
    s->tip.has_chain_sprout_value = false;
    s->tip.has_chain_sapling_value = false;
    arith_uint256_set_u64(&s->tip.nChainWork, (uint64_t)SIM_BASE_HEIGHT);
    s->tip_height = SIM_BASE_HEIGHT - 1;

    /* The live UTXO set: an empty coins cache over a zeroed backing view.
     * Its best block is the synthetic base tip (view/prevblock invariant). */
    memset(&s->null_view, 0, sizeof(s->null_view));
    coins_view_cache_init(&s->view, &s->null_view);
    coins_view_cache_set_best_block(&s->view, &s->tip.hashBlock);

    s->initialized = true;
    return true;
}

void simnet_free(struct simnet *s)
{
    if (!s || !s->initialized)
        return;
    coins_view_cache_free(&s->view);
    s->initialized = false;
}

bool simnet_mint_coinbase(struct simnet *s, struct uint256 *out_cb_txid)
{
    if (!s || !s->initialized)
        LOG_FAIL("simnet", "uninitialized simnet");

    int height = s->tip_height + 1;
    struct transaction cb = sim_make_coinbase(height);
    struct uint256 cb_txid = cb.hash;

    if (!sim_mint_block(s, &cb, 1, height))
        return false;   /* sim_mint_block already logged + freed */

    if (out_cb_txid)
        *out_cb_txid = cb_txid;
    return true;
}

bool simnet_spend(struct simnet *s, const struct uint256 *in_txid,
                  uint32_t in_n, int64_t out_value, struct uint256 *out_txid)
{
    if (!s || !s->initialized || !in_txid)
        LOG_FAIL("simnet", "invalid spend request");

    /* Resolve the input coin so we can (a) honor coinbase maturity and
     * (b) fail early with a clear message if it is absent. */
    struct coins in_coin;
    coins_init(&in_coin);
    if (!coins_view_cache_get_coins(&s->view, in_txid, &in_coin)) {
        coins_free(&in_coin);
        LOG_FAIL("simnet", "input coin absent");
    }
    if (!coins_is_available(&in_coin, in_n)) {
        coins_free(&in_coin);
        LOG_FAIL("simnet", "input output %u unavailable/spent", in_n);
    }
    if (out_value > in_coin.vout[in_n].value) {
        int64_t have = in_coin.vout[in_n].value;
        coins_free(&in_coin);
        LOG_FAIL("simnet", "out_value %lld exceeds input value %lld",
                 (long long)out_value, (long long)have);
    }

    /* Mint at a height that satisfies the real coinbase-maturity predicate
     * (pindex->nHeight - coin.height >= COINBASE_MATURITY). Non-coinbase
     * inputs are spendable at the next height. */
    int next = s->tip_height + 1;
    if (in_coin.is_coinbase) {
        int mature_at = in_coin.height + COINBASE_MATURITY;
        if (mature_at > next)
            next = mature_at;
    }
    coins_free(&in_coin);

    struct transaction cb = sim_make_coinbase(next);
    struct transaction spend = sim_make_spend(in_txid, in_n, out_value);
    struct uint256 spend_txid = spend.hash;

    struct transaction txs[2] = { cb, spend };
    if (!sim_mint_block(s, txs, 2, next))
        return false;   /* sim_mint_block already logged + freed */

    if (out_txid)
        *out_txid = spend_txid;
    return true;
}

int simnet_tip_height(const struct simnet *s)
{
    return (s && s->initialized) ? s->tip_height : -1;
}

bool simnet_coin_exists(struct simnet *s, const struct uint256 *txid)
{
    if (!s || !s->initialized || !txid)
        return false;
    return coins_view_cache_have_coins(&s->view, txid);
}

bool simnet_coin_value(struct simnet *s, const struct uint256 *txid,
                       uint32_t n, int64_t *out_value)
{
    if (!s || !s->initialized || !txid)
        return false;
    struct coins c;
    coins_init(&c);
    if (!coins_view_cache_get_coins(&s->view, txid, &c)) {
        coins_free(&c);
        return false;
    }
    bool ok = coins_is_available(&c, n);
    if (ok && out_value)
        *out_value = c.vout[n].value;
    coins_free(&c);
    return ok;
}
