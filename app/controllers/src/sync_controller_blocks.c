/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* sync_controller_blocks: connect/disconnect-block handlers and helpers that
 * walk block contents. sync_controller_internal.h owns cross-file glue for the
 * sync controller siblings. */

#include "controllers/sync_controller.h"
#include "sync_controller_internal.h"
#include "services/block_source_policy.h"
#include "services/recovery_policy.h"
#include "models/block.h"
#include "models/db_txn.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "chain/chain.h"
#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "wallet/sapling_keys.h"
#include "keys/key.h"
#include "core/hash.h"
#include "core/serialize.h"
#include "core/utiltime.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "storage/dbwrapper.h"
#include "storage/coins_db.h"
#include "coins/undo.h"
#include "validation/chainstate.h"
#include "validation/mirror_consensus.h"
#include "validation/txmempool.h"
#include "sapling/incremental_merkle_tree.h"
#include "sapling/sapling.h"
#include "sapling/note_encryption.h"
#include "support/cleanse.h"
#include "event/event.h"
#include "sync/sync_state.h"
#include "config/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

extern volatile sig_atomic_t g_shutdown_requested;

/* classify_script: use shared utxo_classify_script() from script/standard.h */
#define classify_script utxo_classify_script

struct connect_block_sync_ctx {
    const struct block *blk;
    const struct block_index *pindex;
    bool ok;
};

struct connect_block_async_ctx {
    struct block blk;
    struct block_index pindex;
    struct uint256 hash;
    bool copied;
};

/* Serialize a transaction to raw bytes. Caller must free.
 * Non-static: also used from sync_controller_writers.c and
 * sync_controller_catchup.c (mempool save). */
uint8_t *serialize_tx(const struct transaction *tx,
                      size_t *out_len)
{
    struct byte_stream s;
    stream_init(&s, 512);
    transaction_serialize(tx, &s);
    *out_len = s.size;
    return s.data;
}


/* Advance Sapling commitment tree and all wallet witnesses for one block.
 * Creates initial witnesses for wallet notes whose cm appears in this block.
 * Saves updated tree and witnesses to SQLite. */
bool advance_wallet_witnesses(struct node_db *ndb,
                                     const struct block *blk,
                                     struct incremental_merkle_tree *tree,
                                     int height)
{
    /* Load ALL unspent notes that may need initial witnesses. A fixed 256-cap
     * here (ORDER BY value DESC) left every note ranked below #256 without a
     * witness — unspendable — and froze the witness of any note demoted out of
     * the top-256 at a stale tree root. Size the load to the live count so
     * EVERY unspent note gets its witness created and advanced each block. */
    struct db_sapling_note *wnotes = NULL;
    int nw = db_sapling_note_list_unspent_alloc(ndb, &wnotes);
    if (nw < 0)
        LOG_FAIL("sync", "advance_wallet_witnesses: failed to load unspent notes");

    /* Track which notes already have witnesses (for initial witness creation) */
    bool *has_witness = nw > 0
        ? zcl_calloc((size_t)nw, sizeof(bool), "sync has_witness")
        : NULL;
    if (nw > 0 && !has_witness) {
        free(wnotes);
        LOG_FAIL("sync", "advance_wallet_witnesses: has_witness calloc failed (%d notes)", nw);
    }
    for (int i = 0; i < nw; i++) {
        uint8_t *wblob = NULL;
        size_t wlen = 0;
        int wh = 0;
        has_witness[i] = db_sapling_note_load_witness(ndb,
            wnotes[i].txid, wnotes[i].output_index,
            &wblob, &wlen, &wh) && wblob;
        if (wblob) free(wblob);
    }

    /* Deserialize existing witnesses for advancement */
    struct incremental_witness *witnesses = NULL;
    int num_with_witness = 0;
    int *witness_idx = NULL; /* maps witness array → wnotes index */
    if (nw > 0) {
        witnesses = zcl_calloc((size_t)nw, sizeof(struct incremental_witness), "sync witnesses");
        witness_idx = zcl_calloc((size_t)nw, sizeof(int), "sync witness_idx");
        for (int i = 0; i < nw; i++) {
            if (!has_witness[i]) continue;
            uint8_t *wblob = NULL;
            size_t wlen = 0;
            int wh = 0;
            if (!db_sapling_note_load_witness(ndb,
                    wnotes[i].txid, wnotes[i].output_index,
                    &wblob, &wlen, &wh) || !wblob)
                continue;
            struct byte_stream ws;
            stream_init_from_data(&ws, wblob, wlen);
            if (incremental_witness_deserialize(&witnesses[num_with_witness],
                    &ws, SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH,
                    tree->combine, tree->uncommitted)) {
                witness_idx[num_with_witness] = i;
                num_with_witness++;
            }
            free(wblob);
        }
    }

    /* Append each Sapling output commitment */
    bool has_sapling_outputs = false;
    bool ok = true;
    for (size_t i = 0; i < blk->num_vtx; i++) {
        const struct transaction *tx = &blk->vtx[i];
        for (size_t j = 0; j < tx->num_shielded_output; j++) {
            const struct uint256 *cm = &tx->v_shielded_output[j].cm;

            /* Advance all existing witnesses */
            for (int wi = 0; wi < num_with_witness; wi++)
                incremental_witness_append(&witnesses[wi], cm);

            /* Append to tree */
            incremental_tree_append(tree, cm);
            has_sapling_outputs = true;

            /* Check if cm matches a wallet note without a witness */
            for (int wi = 0; wi < nw; wi++) {
                if (has_witness[wi]) continue;
                if (memcmp(wnotes[wi].cm, cm->data, 32) != 0) continue;
                /* Create initial witness from current tree state */
                struct incremental_witness iw;
                incremental_witness_init(&iw, tree);
                struct byte_stream iwout;
                stream_init(&iwout, 2048);
                incremental_witness_serialize(&iw, &iwout);
                if (!db_sapling_note_save_witness(ndb,
                    wnotes[wi].txid, wnotes[wi].output_index,
                    iwout.data, iwout.size, height))
                    ok = false;
                stream_free(&iwout);
                has_witness[wi] = true;
                /* Add to witness advancement array for subsequent cms */
                if (witnesses) {
                    witnesses[num_with_witness] = iw;
                    witness_idx[num_with_witness] = wi;
                    num_with_witness++;
                }
                break;
            }
        }
    }

    /* Save all advanced witnesses */
    if (has_sapling_outputs) {
        for (int wi = 0; wi < num_with_witness; wi++) {
            struct byte_stream wout;
            stream_init(&wout, 2048);
            incremental_witness_serialize(&witnesses[wi], &wout);
            int idx = witness_idx[wi];
            if (!db_sapling_note_save_witness(ndb,
                wnotes[idx].txid, wnotes[idx].output_index,
                wout.data, wout.size, height))
                ok = false;
            stream_free(&wout);
        }
    }

    /* Save tree to node_state */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(tree, &ts);
        if (!node_db_state_set(ndb, "sapling_tree", ts.data, ts.size))
            ok = false;
        node_db_state_set_int(ndb, "sapling_tree_rebuild_height",
                              (int64_t)height);
        stream_free(&ts);
    }

    free(witnesses);
    free(witness_idx);
    free(has_witness);
    free(wnotes);
    return ok;
}

static bool node_db_sync_connect_block_local(struct node_db *ndb,
                                             const struct block *blk,
                                             const struct block_index *pindex)
{
    bool tx_active = false;
    const char *fail_reason = "unknown";

    if (!ndb || !ndb->open || !blk || !pindex || !pindex->phashBlock)
        LOG_FAIL("sync", "connect_block_local: invalid args (ndb=%p, blk=%p, pindex=%p)",
                 (void *)ndb, (void *)blk, (void *)pindex);

    /* Batch mode: start transaction if not already in one */
    if (!ndb->sync_in_batch) {
        if (!node_db_begin(ndb))
            LOG_FAIL("sync", "connect_block_local: BEGIN failed");
        ndb->sync_in_batch = true;
        ndb->sync_pending_blocks = 0;
        tx_active = true;
    } else {
        tx_active = true;
    }

    /* 1. Index the block header */
    struct db_block db_blk;
    memset(&db_blk, 0, sizeof(db_blk));
    memcpy(db_blk.hash, pindex->phashBlock->data, 32);
    db_blk.height = pindex->nHeight;
    /* Get prev_hash from block header (always available) rather than
     * pprev pointer (may be NULL if block_index has gaps) */
    memcpy(db_blk.prev_hash, blk->header.hashPrevBlock.data, 32);
    db_blk.version = blk->header.nVersion;
    memcpy(db_blk.merkle_root,
           blk->header.hashMerkleRoot.data, 32);
    db_blk.time = blk->header.nTime;
    db_blk.bits = blk->header.nBits;
    memcpy(db_blk.nonce, blk->header.nNonce.data, 32);
    db_blk.solution = (uint8_t *)blk->header.nSolution;
    db_blk.solution_len = blk->header.nSolutionSize;
    memcpy(db_blk.chain_work, pindex->nChainWork.pn, 32);
    db_blk.status = pindex->nStatus;
    db_blk.file_num = pindex->nFile;
    db_blk.data_pos = (int)pindex->nDataPos;
    db_blk.num_tx = (int)blk->num_vtx;

    /* Compute per-block shielded value from transactions */
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        for (size_t ji = 0; ji < tx->num_joinsplit; ji++)
            db_blk.sprout_value += tx->v_joinsplit[ji].vpub_old
                                  - tx->v_joinsplit[ji].vpub_new;
        db_blk.sapling_value += tx->value_balance;
    }

    if (!db_block_save_canonical(ndb, &db_blk)) {
        fail_reason = "db_block_save_canonical";
        goto fail;
    }

    /* 2. Index each transaction and update UTXOs */
    for (size_t i = 0; i < blk->num_vtx; i++) {
        const struct transaction *tx = &blk->vtx[i];

        /* Index the transaction */
        struct db_tx_index db_tx;
        memset(&db_tx, 0, sizeof(db_tx));
        memcpy(db_tx.txid, tx->hash.data, 32);
        memcpy(db_tx.block_hash,
               pindex->phashBlock->data, 32);
        db_tx.block_height = pindex->nHeight;
        db_tx.tx_index = (int)i;
        db_tx.file_num = pindex->nFile;
        db_tx.file_pos = (int)pindex->nDataPos;
        db_tx.is_coinbase = (i == 0);

        if (!db_tx_save(ndb, &db_tx)) {
            fail_reason = "db_tx_save";
            goto fail;
        }

        /* UTXOs are managed by coins_view_sqlite (the canonical UTXO store).
         * Do NOT write UTXOs here — sync_controller handles blocks, txs,
         * sapling nullifiers, and wallet scanning only. */

        /* Track Sapling nullifiers (spends) */
        for (size_t j = 0; j < tx->num_shielded_spend; j++) {
            sqlite3_stmt *ns = ndb->stmt_nullifier_insert;
            if (!ns) {
                fail_reason = "stmt_nullifier_insert_missing";
                goto fail;
            }
            sqlite3_reset(ns);
            if (sqlite3_bind_blob(ns, 1,
                    tx->v_shielded_spend[j].nullifier.data,
                    32, SQLITE_STATIC) != SQLITE_OK) {
                fail_reason = "nullifier_bind";
                goto fail;
            }
            if (AR_STEP_ROW_READONLY(ns) != SQLITE_DONE) {
                fail_reason = "nullifier_insert";
                goto fail;
            }
        }
    }

    /* 3. Update Sapling commitment tree + maintain wallet witnesses */
    {
        struct incremental_merkle_tree tree;
        sapling_tree_init(&tree);
        uint8_t tree_buf[8192];
        size_t tree_len = 0;
        if (node_db_state_get(ndb, "sapling_tree", tree_buf, sizeof(tree_buf), &tree_len)
            && tree_len > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tree_buf, tree_len);
            sapling_tree_init(&tree);
            incremental_tree_deserialize(&tree, &ts);
        }

        if (!advance_wallet_witnesses(ndb, blk, &tree, pindex->nHeight)) {
            fail_reason = "advance_wallet_witnesses";
            goto fail;
        }

        /* Verify tree root matches block header.
         * Smart mismatch policy:
         * - rebuilding flag set → accept silently (rebuild in progress)
         * - IBD with empty tree → accept (building from genesis)
         * - tree clearly broken (size < 500K at tip) → auto-set rebuild
         *   flag, log warning, accept block (will be fixed at next boot)
         * - tree was rebuilt (size >= 500K) but still wrong → FATAL
         *   reject (real consensus bug, not just stale state) */
        struct uint256 tree_root;
        incremental_tree_root(&tree, &tree_root);
        if (memcmp(tree_root.data,
                   blk->header.hashFinalSaplingRoot.data, 32) != 0) {
            bool is_ibd = (sync_get_state() <= SYNC_BLOCKS_DOWNLOAD);
            bool rebuilding = atomic_load(&g_sapling_tree_rebuilding);
            size_t tsize = incremental_tree_size(&tree);

            if (rebuilding) {
                /* Tree rebuild in progress — accept silently */
            } else if (is_ibd && tsize == 0) {
                /* IBD from genesis — tree building naturally */
            } else if (tsize < 500000 && pindex->nHeight > 500000) {
                /* Tree is clearly incomplete — auto-flag for rebuild.
                 * Accept blocks so the node keeps running. The tree
                 * will be rebuilt at next boot (Phase 1.2). */
                if (!atomic_load(&g_sapling_tree_rebuilding)) {
                    atomic_store(&g_sapling_tree_rebuilding, true);
                    LOG_WARN("sync", "Sapling tree incomplete " "(size=%zu at h=%d) — flagged for rebuild", tsize, pindex->nHeight);
                    fflush(stderr);
                }
            } else {
                /* Tree root doesn't match — log but accept the block.
                 * The tree will be rebuilt correctly at next boot.
                 * Never reject blocks based on Sapling tree state — the
                 * tree is a derived data structure, not consensus-critical
                 * for block acceptance (the header root was already
                 * validated by the peer that mined the block). */
                static int log_count = 0;
                if (log_count < 5) {
                    log_count++;
                    LOG_WARN("sync", "Sapling tree mismatch " "at h=%d (size=%zu) — will rebuild at next boot", pindex->nHeight, tsize);
                    fflush(stderr);
                }
                if (!atomic_load(&g_sapling_tree_rebuilding))
                    atomic_store(&g_sapling_tree_rebuilding, true);
            }
        }

        /* Store tree state per-block for disconnect */
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(&tree, &ts);
        if (!db_block_update_sapling_tree_data(ndb, pindex->phashBlock->data,
                                               ts.data, ts.size)) {
            fail_reason = "sapling_tree_update";
            stream_free(&ts);
            goto fail;
        }
        stream_free(&ts);
    }

    /* 4. Update chain tip in state table */
    if (!node_db_sync_set_tip(ndb,
            pindex->phashBlock->data, pindex->nHeight)) {
        fail_reason = "set_tip";
        goto fail;
    }

    /* Batch mode: commit only when batch_size reached */
    ndb->sync_pending_blocks++;
    int batch = ndb->sync_batch_size > 0 ? ndb->sync_batch_size : 1;
    if (ndb->sync_pending_blocks >= batch) {
        if (!node_db_commit(ndb)) {
            fail_reason = "commit";
            goto fail;
        }
        ndb->sync_in_batch = false;
        ndb->sync_pending_blocks = 0;
        tx_active = false;
    }
    return true;

fail:
    int sqlite_rc = (ndb && ndb->last_sqlite_rc != SQLITE_OK)
        ? ndb->last_sqlite_rc
        : ((ndb && ndb->db) ? sqlite3_errcode(ndb->db) : SQLITE_MISUSE);
    char last_op[64];
    snprintf(last_op, sizeof(last_op), "%s",
             (ndb && ndb->last_op[0]) ? ndb->last_op : "n/a");
    if (tx_active)
        node_db_rollback(ndb);
    ndb->sync_in_batch = false;
    ndb->sync_pending_blocks = 0;
    if (sqlite_rc == SQLITE_BUSY || sqlite_rc == SQLITE_LOCKED)
        mirror_consensus_record_blocker("db-writer-busy");
    LOG_FAIL("sync", "connect_block_local: failed at height %d "
             "reason=%s sqlite_rc=%d sqlite_msg=%s last_op=%s",
             pindex->nHeight, fail_reason,
             sqlite_rc, sqlite3_errstr(sqlite_rc), last_op);
}

static bool node_db_sync_connect_block_write(struct node_db *ndb, void *ctx)
{
    struct connect_block_sync_ctx *sync = ctx;

    if (!sync || !sync->blk || !sync->pindex)
        LOG_FAIL("sync", "connect_block_write: invalid ctx (sync=%p)", (void *)sync);
    sync->ok = node_db_sync_connect_block_local(ndb, sync->blk, sync->pindex);
    return sync->ok;
}

static bool node_db_sync_copy_block(struct block *dst, const struct block *src)
{
    if (!dst || !src)
        return false;

    block_init(dst);
    dst->header = src->header;
    dst->num_vtx = src->num_vtx;
    if (src->num_vtx == 0)
        return true;

    dst->vtx = zcl_calloc(src->num_vtx, sizeof(*dst->vtx),
                          "async projection block txs");
    if (!dst->vtx) {
        dst->num_vtx = 0;
        return false;
    }

    for (size_t i = 0; i < src->num_vtx; i++) {
        if (!transaction_copy(&dst->vtx[i], &src->vtx[i])) {
            dst->num_vtx = i;
            block_free(dst);
            return false;
        }
    }
    return true;
}

static bool node_db_sync_connect_block_async_write(struct node_db *ndb,
                                                   void *ctx)
{
    struct connect_block_async_ctx *async = ctx;

    if (!async || !async->copied)
        LOG_FAIL("sync", "connect_block_async_write: invalid ctx");
    return node_db_sync_connect_block_local(ndb, &async->blk,
                                            &async->pindex);
}

static void node_db_sync_connect_block_async_free(void *ctx)
{
    struct connect_block_async_ctx *async = ctx;

    if (!async)
        return;
    if (async->copied)
        block_free(&async->blk);
    free(async);
}

bool node_db_sync_connect_block(struct node_db *ndb,
                                const struct block *blk,
                                const struct block_index *pindex)
{
    struct connect_block_sync_ctx ctx = {
        .blk = blk,
        .pindex = pindex,
        .ok = false,
    };

    return sync_run_write(ndb, node_db_sync_connect_block_write, &ctx) &&
           ctx.ok;
}

bool node_db_sync_connect_block_async(struct node_db *ndb,
                                      const struct block *blk,
                                      const struct block_index *pindex)
{
    struct db_service *dbsvc = sync_db_service_for(ndb);
    struct connect_block_async_ctx *ctx;

    if (!ndb || !ndb->open || !blk || !pindex || !pindex->phashBlock)
        LOG_FAIL("sync", "connect_block_async: invalid args (ndb=%p, blk=%p, pindex=%p)",
                 (void *)ndb, (void *)blk, (void *)pindex);
    if (!dbsvc) {
        /* This is a derived projection of a block that consensus and the
         * coins view already accepted. During boot activation the runtime
         * DB service is not started yet, and falling back to a synchronous
         * write here lets SQLite projection contention stall canonical chain
         * advance. Leave the projection for later repair/backfill instead. */
        block_source_policy_note_projection_deferred(
            pindex->nHeight, "no_db_service");
        return true;
    }

    ctx = zcl_calloc(1, sizeof(*ctx), "async projection ctx");
    if (!ctx)
        return false;
    if (!node_db_sync_copy_block(&ctx->blk, blk)) {
        free(ctx);
        return false;
    }
    ctx->pindex = *pindex;
    ctx->hash = *pindex->phashBlock;
    ctx->pindex.phashBlock = &ctx->hash;
    ctx->copied = true;

    return db_service_enqueue_write(dbsvc,
                                    node_db_sync_connect_block_async_write,
                                    ctx,
                                    node_db_sync_connect_block_async_free);
}
