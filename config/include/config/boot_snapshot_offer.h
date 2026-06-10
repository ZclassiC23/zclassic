/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot snapshot-offer worker unit — the fast-sync snapshot offer builder
 * lifted out of boot_background_workers.c so that file stays under the E1
 * 800-line ceiling. The worker body (build_snapshot_offer_thread) exports the
 * UTXO snapshot, embeds the MMR + MMB roots, and publishes the snapshot /
 * chunk / block-piece manifests for fast-sync peers.
 *
 * It is a supervised background worker (Shape 5 — MONITOR): it shares the
 * worker_on_stall handler and boot_register_worker_supervisor helper exposed
 * by boot_background_workers.h, and reaches the single MMB leaf store via the
 * g_mmb_leaf_store extern in boot_internal.h.
 *
 * The start/join pair is called from its existing sites in app_init_services /
 * app_shutdown_svc (boot_services.c), boot order preserved.
 *
 * Not for use outside config/src/.
 */

#ifndef ZCL_BOOT_SNAPSHOT_OFFER_H
#define ZCL_BOOT_SNAPSHOT_OFFER_H

#include <stdbool.h>

struct boot_svc_ctx;

/* Fast-sync snapshot offer + chunk/block manifest builder. */
bool boot_start_offer_service(struct boot_svc_ctx *svc);
void boot_join_offer_service(struct boot_svc_ctx *svc);

#endif
