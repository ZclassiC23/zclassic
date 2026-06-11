/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * accept_to_mempool — the ONE shared mempool-acceptance gate used by
 * both the P2P `tx` handler and the RPC sendrawtransaction handler.
 * See validation/accept_to_mempool.h for the contract and the bug this
 * closes (unverified-sig / unverified-proof txs being relayed). */

#include "validation/accept_to_mempool.h"
#include "validation/check_transaction.h"
#include "validation/contextual_check_tx.h"
#include "validation/txmempool.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "coins/coins_view.h"
#include "chain/chainparams.h"
#include "script/interpreter.h"
#include "script/script_flags.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"

/* Verify every transparent input's scriptSig against its prevout's
 * scriptPubKey. Mirrors the per-input check in connect_block.c exactly
 * (same flags, same branch id, same precomputed sighash data) so a tx
 * that passes here would also pass connect_block — and one that fails
 * here is one connect_block would have rejected too. Runs serially
 * (mempool txs are single, not whole blocks; the parallel workpool is
 * connect_block's optimisation for big blocks).
 *
 * Returns true iff all inputs verify. On a missing prevout returns
 * false via *missing_inputs (orphan, not a malicious script failure). */
static bool verify_tx_inputs_scripts(struct coins_view_cache *view,
                                     const struct transaction *tx,
                                     uint32_t branch_id,
                                     bool *missing_inputs)
{
    *missing_inputs = false;

    uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

    struct precomputed_tx_data txdata;
    precompute_tx_data(tx, &txdata);

    for (size_t j = 0; j < tx->num_vin; j++) {
        const struct tx_out *prev_out =
            coins_view_cache_get_output_for(view, &tx->vin[j]);
        if (!prev_out) {
            /* Should not happen — caller already ran have_inputs — but
             * stay defensive: treat as orphan rather than a script
             * failure so an honest race doesn't earn a ban-score. */
            *missing_inputs = true;
            return false;
        }

        struct tx_sig_checker tsc;
        tx_sig_checker_init(&tsc, tx, (unsigned int)j, prev_out->value,
                            branch_id, &txdata);
        struct sig_checker checker = tx_make_sig_checker(&tsc);

        ScriptError serror = SCRIPT_ERR_OK;
        if (!verify_script(&tx->vin[j].script_sig,
                           &prev_out->script_pub_key,
                           flags, &checker, branch_id, &serror)) {
            return false;
        }
    }
    return true;
}

enum mempool_accept_result accept_to_mempool(
    struct tx_mempool *pool,
    struct coins_view_cache *coins_tip,
    struct main_state *main_state,
    const struct chain_params *params,
    struct transaction *tx)
{
    if (!pool || !tx)
        return MEMPOOL_ACCEPT_INTERNAL_ERROR;

    /* Coinbase txs are only valid as the first tx of a block; they must
     * never be relayed standalone. */
    if (transaction_is_coinbase(tx))
        return MEMPOOL_ACCEPT_INVALID;

    /* 1. Structural / context-free checks. STANDALONE context on
     * purpose: a NEW tx always gets the strict post-Sapling 102000 size
     * cap regardless of history — the empirical oversize grandfather is
     * in-block only, matching zclassicd's AcceptToMemoryPool ->
     * CheckTransaction, which is always strict. */
    struct validation_state state;
    validation_state_init(&state);
    if (!check_transaction(tx, &state))
        return MEMPOOL_ACCEPT_INVALID;

    transaction_compute_hash(tx);
    struct uint256 hash = tx->hash;

    if (tx_mempool_exists(pool, &hash))
        return MEMPOOL_ACCEPT_DUPLICATE;

    /* Double-spend vs current mempool — explicit probe so the caller
     * can attribute it to the sending peer. */
    if (tx_mempool_has_conflict(pool, tx))
        return MEMPOOL_ACCEPT_CONFLICT;

    int tip_height = main_state
        ? active_chain_height(&main_state->chain_active) : 0;
    int next_height = tip_height + 1;
    uint32_t branch_id = params
        ? consensus_current_epoch_branch_id(next_height, &params->consensus)
        : 0;

    /* 2. Height-aware shielded checks: JoinSplit Ed25519 signature,
     *    Sapling Groth16 spend/output proofs + binding signature, and
     *    Sprout zk-SNARK proofs. THIS is the proof verification the
     *    relay path was missing. dosLevel 100 mirrors the relay-time
     *    contextual check in zclassicd's AcceptToMemoryPool. Requires
     *    chain params; without them (unit-test scaffolding) skip — a
     *    NULL params can only occur off the live path. */
    if (params) {
        validation_state_init(&state);
        if (!contextual_check_transaction(tx, &state, &params->consensus,
                                          next_height, 100))
            return MEMPOOL_ACCEPT_INVALID;
    }

    /* 3. Input-dependent checks: require a live coins view. Absent view
     *    (unit tests only) means we accept at fee=0 and rely on the
     *    post-add policy hook for eviction. The transparent SIGNATURE
     *    check (verify_script) lives here because it needs each prevout's
     *    scriptPubKey + value from the view. */
    int64_t fee = 0;
    if (coins_tip) {
        if (!coins_view_cache_have_inputs(coins_tip, tx))
            return MEMPOOL_ACCEPT_MISSING_INPUTS;

        int64_t value_in = coins_view_cache_get_value_in(coins_tip, tx);
        if (value_in < 0)
            return MEMPOOL_ACCEPT_INVALID;

        int64_t value_out = transaction_get_value_out(tx);
        if (value_out < 0 || value_in < value_out)
            return MEMPOOL_ACCEPT_INVALID;

        fee = value_in - value_out;

        int64_t min_relay_fee = pool->min_relay_fee;
        if (min_relay_fee > 0 && fee < min_relay_fee)
            return MEMPOOL_ACCEPT_BELOW_FEE;

        /* Transparent scriptSig verification — the missing signature
         * check. A bad-sig tx is rejected here, BEFORE add+relay. */
        bool missing_inputs = false;
        if (!verify_tx_inputs_scripts(coins_tip, tx, branch_id,
                                      &missing_inputs)) {
            return missing_inputs ? MEMPOOL_ACCEPT_MISSING_INPUTS
                                  : MEMPOOL_ACCEPT_INVALID;
        }
    }

    struct mempool_entry entry;
    mempool_entry_init(&entry, tx, fee,
                       (int64_t)platform_time_wall_time_t(), 0.0,
                       (unsigned int)next_height,
                       tx_mempool_has_no_inputs_of(pool, tx),
                       false, branch_id);

    if (!tx_mempool_add_unchecked(pool, &hash, &entry)) {
        mempool_entry_free(&entry);
        return MEMPOOL_ACCEPT_INTERNAL_ERROR;
    }

    return MEMPOOL_ACCEPT_OK;
}
