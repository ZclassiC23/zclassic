/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

/* Condition: tip_fork_stale
 *
 * The capstone self-healer for the "stale data-bearing fork at tip+1"
 * wedge class. The active tip stops
 * advancing because a STALE block sits at tip+1 as an active-tip child
 * (it carries BLOCK_HAVE_DATA), the evidence controller correctly
 * refuses to connect it (incomplete_index_evidence, nakamoto=0), and the
 * REAL higher-work chain — visible in the header tree (pindex_best_header
 * now has strictly more chainwork than the active tip) — has bodies the
 * node never fetched. find_most_work_chain keeps re-selecting the stale
 * child and the tip never moves.
 *
 * detect() fires TRUE only when ALL three hold:
 *   (a) the active tip has not advanced for a sustained window
 *       (TIP_STALL_SECS) — same tip-age signal block_failed_mask_at_tip
 *       uses;
 *   (b) a higher-work HEADER chain exists — pindex_best_header has
 *       strictly more nChainWork than the active tip (best_header is
 *       chainwork-ranked);
 *   (c) the block the node keeps trying to connect at tip+1 is a CHILD of
 *       the active tip carrying BLOCK_HAVE_DATA, AND it is NOT on the
 *       best-header chain:
 *         block_index_get_ancestor(best_header, tip+1) != that_child.
 *       i.e. the higher-work chain has a DIFFERENT block at tip+1 — the
 *       data-bearing child the node keeps retrying is a confirmed stale
 *       fork.
 *
 * SAFETY: detect is deliberately conservative. We NEVER fire on normal
 * IBD / catch-up where the tip+1 child we are missing data for IS on the
 * best-header chain (just missing its body): in that case
 * get_ancestor(best_header, tip+1) == that_child, so condition (c) is
 * false and we stay quiet. We only ever invalidate a block that is
 * provably NOT on the most-work header chain.
 *
 * remedy():
 *   1. process_block_invalidate(stale tip+1 child) — marks it
 *      BLOCK_FAILED_VALID + disconnects/reorgs via the validated path so
 *      find_most_work_chain stops selecting it.
 *   2. rebuild_recent(from ~tip-2) — fetch+connect the canonical chain's
 *      recent bodies from zclassicd through the normal validated accept
 *      path.
 *   Both are bounded and idempotent. No new consensus writer.
 *
 * witness(): the active tip advanced beyond its detect-time height.
 */

#include "conditions/tip_fork_stale.h"
#include "framework/condition.h"
#include "util/log_macros.h"

#include "controllers/repair_controller.h"
#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "platform/time_compat.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block_invalidate.h"

#include <stdatomic.h>
#include <stddef.h>

/* Sustained no-advance window before we even consider a stale-fork stall.
 * Matches block_failed_mask_at_tip's 300 s — a normal block is ~75 s, so
 * 300 s with no advance and a higher-work header chain is clearly stuck. */
#define TIP_STALL_SECS 300

static _Atomic int64_t g_tip_height_at_check = -1;
static _Atomic int64_t g_tip_unchanged_since = 0;
static _Atomic int64_t g_tip_at_detect = -1;
static _Atomic int64_t g_stale_child_height = -1;
/* Hash of the confirmed-stale tip+1 child captured at detect time, so the
 * remedy invalidates EXACTLY the block detect proved is off the best-header
 * chain (never re-resolves a different block between detect and remedy). */
static struct uint256 g_stale_child_hash;
static _Atomic bool g_stale_child_valid = false;

/* Test seams: the remedy's two real side effects are an invalidate (reaches
 * the activation controller) and a rebuild_recent (reaches zclassicd). Both
 * are unavailable in a unit test, so route them through overridable function
 * pointers that default to the production calls. */
typedef enum invalidate_result (*tfs_invalidate_fn)(
    struct main_state *ms, const struct uint256 *hash,
    struct uint256 *out_hash);
typedef bool (*tfs_rebuild_fn)(int from_height);

static enum invalidate_result tfs_default_invalidate(
    struct main_state *ms, const struct uint256 *hash,
    struct uint256 *out_hash)
{
    return process_block_invalidate(ms, hash, out_hash);
}

static bool tfs_default_rebuild(int from_height)
{
    return rebuild_recent_repair(from_height);
}

static tfs_invalidate_fn g_invalidate_fn = tfs_default_invalidate;
static tfs_rebuild_fn g_rebuild_fn = tfs_default_rebuild;

#ifdef ZCL_TESTING
static _Atomic int g_test_invalidate_calls;
static _Atomic int g_test_rebuild_calls;
static _Atomic int64_t g_test_last_invalidate_height = -1;
static _Atomic int g_test_last_rebuild_from = -1;
#endif

static struct main_state *ms_or_null(void)
{
    return condition_engine_main_state();
}

static int64_t current_tip_height(struct main_state *ms)
{
    return ms ? (int64_t)active_chain_height(&ms->chain_active) : -1;
}

/* Among the children at `target` height, return the one that is a CHILD of
 * the active tip carrying BLOCK_HAVE_DATA but NOT itself failed — i.e. the
 * data-bearing block the node keeps retrying at tip+1. NULL if none. */
static struct block_index *find_active_tip_child_with_data(
    struct main_state *ms, struct block_index *tip, int target_height)
{
    if (!ms || !tip)
        return NULL;
    size_t iter = 0;
    struct block_index *p = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p || p->nHeight != target_height)
            continue;
        if (p->pprev != tip)
            continue; /* must be a direct child of the active tip */
        if (!(p->nStatus & BLOCK_HAVE_DATA))
            continue;
        if (block_has_any_failure(p))
            continue; /* already invalidated — nothing to heal */
        return p;
    }
    return NULL;
}

static bool detect_tip_fork_stale(void)
{
    struct main_state *ms = ms_or_null();
    if (!ms)
        return false;

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    int64_t tip_h = current_tip_height(ms);
    if (!tip || tip_h <= 0)
        return false; /* tip not established — stay quiet */

    /* (a) sustained no-advance window. Reset the timer whenever the tip
     * moves; only a tip that has held still for TIP_STALL_SECS qualifies. */
    int64_t now = platform_time_wall_unix();
    int64_t prev_tip = atomic_load(&g_tip_height_at_check);
    if (prev_tip != tip_h) {
        atomic_store(&g_tip_height_at_check, tip_h);
        atomic_store(&g_tip_unchanged_since, now);
        return false;
    }
    int64_t since = atomic_load(&g_tip_unchanged_since);
    if (since == 0) {
        atomic_store(&g_tip_unchanged_since, now);
        return false;
    }
    if (now - since < TIP_STALL_SECS)
        return false;

    /* (b) a strictly higher-work HEADER chain exists. */
    struct block_index *bh = ms->pindex_best_header;
    if (!bh)
        return false;
    if (arith_uint256_compare(&bh->nChainWork, &tip->nChainWork) <= 0)
        return false; /* no more-work header chain — normal at-tip */
    if (bh->nHeight <= tip_h)
        return false; /* best header not actually ahead — not a fork stall */

    /* (c) the data-bearing tip+1 child the node keeps retrying is NOT on
     * the best-header chain. */
    int target = (int)tip_h + 1;
    struct block_index *child =
        find_active_tip_child_with_data(ms, tip, target);
    if (!child)
        return false; /* nothing stuck at tip+1 — normal missing-body IBD */

    struct block_index *bh_at_target =
        block_index_get_ancestor(bh, target);
    if (bh_at_target == child)
        return false; /* child IS on the best-header chain — LEGIT, never
                       * invalidate. Just a missing body; let IBD fetch it. */

    /* Confirmed stale fork: tip stalled, a higher-work header chain exists,
     * and the data-bearing child at tip+1 is off that chain. */
    atomic_store(&g_tip_at_detect, tip_h);
    atomic_store(&g_stale_child_height, target);
    if (child->phashBlock) {
        g_stale_child_hash = *child->phashBlock;
        atomic_store(&g_stale_child_valid, true);
    } else {
        atomic_store(&g_stale_child_valid, false);
    }
    return true;
}

static enum condition_remedy_result remedy_tip_fork_stale(void)
{
    struct main_state *ms = ms_or_null();
    if (!ms || !atomic_load(&g_stale_child_valid))
        return COND_REMEDY_SKIP;

    int64_t tip_at_detect = atomic_load(&g_tip_at_detect);
    int64_t child_h = atomic_load(&g_stale_child_height);
    struct uint256 stale = g_stale_child_hash;

    /* (1) Invalidate the confirmed-stale tip+1 child so find_most_work_chain
     * stops selecting it. Reuses the validated disconnect/reorg path. */
    struct uint256 out_hash;
    enum invalidate_result ir = g_invalidate_fn(ms, &stale, &out_hash);
#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_invalidate_calls, 1);
    atomic_store(&g_test_last_invalidate_height, child_h);
#endif
    if (ir != INVALIDATE_OK) {
        LOG_WARN("condition",
            "[condition:tip_fork_stale] invalidate h=%lld result=%s — "
            "remedy aborted before rebuild",
            (long long)child_h, invalidate_result_name(ir));
        return COND_REMEDY_FAILED;
    }

    /* (2) Pull the canonical recent bodies from zclassicd and connect them
     * through the normal validated accept path. Start a small margin below
     * the detect-time tip so a 1-block stale fork is always inside window. */
    int from_h = (int)(tip_at_detect - 2);
    if (from_h < 0)
        from_h = 0;
    bool rebuilt = g_rebuild_fn(from_h);
#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_rebuild_calls, 1);
    atomic_store(&g_test_last_rebuild_from, from_h);
#endif

    int64_t tip_now = current_tip_height(ms);
    LOG_WARN("condition",
        "[condition:tip_fork_stale] invalidated stale child h=%lld, "
        "rebuild_recent from=%d ok=%s tip %lld -> %lld",
        (long long)child_h, from_h, rebuilt ? "yes" : "no",
        (long long)tip_at_detect, (long long)tip_now);

    if (!rebuilt)
        return COND_REMEDY_FAILED;
    /* The engine downgrades to UNWITNESSED if the tip did not actually
     * advance, so returning OK here only sticks when witness() confirms. */
    return COND_REMEDY_OK;
}

static bool witness_tip_fork_stale(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct main_state *ms = ms_or_null();
    if (!ms)
        return false;
    return current_tip_height(ms) > atomic_load(&g_tip_at_detect);
}

static struct condition c_tip_fork_stale = {
    .name = "tip_fork_stale",
    .severity = COND_CRITICAL,
    .poll_secs = 10,
    .backoff_secs = 60,
    .max_attempts = 3,
    .detect = detect_tip_fork_stale,
    .remedy = remedy_tip_fork_stale,
    .witness = witness_tip_fork_stale,
    .witness_window_secs = 120,
};

void register_tip_fork_stale(void)
{
    (void)condition_register(&c_tip_fork_stale);
}

#ifdef ZCL_TESTING
void tip_fork_stale_test_reset(void)
{
    atomic_store(&g_tip_height_at_check, -1);
    atomic_store(&g_tip_unchanged_since, 0);
    atomic_store(&g_tip_at_detect, -1);
    atomic_store(&g_stale_child_height, -1);
    atomic_store(&g_stale_child_valid, false);
    g_invalidate_fn = tfs_default_invalidate;
    g_rebuild_fn = tfs_default_rebuild;
    atomic_store(&g_test_invalidate_calls, 0);
    atomic_store(&g_test_rebuild_calls, 0);
    atomic_store(&g_test_last_invalidate_height, -1);
    atomic_store(&g_test_last_rebuild_from, -1);
    condition_reset_state(&c_tip_fork_stale);
}

/* Force the no-advance stall timer to satisfy the TIP_STALL_SECS window
 * without waiting in real time. Pins the tip-height/unchanged-since state
 * to (tip_h, now - age_secs). */
void tip_fork_stale_test_force_stall(int64_t tip_h, int64_t age_secs)
{
    atomic_store(&g_tip_height_at_check, tip_h);
    atomic_store(&g_tip_unchanged_since,
                 platform_time_wall_unix() - age_secs);
}

void tip_fork_stale_test_set_remedy_stubs(
    enum invalidate_result (*inv)(struct main_state *,
                                  const struct uint256 *,
                                  struct uint256 *),
    bool (*reb)(int))
{
    if (inv) g_invalidate_fn = inv;
    if (reb) g_rebuild_fn = reb;
}

int tip_fork_stale_test_invalidate_calls(void)
{
    return atomic_load(&g_test_invalidate_calls);
}

int tip_fork_stale_test_rebuild_calls(void)
{
    return atomic_load(&g_test_rebuild_calls);
}

int64_t tip_fork_stale_test_last_invalidate_height(void)
{
    return atomic_load(&g_test_last_invalidate_height);
}

int tip_fork_stale_test_last_rebuild_from(void)
{
    return atomic_load(&g_test_last_rebuild_from);
}
#endif
