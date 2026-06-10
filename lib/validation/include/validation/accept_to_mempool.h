/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_ACCEPT_TO_MEMPOOL_H
#define ZCL_ACCEPT_TO_MEMPOOL_H

#include "primitives/transaction.h"
#include "consensus/params.h"
#include <stdbool.h>
#include <stdint.h>

/* accept_to_mempool — the ONE shared mempool-acceptance gate.
 *
 * Before this existed, the P2P `tx` handler (lib/net/src/msg_tx.c) and
 * the RPC sendrawtransaction handler (app/controllers) each had their
 * own partial accept path. Both ran check_transaction (structural) and
 * — on the P2P side only — inputs-exist + min-relay-fee. NEITHER ran
 * the cryptographic checks: per-input scriptSig verification
 * (verify_script) and the shielded-proof / JoinSplit-signature
 * verification in contextual_check_transaction. The result: a tx with a
 * structurally-valid shape, existing prevouts, but an INVALID signature
 * or a forged Sapling/Sprout proof was added to the mempool and fluffed
 * network-wide; only block-connect later rejected it. An invalid-sig
 * flood thus wasted every node's bandwidth and mempool space.
 *
 * This helper closes that gap with a single code path used by BOTH
 * callers. It runs, in order:
 *   1. check_transaction              (structural / context-free)
 *   2. contextual_check_transaction   (JoinSplit Ed25519 sig, Sapling
 *                                      Groth16 spend/output proofs +
 *                                      binding sig, Sprout zk-SNARKs)
 *   3. (with a live coins view) inputs-exist, value/fee sanity,
 *      min-relay-fee, and per-input verify_script (the transparent
 *      signature check that was missing) BEFORE add+relay.
 *
 * connect_block remains the consensus backstop and re-verifies
 * everything at block time — this helper is mempool admission policy,
 * not a consensus relaxation. It NEVER admits a tx that connect_block
 * would reject; it only rejects (some) txs earlier, so they are never
 * relayed.
 *
 * Parameters are plain primitives so the helper lives in lib/validation
 * (a layer both lib/net and app/controllers already depend on) without
 * pulling in either caller's context struct:
 *   - pool       : destination mempool (required)
 *   - coins_tip  : live UTXO view; if NULL (unit-test scaffolding) the
 *                  input-existence / fee / script checks are skipped and
 *                  the tx is admitted at fee=0 (post-add policy hooks
 *                  still apply). Shielded-proof checks ALWAYS run.
 *   - main_state : chain tip, for next-block height / branch id; if NULL
 *                  height 0 is assumed.
 *   - params     : chain params (consensus). If NULL, height-aware
 *                  shielded checks are skipped (test scaffolding only).
 *   - tx         : the candidate. Its hash must already be computed by
 *                  the caller (transaction_compute_hash).
 *
 * Returns one of enum mempool_accept_result. The caller maps the result
 * onto its own reject reason / peer scoring / RPC error string. */
enum mempool_accept_result {
    MEMPOOL_ACCEPT_OK = 0,
    MEMPOOL_ACCEPT_INVALID,         /* structural / proof / script failed */
    MEMPOOL_ACCEPT_DUPLICATE,       /* already in mempool */
    MEMPOOL_ACCEPT_CONFLICT,        /* double-spend vs current mempool */
    MEMPOOL_ACCEPT_BELOW_FEE,       /* fee < min_relay_fee */
    MEMPOOL_ACCEPT_MISSING_INPUTS,  /* unknown inputs (orphan) */
    MEMPOOL_ACCEPT_INTERNAL_ERROR,  /* mempool full / OOM / bad args */
};

struct tx_mempool;
struct coins_view_cache;
struct main_state;
struct chain_params;

enum mempool_accept_result accept_to_mempool(
    struct tx_mempool *pool,
    struct coins_view_cache *coins_tip,
    struct main_state *main_state,
    const struct chain_params *params,
    struct transaction *tx);

/* Same gate with an explicit insert switch. dry_run=true runs every
 * check (structural, shielded proofs, inputs, fees, scripts) but does
 * NOT add the tx to the mempool — used by Dandelion (BIP 156) to vet a
 * stem tx that must stay out of the mempool until it fluffs. A
 * dry_run=true OK result therefore means "would be accepted right
 * now"; callers re-run with dry_run=false at fluff time. */
enum mempool_accept_result accept_to_mempool_ex(
    struct tx_mempool *pool,
    struct coins_view_cache *coins_tip,
    struct main_state *main_state,
    const struct chain_params *params,
    struct transaction *tx,
    bool dry_run);

#endif /* ZCL_ACCEPT_TO_MEMPOOL_H */
