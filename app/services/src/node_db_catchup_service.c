/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* node_db_catchup_service: the bulk node_db sync-catchup orchestration.
 *
 * node_db_catchup_service_run is the core catchup algorithm lifted
 * verbatim out of sync_controller_catchup.c (node_db_sync_catchup). It
 * drives the turbo-mode scope, the DB connection verify, Sapling-tree
 * init, the main transaction/commit block-index loop (lean index +
 * wallet scan + witness advance), and turbo-mode teardown.
 *
 * The two block-level helpers it owns (sync_block_lean,
 * catchup_try_sapling_decrypt) are single-use orchestration details of
 * catchup and live here as statics. The shared sync-controller helpers
 * it calls (turbo scope, job-status setters, wallet-tx checked write,
 * witness advance, block-file mmap) are defined in the sync controller
 * siblings and reached here by forward declaration — their definitions
 * are not duplicated. */

// one-result-type-ok:recovery-primitive-int — E2 (one way out):
// node_db_catchup_service_run keeps the LOCKED plain-int contract (blocks
// indexed, or -1 on setup failure). Its single caller is the catchup job
// thread, which stores the int into job->result; the recovery-primitive
// int is consumed across the coins-wedge recovery surface. A zcl_result
// here buys nothing and forces a job-struct rewrite. Every failure path
// is logged via LOG_*/fprintf before the return, so the reason still
// travels with the failure.

#include "platform/time_compat.h"
#include "controllers/sync_controller.h"
#include "services/node_db_catchup_service.h"
#include "util/boot_progress.h"
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
#include "validation/txmempool.h"
#include "sapling/incremental_merkle_tree.h"
#include "sapling/sapling.h"
#include "sapling/note_encryption.h"
#include "support/cleanse.h"
#include "event/event.h"
#include "config/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

extern volatile sig_atomic_t g_shutdown_requested;

/* ── Sync-controller helpers reached by forward declaration ──
 * These are defined in the sync_controller_*.c siblings and declared in
 * the controller-private sync_controller_internal.h (which is not on the
 * services include path). The catchup body calls them but does not own
 * them; their declarations are mirrored here verbatim so the contract is
 * a single source of truth. */
struct sync_db_turbo_scope {
    struct node_db *ndb;
    bool entered;
};

bool sync_db_turbo_scope_begin(struct sync_db_turbo_scope *scope,
                               struct node_db *ndb,
                               bool enabled);
bool sync_db_turbo_scope_end(struct sync_db_turbo_scope *scope);

void sync_job_catchup_begin(int start_height, int target_height);
void sync_job_catchup_progress(int height);
void sync_job_catchup_finish(void);

bool node_db_sync_wallet_tx_checked(struct node_db *ndb,
                                    const struct transaction *tx,
                                    const struct wallet *w,
                                    int block_height,
                                    bool *is_ours_out,
                                    bool *success_out);

bool advance_wallet_witnesses(struct node_db *ndb,
                              const struct block *blk,
                              struct incremental_merkle_tree *tree,
                              int height);

uint8_t *sync_controller_mmap_block_file(const char *datadir,
                                         int file_num,
                                         size_t *out_size);

/* Lean index: block header + txid index only.
 * No UTXO tracking, no nullifiers, no solution blob.
 * ~5x fewer SQLite ops than sync_block_inner. */
static bool sync_block_lean(struct node_db *ndb,
                            const struct block *blk,
                            const struct block_index *pindex)
{
    if (!ndb || !ndb->open || !blk || !pindex)
        LOG_FAIL("sync", "sync_block_lean: invalid args (ndb=%p, blk=%p, pindex=%p)",
                 (void *)ndb, (void *)blk, (void *)pindex);

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
    if ((db_blk.status & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS)
        db_blk.status = (db_blk.status & ~BLOCK_VALID_MASK) |
                        BLOCK_VALID_TRANSACTIONS;
    db_blk.status |= BLOCK_HAVE_DATA;
    db_blk.file_num = pindex->nFile;
    db_blk.data_pos = (int)pindex->nDataPos;
    db_blk.num_tx = (int)blk->num_vtx;

    if (!db_block_save(ndb, &db_blk))
        LOG_FAIL("sync", "sync_block_lean: db_block_save failed at height %d",
                 pindex->nHeight);

    for (size_t i = 0; i < blk->num_vtx; i++) {
        const struct transaction *tx = &blk->vtx[i];

        struct db_tx_index db_tx;
        memset(&db_tx, 0, sizeof(db_tx));
        memcpy(db_tx.txid, tx->hash.data, 32);
        memcpy(db_tx.block_hash, pindex->phashBlock->data, 32);
        db_tx.block_height = pindex->nHeight;
        db_tx.tx_index = (int)i;
        db_tx.file_num = pindex->nFile;
        db_tx.file_pos = (int)pindex->nDataPos;
        db_tx.is_coinbase = (i == 0);
        if (!db_tx_save(ndb, &db_tx))
            LOG_FAIL("sync", "sync_block_lean: db_tx_save failed at height %d tx %zu",
                     pindex->nHeight, i);
    }

    return true;
}

/* Try-decrypt Sapling outputs in a transaction and save to SQLite.
 * Returns number of notes found. */
static int catchup_try_sapling_decrypt(struct node_db *ndb,
                                        const struct transaction *tx,
                                        const struct wallet *w,
                                        int height,
                                        bool *ok_out)
{
    if (!ndb || !tx || !w || tx->num_shielded_output == 0 ||
        w->sapling_keys.num_keys == 0) {
        if (ok_out)
            *ok_out = true;
        return 0;
    }

    int found = 0;
    bool ok = true;
    struct uint256 txid;
    {
        struct transaction *mtx = (struct transaction *)tx;
        transaction_compute_hash(mtx);
        txid = mtx->hash;
    }

    for (size_t oi = 0; oi < tx->num_shielded_output; oi++) {
        const struct output_description *od = &tx->v_shielded_output[oi];

        for (size_t ki = 0; ki < w->sapling_keys.num_keys; ki++) {
            const struct sapling_key_entry *ke = &w->sapling_keys.keys[ki];
            if (!ke->used)
                continue;

            uint8_t dhsecret[32];
            if (!sapling_ka_agree(od->ephemeral_key.data, ke->ivk, dhsecret))
                continue;

            uint8_t dec_key[32];
            if (!sapling_kdf(dec_key, dhsecret, od->ephemeral_key.data)) {
                memory_cleanse(dhsecret, sizeof(dhsecret));
                continue;
            }
            memory_cleanse(dhsecret, sizeof(dhsecret));

            uint8_t plaintext[564];
            if (!sapling_note_decrypt(dec_key, od->enc_ciphertext, 580,
                                      plaintext)) {
                memory_cleanse(dec_key, sizeof(dec_key));
                continue;
            }
            memory_cleanse(dec_key, sizeof(dec_key));

            if (plaintext[0] != 0x01)
                continue;

            uint8_t d[11];
            memcpy(d, plaintext + 1, sizeof(d));
            uint64_t value = 0;
            for (int b = 0; b < 8; b++)
                value |= ((uint64_t)plaintext[12 + b]) << (8 * b);
            uint8_t rcm[32];
            memcpy(rcm, plaintext + 20, sizeof(rcm));

            uint8_t pk_d[32];
            if (!sapling_ivk_to_pkd(ke->ivk, d, pk_d))
                continue;

            uint8_t cm[32];
            if (!sapling_compute_cm(d, pk_d, value, rcm, cm))
                continue;
            if (memcmp(cm, od->cm.data, sizeof(cm)) != 0)
                continue;

            uint8_t ak[32], nk[32];
            sapling_ask_to_ak(ke->xsk.expsk.ask, ak);
            sapling_nsk_to_nk(ke->xsk.expsk.nsk, nk);

            /* Position-0 placeholder nullifier. As in
             * wallet_try_sapling_decrypt (lib/wallet/src/wallet.c), the note's
             * absolute commitment-tree position is not available at decrypt
             * time; the spec nf is (re)computed at witness-creation time in
             * advance_wallet_witnesses() where position =
             * incremental_tree_size(tree) - 1 is exact. A guessed position
             * would be a WRONG nullifier, strictly worse than this non-blank
             * placeholder. See BUG #7. */
            uint8_t nf[32];
            sapling_compute_nf(d, pk_d, value, rcm, ak, nk, 0, nf);

            if (!node_db_sync_sapling_note(ndb, txid.data, (uint32_t)oi,
                                          (int64_t)value, rcm,
                                          plaintext + 52, 512,
                                          ke->ivk, d, pk_d, cm, nf,
                                          height)) {
                ok = false;
            } else {
                found++;
            }

            memory_cleanse(plaintext, sizeof(plaintext));
            if (!ok)
                break;

            break;
        }

        if (!ok)
            break;
    }
    if (ok_out)
        *ok_out = ok;
    return found;
}

int node_db_catchup_service_run(struct node_db *ndb,
                                const struct active_chain *chain,
                                const struct wallet *w,
                                const char *datadir)
{
    bool interrupted = false;
    bool tx_open = false;
    bool failed = false;
    int last_indexed_height = 0;
    int last_committed_height = -1;
    const struct block_index *last_indexed_tip = NULL;
    struct sync_db_turbo_scope turbo_mode = {0};
    bool restore_ok = true;

    if (!ndb || !ndb->open || !chain)
        LOG_ERR("sync", "catchup: invalid args (ndb=%p, chain=%p)", (void *)ndb, (void *)chain);

    int db_tip = node_db_sync_get_tip_height(ndb);
    int chain_tip = active_chain_height(chain);
    if (db_tip >= chain_tip) return 0;
    if (!datadir)
        LOG_ERR("sync", "catchup: datadir is NULL (db_tip=%d, chain_tip=%d)",
                db_tip, chain_tip);

    /* Keep pre-existing fast path behavior when there is no catchup work. */
    last_indexed_height = db_tip;
    last_committed_height = db_tip;

    int start = db_tip + 1;
    if (start < 0) start = 0;
    int total = chain_tip - start + 1;
    sync_job_catchup_begin(start, chain_tip);

    int wallet_keys = 0;
    if (w) {
        for (size_t i = 0; i < w->keystore.num_keys; i++)
            if (w->keystore.keys[i].used) wallet_keys++;
    }
    printf("SQLite catchup: %d blocks (%d..%d), lean index + wallet scan"
           " (%d keys)\n", total, start, chain_tip, wallet_keys);
    fflush(stdout);

    /* Turbo mode: disable fsync, drop indexes, enlarge cache. */
    bool bulk_mode = (total > 50000);
    if (!sync_db_turbo_scope_begin(&turbo_mode, ndb, bulk_mode)) {
        fprintf(stderr, "catchup: failed to enter turbo mode\n");
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }

    /* Verify connection works before starting */
    if (!node_db_begin(ndb)) {
        LOG_WARN("catchup", "catchup: BEGIN failed — aborting");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after BEGIN failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }
    tx_open = true;
    if (!node_db_commit(ndb)) {
        LOG_WARN("catchup", "catchup: initial COMMIT failed — aborting");
        node_db_rollback(ndb);
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after initial COMMIT failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }
    tx_open = false;

    int indexed = 0;
    int wallet_hits = 0;
    int batch_size = 100000;
    int64_t t_start = (int64_t)platform_time_wall_time_t();

    /* mmap cache */
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    /* Initialize Sapling commitment tree for catchup */
    struct incremental_merkle_tree sapling_tree;
    sapling_tree_init(&sapling_tree);
    {
        uint8_t tree_buf[8192];
        size_t tree_len = 0;
        if (node_db_state_get(ndb, "sapling_tree", tree_buf, sizeof(tree_buf), &tree_len)
            && tree_len > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tree_buf, tree_len);
            sapling_tree_init(&sapling_tree);
            incremental_tree_deserialize(&sapling_tree, &ts);
        }
    }

    if (!node_db_begin(ndb)) {
        LOG_WARN("catchup", "catchup: failed to open main transaction");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "catchup: failed to restore normal mode after tx open failure\n");
        restore_ok = false;
        sync_job_catchup_finish();
        return -1; // raw-return-ok:logged-above
    }
    tx_open = true;

    for (int h = start; h <= chain_tip; h++) {
        if (g_shutdown_requested) {
            interrupted = true;
            break;
        }
        /* Pump systemd watchdog liveness during long node_db sync-catchup
         * loops (block indexing replay). */
        if ((h % 100) == 0)
            boot_progress_tick("node_db_sync_catchup");
        const struct block_index *pindex = active_chain_at(chain, h);
        if (!pindex) continue;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) continue;

        /* mmap new file if needed */
        if (pindex->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            cached_data = sync_controller_mmap_block_file(
                datadir, pindex->nFile, &cached_size);
            cached_file = cached_data ? pindex->nFile : -1;
            if (!cached_data) {
                LOG_WARN("catchup", "catchup: failed to mmap file blk%05d.dat", pindex->nFile);
                failed = true;
                break;
            }
        }

        if (pindex->nDataPos >= cached_size) {
            LOG_INFO("catchup", "catchup: malformed block data offset at height %d", h);
            failed = true;
            break;
        }

        struct block blk;
        block_init(&blk);

        size_t remaining = cached_size - pindex->nDataPos;
        struct byte_stream s;
        stream_init_from_data(&s, cached_data + pindex->nDataPos,
                              remaining);
        if (!block_deserialize(&blk, &s)) {
            block_free(&blk);
            failed = true;
            break;
        }

        /* Lean index: block header + txid index */
        if (!sync_block_lean(ndb, &blk, pindex)) {
            LOG_WARN("catchup", "catchup: lean index failed at height %d (sqlite=%s)", h, sqlite3_errmsg(ndb->db));
            block_free(&blk);
            failed = true;
            break;
        }

        /* Wallet scan */
        if (w) {
            for (size_t i = 0; i < blk.num_vtx; i++) {
                bool tx_is_ours = false;
                bool tx_ok = false;
                if (!node_db_sync_wallet_tx_checked(ndb, &blk.vtx[i], w, h,
                                                   &tx_is_ours, &tx_ok) || !tx_ok) {
                    LOG_WARN("catchup", "catchup: wallet tx sync failed at height %d " "(tx=%d)", h, (int)i);
                    block_free(&blk);
                    failed = true;
                    break;
                }
                if (tx_is_ours)
                    wallet_hits++;
                bool decrypt_ok = true;
                catchup_try_sapling_decrypt(ndb, &blk.vtx[i], w, h,
                                           &decrypt_ok);
                if (!decrypt_ok) {
                    LOG_WARN("catchup", "catchup: sapling decrypt failed at height %d " "(tx=%d)", h, (int)i);
                    block_free(&blk);
                    failed = true;
                    break;
                }
                for (size_t si = 0; si < blk.vtx[i].num_shielded_spend; si++) {
                    struct transaction *mtx = (struct transaction *)&blk.vtx[i];
                    transaction_compute_hash(mtx);
                    /* Only ERROR is fatal; NOT_FOUND (not our note) is benign. */
                    if (node_db_sync_sapling_spend_ex(ndb,
                            blk.vtx[i].v_shielded_spend[si].nullifier.data,
                            mtx->hash.data) == DB_MARK_SPENT_ERROR) {
                        LOG_WARN("catchup", "catchup: sapling spend update failed at height %d " "(tx=%d, spend=%zu)", h, (int)i, si);
                        block_free(&blk);
                        failed = true;
                        break;
                    }
                }
                if (failed) break;
            }
        }
        if (failed) break;

        /* Advance Sapling tree + wallet witnesses */
        if (!advance_wallet_witnesses(ndb, &blk, &sapling_tree, h)) {
            LOG_WARN("catchup", "catchup: witness/tree advance failed at height %d", h);
            block_free(&blk);
            failed = true;
            break;
        }

        block_free(&blk);
        indexed++;
        last_indexed_height = h;
        last_indexed_tip = pindex;
        sync_job_catchup_progress(h);

        if (indexed % batch_size == 0) {
            if (pindex->phashBlock) {
                if (!node_db_sync_set_tip(ndb, pindex->phashBlock->data, h)) {
                    LOG_WARN("catchup", "catchup: failed to set tip at batch commit %d", h);
                    failed = true;
                    break;
                }
            } else {
                LOG_WARN("catchup", "catchup: missing hash at batch commit %d", h);
                failed = true;
                break;
            }
            if (!node_db_commit(ndb)) {
                LOG_WARN("catchup", "catchup: batch COMMIT failed at height %d", h);
                node_db_rollback(ndb);
                tx_open = false;
                failed = true;
                break;
            }
            tx_open = false;
            last_committed_height = h;
            int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
            int rate = elapsed > 0 ? indexed / (int)elapsed : 0;
            printf("SQLite: %d/%d blocks (height %d, %d blk/s, %d wallet txs)\n",
                   indexed, total, h, rate, wallet_hits);
            fflush(stdout);
            if (!node_db_begin(ndb)) {
                LOG_WARN("catchup", "catchup: failed to reopen transaction after batch commit");
                failed = true;
                break;
            }
            tx_open = true;
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    if (failed) {
        if (tx_open && !node_db_rollback(ndb))
            LOG_WARN("catchup", "catchup: rollback failed after failure");
        tx_open = false;
    }

    /* Final commit */
    if (tx_open && !failed) {
        if (last_indexed_tip && last_indexed_tip->phashBlock) {
            if (!node_db_sync_set_tip(ndb,
                                      last_indexed_tip->phashBlock->data,
                                      last_indexed_height)) {
                LOG_WARN("catchup", "catchup: failed to set tip before final commit");
                failed = true;
            }
        } else {
            LOG_WARN("catchup", "catchup: final commit missing tip hash");
            failed = true;
        }
        if (!failed) {
            if (!node_db_commit(ndb)) {
                LOG_WARN("catchup", "catchup: final COMMIT failed");
                if (!node_db_rollback(ndb))
                    LOG_WARN("catchup", "catchup: final ROLLBACK failed");
                tx_open = false;
                failed = true;
                last_indexed_height = last_committed_height;
            } else {
                tx_open = false;
                last_committed_height = last_indexed_height;
            }
        }
    }

    if (failed) {
        if (tx_open && !node_db_rollback(ndb))
            LOG_WARN("catchup", "catchup: final rollback path failed");
        interrupted = false;
    }

    /* Restore safe pragmas and rebuild indexes */
    if (!sync_db_turbo_scope_end(&turbo_mode)) {
        LOG_WARN("catchup", "catchup: failed to restore normal mode");
        restore_ok = false;
    }

    if (failed || !restore_ok) {
        sync_job_catchup_finish();
        LOG_ERR("sync", "catchup: aborting (failed=%d, restore_ok=%d, indexed=%d)",
                failed, restore_ok, indexed);
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    printf("SQLite catchup %s: %d blocks in %llds (%d blk/s, tip=%d)\n",
           interrupted ? "stopped" : "complete",
           indexed, (long long)elapsed,
           elapsed > 0 ? indexed / (int)elapsed : indexed,
           last_committed_height);
    fflush(stdout);

    /* Checkpoint WAL after bulk catchup to reclaim disk space */
    if (indexed > 10000)
        node_db_wal_checkpoint(ndb);

    sync_job_catchup_finish();
    return indexed;
}
