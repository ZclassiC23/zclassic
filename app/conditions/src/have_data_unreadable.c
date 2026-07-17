/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/have_data_unreadable.h"
#include "framework/condition.h"
#include "util/log_macros.h"

#include "chain/chain.h"
#include "event/event.h"
#include "jobs/utxo_apply_stage.h"
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

/* Test seam: the mid-chain candidate reads utxo_apply's own select-idle
 * record (see jobs/utxo_apply_stage.h) — real side effects a unit test
 * cannot manufacture without driving the whole reducer pipeline, so route
 * through overridable function pointers exactly like
 * tip_stall_oracle_rebuild.c's oracle/rebuild seams. */
typedef int64_t (*hdu_select_idle_height_fn)(void);
typedef bool (*hdu_select_idle_is_read_failure_fn)(void);

static hdu_select_idle_height_fn g_select_idle_height_fn =
    utxo_apply_stage_select_idle_height;
static hdu_select_idle_is_read_failure_fn g_select_idle_is_read_failure_fn =
    utxo_apply_stage_select_idle_is_read_failure;

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

/* `h` as a candidate iff a block there is HAVE_DATA-flagged but not actually
 * readable from disk (the provably-bogus flag this Condition heals). */
static struct block_index *unreadable_at(struct main_state *ms, int h,
                                         const char *datadir)
{
    struct block_index *p = target_index(ms, h);
    if (!p)
        return NULL;
    if (block_index_have_data_readable(p, datadir))
        return NULL;
    return p;
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

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));

    /* Candidate 1: the live tip's immediate next block — the class this
     * Condition originally healed (a torn HAVE_DATA flag wedging the tip). */
    int target = tip + 1;
    struct block_index *p = unreadable_at(ms, target, datadir);

    /* Candidate 2: the LOWEST read-failed height across the reducer stages,
     * NOT necessarily tip+1 — utxo_apply's own select-idle record of the
     * height it is currently stuck trying to read a body for (e.g. mid-chain,
     * during a stale-script/coin-backfill replay that rewinds the stage
     * cursor to re-derive an OLDER height). Only a genuine local read failure
     * (INDEXED_BODY_READ_FAILED / STAGE_READ_FAILED) qualifies — a missing or
     * hash-mismatched body is a different repair's domain. Prefer the lower
     * of the two candidates so the earliest blocker heals first; re-verified
     * against the live block_index (the atomic is a sticky "last observed"
     * value, not necessarily still pending). */
    if (g_select_idle_is_read_failure_fn()) {
        int64_t ua_h = g_select_idle_height_fn();
        if (ua_h >= 0 && (p == NULL || ua_h < target)) {
            struct block_index *ua_p = unreadable_at(ms, (int)ua_h, datadir);
            if (ua_p) {
                target = (int)ua_h;
                p = ua_p;
            }
        }
    }

    if (!p)
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

    struct block_index *p = target_index(ms, target);
    if (!p)
        return true;

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));
    if (block_index_have_data_readable(p, datadir))
        return true;

    /* The reducer stage's own cursor advancing past target is the
     * authoritative "re-derived through the repaired height" signal for the
     * mid-chain (backfill/replay) case: there the active/finalized tip can
     * already sit above an arbitrary rewound target even though nothing has
     * actually been repaired yet, so tip-height alone is not a safe witness.
     * Covers the tip+1 case too — once the block is re-fetched and applied,
     * the cursor advances to target+1. */
    return (int64_t)utxo_apply_stage_cursor() > (int64_t)target;
}

static struct condition c_have_data_unreadable = {
    .name = "have_data_unreadable",
    .severity = COND_WARN,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 3,
    /* Continue-with-cooldown (sticky-node plan #7): the remedy depends on an
     * external resource (a P2P re-fetch of the cleared body, driven by the
     * sibling body_fetch_missing_have_data Condition) — giving up forever at
     * max_attempts would leave a mid-chain unreadable/corrupt body wedged
     * permanently. After 3 fast attempts (one operator page per episode)
     * re-arm every 10 minutes so the clear+re-fetch keeps retrying; unbounded
     * re-arms (0) since this is a purely external dependency, same posture as
     * body_fetch_missing_have_data. */
    .cooldown_secs = 600,
    .cooldown_max_rearms = 0,
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
static int64_t hdu_test_no_select_idle_height(void) { return -1; } // raw-return-ok:test-stub-no-signal-sentinel
static bool hdu_test_no_select_idle_read_failure(void) { return false; }

void have_data_unreadable_test_reset(void)
{
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_file_at_detect, -1);
    atomic_store(&g_pos_at_detect, 0);
    atomic_store(&g_remedy_calls, 0);
    g_select_idle_height_fn = hdu_test_no_select_idle_height;
    g_select_idle_is_read_failure_fn = hdu_test_no_select_idle_read_failure;
    condition_reset_state(&c_have_data_unreadable);
}

int have_data_unreadable_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}

void have_data_unreadable_test_set_select_idle_stubs(
    int64_t (*height_fn)(void), bool (*is_read_failure_fn)(void))
{
    g_select_idle_height_fn =
        height_fn ? height_fn : hdu_test_no_select_idle_height;
    g_select_idle_is_read_failure_fn =
        is_read_failure_fn ? is_read_failure_fn
                           : hdu_test_no_select_idle_read_failure;
}
#endif
