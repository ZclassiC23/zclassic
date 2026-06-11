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
 *     contiguously to a trust root) run a NON-DESTRUCTIVE in-memory
 *     slot-fill (the pprev chain IS contiguous at closure — that is the
 *     closure condition), repropagate chainwork above the SHA3 anchor,
 *     re-derive the band fact, and only then clear the blocker and
 *     unlatch the chain-evidence startup freeze. The boot disk-rebuild
 *     ladder (chain_restore_finalize) is deliberately NOT called here:
 *     band headers are header-only (no BLOCK_HAVE_DATA), so the disk
 *     walk is guaranteed to under-populate, fall into the full blk*.dat
 *     scan, and rewrite pprev=NULL across millions of shared
 *     block_index nodes on the live net thread — the exact
 *     ancestry-tear class this service exists to close.
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
#include "services/utxo_recovery_service.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "jobs/reducer_frontier.h"
#include "chain/chain.h"
#include "chain/checkpoints.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include "event/event.h"
#include "util/blocker.h"
#include "util/log_macros.h"

/* Conversation frontier: the highest trust-rooted below-island header
 * seen this process. Chain SLOTS only fill at closure, so an anchor
 * re-derived from slots alone sits one batch below the index frontier
 * forever — the peer re-serves the same already-known range and the
 * band walk livelocks (defect #7, live 2026-06-11: pinned at the same
 * 160-header batch across 8+ cycles, +160 per restart). block_index
 * nodes are never freed, so the raw pointer stays valid for the
 * process lifetime; consumers re-validate height + trust-rooting
 * against the CURRENT island before use, so a stale value can only
 * fall back to the slot walk, never mis-anchor. */
static struct block_index *_Atomic g_band_walk_frontier;

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

    /* Advance the conversation frontier even when the batch was 100%
     * already-known headers (newly_added==0): last_header is verified
     * trust-rooted + below the island, so the NEXT request must anchor
     * at or above it — never re-derive from the lagging slots. */
    struct block_index *prev = atomic_load(&g_band_walk_frontier);
    if (!prev || last_header->nHeight > prev->nHeight)
        atomic_store(&g_band_walk_frontier,
                     (struct block_index *)last_header);

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

    /* Prefer the conversation frontier (highest trust-rooted below-island
     * header SEEN, advanced by band_continue) — the slot walk below lags
     * it by a whole batch until closure fills slots, which is the anchor
     * livelock (defect #7). Re-validate against the CURRENT island so a
     * frontier recorded for an earlier band can never mis-anchor. */
    struct block_index *walk = atomic_load(&g_band_walk_frontier);
    if (walk && walk->nHeight < island_root->nHeight &&
        utxo_recovery_block_trust_rooted(walk))
        return walk;

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

/* Closure slot-fill: non-destructive and in-memory only. At closure the
 * tip's pprev chain is contiguous down to the trust root BY DERIVATION
 * (utxo_recovery_block_ancestry_break(tip) == NULL is the closure
 * condition), so slotting it into chain[] is a pure walk-and-store under
 * the writer lock. It only ever stores the tip's own ancestors — never
 * NULL, never a different fork — and never mutates pprev/pskip/nHeight
 * of any shared block_index node, so lock-free readers (reducer stages,
 * RPC, chain-evidence probes) see the old slot or the canonical ancestor,
 * both valid. The walk stops at the compiled SHA3 anchor: a band can
 * only exist above it (the ancestry derivation early-exits there), so
 * slots at or below the anchor are not this hole's to touch. */
static int syncsvc_header_band_fill_slots(struct active_chain *c,
                                          struct block_index *tip,
                                          int32_t stop_height)
{
    int filled = 0;
    zcl_mutex_lock(&c->write_lock);
    struct block_index **arr = c->chain;
    if (arr) {
        int budget = tip->nHeight + 1;   /* cycle defense */
        for (struct block_index *p = tip;
             p && p->nHeight > stop_height && budget-- > 0;
             p = p->pprev) {
            int h = p->nHeight;
            if (h < 0 || h > tip->nHeight)
                break;         /* defensive: never slot a tear shape */
            if (arr[h] != p) {
                arr[h] = p;
                filled++;
            }
        }
    }
    zcl_mutex_unlock(&c->write_lock);
    return filled;
}

void syncsvc_header_band_after_batch(struct main_state *ms,
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

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    const int32_t anchor_h = cp ? cp->height
                                : REDUCER_FRONTIER_TRUSTED_ANCHOR;

    /* Band CLOSED: the tip now descends contiguously to a trust root.
     * 1) Non-destructive in-memory slot-fill (heals getblockhash /
     *    active_chain_at in-process), then pskip for relinked island
     *    nodes (accepted pprev-less, so accept_block_header never built
     *    theirs). Bottom-up so each build reuses the parent's pskip. */
    int filled = syncsvc_header_band_fill_slots(&ms->chain_active, tip,
                                                anchor_h);
    for (int h = anchor_h + 1; h <= tip->nHeight; h++) {
        struct block_index *b = active_chain_at(&ms->chain_active, h);
        if (b && b->pprev && b->pskip == NULL)
            block_index_build_skip(b);
    }

    /* 2) Chainwork repropagation above the attested anchor extent: the
     *    island's work was seeded from a pprev-less anchor (nChainWork=0),
     *    understating every island block. Idempotent for already-correct
     *    chains — same write pattern as the accept_block_header relink. */
    int repropagated = 0;
    for (int h = anchor_h + 1; h <= tip->nHeight; h++) {
        struct block_index *b = active_chain_at(&ms->chain_active, h);
        if (!b || !b->pprev)
            continue;
        struct arith_uint256 proof = GetBlockProof(b);
        arith_uint256_add(&b->nChainWork, &b->pprev->nChainWork, &proof);
        repropagated++;
    }

    /* 3) The clear is bound to the DERIVED fact, never to control flow:
     *    re-derive after the heal and keep the band fact recorded if the
     *    ancestry is somehow torn again (the fill above is non-mutating,
     *    so this holds by construction today — the guard pins the
     *    invariant against any future closure step that touches pprev).
     *    Losing the fact while the tear persists would strand the
     *    backfill driver for the rest of the process lifetime. */
    island_root = utxo_recovery_block_ancestry_break(tip);
    if (island_root) {
        LOG_WARN("headers",
                 "header band close ABORTED: ancestry re-tore during "
                 "closure (island_root=%d tip=%d) — band fact retained, "
                 "backfill stays armed",
                 island_root->nHeight, tip->nHeight);
        return;
    }

    blocker_clear(HEADER_BAND_BLOCKER_ID);
    atomic_store(&g_band_walk_frontier, NULL);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=header_band_closed tip=%d slots_filled=%d "
                "chainwork_blocks=%d",
                tip->nHeight, filled, repropagated);
    LOG_WARN("headers",
             "HEADER BAND CLOSED: tip h=%d now descends contiguously to "
             "its trust root (%d chain slots filled, chainwork "
             "repropagated over %d blocks); requesting chain-evidence "
             "startup re-reconcile",
             tip->nHeight, filled, repropagated);

    /* 4) Unlatch the evidence freeze: the next controller construction
     *    re-runs cec_reconcile_startup once; ancestry now links, so the
     *    stale contradiction_frozen lifts and health recovers. */
    chain_evidence_request_startup_reconcile("header_band_closed");
}
