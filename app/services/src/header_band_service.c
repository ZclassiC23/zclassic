/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Header band backfill — closes the installed-above-frontier header hole.
 *
 * The defect class (live, 2026-06-11): cold-import installs a pprev-less
 * island anchor + cursor tip ABOVE the genesis-rooted header frontier,
 * leaving a band of never-requested headers between the contiguous
 * frontier (h=3,140,573) and the island root (h=3,143,301). Header
 * sync's restart-from-tip policy then kills every conversation that
 * could fill the band ("low batch ... restarting getheaders from tip"),
 * while tip-anchored locators die at the island root and can never name
 * a band block — the hole is permanent by construction.
 *
 * The fix is request-side only (header ACCEPTANCE is untouched):
 *   - syncsvc_header_band_continue: a below-tip batch that extends the
 *     trust-rooted frontier is PROGRESS, not a restart trigger.
 *   - syncsvc_header_band_backfill_anchor: while the band fact is
 *     recorded, periodic getheaders anchor at the contiguous frontier
 *     (the peer forks there and serves the band).
 *   - syncsvc_header_band_after_batch: on closure (tip descends
 *     contiguously to a trust root) run the ONE rebuild primitive,
 *     repropagate chainwork above the SHA3 anchor, clear the blocker,
 *     and unlatch the chain-evidence startup freeze.
 *
 * All band facts are DERIVED from pprev contiguity on demand
 * (utxo_recovery_block_ancestry_break); the typed blocker is a loud
 * cache of that derivation, never an authority. */
// one-result-type-ok:band-planner-predicates — these are getheaders
// planner predicates/selectors consumed on the net thread (bool/anchor
// pointer/void closure hook); every refusal that matters travels via
// structured LOG + EV_RECOVERY_ACTION, not a result carrier.

#include "sync/sync_planner.h"

#include "services/chain_evidence_authority_service.h"
#include "services/chain_restore_repair.h"
#include "services/utxo_recovery_service.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "jobs/reducer_frontier.h"
#include "chain/checkpoints.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include "event/event.h"
#include "util/blocker.h"
#include "util/log_macros.h"

bool syncsvc_header_band_continue(const struct active_chain *chain,
                                  const struct block_index *last_header)
{
    if (!chain || !last_header)
        return false;

    struct block_index *tip = active_chain_tip(chain);
    if (!tip)
        return false;

    const struct block_index *island_root =
        utxo_recovery_block_ancestry_break(tip);
    if (!island_root)
        return false;          /* tip is trust-rooted — no band exists */
    if (last_header->nHeight >= island_root->nHeight)
        return false;          /* island-side header — not band fill */
    /* The peer cannot buy restart-suppression with a detached low fork:
     * only a batch that extends the trust-rooted frontier is progress. */
    if (!utxo_recovery_block_trust_rooted(last_header))
        return false;

    /* Defensive: a producer or the boot scan normally recorded the band
     * already; derive-and-record here so a runtime-created band is just
     * as loud (registry rate-limits dups). */
    if (!blocker_exists(HEADER_BAND_BLOCKER_ID))
        utxo_recovery_note_band_unrooted_tip(tip, "band_continue");

    LOG_INFO("headers",
             "header band backfill: continuing from h=%d (island_root=%d)",
             last_header->nHeight, island_root->nHeight);
    return true;
}

struct block_index *syncsvc_header_band_backfill_anchor(
    const struct active_chain *chain)
{
    if (!chain)
        return NULL;
    /* O(1) exit for healthy nodes — the periodic getheaders planner
     * calls this on every kick; no ancestry walk without the band fact. */
    if (!blocker_exists(HEADER_BAND_BLOCKER_ID))
        return NULL;

    struct block_index *tip = active_chain_tip(chain);
    if (!tip)
        return NULL;
    const struct block_index *island_root =
        utxo_recovery_block_ancestry_break(tip);
    if (!island_root)
        return NULL;           /* band closed; after_batch clears the fact */

    /* The highest populated slot below the island root is the contiguous
     * frontier — anchor the conversation there so the peer forks at the
     * frontier and serves the band. */
    for (int h = island_root->nHeight - 1; h >= 0; h--) {
        struct block_index *frontier = active_chain_at(chain, h);
        if (!frontier)
            continue;
        if (!utxo_recovery_block_trust_rooted(frontier)) {
            LOG_WARN("headers",
                     "header band backfill: frontier candidate h=%d below "
                     "island_root=%d is itself not trust-rooted — no "
                     "servable band anchor", h, island_root->nHeight);
            return NULL;
        }
        return frontier;
    }
    LOG_WARN("headers",
             "header band backfill: no populated slot below island_root=%d "
             "— no servable band anchor", island_root->nHeight);
    return NULL;
}

void syncsvc_header_band_after_batch(struct main_state *ms,
                                     const char *datadir,
                                     const struct block_index *last_header)
{
    if (!ms)
        return;
    if (!blocker_exists(HEADER_BAND_BLOCKER_ID))
        return;                /* no band fact — no-op on the hot path */

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    if (!tip)
        return;
    const struct block_index *island_root =
        utxo_recovery_block_ancestry_break(tip);
    if (island_root) {
        /* Band still open — surface fill progress when the batch landed
         * in the band (closure needs the peer to re-serve the island
         * root so accept_block_header hash-binds its pprev). */
        if (last_header && last_header->nHeight < island_root->nHeight)
            LOG_INFO("headers",
                     "header band backfill progress: filled to h=%d "
                     "(island_root=%d)",
                     last_header->nHeight, island_root->nHeight);
        return;
    }

    /* Band CLOSED: the tip now descends contiguously to a trust root.
     * 1) The ONE rebuild primitive: slots the band headers into chain[]
     *    (heals getblockhash/active_chain_at in-process) + wires pskip. */
    struct zcl_result fin = chain_restore_finalize(ms, datadir);
    if (!fin.ok)
        LOG_WARN("headers",
                 "header band close: chain_restore_finalize: %s",
                 fin.message);
    /* finalize may have re-selected the tip; describe what is installed. */
    {
        struct block_index *post = active_chain_tip(&ms->chain_active);
        if (post)
            tip = post;
    }

    /* 2) Chainwork repropagation above the attested anchor extent: the
     *    island's work was seeded from a pprev-less anchor (nChainWork=0),
     *    understating every island block. Idempotent for already-correct
     *    chains — same write pattern as the accept_block_header relink. */
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    const int32_t anchor_h = cp ? cp->height
                                : REDUCER_FRONTIER_TRUSTED_ANCHOR;
    int repropagated = 0;
    for (int h = anchor_h + 1; h <= tip->nHeight; h++) {
        struct block_index *b = active_chain_at(&ms->chain_active, h);
        if (!b || !b->pprev)
            continue;
        struct arith_uint256 proof = GetBlockProof(b);
        arith_uint256_add(&b->nChainWork, &b->pprev->nChainWork, &proof);
        repropagated++;
    }

    blocker_clear(HEADER_BAND_BLOCKER_ID);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=header_band_closed tip=%d chainwork_blocks=%d",
                tip->nHeight, repropagated);
    LOG_WARN("headers",
             "HEADER BAND CLOSED: tip h=%d now descends contiguously to "
             "its trust root (chainwork repropagated over %d blocks); "
             "requesting chain-evidence startup re-reconcile",
             tip->nHeight, repropagated);

    /* 3) Unlatch the evidence freeze: the next controller construction
     *    re-runs cec_reconcile_startup once; ancestry now links, so the
     *    stale contradiction_frozen lifts and health recovers. */
    chain_evidence_request_startup_reconcile("header_band_closed");
}
