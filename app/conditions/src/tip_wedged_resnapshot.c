/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"
#include "util/log_macros.h"

#include "config/runtime.h"
#include "event/event.h"
#include "net/snapshot_sync_contract.h"
#include "sync/sync_state.h"
#include "services/sync_monitor.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TIP_WEDGED_RESNAPSHOT_NEAR_TIP_BLOCKS 5000
#define TIP_WEDGED_RESNAPSHOT_LOCAL_RETRIES 3

bool block_failed_mask_at_tip_recovery_exhausted(int *target_height,
                                                 int *attempts_out);

enum tip_wedged_trigger {
    TWR_TRIGGER_NONE = 0,
    TWR_TRIGGER_FAILED_MASK_EXHAUSTED,
    TWR_TRIGGER_LOCAL_IMPORT_EXHAUSTED,
};

static _Atomic int g_trigger_at_detect = TWR_TRIGGER_NONE;
static _Atomic int g_local_height_at_detect = -1;
static _Atomic int g_best_header_at_detect = -1;
static _Atomic int g_target_height_at_detect = -1;
static _Atomic int g_recovery_attempted;
static _Atomic int g_recovery_accepted;
static _Atomic int g_last_manifest_height = -1;

#ifdef ZCL_TESTING
static struct node_db *g_test_ndb;
static struct snapshot_sync_service *g_test_svc;
static _Atomic int g_test_remedy_calls;
#endif

static const char *trigger_name(enum tip_wedged_trigger trigger)
{
    switch (trigger) {
    case TWR_TRIGGER_FAILED_MASK_EXHAUSTED:
        return "block_failed_mask_exhausted";
    case TWR_TRIGGER_LOCAL_IMPORT_EXHAUSTED:
        return "local_import_exhausted";
    case TWR_TRIGGER_NONE:
    default:
        return "none";
    }
}

static struct node_db *runtime_ndb(void)
{
#ifdef ZCL_TESTING
    if (g_test_ndb)
        return g_test_ndb;
#endif
    return app_runtime_node_db();
}

static struct snapshot_sync_service *runtime_snapsync(struct node_db *ndb)
{
#ifdef ZCL_TESTING
    if (g_test_svc)
        return g_test_svc;
#endif
    struct snapshot_sync_service *svc = app_runtime_snapshot_sync();
    if (svc)
        return svc;
    if (!ndb)
        return NULL;
    if (!snapsync_global_initialized())
        snapsync_global_ensure_init(ndb);
    return snapsync_global();
}

static bool snapshot_recovery_already_active(void)
{
    struct snapshot_sync_service *svc = NULL;
#ifdef ZCL_TESTING
    if (g_test_svc)
        svc = g_test_svc;
#endif
    if (!svc)
        svc = app_runtime_snapshot_sync();
    if (!svc && snapsync_global_initialized())
        svc = snapsync_global();
    return svc && svc->state != SNAPSYNC_IDLE;
}

static int best_header_height(struct main_state *ms)
{
    struct connman *cm = sync_monitor_connman();
    int peer_max = cm ? connman_max_peer_height(cm) : -1;
    int best = ms && ms->pindex_best_header ? ms->pindex_best_header->nHeight
                                            : -1;
    return peer_max > best ? peer_max : best;
}

static bool local_import_exhausted(int local_height, int *target_height)
{
    struct watchdog_local_recovery_stats stats;
    sync_monitor_get_local_recovery_stats(&stats);
    bool exhausted = stats.active &&
        strcmp(stats.mode, "next-child-missing") == 0 &&
        (stats.retries_exhausted ||
         stats.retry_count >= TIP_WEDGED_RESNAPSHOT_LOCAL_RETRIES);
    if (target_height)
        *target_height = stats.missing_height > 0
            ? stats.missing_height : local_height + 1;
    return exhausted;
}

static bool detect_tip_wedged_resnapshot(void)
{
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        ms = condition_engine_main_state();
    if (!ms)
        return false;

    int local = active_chain_height(&ms->chain_active);
    int best = best_header_height(ms);
    int gap = best - local;
    if (local < 0 || best <= local ||
        gap > TIP_WEDGED_RESNAPSHOT_NEAR_TIP_BLOCKS)
        return false;
    if (snapshot_recovery_already_active())
        return false;

    int target = -1;
    int attempts = 0;
    enum tip_wedged_trigger trigger = TWR_TRIGGER_NONE;
    if (block_failed_mask_at_tip_recovery_exhausted(&target, &attempts)) {
        (void)attempts;
        trigger = TWR_TRIGGER_FAILED_MASK_EXHAUSTED;
    } else if (local_import_exhausted(local, &target)) {
        trigger = TWR_TRIGGER_LOCAL_IMPORT_EXHAUSTED;
    }
    if (trigger == TWR_TRIGGER_NONE || target <= local)
        return false;

    atomic_store(&g_trigger_at_detect, trigger);
    atomic_store(&g_local_height_at_detect, local);
    atomic_store(&g_best_header_at_detect, best);
    atomic_store(&g_target_height_at_detect, target);
    return true;
}

static enum condition_remedy_result remedy_tip_wedged_resnapshot(void)
{
    struct node_db *ndb = runtime_ndb();
    struct snapshot_sync_service *svc = runtime_snapsync(ndb);
    struct snapshot_offer_params manifest;
    int target = atomic_load(&g_target_height_at_detect);
    enum tip_wedged_trigger trigger =
        (enum tip_wedged_trigger)atomic_load(&g_trigger_at_detect);

    atomic_store(&g_recovery_attempted, 1);
#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif

    if (!ndb || !svc || target <= 0) {
        LOG_WARN("condition", "[condition:tip_wedged_resnapshot] missing runtime " "ndb=%p svc=%p target=%d", (void *)ndb, (void *)svc, target);
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "condition tip_wedged_resnapshot result=failed "
                    "reason=missing_runtime target=%d",
                    target);
        return COND_REMEDY_FAILED;
    }

    if (!snapsync_build_local_recovery_manifest(ndb, &manifest, 0).ok) {
        LOG_WARN("condition", "[condition:tip_wedged_resnapshot] trigger=%s target=%d " "local=%d best=%d result=manifest_unavailable", trigger_name(trigger), target, atomic_load(&g_local_height_at_detect), atomic_load(&g_best_header_at_detect));
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "condition tip_wedged_resnapshot result=failed "
                    "reason=manifest_unavailable target=%d",
                    target);
        return COND_REMEDY_FAILED;
    }

    atomic_store(&g_last_manifest_height, manifest.height);
    bool accepted = snapsync_request_recovery(svc, target, &manifest).ok;
    atomic_store(&g_recovery_accepted, accepted ? 1 : 0);
    LOG_WARN("condition", "[condition:tip_wedged_resnapshot] trigger=%s target=%d " "manifest_h=%d local=%d best=%d accepted=%d", trigger_name(trigger), target, manifest.height, atomic_load(&g_local_height_at_detect), atomic_load(&g_best_header_at_detect), accepted ? 1 : 0);
    event_emitf(EV_SYNC_STATE_CHANGE, 0,
                "condition tip_wedged_resnapshot trigger=%s target=%d "
                "manifest_h=%d accepted=%d",
                trigger_name(trigger), target, manifest.height,
                accepted ? 1 : 0);
    if (accepted) {
        sync_monitor_record_snapshot_resnapshot(
            atomic_load(&g_local_height_at_detect),
            atomic_load(&g_best_header_at_detect),
            0,
            target,
            manifest.height,
            trigger_name(trigger),
            "condition:tip_wedged_resnapshot");
        return COND_REMEDY_OK;
    }
    return COND_REMEDY_FAILED;
}

static bool witness_tip_wedged_resnapshot(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        ms = condition_engine_main_state();
    int target = atomic_load(&g_target_height_at_detect);
    if (ms && target > 0 && active_chain_height(&ms->chain_active) >= target)
        return true;

    struct snapshot_sync_service *svc = runtime_snapsync(runtime_ndb());
    return svc && svc->state == SNAPSYNC_COMPLETE;
}

static struct condition c_tip_wedged_resnapshot = {
    .name = "tip_wedged_resnapshot",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 300,
    .max_attempts = 3,
    .detect = detect_tip_wedged_resnapshot,
    .remedy = remedy_tip_wedged_resnapshot,
    .witness = witness_tip_wedged_resnapshot,
    .witness_window_secs = 300,
};

void register_tip_wedged_resnapshot(void)
{
    (void)condition_register(&c_tip_wedged_resnapshot);
}

#ifdef ZCL_TESTING
void tip_wedged_resnapshot_test_reset(void)
{
    atomic_store(&g_trigger_at_detect, TWR_TRIGGER_NONE);
    atomic_store(&g_local_height_at_detect, -1);
    atomic_store(&g_best_header_at_detect, -1);
    atomic_store(&g_target_height_at_detect, -1);
    atomic_store(&g_recovery_attempted, 0);
    atomic_store(&g_recovery_accepted, 0);
    atomic_store(&g_last_manifest_height, -1);
    atomic_store(&g_test_remedy_calls, 0);
    g_test_ndb = NULL;
    g_test_svc = NULL;
}

void tip_wedged_resnapshot_test_set_runtime(
    struct node_db *ndb,
    struct snapshot_sync_service *svc)
{
    g_test_ndb = ndb;
    g_test_svc = svc;
}

int tip_wedged_resnapshot_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}

int tip_wedged_resnapshot_test_recovery_accepted(void)
{
    return atomic_load(&g_recovery_accepted);
}

int tip_wedged_resnapshot_test_last_manifest_height(void)
{
    return atomic_load(&g_last_manifest_height);
}
#endif
