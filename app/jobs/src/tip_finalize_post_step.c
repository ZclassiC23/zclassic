/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_post_step — reducer post-finalize side effects.
 * See tip_finalize_post_step.h for the contract.
 *
 * tip_finalize_run_post_finalize owns the wallet_sync / Sapling trial-decrypt
 * / nullifier-spend / mempool-remove / MMR / MMB side effects, run after tip
 * publication. The connected block is READ BACK from disk via
 * stage_default_block_reader (the reducer does not receive it as a parameter),
 * and the wallet / node_db / mempool handles are fetched through the public
 * app_runtime_* accessors.
 */

#include "tip_finalize_post_step.h"
#include "jobs/stage_helpers.h"

#include "chain/chain.h"
#include "chain/mmb.h"
#include "config/runtime.h"
#include "controllers/blockchain_controller.h"
#include "controllers/sync_controller.h"
#include "core/uint256.h"
#include "models/database.h"            /* struct node_db */
#include "chain/mmr.h"                  /* MMR_COMMITMENT_INTERVAL boundary */
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/coins_kv.h"           /* coins_kv_commitment + boundary root */
#include "storage/progress_store.h"     /* progress_store_db() handle */
#include "services/block_source_policy.h" /* projection-deferred diagnostic */
#include "services/chain_evidence_authority_service.h" /* live evidence follow */
#include "util/util.h"                  /* GetDataDir */
#include "validation/check_block.h"     /* coinbase-height label check */
#include "validation/chain_linkage_check.h" /* fail-loud HOLD latch */
#include "validation/process_block.h"   /* g_body_pull_active */
#include "validation/txmempool.h"
#include "wallet/wallet.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void tip_finalize_run_post_finalize(struct block_index *pindex_new)
{
    if (!pindex_new)
        return;

    /* Note the published tip for the chain-evidence follow. The drive MUST
     * NOT run the evidence machinery itself: it holds the coins_kv authority
     * mutex, and the evidence path takes csr->lock then coins_kv — calling it
     * from here is the inverted ABBA edge that would deadlock via inverted
     * lock order. This stamps one leaf-mutex slot and returns;
     * node_health_collect drains it with the correct lock order before every
     * health snapshot, so the mismatch this follow exists to clear is never
     * observed. */
    chain_evidence_note_finalized_tip(pindex_new);

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));

    struct block blk;
    block_init(&blk);
    if (!stage_read_block(&blk, pindex_new, pindex_new->nHeight, datadir,
                          NULL, NULL)) {
        /* No on-disk body (HAVE_DATA absent / read failed). The body is read
         * back from disk, so a missing body is a benign skip — the tip still
         * advanced, only the derived side effects are deferred. The skip must
         * be DIAGNOSED, never silent: all six side effects (wallet sync, note
         * decrypt, nullifier spend, mempool remove, MMR, MMB) are dropped for
         * this height. */
        LOG_WARN("tip_finalize",
                 "post-finalize side effects skipped h=%d have_data=%d: "
                 "body unreadable; wallet/mempool/MMR/MMB deferred",
                 pindex_new->nHeight,
                 (pindex_new->nStatus & BLOCK_HAVE_DATA) ? 1 : 0);
        /* A partial deserialize can have allocated blk.vtx before the
         * read failed — the success path frees it at the bottom; this
         * early return must too. */
        block_free(&blk);
        return;
    }

    /* Fail-loud validation pack, check 2: the BIP34-style height embedded
     * in the coinbase scriptSig must equal OUR label for the block just
     * finalized. The body was already read back from disk above, so this
     * costs one header hash + a few byte compares — no extra I/O. A
     * mismatch is a label/height shift (a mis-spliced chain): HOLD the
     * pipeline (refuse h+1 onward) + PAGE. E13-neutral: the block stays
     * valid; only OUR pipeline holds. Crash-only: no FATAL, side effects
     * still run so the published tip stays coherent.
     *
     * HASH-BOUND: the check fires only when the read body IS the indexed
     * block (header hash == phashBlock). A body that hashes differently
     * is a mis-positioned/corrupt body — a different defect class, owned
     * by the have_data_unreadable machinery, and comparing ITS coinbase
     * against OUR label would be a false splice signal. */
    if (pindex_new->nHeight >= 1 && pindex_new->phashBlock &&
        blk.num_vtx > 0) {
        struct uint256 body_hash;
        block_get_hash(&blk, &body_hash);
        if (uint256_eq(&body_hash, pindex_new->phashBlock) &&
            !check_block_coinbase_height_matches(&blk.vtx[0],
                                                 pindex_new->nHeight)) {
            char reason[160];
            snprintf(reason, sizeof(reason),
                     "coinbase-embedded height != our label h=%d (label "
                     "shift detected at finalize; held before h=%d)",
                     pindex_new->nHeight, pindex_new->nHeight + 1);
            LOG_WARN("tip_finalize", "[validation_pack] %s", reason);
            chain_linkage_hold_raise("coinbase_label",
                                     "chain.coinbase_label_mismatch",
                                     pindex_new->nHeight + 1, reason);
        }
    }

    /* Notify wallet of transactions in the connected block.
     * Skipped during fast-sync body-pull: evidence-mode caller runs a
     * single wallet_rescan over the imported range at the end. */
    if (!atomic_load_explicit(&g_body_pull_active, memory_order_relaxed))
    {
        struct wallet *wallet = app_runtime_wallet();
        struct node_db *ndb = app_runtime_node_db();
        if (wallet) {
            for (size_t i = 0; i < blk.num_vtx; i++) {
                wallet_sync_transaction(wallet, &blk.vtx[i], pindex_new);
                /* Trial-decrypt Sapling shielded outputs for our wallet */
                if (blk.vtx[i].num_shielded_output > 0 &&
                    wallet->sapling_keys.num_keys > 0) {
                    struct transaction *tx =
                        (struct transaction *)&blk.vtx[i];
                    transaction_compute_hash(tx);
                    size_t notes_before = wallet->num_sapling_notes;
                    wallet_try_sapling_decrypt(wallet, tx, &tx->hash);
                    /* Persist newly discovered notes to SQLite. Snapshot the
                     * notes under the wallet lock first: iterating the live
                     * array here would race a concurrent note-append realloc
                     * (e.g. an RPC rescan) and read freed memory. */
                    if (ndb) {
                        size_t n_notes = 0;
                        struct sapling_received_note *snap =
                            wallet_copy_sapling_notes(wallet, &n_notes);
                        for (size_t ni = notes_before; ni < n_notes; ni++) {
                            struct sapling_received_note *note = &snap[ni];
                            node_db_sync_sapling_note(ndb,
                                note->txid.data, note->output_index,
                                (int64_t)note->value, note->rcm,
                                note->memo, 512, note->ivk,
                                note->diversifier, note->pk_d,
                                note->cm, note->nf,
                                pindex_new->nHeight);
                        }
                        free(snap);
                    }
                }
                /* Mark spent nullifiers */
                if (blk.vtx[i].num_shielded_spend > 0)
                    wallet_mark_sapling_nullifiers_spent(
                        wallet, (struct transaction *)&blk.vtx[i]);
            }
            wallet->best_block_height = pindex_new->nHeight;
        }
    }

    /* Remove confirmed transactions from mempool */
    {
        struct tx_mempool *mempool = app_runtime_mempool();
        if (mempool)
            tx_mempool_remove_for_block(mempool,
                blk.vtx, blk.num_vtx,
                (unsigned int)pindex_new->nHeight);
    }

    /* Projection-deferred DIAGNOSTIC.
     * The reducer consensus path does NOT write the derived block/tx SQLite
     * projection inline — the active chain, block index, and coins view are
     * authoritative and the projection is repairable from verified block
     * bytes. Record that the per-block projection write was deferred as a
     * DIAGNOSTIC counter. This is NOT a block reject: the tip already
     * advanced. Explicit import/catchup paths backfill the projection under
     * the DB service's write ownership. */
    {
        struct node_db *ndb = app_runtime_node_db();
        if (ndb)
            block_source_policy_note_projection_deferred(
                pindex_new->nHeight, "consensus_path");
    }

    /* Append block hash to Merkle Mountain Range */
    if (pindex_new->phashBlock)
        rpc_blockchain_mmr_append(pindex_new->phashBlock->data);

    /* Append rich leaf to Merkle Mountain Belt (O(1) per block).
     *
     * Keystone: at a 100-block boundary, the leaf carries the SHA3 root of the
     * full UTXO set as it stands AFTER this block (coins_kv_commitment — the
     * one canonical encoder). This is the only path that observes the live
     * coins set at the exact moment it equals height H, so it computes the
     * boundary root ONCE here and records it under the per-height key; catch-up
     * and leaf-store rebuild read that recorded value so every leaf hash is
     * byte-identical regardless of which path built it. The O(N) SHA3 fold runs
     * only once per 100 blocks and only at the live tip — never on a
     * latency-critical path. It is gated OFF during deferred-proof-validation
     * IBD (the same guard the MMR commit uses): re-folding the full set every
     * 100 blocks while replaying millions of blocks is wasteful, and the
     * boundary roots are back-filled on the catch-up pass once the tip is live. */
    if (pindex_new->phashBlock) {
        uint8_t utxo_root[32] = {0};
        if (pindex_new->nHeight > 0 &&
            pindex_new->nHeight % MMR_COMMITMENT_INTERVAL == 0) {
            extern _Atomic int g_deferred_proof_validation_below_height;
            int defer_below = atomic_load(&g_deferred_proof_validation_below_height);
            bool ibd_defer = (defer_below >= 0 &&
                              pindex_new->nHeight <= defer_below);
            if (!ibd_defer) {
                sqlite3 *pdb = progress_store_db();
                if (pdb && coins_kv_commitment(pdb, utxo_root) == 0) {
                    /* Persist before the leaf so a crash between append and
                     * save still lets the next catch-up reproduce this hash. */
                    if (!coins_kv_boundary_root_set(pdb, pindex_new->nHeight,
                                                    utxo_root))
                        LOG_WARN("tip_finalize",
                                 "boundary utxo_root persist failed h=%d "
                                 "(leaf still carries the computed root)",
                                 pindex_new->nHeight);
                } else {
                    memset(utxo_root, 0, 32);
                    LOG_WARN("tip_finalize",
                             "coins_kv_commitment failed h=%d; leaf carries "
                             "zero utxo_root sentinel", pindex_new->nHeight);
                }
            }
        }

        struct mmb_leaf leaf;
        mmb_leaf_from_block(&leaf,
            pindex_new->phashBlock->data,
            pindex_new->nHeight, pindex_new->nTime, pindex_new->nBits,
            pindex_new->hashFinalSaplingRoot.data,
            (const uint8_t *)pindex_new->nChainWork.pn,
            utxo_root);
        rpc_blockchain_mmb_append(&leaf);
    }

    block_free(&blk);
}
