/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

/* Condition: tip_label_divergence (fail-loud validation pack, checks 1+2)
 *
 * Detects the HOLD latch raised by the per-connect linkage check
 * (chain.linkage_violation) or the coinbase-height label check
 * (chain.coinbase_label_mismatch): OUR height labels diverged from the
 * chain's own structure. The blocker + EV_OPERATOR_NEEDED already fired
 * at detection; this Condition keeps the divergence VISIBLE in the
 * condition engine, `zclassic23 dumpstate`, and health until repaired,
 * and escalates through the engine's attempt ladder.
 *
 * WINDOW_REBUILD TRIGGER SEAM (wired): the remedy consumes the latch's
 * refuse_from_h and re-derives [refuse_from_h, tip] from PoW-verified
 * on-disk bodies via stage_rederive_range() (THE universal re-derive
 * primitive — it rewinds the body-dependent stage cursors + inverse-
 * rewinds coins in one transaction, then the forward fold rewrites the
 * SAME verdicts; consensus-parity-neutral). It then releases the linkage
 * holds so the forward fold re-connects those blocks and re-runs the O(1)
 * linkage/coinbase-label checks: a genuinely repaired label passes and
 * stays clear; a persistent divergence RE-RAISES the hold+blocker at the
 * first offending connect, which the witness reads as still-present. The
 * repair is bounded (max_attempts) and loud (WARN + EV_RECOVERY_ACTION);
 * an LCC refusal or a store error escalates to the operator. */

#include "conditions/condition_registry.h"
#include "framework/condition.h"
#include "jobs/reducer_frontier.h"
#include "jobs/stage_rederive_range.h"
#include "services/sync_monitor.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "validation/chain_linkage_check.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "event/event.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

static bool detect_tip_label_divergence(void)
{
    return blocker_exists("chain.linkage_violation") ||
           blocker_exists("chain.coinbase_label_mismatch");
}

static enum condition_remedy_result remedy_tip_label_divergence(void)
{
    /* WINDOW_REBUILD TRIGGER SEAM: re-derive [refuse_from_h, tip] and
     * release the linkage holds so the forward fold re-checks. */
    int refuse_from = chain_linkage_hold_refuse_from();
    if (refuse_from < 0) {
        /* The blocker is present but no hold latch carries a refuse_from height
         * (the production raiser chain_linkage_hold_raise always sets both, so
         * this is either a test-injected bare blocker or a lingering blocker
         * whose hold was cleared out-of-band). Without a refuse_from there is no
         * rebuild window to open — report FAILED so the engine escalates rather
         * than pretend a repair we could not attempt. */
        LOG_WARN("tip_label_divergence",
                 "[tip_label_divergence] window_rebuild: blocker present but no "
                 "linkage hold latch carries a refuse_from height — cannot "
                 "locate a rebuild window, escalating");
        return COND_REMEDY_FAILED;
    }

    sqlite3 *db = progress_store_db();
    struct main_state *ms = sync_monitor_main_state();
    if (!db || !ms) {
        /* NOT LOG_FAIL: that macro returns and would be read as a value; here
         * we must return the typed FAILED so the engine escalates. */
        LOG_WARN("tip_label_divergence",
                 "[tip_label_divergence] window_rebuild: %s unavailable — "
                 "cannot re-derive [%d, tip]",
                 db ? "main_state" : "progress db", refuse_from);
        return COND_REMEDY_FAILED;
    }

    int32_t tip = reducer_frontier_provable_tip_cached();
    if (tip <= refuse_from)
        tip = (int32_t)active_chain_height(&ms->chain_active);
    if (tip < refuse_from) {
        LOG_WARN("tip_label_divergence",
                 "[tip_label_divergence] window_rebuild: tip=%d below "
                 "refuse_from=%d — no range to re-derive, escalating",
                 (int)tip, refuse_from);
        return COND_REMEDY_FAILED;
    }

    struct stage_rederive_range_result rr = {0};
    if (!stage_rederive_range(db, ms, refuse_from, (int)tip, &rr)) {
        LOG_WARN("tip_label_divergence",
                 "[tip_label_divergence] window_rebuild: stage_rederive_range "
                 "[%d, %d] hit a store error — escalating", refuse_from,
                 (int)tip);
        return COND_REMEDY_FAILED;
    }
    if (rr.refused_no_inverse) {
        /* LCC refusal: an applied height in the range has no crypto-vetted
         * inverse delta to rewind coins. This class needs a refold-from-anchor
         * (or operator); the in-place re-derive cannot open the window. */
        LOG_WARN("tip_label_divergence",
                 "[tip_label_divergence] window_rebuild: LCC refused re-derive "
                 "[%d, %d] (applied height lacks inverse delta) — escalating to "
                 "refold-from-anchor/operator", refuse_from, (int)tip);
        return COND_REMEDY_FAILED;
    }
    if (!rr.ok) {
        LOG_WARN("tip_label_divergence",
                 "[tip_label_divergence] window_rebuild: re-derive [%d, %d] did "
                 "not commit — escalating", refuse_from, (int)tip);
        return COND_REMEDY_FAILED;
    }

    /* Rewind committed: the stale [refuse_from, tip] suffix is scheduled for
     * re-derivation from PoW-verified on-disk bodies. Release the linkage holds
     * so the forward fold re-connects those blocks and re-runs the O(1)
     * linkage/coinbase-label checks. A genuinely repaired label stays clear; a
     * persistent divergence re-raises the hold+blocker at the first offending
     * connect, which the witness reads as still-present (UNWITNESSED) — the
     * engine escalates. */
    LOG_WARN("tip_label_divergence",
             "[tip_label_divergence] window_rebuild: re-derived [%d, %d] "
             "rewound=%d cursors=%d coins_rewound=%d — releasing linkage holds "
             "for the forward re-fold re-check", refuse_from, (int)tip,
             (int)rr.rewound, rr.cursors_rewound, (int)rr.coins_rewound);
    chain_linkage_hold_clear("linkage");
    chain_linkage_hold_clear("coinbase_label");
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=tip-label-window-rebuild from=%d to=%d rewound=%d "
                "coins_rewound=%d", refuse_from, (int)tip, (int)rr.rewound,
                (int)rr.coins_rewound);
    return COND_REMEDY_OK;
}

static bool witness_tip_label_divergence(int64_t target_at_detect)
{
    (void)target_at_detect;
    // honest-witness-ok: the remedy triggers a REAL repair — it re-derives
    // [refuse_from, tip] from on-disk bodies and releases the linkage holds so
    // the forward fold re-runs the O(1) linkage/coinbase-label checks. Those
    // checks RE-RAISE the PERMANENT blocker at the first still-divergent connect,
    // so a blocker read as absent here means the re-fold re-connected the range
    // cleanly (a true "divergence actually repaired"), and a still-present
    // blocker downgrades the OK to UNWITNESSED. The remedy never deletes the
    // blocker except via chain_linkage_hold_clear, which only opens the door for
    // the re-check to re-raise — this is not poison-absence we manufactured.
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
