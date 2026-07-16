/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * simnet chaos-fault injectors — the always-sync G-TIP harness.
 *
 * MISSION this file serves: the node must ALWAYS fold to the network tip and
 * never silently stall. A stall is always a NAMED, SELF-HEALED blocker at a
 * known height — never a silent halt, never an operator page for a
 * recoverable cause. These five functions each reproduce ONE class of fault
 * against a throwaway fixture (a scratch datadir, a scratch progress.kv, a
 * scratch supervisor/condition/blocker registration) and report whether the
 * subsystem under test named the fault and returned to advancing.
 *
 * This is TEST/SIM SCAFFOLDING, not production code: it builds fixtures with
 * raw sqlite3 INSERTs (the same convention lib/test/src/test_reducer_frontier.c
 * documents — "does not route through the AR lifecycle... this is TEST
 * scaffolding building the durable image, not production reducer code") and
 * calls real production entry points (block_index_loader_seed_tip_from_
 * finalized, reducer_frontier_compute_hstar, segment_corruption_scan_one/
 * repair, the supervisor + condition-engine + blocker primitives) as pure
 * oracles. No production .c/.h file is touched by this module's existence.
 *
 * Every fault:
 *   (a) chaos_fault_empty_active_chain_window — the block-index map is FULL
 *       (every header admitted, chained, HAVE_DATA+VALID_SCRIPTS) while the
 *       active-chain window is EMPTY (active_chain_tip() == NULL) — the
 *       "Pillar-0" boot wedge. Reproduces it at a caller-chosen gap size so
 *       one call can model both the in-bounds repair (already landed,
 *       BLOCK_INDEX_LOADER_SEED_MAX_GAP) and the live-node wedge scale
 *       (currently refused by design — see docs/HANDOFF.md).
 *   (b) chaos_fault_kill_restart_mid_fold — an abrupt close+reopen of
 *       progress.kv mid-fold (simulates kill -9 before the last stage
 *       committed); asserts H* recomputes identically after the "restart".
 *   (c) chaos_fault_corrupt_sealed_segment — tampers one sealed ROM segment
 *       byte and drives the existing segment_corruption {detect, remedy,
 *       witness} healer to completion.
 *   (d) chaos_fault_freeze_reducer_drive — freezes a supervised child's
 *       heartbeat past its deadline, asserts a NAMED stall (never silent),
 *       then unfreezes it and asserts it resumes ticking.
 *   (e) chaos_fault_stall_single_stage — a typed TRANSIENT blocker + a
 *       synthetic condition model a single reducer stage stalling; asserts
 *       zero EV_OPERATOR_NEEDED pages while the retry budget holds, then
 *       clears and asserts a witnessed self-heal.
 *
 * Every function is self-contained: it opens/builds its own fixture,
 * exercises the fault, tears down, and never mutates a live datadir (never
 * touches ~/.zclassic-c23 or the mint producer datadirs).
 */

#ifndef ZCL_SIM_SIMNET_CHAOS_FAULTS_H
#define ZCL_SIM_SIMNET_CHAOS_FAULTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Common outcome record every fault-injector fills in. */
struct chaos_fault_result {
    bool    ok;              /* the fault was injected and the fixture built
                              * cleanly (a false here is a HARNESS defect,
                              * not a finding about the subsystem). */
    bool    recovered;       /* the subsystem returned to advancing/serving
                              * after the fault (H* climbed again / the tip
                              * resolved / the child resumed ticking / the
                              * blocker cleared). */
    bool    operator_paged;  /* an EV_OPERATOR_NEEDED (or equivalent named
                              * escalation) fired for what should have been
                              * a RECOVERABLE cause. This is the failure mode
                              * the mission forbids — it must be false on
                              * every case this harness asserts (not merely
                              * skips). */
    int32_t hstar_before;    /* -1 when not applicable to this fault */
    int32_t hstar_after;     /* -1 when not applicable to this fault */
    char    note[192];       /* human-readable outcome for the test's printf */
};

/* (a) Pillar-0: full block index, empty active-chain window.
 *
 * Builds a genesis-rooted regtest chain 0..gap_height (every link
 * HAVE_DATA + VALID_SCRIPTS, contiguous pprev — "block index map full"),
 * leaves the active-chain window freshly initialized (NULL tip — "active
 * window empty"), seeds the durable finalized anchor + coins-applied
 * frontier at gap_height, then calls the real
 * block_index_loader_seed_tip_from_finalized() boot-repair path.
 *
 * On a successful install (`out->ok`), also stamps a full consistent ok=1
 * stage-log prefix [0, gap_height] and reads H* back via
 * reducer_frontier_compute_hstar() so `out->hstar_after` proves H* CLIMBED
 * to gap_height, not merely that a pointer became non-NULL.
 *
 * `out->recovered` mirrors `out->ok` here (there is no separate "unfreeze"
 * step for this fault — the repair either installs or it doesn't).
 * Returns false only on a harness-fixture error (disk, sqlite, OOM); check
 * `out->ok` for the actual repair verdict. */
bool chaos_fault_empty_active_chain_window(int gap_height,
                                           struct chaos_fault_result *out);

/* (b) kill/restart mid-fold.
 *
 * Builds a progress.kv fixture with a consistent ok=1 stage-log prefix
 * through a chosen height K, closes the store (simulating kill -9 before
 * any further stage advances), reopens it fresh on the same directory
 * (simulating the restart), and recomputes H*. Asserts nothing was lost or
 * corrupted by the abrupt close/reopen: hstar_after == hstar_before == K. */
bool chaos_fault_kill_restart_mid_fold(struct chaos_fault_result *out);

/* (c) corrupt a sealed chain_segment.
 *
 * Seals two segments into a fixture directory, tampers one byte in the
 * second's block-data region, drives segment_corruption_scan_one (detect)
 * then segment_corruption_repair (remedy), and re-opens the store to
 * witness the corrupt range is no longer covered while the survivor still
 * verifies clean — the "returns to SERVING" proof for this fault (reads
 * fall back to blk*.dat instead of ever serving corrupt bytes). */
bool chaos_fault_corrupt_sealed_segment(struct chaos_fault_result *out);

/* (d) freeze the reducer drive (supervisor liveness layer).
 *
 * Registers a synthetic supervised child modeling the reducer-drive tick
 * loop, freezes its heartbeat past its deadline, asserts the supervisor
 * fires exactly one NAMED stall (SUPERVISOR_STALL_TIME_DEADLINE, never a
 * silent halt), then "unfreezes" it (fresh heartbeat) and asserts it
 * resumes ticking on the next sweep. */
bool chaos_fault_freeze_reducer_drive(struct chaos_fault_result *out);

/* (e) stall a single stage.
 *
 * Registers a synthetic condition modeling one reducer stage stalled
 * behind a typed TRANSIENT blocker. Drives the condition engine while
 * counting EV_OPERATOR_NEEDED fires: asserts zero pages while the retry
 * budget holds, then flips the fixture's witness to simulate the stage's
 * self-heal and asserts the condition clears and the blocker is removed
 * with STILL zero operator pages. */
bool chaos_fault_stall_single_stage(struct chaos_fault_result *out);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_SIM_SIMNET_CHAOS_FAULTS_H */
