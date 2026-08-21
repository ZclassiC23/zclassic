/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/body_fetch_missing_have_data.h"

#include "framework/condition.h"
#include "jobs/stage_repair.h"
#include "jobs/utxo_apply_stage.h"
#include "services/sync_monitor.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <string.h>

static _Atomic int g_target_at_detect = -1;
static _Atomic int g_remedy_calls = 0;

enum bfmhd_target_route {
    BFMHD_TARGET_NONE = 0,
    BFMHD_TARGET_ACTIVE_FRONTIER,
    BFMHD_TARGET_BEST_HEADER,
};

static _Atomic int g_target_route = BFMHD_TARGET_NONE;
static _Atomic bool g_target_hash_valid;
static struct uint256 g_target_hash;

enum bfmhd_exact_state {
    BFMHD_EXACT_READY = 0,
    BFMHD_EXACT_HAVE_DATA,
    BFMHD_EXACT_BEST_ABSENT,
    BFMHD_EXACT_BEST_HASH_MISMATCH,
    BFMHD_EXACT_PARENT_ABSENT,
    BFMHD_EXACT_PARENT_MISMATCH,
    BFMHD_EXACT_FAILED,
};

static _Atomic int g_last_skip_state = BFMHD_EXACT_READY;

static const char *bfmhd_exact_state_name(enum bfmhd_exact_state state)
{
    static const char *const names[] = {
        "ready",
        "exact_have_data",
        "best_absent",
        "best_hash_mismatch",
        "visible_parent_absent",
        "visible_parent_mismatch",
        "authority_failed",
    };
    return state >= BFMHD_EXACT_READY && state <= BFMHD_EXACT_FAILED
        ? names[state] : "unknown";
}

/* Test seam: the mid-chain candidate reads utxo_apply's own select-idle
 * record (see jobs/utxo_apply_stage.h) — real side effects a unit test
 * cannot manufacture without driving the whole reducer pipeline, so route
 * through overridable function pointers (same seam as the sibling
 * have_data_unreadable Condition, kept file-local rather than shared since
 * each condition file is a self-contained unit). */
typedef int64_t (*bfmhd_select_idle_height_fn)(void);
typedef bool (*bfmhd_select_idle_is_read_failure_fn)(void);

static bfmhd_select_idle_height_fn g_select_idle_height_fn =
    utxo_apply_stage_select_idle_height;
static bfmhd_select_idle_is_read_failure_fn g_select_idle_is_read_failure_fn =
    utxo_apply_stage_select_idle_is_read_failure;

static struct block_index *active_target_index_locked(struct main_state *ms,
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

/* Candidate 1 is a durable validate_headers verdict above the finalized
 * active window. Resolve it by the verdict's exact hash on best-header
 * ancestry, then prove that ancestry still extends the visible parent. A
 * height-only block-map scan may select a data-bearing stale sibling and
 * suppress the canonical fetch indefinitely. */
static struct block_index *validated_best_header_target_locked(
    struct main_state *ms, int target, const struct uint256 *expected_hash,
    enum bfmhd_exact_state *out_state)
{
    *out_state = BFMHD_EXACT_BEST_ABSENT;
    if (!ms || target < 0 || !expected_hash || !ms->pindex_best_header ||
        target > ms->pindex_best_header->nHeight)
        return NULL;

    struct block_index *bi = block_index_get_ancestor(
        ms->pindex_best_header, target);
    if (!bi || bi->nHeight != target || !bi->phashBlock)
        return NULL;
    if (!uint256_eq(bi->phashBlock, expected_hash)) {
        *out_state = BFMHD_EXACT_BEST_HASH_MISMATCH;
        return NULL;
    }
    if (block_has_any_failure(bi)) {
        *out_state = BFMHD_EXACT_FAILED;
        return NULL;
    }
    if (target == 0) {
        *out_state = (block_index_status_load(bi) & BLOCK_HAVE_DATA)
            ? BFMHD_EXACT_HAVE_DATA : BFMHD_EXACT_READY;
        return bi;
    }

    struct block_index *visible_parent =
        active_chain_at(&ms->chain_active, target - 1);
    if (!visible_parent || !visible_parent->phashBlock) {
        *out_state = BFMHD_EXACT_PARENT_ABSENT;
        return NULL;
    }
    if (!bi->pprev || !bi->pprev->phashBlock ||
        !uint256_eq(bi->pprev->phashBlock, visible_parent->phashBlock)) {
        *out_state = BFMHD_EXACT_PARENT_MISMATCH;
        return NULL;
    }
    *out_state = (block_index_status_load(bi) & BLOCK_HAVE_DATA)
        ? BFMHD_EXACT_HAVE_DATA : BFMHD_EXACT_READY;
    return bi;
}

static struct block_index *target_for_route_locked(
    struct main_state *ms, int target, enum bfmhd_target_route route,
    const struct uint256 *expected_hash)
{
    struct block_index *bi = NULL;
    if (route == BFMHD_TARGET_BEST_HEADER) {
        enum bfmhd_exact_state state = BFMHD_EXACT_READY;
        bi = validated_best_header_target_locked(
            ms, target, expected_hash, &state);
    } else {
        bi = active_target_index_locked(ms, target);
    }
    if (!bi || !bi->phashBlock ||
        (expected_hash && !uint256_eq(bi->phashBlock, expected_hash)))
        return NULL;
    return bi;
}

static bool target_has_readable_data(
    struct main_state *ms, int target, enum bfmhd_target_route route,
    const struct uint256 *expected_hash)
{
    bool readable = false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = target_for_route_locked(
        ms, target, route, expected_hash);
    if (bi && (bi->nStatus & BLOCK_HAVE_DATA)) {
        char datadir[2048];
        GetDataDir(true, datadir, sizeof(datadir));
        readable = block_index_have_data_readable(bi, datadir);
    }
    zcl_mutex_unlock(&ms->cs_main);
    return readable;
}

static bool active_target_missing_data(struct main_state *ms, int target,
                                       struct uint256 *out_hash)
{
    bool missing = false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = active_target_index_locked(ms, target);
    if (bi) {
        missing = (bi->nStatus & BLOCK_HAVE_DATA) == 0;
        if (missing && out_hash)
            *out_hash = *bi->phashBlock;
    }
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

    int target = -1;
    enum bfmhd_target_route route = BFMHD_TARGET_NONE;
    struct uint256 target_hash;
    memset(&target_hash, 0, sizeof(target_hash));

    /* Candidate 1: the body_fetch stage's own frontier cursor — the class
     * this Condition originally healed (validate_headers led body_fetch with
     * no observed body). */
    struct stage_repair_body_fetch_gap gap;
    if (stage_repair_body_fetch_missing_have_data_frontier_candidate(
            db, &gap) && !gap.body_observed) {
        bool missing = false;
        enum bfmhd_exact_state exact_state = BFMHD_EXACT_BEST_ABSENT;
        if (gap.has_target_hash) {
            zcl_mutex_lock(&ms->cs_main);
            struct block_index *bi = validated_best_header_target_locked(
                ms, gap.target_height, &gap.target_hash, &exact_state);
            if (bi && !(block_index_status_load(bi) & BLOCK_HAVE_DATA)) {
                missing = true;
                target_hash = gap.target_hash;
            }
            zcl_mutex_unlock(&ms->cs_main);
        }
        if (missing) {
            target = gap.target_height;
            route = BFMHD_TARGET_BEST_HEADER;
        } else {
            atomic_store(&g_last_skip_state, exact_state);
            LOG_WARN("condition",
                     "[condition:body_fetch_missing_have_data] canonical "
                     "validated target h=%d skip_reason=%s",
                     gap.target_height,
                     bfmhd_exact_state_name(exact_state));
        }
    }

    /* Candidate 2: an ARBITRARY mid-chain height the reducer is stuck
     * re-reading — utxo_apply's select-idle record, the same signal the
     * sibling have_data_unreadable Condition targets. Once that Condition
     * clears the provably-bogus HAVE_DATA flag there, the frontier-cursor
     * candidate above never matches it (body_fetch's own cursor has long
     * since passed that height), so this is the path that re-queues the P2P
     * fetch for it. Prefer the lower of the two candidates. */
    if (g_select_idle_is_read_failure_fn()) {
        int64_t ua_h = g_select_idle_height_fn();
        if (ua_h >= 0 && (target < 0 || ua_h < target) &&
            active_target_missing_data(ms, (int)ua_h, &target_hash)) {
            target = (int)ua_h;
            route = BFMHD_TARGET_ACTIVE_FRONTIER;
        }
    }

    if (target < 0)
        return false;

    atomic_store(&g_target_at_detect, target);
    g_target_hash = target_hash;
    atomic_store(&g_target_hash_valid, true);
    atomic_store(&g_target_route, route);
    return true;
}

static enum condition_remedy_result remedy_body_fetch_missing_have_data(void)
{
    int target = atomic_load(&g_target_at_detect);
    enum bfmhd_target_route route =
        (enum bfmhd_target_route)atomic_load(&g_target_route);
    if (target < 0 || route == BFMHD_TARGET_NONE ||
        !atomic_load(&g_target_hash_valid))
        return COND_REMEDY_SKIP;

    /* Re-prove detect's exact identity before asking the shared queue path.
     * A concurrent best-header switch is safe but belongs to the next detect
     * tick, never to this captured remedy. */
    struct main_state *ms = sync_monitor_main_state();
    if (!ms)
        return COND_REMEDY_FAILED;
    zcl_mutex_lock(&ms->cs_main);
    bool still_exact = target_for_route_locked(
        ms, target, route, &g_target_hash) != NULL;
    zcl_mutex_unlock(&ms->cs_main);
    if (!still_exact)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);
    struct zcl_result r = route == BFMHD_TARGET_BEST_HEADER
        ? sync_monitor_queue_best_header_body(
              target, &g_target_hash,
              "condition:body_fetch_missing_have_data canonical")
        : sync_monitor_queue_active_frontier_body(
              target, "condition:body_fetch_missing_have_data mid-chain");
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
    enum bfmhd_target_route route =
        (enum bfmhd_target_route)atomic_load(&g_target_route);
    sqlite3 *db = progress_store_db();
    if (db && target >= 0 && atomic_load(&g_target_hash_valid) &&
        stage_repair_body_fetch_observed_hash(
            db, target, &g_target_hash))
        return true;

    struct main_state *ms = sync_monitor_main_state();
    if (!ms || target < 0)
        return false;
    return atomic_load(&g_target_hash_valid) &&
        target_has_readable_data(ms, target, route, &g_target_hash);
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
static int64_t bfmhd_test_no_select_idle_height(void) { return -1; } // raw-return-ok:test-stub-no-signal-sentinel
static bool bfmhd_test_no_select_idle_read_failure(void) { return false; }

void body_fetch_missing_have_data_test_reset(void)
{
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_remedy_calls, 0);
    atomic_store(&g_target_route, BFMHD_TARGET_NONE);
    atomic_store(&g_target_hash_valid, false);
    atomic_store(&g_last_skip_state, BFMHD_EXACT_READY);
    memset(&g_target_hash, 0, sizeof(g_target_hash));
    g_select_idle_height_fn = bfmhd_test_no_select_idle_height;
    g_select_idle_is_read_failure_fn = bfmhd_test_no_select_idle_read_failure;
    condition_reset_state(&c_body_fetch_missing_have_data);
}

int body_fetch_missing_have_data_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}

const char *body_fetch_missing_have_data_test_last_skip_reason(void)
{
    return bfmhd_exact_state_name(
        (enum bfmhd_exact_state)atomic_load(&g_last_skip_state));
}

void body_fetch_missing_have_data_test_set_select_idle_stubs(
    int64_t (*height_fn)(void), bool (*is_read_failure_fn)(void))
{
    g_select_idle_height_fn =
        height_fn ? height_fn : bfmhd_test_no_select_idle_height;
    g_select_idle_is_read_failure_fn =
        is_read_failure_fn ? is_read_failure_fn
                           : bfmhd_test_no_select_idle_read_failure;
}
#endif
