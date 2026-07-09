/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * simnet — deterministic, RAM-only single-node chain harness (Phase 6d
 * foundation slice).
 *
 * The keystone the token / name / spend simulators build on: assemble a
 * block, drive it through the REAL consensus code (connect_block()), and
 * watch the in-memory UTXO state advance — with NO disk, NO real PoW, and
 * NO real funds.
 *
 *   assemble block  →  connect_block(&blk, &vs, &pindex, &view, &params,
 *                                    false)  →  coins_view advances in RAM
 *
 * How PoW / script validation is skipped WITHOUT touching consensus code:
 * simnet mints at heights covered by a synthetic checkpoint, so
 * connect_block sets expensive_checks=false (identical mechanism to
 * lib/test/src/test_connect_block_self_write.c). We never modify a
 * consensus predicate — if a block is rejected, the harness fixed its own
 * block construction.
 *
 * State model (all in RAM, no sqlite):
 *   - `view`     : the live UTXO set (a coins_view_cache over a zeroed
 *                  backing view). This IS the chain state.
 *   - `tip`      : the current tip's block_index; `tip.hashBlock` is the
 *                  stable storage the next block's hashPrevBlock links to.
 *   - `params`   : a value-copy of chain_params_get() with a single
 *                  high-height checkpoint so every minted height is
 *                  "covered" (expensive_checks=false).
 *
 * Coinbase maturity: connect_block's real predicate is
 *   pindex->nHeight - coin.height >= COINBASE_MATURITY (100).
 * simnet_spend() honors it by minting the spending block at a height that
 * satisfies the predicate — see the note on simnet_spend below. Nothing at
 * the connect_block layer requires contiguous heights, so this is a
 * minimal, faithful way to produce a mature-coinbase spend through the real
 * validator.
 */

#ifndef ZCL_SIM_SIMNET_H
#define ZCL_SIM_SIMNET_H

#include "coins/coins_view.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/checkpoints.h"
#include "core/uint256.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct simnet {
    struct coins_view_cache view;       /* the live in-RAM UTXO set    */
    struct coins_view       null_view;  /* zeroed backing (no disk)    */
    struct chain_params     params;     /* value copy + high checkpoint */
    struct checkpoint_entry cpentry;    /* params.checkpointData -> here */
    struct block_index      tip;        /* current tip; tip.hashBlock = stable tip hash */
    int  tip_height;
    bool initialized;
};

/* Initialize a fresh, empty single-node harness. Places the synthetic base
 * tip just below the first mintable height (Sapling inactive there), wires
 * the coins view's best block to it, and installs the covering checkpoint.
 * Returns false (and logs) on OOM; on false the struct must not be used. */
bool simnet_init(struct simnet *s);

/* Release the harness's in-memory state (coins view). Idempotent. */
void simnet_free(struct simnet *s);

/* Mint a new block whose only transaction is a coinbase, driving it through
 * connect_block(). On success the tip advances by one and the coinbase's
 * single output is a spendable (maturing) coin in `view`. `out_cb_txid`, if
 * non-NULL, receives the coinbase txid. Returns false (and logs the reject
 * reason) if the real validator rejects the block. */
bool simnet_mint_coinbase(struct simnet *s, struct uint256 *out_cb_txid);

/* Mint a new block containing the harness coinbase followed by caller-built
 * transparent transactions. `txs[0..ntx)` may include OP_RETURN outputs and
 * spends against the live in-RAM coins view. On a valid request ownership of
 * each tx's allocated vin/vout arrays transfers to simnet; the caller must
 * keep any needed txids before calling. Returns false (and logs) if the real
 * validator rejects the block. */
bool simnet_mint_txs(struct simnet *s, struct transaction *txs, size_t ntx);

/* Mint a block that spends `in_txid`:`in_n` to one new output of
 * `out_value`, plus the block's own coinbase (vtx[0]). The block is minted
 * at a height that satisfies coinbase maturity for the spent coin, so a
 * freshly-minted coinbase can be spent through the REAL maturity predicate
 * without minting 100 filler blocks. On success the input coin is consumed
 * and the new output is present in `view`. `out_txid`, if non-NULL, receives
 * the spend tx's txid. Returns false (and logs) on rejection or if the input
 * coin is absent. `out_value` must be <= the spent output's value (the
 * difference is the fee). */
bool simnet_spend(struct simnet *s, const struct uint256 *in_txid,
                  uint32_t in_n, int64_t out_value, struct uint256 *out_txid);

/* Height of the current tip. */
int simnet_tip_height(const struct simnet *s);

/* Copy the current tip hash into `out`. */
bool simnet_tip_hash(const struct simnet *s, struct uint256 *out);

/* True iff `txid` has at least one unspent output in the live UTXO set. */
bool simnet_coin_exists(struct simnet *s, const struct uint256 *txid);

/* Fetch the value of `txid`:`n` from the live UTXO set. Returns false (and
 * leaves *out_value untouched) if the coin or output index is absent/spent. */
bool simnet_coin_value(struct simnet *s, const struct uint256 *txid,
                       uint32_t n, int64_t *out_value);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_SIM_SIMNET_H */
