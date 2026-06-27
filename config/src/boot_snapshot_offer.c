/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot snapshot-offer worker — the fast-sync snapshot offer + manifest
 * builder, lifted out of boot_background_workers.c so that file stays under
 * the E1 800-line ceiling. PURE relocation: the worker body is byte-identical
 * to its prior home; only its surrounding TU changed.
 *
 * The worker exports the consensus snapshot, embeds the MMR + MMB roots, and
 * publishes the snapshot / chunk / block-piece manifests for fast-sync peers.
 * It is a supervised background worker (Shape 5 — MONITOR): it shares the
 * worker_on_stall handler + boot_register_worker_supervisor helper exposed by
 * boot_background_workers.h (single source — not duplicated here), reaches the
 * boot context through the boot_services.c accessors declared in
 * boot_internal.h (boot_node_db / boot_profile_has_file_service /
 * boot_serialize_utxo_snapshot), and reaches the single MMB leaf store via the
 * g_mmb_leaf_store extern (also in boot_internal.h).
 *
 * The start/join pair is called from its existing sites in app_init_services /
 * app_shutdown_svc (boot_services.c), boot order preserved.
 */

#include "config/boot_internal.h"
#include "config/boot_background_workers.h"
#include "config/boot_snapshot_offer.h"
#include "services/consensus_snapshot_export_service.h"
#include "models/mmb_leaf_store.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "controllers/blockchain_controller.h"
#include "controllers/file_controller.h"
#include "net/file_service.h"
#include "net/fast_sync.h"
#include "supervisors/domains.h"
#include "util/supervisor.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Supervisor liveness (Model A — MONITOR; disk_monitor.c exemplar) ─
 *
 * The snapshot-offer worker keeps its own pthread and gains an observe-only
 * liveness_contract registered ONCE at boot_start_offer_service. The
 * supervisor never owns or tears down the thread (period_secs == 0); it only
 * watches the heartbeat the worker emits at each phase checkpoint and fires
 * worker_on_stall (log + EV_RECOVERY_ACTION) when the deadline lapses. */
#define SNAPSHOT_OFFER_SUPERVISOR_DEADLINE_SEC      1800

static struct liveness_contract g_offer_contract;
static _Atomic supervisor_child_id g_offer_sup_id = SUPERVISOR_INVALID_ID;

static void *build_snapshot_offer_thread(void *arg);

/* The supervisor register helper (boot_register_worker_supervisor), the
 * observe-only stall handler (worker_on_stall), and the generic thread
 * start/named-join plumbing (boot_start_thread_service /
 * boot_join_thread_service_named) all live in boot_background_workers.c and
 * are shared via boot_background_workers.h — single source, so this TU spawns
 * no raw worker thread of its own. */

bool boot_start_offer_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    boot_register_worker_supervisor(&g_offer_sup_id, &g_offer_contract,
                                    &g_op_sup, "op.build_snapshot_offer",
                                    SNAPSHOT_OFFER_SUPERVISOR_DEADLINE_SEC, 0);
    return boot_start_thread_service(&svc->offer_thread,
                                     &svc->offer_thread_started,
                                     build_snapshot_offer_thread, svc);
}

void boot_join_offer_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->offer_thread,
                                   &svc->offer_thread_started,
                                   "snapshot_offer", 5);
}

/* One-shot-checkpointed heartbeat for the snapshot-offer worker. It has no
 * polling loop, so it ticks + advances its progress marker at each phase
 * boundary (export, offer build, chunk manifest, block manifest) — enough
 * forward signal for the supervisor to distinguish "working" from "wedged"
 * across the worker's slow I/O phases. */
static void offer_checkpoint(int64_t *checkpoint)
{
    supervisor_child_id sup_id = atomic_load(&g_offer_sup_id);
    if (sup_id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_tick(sup_id);
    supervisor_progress(sup_id, ++(*checkpoint));
}

static void *build_snapshot_offer_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    const char *datadir = svc ? svc->datadir : NULL;
    int64_t checkpoint = 0;

    if (!datadir || datadir[0] == '\0')
        return NULL;

    offer_checkpoint(&checkpoint);

    /* Sticky/global-sync (Lane F #9c): ANY at-tip zclassic23 node builds
     * and offers a snapshot, not only file_service-profile nodes. The more
     * nodes that contribute a shareable consensus_snapshot.db, the more
     * reliably a recovering/behind node finds a supplier from the P2P
     * network alone — no central download host, no co-located oracle. The
     * offer is still only PUBLISHED once a disk-backed serving buffer is
     * ready (see snapshot_serving_ready below), so this only widens who
     * BUILDS, never who falsely advertises.
     *
     * Cost: SQLite vacuum allocates transiently — typically a few GB
     * on archival nodes, sub-second to a few seconds on healthy hosts.
     * Operators on memory-constrained boxes can opt out by setting
     * ZCL_EXPORT_CONSENSUS_SNAPSHOT_ON_BOOT=0.
     */
    bool file_service_enabled =
        svc && boot_profile_has_file_service(svc->app_ctx);
    const char *export_snapshot =
        getenv("ZCL_EXPORT_CONSENSUS_SNAPSHOT_ON_BOOT");
    bool export_opt_out = export_snapshot &&
                          strcmp(export_snapshot, "0") == 0;
    /* Build on any profile unless explicitly opted out. */
    if (!export_opt_out) {
        printf("Exporting consensus snapshot (no wallet data)...\n");
        struct zcl_result export_result =
            consensus_snapshot_export_service_run(datadir);
        if (export_result.ok) {
            file_controller_refresh_manifest();
            fs_server_refresh_manifest();
            printf("Consensus snapshot ready for file service\n");
        } else {
            printf("Consensus snapshot export skipped/failed (%s)\n",
                   export_result.message);
        }
    } else {
        printf("Consensus snapshot export skipped on boot "
               "(ZCL_EXPORT_CONSENSUS_SNAPSHOT_ON_BOOT=0 — opt-out)\n");
    }
    (void)file_service_enabled;

    offer_checkpoint(&checkpoint);

    printf("Building fast sync snapshot offer...\n");

    /* Zero-init: on the no-snapshot-yet path fast_sync_build_offer() returns
     * false WITHOUT filling offer, but offer.height is still read below to
     * gate the block-piece manifest. Uninitialized, that fed a garbage
     * end-height into the manifest builder on every at-tip node. With {0},
     * height==0 fails the `tip_h > BLOCKS_PER_PIECE` guard and we skip it. */
    struct snapshot_offer offer = {0};
    if (fast_sync_build_offer(datadir, &offer)) {
        /* Embed MMR root — cryptographically binds UTXO snapshot to PoW chain */
        struct mmr *m = rpc_blockchain_get_mmr();
        if (m && m->num_leaves > 0) {
            mmr_root(m, offer.mmr_root);
        } else {
            memset(offer.mmr_root, 0, 32);
        }

        /* Embed MMB root — replayed from leaf hash store via
         * mmb_append_hash(), guaranteeing identical merge logic
         * as mmb_prove() uses internally. */
        if (g_mmb_leaf_store.open && g_mmb_leaf_store.num_leaves > 0) {
            const uint8_t (*lh)[32] = mmb_leaf_store_all(&g_mmb_leaf_store);
            /* Replay through the snapshot anchor.  Heights are zero-based,
             * so a snapshot at height H needs H+1 MMB leaves and a
             * FlyClient chain_length of H+1. */
            uint64_t replay_count = (uint64_t)offer.height + 1;
            if (g_mmb_leaf_store.num_leaves < replay_count) {
                printf("Fast sync: MMB leaf store short (%llu/%llu); "
                       "snapshot offer unavailable\n",
                       (unsigned long long)g_mmb_leaf_store.num_leaves,
                       (unsigned long long)replay_count);
                memset(offer.mmb_root, 0, 32);
            } else if (lh && replay_count > 0) {
                struct mmb replay;
                mmb_init(&replay);
                for (uint64_t i = 0; i < replay_count; i++)
                    mmb_append_hash(&replay, lh[i]);
                mmb_root(&replay, offer.mmb_root);
                printf("Fast sync: MMB root at h=%d (%llu/%llu leaves, "
                       "%u peaks)\n", offer.height,
                       (unsigned long long)replay.num_leaves,
                       (unsigned long long)g_mmb_leaf_store.num_leaves,
                       replay.num_mountains);
            } else {
                memset(offer.mmb_root, 0, 32);
            }
        } else {
            memset(offer.mmb_root, 0, 32);
            printf("Fast sync: WARNING — no MMB leaf store\n");
        }
        /* Pre-serialize snapshot file and publish its SHA3 metadata.
         * The legacy zsnapshot offer is only advertised when an explicit
         * bounded RAM serving cache exists; otherwise peers should use the
         * chunk/file manifests below. */
        bool snapshot_serving_ready = false;
        struct node_db *ndb = boot_node_db(svc);
        if (ndb && ndb->open) {
            int64_t snap_count = fast_sync_prebuild_snapshot(
                datadir, boot_serialize_utxo_snapshot, ndb);
            if (snap_count > 0) {
                /* Use the SHA3 computed during serialization */
                uint8_t snap_sha3[32];
                uint64_t snap_n = 0;
                if (fast_sync_get_snapshot_sha3(snap_sha3, &snap_n)) {
                    memcpy(offer.utxo_root, snap_sha3, 32);
                    offer.num_utxos = snap_n;
                    printf("Snapshot: %llu UTXOs, SHA3 from file\n",
                           (unsigned long long)snap_n);
                }
                int64_t snap_buf_size = 0;
                snapshot_serving_ready =
                    fast_sync_get_snapshot_buf(&snap_buf_size) != NULL &&
                    snap_buf_size > 0;
            }
        }

        bool have_mmb_root = false;
        for (size_t i = 0; i < sizeof(offer.mmb_root); i++) {
            if (offer.mmb_root[i] != 0) {
                have_mmb_root = true;
                break;
            }
        }
        if (have_mmb_root && snapshot_serving_ready) {
            msg_processor_update_offer(&offer);
            printf("Fast sync ready: h=%d, %llu UTXOs, MMB+MMR secured\n",
                   offer.height, (unsigned long long)offer.num_utxos);
        } else if (have_mmb_root) {
            printf("Fast sync: snapshot offer withheld "
                   "(disk-backed snapshot serving not enabled)\n");
        } else {
            printf("Fast sync: snapshot offer withheld (MMB root unavailable)\n");
        }
    } else {
        printf("Fast sync: no snapshot available yet\n");
    }

    offer_checkpoint(&checkpoint);

    if (file_service_enabled) {
        printf("Building chunk sync manifest...\n");
        struct sync_manifest chunk_manifest;
        memset(&chunk_manifest, 0, sizeof(chunk_manifest));
        if (fast_sync_build_manifest(datadir, &chunk_manifest)) {
            int32_t manifest_height = chunk_manifest.height;
            uint32_t num_chunks = chunk_manifest.num_chunks;
            uint64_t num_utxos = chunk_manifest.num_utxos;
            msg_processor_publish_manifest(&chunk_manifest);
            printf("Chunk manifest ready: h=%d, %u chunks (%llu UTXOs)\n",
                   manifest_height, num_chunks,
                   (unsigned long long)num_utxos);
        } else {
            printf("Chunk manifest: not available yet\n");
        }
    }

    offer_checkpoint(&checkpoint);

    int32_t tip_h = offer.height;

    if (file_service_enabled && tip_h > BLOCKS_PER_PIECE) {
        printf("Building block piece manifest...\n");
        struct block_piece_manifest block_manifest;
        memset(&block_manifest, 0, sizeof(block_manifest));
        if (block_piece_manifest_build_active_chain(&svc->state->chain_active,
                                                    1, tip_h,
                                                    &block_manifest) ||
            block_piece_manifest_build(datadir, 1, tip_h,
                                       &block_manifest)) {
            int32_t start_height = block_manifest.start_height;
            int32_t end_height = block_manifest.end_height;
            uint32_t num_pieces = block_manifest.num_pieces;
            msg_processor_publish_block_manifest(&block_manifest, tip_h);
            printf("Block manifest ready: h=%d..%d, %u pieces\n",
                   start_height, end_height, num_pieces);
        } else {
            printf("Block manifest: build failed\n");
        }
    }

    offer_checkpoint(&checkpoint);
    return NULL;
}
