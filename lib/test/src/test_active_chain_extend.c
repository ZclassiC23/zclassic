/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for active_chain_extend_window_have_data — the anchored, contiguity-
 * proven window extender that lets tip_finalize see its lookahead successor
 * WITHOUT the false-reorg cascade a generic best_header/most-work candidate
 * caused on the live node.
 *
 * The extender must:
 *   - extend the window along the CONTIGUOUS have-data + script-validated
 *     frontier above the finalized tip, up to max_height;
 *   - REFUSE to cross a gap, a fork (pprev not pointer-equal to the parent),
 *     a missing-body, or a not-yet-script-validated block — never exposing a
 *     divergent block that would overwrite a finalized slot;
 *   - be a no-op when there is no gap (max_height <= window height).
 *
 * Chain build mirrors test_tip_fork_stale.c: a minimal in-RAM main_state with
 * blocks inserted into map_block_index and pprev linked to the map-resident
 * entries (so the extender's by-pprev contiguity walk is exercised on real
 * pointers).
 */

#include "test/test_helpers.h"

#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdio.h>
#include <string.h>

#define ACE_CHECK(name, expr) do {                  \
    printf("active_chain_extend: %s... ", (name));  \
    if (expr) printf("OK\n");                         \
    else { printf("FAIL\n"); failures++; }            \
} while (0)

/* Insert a block at height h building on prev, with the given status. */
static struct block_index *ace_insert(struct main_state *ms,
                                      struct uint256 *hash, int h,
                                      struct block_index *prev, unsigned status)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(h & 0xFF);
    hash->data[1] = (uint8_t)((h >> 8) & 0xFF);
    hash->data[2] = 0xAC; /* distinct salt */

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
    arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
    pi->pprev = prev;
    return pi;
}

/* Build a contiguous valid have-data chain [0..n-1] in the map; window tip is
 * left at `tip_h`. Returns block pointers in out[]. */
static bool ace_build(struct main_state *ms, struct block_index **out, int n,
                      int tip_h)
{
    static struct uint256 hashes[64];
    struct block_index *prev = NULL;
    for (int h = 0; h < n; h++) {
        out[h] = ace_insert(ms, &hashes[h], h, prev,
                            BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS);
        if (!out[h]) return false;
        prev = out[h];
    }
    return active_chain_move_window_tip(&ms->chain_active, out[tip_h]);
}

int test_active_chain_extend(void)
{
    printf("\n=== active_chain_extend_window_have_data tests ===\n");
    int failures = 0;
    const int N = 8;

    /* 1. Contiguous frontier: window at 3, bodies through 7 -> extend to 7. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3);
        ok = ok && active_chain_at(&ms.chain_active, 4) == NULL; /* gap pre */
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 7);
        ok = ok && active_chain_at(&ms.chain_active, 4) == b[4];
        ok = ok && active_chain_at(&ms.chain_active, 7) == b[7];
        ok = ok && active_chain_at(&ms.chain_active, 3) == b[3]; /* tip intact */
        ACE_CHECK("contiguous have-data frontier -> window extends to max", ok);
        main_state_free(&ms);
    }

    /* 2. Capped by max_height: extend only to 5 even though 6,7 are present. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3);
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 5);
        ok = ok && active_chain_at(&ms.chain_active, 5) == b[5];
        ok = ok && active_chain_at(&ms.chain_active, 6) == NULL; /* capped */
        ACE_CHECK("max_height caps the extension", ok);
        main_state_free(&ms);
    }

    /* 3. Fork at 5 (pprev points to 3, not 4) -> stop at 4, never expose 5. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3);
        b[5]->pprev = b[3]; /* divergent: not a child of b[4] */
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 7);
        ok = ok && active_chain_at(&ms.chain_active, 4) == b[4];
        ok = ok && active_chain_at(&ms.chain_active, 5) == NULL; /* refused */
        ACE_CHECK("fork/divergent successor refused (stops at contiguous edge)",
                  ok);
        main_state_free(&ms);
    }

    /* 4. Missing body at 5 -> stop at 4. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3);
        b[5]->nStatus = BLOCK_VALID_SCRIPTS; /* no BLOCK_HAVE_DATA */
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 7);
        ok = ok && active_chain_at(&ms.chain_active, 4) == b[4];
        ok = ok && active_chain_at(&ms.chain_active, 5) == NULL;
        ACE_CHECK("missing-body successor refused", ok);
        main_state_free(&ms);
    }

    /* 5. Not script-validated at 5 -> stop at 4 (only VALID_TREE). */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3);
        b[5]->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_TREE;
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 7);
        ok = ok && active_chain_at(&ms.chain_active, 4) == b[4];
        ok = ok && active_chain_at(&ms.chain_active, 5) == NULL;
        ACE_CHECK("not-yet-script-validated successor refused", ok);
        main_state_free(&ms);
    }

    /* 6. No gap (max_height <= window height) -> no-op, window unchanged. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 7); /* window already at 7 */
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 7);
        ok = ok && active_chain_at(&ms.chain_active, 7) == b[7];
        ok = ok && active_chain_height(&ms.chain_active) == 7;
        ACE_CHECK("no gap -> no-op", ok);
        main_state_free(&ms);
    }

    /* 7. Window grow RETIRES (never frees) the superseded chain[] array —
     * regression for the cross-thread grow/realloc UAF: a lock-free reader
     * that loaded the pre-grow array pointer must still be able to index it
     * after the grow. Also pins geometric growth (capacity at least doubles),
     * which bounds the retired set, and that active_chain_free reclaims the
     * retired arrays (leak/double-free caught under sanitizers). */
    {
        static struct uint256 far_hashes[2];
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3); /* capacity = 3 + 1024 */
        struct block_index **pre = ms.chain_active.chain;
        int pre_cap = ms.chain_active.capacity;
        ok = ok && pre != NULL && pre_cap > 0;

        struct block_index *far = ace_insert(&ms, &far_hashes[0], pre_cap + 100,
                                             b[7],
                                             BLOCK_HAVE_DATA |
                                             BLOCK_VALID_SCRIPTS);
        ok = ok && far &&
             active_chain_move_window_tip(&ms.chain_active, far);
        ok = ok && ms.chain_active.chain != pre;          /* grew */
        ok = ok && ms.chain_active.capacity >= 2 * pre_cap; /* geometric */
        ok = ok && ms.chain_active.retired != NULL &&
             ms.chain_active.retired->arr == pre;         /* retired, not freed */
        ok = ok && pre[3] == b[3];      /* old array still live + intact */
        ok = ok && active_chain_at(&ms.chain_active, far->nHeight) == far;
        ok = ok && active_chain_at(&ms.chain_active, 3) == b[3];

        /* Second grow chains a second retired array; the first stays live. */
        struct block_index **mid = ms.chain_active.chain;
        int mid_cap = ms.chain_active.capacity;
        struct block_index *far2 = ace_insert(&ms, &far_hashes[1],
                                              mid_cap + 100, far,
                                              BLOCK_HAVE_DATA |
                                              BLOCK_VALID_SCRIPTS);
        ok = ok && far2 &&
             active_chain_move_window_tip(&ms.chain_active, far2);
        ok = ok && ms.chain_active.retired != NULL &&
             ms.chain_active.retired->arr == mid &&
             ms.chain_active.retired->next != NULL &&
             ms.chain_active.retired->next->arr == pre;
        ok = ok && pre[3] == b[3] && mid[far->nHeight] == far;
        ACE_CHECK("grow retires superseded chain[] (value-stable for readers)",
                  ok);
        main_state_free(&ms); /* frees both retired arrays */
    }

    return failures;
}
