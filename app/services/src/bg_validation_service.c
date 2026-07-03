/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright (c) 2014-2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */

#define _GNU_SOURCE  /* pthread_timedjoin_np */

/*
 *
 * Background Full Validation Service
 * -----------------------------------
 * After fast sync via FlyClient + SHA3, walks every block from genesis
 * and verifies all cryptographic proofs:
 *   - Equihash PoW solutions
 *   - ECDSA script signatures (every input of every transaction)
 *   - Ed25519 JoinSplit signatures
 *   - Sapling Groth16 spend/output proofs + binding signatures
 *   - Sprout Groth16 JoinSplit proofs
 *   - Merkle root integrity
 *
 * Uses a thread pool for parallel script verification within each block.
 * Saves progress to SQLite every 1000 blocks for crash-resume.
 * Resets g_deferred_proof_validation_below_height = -1 when complete.
 */

// one-result-type-ok:validation-progress-result
//
// The service result of this file is the validation PROGRESS — a single
// coherent type carried by struct bg_validation_progress (returned by
// bg_validation_get_progress) and the enum bg_validation_state within it
// (IDLE/RUNNING/PAUSED/COMPLETE/FAILED). A validation failure surfaces as
// progress.state = BG_VALIDATION_FAILED, not a reason-less bool.
// The bool/int helpers do NOT strip a failure reason:
//   - read_block_undo(), validate_block_proofs(), bg_validation_start() each
//     LOG_FAIL / LOG_WARN (with state.reject_reason) on every failure branch.
//   - load_progress() returns a height-or-sentinel and LOG_ERRs when absent.
//   - bg_validation_state_name() is the enum->name table.
// init/stop/reset are void lifecycle. Behavior bit-for-bit.

#include "platform/time_compat.h"
#include "services/bg_validation_service.h"
#include "bg_validation_internal.h"
#include "supervisors/domains.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/check_block.h"
#include "validation/contextual_check_tx.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"
#include "validation/main_constants.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "storage/disk_block_io.h"
#include "coins/undo.h"
#include "script/interpreter.h"
#include "script/script_flags.h"
#include "crypto/ed25519.h"
#include "sapling/sprout.h"
#include "sapling/bn254.h"
#include "sapling/sapling_prover.h"
#include "models/database.h"
#include "adapters/outbound/persistence/bg_validation_store_sqlite.h"
#include "ports/bg_validation_store_port.h"
#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sched.h>
#ifdef __GLIBC__
#include <malloc.h>  /* malloc_trim — return retained transient heap to the OS */
#endif
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"

/* Global instance for RPC access */
struct bg_validation_service *g_bg_validation = NULL;

/* The crash-resume cursor (state key "bg_validation_height") now lives
 * behind bg_validation_store_port; the sqlite adapter owns the key. */

/* ── How often to save progress and log ─────────────────────── */
#define SAVE_INTERVAL  1000
#define LOG_INTERVAL   10000
#define BG_VALIDATION_SUPERVISOR_DEADLINE_SEC 600

static _Atomic supervisor_child_id g_bg_validation_supervisor_id =
    SUPERVISOR_INVALID_ID;
static struct liveness_contract g_bg_validation_contract;

static int64_t bg_validation_progress_marker(
    const struct bg_validation_service *svc)
{
    if (!svc)
        LOG_RETURN((int64_t)-1, "bg_validation",
                   "bg_validation_progress_marker: null svc");
    return atomic_load(&svc->progress.verified_height);
}

static void bg_validation_supervisor_heartbeat(
    const struct bg_validation_service *svc)
{
    supervisor_child_id id = atomic_load(&g_bg_validation_supervisor_id);
    if (id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_tick(id);
    supervisor_progress(id, bg_validation_progress_marker(svc));
}

static void bg_validation_supervisor_done(void)
{
    supervisor_child_id id = atomic_load(&g_bg_validation_supervisor_id);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_set_deadline(id, 0);
}

static void bg_validation_on_stall(struct liveness_contract *c)
{
    const struct bg_validation_service *svc =
        c ? (const struct bg_validation_service *)c->ctx : NULL;
    const char *reason = c
        ? supervisor_stall_reason_name(
              (enum supervisor_stall_reason)atomic_load(&c->stall_reason))
        : "unknown";
    int verified = svc ? atomic_load(&svc->progress.verified_height) : -1;
    int chain_height = svc ? atomic_load(&svc->progress.chain_height) : -1;
    int state = svc ? atomic_load(&svc->progress.state) : BG_VALIDATION_IDLE;
    int64_t sigs = svc ? atomic_load(&svc->progress.sigs_verified) : 0;
    int64_t proofs = svc ? atomic_load(&svc->progress.proofs_verified) : 0;
    LOG_WARN("bg_validation",
             "[bg-valid] supervisor stall reason=%s verified=%d chain_height=%d state=%s sigs=%lld proofs=%lld",
             reason, verified, chain_height,
             bg_validation_state_name((enum bg_validation_state)state),
             (long long)sigs, (long long)proofs);
    event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
                "source=chain.bg_validation decision=worker_stall "
                "reason=%s verified=%d chain_height=%d state=%s sigs=%lld proofs=%lld",
                reason, verified, chain_height,
                bg_validation_state_name((enum bg_validation_state)state),
                (long long)sigs, (long long)proofs);
}

static bool bg_validation_register_supervisor(
    struct bg_validation_service *svc)
{
    if (!supervisor_start()) {
        LOG_FAIL("bg_validation", "bg_validation_start: supervisor_start failed");
        return false;
    }

    supervisor_child_id id = atomic_load(&g_bg_validation_supervisor_id);
    if (id != SUPERVISOR_INVALID_ID) {
        supervisor_set_deadline(id, BG_VALIDATION_SUPERVISOR_DEADLINE_SEC);
        supervisor_progress(id, bg_validation_progress_marker(svc));
        supervisor_tick(id);
        return true;
    }

    liveness_contract_init(&g_bg_validation_contract, "chain.bg_validation");
    atomic_store(&g_bg_validation_contract.period_secs, 0);
    atomic_store(&g_bg_validation_contract.deadline_secs,
                 BG_VALIDATION_SUPERVISOR_DEADLINE_SEC);
    atomic_store(&g_bg_validation_contract.progress_max_quiet_us, 0);
    g_bg_validation_contract.ctx = svc;
    g_bg_validation_contract.on_stall = bg_validation_on_stall;

    supervisor_domains_init();
    id = supervisor_register_in_domain(g_chain_sup, &g_bg_validation_contract);
    if (id == SUPERVISOR_INVALID_ID) {
        LOG_FAIL("bg_validation", "bg_validation_start: supervisor_register failed");
        return false;
    }
    atomic_store(&g_bg_validation_supervisor_id, id);
    supervisor_progress(id, bg_validation_progress_marker(svc));
    supervisor_tick(id);
    return true;
}

/* Parallel script verification (struct script_check_item +
 * bg_validation_verify_scripts_parallel) lives in
 * bg_validation_scripts.c — see bg_validation_internal.h. */

/* ── Read undo data for a block ──────────────────────────────── */

/* Maximum bytes to read for a single block's undo data.
 * Typical blocks need <1MB; even dust-attack blocks fit in 4MB.
 * This caps memory per-block without rejecting large rev files. */
#define MAX_UNDO_READ  (4 * 1024 * 1024)

static bool read_block_undo(struct block_undo *undo, const struct block_index *pindex,
                            const char *datadir)
{
    block_undo_init(undo);

    struct disk_block_pos undo_pos = { .nFile = -1, .nPos = 0 };
    if (!block_index_undo_pos_snapshot(pindex, &undo_pos, NULL)) return false; // raw-return-ok:missing-undo-is-counted-as-script-skip

    if (undo_pos.nPos == 0) LOG_FAIL("bg_validation", "read_block_undo: undo pos is 0 for file %d", undo_pos.nFile);

    char path[512];
    get_block_pos_filename(path, sizeof(path), datadir, &undo_pos, "rev");

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        LOG_FAIL("bg_validation", "read_block_undo: cannot open %s", path);

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        LOG_FAIL("bg_validation", "read_block_undo: fstat failed or empty file %s", path);
    }

    /* Read from undo_pos.nPos, capped at MAX_UNDO_READ.
     * The deserializer is stream-based and stops when done — we don't
     * need to read to EOF. This keeps memory bounded per block. */
    size_t avail = (size_t)(st.st_size - (off_t)undo_pos.nPos);
    if (avail == 0) {
        close(fd);
        LOG_FAIL("bg_validation", "read_block_undo: no data available at pos %u in %s",
                 undo_pos.nPos, path);
    }
    size_t read_len = avail < MAX_UNDO_READ ? avail : MAX_UNDO_READ;

    uint8_t *buf = zcl_malloc(read_len, "bg_valid undo buf");
    if (!buf) {
        close(fd);
        LOG_FAIL("bg_validation", "read_block_undo: malloc failed for %zu bytes", read_len);
    }

    ssize_t nread = pread(fd, buf, read_len, (off_t)undo_pos.nPos);
    close(fd);

    if (nread <= 0) {
        free(buf);
        LOG_FAIL("bg_validation", "read_block_undo: pread returned %zd for %s", nread, path);
    }

    struct byte_stream s;
    stream_init_from_data(&s, buf, (size_t)nread);
    bool ok = block_undo_deserialize(undo, &s);
    free(buf);
    return ok;
}

/* Shielded proof verification (bg_validation_verify_shielded_proofs)
 * lives in bg_validation_proofs.c — see bg_validation_internal.h. */

/* ── Single block full validation (read-only) ────────────────── */

/* Validates all cryptographic proofs in a block WITHOUT modifying UTXO set.
 * Verifies: Equihash, Merkle root, all script sigs, all shielded proofs.
 * Uses undo data (revXXXXX.dat) to recover spent outputs for sig verification.
 * max_script_batch: cap on script_check_item allocation (0 = unlimited). */
static bool validate_block_proofs(const struct block *block,
                                  struct block_index *pindex,
                                  const char *datadir,
                                  const struct chain_params *params,
                                  int num_workers,
                                  size_t max_script_batch,
                                  int64_t *sigs_out,
                                  int64_t *proofs_out,
                                  int64_t *skips_out)
{
    bool ok = false;
    struct validation_state state;
    validation_state_init(&state);
    int64_t sigs = 0, proofs = 0, skips = 0;
    struct block_undo blockundo;
    bool have_undo = false;
    struct script_check_item *check_items = NULL;
    size_t check_count = 0;

    /* 1. Block header: Equihash + PoW + timestamp */
    if (!check_block_header(&block->header, &state, params, true)) {
        LOG_WARN("bg-valid", "[bg-valid] check_block_header FAILED h=%d: %s",
                pindex->nHeight, state.reject_reason);
        goto out;
    }

    /* 2. Block structure: Merkle root + size limits + tx structure */
    if (!check_block(block, &state, params, true, true, false)) {
        LOG_WARN("bg-valid", "[bg-valid] check_block FAILED h=%d: %s",
                pindex->nHeight, state.reject_reason);
        goto out;
    }

    /* 3. Contextual header: difficulty, median time, checkpoints */
    if (pindex->pprev) {
        if (!contextual_check_block_header(&block->header, &state, params,
                                            pindex->pprev, true)) {
            LOG_WARN("bg-valid", "[bg-valid] contextual_check_header FAILED h=%d: %s",
                    pindex->nHeight, state.reject_reason);
            goto out;
        }
    }

    /* 4. Transaction-level verification */
    uint32_t branch_id = consensus_current_epoch_branch_id(
        pindex->nHeight, &params->consensus);
    uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

    if (block->num_vtx > 1)
        have_undo = read_block_undo(&blockundo, pindex, datadir);

    /* Count transparent inputs and cap allocation */
    size_t total_inputs = 0;
    for (size_t i = 1; i < block->num_vtx; i++)
        total_inputs += block->vtx[i].num_vin;

    size_t alloc_size = total_inputs;
    if (max_script_batch > 0 && alloc_size > max_script_batch)
        alloc_size = max_script_batch;

    if (alloc_size > 0) {
        check_items = zcl_calloc(alloc_size, sizeof(struct script_check_item), "bg_valid script checks");
        if (!check_items)
            goto out;
    }

    for (size_t i = 0; i < block->num_vtx; i++) {
        const struct transaction *tx = &block->vtx[i];

        /* 4a. Shielded proof verification */
        if (tx->num_joinsplit > 0 || tx->num_shielded_spend > 0 ||
            tx->num_shielded_output > 0) {
            if (!bg_validation_verify_shielded_proofs(tx, pindex->nHeight, i,
                                        branch_id, &proofs))
                goto out;
        }

        /* 4b. Collect script verification items */
        if (transaction_is_coinbase(tx))
            continue;

        size_t undo_idx = i - 1;
        bool have_tx_undo = have_undo && undo_idx < blockundo.num_txundo &&
                            blockundo.vtxundo[undo_idx].num_prevout == tx->num_vin;
        if (!have_tx_undo) {
            /* No undo (rev file): cannot recover spent outputs, so this tx
             * CANNOT be script-verified. Expected post-snapshot. Don't stall;
             * record the gap so "verified" stays honest, not a silent skip. */
            skips++;
            continue;
        }

        struct precomputed_tx_data txdata;
        precompute_tx_data(tx, &txdata);

        for (size_t j = 0; j < tx->num_vin; j++) {
            /* Flush batch if at capacity */
            if (max_script_batch > 0 && check_count >= max_script_batch) {
                if (!bg_validation_verify_scripts_parallel(check_items, check_count,
                                              num_workers))
                    goto out;
                check_count = 0;
            }

            const struct tx_out *prev_out =
                &blockundo.vtxundo[undo_idx].vprevout[j].txout;
            struct script_check_item *item = &check_items[check_count++];
            item->tx = tx;
            item->input_index = (unsigned int)j;
            item->amount = prev_out->value;
            item->branch_id = branch_id;
            item->txdata = txdata;
            item->script_pub_key = prev_out->script_pub_key;
            item->flags = flags;
            sigs++;
        }
    }

    /* 5. Final script verification flush */
    if (!bg_validation_verify_scripts_parallel(check_items, check_count, num_workers)) {
        LOG_WARN("bg-valid", "[bg-valid] script verification FAILED h=%d",
                pindex->nHeight);
        goto out;
    }

    if (skips > 0)
        LOG_WARN("bg-valid", "[bg-valid] h=%d: %lld non-coinbase tx(s) NOT "
                "script-verified (undo missing) — block advances, not fully "
                "verified", pindex->nHeight, (long long)skips);

    *sigs_out += sigs;
    *proofs_out += proofs;
    *skips_out += skips;
    ok = true;

out:
    free(check_items);
    if (have_undo)
        block_undo_free(&blockundo);
    return ok;
}

/* ── Load/save progress from SQLite ──────────────────────────── */

static int load_progress(const struct bg_validation_store_port *store)
{
    int val = -1;
    if (store && store->load_progress &&
        store->load_progress(store->self, &val))
        return val;
    LOG_ERR("bgv", "no saved progress found");
}

static void save_progress(const struct bg_validation_store_port *store,
                          int height)
{
    if (store && store->save_progress)
        store->save_progress(store->self, height);
}

/* Cumulative non-coinbase txs not script-verified (undo missing). Persisted
 * under a separate key so the tally survives restarts. -1 = never written. */
static int64_t load_skips(const struct bg_validation_store_port *store)
{
    int64_t val = -1;
    if (store && store->load_skips &&
        store->load_skips(store->self, &val))
        return val;
    return -1; // raw-return-ok:no-key-on-fresh-datadir-means-zero-skips-not-an-error
}

static void save_skips(const struct bg_validation_store_port *store,
                       int64_t skips)
{
    if (store && store->save_skips)
        store->save_skips(store->self, skips);
}

/* ── Main validation thread ──────────────────────────────────── */

static void *bg_validation_thread(void *arg)
{
    struct bg_validation_service *svc = arg;
    struct main_state *ms = svc->ms;
    const struct chain_params *params = svc->params;
    const char *datadir = svc->datadir;
    int num_workers = svc->num_workers;
    bg_validation_supervisor_heartbeat(svc);

    /* Load resume point */
    int start_height = load_progress(&svc->progress_store);
    if (start_height < 0)
        start_height = 0;
    else
        start_height++; /* Resume from next unverified block */

    int chain_height = active_chain_height(&ms->chain_active);
    atomic_store(&svc->progress.chain_height, chain_height);
    atomic_store(&svc->progress.verified_height, start_height - 1);
    atomic_store(&svc->progress.state, BG_VALIDATION_RUNNING);

    printf("[bg-valid] Starting full validation from height %d to %d "
           "(%d workers)\n", start_height, chain_height, num_workers);
    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "bg_validation start from=%d to=%d workers=%d",
                start_height, chain_height, num_workers);

    int64_t t_start = (int64_t)platform_time_wall_time_t();
    int64_t t_last_log = t_start;
    int h_last_log = start_height;
    int64_t total_sigs = 0;
    int64_t total_proofs = 0;
    int64_t ls = load_skips(&svc->progress_store);
    int64_t total_skips = ls < 0 ? 0 : ls;
    atomic_store(&svc->progress.script_verif_skipped_no_undo, total_skips);

    for (int h = start_height; h <= chain_height; h++) {
        if (atomic_load(&svc->stop_requested))
            break;
        bg_validation_supervisor_heartbeat(svc);

        /* Refresh chain height periodically (chain may advance) */
        if (h % 100 == 0) {
            chain_height = active_chain_height(&ms->chain_active);
            atomic_store(&svc->progress.chain_height, chain_height);
        }

        struct block_index *pindex = active_chain_at(&ms->chain_active, h);
        if (!pindex) {
            /* Block not yet in chain (snapshot anchor gap) — skip */
            continue;
        }

        /* Skip genesis (hardcoded, nothing to validate) and blocks
         * without valid disk positions */
        if (h == 0) continue;
        struct disk_block_pos pos;
        disk_block_pos_init(&pos);
        if (!block_index_disk_pos_snapshot(pindex, &pos, NULL)) {
            continue;
        }

        struct block blk;
        block_init(&blk);
        if (!read_block_from_disk_index_pread(&blk, pindex, datadir)) {
            /* Block file not on disk (e.g. post-snapshot, not yet
             * downloaded). Skip — these will be validated when they
             * arrive via delta sync with expensive_checks=true. */
            continue;
        }

        /* Full validation */
        int64_t block_sigs = 0, block_proofs = 0, block_skips = 0;
        if (!validate_block_proofs(&blk, pindex, datadir, params,
                                    num_workers, svc->max_script_batch,
                                    &block_sigs, &block_proofs, &block_skips)) {
            fprintf(stderr, "[bg-valid] VALIDATION FAILURE at height %d\n", h);
            atomic_store(&svc->progress.state, BG_VALIDATION_FAILED);
            block_free(&blk);
            bg_validation_supervisor_done();
            return NULL;
        }

        block_free(&blk);
        total_sigs += block_sigs;
        total_proofs += block_proofs;
        total_skips += block_skips;
        atomic_store(&svc->progress.verified_height, h);
        atomic_store(&svc->progress.sigs_verified, total_sigs);
        atomic_store(&svc->progress.proofs_verified, total_proofs);
        atomic_store(&svc->progress.script_verif_skipped_no_undo, total_skips);
        bg_validation_supervisor_heartbeat(svc);

        /* Save progress periodically */
        if (h % SAVE_INTERVAL == 0) {
            save_progress(&svc->progress_store, h);
            save_skips(&svc->progress_store, total_skips);

            /* Bound peak RSS. Every block here churns large transient
             * heap — a per-block undo buffer (up to MAX_UNDO_READ = 4 MB),
             * the script_check_item array, and a fully-deserialized block
             * (hundreds of small tx/vin/vout/joinsplit allocations) — all
             * freed before the next iteration. The PER-BLOCK footprint is
             * already bounded, but glibc keeps freed chunks in its arenas
             * instead of returning them to the OS, so RESIDENT memory
             * stair-steps upward as the walk deepens (~1.5 GB -> 2.4 GB+
             * over millions of blocks) and never falls back. malloc_trim
             * hands the retained pages back to the kernel, flattening peak
             * RSS. This is purely memory discipline — it does not change
             * what gets verified. Same idiom fast_sync.c uses after its
             * bulk UTXO serialization. Called once per SAVE_INTERVAL
             * (1000 blocks) so the cost is negligible vs. the per-block
             * crypto. */
#ifdef __GLIBC__
            malloc_trim(0);
#endif
        }

        /* Log progress */
        if (h % LOG_INTERVAL == 0 && h > start_height) {
            int64_t now = (int64_t)platform_time_wall_time_t();
            int64_t elapsed = now - t_last_log;
            double bps = elapsed > 0 ?
                (double)(h - h_last_log) / (double)elapsed : 0;
            int remaining = chain_height - h;
            int eta = bps > 0 ? (int)((double)remaining / bps) : 0;

            printf("[bg-valid] height %d/%d  %.0f blk/s  "
                   "%lld sigs  %lld proofs  ETA %dm%ds\n",
                   h, chain_height, bps,
                   (long long)total_sigs, (long long)total_proofs,
                   eta / 60, eta % 60);

            atomic_store(&svc->progress.blocks_per_sec, (int64_t)bps);
            t_last_log = now;
            h_last_log = h;
        }

        /* Yield CPU periodically to avoid starving the node */
        if (h % 100 == 0)
            sched_yield();
    }

    if (!atomic_load(&svc->stop_requested)) {
        /* Validation complete — save final progress */
        save_progress(&svc->progress_store, chain_height);
        atomic_store(&svc->progress.verified_height, chain_height);
        atomic_store(&svc->progress.state, BG_VALIDATION_COMPLETE);

        /* Only reset if we crossed the deferred-validation floor.
         * Otherwise an empty-chain run (chain_height==0) trivially
         * "completes" and clears the boot-time deferred=3,100,000
         * setting, which then makes incoming peer blocks at low heights
         * (h=737 etc.) fail phgr13 verify before the PHGR13 verifying
         * key is ready. */
        if (chain_height >= g_deferred_proof_validation_below_height)
            g_deferred_proof_validation_below_height = -1;

        int64_t total_time = (int64_t)platform_time_wall_time_t() - t_start;
        printf("[bg-valid] COMPLETE: %d blocks, %lld sigs, %lld proofs "
               "in %lldm%llds\n",
               chain_height - start_height + 1,
               (long long)total_sigs, (long long)total_proofs,
               (long long)(total_time / 60), (long long)(total_time % 60));
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "bg_validation complete height=%d sigs=%lld proofs=%lld "
                    "time=%llds",
                    chain_height, (long long)total_sigs,
                    (long long)total_proofs, (long long)total_time);
    } else {
        /* Stopped early — save where we got to */
        int verified = atomic_load(&svc->progress.verified_height);
        save_progress(&svc->progress_store, verified);
        printf("[bg-valid] Stopped at height %d (will resume next start)\n",
               verified);
    }

    bg_validation_supervisor_done();
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

void bg_validation_init(struct bg_validation_service *svc,
                        struct main_state *ms,
                        struct node_db *ndb,
                        const char *datadir,
                        const struct chain_params *params)
{
    memset(svc, 0, sizeof(*svc));
    svc->ms = ms;
    svc->ndb = ndb;
    svc->datadir = datadir;
    svc->params = params;
    svc->thread_started = false;
    atomic_store(&svc->stop_requested, false);

    /* Bind the crash-resume cursor store to the (already-open) node DB.
     * The sqlite adapter is the only code that names the DB for this
     * subsystem; the cursor key/semantics are unchanged. */
    bg_validation_store_sqlite_bind(ndb, &svc->progress_store);

    /* Use nproc/2 workers for parallel script verification, capped at 4.
     * pread()-based disk I/O is fully thread-safe, so multiple workers
     * can read blocks concurrently without the old FILE* cache races. */
    {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        int workers = (nproc > 0) ? (int)(nproc / 2) : 1;
        if (workers < 2) workers = 2;
        if (workers > 4) workers = 4;
        svc->num_workers = workers;
    }

    /* Auto-detect memory constraints.  On machines with <8GB, cap
     * the per-block script batch to reduce peak RSS. Each item is
     * ~200 bytes; 10K items ≈ 2MB — safe for any machine. */
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_sz = sysconf(_SC_PAGE_SIZE);
    int64_t ram_mb = (pages > 0 && page_sz > 0)
        ? (int64_t)pages * page_sz / (1024 * 1024) : 0;
    if (ram_mb > 0 && ram_mb < 8192)
        svc->max_script_batch = 10000;
    else
        svc->max_script_batch = 0;  /* unlimited on >=8GB machines */

    atomic_store(&svc->progress.state, BG_VALIDATION_IDLE);
    atomic_store(&svc->progress.verified_height, -1);
    atomic_store(&svc->progress.chain_height, 0);
    atomic_store(&svc->progress.sigs_verified, 0);
    atomic_store(&svc->progress.proofs_verified, 0);
    atomic_store(&svc->progress.blocks_per_sec, 0);
}

bool bg_validation_start(struct bg_validation_service *svc)
{
    if (!svc || svc->thread_started)
        LOG_FAIL("bg_validation", "bg_validation_start: null svc or thread already started");

    /* Don't start if already fully validated */
    int saved = load_progress(&svc->progress_store);
    int chain_h = active_chain_height(&svc->ms->chain_active);
    if (saved >= chain_h && chain_h > 0) {
        printf("[bg-valid] Already fully validated to height %d\n", saved);
        atomic_store(&svc->progress.state, BG_VALIDATION_COMPLETE);
        atomic_store(&svc->progress.verified_height, saved);
        atomic_store(&svc->progress.chain_height, chain_h);
        /* Only reset deferred-proof-validation if we've actually
         * validated PAST the checkpoint. Without this check, a fresh
         * datadir at chain_h=0 trivially satisfies saved>=chain_h,
         * marks bg-validation "complete", clears the deferred flag,
         * and then the very first peer block (e.g. h=737) fails
         * phgr13 verify because the chain hasn't caught up to where
         * proofs are expected to verify cleanly (and PHGR13 keys may
         * not even be loaded). Keep the boot-time deferred floor in
         * place until we actually cross it. */
        if (chain_h >= g_deferred_proof_validation_below_height)
            g_deferred_proof_validation_below_height = -1;
        return true;
    }

    /* Safety check: verify active_chain has valid entries at h=0 and h=1.
     * After block_map_grow, phashBlock pointers may be stale (fixed by
     * re-linking at boot). If entries are still bad, skip safely. */
    if (chain_h > 1000) {
        struct block_index *h0 = active_chain_at(&svc->ms->chain_active, 0);
        struct block_index *h1 = active_chain_at(&svc->ms->chain_active, 1);
        if (!h0 || !h1 || !(block_index_status_load(h0) & BLOCK_HAVE_DATA)) {
            printf("[bg-valid] Deferred — chain[0] or chain[1] not valid "
                   "(tip=%d)\n", chain_h);
            atomic_store(&svc->progress.state, BG_VALIDATION_COMPLETE);
            atomic_store(&svc->progress.verified_height, chain_h);
            atomic_store(&svc->progress.chain_height, chain_h);
            return true;
        }
    }

    atomic_store(&svc->stop_requested, false);
    if (!bg_validation_register_supervisor(svc))
        return false;
    if (thread_registry_spawn_ex("zcl_bg_valid", bg_validation_thread, svc,
                                  &svc->thread) != 0) {
        bg_validation_supervisor_done();
        LOG_FAIL("bg-valid", "failed to create thread");
    }
    svc->thread_started = true;
    return true;
}

void bg_validation_stop(struct bg_validation_service *svc)
{
    if (!svc || !svc->thread_started)
        return;
    bg_validation_supervisor_done();
    atomic_store(&svc->stop_requested, true);
    /* cap join at 5 s. bg-validation can be in the
     * middle of a slow signature/proof batch — better to detach than
     * to overrun TimeoutStopSec and earn a SIGKILL. */
    struct timespec ts;
    if (platform_time_realtime_timespec(&ts) == 0) {
        ts.tv_sec += 5;
        int rc = pthread_timedjoin_np(svc->thread, NULL, &ts);
        if (rc != 0) {
            LOG_WARN("bg_validation_stop", "bg_validation_stop: thread join timed out (rc=%d) " "— detaching", rc);
            pthread_detach(svc->thread);
        }
    } else {
        pthread_join(svc->thread, NULL);
    }
    svc->thread_started = false;
#ifdef ZCL_TESTING
    supervisor_child_id id = atomic_exchange(&g_bg_validation_supervisor_id,
                                             SUPERVISOR_INVALID_ID);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_unregister(id);
#endif
}

struct bg_validation_progress bg_validation_get_progress(
    const struct bg_validation_service *svc)
{
    struct bg_validation_progress p;
    p.verified_height = atomic_load(&svc->progress.verified_height);
    p.chain_height = atomic_load(&svc->progress.chain_height);
    p.sigs_verified = atomic_load(&svc->progress.sigs_verified);
    p.proofs_verified = atomic_load(&svc->progress.proofs_verified);
    p.blocks_per_sec = atomic_load(&svc->progress.blocks_per_sec);
    p.script_verif_skipped_no_undo =
        atomic_load(&svc->progress.script_verif_skipped_no_undo);
    p.state = atomic_load(&svc->progress.state);
    return p;
}

void bg_validation_reset(struct bg_validation_service *svc)
{
    if (!svc) return;
    bg_validation_stop(svc);
    save_progress(&svc->progress_store, -1);
    save_skips(&svc->progress_store, 0);
    atomic_store(&svc->progress.verified_height, -1);
    atomic_store(&svc->progress.sigs_verified, 0);
    atomic_store(&svc->progress.proofs_verified, 0);
    atomic_store(&svc->progress.script_verif_skipped_no_undo, 0);
    atomic_store(&svc->progress.blocks_per_sec, 0);
    atomic_store(&svc->progress.state, BG_VALIDATION_IDLE);
    printf("[bg-valid] Progress reset — will re-verify from block 0\n");
    bg_validation_start(svc);
}

const char *bg_validation_state_name(enum bg_validation_state state)
{
    switch (state) {
    case BG_VALIDATION_IDLE:     return "idle";
    case BG_VALIDATION_RUNNING:  return "running";
    case BG_VALIDATION_PAUSED:   return "paused";
    case BG_VALIDATION_COMPLETE: return "complete";
    case BG_VALIDATION_FAILED:   return "failed";
    }
    return "unknown";
}
