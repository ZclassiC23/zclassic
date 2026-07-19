/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Graceful-shutdown pipeline for the runtime services boot_services.c
 * starts: staged, deadline-watched teardown (emergency coins flush ->
 * frontend -> network quiesce -> runtime persist -> durability marker ->
 * best-effort release). Split from boot_services.c along the
 * app_shutdown_svc seam. */
#include "config/boot_internal.h"
#include "config/boot_background_workers.h"
#include "config/boot_snapshot_offer.h"
#include "config/boot_shutdown_marker.h"
#include "config/boot_fast_restart.h"
#include "config/boot_loop_guard.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "util/shutdown_stagewatch.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"
#include "util/log_macros.h"
#include "supervisors/self_heal.h"
#include "supervisors/staged_sync_supervisor.h"
#include "services/block_index_loader.h"
#include "controllers/blockchain_controller.h"
#include "event/event.h"
#include "net/msgprocessor.h"
#include "validation/process_block.h"
#include "validation/txmempool.h"
#include "validation/main_state.h"
#include "wallet/wallet.h"
#include "sapling/params_init.h"
#include "keys/pubkey.h"
#include "storage/txdb.h"
#include "storage/coins_view_sqlite.h"
#include "storage/progress_store.h"
#include "coins/utxo_commitment.h"
#include "coins/coins_view.h"
#include "kernel/service_kernel.h"
#include "models/database.h"
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>

static void shutdown_stop_frontend_services(struct boot_svc_ctx *svc)
{
    printf("[shutdown] stopping frontend services\n");
    zcl_service_kernel_stop_all(&svc->frontend_kernel);
    printf("[shutdown] frontend services stopped\n");
}

static void shutdown_persist_fast_restart_state(struct boot_svc_ctx *svc)
{
    printf("[shutdown] persisting fast restart state\n");
    /* >1 not >1000: persist small reducer chains (regtest generate) too, else the map restores empty and the finalized-tip seed no-op's (getblockcount=0). */
    if (svc->state->map_block_index.size > 1) {
        printf("Saving block index flat file (%zu entries)...\n",
               svc->state->map_block_index.size);
        save_block_index_flat(svc->datadir, svc->state);
    }
    printf("[shutdown] fast restart state persisted\n");
}

static bool shutdown_flush_coins_to_sqlite(struct boot_svc_ctx *svc,
                                           const char *label)
{
    const char *flush_label = label ? label : "shutdown";
    if (!svc || !svc->coins_sqlite || !svc->coins_tip) {
        LOG_WARN("shutdown",
                 "%s coins flush skipped: svc=%p coins_sqlite=%p "
                 "coins_tip=%p",
                 flush_label, (void *)svc,
                 svc ? (void *)svc->coins_sqlite : NULL,
                 svc ? (void *)svc->coins_tip : NULL);
        return false;
    }

    bool ok = coins_view_sqlite_batch_write( // one-write-path-ok:shutdown-single-writer
        svc->coins_sqlite, &svc->coins_tip->cache_coins,
        &svc->coins_tip->hash_block, &svc->coins_tip->commitment);
    if (!ok) {
        LOG_WARN("shutdown",
                 "%s coins flush failed; retaining %zu dirty entries",
                 flush_label, svc->coins_tip->cache_coins.size);
        return false;
    }

    coins_map_free(&svc->coins_tip->cache_coins);
    coins_map_init(&svc->coins_tip->cache_coins);
    utxo_commitment_init(&svc->coins_tip->commitment);
    return true;
}

static void shutdown_quiesce_network_and_flush_coins(struct boot_svc_ctx *svc)
{
    /* Stop P2P entrypoints before flush; any in-flight reducer sees
     * g_shutdown_requested and returns before mutating coins further. */
    printf("[shutdown] stopping network services\n");
    zcl_service_kernel_stop_all(&svc->network_kernel);
    printf("[shutdown] joining replay service\n");
    boot_join_replay_service(svc);
    msg_processor_stop_block_intake(svc->msg_processor);

    /* The message thread is finishing its current iteration; reducer
     * activation already handles shutdown persistence. */
    printf("Flushing coins cache to SQLite...\n");
    if (shutdown_flush_coins_to_sqlite(svc, "network-quiesce")) {
        printf("Coins cache flushed.\n");
    } else {
        fprintf(stderr, "WARNING: Coins cache flush FAILED during shutdown!\n");
    }
    /* Now join threads — safe, coins already persisted */
    printf("[shutdown] joining connman threads\n");
    connman_join(svc->connman);
    connman_free(svc->connman);
    printf("[shutdown] connman stopped\n");

    /* Final flush in case message thread connected blocks before exit. */
    (void)shutdown_flush_coins_to_sqlite(svc, "final");
    coins_view_cache_free(svc->coins_tip);
    coins_view_sqlite_close(svc->coins_sqlite);

    /* Close cached block file handles */
    disk_block_io_close_cache();
    printf("[shutdown] network quiesced and coins closed\n");
}

static void shutdown_persist_runtime_state(struct boot_svc_ctx *svc)
{
    printf("[shutdown] stopping runtime services\n");
    /* Stop + join the self-heal condition runner FIRST, while main_state and
     * the progress store are still live: the runner dereferences both inside a
     * condition tick, so it must never outlive them (they are freed in
     * shutdown_release_owned_resources). The global shutdown flag is already
     * set, so this joins at most one in-flight tick. */
    self_heal_stop();
    zcl_service_kernel_stop_all(&svc->runtime_kernel);
    /* Stop the supervisor AFTER runtime services so any stall-detection
     * callbacks they emit at teardown are still delivered. */
    supervisor_stop();
    printf("[shutdown] joining runtime workers\n");
    boot_join_address_backfill_service(svc);
    boot_join_hodl_history_service(svc);
    boot_join_tx_index_service(svc);
    boot_join_offer_service(svc);
    boot_join_projection_backfill_service(svc);
    boot_join_catchup_service(svc);

    rpc_blockchain_mmr_save(boot_node_db(svc));
    rpc_blockchain_mmb_save(boot_node_db(svc));
    rpc_blockchain_commitment_mmr_save(boot_node_db(svc));

    if (svc->block_tree_open) {
        block_tree_db_close(svc->block_tree);
        svc->block_tree_open = false;
    }

    if (svc->wallet_sqlite->open) {
        wallet_sqlite_flush(svc->wallet_sqlite, svc->wallet);
        wallet_sqlite_close(svc->wallet_sqlite);
    }
    if (svc->node_db->open) {
        db_service_flush_write(svc->db_service);
        node_db_sync_mempool_save(svc->node_db, svc->mempool);
        /* Checkpoint WAL before closing — prevents WAL corruption on
         * unclean restart and keeps the WAL file small. */
        if (node_db_wal_checkpoint(svc->node_db))
            printf("[shutdown] WAL checkpoint complete\n");
        else
            fprintf(stderr, "[shutdown] WAL checkpoint failed\n");
        db_service_close_write(svc->db_service);
    }
    printf("[shutdown] stopping DB/service kernels\n");
    boot_stop_db_service_kernel();
    zcl_service_kernel_stop_all(&svc->service_kernel);
    printf("[shutdown] runtime state persisted\n");
}

static void shutdown_release_owned_resources(struct boot_svc_ctx *svc)
{
    printf("[shutdown] releasing owned resources\n");
    app_runtime_set_current(NULL);
    zcl_service_kernel_reset(&svc->frontend_kernel);
    zcl_service_kernel_reset(&svc->runtime_kernel);
    zcl_service_kernel_reset(&svc->network_kernel);
    zcl_service_kernel_reset(&svc->service_kernel);
    /* Staged-sync stage teardown — the bottom-up (tip_finalize → utxo_apply →
     * … → header_admit) reverse-dependency shutdown the at-tip kill-9 ordering
     * invariant requires, BEFORE the frees below: a straggler drain ticked
     * after the join sweep must see cleared bindings, not freed chainstate
     * (each stage's shutdown reads the log the next-lower stage still owns, so
     * they must quiesce top-of-pipeline first). This lifecycle belongs to the
     * staged-sync supervisor unit that also registers the eight stages and
     * lists them in the supervisor tree; delegate to it instead of re-listing
     * every per-stage shutdown here. Behaviour is identical: each
     * *_stage_shutdown() is null-safe, so the unit's per-stage init_ok guard
     * only ever skips a no-op, in the same bottom-up order. */
    staged_sync_supervisor_shutdown_stages();

    /* Stages quiesced; the state they read can go. proof_validate uses the Sapling params. */
    wallet_free(svc->wallet);
    tx_mempool_free(svc->mempool);
    main_state_free(svc->state);
    sapling_free_params();
    /* Graceful checkpoint and close of progress.kv. No-op if never opened. */
    progress_store_close();

    boot_stop_projection_storage();

    ecc_verify_destroy();
    ecc_stop();
    printf("[shutdown] owned resources released\n");
}

void app_shutdown_svc(struct boot_svc_ctx *svc)
{
    extern volatile sig_atomic_t g_shutdown_requested;

    /* Per-stage deadlines (shutdown_stagewatch_enter below) replace the old
     * single alarm(90) cliff: each stage is timed + budgeted, a fired deadline
     * names its stage and escalates truthfully, and a datadir receipt records
     * the verdict so a forced-but-durable stop is never mis-reported as
     * failure. See util/shutdown_stagewatch.h. */
    shutdown_stagewatch_begin(svc->datadir);
    boot_loop_guard_note_shutdown_intent();   /* E2: exit-reason breadcrumb */

    atomic_store(svc->running, false);
    process_block_set_gap_fill_kick(NULL, NULL);
    process_block_set_tip_publication_hooks(NULL, NULL, NULL);
    g_shutdown_requested = 1;
    thread_registry_request_shutdown();
    event_emitf(EV_NODE_SHUTDOWN, 0, "graceful");
    event_async_stop();

    printf("Shutting down...\n");

    /* Emergency coins flush FIRST — minimize UTXO loss window.
     * SIGKILL from OOM killer / systemd timeout can arrive at any time
     * during shutdown. Flushing coins before anything else ensures the
     * UTXO state is safe even if the rest of shutdown is interrupted.
     * Durability-critical: a fired deadline grants a grace, never a skip. */
    shutdown_stagewatch_enter("emergency-coins-flush", 30, true, true);
    if (svc->coins_tip) {
        printf("Emergency coins flush...\n");
        (void)shutdown_flush_coins_to_sqlite(svc, "emergency");
        printf("Emergency flush done.\n");
    }

    /* I-7b phase-1: detach hot path observers from the feeder while
     * the network is still draining. New block_msg arrivals between
     * here and quiesce will short-circuit at the global hook. */

    shutdown_stagewatch_enter("frontend-stop", 15, false, true);
    shutdown_stop_frontend_services(svc);
    shutdown_stagewatch_enter("network-quiesce", 30, true, true);
    shutdown_quiesce_network_and_flush_coins(svc);
    /* runtime-persist holds the final WAL checkpoint + wallet flush + mempool
     * save — the slow-after-a-long-fold stage that used to breach the 90s
     * cliff. Durability-critical: never skipped, only graced. */
    shutdown_stagewatch_enter("runtime-persist", 45, true, true);
    shutdown_persist_runtime_state(svc);

    /* Tier-2 P2: record the fast-restart binding while state + progress.kv are
     * still live (values match the flat index saved just after the marker). */
    boot_fast_restart_capture_shutdown_facts(svc->state);

    /* Write the verified-clean shutdown marker HERE — node.db is now
     * WAL-checkpointed and closed, so its on-disk identity is final and binds
     * the next boot's quick_check-skip. This point is reached on BOTH the
     * straggler _exit(0) path below AND the normal completion, whereas
     * app_shutdown()'s later write is skipped when the straggler path _exit()s.
     * Idempotent: app_shutdown() may re-write the identical marker. */
    boot_shutdown_marker_write_clean(svc->datadir);

    /* THE durability point: coins flushed, node.db WAL-checkpointed + closed,
     * clean marker written. Everything past here is resumable at next boot, so
     * a fired deadline now forces a TRUTHFUL clean exit (0), not a false fail. */
    shutdown_stagewatch_mark_durable();

    /* Durability secured; only best-effort teardown follows. The block-index flat cache is written AFTER the marker (it previously preceded the checkpoint and lost the marker on a mid-teardown kill). */
    shutdown_stagewatch_enter("fast-restart-persist", 20, false, true);
    shutdown_persist_fast_restart_state(svc);
    shutdown_stagewatch_enter("thread-join", 15, false, true);
    {
        int stragglers = thread_registry_join_all(2);
        if (stragglers > 0) {
            /* A worker thread's bounded join timed out and it was detached, so
             * it may still be running. All durable state is already persisted
             * above (coins flushed, WAL checkpointed, DBs closed), so running
             * the destructive frees in shutdown_release_owned_resources would
             * race that thread (use-after-free on main_state / Sapling params /
             * caches it reads). Skip the frees and exit now — the OS reclaims
             * everything microseconds later. Durability was secured above, so
             * record the CLEAN receipt first: this is a truthful success. */
            fprintf(stderr,
                    "[shutdown] %d background thread(s) still finishing; state is "
                    "already saved, exiting now\n",
                    stragglers);
            shutdown_stagewatch_complete_clean();
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
    }
    /* I-7b phase-2: net threads are joined; safe to destroy. */
    shutdown_stagewatch_enter("release-resources", 15, false, true);
    shutdown_release_owned_resources(svc);

    printf("Shutdown complete.\n");
    /* Closes the last stage, cancels the alarm, writes the CLEAN receipt. */
    shutdown_stagewatch_complete_clean();
}
