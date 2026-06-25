/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for active_chain_extend_window_have_data — the anchored, contiguity-
 * proven window extender that lets tip_finalize see its lookahead successor
 * WITHOUT the false-reorg cascade a generic best_header/most-work candidate
 * caused on the live node.
 *
 * The extender must:
 *   - extend the window along the CONTIGUOUS have-data frontier above the
 *     finalized tip, up to max_height (gate is BLOCK_HAVE_DATA, NOT
 *     BLOCK_VALID_SCRIPTS — the body stages must see a have-data block before
 *     it is script-validated; per-stage validity is enforced by each stage);
 *   - REFUSE to cross a gap, a fork (pprev not pointer-equal to the parent),
 *     or a missing-body / header-only block — never exposing a divergent or
 *     bodiless block that would overwrite a finalized slot;
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

    /* 5. Have-data but NOT yet script-validated at 5 -> still EXPOSED. The gate
     * is BLOCK_HAVE_DATA, not BLOCK_VALID_SCRIPTS: the body-dependent stages
     * (body_fetch / body_persist / script_validate) read active_chain_at(their
     * cursor + 1) and MUST see a have-data block before it is script-validated
     * (script validation is exactly what that stage is about to do). Requiring
     * VALID_SCRIPTS here was a chicken-and-egg that wedged the body pipeline.
     * The remaining heights (6,7) retain BLOCK_VALID_SCRIPTS, so with a
     * contiguous have-data frontier the window extends all the way to 7. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3);
        b[5]->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_TREE; /* body, no scripts */
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 7);
        ok = ok && active_chain_at(&ms.chain_active, 4) == b[4];
        ok = ok && active_chain_at(&ms.chain_active, 5) == b[5]; /* exposed */
        ok = ok && active_chain_at(&ms.chain_active, 7) == b[7];
        ACE_CHECK("have-data not-yet-script-validated successor IS exposed", ok);
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

        /* pprev = NULL: this synthetic far-jump block exists only to force
         * an array grow; a non-NULL pprev with a non-adjacent height label
         * would (correctly) trip the validation-pack label-splice check in
         * active_chain_move_window_tip. NULL pprev is scoped out and the
         * fill loop preserves the lower window identically. */
        struct block_index *far = ace_insert(&ms, &far_hashes[0], pre_cap + 100,
                                             NULL,
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
                                              mid_cap + 100, NULL /* see above */,
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

    /* 8. The wrong-fork wedge regression: pindex_best_header points at a
     * BODILESS orphan above the have-data frontier. The fix scans the range UP
     * TO the header tip (a generous upper bound, NOT a per-stage cap), but the
     * have-data extender refuses to fill to a header-only candidate (no
     * BLOCK_HAVE_DATA), so the window must extend ONLY to the contiguous
     * have-data tip and must NOT pin the header-only orphan — even though the
     * scan bound now sits ABOVE the body floor.
     *
     * Bodies/scripts through 5; 6 and 7 are header-only (no BLOCK_HAVE_DATA);
     * max_height = the HEADER tip (7), proving the orphan-exclusion comes from
     * the internal have-data gate, not from an artificially low scan bound. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        bool ok = ace_build(&ms, b, N, 3);
        /* Bodies + scripts only through 5; 6,7 are header-only (no body). */
        b[6]->nStatus = BLOCK_VALID_TREE; /* header-only orphan, no HAVE_DATA */
        b[7]->nStatus = BLOCK_VALID_TREE;
        ms.pindex_best_header = b[7]; /* best HEADER sits above the body floor */

        /* Scan all the way to the header tip (7), exactly as the fixed
         * reducer_extend_window_to_candidate now passes pindex_best_header. */
        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index, 7);
        ok = ok && active_chain_at(&ms.chain_active, 5) == b[5]; /* reached body */
        ok = ok && active_chain_at(&ms.chain_active, 6) == NULL; /* NOT pinned */
        ok = ok && active_chain_at(&ms.chain_active, 7) == NULL;
        ok = ok && active_chain_height(&ms.chain_active) == 5;
        ACE_CHECK("bodiless best-header orphan NOT pinned even with header-tip "
                  "scan bound", ok);
        main_state_free(&ms);
    }

    /* 9. Upstream-lookahead preservation (the bug this rework fixes): the
     * previous bound was utxo_apply's cursor (the LOWEST stage cursor). An
     * upstream stage (body_persist/script_validate/proof_validate) reads
     * active_chain_at(its_cursor + 1); if the window stopped at utxo_apply's
     * cursor, every height above it returned NULL -> JOB_IDLE -> bodies could
     * never lead utxo_apply -> wedge.
     *
     * Model: utxo_apply finalized at the window tip (3); bodies+scripts present
     * and contiguous through 7. With the header-tip scan bound, the window MUST
     * expose heights 4..7 so an upstream stage at cursor=4,5,6 can read its
     * next block. Concretely: active_chain_at(utxo_apply_cursor + k) for k>=1
     * (i.e. the heights the leading stages consume) is non-NULL up to the
     * have-data frontier — the starvation the cursor-bound caused is gone. */
    {
        struct main_state ms; main_state_init(&ms);
        struct block_index *b[8];
        int utxo_apply_cursor = 3;            /* lowest stage; == window tip */
        bool ok = ace_build(&ms, b, N, utxo_apply_cursor);
        ms.pindex_best_header = b[7];         /* header tip = body frontier here */

        active_chain_extend_window_have_data(&ms.chain_active,
                                             &ms.map_block_index,
                                             ms.pindex_best_header->nHeight);

        /* Every height a leading upstream stage would consume (cursor+1 ..
         * have-data tip) is now exposed — NOT NULL as it was under the
         * utxo_apply-cursor cap. */
        for (int h = utxo_apply_cursor + 1; h <= 7; h++)
            ok = ok && active_chain_at(&ms.chain_active, h) == b[h];
        ok = ok && active_chain_height(&ms.chain_active) == 7;
        ACE_CHECK("upstream lookahead preserved (successors above utxo_apply "
                  "cursor exposed)", ok);
        main_state_free(&ms);
    }

    return failures;
}
