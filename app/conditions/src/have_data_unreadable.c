/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/have_data_unreadable.h"
#include "framework/condition.h"
#include "util/log_macros.h"

#include "chain/chain.h"
#include "event/event.h"
#include "services/sync_monitor.h"
#include "storage/disk_block_io.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>

static _Atomic int g_target_at_detect = -1;
static _Atomic int g_file_at_detect = -1;
static _Atomic unsigned g_pos_at_detect = 0;
static _Atomic int g_remedy_calls = 0;

static struct block_index *target_index(struct main_state *ms, int target)
{
    if (!ms || target < 0)
        return NULL;
    size_t iter = 0;
    struct block_index *p = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && p->nHeight == target && (p->nStatus & BLOCK_HAVE_DATA) &&
            !(p->nStatus & BLOCK_FAILED_MASK))
            return p;
    }
    return NULL;
}

static bool detect_have_data_unreadable(void)
{
    int64_t tip_age = sync_monitor_tip_advance_age();
    if (tip_age >= 0 && tip_age < 60)
        return false;

    struct main_state *ms = condition_engine_main_state();
    if (!ms)
        return false;
    int tip = active_chain_height(&ms->chain_active);
    if (tip < 0)
        return false;

    int target = tip + 1;
    struct block_index *p = target_index(ms, target);
    if (!p)
        return false;

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));
    if (block_index_have_data_readable(p, datadir))
        return false;

    atomic_store(&g_target_at_detect, target);
    atomic_store(&g_file_at_detect, p->nFile);
    atomic_store(&g_pos_at_detect, p->nDataPos);
    return true;
}

static enum condition_remedy_result remedy_have_data_unreadable(void)
{
    struct main_state *ms = condition_engine_main_state();
    int target = atomic_load(&g_target_at_detect);
    struct block_index *p = target_index(ms, target);
    if (!p)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);
    LOG_WARN("condition", "[condition:have_data_unreadable] clearing h=%d file=%d pos=%u", target, p->nFile, p->nDataPos);
    event_emitf(EV_BLOCK_REJECTED, 0,
                "HAVE_DATA_UNREADABLE h=%d file=%d pos=%u",
                target, p->nFile, p->nDataPos);
    p->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
    p->nFile = -1;
    p->nDataPos = 0;
    return COND_REMEDY_OK;
}

static bool witness_have_data_unreadable(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct main_state *ms = condition_engine_main_state();
    int target = atomic_load(&g_target_at_detect);
    if (!ms || target < 0)
        return false;

    if (active_chain_height(&ms->chain_active) >= target)
        return true;

    struct block_index *p = target_index(ms, target);
    if (!p)
        return true;

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));
    return block_index_have_data_readable(p, datadir);
}

static struct condition c_have_data_unreadable = {
    .name = "have_data_unreadable",
    .severity = COND_WARN,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 3,
    .detect = detect_have_data_unreadable,
    .remedy = remedy_have_data_unreadable,
    .witness = witness_have_data_unreadable,
    .witness_window_secs = 30,
};

void register_have_data_unreadable(void)
{
    (void)condition_register(&c_have_data_unreadable);
}

#ifdef ZCL_TESTING
void have_data_unreadable_test_reset(void)
{
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_file_at_detect, -1);
    atomic_store(&g_pos_at_detect, 0);
    atomic_store(&g_remedy_calls, 0);
    condition_reset_state(&c_have_data_unreadable);
}

int have_data_unreadable_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}
#endif
