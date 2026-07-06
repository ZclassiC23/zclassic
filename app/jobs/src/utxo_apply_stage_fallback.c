/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage_fallback — hash-bound apply candidate recovery.
 *
 * The normal apply path reads active_chain[height]. During bootstrap/refold
 * the visible active-chain window can lag durable reducer rows even though the
 * block map already holds the body. This helper allows exactly that narrow
 * case without trusting height alone. */

#include "utxo_apply_stage_internal.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "script_validate_log_store.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

static bool candidate_extends_visible_parent(struct main_state *ms,
                                             const struct block_index *bi,
                                             int height)
{
    if (!ms || !bi)
        return false;
    if (height == 0)
        return bi->pprev == NULL;

    struct block_index *parent =
        active_chain_at(&ms->chain_active, height - 1);
    if (!parent || !parent->phashBlock || !bi->pprev ||
        !bi->pprev->phashBlock)
        return false;
    return uint256_eq(bi->pprev->phashBlock, parent->phashBlock);
}

static struct block_index *hash_bound_fallback(
        struct main_state *ms, int height,
        const struct script_validate_verdict_row *sv_row)
{
    if (!ms || !sv_row || sv_row->ok != 1 || !sv_row->has_block_hash)
        return NULL;

    struct block_index *bi = block_map_find(&ms->map_block_index,
                                            &sv_row->block_hash);
    if (!bi || bi->nHeight != height || !(bi->nStatus & BLOCK_HAVE_DATA))
        return NULL;
    if (!candidate_extends_visible_parent(ms, bi, height))
        return NULL;
    return bi;
}

struct block_index *utxo_apply_select_apply_block(
        struct main_state *ms, int height,
        const struct script_validate_verdict_row *sv_row)
{
    if (!ms)
        return NULL;

    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (bi && (bi->nStatus & BLOCK_HAVE_DATA))
        return bi;

    atomic_fetch_add(&g_ua_window_miss_total, 1);
    atomic_store(&g_ua_window_miss_height, (int64_t)height);

    struct block_index *fallback = hash_bound_fallback(ms, height, sv_row);
    if (!fallback)
        return NULL;

    atomic_fetch_add(&g_ua_hash_bound_fallback_total, 1);
    atomic_store(&g_ua_hash_bound_fallback_height, (int64_t)height);
    return fallback;
}
