/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Best-work chain-selection helpers for the reducer activation path. */

#include "platform/time_compat.h"
#include <stdio.h>

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "process_block_internal.h"

/* add_to_block_index() lives in lib/validation/src/accept_block_header.c
 * with its sole caller so header admission owns the runtime in-memory
 * block_index producer. The declaration remains in
 * process_block_internal.h for validation-internal callers. */

struct block_index *find_most_work_chain(struct main_state *ms)
{
    struct block_index *best = active_chain_tip(&ms->chain_active);
    int skipped_no_chaintx = 0;
    int skipped_failed = 0;
    int skipped_invalid = 0;


    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (!pindex)
            continue;

        /* Skip failed blocks and their children (typed: PERMANENT,
         * DEPENDENCY, and TRANSIENT all gate selection here; the
         * eventual retry-budget for TRANSIENT lands in a separate
         * path that re-validates without involving this scan). */
        if (block_has_any_failure(pindex)) {
            skipped_failed++;
            continue;
        }

        /* Must have at least header validation */
        if (!block_index_is_valid(pindex, BLOCK_VALID_TREE)) {
            skipped_invalid++;
            continue;
        }

        /* Only consider chains where the candidate block has data available.
         * Prefer nChainTx > 0 (cumulative tx count — means block AND all
         * ancestors have data), but also accept BLOCK_HAVE_DATA (block
         * data exists on disk from zclassicd import / scan, even if
         * nChainTx wasn't propagated yet). connect_block will fully
         * validate before committing. Without the HAVE_DATA fallback,
         * imported blocks with data but nChainTx==0 are invisible to
         * chain selection, causing sync stalls. */
        if (pindex->nChainTx == 0 &&
            !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            skipped_no_chaintx++;
            continue;
        }

        /* LANE D / SELF-HEAL (S3 sibling-adopt): strictly-more-work always wins;
         * EQUAL work wins ONLY when the active-chain incumbent at pindex's height
         * is PRESENT and FAILED (the zeroed-Sapling-root sibling case). Two VALID
         * equal-work siblings keep the strict `>` rule and never oscillate.
         * Shared with active_chain_most_work_candidate so the policy lives at one
         * site. Parity-restoring (a FAILED incumbent is not a valid tip). */
        if (active_chain_selection_candidate_beats_best(&ms->chain_active,
                                                        pindex, best)) {
            /* Check ancestry for failed blocks */
            bool chain_ok = true;
            struct block_index *check = pindex;
            int tip_h = best ? best->nHeight : -1;
            while (check && check->nHeight > tip_h) {
                if (block_has_any_failure(check)) {
                    chain_ok = false;
                    break;
                }
                if (!check->pprev && check->nHeight > 0) {
                    /* pprev not linked — stop walking ancestry.
                     * This is normal after LDB import where block_index
                     * entries have nChainTx set (from the import) but
                     * pprev pointers aren't fully resolved yet. The
                     * nChainTx > 0 check above already ensures data
                     * availability — don't reject the chain just
                     * because we can't walk pprev to genesis. */
                    break;
                }
                check = check->pprev;
            }
            if (chain_ok)
                best = pindex;
        }
    }

    if (skipped_no_chaintx > 0 && !best) {
        printf("find_most_work_chain: WARNING: %d blocks skipped "
               "(no data, nChainTx==0)\n", skipped_no_chaintx);
    }

    /* refuse to return a candidate BELOW the current tip.
     * The tip is canonical. A "fork tip" at a lower height with higher
     * nChainWork can appear from old import data with incorrect work
     * accounting, but reorging backwards 17 k blocks because of it is
     * never the right answer — the staged activation path would hit the
     * finality guard, log "below_finality_depth" every second, and the chain
     * would never advance. Treat below-tip best as "no work pending" and let
     * gap-fill close the headers-vs-bodies window. */
    {
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip && best && best != tip && best->nHeight < tip->nHeight) {
            static time_t g_last_stale_log = 0;
            time_t now_log = platform_time_wall_time_t();
            if (now_log - g_last_stale_log >= 60) {
                g_last_stale_log = now_log;
                printf("find_most_work_chain: ignoring stale fork tip "
                       "h=%d (tip h=%d, depth=%d) — returning tip\n",
                       best->nHeight, tip->nHeight,
                       tip->nHeight - best->nHeight);
            }
            best = tip;
        }
    }

    /* Diagnostic: when chain selection keeps the current tip as best, emit a
     * rate-limited log line naming the filter counters. This is how the
     * canary identifies which shortlisted cause is keeping production stuck
     * without another investigative round-trip.
     *
     * also kick the gap-fill service so it requests the
     * missing bodies for headers above the tip. Without this kick,
     * gap_fill only wakes every GAPFILL_TICK_SECS=5s and the headers
     * gap closes slowly; an explicit kick from chain selection
     * accelerates convergence whenever activation runs. */
    {
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip && best == tip) {
            int header_h = ms->pindex_best_header
                         ? ms->pindex_best_header->nHeight : 0;
            if (header_h > tip->nHeight + 1) {
                process_block_kick_gap_fill();
            }
            if (header_h > tip->nHeight + 100) {
                static time_t g_last_stuck_log = 0;
                time_t now_log = platform_time_wall_time_t();
                if (now_log - g_last_stuck_log >= 60) {
                    g_last_stuck_log = now_log;
                    printf("find_most_work_chain: STUCK at tip h=%d "
                           "(best_header h=%d, gap=%d) "
                           "skipped[failed=%d invalid=%d no_data=%d]\n",
                           tip->nHeight, header_h,
                           header_h - tip->nHeight,
                           skipped_failed, skipped_invalid,
                           skipped_no_chaintx);
                }
            }
        }
    }

    return best;
}

/* Header acceptance moved to lib/validation/src/accept_block_header.c. The
 * reducer (reducer_ingest_block / reducer_kick, app/services + app/jobs) is
 * the sole block-connect engine; every ingest call site routes through it.
 * Selection helpers remain here. Active-tip child disk verification lives in
 * process_block_tip_child.c, tip publication lives in
 * process_block_tip_publish.c, contextual-header skip policy lives in
 * process_block_contextual_header.c, block-index hydration lives in
 * process_block_index.c, and failed-child propagation lives in
 * process_block_failed_child.c. */
