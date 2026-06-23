/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_boot_refold_window_extend — the deterministic regression proof for the
 * boot-loader fix in config/src/boot_refold_staged.c
 * (boot_load_snapshot_at_own_height_reset, commit ab512d577).
 *
 * THE BUG THIS PINS
 * -----------------
 * The -load-snapshot-at-own-height loader consensus-binds a snapshot by looking
 * its seed height up in the active-chain WINDOW (active_chain_at). That window
 * is pinned to coins-best on every boot path. When the snapshot height (seed_h)
 * was ABOVE coins-best, active_chain_at returned NULL and the loader FATAL'd
 * "Run --importblockindex" — but --importblockindex only sets
 * pindex_best_header, it never fills chain[]. No recipe could satisfy the check;
 * a node pinned below a newer, complete snapshot stayed wedged.
 *
 * The fix: when active_chain_at(seed_h) is NULL but
 * pindex_best_header->nHeight >= seed_h, widen the window forward to the
 * PoW-proven header tip with active_chain_extend_window (which walks pprev to
 * fill chain[] and never publishes finalized authority), then re-read. A real
 * pprev gap leaves the slot NULL and the downstream FATAL still fires (fails
 * closed — never bind a coin against a missing/forged anchor).
 *
 * WHY A UNIT SEAM (and not a fork-child loader integration like
 * test_refold_auto_arm)
 * ----------------------------------------------------------------------------
 * The fix is two lines of glue over one primitive: active_chain_extend_window +
 * active_chain_at. The whole-loader path additionally requires a real
 * SHA3-verified snapshot file on disk, an open node.db, and a checkpoint
 * override — none of which exercise the window-extend logic that is the subject
 * of the fix. This test isolates EXACTLY the primitive seam the fix relies on,
 * with cheap in-memory block_index fixtures and zero datadir/snapshot. It
 * asserts BOTH halves of the fix's contract:
 *
 *   (A) RECOVERY  — seed_h above the coins-best window but reachable by walking
 *       pprev from pindex_best_header is INVISIBLE before the extend
 *       (active_chain_at == NULL, the exact pre-fix FATAL trigger) and VISIBLE
 *       (the correct, hash-matching block_index) after it. This is the case the
 *       fix unwedges.
 *
 *   (B) FAILS CLOSED — when a genuine pprev gap sits between the coins-best
 *       window and the header tip, the extend cannot bridge it: the slot at
 *       seed_h stays NULL after the extend, so the loader's FATAL still fires
 *       (the fix never binds against a missing anchor).
 *
 * No FATAL path runs in-process here (the fix's _exit is downstream of the NULL
 * the (B) case asserts), so this test needs no fork.
 */

#include "test/test_helpers.h"

#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "core/uint256.h"

#include <stdio.h>
#include <string.h>

#define BRWE_CHECK(name, expr) do {                       \
    printf("  boot_refold_window_extend: %s... ", (name)); \
    if (expr) printf("OK\n");                              \
    else { printf("FAIL\n"); failures++; }                 \
} while (0)

/* Deterministic per-height block hash (distinct, non-null). The high byte is a
 * fixed tag so two different heights never collide and no hash is all-zero
 * (chainstate_insert_block_index rejects a null hash). */
static void brwe_hash_for(int h, struct uint256 *out)
{
    memset(out->data, 0, 32);
    out->data[0] = (uint8_t)(h & 0xFF);
    out->data[1] = (uint8_t)((h >> 8) & 0xFF);
    out->data[2] = (uint8_t)((h >> 16) & 0xFF);
    out->data[31] = 0x77;  /* non-null tag */
}

/* Insert a block_index at height h into ms's block map, linked to `prev` via
 * pprev (NULL for the base). Returns the node (NULL only on insert failure).
 * pprev linkage is load-bearing: active_chain_extend_window -> fill_window
 * assembles chain[] by walking pprev down from the candidate. */
static struct block_index *brwe_insert(struct main_state *ms, int h,
                                       struct block_index *prev)
{
    struct uint256 bh;
    brwe_hash_for(h, &bh);
    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, &bh);
    if (!bi)
        return NULL;
    bi->nHeight = h;
    bi->pprev = prev;
    bi->nStatus = BLOCK_VALID_TREE | BLOCK_HAVE_DATA;
    return bi;
}

int test_boot_refold_window_extend(void);
int test_boot_refold_window_extend(void)
{
    test_reset_shared_globals();
    printf("\n=== boot_refold_window_extend tests ===\n");
    int failures = 0;

    /* Heights chosen far apart so coins-best, seed_h, and the header tip are
     * unambiguous. The window is pinned to COINS_BEST; the snapshot seed sits
     * above it; the PoW-proven header tip is higher still. */
    const int COINS_BEST = 100;
    const int SEED_H      = 130;   /* the snapshot height: above the window */
    const int HEADER_TIP  = 150;   /* pindex_best_header: above seed_h */

    /* ── Case (A): RECOVERY — contiguous pprev chain to the header tip. ─────
     * Build a fully linked header chain [0 .. HEADER_TIP], pin the active
     * window to COINS_BEST, point pindex_best_header at the tip. seed_h is then
     * invisible (the pre-fix FATAL) until the extend widens the window. */
    {
        struct main_state ms;
        main_state_init(&ms);

        struct block_index *prev = NULL;
        struct block_index *at_coins_best = NULL;
        struct block_index *at_seed = NULL;
        struct block_index *header_tip = NULL;
        bool insert_ok = true;
        for (int h = 0; h <= HEADER_TIP; h++) {
            struct block_index *bi = brwe_insert(&ms, h, prev);
            if (!bi) { insert_ok = false; break; }
            if (h == COINS_BEST) at_coins_best = bi;
            if (h == SEED_H)     at_seed = bi;
            if (h == HEADER_TIP) header_tip = bi;
            prev = bi;
        }
        BRWE_CHECK("A: header chain [0..tip] built", insert_ok &&
                   at_coins_best && at_seed && header_tip);

        if (insert_ok && at_coins_best && at_seed && header_tip) {
            /* Pin the visible window to coins-best (mirrors the boot restore:
             * utxo_recovery_restore_chain_tip -> csr_commit_tip). */
            BRWE_CHECK("A: window pinned to coins-best",
                       active_chain_install_tip_slot(&ms.chain_active,
                                                     at_coins_best) &&
                       active_chain_height(&ms.chain_active) == COINS_BEST);
            ms.pindex_best_header = header_tip;

            /* PRE-FIX TRIGGER: seed_h is above the window → invisible.
             * This is the exact NULL the old loader FATAL'd on. */
            BRWE_CHECK("A: seed_h invisible before extend (the pre-fix FATAL)",
                       active_chain_at(&ms.chain_active, SEED_H) == NULL);

            /* THE FIX: extend the window forward to the PoW-proven header tip. */
            BRWE_CHECK("A: extend to header tip succeeds",
                       active_chain_extend_window(&ms.chain_active, header_tip));

            /* POST-FIX: seed_h is now visible AND is the correct block_index
             * (the consensus anchor cross-check downstream reads bi->hashBlock,
             * so the IDENTITY of the slot — not just non-NULL — is what makes
             * the fix sound). */
            const struct block_index *bi =
                active_chain_at(&ms.chain_active, SEED_H);
            BRWE_CHECK("A: seed_h VISIBLE after extend", bi != NULL);
            BRWE_CHECK("A: seed_h slot is the RIGHT block (correct anchor hash)",
                       bi == at_seed &&
                       memcmp(bi->hashBlock.data, at_seed->hashBlock.data, 32)
                           == 0);
            /* The window now reaches the header tip; coins-best slot intact. */
            BRWE_CHECK("A: window now spans to the header tip",
                       active_chain_height(&ms.chain_active) == HEADER_TIP);
            BRWE_CHECK("A: coins-best slot still resolves after extend",
                       active_chain_at(&ms.chain_active, COINS_BEST) ==
                           at_coins_best);
        }

        main_state_free(&ms);
    }

    /* ── Case (B): FAILS CLOSED — a genuine pprev gap at seed_h. ────────────
     * Build the chain with a HOLE: the block at SEED_H has NO pprev link to its
     * parent (a missing-ancestor gap, e.g. a header the import never linked).
     * fill_window walks pprev down from the header tip; it cannot reach the
     * SEED_H slot through the gap, so active_chain_at(SEED_H) stays NULL after
     * the extend — and the loader's FATAL still fires (never binds a coin
     * against a missing anchor). */
    {
        struct main_state ms;
        main_state_init(&ms);

        struct block_index *prev = NULL;
        struct block_index *at_coins_best = NULL;
        struct block_index *header_tip = NULL;
        bool insert_ok = true;
        for (int h = 0; h <= HEADER_TIP; h++) {
            /* Break the pprev chain ABOVE seed_h: the block at SEED_H+1 has a
             * NULL pprev, so walking down from the header tip stops there and
             * never reaches SEED_H. (Below the break the chain is intact, so
             * coins-best is undisturbed.) */
            struct block_index *link = (h == SEED_H + 1) ? NULL : prev;
            struct block_index *bi = brwe_insert(&ms, h, link);
            if (!bi) { insert_ok = false; break; }
            if (h == COINS_BEST) at_coins_best = bi;
            if (h == HEADER_TIP) header_tip = bi;
            prev = bi;
        }
        BRWE_CHECK("B: header chain with a pprev gap built", insert_ok &&
                   at_coins_best && header_tip);

        if (insert_ok && at_coins_best && header_tip) {
            BRWE_CHECK("B: window pinned to coins-best",
                       active_chain_install_tip_slot(&ms.chain_active,
                                                     at_coins_best) &&
                       active_chain_height(&ms.chain_active) == COINS_BEST);
            ms.pindex_best_header = header_tip;

            /* Pre-extend: invisible (same as case A). */
            BRWE_CHECK("B: seed_h invisible before extend",
                       active_chain_at(&ms.chain_active, SEED_H) == NULL);

            /* Extend toward the header tip. The primitive succeeds (no alloc
             * failure) but the pprev gap means it cannot fill the SEED_H slot. */
            BRWE_CHECK("B: extend call returns true (no alloc failure)",
                       active_chain_extend_window(&ms.chain_active, header_tip));

            /* THE FAIL-CLOSED ASSERTION: seed_h is STILL NULL after the extend.
             * In the loader, this leaves `bi` NULL and the FATAL fires — the
             * fix never binds a coin against a missing/forged anchor. */
            BRWE_CHECK("B: seed_h STILL NULL after extend (FATAL still fires)",
                       active_chain_at(&ms.chain_active, SEED_H) == NULL);
        }

        main_state_free(&ms);
    }

    return failures;
}
