#define _GNU_SOURCE  /* pthread_timedjoin_np */

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot background-workers unit — long-lived helper threads spawned by
 * app_init_services at scattered points during runtime startup and joined by
 * app_shutdown_svc. The start/join helpers are exposed via
 * boot_background_workers.h and called from their original sites in
 * boot_services.c, preserving boot order.
 *
 * Workers: payment_processor_thread, background_utxo_replay,
 * address_backfill_service_thread, hodl_history_worker_thread,
 * projection_backfill_service_thread. (The fast-sync snapshot-offer worker
 * moved to boot_snapshot_offer.c to keep this file under the E1 ceiling; it
 * shares the worker_on_stall / boot_register_worker_supervisor helpers
 * declared by boot_background_workers.h and implemented in
 * boot_worker_supervisor.c.)
 *
 * The worker bodies reach the boot context through the boot_services.c
 * accessors (boot_node_db / boot_db_service / boot_running /
 * boot_profile_has_file_service / boot_start_catchup_service /
 * boot_reap_catchup_service) declared in boot_internal.h. The generic thread
 * helpers move here; boot_join_thread_bounded is re-exported because the
 * catchup-job helpers that stay in boot_services.c reuse it.
 */

#include "platform/time_compat.h"
#include "config/boot_internal.h"
#include "config/boot_background_workers.h"
#include "config/boot_snapshot_import.h"
#include "services/chain_activation_service.h"
#include "services/chain_state_service.h"
#include "services/hodl_history_service.h"
#include "jobs/reducer_frontier.h"
#include "jobs/refold_progress.h"  /* refold_in_progress — suppress projection
                                    * backfill while a mint/refold discards the
                                    * upper tip (see the guard below). */
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "coins/coins_view.h"
#include "validation/process_block.h"
#include "rpc/legacy_chain_oracle.h"
#include "storage/disk_block_io.h"
#include "models/block.h"
#include "event/event.h"
#include "supervisors/domains.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

extern void store_process_payments(const char *datadir);
extern void *backfill_addresses_thread(void *arg);

/* Worker bodies — forward-declared so the start/join pairs below can name them
 * before their definitions further down (matching the original boot_services.c
 * forward declarations). */
static void *payment_processor_thread(void *arg);
static void *background_utxo_replay(void *arg);
static void *address_backfill_service_thread(void *arg);

/* ── Supervisor liveness (Model A — MONITOR; disk_monitor.c exemplar) ─
 *
 * Each background worker keeps its own pthread and gains an observe-only
 * liveness_contract registered ONCE in its domain at the worker's
 * boot_start_*_service. The supervisor never owns or tears down these
 * threads (period_secs == 0); it only watches the heartbeat the worker
 * emits and fires on_stall (log + EV_RECOVERY_ACTION) when the deadline
 * lapses. Behaviour-preserving — it cannot wedge a worker. (The
 * snapshot-offer worker follows the same model from boot_snapshot_offer.c,
 * sharing the helpers below.)
 *
 * Deadline/quiet (set at register; rationale at each heartbeat site):
 * looping workers (payment, projection_backfill, hodl_history) tick +
 * advance a progress marker once per loop; the two single-blocking-call
 * workers (utxo_replay, address_backfill) tick at entry/exit only and are
 * DEADLINE-ONLY (quiet 0) with a generous deadline so a legitimately long
 * call does not false-fire. Shared stall/register helper bodies live in
 * boot_worker_supervisor.c. */

#define PAYMENT_SUPERVISOR_DEADLINE_SEC             120
#define PROJECTION_BACKFILL_SUPERVISOR_DEADLINE_SEC 120
#define HODL_HISTORY_SUPERVISOR_DEADLINE_SEC        180
#define UTXO_REPLAY_SUPERVISOR_DEADLINE_SEC         3600
#define ADDRESS_BACKFILL_SUPERVISOR_DEADLINE_SEC    600

static struct liveness_contract g_payment_contract;
static struct liveness_contract g_projection_backfill_contract;
static struct liveness_contract g_hodl_history_contract;
static struct liveness_contract g_utxo_replay_contract;
static struct liveness_contract g_address_backfill_contract;

static _Atomic supervisor_child_id g_payment_sup_id = SUPERVISOR_INVALID_ID;
static _Atomic supervisor_child_id g_projection_backfill_sup_id =
    SUPERVISOR_INVALID_ID;
static _Atomic supervisor_child_id g_hodl_history_sup_id =
    SUPERVISOR_INVALID_ID;
static _Atomic supervisor_child_id g_utxo_replay_sup_id = SUPERVISOR_INVALID_ID;
static _Atomic supervisor_child_id g_address_backfill_sup_id =
    SUPERVISOR_INVALID_ID;

/* ── Generic boot thread helpers ───────────────────────────── */

/* Exposed via boot_background_workers.h (single source) so the snapshot-offer
 * worker lifted into boot_snapshot_offer.c spawns/joins through the same
 * plumbing rather than carrying its own raw pthread_create. */
bool boot_start_thread_service(pthread_t *thread,
                                      bool *started,
                                      void *(*entry)(void *),
                                      void *arg)
{
    if (!thread || !started || !entry || *started)
        return false;
    /* Generic boot service starter wrapper for composition-owned helper
     * threads. Callers own the pthread_t and join it explicitly. A
     * thread_registry_spawn_ex equivalent here would require a
     * name-from-caller param; deferred to a focused follow-up.
     * raw-pthread-ok */
    if (pthread_create(thread, NULL, entry, arg) != 0)
        return false;
    *started = true;
    return true;
}

static void boot_join_deadline_from_now(struct timespec *ts, int timeout_sec)
{
    platform_time_realtime_timespec(ts);
    if (timeout_sec < 0)
        timeout_sec = 0;
    ts->tv_sec += timeout_sec;
}

bool boot_join_thread_bounded(pthread_t thread,
                              const char *name,
                              int timeout_sec)
{
    struct timespec deadline;
    int rc;

    boot_join_deadline_from_now(&deadline, timeout_sec);
    rc = pthread_timedjoin_np(thread, NULL, &deadline);
    if (rc == 0)
        return true;

    if (rc == ETIMEDOUT) {
        fprintf(stderr,
                "[shutdown] %s join timed out after %ds; detaching\n",
                name ? name : "thread", timeout_sec);
    } else {
        fprintf(stderr,
                "[shutdown] %s join failed rc=%d (%s); detaching\n",
                name ? name : "thread", rc, strerror(rc));
    }
    pthread_detach(thread);
    return false;
}

void boot_join_thread_service_named(pthread_t *thread,
                                           bool *started,
                                           const char *name,
                                           int timeout_sec)
{
    if (!thread || !started || !*started)
        return;
    boot_join_thread_bounded(*thread, name, timeout_sec);
    *started = false;
}

/* ── Start/join pairs ──────────────────────────────────────── */

bool boot_start_payment_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    boot_register_worker_supervisor(&g_payment_sup_id, &g_payment_contract,
                                    &g_op_sup, "op.payment_processor",
                                    PAYMENT_SUPERVISOR_DEADLINE_SEC, 0);
    return boot_start_thread_service(&svc->payment_thread,
                                     &svc->payment_thread_started,
                                     payment_processor_thread, svc);
}

void boot_join_payment_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->payment_thread,
                                   &svc->payment_thread_started,
                                   "payment", 5);
}

bool boot_start_replay_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    boot_register_worker_supervisor(&g_utxo_replay_sup_id,
                                    &g_utxo_replay_contract, &g_chain_sup,
                                    "chain.background_utxo_replay",
                                    UTXO_REPLAY_SUPERVISOR_DEADLINE_SEC, 0);
    return boot_start_thread_service(&svc->replay_thread,
                                     &svc->replay_thread_started,
                                     background_utxo_replay, svc);
}

void boot_join_replay_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->replay_thread,
                                   &svc->replay_thread_started,
                                   "utxo_replay", 5);
}

bool boot_start_address_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir)
        return false;
    boot_register_worker_supervisor(&g_address_backfill_sup_id,
                                    &g_address_backfill_contract, &g_chain_sup,
                                    "chain.address_backfill",
                                    ADDRESS_BACKFILL_SUPERVISOR_DEADLINE_SEC, 0);
    return boot_start_thread_service(&svc->address_backfill_thread,
                                     &svc->address_backfill_thread_started,
                                     address_backfill_service_thread, svc);
}

void boot_join_address_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->address_backfill_thread,
                                   &svc->address_backfill_thread_started,
                                   "address_backfill", 5);
}

bool boot_start_tx_index_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir ||
        snapshot_tx_index_job_is_started(&svc->tx_index_job))
        return false;
    return snapshot_tx_index_job_start(&svc->tx_index_job, svc->datadir);
}

/* HODL history worker.
 *
 * Fills the hodl_history table incrementally so /explorer/hodl can
 * render a time-series of "% of supply held > 1 year" without
 * recomputing on every page hit. The worker walks every ~day
 * (HODL_HISTORY_SAMPLE_STRIDE blocks) up to the current chain tip,
 * idempotent on already-filled rows. Each fill call does one sample
 * then sleeps 1s so we never starve the database. */
static void *hodl_history_worker_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    int64_t loop_iterations = 0;
    if (!svc)
        return NULL;
    /* Initial settle: wait for boot to complete + first chain advance. */
    sleep(15);
    while (!svc->hodl_history_thread_stop) {
        supervisor_child_id sup_id = atomic_load(&g_hodl_history_sup_id);
        if (sup_id != SUPERVISOR_INVALID_ID) {
            supervisor_tick(sup_id);
            supervisor_progress(sup_id, ++loop_iterations);
        }
        struct node_db *ndb = boot_node_db(svc);
        if (ndb && ndb->open && svc->state) {
            int tip = active_chain_height(&svc->state->chain_active);
            if (tip > 0) {
                /* Fill at most 4 samples per tick (~one minute of work
                 * on a busy DB), then sleep so other readers move. */
                (void)hodl_history_fill_pending(ndb->db, tip, 4);
            }
        }
        /* Slow tick — 60s — until the table is caught up, then
         * effectively idle since fill_pending becomes a no-op. */
        for (int i = 0; i < 60 && !svc->hodl_history_thread_stop; i++)
            sleep(1);
    }
    return NULL;
}

static void *projection_backfill_service_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    int last_start_height = -1;
    int last_hole_rewind_height = -1;
    int hole_rewind_attempts = 0;
    bool hole_rewind_gave_up_reported = false;
    int64_t loop_iterations = 0;

    if (!svc)
        return NULL;

    while (!svc->projection_backfill_thread_stop && boot_running(svc)) {
        supervisor_child_id sup_id =
            atomic_load(&g_projection_backfill_sup_id);
        if (sup_id != SUPERVISOR_INVALID_ID) {
            supervisor_tick(sup_id);
            supervisor_progress(sup_id, ++loop_iterations);
        }
        struct node_db *ndb = boot_node_db(svc);
        int chain_tip = svc->state ?
            active_chain_height(&svc->state->chain_active) : -1;
        int projection_tip = -1;
        int projection_block_tip = -1;
        int first_missing_height = -1;

        boot_reap_catchup_service(svc);

        /* A from-genesis mint/refold (-mint-anchor / -refold-staged) RESETS the
         * reducer to genesis and is rebuilding the upper active chain that this
         * thread's cursor-rewind + catchup-start logic publishes. While that
         * fold runs, the active chain still names the (to-be-discarded) upper
         * torn tip, so node_db_sync_set_tip(rewind) below re-publishes it every
         * ~5 s; the sync-projection authority pair-self-check then resolves that
         * tip to a different height and refuses the write — hammering the
         * progress.kv lock against the fold for no benefit (the catchup itself
         * already short-circuits on the same signal in
         * node_db_catchup_service_run). Skip the whole backfill pass while a
         * refold is in progress; the first post-refold pass resumes it once
         * refold_in_progress() clears. Mirrors the proven guards in
         * node_db_catchup_service.c and utxo_mirror_sync_service.c. A normal
         * boot never sets the signal, so this branch is unchanged there. */
        if (refold_in_progress()) {
            for (int i = 0; i < 5 &&
                 !svc->projection_backfill_thread_stop &&
                 boot_running(svc); i++)
                sleep(1);
            continue;
        }

        if (ndb && ndb->open && chain_tip >= 0 &&
            !node_db_sync_catchup_job_is_started(&svc->catchup_job)) {
            projection_tip = node_db_sync_get_tip_height(ndb);
            projection_block_tip = db_block_max_height(ndb);
            if (projection_block_tip >= 0 &&
                projection_tip > projection_block_tip &&
                projection_block_tip < chain_tip) {
                struct block_index *rewind_tip =
                    active_chain_at(&svc->state->chain_active,
                                    projection_block_tip);
                if (rewind_tip && rewind_tip->phashBlock &&
                    node_db_sync_set_tip(ndb, rewind_tip->phashBlock->data,
                                         projection_block_tip)) {
                    event_emitf(EV_RECOVERY_ACTION, 0,
                                "projection-cursor-rewind from=%d to=%d",
                                projection_tip, projection_block_tip);
                    projection_tip = projection_block_tip;
                }
            }
            int repair_cap = projection_tip < chain_tip ? projection_tip : chain_tip;
            if (repair_cap >= 0 &&
                db_block_first_missing_connected_height(ndb, repair_cap,
                                                        &first_missing_height) &&
                first_missing_height >= 0 &&
                first_missing_height <= repair_cap) {
                if (first_missing_height != last_hole_rewind_height) {
                    last_hole_rewind_height = first_missing_height;
                    hole_rewind_attempts = 0;
                    hole_rewind_gave_up_reported = false;
                }
                if (hole_rewind_attempts < 3) {
                    int rewind_height = first_missing_height - 1;
                    bool rewound = false;
                    hole_rewind_attempts++;
                    if (rewind_height < 0) {
                        rewound = node_db_sync_reset_tip(ndb);
                    } else {
                        struct block_index *rewind_tip =
                            active_chain_at(&svc->state->chain_active,
                                            rewind_height);
                        rewound = rewind_tip && rewind_tip->phashBlock &&
                                  node_db_sync_set_tip(ndb,
                                                       rewind_tip->phashBlock->data,
                                                       rewind_height);
                    }
                    if (rewound) {
                        event_emitf(EV_RECOVERY_ACTION, 0,
                                    "projection-hole-rewind missing=%d "
                                    "from=%d to=%d attempt=%d",
                                    first_missing_height, projection_tip,
                                    rewind_height, hole_rewind_attempts);
                        projection_tip = rewind_height;
                    } else {
                        event_emitf(EV_RECOVERY_ACTION, 0,
                                    "projection-hole-rewind-unavailable "
                                    "missing=%d to=%d attempt=%d",
                                    first_missing_height, rewind_height,
                                    hole_rewind_attempts);
                    }
                } else if (!hole_rewind_gave_up_reported) {
                    event_emitf(EV_RECOVERY_ACTION, 0,
                                "projection-hole-repair-gave-up missing=%d "
                                "attempts=%d cap=%d",
                                first_missing_height, hole_rewind_attempts,
                                repair_cap);
                    hole_rewind_gave_up_reported = true;
                }
            } else if (first_missing_height < 0) {
                last_hole_rewind_height = -1;
                hole_rewind_attempts = 0;
                hole_rewind_gave_up_reported = false;
            }
            if (projection_tip < chain_tip) {
                if (last_start_height != chain_tip) {
                    event_emitf(EV_RECOVERY_ACTION, 0,
                                "projection-backfill-start from=%d to=%d",
                                projection_tip + 1, chain_tip);
                    last_start_height = chain_tip;
                }
                if (!boot_start_catchup_service(svc, svc->datadir)) {
                    fprintf(stderr,
                            "WARNING: projection backfill start failed "
                            "(projection=%d chain=%d)\n",
                            projection_tip, chain_tip);
                }
            }
        }

        for (int i = 0; i < 5 &&
             !svc->projection_backfill_thread_stop &&
             boot_running(svc); i++) {
            sleep(1);
        }
    }

    boot_reap_catchup_service(svc);
    return NULL;
}

bool boot_start_hodl_history_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir)
        return false;
    boot_register_worker_supervisor(&g_hodl_history_sup_id,
                                    &g_hodl_history_contract, &g_op_sup,
                                    "op.hodl_history",
                                    HODL_HISTORY_SUPERVISOR_DEADLINE_SEC, 0);
    svc->hodl_history_thread_stop = false;
    return boot_start_thread_service(&svc->hodl_history_thread,
                                     &svc->hodl_history_thread_started,
                                     hodl_history_worker_thread, svc);
}

void boot_join_hodl_history_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    svc->hodl_history_thread_stop = true;
    boot_join_thread_service_named(&svc->hodl_history_thread,
                                   &svc->hodl_history_thread_started,
                                   "hodl_history", 5);
}

bool boot_start_projection_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    boot_register_worker_supervisor(&g_projection_backfill_sup_id,
                                    &g_projection_backfill_contract, &g_op_sup,
                                    "op.projection_backfill",
                                    PROJECTION_BACKFILL_SUPERVISOR_DEADLINE_SEC,
                                    0);
    svc->projection_backfill_thread_stop = false;
    return boot_start_thread_service(&svc->projection_backfill_thread,
                                     &svc->projection_backfill_thread_started,
                                     projection_backfill_service_thread, svc);
}

void boot_join_projection_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    svc->projection_backfill_thread_stop = true;
    boot_join_thread_service_named(&svc->projection_backfill_thread,
                                   &svc->projection_backfill_thread_started,
                                   "projection_backfill", 5);
}

void boot_join_tx_index_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    if (!svc->tx_index_job.started)
        return;
    boot_join_thread_bounded(svc->tx_index_job.thread, "snapshot_tx_index", 5);
    svc->tx_index_job.started = false;
}

/* ── Helper threads ────────────────────────────────────────── */

/* Watchdog: detect a stuck chain and clear ONLY the TRANSIENT failure cause
 * (BLOCK_FAILED_CHILD with no BLOCK_FAILED_VALID of its own) on the next
 * block, so a child masked solely by an ancestor's now-cleared failure can be
 * retried. A block carrying its OWN BLOCK_FAILED_VALID is a deterministic,
 * self-attributed verdict (bad PoW / bad sapling root / consensus reject) that
 * a blind retry every 300s would re-admit as a forgery — leave it for the
 * re-derivation invariant (sticky-node plan #2/#11) to repair from peers. */
static void watchdog_check_stuck(struct boot_svc_ctx *svc)
{
    static int64_t last_height_change = 0;
    static int last_height = -1;

    int h = active_chain_height(&svc->state->chain_active);
    int64_t now = (int64_t)platform_time_wall_time_t();

    if (h != last_height) {
        last_height = h;
        last_height_change = now;
        return;
    }

    /* No progress for 5 minutes — try clearing the TRANSIENT failure cause on
     * the next block. We clear BLOCK_FAILED_CHILD only when the block does NOT
     * carry BLOCK_FAILED_VALID: a child failure can be stale once its ancestor
     * was repaired, whereas a self-attributed VALID failure is deterministic
     * and must NOT be blindly un-failed (that re-admits a forgery). */
    if (last_height_change > 0 && now - last_height_change > 300 && h > 100) {
        struct block_index *next = active_chain_at(
            &svc->state->chain_active, h + 1);
        if (!next) {
            /* Not in active chain — scan block map for height h+1 */
            size_t iter = 0;
            struct block_index *bi = NULL;
            while (block_map_next(&svc->state->map_block_index,
                                   &iter, NULL, &bi)) {
                if (bi && bi->nHeight == h + 1 &&
                    (bi->nStatus & BLOCK_FAILED_CHILD) &&
                    !(bi->nStatus & BLOCK_FAILED_VALID)) {
                    bi->nStatus &= ~BLOCK_FAILED_CHILD;
                    fprintf(stderr, "WATCHDOG: stuck at h=%d for %llds; "
                            "cleared transient BLOCK_FAILED_CHILD on h=%d "
                            "to retry\n", h,
                            (long long)(now - last_height_change),
                            bi->nHeight);
                }
            }
        } else if ((next->nStatus & BLOCK_FAILED_CHILD) &&
                   !(next->nStatus & BLOCK_FAILED_VALID)) {
            next->nStatus &= ~BLOCK_FAILED_CHILD;
            fprintf(stderr, "WATCHDOG: stuck at h=%d for %llds; cleared "
                    "transient BLOCK_FAILED_CHILD on h=%d to retry\n", h,
                    (long long)(now - last_height_change), next->nHeight);
        } else if (next->nStatus & BLOCK_FAILED_VALID) {
            /* Deterministic self-attributed failure: a blind clear would
             * re-admit a forgery. Leave it for the re-derivation invariant
             * (re-pull bytes from peers + re-run the stage) to repair. */
            fprintf(stderr, "WATCHDOG: stuck at h=%d for %llds; h=%d carries "
                    "BLOCK_FAILED_VALID (deterministic) — NOT clearing; "
                    "deferring to re-derivation\n", h,
                    (long long)(now - last_height_change), next->nHeight);
        }
        last_height_change = now; /* don't spam — wait another 5 min */
    }
}

static void *payment_processor_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    int64_t loop_iterations = 0;

    /* The frontend kernel spawns this thread BEFORE boot flips
     * svc->running true (boot_services.c starts the kernel ahead of the
     * atomic_store), so entering the while(boot_running) loop immediately
     * made the thread exit at birth on every boot: store payments never
     * ran and the orphaned supervisor contract fired the one-per-boot
     * op.payment_processor time_deadline stall. Wait (bounded) for boot
     * to finish; if it never flips, this was a real shutdown-during-boot
     * and exiting is correct. */
    for (int i = 0; i < 60 && !boot_running(svc); i++)
        sleep(1);

    while (boot_running(svc)) {
        supervisor_child_id sup_id = atomic_load(&g_payment_sup_id);
        if (sup_id != SUPERVISOR_INVALID_ID) {
            supervisor_tick(sup_id);
            supervisor_progress(sup_id, ++loop_iterations);
        }
        for (int i = 0; i < 30 && boot_running(svc); i++)
            sleep(1);
        if (!boot_running(svc))
            break;
        store_process_payments(svc->datadir);
        watchdog_check_stuck(svc);
    }
    return NULL;
}

static void *address_backfill_service_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    char *db_path;
    supervisor_child_id sup_id;

    if (!svc || !svc->datadir)
        return NULL;

    /* Single blocking call: heartbeat at entry and exit only. The
     * generous deadline (600 s) tolerates a legitimately long backfill;
     * progress_max_quiet_us == 0 disables the NO_PROGRESS gate so the
     * silence between entry and exit is not mistaken for a wedge. */
    sup_id = atomic_load(&g_address_backfill_sup_id);
    if (sup_id != SUPERVISOR_INVALID_ID)
        supervisor_tick(sup_id);

    db_path = zcl_malloc(1024, "address_backfill_db_path");
    if (!db_path)
        return NULL;
    snprintf(db_path, 1024, "%s/node.db", svc->datadir);
    backfill_addresses_thread(db_path);
    free(db_path);

    if (sup_id != SUPERVISOR_INVALID_ID)
        supervisor_tick(sup_id);
    return NULL;
}

/* ── Background UTXO replay ───────────────────────────────── */
/* After file sync, replay blocks to build UTXO set in background.
 * Node serves data immediately; UTXO set builds while running. */

_Atomic bool g_utxo_replay_active = false;
_Atomic int g_utxo_replay_height = 0;

/* boot_import_snapshot_db lives in config/src/boot_snapshot_import.c so
 * both boot.c (the pre-restore probe — the authoritative call site) and
 * this file (the late-receive path) share one implementation. */

static void *background_utxo_replay(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    const struct chain_params *params = chain_params_get();

    if (!svc || !svc->state || !svc->coins_tip || !params || !svc->datadir)
        return NULL;

    /* Single blocking activation call: heartbeat at entry and exit only.
     * Deep replay is slow sequential I/O, so the deadline is generous
     * (3600 s) and progress_max_quiet_us == 0 keeps the long silent run
     * from false-firing NO_PROGRESS. */
    supervisor_child_id sup_id = atomic_load(&g_utxo_replay_sup_id);
    if (sup_id != SUPERVISOR_INVALID_ID)
        supervisor_tick(sup_id);

    atomic_store(&g_utxo_replay_active, true);
    int64_t t0 = (int64_t)platform_time_wall_time_t();

    printf("UTXO replay: starting background chain validation...\n");
    fflush(stdout);

    /* ── Restore chain state from coins_best_block ──────────────
     * After snapshot import (file or P2P), coins_best_block in SQLite
     * points to the snapshot height, but the in-memory g_coins_tip and
     * active chain are still at genesis. We must advance both so the reducer
     * activation path starts from the snapshot height, not genesis.
     * Without this, connect_block fails at height 1 with BIP30 because
     * the snapshot's UTXOs include block 1's unspent coinbase. */
    struct node_db *ndb_restore = boot_node_db(svc);
    if (ndb_restore && ndb_restore->open) {
        /* Wave 2: locate the snapshot block from the DERIVED coins-best
         * (coins_kv's own co-committed state) first; the node_state
         * 'coins_best_block' key is only the legacy (!found) fallback. */
        struct uint256 cb_hash;
        uint256_set_null(&cb_hash);
        const char *cb_evidence = "snapshot_coins_best_block";
        {
            int32_t d_h = -1;
            uint8_t d_hash[32];
            bool d_hf = false;
            if (reducer_frontier_derive_coins_best_now(&d_h, d_hash,
                                                       &d_hf) && d_hf) {
                memcpy(cb_hash.data, d_hash, 32);
                cb_evidence = "derived_coins_best";
            }
        }
        if (uint256_is_null(&cb_hash)) {
            uint8_t cb_buf[32] = {0};
            size_t cb_len = 0;
            if (node_db_state_get(ndb_restore, "coins_best_block",
                                  cb_buf, sizeof(cb_buf), &cb_len) &&
                cb_len == 32)
                memcpy(cb_hash.data, cb_buf, 32);
        }
        {
            if (!uint256_is_null(&cb_hash)) {
                struct block_index *snap_block = block_map_find(
                    &svc->state->map_block_index, &cb_hash);
                if (snap_block && snap_block->nHeight > 0) {
                    struct chain_state_rollback_authorization rollback_auth = {
                        .source = CSR_ROLLBACK_SOURCE_SNAPSHOT,
                        .decision = POLICY_ALLOW,
                        .from_height = active_chain_height(
                            &svc->state->chain_active),
                        .to_height = snap_block->nHeight,
                        .max_depth = INT64_MAX,
                        .evidence_class = cb_evidence,
                        .reason = "utxo_replay_snapshot_restore",
                    };
                    struct chain_state_commit commit = {
                        .new_tip = snap_block,
                        .new_coins_best = cb_hash,
                        .expected_utxo_count = 0,
                        .update_header_tip = false,
                        .rollback_auth = &rollback_auth,
                        .wallet_scan_height = -1,
                        .reason = "utxo_replay_snapshot_restore",
                    };
                    enum csr_result rc = csr_commit_tip(csr_instance(),
                                                        &commit);
                    if (rc == CSR_OK) {
                        printf("UTXO replay: restored chain state from snapshot "
                               "at h=%d\n", snap_block->nHeight);
                    } else {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "UTXO replay: csr rejected snapshot restore "
                                "(%s) h=%d\n", csr_result_name(rc),
                                snap_block->nHeight);
                    }
                } else if (!snap_block) {
                    printf("UTXO replay: coins_best_block not in index "
                           "(waiting for P2P headers)\n");
                }
            }
        }
    }

    /* IBD turbo: skip non-essential work during replay */
    struct db_service *dbsvc = boot_db_service(svc);
    struct node_db *ndb = boot_node_db(svc);
    if (dbsvc) {
        db_service_ibd_turbo_mode(dbsvc);
        db_service_set_sync_batch_size(dbsvc, 1000);
    } else if (ndb && ndb->open) {
        node_db_ibd_turbo_mode(ndb);
        node_db_set_sync_batch_size(ndb, 1000);
    }
    /* Flush every 500 blocks even during IBD to limit UTXO loss on
     * SIGKILL. Previous value of 100000 meant a SIGKILL during boot
     * could lose 100K blocks of UTXO state, requiring full re-sync. */
    set_flush_policy(3600, 1000000, 500);

    {
        struct activation_exec_outcome outcome;
        activation_request_connect(boot_activation_controller(),
                                   ACTIVATION_SRC_UTXO_REPLAY,
                                   NULL, &outcome);
    }

    /* Restore normal flush policy */
    set_flush_policy(3600, 500000, 500);
    if (dbsvc) {
        if (!db_service_flush_write(dbsvc))
            fprintf(stderr, "UTXO replay: flush before normal mode failed\n");
        db_service_normal_mode(dbsvc);
        db_service_set_sync_batch_size(dbsvc, 100);
    } else if (ndb && ndb->open) {
        if (!node_db_sync_flush(ndb))
            fprintf(stderr, "UTXO replay: flush before normal mode failed\n");
        node_db_normal_mode(ndb);
        node_db_set_sync_batch_size(ndb, 100);
    }

    int tip = active_chain_height(&svc->state->chain_active);
    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    atomic_store(&g_utxo_replay_height, tip);
    atomic_store(&g_utxo_replay_active, false);

    printf("=== UTXO replay complete: tip=%d in %llds "
           "(%.0f blocks/sec) ===\n",
           tip, (long long)elapsed,
           elapsed > 0 ? (double)tip / (double)elapsed : 0);
    fflush(stdout);

    event_emitf(EV_NODE_READY, 0, "utxo_replay_done height=%d secs=%lld",
                tip, (long long)elapsed);

    if (sup_id != SUPERVISOR_INVALID_ID)
        supervisor_tick(sup_id);
    return NULL;
}
