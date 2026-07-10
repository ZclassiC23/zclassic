/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_root_ladder_tripwire — OBSERVE-ONLY production caller for the golden
 * UTXO root ladder verifier (models/utxo_root_ladder_verify.h /
 * chain/utxo_root_ladder.h). Same posture as the SHA3 golden-window
 * corroboration tripwire in this directory (tip_finalize_post_step.c's
 * sha3_window_tripwire_*): recomputes nothing new — it compares THIS node's
 * own already-persisted coins_kv boundary-root store
 * (coins_kv_boundary_root_get) against the locked golden-height ladder
 * table, and on a byte mismatch raises a typed PERMANENT blocker + an
 * evidence event, naming the divergent height. It NEVER rejects a block,
 * NEVER raises a pipeline HOLD, NEVER changes the tip or consensus
 * accept/reject — E13-neutral, identical to the SHA3 tripwire's contract.
 *
 * Cadence: only re-checked at MMR_COMMITMENT_INTERVAL (100-block) boundaries
 * — the same cadence tip_finalize_post_step.c persists a fresh boundary root
 * on, so a newly-written root is corroborated at the very next opportunity.
 * The dense mmb_root() layer (utxo_root_ladder_verify_dense_anchor) is
 * DELIBERATELY NOT called from here: it re-folds the ENTIRE leaf-hash
 * pipeline (millions of leaves once past its anchor height) and is only
 * cheap as a rare/opt-in operation — see the ZCL_UTXO_LADDER_HEAVY nightly
 * step (Makefile `simnet-nightly` target) and test_utxo_root_ladder.c's
 * heavy section. The per-rung compare this file drives is O(g_utxo_root_
 * ladder_count) point reads (today count==1), so running it every 100
 * blocks costs nothing measurable on the reducer thread.
 *
 * Internal to app/jobs/src (not a public jobs/ API) — same convention as
 * tip_finalize_post_step.h. The test group redeclares the entry points it
 * needs directly (see lib/test/src/test_utxo_root_ladder_tripwire.c). */

#ifndef ZCL_JOBS_UTXO_ROOT_LADDER_TRIPWIRE_H
#define ZCL_JOBS_UTXO_ROOT_LADDER_TRIPWIRE_H

#include <stddef.h>

struct utxo_root_ladder_verify_result;

enum utxo_root_ladder_tripwire_result {
    UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY  = 0,  /* no divergence (incl. all
                                              * NOT_YET_REACHED / empty) */
    UTXO_ROOT_LADDER_TRIPWIRE_MISMATCH = 1,  /* >=1 rung diverged; evidence
                                              * emitted (blocker + event) */
};

/* Emit the observe-only evidence for a completed
 * utxo_root_ladder_verify_against_store() call. `results`/`n` are that
 * call's own output array. Silent (HEALTHY, no blocker) when every rung is
 * MATCH or NOT_YET_REACHED. Registers the `utxo_ladder_mismatch` typed
 * blocker (PERMANENT, owner "utxo_root_ladder_tripwire") naming the FIRST
 * divergent rung's height and the divergent/total rung counts when at
 * least one rung is DIVERGENT. `results` NULL or `n` 0 is a no-op HEALTHY
 * return. */
enum utxo_root_ladder_tripwire_result
utxo_root_ladder_tripwire_report(const struct utxo_root_ladder_verify_result *results,
                                 size_t n);

/* Run the tripwire iff `height` is an MMR_COMMITMENT_INTERVAL boundary (the
 * cadence a fresh boundary root can appear on) and the operator kill-switch
 * env `ZCL_DISABLE_UTXO_LADDER_TRIPWIRE` is unset. Reads the live
 * progress_store_db() handle; a NULL handle (store not open yet) is a
 * benign skip. Off-boundary heights are a no-op — no db touch, no cost. */
void utxo_root_ladder_tripwire_at_boundary(int height);

#endif /* ZCL_JOBS_UTXO_ROOT_LADDER_TRIPWIRE_H */
