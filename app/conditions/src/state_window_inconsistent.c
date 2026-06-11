/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

/* Condition: state_window_inconsistent (fail-loud validation pack,
 * checks 4+5)
 *
 * Detects the window-consistency sweep blocker (window.consistency: a
 * named cursor-ordering / log-contiguity / coin-tear / oscillation
 * invariant violated in durable state) or the commitment audit blocker
 * (coins.commitment_spot_check: the utxos table no longer recomputes to
 * its commitment — silent truncation/corruption). Both blockers are
 * SELF-CLEARING on a later clean pass (repair jobs may legitimately fix
 * holes); this Condition keeps an unresolved violation escalating.
 *
 * WINDOW_REBUILD TRIGGER SEAM (wave 3, designed): replace this
 * re-witness remedy with window_rebuild(first_bad_h) — the blocker
 * reason names the exact invariant + heights. Clear ON WITNESSED success
 * only. Until then: HOLD + PAGE, the operator owns the repair. */

#include "conditions/condition_registry.h"
#include "framework/condition.h"
#include "util/blocker.h"

#include <stdbool.h>

static bool detect_state_window_inconsistent(void)
{
    return blocker_exists("window.consistency") ||
           blocker_exists("coins.commitment_spot_check");
}

static enum condition_remedy_result remedy_state_window_inconsistent(void)
{
    /* WINDOW_REBUILD TRIGGER SEAM (wave 3): rebuild the named window.
     * v1: no automated repair — FAILED escalates to the operator. */
    return COND_REMEDY_FAILED;
}

static bool witness_state_window_inconsistent(int64_t target_at_detect)
{
    (void)target_at_detect;
    // honest-witness-ok: remedy returns COND_REMEDY_FAILED (no automated
    // repair until the wave-3 window_rebuild seam), so this read can
    // never be credited as a remedy-caused clear. The blockers it reads
    // are SELF-CLEARING only on a later CLEAN sweep/audit over the real
    // durable state (cursors/logs/commitment re-derived), so a clear
    // here witnesses the invariant actually holding again.
    return !detect_state_window_inconsistent();
}

static struct condition c_state_window_inconsistent = {
    .name = "state_window_inconsistent",
    .severity = COND_CRITICAL,
    .poll_secs = 60,
    .backoff_secs = 300,
    .max_attempts = 2,
    .detect = detect_state_window_inconsistent,
    .remedy = remedy_state_window_inconsistent,
    .witness = witness_state_window_inconsistent,
    .witness_window_secs = 90,
};

void register_state_window_inconsistent(void)
{
    (void)condition_register(&c_state_window_inconsistent);
}
