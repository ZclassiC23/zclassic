/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Backfill — one-time historical-chain seed for the
 * block_index_projection durable store. See
 * services/block_index_backfill.h for the contract and rationale.
 *
 * Sequence
 * --------
 *   (a) gate on the durable flag (block_index_projection_backfill_done);
 *   (b) walk active_chain_tip->pprev into a height-ASC vector;
 *   (c) emit each header via block_index_emit_header_event, restoring
 *       nSolution from the legacy LevelDB disk_block_index when the
 *       in-memory entry doesn't retain one;
 *   (d) block_index_projection_catch_up to fold;
 *   (e) set the durable flag.
 *
 * Failure before (e) leaves the flag unset so a later call retries; the
 * fold is idempotent (INSERT OR REPLACE on hash) so a retry is safe.
 */

#include "services/block_index_backfill.h"

#include "jobs/block_header_emit.h"
#include "storage/block_index_db.h"
#include "storage/block_index_projection.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "chain/chain.h"
#include "core/uint256.h"

#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Emit one canonical block's header. If the in-memory entry doesn't
 * retain a solution (the loaders set it NULL to save RAM), read the
 * disk_block_index from the legacy LevelDB by this block's hash and emit
 * with its solution attached. Restores bi to its NULL-solution state
 * afterwards (we never persist a solution into the in-memory index).
 *
 * `emit_ok`/`emit_fail` are forwarded to block_index_emit_header_event so
 * the caller can detect a serialize/append failure. */
static void backfill_emit_one(struct block_index *bi,
                              struct block_tree_db *btdb,
                              uint64_t *solution_disk_reads,
                              _Atomic uint64_t *emit_ok,
                              _Atomic uint64_t *emit_fail)
{
    /* In-memory entry already carries the solution (e.g. a freshly
     * connected tip, or a synthetic test chain) — emit as-is. */
    if (bi->nSolution && bi->nSolutionSize > 0) {
        block_index_emit_header_event(bi, "block_index_backfill",
                                      emit_ok, emit_fail);
        return;
    }

    /* No in-RAM solution — recover it from the legacy LevelDB index. */
    struct disk_block_index dbi;
    if (btdb && bi->phashBlock &&
        block_tree_db_read_block_index(btdb, bi->phashBlock, &dbi) &&
        dbi.nSolutionSize > 0) {
        /* Temporarily attach the disk solution to the in-mem entry so the
         * shared emitter (which sources bi->nSolution) carries it. The
         * dbi buffer outlives the emit call (stack-local, scoped here). */
        unsigned char *saved_sol      = bi->nSolution;
        size_t         saved_sol_size = bi->nSolutionSize;
        bi->nSolution     = dbi.nSolution;
        bi->nSolutionSize = dbi.nSolutionSize;
        block_index_emit_header_event(bi, "block_index_backfill",
                                      emit_ok, emit_fail);
        bi->nSolution     = saved_sol;
        bi->nSolutionSize = saved_sol_size;
        if (solution_disk_reads)
            (*solution_disk_reads)++;
        return;
    }

    /* No solution available anywhere — emit the header with an empty
     * solution. The live path always passes a btdb that carries it; this
     * branch only fires for a malformed/missing LevelDB entry, in which
     * case a partial header is still better than a missing one. */
    block_index_emit_header_event(bi, "block_index_backfill",
                                  emit_ok, emit_fail);
}

struct zcl_result block_index_backfill_canonical_chain(
        struct main_state *ms,
        struct block_index_projection *bip,
        struct block_tree_db *btdb,
        struct block_index_backfill_result *out)
{
    struct block_index_backfill_result r;
    memset(&r, 0, sizeof(r));

    if (!ms || !bip) {
        /* Unwired (cold boot / no projection) — nothing to do, not fatal. */
        if (out) *out = r;
        return ZCL_OK;
    }

    /* (a) Durable one-time gate — flag ONLY, never a row count. */
    if (block_index_projection_backfill_done(bip)) {
        r.already_done = true;
        if (out) *out = r;
        return ZCL_OK;
    }

    /* (b) Walk the canonical active chain tip..genesis via ->pprev. The
     * active chain is the most-work lineage the boot loaders linked; the
     * walk visits only canonical blocks (no stale forks). */
    struct block_index *tip = active_chain_tip(&ms->chain_active);
    if (!tip) {
        /* No active tip (cold datadir at genesis) — nothing to backfill,
         * but the flag is left UNSET so a later boot (after fast_sync or
         * the legacy loaders set a tip) does the real backfill. */
        if (out) *out = r;
        return ZCL_OK;
    }

    /* Count the lineage so we can size a single vector and emit ASC. */
    size_t n = 0;
    for (struct block_index *p = tip; p; p = p->pprev)
        n++;

    struct block_index **chain = (struct block_index **)
        zcl_malloc(n * sizeof(*chain), "block_index_backfill/chain");
    if (!chain) {
        if (out) *out = r;
        return ZCL_ERR(-1, "malloc failed for %zu canonical block pointers", n);
    }

    /* Fill descending (tip..genesis), then index ASC when emitting. */
    size_t idx = 0;
    for (struct block_index *p = tip; p && idx < n; p = p->pprev)
        chain[idx++] = p;
    size_t walked = idx;
    r.walked = walked;

    /* (c) Emit each header in height-ASC order (genesis first). */
    _Atomic uint64_t emit_ok = 0;
    _Atomic uint64_t emit_fail = 0;
    for (size_t i = walked; i-- > 0; ) {
        backfill_emit_one(chain[i], btdb, &r.solution_disk_reads,
                          &emit_ok, &emit_fail);
    }
    free(chain);

    uint64_t failed = atomic_load_explicit(&emit_fail, memory_order_relaxed);
    if (failed != 0) {
        /* A serialize/append failure — do NOT set the flag, so a later
         * call retries (the fold is idempotent). */
        if (out) *out = r;
        return ZCL_ERR(-2,
                "emit failed for %" PRIu64 " of %zu headers; flag left unset",
                failed, walked);
    }
    r.emitted = atomic_load_explicit(&emit_ok, memory_order_relaxed);

    /* (d) Fold the freshly emitted events into the projection. */
    uint64_t off = block_index_projection_catch_up(bip);
    if (off == (uint64_t)-1) {
        if (out) *out = r;
        return ZCL_ERR(-3,
                "projection catch_up failed after emitting %" PRIu64
                " headers", r.emitted);
    }

    /* (e) Mark the durable one-time flag. */
    if (!block_index_projection_mark_backfilled(bip)) {
        if (out) *out = r;
        return ZCL_ERR(-4,
                "mark_backfilled failed after folding %" PRIu64 " headers",
                r.emitted);
    }

    r.ran = true;
    if (out) *out = r;
    return ZCL_OK;
}
