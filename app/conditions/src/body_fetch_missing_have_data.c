/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/body_fetch_missing_have_data.h"

#include "framework/condition.h"
#include "jobs/stage_repair.h"
#include "services/sync_monitor.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>

static _Atomic int g_target_at_detect = -1;
static _Atomic int g_remedy_calls = 0;

static struct block_index *target_index_locked(struct main_state *ms,
                                               int target)
{
    if (!ms || target < 0)
        return NULL;

    struct block_index *bi = active_chain_at(&ms->chain_active, target);
    struct block_index *prev = target > 0
        ? active_chain_at(&ms->chain_active, target - 1)
        : NULL;
    if (bi && bi->nHeight == target && bi->phashBlock &&
        !block_has_any_failure(bi) &&
        (target == 0 || !prev || bi->pprev == prev))
        return bi;

    size_t iter = 0;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (!bi || bi->nHeight != target || !bi->phashBlock ||
            block_has_any_failure(bi))
            continue;
        if (target > 0 && prev && bi->pprev != prev)
            continue;
        return bi;
    }
    return NULL;
}

static bool target_has_readable_data(struct main_state *ms, int target)
{
    bool readable = false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = target_index_locked(ms, target);
    if (bi && (bi->nStatus & BLOCK_HAVE_DATA)) {
        char datadir[2048];
        GetDataDir(true, datadir, sizeof(datadir));
        readable = block_index_have_data_readable(bi, datadir);
    }
    zcl_mutex_unlock(&ms->cs_main);
    return readable;
}

static bool target_missing_data(struct main_state *ms, int target)
{
    bool missing = false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = target_index_locked(ms, target);
    if (bi)
        missing = (bi->nStatus & BLOCK_HAVE_DATA) == 0;
    zcl_mutex_unlock(&ms->cs_main);
    return missing;
}

static bool detect_body_fetch_missing_have_data(void)
{
    int64_t tip_age = sync_monitor_tip_advance_age();
    if (tip_age >= 0 && tip_age < 60)
        return false;

    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms)
        return false;

    struct stage_repair_body_fetch_gap gap;
    if (!stage_repair_body_fetch_missing_have_data_frontier_candidate(
            db, &gap))
        return false;
    if (gap.body_observed)
        return false;

    if (!target_missing_data(ms, gap.target_height))
        return false;

    atomic_store(&g_target_at_detect, gap.target_height);
    return true;
}

static enum condition_remedy_result remedy_body_fetch_missing_have_data(void)
{
    int target = atomic_load(&g_target_at_detect);
    if (target < 0)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);
    struct zcl_result r = sync_monitor_queue_active_frontier_body(
        target, "condition:body_fetch_missing_have_data");
    if (!r.ok) {
        LOG_WARN("condition",
                 "[condition:body_fetch_missing_have_data] queue failed "
                 "h=%d code=%d msg=%s",
                 target, r.code, r.message);
        return COND_REMEDY_FAILED;
    }

    LOG_WARN("condition",
             "[condition:body_fetch_missing_have_data] queued h=%d",
             target);
    return COND_REMEDY_OK;
}

static bool witness_body_fetch_missing_have_data(int64_t target_at_detect)
{
    (void)target_at_detect;

    int target = atomic_load(&g_target_at_detect);
    sqlite3 *db = progress_store_db();
    if (db && target >= 0 &&
        stage_repair_body_fetch_observed(db, target))
        return true;

    struct main_state *ms = sync_monitor_main_state();
    if (!ms || target < 0)
        return false;
    return target_has_readable_data(ms, target);
}

static struct condition c_body_fetch_missing_have_data = {
    .name = "body_fetch_missing_have_data",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_body_fetch_missing_have_data,
    .remedy = remedy_body_fetch_missing_have_data,
    .witness = witness_body_fetch_missing_have_data,
    .witness_window_secs = 60,
    /* External-resource fault (a present-but-unreadable/missing body must be
     * re-fetched from peers): re-arm on a cooldown so the re-queue keeps
     * retrying until the body lands, instead of latching operator_needed
     * forever. Pages once at the cap; never gives up. Mirrors peer_floor_violated. */
    .cooldown_secs = 600,
    .cooldown_max_rearms = 0,
};

void register_body_fetch_missing_have_data(void)
{
    (void)condition_register(&c_body_fetch_missing_have_data);
}

#ifdef ZCL_TESTING
void body_fetch_missing_have_data_test_reset(void)
{
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_remedy_calls, 0);
    condition_reset_state(&c_body_fetch_missing_have_data);
}

int body_fetch_missing_have_data_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}
#endif
