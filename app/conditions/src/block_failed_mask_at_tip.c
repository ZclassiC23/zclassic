/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"
#include "util/log_macros.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "jobs/stage_repair.h"
#include "platform/time_compat.h"
#include "storage/progress_store.h"
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
static _Atomic int g_validate_repair_owner_height = -1;
static _Atomic int g_validate_repair_owner_mode = STAGE_REPAIR_POISON_NONE;

enum block_failed_stall_type {
    BF_STALL_NONE = 0,
    BF_STALL_FAILED_MASK,
    BF_STALL_NO_ADVANCE,
};

static _Atomic int g_stall_type_at_detect = BF_STALL_NONE;

static bool validate_repairable_mode(
    enum stage_repair_header_solution_poison mode)
{
    return mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS ||
           mode == STAGE_REPAIR_POISON_VALIDATE_HASH_MISMATCH;
}

static bool repairable_validate_frontier_owns_stall(int64_t stall_target,
                                                    int *out_height,
                                                    int *out_mode)
{
    if (out_height)
        *out_height = -1;
    if (out_mode)
        *out_mode = STAGE_REPAIR_POISON_NONE;

    sqlite3 *db = progress_store_db();
    if (!db || stall_target < 0)
        return false;

    int repair_height = -1;
    if (!stage_repair_header_solution_repairable_validate_frontier(
            db, &repair_height) ||
        repair_height < 0 || repair_height > stall_target)
        return false;

    enum stage_repair_header_solution_poison mode =
        stage_repair_header_solution_poison_mode(db, repair_height);
    if (!validate_repairable_mode(mode))
        return false;

    if (out_height)
        *out_height = repair_height;
    if (out_mode)
        *out_mode = (int)mode;
    return true;
}

static void remember_validate_repair_owner(int repair_height, int mode,
                                           int64_t stall_target)
{
    atomic_store(&g_validate_repair_owner_mode, mode);
    int prev = atomic_exchange(&g_validate_repair_owner_height, repair_height);
    if (prev != repair_height) {
        LOG_WARN("condition",
                 "[condition:block_failed_mask_at_tip] delegated no_advance "
                 "stall_target=%lld repair_height=%d repair_mode=%d owner="
                 "stale_validate_headers_repair",
                 (long long)stall_target, repair_height, mode);
    }
}

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
        int repair_height = -1;
        int repair_mode = STAGE_REPAIR_POISON_NONE;
        if (repairable_validate_frontier_owns_stall(
                target, &repair_height, &repair_mode)) {
            remember_validate_repair_owner(repair_height, repair_mode, target);
            return false;
        }
        atomic_store(&g_validate_repair_owner_height, -1);
        atomic_store(&g_validate_repair_owner_mode, STAGE_REPAIR_POISON_NONE);
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
    int64_t target = atomic_load(&g_target_at_detect);
    if (target < 0)
        target = target_height();
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
    if (stall_type == BF_STALL_NO_ADVANCE) {
        int repair_height = -1;
        int repair_mode = STAGE_REPAIR_POISON_NONE;
        if (repairable_validate_frontier_owns_stall(
                target, &repair_height, &repair_mode)) {
            remember_validate_repair_owner(repair_height, repair_mode, target);
            return true;
        }
        return current_tip_height(ms) > atomic_load(&g_tip_at_detect);
    }
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
    int last_outcome = atomic_load(&s->last_outcome);
    int stall_type = atomic_load(&g_stall_type_at_detect);
    bool exhausted_outcome = last_outcome == COND_REMEDY_FAILED ||
        (stall_type == BF_STALL_NO_ADVANCE &&
         last_outcome == COND_REMEDY_UNWITNESSED);
    bool exhausted = atomic_load(&s->currently_active) &&
        attempts >= max_attempts &&
        exhausted_outcome;

    if (target_height)
        *target_height = (int)atomic_load(&g_target_at_detect);
    if (attempts_out)
        *attempts_out = attempts;
    return exhausted;
}

bool block_failed_mask_at_tip_recovery_exhausted_is_no_advance(void)
{
    return atomic_load(&g_stall_type_at_detect) == BF_STALL_NO_ADVANCE;
}

#ifdef ZCL_TESTING
void block_failed_mask_at_tip_test_reset(void)
{
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_tip_height_at_check, -1);
    atomic_store(&g_tip_unchanged_since, 0);
    atomic_store(&g_tip_at_detect, -1);
    atomic_store(&g_tip_age_at_detect, 0);
    atomic_store(&g_validate_repair_owner_height, -1);
    atomic_store(&g_validate_repair_owner_mode, STAGE_REPAIR_POISON_NONE);
    atomic_store(&g_stall_type_at_detect, BF_STALL_NONE);
    condition_reset_state(&c_block_failed_mask_at_tip);
}

int block_failed_mask_at_tip_test_stall_type(void)
{
    return atomic_load(&g_stall_type_at_detect);
}

int block_failed_mask_at_tip_test_target_height(void)
{
    return (int)atomic_load(&g_target_at_detect);
}

int block_failed_mask_at_tip_test_validate_repair_owner_height(void)
{
    return atomic_load(&g_validate_repair_owner_height);
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

void block_failed_mask_at_tip_test_mark_no_advance_exhausted(
    int target_height,
    int tip_height)
{
    struct condition_state *s = &c_block_failed_mask_at_tip.state;
    atomic_store(&g_target_at_detect, target_height);
    atomic_store(&g_tip_at_detect, tip_height);
    atomic_store(&g_stall_type_at_detect, BF_STALL_NO_ADVANCE);
    atomic_store(&s->currently_active, true);
    atomic_store(&s->attempts, c_block_failed_mask_at_tip.max_attempts);
    atomic_store(&s->last_outcome, COND_REMEDY_UNWITNESSED);
}
#endif
