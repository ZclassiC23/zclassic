/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Loader: projection-backed boot rebuild.
 *
 * load_block_index_from_projection() reconstructs the in-memory block
 * index map purely from the log-derived block_index_projection, then
 * seeds the active tip from the tip_finalize cursor in progress.kv. This
 * restores the active tip from the tip_finalize cursor in progress.kv.
 *
 * This file owns projection-backed rebuild. The shared height-sorted forward
 * pass (block_index_forward_pass) lives in block_index_loader.c and is
 * declared in services/block_index_loader.h. */

#include "platform/time_compat.h"
#include "services/block_index_loader.h"
#include "services/chain_tip.h"
#include "chain/chain.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "storage/block_index_db.h"
#include "storage/block_index_projection.h"
#include "storage/progress_store.h"
#include "event/event.h"
#include "chain/chainparams.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_helpers.h"
#include "core/uint256.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log_macros.h"
#include "util/safe_alloc.h"

static int rebuild_cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1; // raw-return-ok:qsort-comparator
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

/* Per-row callback context for the projection fold. */
struct projection_fold_ctx {
    struct main_state *ms;
    size_t folded;
    bool   failed;
};

/* Fold one disk_block_index row into the in-memory map. Copies the same
 * scalar fields block_index_db.c maps, OMITTING the +1703 file-0 fixup
 * (the projection's nDataPos is this node's own body_persist position,
 * not a zclassicd-LDB position). pprev is linked in a second pass below
 * (the iterate order is height ASC, but a sibling/orphan can precede its
 * parent at the same height, so we must resolve pprev after all rows
 * are inserted — exactly as the flat/LevelDB loaders do). */
static bool projection_fold_cb(const uint8_t hash[32],
                               const struct disk_block_index *dbi,
                               void *user)
{
    struct projection_fold_ctx *c = (struct projection_fold_ctx *)user;

    struct uint256 h;
    memcpy(h.data, hash, 32);

    struct block_index *pindex = chainstate_insert_block_index(
        (struct chainstate *)c->ms, &h);
    if (!pindex) {
        c->failed = true;
        return false;  /* stop iteration on OOM */
    }

    pindex->nHeight              = dbi->nHeight;
    pindex->nFile                = dbi->nFile;
    pindex->nDataPos             = dbi->nDataPos;   /* no +1703 fixup */
    pindex->nUndoPos             = dbi->nUndoPos;
    pindex->nVersion             = dbi->nVersion;
    pindex->hashMerkleRoot       = dbi->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = dbi->hashFinalSaplingRoot;
    pindex->nTime                = dbi->nTime;
    pindex->nBits                = dbi->nBits;
    pindex->nNonce               = dbi->nNonce;
    pindex->nSolution            = NULL;  /* not retained in RAM */
    pindex->nSolutionSize        = 0;
    pindex->nStatus              = dbi->nStatus;
    pindex->nCachedBranchId      = dbi->nCachedBranchId;
    pindex->nTx                  = dbi->nTx;
    if (dbi->has_sprout_value) {
        pindex->nSproutValue     = dbi->nSproutValue;
        pindex->has_sprout_value = true;
    }
    pindex->nSaplingValue        = dbi->nSaplingValue;

    c->folded++;
    return true;
}

/* Second-pass callback: link each in-memory entry's pprev via the
 * disk_block_index.hashPrev carried by the projection. Genesis (and any
 * row whose hashPrev is all-zero) keeps pprev == NULL. */
static bool projection_link_pprev_cb(const uint8_t hash[32],
                                     const struct disk_block_index *dbi,
                                     void *user)
{
    struct main_state *ms = (struct main_state *)user;

    if (uint256_is_null(&dbi->hashPrev))
        return true;  /* genesis / no parent */

    struct uint256 h;
    memcpy(h.data, hash, 32);
    struct block_index *pindex = block_map_find(&ms->map_block_index, &h);
    if (!pindex)
        return true;

    struct block_index *pprev = block_map_find(&ms->map_block_index,
                                               &dbi->hashPrev);
    if (pprev)
        pindex->pprev = pprev;
    return true;
}

/* Seed the active tip from the durable tip_finalize cursor. The cursor
 * counts finalized heights; the tip is at cursor-1. NULL `progress_db`
 * skips the seed (map rebuilt, no tip published). */
static void rebuild_seed_tip(struct main_state *ms, sqlite3 *progress_db)
{
    if (!progress_db)
        return;

    uint64_t cursor = stage_cursor_persisted(progress_db, "tip_finalize",
                                              "block_index_loader");
    if (cursor == 0)
        return;

    int tip_height = (int)cursor - 1;
    uint8_t tip_hash[32];
    if (!tip_finalize_stage_finalized_tip_at(progress_db, tip_height,
                                             tip_hash)) {
        LOG_WARN("block_index",
                 "load_block_index_from_projection: no finalized tip hash at "
                 "h=%d (cursor=%llu)",
                 tip_height, (unsigned long long)cursor);
        return;
    }

    struct uint256 th;
    memcpy(th.data, tip_hash, 32);
    struct block_index *tip = block_map_find(&ms->map_block_index, &th);
    if (!tip) {
        LOG_WARN("block_index",
                 "load_block_index_from_projection: tip hash at h=%d "
                 "(cursor=%llu) not found in folded map",
                 tip_height, (unsigned long long)cursor);
        return;
    }

    tip_finalize_stage_set_authoritative_tip(tip_height, tip_hash);
    struct zcl_result r = chain_set_active_tip(ms, tip, TIP_FROM_RESTORE,
                                               "loader_from_projection");
    if (!r.ok)
        LOG_WARN("block_index",
                 "load_block_index_from_projection: chain_set_active_tip "
                 "failed at h=%d: %s",
                 tip_height, r.message);
}

/* Max forward gap this seed will adopt in one shot. The finalized frontier
 * is normally a handful of blocks ahead of the established tip; a larger gap
 * means the current active chain is not yet at the coins/UTXO frontier (the
 * full-restore loaders' job), so we no-op instead of walking the whole chain. */
#define BLOCK_INDEX_LOADER_SEED_MAX_GAP 50000

/* Public, FORWARD-ONLY finalized-tip seed for the NORMAL boot path.
 *
 * rebuild_seed_tip() (above) directly publishes the tip on the
 * -rebuildfromlog restore, where the in-memory map IS the full projection
 * and there is no prior tip to extend. This variant is for the normal boot
 * path, where an active tip has ALREADY been established from the coins
 * authority. It adopts the durable finalized frontier (tip_finalize
 * cursor-1) ONLY when it is a strictly-higher, CONTIGUOUS forward extension
 * of the current chain — every intermediate block HAVE_DATA + script-valid +
 * failure-free, with the pprev walk landing pointer-equal on the current
 * active tip. Otherwise it is a no-op.
 *
 * Safety (this runs on the live consensus-boot path): it NEVER rewinds the
 * tip (strictly-higher guard), NEVER swaps a fork (the walk must land on the
 * current tip), and NEVER mutates the tip_finalize_log or any cursor (read
 * only). A sparse/header-only frontier yields a no-op, not a hole — so it
 * cannot reproduce the reverted best-header pre-extend churn.
 *
 * Returns 1 = seeded forward, 0 = no-op, -1 = error. */
int block_index_loader_seed_tip_from_finalized(struct main_state *ms,
                                               sqlite3 *progress_db)
{
    if (!ms)
        LOG_ERR("block_index",
                "seed_tip_from_finalized: null main_state");
    if (!progress_db)
        return 0;

    uint64_t cursor = stage_cursor_persisted(progress_db, "tip_finalize",
                                             "block_index_loader");
    if (cursor == 0)
        return 0;

    int tip_height = (int)cursor - 1;
    int cur_h = active_chain_height(&ms->chain_active);

    /* (a) Forward-only: never rewind or sidestep the current tip. */
    if (tip_height <= cur_h)
        return 0;

    /* (a2) Bounded: this is for a SMALL forward extension (the durable
     * finalized frontier is typically a handful of blocks ahead of the
     * established tip). A large gap means the current tip is not the
     * coins/UTXO frontier yet (e.g. a cold/genesis active chain) — that is
     * the full-restore loaders' job, not this seed. Refuse rather than walk
     * millions of pprev links on the boot/liveness path. */
    if ((int64_t)tip_height - (int64_t)cur_h > BLOCK_INDEX_LOADER_SEED_MAX_GAP)
        return 0;

    struct block_index *cur_tip = active_chain_tip(&ms->chain_active);
    if (!cur_tip)
        return 0;  /* no current tip to extend from — leave it to the loaders */

    uint8_t tip_hash[32];
    if (!tip_finalize_stage_finalized_tip_at(progress_db, tip_height, tip_hash))
        return 0;

    struct uint256 th;
    memcpy(th.data, tip_hash, 32);
    struct block_index *tip = block_map_find(&ms->map_block_index, &th);
    if (!tip)
        return 0;

    /* (b) Contiguity guard: walk pprev from the finalized block down to the
     * current tip's height. Every intermediate block must be a have-data,
     * script-valid, failure-free link (block_index_is_valid already rejects
     * any failure), and the walk MUST land pointer-equal on the current
     * active tip — a pure forward extension of the live chain, never a fork. */
    struct block_index *node = tip;
    for (int h = tip_height; h > cur_h; h--) {
        if (!node || node->nHeight != h)
            return 0;
        if (!(node->nStatus & BLOCK_HAVE_DATA))
            return 0;
        if (!block_index_is_valid(node, BLOCK_VALID_SCRIPTS))
            return 0;
        node = node->pprev;
    }
    if (node != cur_tip)
        return 0;  /* not a contiguous extension of the current chain */

    /* (c) Safe: adopt the durable finalized tip forward-only. */
    tip_finalize_stage_set_authoritative_tip(tip_height, tip_hash);
    struct zcl_result r = chain_set_active_tip(ms, tip, TIP_FROM_RESTORE,
                                               "loader_seed_from_finalized");
    if (!r.ok)
        LOG_RETURN(-1, "block_index",
                   "seed_tip_from_finalized: chain_set_active_tip failed at "
                   "h=%d: %s", tip_height, r.message);

    printf("[boot] active tip seeded forward from durable finalized cursor: "
           "h=%d (was %d)\n", tip_height, cur_h);
    return 1;
}

bool load_block_index_from_projection(struct main_state *ms,
                                      const struct chain_params *params,
                                      struct block_index_projection *bip,
                                      struct sqlite3 *progress_db)
{
    if (!ms)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: null main_state");

    /* Cold / unwired: empty map, no tip. The caller (boot) seeds genesis
     * or fast_sync separately. */
    if (!bip)
        return true;

    /* (1) Drain the event log into the projection. */
    uint64_t off = block_index_projection_catch_up(bip);
    if (off == (uint64_t)-1)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: projection catch_up failed");

    /* (2) Fold every projection row into the in-memory map. */
    struct projection_fold_ctx ctx = { .ms = ms, .folded = 0, .failed = false };
    int64_t t0 = (int64_t)platform_time_wall_time_t();
    if (block_index_projection_iterate(bip, projection_fold_cb, &ctx) != 0 ||
        ctx.failed)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: fold failed after %zu rows",
                 ctx.folded);

    if (ctx.folded == 0) {
        /* Empty projection — cold datadir. Genesis/fast_sync seeds later. */
        printf("Block index projection: empty — no entries folded\n");
        return true;
    }

    /* Option A: re-seed every node's per-node hash storage and point
     * phashBlock at it (never into the reallocatable bucket array). The
     * projection-fold inserts go through chainstate_insert_block_index
     * which already does this; this pass re-asserts it idempotently. */
    {
        size_t iter = 0;
        struct block_index *pi;
        const struct uint256 *hash;
        while (block_map_next(&ms->map_block_index, &iter, &hash, &pi)) {
            if (pi && hash) {
                pi->hashBlock = *hash;
                pi->phashBlock = &pi->hashBlock;
            }
        }
    }

    /* (2b) Ensure genesis exists in the map so block 1's pprev links.
     * The projection persists blocks 1..tip but NOT genesis (genesis is
     * canonically initialized later, at config/src/boot.c's
     * "Ensure genesis block is always properly initialized" block, which
     * runs AFTER this rebuild on the kill-9 fallback path). Without genesis
     * in the map, projection_link_pprev_cb leaves block 1's pprev NULL and
     * the forward-only finalized-tip seed's contiguity walk falls off the
     * bottom (NOT_CONTIGUOUS). Insert a BARE genesis node here — height 0,
     * nStatus untouched (no BLOCK_HAVE_DATA) — so it only carries the pprev
     * link; boot.c's genesis-ensure block still performs the full canonical
     * init (HAVE_DATA / nTx / nChainTx / validity / chainwork) because it
     * only does so when BLOCK_HAVE_DATA is NOT already set. No-op when
     * genesis is already present (e.g. the -rebuildfromlog path). */
    if (params && !block_map_find(&ms->map_block_index,
                                  &params->consensus.hashGenesisBlock)) {
        struct block_index *g = chainstate_insert_block_index(
            (struct chainstate *)ms, &params->consensus.hashGenesisBlock);
        if (g)
            g->nHeight = 0;
    }

    /* (3) Link pprev via the carried hashPrev. Re-iterate the projection
     * (one ORDER BY scan) — hashPrev is not retained on the in-memory
     * entry. Resolving after all rows are inserted handles same-height
     * siblings/orphans correctly, exactly as the flat/LevelDB loaders. */
    if (block_index_projection_iterate(bip, projection_link_pprev_cb, ms) != 0)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: pprev link iterate failed");

    /* (4) Forward pass: nChainWork, nChainTx, skip links, branch id,
     * failed-child propagation — identical to load_block_index post-load. */
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(
        count * sizeof(struct block_index *), "projection sorted");
    if (!sorted)
        LOG_FAIL("block_index",
                 "load_block_index_from_projection: malloc failed for %zu entries",
                 count);
    size_t idx = 0, iter = 0;
    struct block_index *pi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
        if (pi && idx < count)
            sorted[idx++] = pi;
    }
    count = idx;
    qsort(sorted, count, sizeof(struct block_index *), rebuild_cmp_height);
    block_index_forward_pass(sorted, count);
    free(sorted);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("Block index projection: folded %zu entries in %llds\n",
           ctx.folded, (long long)elapsed);

    /* (5) Seed the tip from the durable tip_finalize cursor. */
    rebuild_seed_tip(ms, progress_db);

    return true;
}

/* Shared projection-rebuild front door for boot. Folds the durable
 * block_index_projection into the in-memory map and ACCEPTS only when the
 * folded map has > `min_entries` nodes (re-checked on the actual map size,
 * NOT the bool return: load_block_index_from_projection returns true even
 * when it folds zero rows from a cold datadir). On accept it logs + emits
 * EV_BOOT_BLOCK_INDEX and returns true; otherwise false so boot falls through
 * unchanged.
 *
 * `publish_tip` gates the cursor-driven tip publish inside the rebuild:
 *   - true  → the projection IS the authority (the -rebuildfromlog path);
 *             the tip is published from the tip_finalize cursor.
 *   - false → PURE MAP REBUILD, no tip published (the kill-9 fallback). The
 *             coins/UTXO authority then owns the active tip and the guarded
 *             block_index_loader_seed_tip_from_finalized advances it forward.
 *             This is the load-bearing safety distinction: publishing an
 *             unguarded cursor tip here would short-circuit coins-restore and
 *             genesis-init. */
bool boot_try_rebuild_block_index_from_projection(struct main_state *ms,
                                                  const struct chain_params *params,
                                                  size_t min_entries,
                                                  bool publish_tip)
{
    if (!ms)
        return false;
    struct block_index_projection *bip = block_index_projection_singleton();
    if (!bip)
        return false;
    if (!load_block_index_from_projection(
            ms, params, bip, publish_tip ? progress_store_db() : NULL))
        return false;
    if (ms->map_block_index.size <= min_entries)
        return false;
    if (publish_tip && !active_chain_tip(&ms->chain_active))
        return false;
    printf("[boot] block index rebuilt from projection: %zu entries "
           "(publish_tip=%d)\n", ms->map_block_index.size, (int)publish_tip);
    event_emitf(EV_BOOT_BLOCK_INDEX, 0, "rebuilt_from_projection entries=%zu",
                ms->map_block_index.size);
    return true;
}
