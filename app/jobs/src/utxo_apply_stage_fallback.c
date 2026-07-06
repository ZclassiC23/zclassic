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
#include "jobs/tip_finalize_stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdbool.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

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

static bool candidate_extends_durable_parent(sqlite3 *db,
                                             const struct block_index *bi,
                                             int height)
{
    if (!db || !bi)
        return false;
    if (height == 0)
        return bi->pprev == NULL;
    if (!bi->pprev || !bi->pprev->phashBlock)
        return false;

    uint8_t parent_hash[32];
    if (!tip_finalize_stage_block_hash_at(db, height - 1, parent_hash))
        return false;  // raw-return-ok:optional-durable-parent-witness
    return memcmp(parent_hash, bi->pprev->phashBlock->data,
                  sizeof(parent_hash)) == 0;
}

static struct block_index *hash_bound_fallback(
        sqlite3 *db, struct main_state *ms, int height,
        const struct script_validate_verdict_row *sv_row)
{
    if (!ms || !sv_row || sv_row->ok != 1 || !sv_row->has_block_hash)
        return NULL;

    struct block_index *bi = block_map_find(&ms->map_block_index,
                                            &sv_row->block_hash);
    if (!bi || bi->nHeight != height || !(bi->nStatus & BLOCK_HAVE_DATA))
        return NULL;
    if (!candidate_extends_visible_parent(ms, bi, height) &&
        !candidate_extends_durable_parent(db, bi, height))
        return NULL;
    return bi;
}

struct block_index *utxo_apply_select_apply_block(
        sqlite3 *db, struct main_state *ms, int height,
        const struct script_validate_verdict_row *sv_row)
{
    if (!ms)
        return NULL;

    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (bi && (bi->nStatus & BLOCK_HAVE_DATA))
        return bi;

    atomic_fetch_add(&g_ua_window_miss_total, 1);
    atomic_store(&g_ua_window_miss_height, (int64_t)height);

    struct block_index *fallback = hash_bound_fallback(db, ms, height, sv_row);
    if (!fallback)
        return NULL;

    atomic_fetch_add(&g_ua_hash_bound_fallback_total, 1);
    atomic_store(&g_ua_hash_bound_fallback_height, (int64_t)height);
    return fallback;
}
