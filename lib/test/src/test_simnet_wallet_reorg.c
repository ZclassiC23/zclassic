/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet-balance restatement across a reorg (lane W1-c).
 * =====================================================================
 *
 * The coins-VIEW side of a reorg (disconnect_block undo/redo symmetry, the
 * XOR UTXO commitment, cluster convergence) is already proven by
 * test_chain_rollback.c and test_simnet_cluster_reorg.c. What those do NOT
 * cover is the layer above: after the active chain reorgs, does the WALLET's
 * spendable BALANCE restate to match the winning branch? This test closes that
 * gap by driving the production wallet (lib/wallet/src/wallet.c) through a
 * transparent reorg and asserting balance at every transition — no double
 * count, never negative.
 *
 * The three transparent restatement facts a correct wallet must exhibit
 * (owner directive), each asserted below against a REAL coins_view_cache built
 * with the REAL update_coins consensus path:
 *
 *   (1) a coin RECEIVED on the losing branch DISAPPEARS from the balance after
 *       the reorg (wallet_verify_utxos prunes it: the authoritative winning
 *       coins view has no such UTXO);
 *   (2) a coin SPENT on the losing branch becomes UNSPENT again (the losing
 *       spend is retracted via wallet_rollback_transaction, which un-marks the
 *       input outpoint — the coin returns to the balance);
 *   (3) the WINNING branch's own coins APPEAR (wallet_sync_transaction on the
 *       winning-branch tx).
 *
 * These are the production wallet's restatement primitives; a correct reorg
 * handler composes exactly them (prune-against-the-new-tip + retract-the-undone
 * + connect-the-new). The test asserts the composed balance is exactly right.
 *
 * SHIELDED (params-gated; SKIP without ~/.zcash-params): the same restatement
 * shape at the Sapling note layer — a note received on the losing branch is
 * removed, and a note whose nullifier was marked spent on the losing branch has
 * that nullifier UN-marked on rollback, so z-balance restates. Driven through
 * the real wallet_get_sapling_balance / wallet_mark_sapling_nullifiers_spent /
 * wallet_rollback_transaction (shielded nullifier un-mark) primitives. The full
 * Groth16 shielded send cannot be driven end-to-end today (see the pre-existing
 * prover<->verifier blocker documented in test_simnet_sapling_shielded_send.c),
 * so the shielded leg exercises the wallet-side note restatement logic with
 * directly-constructed notes; it is params-gated to keep the default CI path
 * (no params) clean per the lane contract.
 */

#include "test/test_helpers.h"

#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "keys/pubkey.h"
#include "script/standard.h"
#include "script/script.h"
#include "coins/coins_view.h"
#include "validation/update_coins.h"
#include "validation/txmempool.h"
#include "primitives/transaction.h"
#include "core/amount.h"
#include "core/uint256.h"
#include "sim/seed_tape.h"
#include "sapling/params_init.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WR_CHECK(name, expr) do {          \
    printf("%s... ", (name));              \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Build a transaction: `nvin` inputs from the given prevouts, `nvout` outputs
 * with the given (value, script) pairs. A NULL prevout hash + n==0xFFFFFFFF
 * models a coinbase (no real inputs). Caller owns the tx and must
 * transaction_free() it. */
static void wr_build_tx(struct transaction *tx,
                        const struct uint256 *prev_hashes,
                        const uint32_t *prev_ns, size_t nvin,
                        const int64_t *values,
                        const struct script *scripts, size_t nvout)
{
    transaction_init(tx);
    bool ok = transaction_alloc(tx, nvin, nvout);
    if (!ok) {
        fprintf(stderr, "wr_build_tx: transaction_alloc failed\n");
        abort();
    }
    tx->version = 1;
    for (size_t i = 0; i < nvin; i++) {
        tx->vin[i].prevout.hash = prev_hashes[i];
        tx->vin[i].prevout.n = prev_ns[i];
        uint8_t sig[] = {0x00, 0x00};
        script_set(&tx->vin[i].script_sig, sig, sizeof(sig));
        tx->vin[i].sequence = 0xFFFFFFFFu;
    }
    for (size_t j = 0; j < nvout; j++) {
        tx->vout[j].value = values[j];
        tx->vout[j].script_pub_key = scripts[j];
    }
    transaction_compute_hash(tx);
}

/* A synthetic block_index for wallet_sync_transaction: only nHeight +
 * phashBlock are read. `hash_store` must outlive the index. */
static void wr_index(struct block_index *bi, struct uint256 *hash_store,
                     int height, uint8_t tag)
{
    memset(bi, 0, sizeof(*bi));
    memset(hash_store->data, tag, sizeof(hash_store->data));
    bi->nHeight = height;
    bi->phashBlock = hash_store;
}

/* ── Transparent reorg -> wallet-balance restatement ──────────────────── */
static int wr_transparent(void)
{
    int failures = 0;

    /* A real wallet is ~65 MB — heap-allocate (see simnet_wallet.c note). */
    struct wallet *w = zcl_malloc(sizeof(*w), "wr_wallet");
    WR_CHECK("transparent: wallet alloc", w != NULL);
    if (!w)
        return failures + 1;
    wallet_init(w);

    /* Our receiving key + its P2PKH script (the "mine" predicate). */
    struct pubkey pk;
    WR_CHECK("transparent: generate receiving key",
             wallet_generate_new_key(w, &pk));
    struct key_id kid = pubkey_get_id(&pk);
    struct script mine;
    script_for_p2pkh(&mine, &kid);

    /* A non-wallet ("faucet"/counterparty) script for coinbase + spend sinks. */
    struct key_id other_kid;
    memset(other_kid.id.data, 0x5A, sizeof(other_kid.id.data));
    struct script other;
    script_for_p2pkh(&other, &other_kid);

    /* best_block high so every non-coinbase tx confirms deep (>=1). */
    w->best_block_height = 1000;

    const int64_t V0 = 100 * COIN; /* pre-fork coin paid to us            */
    const int64_t VL = 50 * COIN;  /* coin RECEIVED on the losing branch  */
    const int64_t VW = 70 * COIN;  /* coin the WINNING branch pays us     */

    /* ── The authoritative WINNING-branch coins view, built through the REAL
     * update_coins path. It contains: a faucet coinbase (both outputs later
     * spent), P0:0 (=V0, unspent on W) and PW:0 (=VW). It does NOT contain the
     * losing-branch coin. This is exactly the tip UTXO set the wallet must
     * reconcile against after the reorg. */
    struct coins_view_cache cw;
    struct coins_view null_view;
    memset(&null_view, 0, sizeof(null_view));
    coins_view_cache_init(&cw, &null_view);

    struct transaction fc, p0, pw;
    {
        int64_t fvals[2] = {10 * COIN, 10 * COIN};
        struct script fscr[2] = {other, other};
        struct uint256 cb_hash; uint256_set_null(&cb_hash);
        uint32_t cb_n = 0xFFFFFFFFu;
        wr_build_tx(&fc, &cb_hash, &cb_n, 1, fvals, fscr, 2);
        update_coins(&fc, &cw, 0);

        struct uint256 in_h[1] = { fc.hash };
        uint32_t in_n[1] = { 0 };
        int64_t v[1] = { V0 };
        struct script s[1] = { mine };
        wr_build_tx(&p0, in_h, in_n, 1, v, s, 1);
        update_coins(&p0, &cw, 1);

        struct uint256 in_h2[1] = { fc.hash };
        uint32_t in_n2[1] = { 1 };
        int64_t v2[1] = { VW };
        struct script s2[1] = { mine };
        wr_build_tx(&pw, in_h2, in_n2, 1, v2, s2, 1);
        update_coins(&pw, &cw, 2);
    }
    WR_CHECK("transparent: winning coins view has P0:0 unspent",
             coins_view_cache_have_coins(&cw, &p0.hash));
    WR_CHECK("transparent: winning coins view has PW:0",
             coins_view_cache_have_coins(&cw, &pw.hash));

    /* ── Pre-fork: the wallet learns P0 (pays us V0). ── */
    struct block_index bi0; struct uint256 h0;
    wr_index(&bi0, &h0, 1, 0x01);
    wallet_sync_transaction(w, &p0, &bi0);
    WR_CHECK("transparent: balance == V0 after pre-fork receive",
             wallet_get_balance(w) == V0);

    /* ── Losing branch: receive VL, and SPEND P0:0 to a non-wallet sink. ── */
    struct transaction prl, psl;
    {
        /* PRL: pays us VL (its input is an unrelated faucet coin — the wallet
         * only cares that an output is ours). */
        struct uint256 in_h[1]; memset(in_h[0].data, 0x33, 32);
        uint32_t in_n[1] = { 0 };
        int64_t v[1] = { VL };
        struct script s[1] = { mine };
        wr_build_tx(&prl, in_h, in_n, 1, v, s, 1);

        /* PSL: spends P0:0 (ours) -> paying `other`. Marks P0:0 spent. */
        struct uint256 in_h2[1] = { p0.hash };
        uint32_t in_n2[1] = { 0 };
        int64_t v2[1] = { 90 * COIN };
        struct script s2[1] = { other };
        wr_build_tx(&psl, in_h2, in_n2, 1, v2, s2, 1);
    }
    struct block_index bi_l; struct uint256 hl;
    wr_index(&bi_l, &hl, 2, 0x02);
    wallet_sync_transaction(w, &prl, &bi_l);
    WR_CHECK("transparent: balance == V0+VL after losing receive",
             wallet_get_balance(w) == V0 + VL);

    struct block_index bi_l2; struct uint256 hl2;
    wr_index(&bi_l2, &hl2, 3, 0x03);
    wallet_sync_transaction(w, &psl, &bi_l2);
    WR_CHECK("transparent: balance == VL after losing spend of P0",
             wallet_get_balance(w) == VL);
    WR_CHECK("transparent: balance never negative on losing branch",
             wallet_get_balance(w) >= 0);

    /* ── REORG. Reconcile the wallet against the winning coins view `cw`. ── */

    /* (1) Prune coins that no longer exist on the winning tip: this drops the
     * losing-branch received coin (PRL:0 is absent from `cw`). P0:0 is present
     * in `cw` but is currently marked spent in the wallet, so verify_utxos
     * (which only prunes) leaves it — the spent state is undone in step (2). */
    wallet_verify_utxos(w, &cw);
    WR_CHECK("transparent: (1) losing receive pruned -> balance 0",
             wallet_get_balance(w) == 0);

    /* (2) Retract the losing-branch spend: un-marks P0:0, so the coin the
     * losing branch consumed is spendable again. */
    struct tx_mempool mp;
    tx_mempool_init(&mp, 1000);
    struct wallet_tx psl_wtx;
    memset(&psl_wtx, 0, sizeof(psl_wtx));
    psl_wtx.tx = psl; /* aliases vin/vout for the outpoint-unmark walk */
    struct zcl_result rr = wallet_rollback_transaction(w, &psl_wtx, &mp);
    WR_CHECK("transparent: (2) losing spend retracts cleanly", rr.ok);
    WR_CHECK("transparent: (2) P0 coin unspent again -> balance == V0",
             wallet_get_balance(w) == V0);

    /* (3) Connect the winning branch's coin to us. */
    struct block_index bi_w; struct uint256 hw;
    wr_index(&bi_w, &hw, 2, 0x04);
    wallet_sync_transaction(w, &pw, &bi_w);
    WR_CHECK("transparent: (3) winning coin appears -> balance == V0+VW",
             wallet_get_balance(w) == V0 + VW);

    /* No double count: exactly the winning-branch spendable set, and the
     * losing receive is gone (V0+VW, never V0+VL+VW or V0+VL). */
    int64_t final_bal = wallet_get_balance(w);
    WR_CHECK("transparent: final balance is exactly V0+VW (no double count)",
             final_bal == V0 + VW);
    WR_CHECK("transparent: losing-branch coin excluded from final balance",
             final_bal != V0 + VL + VW && final_bal != V0 + VL);
    WR_CHECK("transparent: balance non-negative after full reconcile",
             final_bal >= 0);

    tx_mempool_free(&mp);
    transaction_free(&fc);
    transaction_free(&p0);
    transaction_free(&pw);
    transaction_free(&prl);
    transaction_free(&psl);
    coins_view_cache_free(&cw);
    wallet_free(w);
    free(w);
    return failures;
}

/* ── Shielded reorg -> z-balance restatement (params-gated) ──────────────
 * Directly constructs received notes (the real receive path needs the Groth16
 * prover + params and is blocked today) and drives the production shielded
 * restatement primitives across a modeled reorg. */
static int wr_shielded(void)
{
    int failures = 0;

    const char *home = getenv("HOME");
    char params_dir[512];
    snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
             (home && *home) ? home : ".");
    if (!sapling_init_params(params_dir)) {
        printf("  ~/.zcash-params absent — SKIPPING shielded reorg leg "
               "(transparent leg above ran)\n");
        return 0; /* clean skip */
    }
    printf("  ~/.zcash-params present — running shielded z-balance restatement\n");

    struct wallet *w = zcl_malloc(sizeof(*w), "wr_wallet_z");
    WR_CHECK("shielded: wallet alloc", w != NULL);
    if (!w)
        return failures + 1;
    wallet_init(w);

    const uint64_t VK = 60; /* keep-branch note value */
    const uint64_t VL = 40; /* losing-branch received note value */

    /* Two received notes: NK (survives) and NL (received on the losing
     * branch). Struct wallet is public; append into its notes array with a
     * free()-compatible allocation (wallet_free() releases it). */
    w->sapling_notes = zcl_malloc(2 * sizeof(*w->sapling_notes), "wr_notes");
    WR_CHECK("shielded: notes alloc", w->sapling_notes != NULL);
    if (!w->sapling_notes) { wallet_free(w); free(w); return failures + 1; }
    w->sapling_notes_cap = 2;
    memset(w->sapling_notes, 0, 2 * sizeof(*w->sapling_notes));

    struct sapling_received_note *nk = &w->sapling_notes[0];
    nk->used = true; nk->spent = false; nk->value = VK;
    memset(nk->nf, 0x11, 32);
    struct sapling_received_note *nl = &w->sapling_notes[1];
    nl->used = true; nl->spent = false; nl->value = VL;
    memset(nl->nf, 0x22, 32);
    w->num_sapling_notes = 2;

    WR_CHECK("shielded: z-balance == VK+VL initially",
             wallet_get_sapling_balance(w) == (int64_t)(VK + VL));

    /* Losing-branch spend of NK: a tx carrying NK's nullifier. Record it in the
     * wallet map so the retraction path can find + undo it, then mark spent. */
    struct transaction ts;
    transaction_init(&ts);
    ts.version = 4;
    ts.v_shielded_spend =
        zcl_malloc(sizeof(struct spend_description), "wr_spend");
    WR_CHECK("shielded: spend desc alloc", ts.v_shielded_spend != NULL);
    if (!ts.v_shielded_spend) { wallet_free(w); free(w); return failures + 1; }
    memset(ts.v_shielded_spend, 0, sizeof(struct spend_description));
    ts.num_shielded_spend = 1;
    memset(ts.v_shielded_spend[0].nullifier.data, 0x11, 32); /* == NK.nf */
    transaction_compute_hash(&ts);

    struct wallet_tx ts_wtx;
    memset(&ts_wtx, 0, sizeof(ts_wtx));
    ts_wtx.tx = ts;
    ts_wtx.used = true;
    ts_wtx.confirms = 1;
    WR_CHECK("shielded: record losing spend tx",
             wallet_add_to_wallet(w, &ts_wtx));

    wallet_mark_sapling_nullifiers_spent(w, &ts);
    WR_CHECK("shielded: NK marked spent -> z-balance == VL",
             wallet_get_sapling_balance(w) == (int64_t)VL);
    WR_CHECK("shielded: z-balance non-negative after spend",
             wallet_get_sapling_balance(w) >= 0);

    /* REORG (a): retract the losing spend -> NK's nullifier un-marked. */
    struct tx_mempool mp;
    tx_mempool_init(&mp, 1000);
    struct zcl_result rr = wallet_rollback_transaction(w, &ts_wtx, &mp);
    WR_CHECK("shielded: losing spend retracts cleanly", rr.ok);
    WR_CHECK("shielded: (a) NK nullifier un-marked -> z-balance == VK+VL",
             wallet_get_sapling_balance(w) == (int64_t)(VK + VL));

    /* REORG (b): the note received on the losing branch is removed. */
    nl->used = false;
    WR_CHECK("shielded: (b) losing-branch note removed -> z-balance == VK",
             wallet_get_sapling_balance(w) == (int64_t)VK);
    WR_CHECK("shielded: z-balance non-negative after full reconcile",
             wallet_get_sapling_balance(w) >= 0);

    tx_mempool_free(&mp);
    transaction_free(&ts);
    wallet_free(w);
    free(w);
    return failures;
}

int test_simnet_wallet_reorg(void)
{
    printf("\n=== wallet-balance restatement across a reorg ===\n");
    int failures = 0;

    seed_tape_t *tape = seed_tape_open(0x57414C4C5245F00DULL, 1700000000);
    if (tape)
        seed_tape_install(tape);

    failures += wr_transparent();
    failures += wr_shielded();

    if (tape) {
        seed_tape_uninstall();
        seed_tape_close(tape);
    }

    printf("wallet-balance reorg restatement: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}
