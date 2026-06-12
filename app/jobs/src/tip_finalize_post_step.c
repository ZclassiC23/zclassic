/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_post_step — reducer post-finalize side effects.
 * See tip_finalize_post_step.h for the contract.
 *
 * tip_finalize_run_post_finalize owns the wallet_sync / Sapling trial-decrypt
 * / nullifier-spend / mempool-remove / MMR / MMB side effects that used to
 * live inside the old block-connect engine. The reducer now runs them after
 * tip publication; the differences from the old inline source are mechanical,
 * not behavioural:
 *
 *   - the connected block is READ BACK from disk via
 *     stage_default_block_reader rather than received as a parameter (the
 *     legacy path already holds the block in hand);
 *   - the wallet / node_db / mempool handles are fetched through the
 *     public app_runtime_* accessors instead of the lib/validation private
 *     process_block_{wallet,mempool,node_db_internal}() accessors (both
 *     ultimately resolve to the same app_runtime context);
 *   - the per-stage timing logger (process_block_log_live_stage) and the
 *     "projection deferred" no-op comment block are dropped — neither is a
 *     side effect, and the design step scopes this to the six effects.
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
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "services/block_source_policy.h" /* projection-deferred diagnostic */
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

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));

    struct block blk;
    block_init(&blk);
    if (!stage_default_block_reader(&blk, pindex_new, datadir, NULL)) {
        /* No on-disk body (HAVE_DATA absent / read failed). The legacy
         * path always has the just-connected block in hand; here we read
         * it back from disk, so a missing body is a benign skip — the tip
         * still advanced, only the derived side effects are deferred.
         * The skip must be DIAGNOSED, never silent: all six side effects
         * (wallet sync, note decrypt, nullifier spend, mempool remove,
         * MMR, MMB) are dropped for this height. */
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
     * mismatch is the 28-blocks-late splice signature caught at block 1:
     * HOLD the pipeline (refuse h+1 onward) + PAGE. E13-neutral: the
     * block stays valid; only OUR pipeline holds. Crash-only: no FATAL,
     * side effects still run so the published tip stays coherent.
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

    /* Projection-deferred DIAGNOSTIC (preserved from the old inline
     * block-connect side effect).
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

    /* Append rich leaf to Merkle Mountain Belt (O(1) per block) */
    if (pindex_new->phashBlock) {
        struct mmb_leaf leaf;
        mmb_leaf_from_block(&leaf,
            pindex_new->phashBlock->data,
            pindex_new->nHeight, pindex_new->nTime, pindex_new->nBits,
            pindex_new->hashFinalSaplingRoot.data,
            (const uint8_t *)pindex_new->nChainWork.pn);
        rpc_blockchain_mmb_append(&leaf);
    }

    block_free(&blk);
}
