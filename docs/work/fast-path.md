# The Fast Path — our information algorithm for getting to correct C

The C diff that fixes a problem is usually small (the reducer un-wedge was ~40
lines; the service_state driver ~180) — the **information algorithm** is the
work: turning a vague live symptom into the one correct small change, with
confidence, without expensive wrong turns. Each stage below makes one historical
failure class structurally impossible: the bodies-vs-coins misdiagnosis (a
design cycle on the wrong cause), and the 3,130,701 → 47,279 chain reset (a fix
that deleted tip_finalize_log rows, shipped without a reset-safe test).

## The stages (scale down for trivial changes — don't 9-agent a 3-line edit)

1. **GROUND in live truth — before any hypothesis.**
   `make diagnose-gap SLUG=<x>` dumps the three orthogonal views that must
   agree at tip — active public tip (A), best header tip (H), applied coins
   tip (C), and HAVE_DATA at A+1 (D) — plus the operational mode and active
   Conditions, and prints a root-cause verdict. STOP rule: **no hypothesis
   until the triple is captured.** This kills the bodies-vs-coins class:
   `C << A` is coins-application lag (reconcile), not a body gap; `A < H` with
   `D=true` is bodies-present-not-connected, not body-fetch.

2. **DESIGN + adversarially critique — before code.** For consensus-critical
   changes (anything touching `tip_finalize`, `*_log`, `coins_best`,
   `connect_block`, `active_chain_tip`, the boot reconcile span, or import /
   cold-import), write the design first and have it refuted. The load-bearing
   stamp: prove `can_reset_tip=false` and `weakens_gate=false`.

3. **RESET-SAFE UNIT TEST — before live.** Mirror
   `lib/test/src/test_stage_reducer_unwedge.c`: synthesize the broken state,
   assert the invariant the fix must hold (e.g. the public tip never drops
   below `coins_best`; no `*_log` rows deleted). Run it with
   `make t ONLY=<group>` (seconds).

4. **REPRODUCE ON A COPY — before touching the live chain.**
   `make repro-on-copy SLUG=<x> ARGS='...'` snapshots the live datadir to a
   throwaway copy and runs the node against it on an isolated port. It is a
   tip-regression detector: it FAILS LOUD if the public tip collapses — so the
   catastrophic tail (the 47,279 reset; the import-reset to ~199) is caught on
   a copy, never on the live node. For recovery work, make it a real H* climb
   gate too: `make repro-on-copy SLUG=<x> CLIMB_PAST=<height> ARGS='...'`
   FAILS if the copy only boots/holds flat and never serves a provable tip
   strictly above `<height>`. The live datadir is never written.

5. **VERIFY + commit.** `make t ONLY=<group>` (inner loop) → `make build-only`
   / `make syntax-check` (does it compile) → `make lint` (full gates) → commit.

## The fast inner loop (use these, never `build/bin/test_zcl` in the loop)

| command | what |
|---|---|
| `make t ONLY=<group>` | run ONE test group, rebuilding the harness first (closes the stale-`test_parallel` rebuild trap) |
| `make build-only` | incremental compile-check of the whole node (no link) |
| `make syntax-check` | full no-link syntax check across every TU |
| `make lint-fast` | the 5 highest-signal lint gates (full `make lint` before commit) |
| `make test` | the fast fork-based parallel suite (~1 min); `make test-full` is the slow single-process binary |

## Invariants that hold across every stage

- **Live truth before design.** Pull `make diagnose-gap` first; never reason
  from a guessed cause.
- **Reproduce on a copy before any live chain/datadir mutation.** No exceptions
  for consensus-critical experiments.
- **Never weaken a consensus gate; never delete `tip_finalize_log` rows; never
  lower the public tip below `coins_best`.** Recovery only ever raises or holds
  the tip.
