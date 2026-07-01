#define _GNU_SOURCE  /* pthread_timedjoin_np */

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Runtime service initialization: mempool, P2P, RPC, Tor, HTTPS,
 * mining, wallet sync, shutdown, and utility functions. */
#include "platform/time_compat.h"
#include "config/boot_internal.h"
#include "config/boot_background_workers.h"
#include "config/boot_flyclient.h"
#include "config/boot_snapshot_offer.h"
#include "config/boot_msg_callbacks.h"
#include "services/chain_activation_service.h"
#include "services/block_index_integrity.h"
#include "services/block_source_policy.h"
#include "services/chain_evidence_authority_service.h"
#include "services/chain_state_service.h"
#include "services/chain_tip.h"
#include "services/hodl_history_service.h"
#include "services/quorum_oracle_service.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/refold_progress.h"      /* refold_from_anchor_active (-load-verify-boot skip) */
#include "services/chain_tip_watchdog.h"
#include "services/sticky_escalator.h"
#include "services/invariant_sentinel.h"
#include "conditions/condition_registry.h"
#include "supervisors/domains.h"
#include "supervisors/self_heal.h"
#include "supervisors/net_supervisor.h"
#include "supervisors/chain_supervisor.h"
#include "supervisors/staged_sync_supervisor.h"
#include "services/node_health_service.h"
#include "health/heartbeat.h"
#include "util/sd_notify.h"
#include "util/alerts.h"
#include "util/boot_progress.h"
#include "util/log_macros.h"
#include "util/ar_step_readonly.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "util/blocker.h"
#include "net/connman.h"
#include "config/boot_snapshot_import.h"
#include "storage/disk_block_io.h"
#include "storage/event_log.h"
#include "storage/mempool_projection.h"
#include "storage/peers_projection.h"
#include "storage/event_log_singleton.h"
#include "storage/block_index_projection.h"
#include "storage/znam_projection.h"
#include "storage/wallet_projection.h"
#include "storage/small_projections.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "services/block_index_loader.h"
#include "models/block.h"
#include "models/file_offer.h"
#include "models/file_service.h"
#include "models/utxo.h"
#include "models/zmsg.h"
#include "chain/chainparams.h"
#include "chain/mmr.h"
#include "chain/subsidy.h"
#include "core/uint256.h"
#include "coins/coins_view.h"
#include "controllers/blockchain_controller.h"
#include "controllers/diagnostics_controller.h"
#include "controllers/hodl_controller.h"
#include "controllers/repair_controller.h"
#include "controllers/chain_inspect_controller.h"
#include "controllers/misc_controller.h"
#include "controllers/network_controller.h"
#include "controllers/mining_controller.h"
#include "controllers/file_controller.h"
#include "net/file_service.h"
#include "controllers/transaction_controller.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/explorer_controller.h"
#include "controllers/wallet_controller.h"
#include "controllers/zslp_controller.h"
#include "controllers/sync_controller.h"
#include "controllers/event_controller.h"
#include "controllers/snapshot_controller.h"
#include "controllers/game_controller.h"
#include "controllers/health_controller.h"
#include "controllers/file_market_controller.h"
#include "controllers/name_controller.h"
#include "controllers/messaging_controller.h"
#include "controllers/swap_controller.h"
#include "controllers/blog_controller.h"
#include "rpc/httpserver.h"
#include "rpc/legacy_chain_oracle.h"
#include "rpc/server.h"
#include "json/json.h"
#include "net/https_server.h"
#include "net/fast_sync.h"
#include "net/peer_lifecycle.h"
#include <limits.h>
#include "net/onion_service.h"
#include "net/peer_strategy.h"
#include "net/tor_integration.h"
#include "util/thread_registry.h"
#include "validation/mirror_consensus.h"
#include "validation/process_block.h"
#include "event/event.h"
#include "util/service_state.h"
#include "sync/sync_state.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "sapling/params_init.h"
#include <netdb.h>
#include <errno.h>

/* msg_version.c — external IP advertisement to peers */
extern void msg_version_set_external_ip(const char *ip_str, uint16_t port);
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include "services/mempool_limits.h"
#include "services/wallet_backup_service.h"
#include "services/disk_monitor.h"
#include "services/ibd_throttle.h"
#include "services/sync_monitor.h"
#include "net/download.h"
#include "services/db_maintenance.h"
#include "mcp/metrics.h"

extern _Atomic int g_deferred_proof_validation_below_height;


/* Boot context accessors. The handle is threaded explicitly by every caller;
 * the boot svc is owned by boot.c's g_svc, reached via boot_active_svc(). */
static struct app_runtime_context *boot_runtime(struct boot_svc_ctx *svc)
{
    if (!svc)
        return NULL;
    return &svc->runtime;
}

struct node_db *boot_node_db(struct boot_svc_ctx *svc)
{
    struct app_runtime_context *runtime = boot_runtime(svc);
    if (!runtime || !runtime->db_service)
        return NULL;
    return db_service_node_db(runtime->db_service);
}

struct db_service *boot_db_service(struct boot_svc_ctx *svc)
{
    struct app_runtime_context *runtime = boot_runtime(svc);
    if (!runtime)
        return NULL;
    return runtime->db_service;
}

static struct wallet *boot_wallet(struct boot_svc_ctx *svc)
{
    struct app_runtime_context *runtime = boot_runtime(svc);
    if (!runtime)
        return NULL;
    return runtime->wallet;
}

/* Runtime-profile gate accessors. Non-static (prototypes in boot_internal.h)
 * because several staying app_init call sites read them AND the frontend
 * service starts in boot_frontend_services.c gate on them across the TU
 * boundary; they remain co-located here beside boot_profile_has_file_service. */
bool boot_profile_has_explorer(const struct app_context *ctx)
{
    if (!ctx)
        return true;
    return app_runtime_profile_has_explorer(ctx->runtime_profile);
}

bool boot_profile_has_store(const struct app_context *ctx)
{
    if (!ctx)
        return true;
    return app_runtime_profile_has_store(ctx->runtime_profile);
}

bool boot_profile_has_onion(const struct app_context *ctx)
{
    return ctx && app_runtime_profile_has_onion(ctx->runtime_profile,
                                                ctx->tor);
}

/* FIX 1 seam (see boot_internal.h). PURE: no side effects. */
bool boot_loader_owns_seed(const struct app_context *ctx)
{
    return ctx && ctx->load_snapshot_at_own_height != NULL;
}

bool boot_profile_has_file_service(const struct app_context *ctx)
{
    if (!ctx)
        return true;
    return app_runtime_profile_has_file_service(ctx->runtime_profile);
}

/* Boot timing helper — mirrors boot.c:boot_clock_ms() so the
 * app_init_services sub-stage markers use the same monotonic-ms basis as
 * the top-level [boot] <phase> Nms markers. Timing only. */
static int64_t svc_clock_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool boot_mempool_limits_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->mempool)
        return false;

    struct mempool_limits_config ml_cfg;
    mempool_limits_config_defaults(&ml_cfg);
    struct zcl_result mr = mempool_limits_start(svc->mempool, &ml_cfg);
    if (!mr.ok) {
        fprintf(stderr, "[boot] %s:%d mempool_limits_start failed: code=%d %s\n",
                mr.source_file, mr.source_line, mr.code, mr.message);
        return false;
    }

    printf("Mempool limits started (max=%lldMB max_tx=%lld)\n",
           (long long)(ml_cfg.max_bytes >> 20),
           (long long)ml_cfg.max_tx_count);
    return true;
}

static void boot_mempool_limits_stop(void *ctx)
{
    (void)ctx;
    mempool_limits_stop();
}

static bool boot_connman_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    return svc && svc->connman && connman_start(svc->connman);
}

static void boot_connman_stop(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (svc && svc->connman)
        connman_signal_stop(svc->connman);
}

static int boot_known_zcl23_peers(void *ctx,
                                  struct connman_known_peer *out,
                                  size_t max)
{
    struct boot_svc_ctx *svc = ctx;
    struct db_peer peers[8];
    size_t want = max < 8 ? max : 8;

    if (!svc || !svc->node_db || !svc->node_db->open || !out || want == 0)
        return 0;

    int n = db_peer_fast_zcl23(svc->node_db, peers, want);
    if (n <= 0)
        return n;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].ip, peers[i].ip, 16);
        out[i].port = peers[i].port;
        out[i].services = peers[i].services;
    }
    return n;
}

static bool boot_register_network_services(struct boot_svc_ctx *svc)
{
    const struct zcl_service_spec connman_spec = {
        .name = "connman",
        .start = boot_connman_start,
        .stop = boot_connman_stop,
        .ctx = svc,
    };
    return zcl_service_kernel_register(&svc->network_kernel, &connman_spec);
}

static bool boot_register_runtime_services(struct boot_svc_ctx *svc)
{
    const struct zcl_service_spec specs[] = {
        {
            .name = "bg_validation",
            .start = boot_bg_validation_start,
            .stop = boot_bg_validation_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "bg_hash_verify",
            .start = boot_bg_hash_verify_start,
            .stop = boot_bg_hash_verify_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "header_probe",
            .start = boot_header_probe_start,
            .stop = boot_header_probe_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "gap_fill",
            .start = boot_gap_fill_start,
            .stop = boot_gap_fill_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "legacy_mirror",
            .start = boot_legacy_mirror_start,
            .stop = boot_legacy_mirror_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "zclassicd_oracle",
            .start = boot_zclassicd_oracle_start,
            .stop = boot_zclassicd_oracle_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "rolling_anchor",
            .start = boot_rolling_anchor_start,
            .stop = boot_rolling_anchor_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
        {
            .name = "sd_watchdog",
            .start = boot_sd_watchdog_start,
            .stop = boot_sd_watchdog_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        },
    };

    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        if (!zcl_service_kernel_register(&svc->runtime_kernel, &specs[i]))
            return false;
    }
    return boot_utxo_parity_register(svc) && boot_soak_attestation_register(svc) &&
           boot_canary_watch_register(svc) && /* parity + soak log + canary pager */
           boot_utxo_mirror_sync_register(svc); /* explorer-mirror feeder */
}

bool boot_running(const struct boot_svc_ctx *svc)
{
    return svc && svc->running && atomic_load(svc->running);
}

bool boot_start_catchup_service(struct boot_svc_ctx *svc,
                                const char *datadir)
{
    if (!svc || node_db_sync_catchup_job_is_started(&svc->catchup_job))
        return false;

    return node_db_sync_catchup_job_start(&svc->catchup_job, boot_node_db(svc),
                                          &svc->state->chain_active,
                                          svc->wallet, datadir);
}


static void boot_join_catchup_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    if (!svc->catchup_job.started)
        return;
    boot_join_thread_bounded(svc->catchup_job.thread, "catchup", 5);
    svc->catchup_job.started = false;
}

bool boot_reap_catchup_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->catchup_job.started)
        return true;
    if (!atomic_load(&svc->catchup_job.finished))
        return true;
    if (!boot_join_thread_bounded(svc->catchup_job.thread, "catchup", 1))
        return false;
    svc->catchup_job.started = false;
    return true;
}

/* ── Runtime service startup (called from app_init) ────────── */

bool app_init_services(struct app_context *ctx,
                        const struct chain_params *params,
                        struct boot_svc_ctx *svc)
{
    /* Timing-only: break p2p_services_start into its synchronous
     * sub-stages so projection-storage / file-sync / network /
     * RPC-register / frontend(Tor) / runtime costs are individually
     * visible. Markers reuse the existing [boot] <phase> Nms idiom on the
     * same monotonic-ms basis. */
    int64_t t_svc = svc_clock_ms();

    node_db_sync_catchup_job_init(&svc->catchup_job);
    snapshot_tx_index_job_init(&svc->tx_index_job);
    snapsync_init(&svc->snapshot_sync, svc->node_db);
    svc->app_ctx = ctx;
    svc->params = params;
    tx_mempool_init(svc->mempool, 1000);
    zcl_service_kernel_init(&svc->service_kernel);
    zcl_service_kernel_init(&svc->network_kernel);
    zcl_service_kernel_init(&svc->runtime_kernel);
    zcl_service_kernel_init(&svc->frontend_kernel);
    if (svc->db_service) {
        const struct zcl_service_spec mempool_limits_spec = {
            .name = "mempool_limits",
            .start = boot_mempool_limits_start,
            .stop = boot_mempool_limits_stop,
            .ctx = svc,
            .flags = ZCL_SERVICE_OPTIONAL,
        };
        if (!zcl_service_kernel_register(&svc->service_kernel,
                                         &mempool_limits_spec)) {
            fprintf(stderr, "FATAL: failed to register boot services\n");
            return false;
        }
        if (!zcl_service_kernel_start_all(&svc->service_kernel)) {
            fprintf(stderr, "FATAL: failed to start required boot services\n");
            return false;
        }
    }
    svc->runtime.db_service = svc->db_service;
    svc->runtime.snapshot_sync = &svc->snapshot_sync;
    svc->runtime.mempool = svc->mempool;
    svc->runtime.wallet = svc->wallet;
    app_runtime_set_current(&svc->runtime);
    boot_register_process_block_hooks(svc);

    /* Projection storage fan-out. Opens the append-only event log and the
     * reducer read-model projections used by runtime services. */
    boot_start_projection_storage(ctx->datadir, boot_node_db(svc));

    /* ── Register sync state observer ──────────────────────────── *
     * Logs every sync state transition via the event system.
     * Registered as async observer so it never blocks P2P threads. */
    extern void boot_sync_state_logger(enum event_type, uint32_t,
                                        const void *, uint32_t, void *);
    event_observe_async(EV_SYNC_STATE_CHANGE, boot_sync_state_logger, NULL);
    event_observe_async(EV_TIP_UPDATED, boot_sync_state_logger, NULL);
    event_observe_async(EV_BLOCK_CONNECTED, boot_sync_state_logger, NULL);
    event_observe_async(EV_REORG_START, boot_sync_state_logger, NULL);

    if (boot_node_db(svc))
        node_db_sync_mempool_load(boot_node_db(svc), svc->mempool);

    /* Rescan blockchain for wallet transactions if wallet is behind chain tip */
    {
        struct block_index *chain_tip = active_chain_tip(&svc->state->chain_active);
        int tip_height = active_chain_height(&svc->state->chain_active);
        if (chain_tip && svc->wallet->best_block_height < tip_height) {
            int scan_from = svc->wallet->best_block_height > 0
                ? svc->wallet->best_block_height + 1 : 0;
            if (svc->wallet->time_first_key > 0 && scan_from == 0) {
                int64_t scan_time = svc->wallet->time_first_key - 7200;
                for (int h = tip_height; h >= 0; h--) {
                    struct block_index *bi = active_chain_at(
                        &svc->state->chain_active, h);
                    if (bi && (int64_t)bi->nTime < scan_time) {
                        scan_from = h + 1;
                        break;
                    }
                }
            }
            if (scan_from == 0 && svc->wallet->best_block_height == 0 &&
                tip_height > 1000) {
                printf("Wallet scan height is 0 with %d blocks. "
                       "Use rescanblockchain RPC for targeted rescan.\n",
                       tip_height);
            } else if (tip_height - scan_from < 50000) {
                wallet_rescan(svc->wallet, &svc->state->chain_active,
                              scan_from, tip_height, ctx->datadir);
            } else {
                printf("Wallet needs rescan from %d to %d (%d blocks). "
                       "Deferring — use rescanblockchain RPC.\n",
                       scan_from, tip_height, tip_height - scan_from);
            }
        }
    }

    wallet_verify_utxos(svc->wallet, svc->coins_tip);

    /* Rebuild wallet_utxos from ground truth ONLY if empty */
    {
        struct node_db *ndb = boot_node_db(svc);
        if (ndb && ndb->open) {
            int64_t t0 = (int64_t)platform_time_wall_time_t();
            sqlite3_stmt *chk = NULL;
            int existing = 0;
            if (sqlite3_prepare_v2(ndb->db,
                    "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL",
                    -1, &chk, NULL) == SQLITE_OK) {
                if (AR_STEP_ROW_READONLY(chk) == SQLITE_ROW)
                    existing = sqlite3_column_int(chk, 0);
                sqlite3_finalize(chk);
            }
            if (existing > 0) {
                printf("wallet_utxos: keeping %d existing UTXOs (synced from zclassicd)\n",
                    existing);
            } else {
                char *err = NULL;
                int rc = sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);
                if (rc != SQLITE_OK) {
                    fprintf(stderr, "wallet_utxos: BEGIN failed: %s\n",
                            sqlite3_errmsg(ndb->db));
                } else {
                    rc = sqlite3_exec(ndb->db,
                    "INSERT OR IGNORE INTO wallet_utxos "
                    "(txid, vout, value, address_hash, script, height, is_coinbase) "
                    "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
                    "u.height, u.is_coinbase "
                    "FROM utxos u INNER JOIN wallet_keys wk "
                    "ON u.address_hash = wk.pubkey_hash",
                    NULL, NULL, &err);
                }
                if (err) {
                    printf("wallet_utxos INSERT: %s\n", err);
                    sqlite3_free(err);
                    err = NULL;
                }
                if (rc != SQLITE_OK) {
                    if (sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
                        fprintf(stderr, "wallet_utxos: ROLLBACK failed: %s\n",
                                sqlite3_errmsg(ndb->db));
                    }
                } else if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
                    fprintf(stderr, "wallet_utxos: COMMIT failed: %s\n",
                            sqlite3_errmsg(ndb->db));
                    if (sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
                        fprintf(stderr, "wallet_utxos: ROLLBACK after COMMIT failure failed: %s\n",
                                sqlite3_errmsg(ndb->db));
                    }
                }
            }
            int64_t bal = 0;
            sqlite3_stmt *s = NULL;
            sqlite3_prepare_v2(ndb->db,
                "SELECT COALESCE(sum(value),0) FROM wallet_utxos "
                "WHERE spent_txid IS NULL", -1, &s, NULL);
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
                bal = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
            int cnt = 0;
            sqlite3_prepare_v2(ndb->db,
                "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL",
                -1, &s, NULL);
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
                cnt = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
            printf("Wallet: %.8f ZCL (%d UTXOs, %lldms)\n",
                   (double)bal / 1e8, cnt,
                   (long long)((int64_t)platform_time_wall_time_t() - t0) * 1000);
        }
    }

    /* Sync wallet keys to SQLite */
    if (boot_node_db(svc))
        node_db_sync_wallet_keys(boot_node_db(svc), boot_wallet(svc));

    /* Pass base datadir; msg_processor_init re-resolves NET-SPECIFIC. */
    msg_processor_init(svc->msg_processor, svc->state, svc->mempool,
                       svc->coins_tip, params, ctx->datadir,
                       &svc->connman->manager, &svc->runtime);
    msg_processor_set_block_submit(svc->msg_processor,
                                   boot_submit_p2p_block, svc);
    msg_processor_set_compact_block_submit(svc->msg_processor,
                                           boot_submit_compact_block, svc);
    msg_processor_set_peer_save(svc->msg_processor, boot_save_peer_advisory,
                                svc);
    msg_processor_set_zmsg_save(svc->msg_processor, boot_save_zmsg, svc);
    msg_processor_set_file_offer_save(svc->msg_processor,
                                      boot_save_file_offer, svc);
    msg_processor_set_file_service_save(svc->msg_processor,
                                        boot_save_file_service, svc);
    msg_processor_set_snapshot_active(svc->msg_processor,
                                      boot_snapshot_active, svc);
    msg_processor_set_snapshot_anchor_accessors(
        svc->msg_processor, boot_snapshot_anchor_get, svc,
        boot_snapshot_anchor_set, svc);
    msg_processor_set_activation_hooks(
        svc->msg_processor, boot_request_header_activation, svc,
        boot_clear_header_activation_anchor, svc,
        boot_repair_header_post_activation_anchor, svc);
    msg_processor_set_header_index_hooks(
        svc->msg_processor, boot_scan_header_block_files, svc,
        boot_header_block_index_heights_repaired, svc);
    msg_processor_set_header_chainstate_hooks(
        svc->msg_processor, boot_commit_header_tip, svc,
        boot_recommit_snapshot_anchor, svc);
    msg_processor_set_wallet_tx_accepted(svc->msg_processor,
                                         boot_wallet_tx_accepted, svc);
    msg_processor_set_block_connected(svc->msg_processor,
                                      boot_block_connected_observer, svc);
    msg_processor_set_peer_header_vote(svc->msg_processor,
                                       boot_record_peer_header_vote, svc);
    msg_processor_set_flyclient_proof_builder(
        svc->msg_processor, boot_build_flyclient_proof, svc);
    msg_processor_set_block_hashes_range(
        svc->msg_processor, boot_load_block_hashes_range, svc);
    msg_processor_set_utxo_sha3_compute(
        svc->msg_processor, boot_compute_utxo_sha3, svc);

    /* Initialize P2P connection manager */
    struct node_signals signals = {
        .get_height = msg_get_height,
        .process_messages = msg_process_messages,
        .send_messages = msg_send_messages,
        .initialize_node = NULL,
        .finalize_node = NULL,
        .ctx = svc->msg_processor,
    };
    connman_init(svc->connman, params, &signals);
    svc->connman->datadir = ctx->datadir;
    connman_set_onion_peer_discovery(svc->connman, ctx->datadir,
                                     blog_discover_onion_peers);
    connman_set_known_zcl23_peer_source(svc->connman,
                                        boot_known_zcl23_peers, svc);
    onion_service_set_app_handlers(blog_serve, blog_discover_onion_peers);

    /* Load persisted peer addresses from previous session */
    connman_load_addrman(svc->connman);

    if (ctx->listen) {
        struct net_service bind4;
        net_service_init(&bind4);
        unsigned char any4[4] = {0, 0, 0, 0};
        net_addr_set_ipv4(&bind4.addr, any4);
        bind4.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&svc->connman->manager, &bind4, false))
            printf("P2P listening on 0.0.0.0:%d\n", ctx->p2p_port);
        struct net_service bind6;
        net_service_init(&bind6);
        bind6.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&svc->connman->manager, &bind6, false))
            printf("P2P listening on [::]:%d\n", ctx->p2p_port);
    }

    /* Wait for ZK params before P2P (needed for block verification) */
    if (ctx->params_dir) {
        if (svc->params_thread_started) {
            pthread_join(svc->params_thread, NULL);
            svc->params_thread_started = false;
        }
        if (!atomic_load(svc->params_loaded))
            fprintf(stderr, "Warning: ZK params not loaded\n");
    }

    printf("[boot]   %-28s %lldms\n", "svc.init_wallet",
           (long long)(svc_clock_ms() - t_svc));
    t_svc = svc_clock_ms();

    /* File sync BEFORE P2P — download block files first, then start P2P.
     * This prevents concurrent writes to block files (file sync + P2P
     * both writing to blk*.dat caused crashes). */
    {
        int chain_height = active_chain_height(&svc->state->chain_active);
        if (chain_height <= 0 && ctx->no_file_sync) {
            printf("=== Fresh node — file sync disabled (-nofilesync), "
                   "using P2P snapshot sync ===\n");
            goto skip_file_sync;
        }
        if (chain_height <= 0) {
            printf("=== Fresh node — probing optional fast file sync "
                   "(P2P sync is the fallback) ===\n");
            uint8_t utxo_root[32];
            memset(utxo_root, 0, 32);
            bool file_sync_ok = false;

            /* Try -fileservice= peer first (e.g., localhost speedrun) */
            if (ctx->file_service_peer && !file_sync_ok) {
                printf("Trying file service at %s:%d "
                       "(from -fileservice=)...\n",
                       ctx->file_service_peer, FS_PORT);
                int64_t t0 = (int64_t)platform_time_wall_time_t();
                if (fs_client_sync(ctx->file_service_peer, FS_PORT,
                                    ctx->datadir, utxo_root)) {
                    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
                    printf("=== File sync complete from %s: %llds ===\n",
                           ctx->file_service_peer, (long long)elapsed);
                    file_sync_ok = true;
                }
            }

            /* Fall back to hardcoded clearnet seeds ONLY if the operator
             * explicitly opted in with -allow-clearnet-snapshot-fetch.
             *
             * SECURITY: these seeds are unauthenticated (clearnet, no TLS, no
             * in-binary PoW-root binding). The file_service per-chunk SHA3 only
             * proves the bytes match the SERVING PEER's own manifest, NOT that
             * the chainstate is the real consensus set — and boot_import_snapshot_db
             * only consensus-verifies a snapshot AT the single compiled
             * checkpoint, trusting anything above it on the peer's word. So a
             * MITM or a malicious seed could otherwise seed a FORGED UTXO set
             * into a default cold start (forged-money / consensus divergence).
             * Default OFF: a fresh node falls back to safe P2P IBD or the
             * operator bundle (-load-snapshot-at-own-height, which IS anchor-
             * bound to the in-binary PoW header). An explicit -fileservice=PEER
             * above is always honored (the operator chose that peer).
             *
             * Also skipped in connect-only mode, where all bootstrap data must
             * come from the explicit peer set. */
            const char *file_seeds[] = {
                "205.209.104.118",
                "140.174.189.3",
                NULL
            };
            if (!ctx->allow_clearnet_snapshot_fetch && !file_sync_ok)
                printf("=== Auto-fetch of chainstate from hardcoded clearnet "
                       "seeds is DISABLED (unauthenticated; pass "
                       "-allow-clearnet-snapshot-fetch to opt in) — using P2P "
                       "snapshot sync / operator bundle ===\n");
            for (int round = 0;
                 ctx->allow_clearnet_snapshot_fetch &&
                 !ctx->connect_only && round < 3 && !file_sync_ok;
                 round++) {
                if (round > 0) {
                    printf("File sync: no seed reachable, retrying in 10s "
                           "(optional, round %d/3)...\n", round + 1);
                    sleep(10);
                }
                for (int i = 0; file_seeds[i] && !file_sync_ok; i++) {
                    printf("Probing optional file-sync seed %s:%d...\n",
                           file_seeds[i], FS_PORT);
                    int64_t t0 = (int64_t)platform_time_wall_time_t();
                    if (fs_client_sync(file_seeds[i], FS_PORT,
                                        ctx->datadir, utxo_root)) {
                        int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
                        printf("=== File sync complete from %s: %llds ===\n",
                               file_seeds[i], (long long)elapsed);
                        file_sync_ok = true;
                    }
                }
            }
            if (!ctx->connect_only && !file_sync_ok)
                printf("=== Optional file sync unavailable — continuing "
                       "with P2P snapshot sync (this is normal) ===\n");
            /* After file download: scan block files to populate block
             * index with BLOCK_HAVE_DATA + nChainTx. Without this,
             * 6 GB of blocks sit unused on disk.
             *
             * NOTE: blocks cannot be connected without a UTXO set. The file
             * service only downloads block files, not chainstate. Blocks are
             * indexed so they don't need to be re-downloaded via P2P — once
             * headers arrive and a UTXO snapshot is received, the blocks on
             * disk will be used automatically. */
            /* Run scan even on partial downloads — 94% of blocks is
             * still useful. Blocks on disk can serve headers + delta sync. */
            {
                /* Check if we have any block files at all */
                char blk0[576];
                snprintf(blk0, sizeof(blk0), "%s/blocks/blk00000.dat",
                         ctx->datadir);
                struct stat blk0_st;
                bool have_blocks = (stat(blk0, &blk0_st) == 0 &&
                                    blk0_st.st_size > 100000);
                if (!have_blocks && !file_sync_ok) goto skip_block_scan;
            }
            {
                /* Load block index from flat file if downloaded */
                char dl_flat[576];
                snprintf(dl_flat, sizeof(dl_flat), "%s/block_index.bin",
                         ctx->datadir);
                struct stat flat_st;
                if (stat(dl_flat, &flat_st) == 0 &&
                    flat_st.st_size > 1000000) {
                    printf("Loading downloaded block_index.bin...\n");
                    fflush(stdout);
                    load_block_index_flat(ctx->datadir, svc->state);
                }

                /* Validate block file references — clear HAVE_DATA for
                 * entries pointing to empty/missing block files. The flat
                 * file from the server may reference blk00000.dat which
                 * is empty (genesis has no on-disk data). */
                if (svc->state->map_block_index.size > 1000) {
                    int cleared = 0;
                    /* Build a quick lookup: which block files exist+nonempty */
                    bool file_ok[256] = {false};
                    for (int fi = 0; fi < 256; fi++) {
                        char bp[576];
                        snprintf(bp, sizeof(bp), "%s/blocks/blk%05d.dat",
                                 ctx->datadir, fi);
                        struct stat bst;
                        if (stat(bp, &bst) == 0 && bst.st_size > 0)
                            file_ok[fi] = true;
                    }
                    size_t vi = 0;
                    struct block_index *vp;
                    while (block_map_next(&svc->state->map_block_index,
                                           &vi, NULL, &vp)) {
                        if (!vp) continue;
                        if (!(vp->nStatus & BLOCK_HAVE_DATA)) continue;
                        if (vp->nFile >= 0 && vp->nFile < 256 &&
                            !file_ok[vp->nFile]) {
                            vp->nStatus &= ~BLOCK_HAVE_DATA;
                            cleared++;
                        }
                    }
                    if (cleared > 0)
                        printf("Cleared HAVE_DATA from %d entries "
                               "referencing empty block files\n", cleared);
                }

                /* If no flat file, scan block files from disk */
                if (svc->state->map_block_index.size < 1000) {
                    printf("Scanning downloaded block files...\n");
                    fflush(stdout);
                    int marked = scan_block_files_mark_data(svc->state,
                        ctx->datadir, params);
                    if (marked > 0) {
                        printf("Block file scan: %d blocks indexed\n",
                               marked);
                        save_block_index_flat(ctx->datadir, svc->state);
                    } else {
                        fprintf(stderr, "Block file scan: 0 blocks\n");
                    }
                }

                /* Check if we received consensus_snapshot.db (file_index=254).
                 * If so, import its UTXOs into node.db so the chain tip can
                 * be promoted to the snapshot height. Without this step the
                 * snapshot bytes sit on disk unused and the node falls back
                 * to block-by-block IBD from genesis. */
                bool has_utxo_snapshot = false;
                if (svc->state->map_block_index.size > 1000 &&
                    svc->node_db && svc->node_db->open && svc->node_db->db) {
                    char db_check[576];
                    snprintf(db_check, sizeof(db_check),
                             "%s/consensus_snapshot.db", ctx->datadir);
                    struct stat db_st;
                    if (stat(db_check, &db_st) == 0 &&
                        db_st.st_size > 10000000) {
                        /* Re-boot case: utxos already imported on a prior
                         * boot. node.db is authoritative; do not re-import. */
                        int64_t existing_utxos =
                            node_db_utxo_count(svc->node_db);
                        if (existing_utxos > 1000) {
                            printf("=== UTXO snapshot already imported "
                                   "(%lld UTXOs, %.0f MB on disk) ===\n",
                                   (long long)existing_utxos,
                                   (double)db_st.st_size /
                                       (1024.0 * 1024.0));
                            has_utxo_snapshot = true;
                        } else {
                            int64_t imported = 0, snap_h = 0;
                            uint8_t snap_hash[32];
                            if (boot_import_snapshot_db(svc->node_db,
                                                        db_check,
                                                        &imported,
                                                        &snap_h,
                                                        snap_hash)) {
                                printf("=== UTXO snapshot imported: "
                                       "%lld UTXOs at h=%lld "
                                       "(%.0f MB) ===\n",
                                       (long long)imported,
                                       (long long)snap_h,
                                       (double)db_st.st_size /
                                           (1024.0 * 1024.0));
                                has_utxo_snapshot = true;
                            } else {
                                printf("=== consensus_snapshot.db "
                                       "(%.0f MB) import failed — "
                                       "falling back to IBD ===\n",
                                       (double)db_st.st_size /
                                           (1024.0 * 1024.0));
                            }
                        }
                    }
                }

                if (svc->state->map_block_index.size > 1000) {
                    /* Block-file count on disk — NOT a coin-set verification. */
                    printf("=== Data synced: %zu blocks on disk ===\n",
                           svc->state->map_block_index.size);

                    if (has_utxo_snapshot) {
                        /* UTXO set already on disk from power node.
                         * Only need delta replay from snapshot height
                         * to current tip. This is fast — typically
                         * just the last few hundred blocks. */
                        printf("=== UTXO snapshot imported — "
                               "delta replay only ===\n");
                        /* Fresh receivers should not also start the store
                         * payment scanner. It opens a second node.db handle
                         * and can race the secure snapshot receive path. */
                        svc->defer_payment_service = true;
                        /* Fresh receivers should not also start the local
                         * snapshot/export builder on the shared DB during
                         * bootstrap. That work contends with secure
                         * snapshot receive and can lock the node DB right
                         * when FlyClient verification hands off to receive. */
                        svc->defer_offer_service = true;
                        /* Address aggregation is advisory and can be
                         * rebuilt later; snapshot receive is on the critical
                         * path. Keep bootstrap receivers single-writer until
                         * secure snapshot handoff completes. */
                        svc->want_address_backfill = false;
                    } else {
                        /* Skip full replay — ZCL23 peers will provide
                         * a UTXO snapshot in ~6 seconds. Replaying 3M
                         * blocks would take ~10 min and starve the P2P
                         * socket, preventing snapshot receipt. */
                        printf("=== No UTXO snapshot — waiting for P2P "
                               "snapshot from ZCL23 peers ===\n");
                    }
                    fflush(stdout);

                    /* Only replay if we have a UTXO snapshot from file
                     * service (delta replay). Otherwise, wait for P2P
                     * snapshot which is much faster than full replay. */
                    if (has_utxo_snapshot) {
                        if (!boot_start_replay_service(svc)) {
                            fprintf(stderr,
                                    "WARNING: failed to start tracked UTXO replay thread\n");
                        }
                    }
                } else if (active_chain_height(&svc->state->chain_active) <= 1) {
                    /* Fresh bootstrap receivers with no usable local chain
                     * data should consume secure sync, not waste startup time
                     * building local export/serve state they cannot use yet. */
                    svc->defer_payment_service = true;
                    svc->defer_offer_service = true;
                    svc->want_address_backfill = false;
                    printf("Fresh bootstrap receiver mode: deferring local serve/build work\n");
                }
            }
        skip_block_scan: ;
        }
    }
    skip_file_sync: ;

    printf("[boot]   %-28s %lldms\n", "svc.file_sync",
           (long long)(svc_clock_ms() - t_svc));
    t_svc = svc_clock_ms();

    if (!boot_register_network_services(svc) ||
        !zcl_service_kernel_start_all(&svc->network_kernel)) {
        fprintf(stderr, "FATAL: failed to start P2P threads\n");
        return false;
    }
    sync_set_state(SYNC_FINDING_PEERS, "P2P started");

    printf("[boot]   %-28s %lldms\n", "svc.network_start",
           (long long)(svc_clock_ms() - t_svc));
    t_svc = svc_clock_ms();

    /* Advertise our external IP in version messages so peers relay us */
    if (ctx->external_ip)
        msg_version_set_external_ip(ctx->external_ip,
                                    (uint16_t)ctx->p2p_port);

    /* Initialize RPC */
    rpc_table_init(svc->rpc_table);
    rpc_blockchain_set_state(svc->state, svc->mempool, ctx->datadir);
    rpc_blockchain_set_coins_db(NULL, svc->coins_tip);
    rpc_blockchain_set_node_db(boot_node_db(svc));
    rpc_blockchain_mmr_init_from_state(boot_node_db(svc));
    rpc_blockchain_mmr_catchup(svc->state);
    rpc_blockchain_mmb_init_from_state(boot_node_db(svc));
    rpc_blockchain_mmb_catchup(svc->state);

    boot_prepare_mmb_leaf_store(svc, ctx->datadir, legacy_chain_rpc_get_mmb_leaf);

    rpc_blockchain_commitment_mmr_init_from_state(boot_node_db(svc));

    /* Bootstrap commitment MMR if empty but chain is at height.
     * After legacy import, we have the UTXO set at tip but no
     * commitment history. Compute one commitment at current height
     * as the starting evidence anchor. Full history gets built during
     * reindexchainstate (full block replay). */
    {
        struct mmr *cm = rpc_blockchain_get_commitment_mmr();
        int chain_h = active_chain_height(&svc->state->chain_active);
        if (cm->num_leaves == 0 && chain_h > 1000 &&
            boot_node_db(svc) && boot_node_db(svc)->open) {
            printf("Commitment MMR empty at height %d — computing "
                   "bootstrap commitment...\n", chain_h);

            /* Round down to nearest commitment interval */
            int commit_h = (chain_h / MMR_COMMITMENT_INTERVAL) *
                            MMR_COMMITMENT_INTERVAL;

            /* Get block hash at commit height */
            const struct block_index *tip =
                active_chain_tip(&svc->state->chain_active);
            const struct block_index *bi = tip;
            /* Monotonicity + step-cap guard. */
            int bi_steps = 0;
            while (bi && bi->nHeight > commit_h) {
                const struct block_index *prev = bi->pprev;
                if (!prev || prev->nHeight >= bi->nHeight ||
                    bi_steps++ > 5000000) {
                    bi = NULL; /* corrupt chain — bail */
                    break;
                }
                bi = prev;
            }

            if (bi && bi->phashBlock && bi->nHeight == commit_h) {
                rpc_blockchain_maybe_commit(
                    commit_h, bi->phashBlock->data,
                    svc->coins_tip->commitment.accumulator,
                    svc->coins_tip->commitment.count);
                rpc_blockchain_commitment_mmr_save(boot_node_db(svc));
                printf("Bootstrap commitment at height %d saved\n",
                       commit_h);
            }
        }
    }
    register_blockchain_rpc_commands(svc->rpc_table);

    rpc_hodl_set_state(svc->state, svc->coins_tip, boot_node_db(svc),
                        ctx->datadir);
    register_hodl_rpc_commands(svc->rpc_table);
    rpc_repair_set_state(svc->state, svc->coins_tip, boot_node_db(svc),
                         ctx->datadir, params);
    register_repair_rpc_commands(svc->rpc_table);
    register_rebuild_recent_rpc_commands(svc->rpc_table);
    register_backfill_header_solutions_rpc_commands(svc->rpc_table);

    rpc_chain_inspect_set_state(svc->state, ctx->datadir,
                                 NULL, svc->coins_tip, boot_node_db(svc));
    register_chain_inspect_rpc_commands(svc->rpc_table);

    if (boot_profile_has_explorer(ctx)) {
        explorer_set_state(svc->state, svc->mempool, svc->coins_tip,
                            boot_node_db(svc), ctx->datadir);
    }

    api_set_state(svc->state, svc->mempool, svc->coins_tip,
                   boot_node_db(svc), ctx->datadir);

    rpc_rawtx_set_state(svc->state, svc->mempool, svc->coins_tip, ctx->datadir);
    rpc_rawtx_set_keystore(&svc->wallet->keystore);
    rpc_rawtx_set_connman(svc->connman);
    register_rawtransaction_rpc_commands(svc->rpc_table);

    rpc_mining_set_state(svc->state, svc->mempool, svc->coins_tip);
    register_mining_rpc_commands(svc->rpc_table);

    rpc_misc_set_state(svc->state);
    rpc_misc_set_wallet(svc->wallet);
    register_misc_rpc_commands(svc->rpc_table);
    rpc_net_set_connman(svc->connman);
    block_source_policy_init(svc->connman, svc->state,
                                   boot_node_db(svc));
    register_net_rpc_commands(svc->rpc_table);

    /* Game platform RPC — latency measurement, game types */
    rpc_game_set_connman(svc->connman);
    register_game_rpc_commands(svc->rpc_table);

    sync_monitor_init();
    sync_monitor_set_context(svc->connman, msg_get_download_mgr(),
                             svc->state);

    /* Service health and sync detail RPCs */
    rpc_health_set_state(svc->state, &svc->bg_validation,
                         &svc->bg_hash_verify, svc->connman);
    register_health_rpc_commands(svc->rpc_table);

    /* Diagnostics RPCs — dumpstate, getnodelog, dbquery */
    diagnostics_controller_set_state(svc->state, ctx->datadir);
    register_diagnostics_rpc_commands(svc->rpc_table);

    /* File transfer service — SHA3-verified chunk serving */
    if (boot_profile_has_file_service(ctx)) {
        file_controller_init(ctx->datadir);
        register_file_rpc_commands(svc->rpc_table);
    }

    /* ZCL Market — crypto-incentivized file sharing */
    if (boot_profile_has_store(ctx)) {
        rpc_market_set_state(boot_node_db(svc));
        register_market_rpc_commands(svc->rpc_table);
    }

    /* ZCL Names — on-chain name registry */
    rpc_name_set_state(boot_node_db(svc));
    rpc_name_set_wallet(svc->wallet, svc->mempool);
    register_name_rpc_commands(svc->rpc_table);

    /* ZCL Messaging — encrypted P2P messages */
    rpc_msg_set_state(boot_node_db(svc), svc->connman);
    register_msg_rpc_commands(svc->rpc_table);

    /* Atomic Swaps — HTLC contracts for BTC/LTC/DOGE */
    rpc_swap_set_state(boot_node_db(svc));
    register_swap_rpc_commands(svc->rpc_table);

    /* blk_sync.dat from file service is on disk. P2P will re-request
     * blocks it needs — the OS disk cache serves them fast since the
     * data is already in memory from the recent file sync download.
     * The deferred scanner was causing crashes (SIGABRT from concurrent
     * block_index access) and isn't worth the complexity. */

    rpc_wallet_set_state(svc->wallet, svc->state, ctx->datadir, svc->wallet_sqlite,
                         svc->mempool, svc->connman);
    rpc_wallet_set_coins_tip(svc->coins_tip);
    rpc_wallet_set_node_db(boot_node_db(svc));
    register_wallet_rpc_commands(svc->rpc_table);
    register_event_rpc_commands(svc->rpc_table);

    zslp_rpc_set_datadir(ctx->datadir);
    register_zslp_rpc_commands(svc->rpc_table);

    /* Pre-compute fast sync snapshot offer in background */
    {
        int chain_tip_h = active_chain_height(&svc->state->chain_active);
        int best_header = svc->state->pindex_best_header ?
            svc->state->pindex_best_header->nHeight : chain_tip_h;
        bool behind_ibd = (best_header - chain_tip_h) > 1000;

        if (svc->defer_offer_service) {
            printf("Fast sync offer build deferred during bootstrap receiver mode\n");
        } else if (behind_ibd) {
            printf("Fast sync offer build deferred during IBD "
                   "(chain=%d, headers=%d, behind=%d)\n",
                   chain_tip_h, best_header, best_header - chain_tip_h);
        } else if (!boot_start_offer_service(svc)) {
            fprintf(stderr,
                    "WARNING: failed to start tracked snapshot-offer thread\n");
        }
    }

    /* Initialize metrics observers for Prometheus /metrics */
    mcp_metrics_init();

    printf("[boot]   %-28s %lldms\n", "svc.rpc_mmb_register",
           (long long)(svc_clock_ms() - t_svc));
    t_svc = svc_clock_ms();

    boot_configure_frontend_rpc(svc);

    /* frontend kernel start includes onion_tor bootstrap (Tor) — the
     * span the profile flagged as the likely bulk of the ~11s. */
    /* De-fatal: a frontend-service start failure (rpc_http/explorer/Tor) is NOT
     * data-unrecoverable — the node can still serve P2P + advance the chain. Per
     * the mandate ("never silently dies unless the data is truly unrecoverable")
     * we enter DEGRADED_SERVING and continue instead of crash-looping. It is
     * LOUD: stderr + a structured event. (When rpc_http itself is down, zcl_state
     * is unreachable whether we crash or degrade — so degrading strictly gains a
     * live node + no crash-loop. rpc_http start only fails on a NULL-ctx
     * programming invariant, so this path is effectively unreachable in prod.) */
    if (!boot_register_frontend_services(svc) ||
        !zcl_service_kernel_start_all(&svc->frontend_kernel)) {
        fprintf(stderr,
            "WARNING: frontend services failed to start; serving DEGRADED "
            "(RPC/explorer/Tor may be unavailable)\n");
        event_emitf(EV_BOOT_ACTIVATE, 0,
                    "degraded_serving frontend_services_unavailable");
        service_state_advance(SERVICE_STATE_DEGRADED_SERVING,
                              "frontend_services_unavailable");
    }

    printf("[boot]   %-28s %lldms\n", "svc.frontend_tor_start",
           (long long)(svc_clock_ms() - t_svc));
    t_svc = svc_clock_ms();

    /* Discover peer reachability */
    {
        static struct node_profile g_node_profile;
        peer_strategy_discover_self(&g_node_profile,
                                    (uint16_t)ctx->p2p_port);

        const char *cn = g_node_profile.has_public_ip ? "yes" : "no";
        const char *method = "";
        if (g_node_profile.nat_pmp_available)
            method = " (NAT-PMP)";
        else if (g_node_profile.upnp_available)
            method = " (UPnP)";
        const char *tor = g_node_profile.tor_available ? "yes" : "no";
        printf("Reachability: clearnet=%s%s tor=%s\n", cn, method, tor);

        char addrs[4][68];
        int n = peer_strategy_get_addresses(&g_node_profile, addrs, 4);
        if (n > 0) {
            printf("Addresses:");
            for (int i = 0; i < n; i++)
                printf(" %s", addrs[i]);
            printf("\n");
        }
    }

    if (svc->want_address_backfill) {
        /* Re-enabled: SIGSEGV was caused by SQLite memory pressure from
         * a single massive GROUP BY over 1.3M UTXOs with 256MB mmap.
         * Fixed by batching per-address with bounded memory. */
        if (boot_start_address_backfill_service(svc)) {
            printf("Address backfill: started in tracked background thread\n");
            fflush(stdout);
        } else {
            fprintf(stderr,
                    "WARNING: failed to start tracked address backfill thread\n");
        }
    }

    /* HODL history filler — populates per-day "% held > 1y" snapshots
     * for the /explorer/hodl time-series chart. Idempotent; safe to
     * start on every boot even though the table mostly stays current
     * after first fill. */
    if (boot_profile_has_explorer(svc->app_ctx)) {
        if (boot_start_hodl_history_service(svc)) {
            printf("HODL history: filler started in tracked background thread\n");
            fflush(stdout);
        } else {
            fprintf(stderr,
                    "WARNING: failed to start HODL history filler thread\n");
        }
    }

    if (svc->want_snapshot_tx_index) {
        if (!boot_start_tx_index_service(svc)) {
            fprintf(stderr,
                    "WARNING: failed to start tracked tx-index build thread\n");
        }
    }

    atomic_store(svc->running, true);

    /* Start the supervisor thread BEFORE runtime services register their
     * liveness contracts. Idempotent — subsequent calls return true
     * without re-spawning. The supervisor runs its own monotonic-clock
     * loop independent of the lib/health sweeper, so a wedged sweeper
     * cannot silence stall detection. */
    if (!supervisor_start()) {
        fprintf(stderr,  // obs-ok:supervisor-start-fallback-warn
            "WARNING: supervisor_start failed; lib/health sweeper alone\n");
    }
    supervisor_domains_init();

    /* Initialize the typed blocker primitive. Must come before any
     * subsystem calls blocker_set / mirror_consensus_record_blocker.
     * Idempotent. */
    blocker_module_init();

    /* Outbound peer-floor liveness contract.
     *
     * Failure mode being addressed: the node can sit with 0 outbound
     * peers + a stuck inbound indefinitely. thread_open_connections keeps
     * running but addrman is exhausted, so `connman_pick_next_outbound_target`
     * returns false on every tick — silently, with no log, no event.
     *
     * Contract semantics:
     *   on_tick (every 15 s): snapshot outbound_healthy via
     *     `connman_outbound_healthy_count`; write to progress_marker.
     *     If the count is below the floor (2), do NOT call
     *     supervisor_tick — let the progress-quiet timer advance.
     *   on_stall (after 60 s under floor):
     *     - emit EV_PEER_FLOOR_BREACH for operator visibility
     *     - call connman_kick_seed_discovery to widen addrman
     *
     * The existing thread_open_connections still runs its 1-second
     * adaptive loop; the supervisor only kicks the seed re-walk so the
     * thread has fresh targets to try. */
    net_supervisor_register(svc->connman);
    chain_supervisor_register(svc->state);
    /* Tip-stuck watchdog. Single-purpose: watches active_chain_height
     * advance, emits a named stall event, and lets the operator-needed /
     * condition loop handle recovery. */
    chain_tip_watchdog_register(svc->state);
    /* Top-level always-terminating remedy escalator (sticky-node-plan #1, the
     * keystone of S2). Consumes the watchdog's deterministic-stall signal +
     * condition_engine_get_unresolved_count() and drives an ordered rung ladder
     * that NEVER latches a permanent operator-needed state on a recoverable
     * class. Register AFTER the watchdog (whose deterministic branch now hands
     * the wedge to the escalator instead of dead-ending) and BEFORE
     * self_heal_register (the self-heal tick drives the ladder). */
    sticky_escalator_set_datadir(svc->datadir);
    sticky_escalator_register(svc->state);
    condition_registry_register_all();
    invariant_sentinel_register(); /* fail-loud validation pack sweeps */
    /* Close the alert loop: install the event→sink routing (incl. the
     * EV_OPERATOR_NEEDED rule) BEFORE the condition engine can fire, so a
     * halt that exhausts remedies reaches a human/MCP and the health
     * surface instead of dead-ending. */
    alerts_init();
    self_heal_register(svc->state);
    staged_sync_supervisor_register(svc->state);

    /* Recover the durable finalized frontier a reboot dropped.
     * staged_sync_supervisor_register (above) ran tip_finalize_stage_init,
     * registering the chain-height authority seeded from the coins-restore
     * tip. Adopt the durable frontier forward-only HERE —
     * after the authority is live (active_chain_height reads the real coins tip)
     * but BEFORE runtime services / reducer ingest start — so there is no race.
     * Both calls are no-ops unless their precondition holds; neither rewinds,
     * forks, or deletes log rows. See block_index_loader.h for each contract. */
    {
        int seeded = block_index_loader_seed_tip_from_finalized(
            svc->state, params, progress_store_db());
        (void)seeded;  /* logs its own success line; benign no-op otherwise */
        /* B2 1c — torn-import AUTO-ARM is now the DEFAULT self-heal. On EVERY
         * boot we consult the PURE detect predicate
         * (block_index_loader_torn_import_detect, no side-effects); if it finds a
         * durable tear (a prevout_unresolved hole ABOVE the compiled anchor
         * h=3056758 plus a coin_backfill.refused.<h>.<hash> marker), arm_if_torn
         * re-seeds coins_kv from the SHA3-verified anchor snapshot (uss_open
         * verify_full_sha3=true bound to cp->sha3_hash) and HARD-ASSERTS the
         * re-seeded set == checkpoint (commitment + count==1354771; FATAL on
         * mismatch). It then folds forward from the proven anchor. If it arms (or
         * a from-anchor refold is already in progress — explicit flag at boot.c,
         * or a mid-fold restart), SKIP the cold-import seed.
         *
         * NO FLAG REQUIRED: a normal boot of a TORN datadir now self-heals. On a
         * HEALTHY (untorn) datadir the detect predicate returns false, arm_if_torn
         * does NOT reset, and the cold-import seed runs UNCHANGED — additive,
         * safe; a synced node never re-folds. The reset path itself is the
         * load-bearing safety net: it can only ever stamp the SHA3-verified anchor
         * set (or FATAL), never an unproven one.
         *
         * The explicit -refold-from-anchor flag and the -load-verify-boot route
         * (which armed the from-anchor signal in app_init when the verified
         * snapshot probe passed) are still honored — both surface here as
         * refold_from_anchor_active() == TRUE, which arm_if_torn short-circuits to
         * true without re-resetting. */
        /* -load-snapshot-at-own-height=PATH: the loader at boot.c already
         * RE-SEEDED coins_kv from a self-SHA3-verified snapshot at the snapshot's
         * OWN height, forced the 8 stage cursors to that height, and seeded the
         * tip_finalize trusted base there (raise-only). It is the authoritative
         * seed for THIS boot. Both fallback seed paths below would CLOBBER it:
         *   - boot_refold_from_anchor_arm_if_torn: the loader's coins-dependent
         *     cursors at seed_h (with on-disk bodies above) look like a "torn"
         *     prevout hole to the detector; with a reachable anchor snapshot it
         *     would re-seed coins_kv from the COMPILED checkpoint (3,056,758),
         *     dropping the trusted base back to the checkpoint and pinning H*.
         *   - block_index_loader_seed_stages_from_cold_import: the loader already
         *     cleared the cold-import provenance keys (so this returns 0 today),
         *     but suppress it explicitly so a future provenance write cannot
         *     re-stamp the cursors forward off the loader's seed_h.
         * So when the loader flag is set for this boot, skip BOTH — the loader
         * owns the seed and the staged pipeline folds forward from seed_h. */
        bool loader_owns_seed = boot_loader_owns_seed(svc->app_ctx);
        bool armed_from_anchor =
            loader_owns_seed ||                      /* loader at boot.c re-seeded + armed the stages */
            refold_from_anchor_active() ||           /* already armed: flag / load-verify / mid-fold */
            boot_refold_from_anchor_arm_if_torn(     /* DEFAULT: detect-gated torn-import self-heal */
                svc->state, boot_node_db(svc), progress_store_db());
        if (!armed_from_anchor)
            (void)block_index_loader_seed_stages_from_cold_import(
                svc->state, boot_node_db(svc), progress_store_db());
    }

    /* De-fatal: all runtime specs (bg_validation, gap_fill, legacy_mirror,
     * oracle, rolling_anchor, ...) are ZCL_SERVICE_OPTIONAL, and the
     * reducer/coordinator drives the authoritative chain-advance independently,
     * so a runtime-start failure does not stall consensus. Serve DEGRADED and
     * continue rather than crash-loop; LOUD via stderr + structured event. The
     * sync-monitor self-heal loop re-raises to SYNCING/HEALTHY once services
     * recover. Refresh-only if already DEGRADED (don't clobber a frontend
     * reason with a less-specific one beyond the same state). */
    if (!boot_register_runtime_services(svc) ||
        !zcl_service_kernel_start_all(&svc->runtime_kernel)) {
        fprintf(stderr,
            "WARNING: runtime services failed to start; serving DEGRADED "
            "(bg-validation/gap-fill/legacy-mirror may be unavailable)\n");
        event_emitf(EV_BOOT_ACTIVATE, 0,
                    "degraded_serving runtime_services_unavailable");
        if (service_state_current() != SERVICE_STATE_DEGRADED_SERVING)
            service_state_advance(SERVICE_STATE_DEGRADED_SERVING,
                                  "runtime_services_unavailable");
    }

    {
        struct block_index *tip = active_chain_tip(&svc->state->chain_active);
        int h = tip ? tip->nHeight : 0;
        event_emitf(EV_NODE_READY, 0, "height=%d peers=%zu",
                    h, svc->connman->manager.num_nodes);
    }
    printf("ZClassic C23 node initialized.\n");

    /* SQLite catchup — skip when no UTXO set (P2P snapshot incoming).
     * Running catchup during snapshot receive causes DB lock contention
     * that stalls the snapshot data flow. */
    if (boot_node_db(svc)) {
        int64_t utxo_count = db_utxo_count(boot_node_db(svc));
        if (utxo_count == 0) {
            printf("SQLite catchup: skipped (no UTXOs, waiting for P2P snapshot)\n");
        } else {
            if (!boot_start_catchup_service(svc, ctx->datadir)) {
                fprintf(stderr,
                        "WARNING: failed to start tracked SQLite catchup thread\n");
            }
        }
        if (!boot_start_projection_backfill_service(svc)) {
            fprintf(stderr,
                    "WARNING: failed to start projection backfill watcher\n");
        }
    }

    printf("[boot]   %-28s %lldms\n", "svc.peers_supervisors_runtime",
           (long long)(svc_clock_ms() - t_svc));

    return true;
}

/* ── Shutdown ──────────────────────────────────────────────── */

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

    bool ok = coins_view_sqlite_batch_write_ex( // one-write-path-ok:shutdown-single-writer
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
    /* Signal P2P threads to stop, then flush coins while threads wind down.
     * The message thread checks g_stop each iteration (~100ms). Any in-flight
     * reducer activation sees g_shutdown_requested and returns. After
     * signal_stop, no new block processing starts. */
    printf("[shutdown] stopping network services\n");
    zcl_service_kernel_stop_all(&svc->network_kernel);
    printf("[shutdown] joining replay service\n");
    boot_join_replay_service(svc);

    /* Flush coins to SQLite. The message thread is finishing its current
     * iteration. If it was mid-connect_block, it already flushed via the
     * g_shutdown_requested handler in the reducer activation path. */
    printf("Flushing coins cache to SQLite...\n");
    if (shutdown_flush_coins_to_sqlite(svc, "network-quiesce")) {
        printf("Coins cache flushed.\n");
    } else {
        fprintf(stderr, "WARNING: Coins cache flush FAILED during shutdown!\n");
    }

    shutdown_persist_fast_restart_state(svc);

    /* Now join threads — safe, coins already persisted */
    printf("[shutdown] joining connman threads\n");
    connman_join(svc->connman);
    connman_free(svc->connman);
    printf("[shutdown] connman stopped\n");

    /* Final flush in case message thread connected blocks between
     * our flush and its exit */
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
    /* Staged-sync shutdown order: bottom-up (a stage's upstream stays alive while it drains), and
     * BEFORE the frees below — a straggler drain ticked after the join sweep must see cleared
     * bindings, not freed chainstate. tip_finalize reads utxo_apply_log; tear it down first. */
    tip_finalize_stage_shutdown();

    /* utxo_apply reads from proof_validate_log; tear it down before
     * proof_validate. */
    utxo_apply_stage_shutdown();

    /* proof_validate reads from script_validate_log; tear it down first. */
    proof_validate_stage_shutdown();

    /* script_validate reads from body_persist_log; tear it down before
     * body_persist. */
    script_validate_stage_shutdown();

    /* body_persist reads from body_fetch_log; tear it down before
     * body_fetch. */
    body_persist_stage_shutdown();

    /* body_fetch reads from validate_headers_log; tear it down
     * before validate_headers. */
    body_fetch_stage_shutdown();

    /* Stop validate_headers next so its workers do not outlive the
     * disk_block_io cache. */
    validate_headers_stage_shutdown();

    /* Stop header_admit before closing progress.kv so any in-flight step
     * finishes. */
    header_admit_stage_shutdown();

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

    /* Once graceful shutdown is actually running, give it its own bounded
     * window instead of inheriting time already spent finishing startup. */
    alarm(90);

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
     * UTXO state is safe even if the rest of shutdown is interrupted. */
    if (svc->coins_tip) {
        printf("Emergency coins flush...\n");
        (void)shutdown_flush_coins_to_sqlite(svc, "emergency");
        printf("Emergency flush done.\n");
    }

    /* I-7b phase-1: detach hot path observers from the feeder while
     * the network is still draining. New block_msg arrivals between
     * here and quiesce will short-circuit at the global hook. */

    shutdown_stop_frontend_services(svc);
    shutdown_quiesce_network_and_flush_coins(svc);
    shutdown_persist_runtime_state(svc);
    {
        int stragglers = thread_registry_join_all(2);
        if (stragglers > 0) {
            /* A worker thread's bounded join timed out and it was detached, so
             * it may still be running. All durable state is already persisted
             * above (coins flushed, WAL checkpointed, DBs closed), so running
             * the destructive frees in shutdown_release_owned_resources would
             * race that thread (use-after-free on main_state / Sapling params /
             * caches it reads). Skip the frees and exit now — the OS reclaims
             * everything microseconds later. */
            fprintf(stderr,
                    "[shutdown] %d background thread(s) still finishing; state is "
                    "already saved, exiting now\n",
                    stragglers);
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
    }
    /* I-7b phase-2: net threads are joined; safe to destroy. */
    shutdown_release_owned_resources(svc);

    printf("Shutdown complete.\n");
    alarm(0);
}
