/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the tip_fork_stale Condition — the capstone self-healer for
 * the "stale data-bearing fork at tip+1" wedge class.
 *
 * The Condition's detect/remedy/witness function pointers are static in
 * app/conditions/src/tip_fork_stale.c. We drive it the canonical way —
 * through condition_engine_tick() — and assert via engine-observable
 * state plus the ZCL_TESTING seams:
 *   - tip_fork_stale_test_force_stall(): satisfy the sustained no-advance
 *     window without waiting TIP_STALL_SECS in wall time.
 *   - tip_fork_stale_test_set_remedy_stubs(): replace the two real side
 *     effects (process_block_invalidate, which reaches the activation
 *     controller, and rebuild_recent, which reaches zclassicd) with test
 *     stubs so we can assert the remedy CALLS them with the right args
 *     without any external dependency.
 *
 * Chain-construction pattern reused from test_orphan_utxo_above_tip.c:
 * a minimal in-RAM main_state chain. We build a MAIN chain (the active
 * tip), then a stale fork branching at the tip, plus a higher-work
 * header-only chain. pindex_best_header points at the higher-work head.
 */

#include "test/test_helpers.h"

#include "conditions/tip_fork_stale.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "framework/condition.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block_invalidate.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TFS_CHECK(name, expr) do {                 \
    printf("tip_fork_stale: %s... ", (name));      \
    if (expr) printf("OK\n");                       \
    else { printf("FAIL\n"); failures++; }          \
} while (0)

/* Stub remedy side effects: record the calls, never touch real state. */
static _Atomic int g_stub_invalidate_calls;
static _Atomic int g_stub_rebuild_calls;
static _Atomic bool g_stub_rebuild_ret = true;
/* When true, after the (stub) remedy runs we advance the active tip so the
 * witness confirms the heal — modelling a successful invalidate+rebuild. */
static struct main_state *g_advance_ms;
static struct block_index *g_advance_to;

static enum invalidate_result stub_invalidate(struct main_state *ms,
                                              const struct uint256 *hash,
                                              struct uint256 *out_hash)
{
    (void)ms; (void)hash;
    if (out_hash) memset(out_hash, 0, sizeof(*out_hash));
    atomic_fetch_add(&g_stub_invalidate_calls, 1);
    return INVALIDATE_OK;
}

static bool stub_rebuild(int from_height)
{
    (void)from_height;
    atomic_fetch_add(&g_stub_rebuild_calls, 1);
    /* Model a successful canonical rebuild: advance the active tip so the
     * witness (tip advanced beyond detect height) passes. */
    if (g_advance_ms && g_advance_to)
        active_chain_move_window_tip(&g_advance_ms->chain_active, g_advance_to);
    return atomic_load(&g_stub_rebuild_ret);
}

/* Insert one block_index at height h with the given prev + chainwork. */
static struct block_index *tfs_insert(struct main_state *ms,
                                      struct uint256 *hash, int salt,
                                      int h, struct block_index *prev,
                                      uint64_t chainwork, unsigned status)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(h & 0xFF);
    hash->data[1] = (uint8_t)((h >> 8) & 0xFF);
    hash->data[2] = (uint8_t)(salt & 0xFF);
    hash->data[3] = 0xC5; /* distinct salt from other tests */

    struct block_index *pi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!pi) return NULL;
    pi->nHeight = h;
    pi->nBits = 0x1f07ffff;
    pi->nTime = 1000000 + (uint32_t)h * 150;
    pi->nVersion = 4;
    pi->nStatus = status;
    pi->nTx = 1;
    pi->nChainTx = (uint32_t)(h + 1);
    arith_uint256_set_u64(&pi->nChainWork, chainwork);
    pi->pprev = prev;
    return pi;
}

/* Build the common base chain [0..tip_h] (main, active). Returns the tip. */
static struct block_index *tfs_build_main(struct main_state *ms,
                                          struct uint256 *hashes, int tip_h)
{
    struct block_index *prev = NULL;
    for (int h = 0; h <= tip_h; h++) {
        prev = tfs_insert(ms, &hashes[h], 0, h, prev,
                          (uint64_t)(h + 1),
                          BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
    }
    active_chain_move_window_tip(&ms->chain_active, prev);
    return prev;
}

int test_tip_fork_stale(void)
{
    printf("\n=== tip_fork_stale condition tests ===\n");
    int failures = 0;
    const int TIP_H = 100;

    /* ── 1. Confirmed stale fork → detect TRUE, remedy invalidates +
     *      rebuilds, witness confirms advancement. ───────────────── */
    {
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        atomic_store(&g_stub_invalidate_calls, 0);
        atomic_store(&g_stub_rebuild_calls, 0);
        atomic_store(&g_stub_rebuild_ret, true);

        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[256];
        struct block_index *tip = tfs_build_main(&ms, hashes, TIP_H);

        /* Stale fork: a data-bearing child of the active tip at tip+1, with
         * LOW chainwork (it is the stuck block the node keeps retrying). */
        struct uint256 stale_h;
        struct block_index *stale = tfs_insert(&ms, &stale_h, 1, TIP_H + 1,
            tip, (uint64_t)(TIP_H + 2),
            BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA);

        /* Higher-work header-only chain: branch off the SAME tip but climb
         * higher with strictly more chainwork. Its block at tip+1 is a
         * DIFFERENT block than the stale child (header only, no data). */
        struct uint256 hdr_h[8];
        struct block_index *prev = tip;
        struct block_index *best_header = NULL;
        for (int i = 1; i <= 5; i++) {
            int h = TIP_H + i;
            /* +1000 chainwork per step => strictly more than active tip. */
            prev = tfs_insert(&ms, &hdr_h[i], 2 + i, h, prev,
                              (uint64_t)(TIP_H + 1 + i * 1000),
                              BLOCK_VALID_TREE); /* header only, no data */
            best_header = prev;
        }
        ms.pindex_best_header = best_header;

        /* Sanity: the stale child is NOT the best-header chain's tip+1. */
        bool setup_ok = (stale != NULL && best_header != NULL);

        condition_engine_set_main_state(&ms);
        /* Model a successful heal: rebuild advances the active tip onto the
         * higher-work header chain (use its tip+1 as the new active tip). */
        g_advance_ms = &ms;
        g_advance_to = block_index_get_ancestor(best_header, TIP_H + 2);

        register_tip_fork_stale();
        tip_fork_stale_test_set_remedy_stubs(stub_invalidate, stub_rebuild);
        /* Satisfy the sustained no-advance window. */
        tip_fork_stale_test_force_stall(TIP_H, 400);

        condition_engine_tick();

        bool ok = setup_ok;
        /* remedy ran: invalidate once, rebuild once. */
        ok = ok && tip_fork_stale_test_invalidate_calls() == 1;
        ok = ok && tip_fork_stale_test_rebuild_calls() == 1;
        /* invalidate targeted the stale tip+1 child height. */
        ok = ok && tip_fork_stale_test_last_invalidate_height() == TIP_H + 1;
        /* rebuild started a small margin below detect-time tip. */
        ok = ok && tip_fork_stale_test_last_rebuild_from() == TIP_H - 2;
        /* witness confirmed the heal (tip advanced) so condition cleared. */
        ok = ok && condition_engine_get_active_count() == 0;
        TFS_CHECK("stale fork -> detect true, invalidate+rebuild, witnessed",
                  ok);

        condition_engine_set_main_state(NULL);
        g_advance_ms = NULL; g_advance_to = NULL;
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        main_state_free(&ms);
    }

    /* ── 2. Legit catch-up: tip+1 child IS on the best-header chain (just
     *      missing body) → detect FALSE, NO invalidate. ──────────── */
    {
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        atomic_store(&g_stub_invalidate_calls, 0);
        atomic_store(&g_stub_rebuild_calls, 0);

        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[256];
        struct block_index *tip = tfs_build_main(&ms, hashes, TIP_H);

        /* The tip+1 child the node has data for. It will ALSO be the
         * best-header chain's tip+1 (i.e. on the canonical chain). */
        struct uint256 child_h;
        struct block_index *child = tfs_insert(&ms, &child_h, 1, TIP_H + 1,
            tip, (uint64_t)(TIP_H + 2),
            BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA);

        /* Higher-work header chain that EXTENDS through this same child. */
        struct uint256 hdr_h[8];
        struct block_index *prev = child;
        struct block_index *best_header = child;
        for (int i = 2; i <= 5; i++) {
            int h = TIP_H + i;
            prev = tfs_insert(&ms, &hdr_h[i], 2 + i, h, prev,
                              (uint64_t)(TIP_H + 1 + i * 1000),
                              BLOCK_VALID_TREE);
            best_header = prev;
        }
        ms.pindex_best_header = best_header;

        condition_engine_set_main_state(&ms);
        register_tip_fork_stale();
        tip_fork_stale_test_set_remedy_stubs(stub_invalidate, stub_rebuild);
        tip_fork_stale_test_force_stall(TIP_H, 400);

        condition_engine_tick();

        bool ok = (child != NULL && best_header != NULL);
        /* get_ancestor(best_header, tip+1) == child -> condition (c) false:
         * the child IS on the canonical chain. Must NOT fire / invalidate. */
        ok = ok && block_index_get_ancestor(best_header, TIP_H + 1) == child;
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && tip_fork_stale_test_invalidate_calls() == 0;
        ok = ok && tip_fork_stale_test_rebuild_calls() == 0;
        TFS_CHECK("legit catch-up (child on best-header chain) -> detect "
                  "false, NO invalidate", ok);

        condition_engine_set_main_state(NULL);
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        main_state_free(&ms);
    }

    /* ── 3. No higher-work header chain (normal at-tip) → detect FALSE. ── */
    {
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        atomic_store(&g_stub_invalidate_calls, 0);

        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[256];
        struct block_index *tip = tfs_build_main(&ms, hashes, TIP_H);

        /* A data-bearing fork child exists, but best_header == active tip
         * (no more-work header chain). Must stay quiet. */
        struct uint256 stale_h;
        (void)tfs_insert(&ms, &stale_h, 1, TIP_H + 1, tip,
            (uint64_t)(TIP_H + 2),
            BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA);
        ms.pindex_best_header = tip; /* no more-work header */

        condition_engine_set_main_state(&ms);
        register_tip_fork_stale();
        tip_fork_stale_test_set_remedy_stubs(stub_invalidate, stub_rebuild);
        tip_fork_stale_test_force_stall(TIP_H, 400);

        condition_engine_tick();

        bool ok = condition_engine_get_active_count() == 0;
        ok = ok && tip_fork_stale_test_invalidate_calls() == 0;
        TFS_CHECK("no higher-work header chain -> detect false", ok);

        condition_engine_set_main_state(NULL);
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        main_state_free(&ms);
    }

    /* ── 4. Tip not stalled (fresh advance) → detect FALSE even with a
     *      stale fork + higher-work header. Conservative on catch-up. ── */
    {
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        atomic_store(&g_stub_invalidate_calls, 0);

        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[256];
        struct block_index *tip = tfs_build_main(&ms, hashes, TIP_H);

        struct uint256 stale_h;
        (void)tfs_insert(&ms, &stale_h, 1, TIP_H + 1, tip,
            (uint64_t)(TIP_H + 2),
            BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA);
        struct uint256 hdr_h[8];
        struct block_index *prev = tip;
        struct block_index *best_header = NULL;
        for (int i = 1; i <= 5; i++) {
            int h = TIP_H + i;
            prev = tfs_insert(&ms, &hdr_h[i], 2 + i, h, prev,
                              (uint64_t)(TIP_H + 1 + i * 1000),
                              BLOCK_VALID_TREE);
            best_header = prev;
        }
        ms.pindex_best_header = best_header;

        condition_engine_set_main_state(&ms);
        register_tip_fork_stale();
        tip_fork_stale_test_set_remedy_stubs(stub_invalidate, stub_rebuild);
        /* Do NOT force a stall — first tick just records the tip height and
         * arms the timer; the no-advance window is not yet satisfied. */

        condition_engine_tick();

        bool ok = condition_engine_get_active_count() == 0;
        ok = ok && tip_fork_stale_test_invalidate_calls() == 0;
        TFS_CHECK("tip not stalled (no sustained window) -> detect false",
                  ok);

        condition_engine_set_main_state(NULL);
        condition_engine_reset_for_testing();
        tip_fork_stale_test_reset();
        main_state_free(&ms);
    }

    return failures;
}
