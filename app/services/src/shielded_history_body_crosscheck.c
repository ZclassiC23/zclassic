/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ============================ PLACEHOLDER ============================
 * PLACEHOLDER — replaced by the lane B implementation at merge.
 *
 * The real shielded_history_body_crosscheck_run() re-derives Sprout frontiers
 * and the (nf, pool) set from the copy datadir's own PoW/merkle-verified block
 * bodies over [0, checkpoint_height] and compares them against the producer's
 * tables (both opened read-only). It is being implemented by a sibling lane; the
 * interface contract already lives in
 * app/services/include/services/shielded_history_body_crosscheck.h.
 *
 * Until that real implementation lands, this placeholder returns
 * false (infrastructure failure) so shielded_history_promote_run() REFUSES the
 * sprout + nullifier install and leaves the wedge intact — the safe, fail-closed
 * production default. The promote service's fixture test injects a deterministic
 * verdict through its crosscheck test seam, so it does NOT depend on this stub.
 *
 * This file is deliberately self-contained (single translation unit, one symbol)
 * so the merge is a straight FILE SWAP: this placeholder is dropped and lane B's
 * real .c is taken in its place. Do NOT add other symbols here.
 * ====================================================================
 */
// one-result-type-ok:body-crosscheck-witness — the contract (owned by
// services/shielded_history_body_crosscheck.h, lane B) is deliberately
// `bool + struct crosscheck_result *out`: false means infrastructure failure,
// and the sprout_ok/nullifiers_ok verdicts travel in *out. Not a zcl_result
// service surface. This placeholder mirrors that contract exactly.

#include "services/shielded_history_body_crosscheck.h"

#include "util/log_macros.h"

bool shielded_history_body_crosscheck_run(const char *copy_datadir,
                                          const char *producer_datadir,
                                          int64_t checkpoint_height,
                                          struct crosscheck_result *out)
{
    (void)copy_datadir;
    (void)producer_datadir;
    (void)checkpoint_height;
    if (out) {
        out->sprout_ok = false;
        out->nullifiers_ok = false;
        out->max_height = -1;
        out->nf_count = 0;
        out->sprout_frontier_count = 0;
    }
    /* Infrastructure failure: the real body verifier is not linked yet, so the
     * promote must refuse (never install unverified sprout/nullifier state). */
    LOG_RETURN(false, "shielded_crosscheck",
               "PLACEHOLDER body-crosscheck: real local-body verifier not "
               "linked (lane B) — refusing so the promote stays fail-closed");
}
