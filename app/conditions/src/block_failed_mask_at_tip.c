/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"
#include "util/log_macros.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "platform/time_compat.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include "validation/process_block_revalidate.h"

#include <stdio.h>
#include <stdatomic.h>

static _Atomic int64_t g_target_at_detect = -1;
static _Atomic int64_t g_tip_height_at_check = -1;
static _Atomic int64_t g_tip_unchanged_since = 0;
static _Atomic int64_t g_tip_at_detect = -1;
static _Atomic int64_t g_tip_age_at_detect = 0;

enum block_failed_stall_type {
    BF_STALL_NONE = 0,
    BF_STALL_FAILED_MASK,
    BF_STALL_NO_ADVANCE,
};

static _Atomic int g_stall_type_at_detect = BF_STALL_NONE;

static struct block_index *find_failed_next(struct main_state *ms, int target)
{
    if (!ms) return NULL;
    size_t iter = 0;
    struct block_index *p = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && p->nHeight == target && (p->nStatus & BLOCK_FAILED_MASK))
            return p;
    }
    return NULL;
}

static struct block_index *find_have_data_next(struct main_state *ms, int target)
{
    if (!ms) return NULL;
    size_t iter = 0;
    struct block_index *p = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && p->nHeight == target &&
            (p->nStatus & BLOCK_HAVE_DATA) &&
            !(p->nStatus & BLOCK_FAILED_MASK))
            return p;
    }
    return NULL;
}

static int64_t current_tip_height(struct main_state *ms)
{
    return ms ? (int64_t)active_chain_height(&ms->chain_active) : -1;
}

static int64_t target_height(void)
{
    struct main_state *ms = condition_engine_main_state();
    int64_t tip = current_tip_height(ms);
    return tip >= 0 ? tip + 1 : -1;
}

static bool detect_block_failed_mask_at_tip(void)
{
    struct main_state *ms = condition_engine_main_state();
    int64_t target = target_height();
    if (target >= 0 && find_failed_next(ms, (int)target) != NULL) {
        atomic_store(&g_target_at_detect, target);
        atomic_store(&g_tip_at_detect, current_tip_height(ms));
        atomic_store(&g_tip_age_at_detect, 0);
        atomic_store(&g_stall_type_at_detect, BF_STALL_FAILED_MASK);
        return true;
    }

    int64_t tip = current_tip_height(ms);
    int64_t now = platform_time_wall_unix();
    int64_t prev_tip = atomic_load(&g_tip_height_at_check);
    if (prev_tip != tip) {
        atomic_store(&g_tip_height_at_check, tip);
        atomic_store(&g_tip_unchanged_since, now);
        return false;
    }

    int64_t unchanged_since = atomic_load(&g_tip_unchanged_since);
    if (unchanged_since == 0) {
        atomic_store(&g_tip_unchanged_since, now);
        return false;
    }

    int64_t tip_age_s = now - unchanged_since;
    if (tip >= 0 && target >= 0 && tip_age_s >= 300 &&
        find_have_data_next(ms, (int)target) != NULL) {
        atomic_store(&g_target_at_detect, target);
        atomic_store(&g_tip_at_detect, tip);
        atomic_store(&g_tip_age_at_detect, tip_age_s);
        atomic_store(&g_stall_type_at_detect, BF_STALL_NO_ADVANCE);
        return true;
    }

    return false;
}

static enum condition_remedy_result remedy_block_failed_mask_at_tip(void)
{
    struct main_state *ms = condition_engine_main_state();
    int64_t target = target_height();
    if (!ms || target < 0)
        return COND_REMEDY_SKIP;
    struct uint256 out_hash;
    enum reval_result r =
        process_block_revalidate((int)target, ms, &out_hash);
    LOG_WARN("condition", "[condition:block_failed_mask_at_tip] target=%lld " "stall_type=%s tip_age_s=%lld result=%s", (long long)target, atomic_load(&g_stall_type_at_detect) == BF_STALL_NO_ADVANCE ? "no_advance" : "failed_mask", (long long)atomic_load(&g_tip_age_at_detect), reval_result_name(r));
    if (atomic_load(&g_stall_type_at_detect) == BF_STALL_NO_ADVANCE &&
        r == REVAL_HEIGHT_NOT_FOUND)
        return COND_REMEDY_OK;
    return (r == REVAL_RECOVERED || r == REVAL_NO_FAILURE)
        ? COND_REMEDY_OK : COND_REMEDY_FAILED;
}

static bool witness_block_failed_mask_at_tip(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct main_state *ms = condition_engine_main_state();
    int64_t target = atomic_load(&g_target_at_detect);
    int stall_type = atomic_load(&g_stall_type_at_detect);
    if (!ms || target < 0)
        return false;
    if (stall_type == BF_STALL_NO_ADVANCE)
        return current_tip_height(ms) > atomic_load(&g_tip_at_detect);
    return find_failed_next(ms, (int)target) == NULL;
}

static struct condition c_block_failed_mask_at_tip = {
    .name = "block_failed_mask_at_tip",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_block_failed_mask_at_tip,
    .remedy = remedy_block_failed_mask_at_tip,
    .witness = witness_block_failed_mask_at_tip,
    .witness_window_secs = 60,
};

void register_block_failed_mask_at_tip(void)
{
    (void)condition_register(&c_block_failed_mask_at_tip);
}

bool block_failed_mask_at_tip_recovery_exhausted(int *target_height,
                                                 int *attempts_out)
{
    struct condition_state *s = &c_block_failed_mask_at_tip.state;
    int attempts = atomic_load(&s->attempts);
    int max_attempts = c_block_failed_mask_at_tip.max_attempts > 0
        ? c_block_failed_mask_at_tip.max_attempts : 1;
    bool exhausted = atomic_load(&s->currently_active) &&
        attempts >= max_attempts &&
        atomic_load(&s->last_outcome) == COND_REMEDY_FAILED;

    if (target_height)
        *target_height = (int)atomic_load(&g_target_at_detect);
    if (attempts_out)
        *attempts_out = attempts;
    return exhausted;
}

#ifdef ZCL_TESTING
void block_failed_mask_at_tip_test_reset(void)
{
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_tip_height_at_check, -1);
    atomic_store(&g_tip_unchanged_since, 0);
    atomic_store(&g_tip_at_detect, -1);
    atomic_store(&g_tip_age_at_detect, 0);
    atomic_store(&g_stall_type_at_detect, BF_STALL_NONE);
    condition_reset_state(&c_block_failed_mask_at_tip);
}

int block_failed_mask_at_tip_test_stall_type(void)
{
    return atomic_load(&g_stall_type_at_detect);
}

void block_failed_mask_at_tip_test_mark_exhausted(int target_height)
{
    struct condition_state *s = &c_block_failed_mask_at_tip.state;
    atomic_store(&g_target_at_detect, target_height);
    atomic_store(&g_tip_at_detect, target_height - 1);
    atomic_store(&g_stall_type_at_detect, BF_STALL_FAILED_MASK);
    atomic_store(&s->currently_active, true);
    atomic_store(&s->attempts, c_block_failed_mask_at_tip.max_attempts);
    atomic_store(&s->last_outcome, COND_REMEDY_FAILED);
}
#endif
