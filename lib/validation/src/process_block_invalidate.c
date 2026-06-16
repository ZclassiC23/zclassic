/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * process_block_invalidate — see header for the consensus-safety
 * contract. Mirrors Bitcoin Core's InvalidateBlock / ReconsiderBlock.
 *
 * Mechanism reuse (no reinvention of reducer reorg machinery):
 *   - mark failed         : process_block_propagate_failed_child (the
 *                           same descendant-CHILD walk used by chain advance).
 *   - roll the chain back : active-chain cursor move + reducer kick.
 *   - reconnect best chain: reducer stage drain via the activation controller
 *                           and find_most_work_chain (which already skips
 *                           BLOCK_FAILED entries).
 *   - persist status flips: block_tree_db_write_block_index against the shared
 *                           block-index LevelDB handle.
 */

#include "validation/process_block_invalidate.h"

#include "platform/time_compat.h"
#include "chain/chain.h"
#include "core/uint256.h"
/* The operator lever's reorg kick + disconnect context (coins_tip/params/
 * datadir) flow through the activation controller — the single owner of
 * the connect mutex. The nStatus mutation + LevelDB persistence belong to
 * validation, so this orchestrator necessarily reaches up to the same
 * app-layer controller process_block_revalidate.c uses. Same tradeoff,
 * same marker — keep the lib_layering baseline flat. */
#include "services/chain_activation_service.h"  // lib-layer-ok:invalidate-lever
#include "storage/block_index_db.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"

#include "process_block_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The block_tree_db handle is owned by config/src/boot.c and exposed as
 * a process-wide pointer for validation. We reuse the same handle here so
 * flipped status updates land in the same LevelDB the rest of the validation
 * path persists to. Shared with process_block_revalidate.c. */
extern struct block_tree_db *g_active_block_tree;

const char *invalidate_result_name(enum invalidate_result r)
{
    switch (r) {
        case INVALIDATE_NOT_ATTEMPTED:   return "not_attempted";
        case INVALIDATE_BLOCK_NOT_FOUND: return "block_not_found";
        case INVALIDATE_IS_GENESIS:      return "is_genesis";
        case INVALIDATE_DISCONNECT_FAIL: return "disconnect_failed";
        case INVALIDATE_PERSIST_FAILED:  return "persist_failed";
        case INVALIDATE_OK:              return "ok";
    }
    return "?";
}

const char *reconsider_result_name(enum reconsider_result r)
{
    switch (r) {
        case RECONSIDER_NOT_ATTEMPTED:   return "not_attempted";
        case RECONSIDER_BLOCK_NOT_FOUND: return "block_not_found";
        case RECONSIDER_NO_FAILURE:      return "no_failure";
        case RECONSIDER_PERSIST_FAILED:  return "persist_failed";
        case RECONSIDER_OK:              return "ok";
    }
    return "?";
}

/* Persist a single pindex's current nStatus to the shared block_tree_db.
 * Returns true on success (or when there is no db handle — tests run
 * without one and rely on the in-memory flip). */
static bool invalidate_persist_pindex(const struct block_index *p)
{
    if (!g_active_block_tree)
        return true;
    struct disk_block_index dbi;
    block_index_snapshot_for_persist(&dbi, p);
    return block_tree_db_write_block_index(g_active_block_tree, &dbi);
}

/* ── Pure core ───────────────────────────────────────────────────── */

void process_block_mark_invalid(struct main_state *ms,
                                struct block_index *target,
                                size_t *marked_children_out)
{
    if (marked_children_out) *marked_children_out = 0;
    if (!ms || !target) return;

    /* Mark the target itself BLOCK_FAILED_VALID. We do NOT lower the
     * validity level (BLOCK_VALID_MASK) — mirroring Bitcoin Core's
     * InvalidateBlock, which only ORs in the FAILED bit. find_most_work_chain
     * skips on block_has_any_failure() before it checks the validity
     * level, so the FAILED bit alone gates selection; leaving the validity
     * bits intact means reconsiderblock (clearing only the FAILED bit)
     * restores the block to fully-selectable without having to recompute
     * how far validation had previously progressed. */
    target->nStatus |= BLOCK_FAILED_VALID;

    /* Propagate BLOCK_FAILED_CHILD to every descendant. Reuse the same
     * height-sorted single-pass walk the chain-advance failure path uses.
     * last_propagate_sec=NULL → unconditional (no rate limit):
     * an operator-issued invalidate must take effect immediately. */
    size_t propagated = 0;
    enum propagate_failed_child_result pr =
        process_block_propagate_failed_child(&ms->map_block_index, target,
                                             platform_time_wall_time_t(),
                                             NULL, &propagated);
    if (pr != PROPAGATE_FAILED_CHILD_OK &&
        pr != PROPAGATE_FAILED_CHILD_SKIP_PARENT_FAILED) {
        /* MALLOC_FAILED: descendant CHILD marks could not be set. The
         * target itself is still marked FAILED_VALID, which is enough
         * for find_most_work_chain to skip it; descendants are skipped
         * transitively via the ancestry walk in find_most_work_chain. */
        LOG_WARN("validation",
                 "invalidate: FAILED_CHILD propagation incomplete "
                 "(result=%d) for h=%d; target still marked FAILED_VALID",
                 (int)pr, target->nHeight);
    }
    if (marked_children_out) *marked_children_out = propagated;
}

void process_block_clear_invalid(struct main_state *ms,
                                 struct block_index *target,
                                 size_t *cleared_out)
{
    if (cleared_out) *cleared_out = 0;
    if (!ms || !target) return;

    size_t cleared = 0;

    /* Clear the target's own failure mark. */
    if (target->nStatus & BLOCK_FAILED_ANY_MASK) {
        target->nStatus &= ~(unsigned)BLOCK_FAILED_ANY_MASK;
        cleared++;
    }

    /* Clear failure marks on every descendant of the target. A block is
     * a descendant if its pprev chain reaches target. We bound the walk
     * per entry to avoid amplification on corrupt pprev rings. */
    size_t iter = 0;
    struct block_index *p = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p || p == target) continue;
        if (!(p->nStatus & BLOCK_FAILED_ANY_MASK)) continue;
        if (p->nHeight <= target->nHeight) continue;
        /* Walk ancestry toward genesis; if we pass through target this
         * entry is a descendant. Bounded + monotonic to be ring-safe. */
        bool is_descendant = false;
        struct block_index *w = p->pprev;
        int last_h = p->nHeight;
        int steps = 0;
        while (w && w->nHeight >= target->nHeight && steps++ < 200000) {
            if (w->nHeight >= last_h) break; /* corrupt: not monotonic */
            last_h = w->nHeight;
            if (w == target) { is_descendant = true; break; }
            w = w->pprev;
        }
        if (is_descendant) {
            p->nStatus &= ~(unsigned)BLOCK_FAILED_ANY_MASK;
            cleared++;
        }
    }

    if (cleared_out) *cleared_out = cleared;
}

bool process_block_disconnect_to_parent(struct validation_state *state,
                                         struct main_state *ms,
                                         struct coins_view_cache *coins_tip,
                                         const struct chain_params *params,
                                         struct block_index *target,
                                         const char *datadir)
{
    /* The reducer is the engine: the stage-side unwind needs only the
     * active-chain cursor + a reducer kick. state / coins_tip / params /
     * datadir are retained in the signature (callers pass the
     * controller-owned context) but are unused here. */
    (void)state;
    (void)coins_tip;
    (void)params;
    (void)datadir;
    if (!ms || !target) return false;

    /* If the target is not on the active chain there is nothing to roll
     * back — the FAILED mark + activation kick handle a fork tip. */
    if (!active_chain_contains(&ms->chain_active, target))
        return true;

    /* The STAGE owns the coins.db / UTXO unwind. Drive the stage-side reorg
     * unwind exactly as the live reorg path does — move the active-chain
     * cursor DOWN to target's parent (a pure cursor move, no legacy coins
     * write), then kick the reducer. The stage's own unwind machinery
     * (utxo_apply_reorg_unwind_if_needed /
     * rewind_cursor_if_active_chain_reorged) then OBSERVES the branch_hash
     * divergence at the now-lowered active tip, walks DOWN to the fork
     * boundary emitting the inverse-delta events, deletes the stale
     * log/delta rows, and rewinds the stage cursors to the invalidated
     * height — the byte-exact stage analogue of the legacy undo restore. */
    struct block_index *parent = target->pprev; /* != NULL: non-genesis */
    if (!active_chain_move_window_tip(&ms->chain_active, parent)) {
        LOG_RETURN(false, "validation",
                   "invalidate: stage-unwind cursor move to parent "
                   "(h=%d) failed for target h=%d",
                   parent ? parent->nHeight : -1, target->nHeight);
    }
    /* Drive the stage inverse-delta unwind to convergence. */
    (void)reducer_kick(boot_activation_controller());
    return true;
}

/* ── Orchestrators ───────────────────────────────────────────────── */

enum invalidate_result process_block_invalidate(struct main_state *ms,
                                                const struct uint256 *hash,
                                                struct uint256 *out_hash)
{
    if (out_hash) memset(out_hash, 0, sizeof(*out_hash));
    if (!ms || !hash) {
        LOG_RETURN(INVALIDATE_NOT_ATTEMPTED, "validation",
                     "invalidate: NULL ms or hash");
    }

    struct block_index *target = block_map_find(&ms->map_block_index, hash);
    if (!target) {
        char hex[65];
        uint256_get_hex(hash, hex);
        LOG_RETURN(INVALIDATE_BLOCK_NOT_FOUND, "validation",
                    "invalidate: block %s not found in index", hex);
    }
    if (out_hash && target->phashBlock)
        *out_hash = *target->phashBlock;

    if (target->nHeight == 0 || !target->pprev) {
        LOG_RETURN(INVALIDATE_IS_GENESIS, "validation",
                    "invalidate: refusing to invalidate genesis/rootless "
                    "block h=%d", target->nHeight);
    }

    char hex[65];
    uint256_get_hex(target->phashBlock ? target->phashBlock : hash, hex);
    int tip_h_before = active_chain_height(&ms->chain_active);
    fprintf(stderr, // obs-ok:invalidate-begin
            "[invalidate] h=%d hash=%s tip=%d: marking FAILED_VALID + "
            "disconnect-and-reorg\n", target->nHeight, hex, tip_h_before);

    /* Resolve the disconnect context from the activation controller —
     * the single owner of coins_tip/params/datadir. */
    struct chain_activation_controller *ctl = boot_activation_controller();

    /* Roll the active chain back below the target FIRST. find_most_work_chain
     * refuses to return a candidate below the active tip, so the chain
     * must be disconnected to target's parent before activation can pick
     * the next-best fork. (Bitcoin Core's InvalidateBlock does the same.) */
    if (ctl && ctl->coins_tip &&
        active_chain_contains(&ms->chain_active, target)) {
        struct validation_state vs;
        validation_state_init(&vs);
        if (!process_block_disconnect_to_parent(&vs, ms, ctl->coins_tip,
                                                 ctl->params, target,
                                                 ctl->datadir)) {
            /* Leave the FAILED mark off — we did not mutate status yet —
             * so the operator can retry. */
            LOG_RETURN(INVALIDATE_DISCONNECT_FAIL, "validation",
                        "invalidate: could not disconnect active chain "
                        "below h=%d", target->nHeight);
        }
    }

    /* Mark FAILED_VALID + propagate FAILED_CHILD. */
    size_t marked_children = 0;
    process_block_mark_invalid(ms, target, &marked_children);

    /* Persist the target's flip (descendants are CHILD-derived and will
     * be re-derived on boot from the persisted target). */
    bool persisted = invalidate_persist_pindex(target);
    if (!persisted) {
        LOG_RETURN(INVALIDATE_PERSIST_FAILED, "validation",
                    "invalidate: failed to persist FAILED_VALID for h=%d "
                    "(in-memory mark holds until restart)", target->nHeight);
    }

    /* Kick the engine so the next-best fully-valid chain is reconnected.
     * The reducer re-walks the best chain by draining the staged Job
     * pipeline through reducer_kick. */
    if (ctl)
        (void)reducer_kick(ctl);

    int tip_h_after = active_chain_height(&ms->chain_active);
    fprintf(stderr, // obs-ok:invalidate-done
            "[invalidate] h=%d: marked FAILED_VALID, %zu descendants "
            "FAILED_CHILD, tip %d → %d\n",
            target->nHeight, marked_children, tip_h_before, tip_h_after);

    return INVALIDATE_OK;
}

enum reconsider_result process_block_reconsider(struct main_state *ms,
                                                const struct uint256 *hash,
                                                struct uint256 *out_hash)
{
    if (out_hash) memset(out_hash, 0, sizeof(*out_hash));
    if (!ms || !hash) {
        LOG_RETURN(RECONSIDER_NOT_ATTEMPTED, "validation",
                     "reconsider: NULL ms or hash");
    }

    struct block_index *target = block_map_find(&ms->map_block_index, hash);
    if (!target) {
        char hex[65];
        uint256_get_hex(hash, hex);
        LOG_RETURN(RECONSIDER_BLOCK_NOT_FOUND, "validation",
                    "reconsider: block %s not found in index", hex);
    }
    if (out_hash && target->phashBlock)
        *out_hash = *target->phashBlock;

    /* Clear FAILED on the target + descendants. */
    size_t cleared = 0;
    process_block_clear_invalid(ms, target, &cleared);
    if (cleared == 0) {
        char hex[65];
        uint256_get_hex(target->phashBlock ? target->phashBlock : hash, hex);
        fprintf(stderr, // obs-ok:reconsider-no-failure
                "[reconsider] h=%d hash=%s: no FAILED marks to clear\n",
                target->nHeight, hex);
        return RECONSIDER_NO_FAILURE;
    }

    /* Persist the target's cleared status. */
    bool persisted = invalidate_persist_pindex(target);
    if (!persisted) {
        LOG_RETURN(RECONSIDER_PERSIST_FAILED, "validation",
                    "reconsider: failed to persist cleared status for h=%d",
                    target->nHeight);
    }

    /* Kick the engine so the now-eligible chain is re-evaluated.
     * The reducer re-walks via reducer_kick (the stage forward-apply that
     * mirrors connect_block). */
    struct chain_activation_controller *ctl = boot_activation_controller();
    if (ctl)
        (void)reducer_kick(ctl);

    int tip_h = active_chain_height(&ms->chain_active);
    fprintf(stderr, // obs-ok:reconsider-done
            "[reconsider] h=%d: cleared %zu FAILED entries, tip=%d\n",
            target->nHeight, cleared, tip_h);
    return RECONSIDER_OK;
}
