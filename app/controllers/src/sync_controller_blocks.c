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
        if (!witnesses || !witness_idx) {
            free(witnesses); free(witness_idx);
            free(has_witness); free(wnotes);
            LOG_FAIL("sync", "advance_wallet_witnesses: witness array calloc failed (%d notes)", nw);
        }
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

    /* Save tree to node_state — blob + height as one atomic pair (see
     * sapling_tree_persist_pair, lane/sapling-tree-persist). */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(tree, &ts);
        if (!sapling_tree_persist_pair(ndb, ts.data, ts.size,
                                       (int64_t)height))
            ok = false;
        stream_free(&ts);
    }

    free(witnesses);
    free(witness_idx);
    free(has_witness);
    free(wnotes);
    return ok;
}
