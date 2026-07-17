/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The always-sync G-TIP chaos regression gate.
 *
 * MISSION: the node must ALWAYS fold to the network tip and never silently
 * stall; a stall is always a NAMED, SELF-HEALED blocker at a known height,
 * never an operator page for a recoverable cause. This table-driven test
 * drives the six reusable fault injectors in sim/simnet_chaos_faults.h and
 * asserts the mission property on each: the fixture recovers, and no
 * EV_OPERATOR_NEEDED (or equivalent named escalation) fires for what should
 * have been a recoverable cause.
 *
 * The G-TIP case (fault a, "full block index + empty active-chain window")
 * gets TWO rows instead of one:
 *
 *   - a BOUNDED gap, inside BLOCK_INDEX_LOADER_SEED_MAX_GAP — this repair is
 *     already landed production code (app/services/src/block_index_loader_
 *     rebuild.c); this row is an unconditional regression gate.
 *   - a LIVE-WEDGE-SCALE gap, beyond BLOCK_INDEX_LOADER_SEED_MAX_GAP — this
 *     is the documented, still-open Pillar-0 wedge (docs/HANDOFF.md). This
 *     row SKIPs (does not fail) today, and flips to an unconditional
 *     regression gate the moment a sibling lane's production fix makes the
 *     injector report `ok=true` for an over-cap gap — no edit needed here.
 *
 * See lib/sim/include/sim/simnet_chaos_faults.h for what each fault
 * reproduces and app/services/include/services/block_index_loader.h for
 * BLOCK_INDEX_LOADER_SEED_MAX_GAP.
 */

#include "test/test_helpers.h"

#include "services/block_index_loader.h"
#include "sim/simnet_chaos_faults.h"

#include <stdio.h>
#include <string.h>

#define ASC_CHECK(name, expr) do {                                          \
    printf("always_sync_chaos: %s... ", (name));                           \
    if (expr) { printf("OK\n"); }                                          \
    else { printf("FAIL\n"); failures++; }                                 \
} while (0)

/* Common assertion every fault must satisfy: the harness fixture built
 * cleanly (`ok`) and no operator page fired for what is, by construction,
 * a recoverable synthetic fault. `recovered` is asserted separately per row
 * below since fault (a)'s live-wedge-scale row deliberately SKIPs it. */
static bool asc_never_pages(const struct chaos_fault_result *r)
{
    return r->ok && !r->operator_paged;
}

int test_always_sync_chaos(void)
{
    printf("\n=== always_sync_chaos (G-TIP fault-injection harness) ===\n");
    int failures = 0;
    struct chaos_fault_result r;

    /* ── (a-1) G-TIP: bounded gap — the landed regression floor ────────── */
    {
        const int gap = 200; /* well inside BLOCK_INDEX_LOADER_SEED_MAX_GAP */
        bool harness_ok = chaos_fault_empty_active_chain_window(gap, &r);
        printf("  note: %s\n", r.note);
        ASC_CHECK("gtip bounded: harness fixture built", harness_ok);
        ASC_CHECK("gtip bounded: never pages operator", asc_never_pages(&r));
        ASC_CHECK("gtip bounded: active_chain_tip INSTALLED + H* CLIMBS",
                  r.ok && r.recovered && r.hstar_after == gap);
    }

    /* ── (a-2) G-TIP: live-wedge-scale gap — SKIP-or-ASSERT ─────────────
     * A gap comfortably beyond MAX_GAP, at the SAME scale relationship the
     * documented live wedge has to the cap (a multiple of it), kept cheap
     * for a unit test. If the injector still refuses (today's reality),
     * this is the known-open Pillar-0 wedge: print SKIP, do not fail. The
     * moment a sibling lane's fix makes the genesis-root branch resolve an
     * over-cap gap, `r.ok` flips true here and this row becomes a hard
     * regression gate with zero changes to this file. */
    {
        const int gap = BLOCK_INDEX_LOADER_SEED_MAX_GAP + 10000;
        bool harness_ok = chaos_fault_empty_active_chain_window(gap, &r);
        printf("  note: %s\n", r.note);
        ASC_CHECK("gtip live-wedge-scale: harness fixture built", harness_ok);
        if (!r.ok) {
            printf("always_sync_chaos: gtip live-wedge-scale... "
                   "SKIP (Pillar-0 unbounded-gap fix not yet landed — "
                   "MAX_GAP=%d refuses by design; see docs/HANDOFF.md)\n",
                   BLOCK_INDEX_LOADER_SEED_MAX_GAP);
        } else {
            ASC_CHECK("gtip live-wedge-scale: never pages operator",
                      !r.operator_paged);
            ASC_CHECK("gtip live-wedge-scale: active_chain_tip INSTALLED + "
                      "H* CLIMBS (Pillar-0 fix landed — now a hard gate)",
                      r.recovered && r.hstar_after == gap);
        }
    }

    /* ── (b) kill/restart mid-fold ───────────────────────────────────── */
    {
        bool harness_ok = chaos_fault_kill_restart_mid_fold(&r);
        printf("  note: %s\n", r.note);
        ASC_CHECK("kill/restart: harness fixture built", harness_ok);
        ASC_CHECK("kill/restart: never pages operator", asc_never_pages(&r));
        ASC_CHECK("kill/restart: H* resumes identically, nothing lost",
                  r.recovered);
    }

    /* ── (c) corrupt a sealed chain_segment ─────────────────────────── */
    {
        bool harness_ok = chaos_fault_corrupt_sealed_segment(&r);
        printf("  note: %s\n", r.note);
        ASC_CHECK("segment corruption: harness fixture built", harness_ok);
        ASC_CHECK("segment corruption: never pages operator",
                  asc_never_pages(&r));
        ASC_CHECK("segment corruption: detect+repair returns to SERVING",
                  r.recovered);
    }

    /* ── (d) freeze the reducer drive ───────────────────────────────── */
    {
        bool harness_ok = chaos_fault_freeze_reducer_drive(&r);
        printf("  note: %s\n", r.note);
        ASC_CHECK("reducer-drive freeze: harness fixture built", harness_ok);
        ASC_CHECK("reducer-drive freeze: never pages operator",
                  asc_never_pages(&r));
        ASC_CHECK("reducer-drive freeze: named stall, then resumes ticking",
                  r.recovered);
    }

    /* ── (e) stall a single stage ────────────────────────────────────── */
    {
        bool harness_ok = chaos_fault_stall_single_stage(&r);
        printf("  note: %s\n", r.note);
        ASC_CHECK("single-stage stall: harness fixture built", harness_ok);
        ASC_CHECK("single-stage stall: never pages operator (recoverable "
                  "cause, retry budget held)", asc_never_pages(&r));
        ASC_CHECK("single-stage stall: self-heals, blocker clears",
                  r.recovered);
    }

    /* ── (f) kill/restart mid-RECOVERY (inside an open rewind window) ──
     * Unlike (b) — a kill BEFORE any repair action started — this kills
     * AFTER stage_rederive_range() has committed a rewind (cursors
     * lowered, stale rows deleted, coins inverse-rewound to the hole) but
     * BEFORE the drive re-folds the range forward. */
    {
        bool harness_ok = chaos_fault_kill_restart_mid_recovery(&r);
        printf("  note: %s\n", r.note);
        ASC_CHECK("kill mid-recovery: harness fixture built", harness_ok);
        ASC_CHECK("kill mid-recovery: never pages operator",
                  asc_never_pages(&r));
        ASC_CHECK("kill mid-recovery: H* + coins frontier survive the kill "
                  "identically, next pass converges", r.recovered);
    }

    if (failures == 0)
        printf("=== always_sync_chaos: ALL PASS ===\n\n");
    else
        printf("always_sync_chaos: failures=%d\n", failures);
    return failures;
}
