/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

/* Condition: tip_label_divergence (fail-loud validation pack, checks 1+2)
 *
 * Detects the HOLD latch raised by the per-connect linkage check
 * (chain.linkage_violation) or the coinbase-height label check
 * (chain.coinbase_label_mismatch): OUR height labels diverged from the
 * chain's own structure. The blocker + EV_OPERATOR_NEEDED already fired
 * at detection; this Condition keeps the divergence VISIBLE in the
 * condition engine + zcl_state + health until it is actually repaired,
 * and escalates through the engine's attempt ladder.
 *
 * WINDOW_REBUILD TRIGGER SEAM: replace this re-witness remedy with
 * window_rebuild(first_bad_h). The HOLD latch + blocker reason carry
 * refuse_from_h; window_rebuild consumes it, rebuilds [refuse_from_h,
 * tip], then chain_linkage_hold_clear(check_id) + blocker clear ON
 * WITNESSED success only. Until then: HOLD + PAGE, the operator owns
 * the repair. */

#include "conditions/condition_registry.h"
#include "framework/condition.h"
#include "util/blocker.h"
#include "validation/chain_linkage_check.h"

#include <stdbool.h>

static bool detect_tip_label_divergence(void)
{
    return blocker_exists("chain.linkage_violation") ||
           blocker_exists("chain.coinbase_label_mismatch");
}

static enum condition_remedy_result remedy_tip_label_divergence(void)
{
    /* WINDOW_REBUILD TRIGGER SEAM: call
     * window_rebuild(chain_linkage_hold_refuse_from()) here. v1 has no
     * automated repair for a label divergence — report FAILED so the
     * engine escalates to the operator after its attempt budget. */
    return COND_REMEDY_FAILED;
}

static bool witness_tip_label_divergence(int64_t target_at_detect)
{
    (void)target_at_detect;
    // honest-witness-ok: remedy returns COND_REMEDY_FAILED (v1 has no
    // automated repair; the window_rebuild seam above is the intended
    // replacement), so the engine can never credit this read as a remedy-
    // caused clear. The PERMANENT blockers it reads are cleared only by
    // a real repair (operator / window_rebuild via
    // chain_linkage_hold_clear), so this is a truthful "divergence
    // actually repaired" read, not poison-absence we deleted ourselves.
    return !detect_tip_label_divergence();
}

static struct condition c_tip_label_divergence = {
    .name = "tip_label_divergence",
    .severity = COND_CRITICAL,
    .poll_secs = 60,
    .backoff_secs = 300,
    .max_attempts = 2,
    .detect = detect_tip_label_divergence,
    .remedy = remedy_tip_label_divergence,
    .witness = witness_tip_label_divergence,
    .witness_window_secs = 60,
};

void register_tip_label_divergence(void)
{
    (void)condition_register(&c_tip_label_divergence);
}
