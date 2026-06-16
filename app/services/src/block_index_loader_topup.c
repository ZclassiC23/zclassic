/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
// one-result-type-ok:loader-bool-contract — same bool surface as the
// sibling block_index_loader_rebuild.c loaders; every failure path logs
// its reason via LOG_FAIL before returning.
/*
 * Block Index Loader: projection top-up for the NORMAL boot path.
 *
 * The flat/LevelDB block-index loaders only know what was persisted at
 * the LAST boot-time save.
 * Every block connected after that save mutates its in-memory entry
 * only (reducer ingest / body_persist set HAVE_DATA + nFile/nDataPos
 * + nTx in RAM); the durable record of those mutations is the
 * EV_BLOCK_HEADER event log folded into block_index_projection (WAL,
 * crash-safe). Nothing read it back on a normal boot, so every restart
 * dropped the active chain to the stale flat-file extent and re-chased
 * a window of blocks whose bodies were already on disk.
 *
 * block_index_projection_topup() closes that loop: after the legacy
 * loaders run, fold the projection over the loaded map RAISE-ONLY —
 * apply HAVE_DATA + file positions an entry lacks, raise nTx and the
 * BLOCK_VALID level, insert entries the loaders never saw, and link
 * their pprev. Rows whose recorded height disagrees with the loaded
 * entry's height are refused loudly (label conflicts are surfaced,
 * never merged). FAILED bits are never copied (boot clears them).
 *
 * Legacy rows emitted before body persist learned to stamp nTx carry
 * n_tx=0; for those (HAVE_DATA + valid position + nTx==0) the tx count
 * is recovered from the block file itself, hash-bound to the entry.
 * The caller's existing nChainTx propagation pass (config/src/boot.c)
 * runs immediately after this top-up and turns the recovered nTx into
 * a connected nChainTx chain, which is what the single-pass boot scan
 * and the active-chain rebuild key on. */

#include "services/block_index_loader.h"
#include "chain/chain.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "storage/block_index_db.h"
#include "storage/block_index_projection.h"
#include "storage/disk_block_io.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "jobs/block_header_emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* Hard ceiling on per-boot disk reads for legacy-row nTx recovery. One
 * cold-import window is ~5.5-7k blocks; this is an order of magnitude
 * above that. Hitting it is logged loudly (no silent caps). */
#define TOPUP_NTX_RECOVERY_MAX 50000

struct topup_ctx {
    struct main_state *ms;
    size_t rows;
    size_t inserted;
    size_t data_applied;
    size_t ntx_applied;
    size_t valid_raised;
    size_t height_conflicts;
    /* Inserted entries, collected for the bounded chainwork pass. */
    struct block_index **new_entries;
    size_t new_count;
    size_t new_cap;
    bool failed;
};

static bool topup_track_inserted(struct topup_ctx *c,
                                 struct block_index *bi)
{
    if (c->new_count == c->new_cap) {
        size_t cap = c->new_cap ? c->new_cap * 2 : 256;
        struct block_index **grown = zcl_realloc(
            c->new_entries, cap * sizeof(*grown), "topup.new_entries");
        if (!grown)
            LOG_FAIL("block_index",
                     "topup: realloc failed at %zu inserted entries",
                     c->new_count);
        c->new_entries = grown;
        c->new_cap = cap;
    }
    c->new_entries[c->new_count++] = bi;
    return true;
}

/* Fold one projection row into the map, raise-only. */
static bool topup_row_cb(const uint8_t hash[32],
                         const struct disk_block_index *dbi,
                         void *user)
{
    struct topup_ctx *c = (struct topup_ctx *)user;
    c->rows++;

    struct uint256 h;
    memcpy(h.data, hash, 32);

    struct block_index *bi = block_map_find(&c->ms->map_block_index, &h);
    if (!bi) {
        /* The loaders never saw this block (connected after the last
         * flat save). Insert it with the projection's full record —
         * same field mapping as the -rebuildfromlog fold, no +1703
         * file-0 fixup (the position is this node's own write). */
        bi = chainstate_insert_block_index((struct chainstate *)c->ms, &h);
        if (!bi) {
            c->failed = true;
            return false;
        }
        bi->nHeight              = dbi->nHeight;
        bi->nFile                = dbi->nFile;
        bi->nDataPos             = dbi->nDataPos;
        bi->nUndoPos             = dbi->nUndoPos;
        bi->nVersion             = dbi->nVersion;
        bi->hashMerkleRoot       = dbi->hashMerkleRoot;
        bi->hashFinalSaplingRoot = dbi->hashFinalSaplingRoot;
        bi->nTime                = dbi->nTime;
        bi->nBits                = dbi->nBits;
        bi->nNonce               = dbi->nNonce;
        bi->nSolution            = NULL;  /* not retained in RAM */
        bi->nSolutionSize        = 0;
        bi->nStatus              = dbi->nStatus & ~BLOCK_FAILED_MASK;
        bi->nCachedBranchId      = dbi->nCachedBranchId;
        bi->nTx                  = dbi->nTx;
        if (dbi->has_sprout_value) {
            bi->nSproutValue     = dbi->nSproutValue;
            bi->has_sprout_value = true;
        }
        bi->nSaplingValue        = dbi->nSaplingValue;
        c->inserted++;
        if (!topup_track_inserted(c, bi)) {
            c->failed = true;
            return false;
        }
        return true;
    }

    /* Same hash at a different recorded height: a label conflict. The
     * loaded entry wins; surface the disagreement, never merge it. */
    if (bi->nHeight != dbi->nHeight) {
        if (c->height_conflicts < 3)
            LOG_WARN("block_index",
                     "topup: projection row height %d != loaded entry "
                     "height %d for the same hash — refusing merge",
                     dbi->nHeight, bi->nHeight);
        c->height_conflicts++;
        return true;
    }

    /* Data availability: apply HAVE_DATA + positions the entry lacks. */
    if ((dbi->nStatus & BLOCK_HAVE_DATA) &&
        !(bi->nStatus & BLOCK_HAVE_DATA) && dbi->nFile >= 0) {
        bi->nFile = dbi->nFile;
        bi->nDataPos = dbi->nDataPos;
        bi->nStatus |= BLOCK_HAVE_DATA;
        if (dbi->nStatus & BLOCK_HAVE_UNDO) {
            bi->nUndoPos = dbi->nUndoPos;
            bi->nStatus |= BLOCK_HAVE_UNDO;
        }
        c->data_applied++;
    }

    /* nTx: raise from zero only. */
    if (bi->nTx == 0 && dbi->nTx > 0) {
        bi->nTx = dbi->nTx;
        c->ntx_applied++;
    }

    /* Validity level: the projection may carry a HIGHER validated level
     * (script_validate re-emits after raising it). Raise-only; FAILED
     * bits are never copied. */
    {
        unsigned int cur_lvl = bi->nStatus & BLOCK_VALID_MASK;
        unsigned int row_lvl = dbi->nStatus & BLOCK_VALID_MASK;
        if (row_lvl > cur_lvl) {
            bi->nStatus = (bi->nStatus & ~BLOCK_VALID_MASK) | row_lvl;
            c->valid_raised++;
        }
    }

    return true;
}

/* Second pass: link pprev for entries this top-up inserted. Re-iterates
 * the projection (hashPrev is not retained on the in-memory entry);
 * pre-existing entries keep their loader-resolved pprev. */
static bool topup_link_pprev_cb(const uint8_t hash[32],
                                const struct disk_block_index *dbi,
                                void *user)
{
    struct main_state *ms = (struct main_state *)user;

    if (uint256_is_null(&dbi->hashPrev))
        return true;  /* genesis / no parent */

    struct uint256 h;
    memcpy(h.data, hash, 32);
    struct block_index *bi = block_map_find(&ms->map_block_index, &h);
    if (!bi || bi->pprev)
        return true;

    struct block_index *pprev = block_map_find(&ms->map_block_index,
                                               &dbi->hashPrev);
    if (pprev)
        bi->pprev = pprev;
    return true;
}

static int topup_cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1; // raw-return-ok:qsort-comparator
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

/* Bounded chainwork pass over JUST the inserted entries (height ASC, so
 * an inserted parent is computed before its inserted child). Pre-existing
 * entries already carry loader-computed work; the global boot passes are
 * not re-run here. */
static void topup_compute_inserted_chainwork(struct topup_ctx *c)
{
    if (c->new_count == 0)
        return;
    qsort(c->new_entries, c->new_count, sizeof(*c->new_entries),
          topup_cmp_height);
    for (size_t i = 0; i < c->new_count; i++) {
        struct block_index *bi = c->new_entries[i];
        struct arith_uint256 proof = GetBlockProof(bi);
        if (bi->pprev)
            arith_uint256_add(&bi->nChainWork, &bi->pprev->nChainWork,
                              &proof);
        else
            bi->nChainWork = proof;
        block_index_build_skip(bi);
    }
}

/* Legacy-row nTx recovery: entries with a verified body on disk but
 * nTx==0 (their EV_BLOCK_HEADER was emitted before body persist learned
 * to stamp nTx) break the caller's nChainTx propagation right at the
 * connected window. Read each such block back, hash-bind it to the
 * entry, and recover the tx count from the body itself. */
static void topup_recover_ntx_from_disk(struct main_state *ms,
                                        const char *datadir,
                                        size_t *recovered_out,
                                        size_t *unreadable_out,
                                        size_t *capped_out)
{
    size_t recovered = 0, unreadable = 0, capped = 0;
    size_t iter = 0;
    struct block_index *bi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (!bi || bi->nTx != 0 || bi->nHeight <= 0)
            continue;
        if (!(bi->nStatus & BLOCK_HAVE_DATA) ||
            bi->nFile < 0 || bi->nDataPos == 0 || !bi->phashBlock)
            continue;
        if (recovered + unreadable >= TOPUP_NTX_RECOVERY_MAX) {
            capped++;
            continue;
        }
        struct block blk;
        block_init(&blk);
        if (read_block_from_disk_index_pread(&blk, bi, datadir)) {
            struct uint256 got;
            block_get_hash(&blk, &got);
            if (uint256_eq(&got, bi->phashBlock) && blk.num_vtx > 0) {
                bi->nTx = (unsigned int)blk.num_vtx;
                recovered++;
                /* Re-emit so the projection row self-corrects: the next
                 * boot's catch_up folds the real nTx and this disk read
                 * is never paid again for this block. */
                block_index_emit_header_event(bi, "topup_ntx_recovery",
                                              NULL, NULL);
            } else {
                unreadable++;  /* wrong block at the position — leave 0 */
            }
        } else {
            unreadable++;
        }
        block_free(&blk);
    }
    *recovered_out = recovered;
    *unreadable_out = unreadable;
    *capped_out = capped;
}

bool block_index_projection_topup_with(struct block_index_projection *bip,
                                       struct main_state *ms,
                                       const char *datadir)
{
    if (!ms)
        LOG_FAIL("block_index", "topup: null main_state");
    if (!bip)
        return true;  /* no projection on this datadir — nothing to fold */

    /* Drain any events the prior process lifetime appended but never
     * consumed (live emits queue in the log; the projection only folds
     * at catch_up). Idempotent. */
    if (block_index_projection_catch_up(bip) == (uint64_t)-1)
        LOG_FAIL("block_index", "topup: projection catch_up failed");

    struct topup_ctx ctx = { .ms = ms };
    if (block_index_projection_iterate(bip, topup_row_cb, &ctx) != 0 ||
        ctx.failed) {
        free(ctx.new_entries);
        LOG_FAIL("block_index", "topup: projection iterate failed after "
                 "%zu rows", ctx.rows);
    }

    if (ctx.inserted > 0) {
        if (block_index_projection_iterate(bip, topup_link_pprev_cb, ms)
                != 0) {
            free(ctx.new_entries);
            LOG_FAIL("block_index", "topup: pprev link iterate failed");
        }
        topup_compute_inserted_chainwork(&ctx);
    }
    free(ctx.new_entries);

    size_t ntx_recovered = 0, ntx_unreadable = 0, ntx_capped = 0;
    if (datadir && datadir[0])
        topup_recover_ntx_from_disk(ms, datadir, &ntx_recovered,
                                    &ntx_unreadable, &ntx_capped);

    if (ctx.inserted || ctx.data_applied || ctx.ntx_applied ||
        ctx.valid_raised || ctx.height_conflicts || ntx_recovered ||
        ntx_unreadable || ntx_capped) {
        printf("[boot] block index projection top-up: rows=%zu "
               "inserted=%zu data_applied=%zu ntx_applied=%zu "
               "valid_raised=%zu ntx_recovered_from_disk=%zu "
               "ntx_unreadable=%zu height_conflicts=%zu\n",
               ctx.rows, ctx.inserted, ctx.data_applied, ctx.ntx_applied,
               ctx.valid_raised, ntx_recovered, ntx_unreadable,
               ctx.height_conflicts);
        if (ntx_capped > 0)
            LOG_WARN("block_index",
                     "topup: nTx disk recovery CAPPED at %d reads — "
                     "%zu entries left unrecovered this boot",
                     TOPUP_NTX_RECOVERY_MAX, ntx_capped);
    }
    return true;
}

bool block_index_projection_topup(struct main_state *ms, const char *datadir)
{
    return block_index_projection_topup_with(
        block_index_projection_singleton(), ms, datadir);
}
