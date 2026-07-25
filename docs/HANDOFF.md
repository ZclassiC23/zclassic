> **Read this file first for current live-node state.** Older revisions are
> evidence, not standing fact — recover them with
> `git log --follow -- docs/HANDOFF.md`.

# HANDOFF — current state

The typed status commands are ground truth; this page is a pointer to
evidence files, never a substitute for re-checking them:

```bash
zclassic23 status
zclassic23 dumpstate reducer_frontier
tail -5 ~/.local/state/zclassic23-slo/uptime-ledger.jsonl
```

## Current state

The canonical node is holding network tip on the self-verified (cured)
stack, per the external SLO ledger: `~/.local/state/zclassic23-slo/uptime-ledger.jsonl`
records canonical served height against `gap_vs_oracle`. Treat any
"at tip" / "holding tip" claim as unverified until that ledger's most recent
line confirms it.

## `main` is ahead of the running binary

The canonical node runs a binary built before the current `main`. Everything
below is committed and gated (`make lint`, `make test-parallel`) but **not
deployed**, and none of it is live until an owner-gated `make deploy`:

| In `main`, not running | Where |
|---|---|
| Cost-tiered onion admission (STATIC/CHEAP/EXPENSIVE) + adaptive client puzzle; one admission primitive, the duplicate retired | `lib/net/src/onion_ratelimit.c`, `lib/net/src/puzzle.c` |
| Wallet mutating commands bound to the RPC engine (address new/import/export-key, transaction send, shielded send, rescan, backup-now) and address labels | `docs/API_REFERENCE.md` marks these `ready` |
| Groth16 comb-based verify speedup behind a differential parity oracle | `make check-groth16-parity`, `make bench-groth16-comb` |
| Sapling-tree rebuild seeded from the anchor_kv frontier (the healer for the blocker below) | merged earlier as `82f11c697` |
| Fault-injection and convergence proof harnesses; sync shadow observer; prune-after-seal safety gate; time authority | test groups + `lib/util/src/time_authority.c` |
| `lib/base` — the dependency sink (logging, allocation, the result type). `util/log_macros.h` and `util/result.h` forward to it, so the 733 includers are untouched | `lib/base/` |
| `config/` no longer reachable from `lib/`: the 14 upward symbol references (5 of them bare `extern`, invisible to the include-grep gate) replaced by boot-registered ports | `lib/net/src/net_runtime_port.c`, `config/src/node_db_runtime.c` |
| Per-file test headers: `test_core.h` + facets, so a node-header edit no longer dirties ~36% of the test build | `lib/test/include/test/` |
| Two ISA paths that had never compiled into a shipped binary — AVX-512 SHA3-512 (missing permutation + a 32-byte over-read) and SHA-NI SHA-256 (now runtime-dispatched with a known-answer self-test) | `lib/crypto/src/` |
| Boot refusals that fire before the log exists now report themselves; the doc they name is guaranteed to exist by `check-error-doc-refs` | `config/src/boot_error.c`, `tools/lint/check_error_doc_refs.sh` |

Deploy policy during the hold window is unchanged: a restart resets the 72h
trailing window, so deploy when the escalator fires anyway, or after
`HOLD_PROVEN`. Do not restart the canonical node for any other reason.

## Verify the cure

| Claim | Evidence file |
|---|---|
| Anchor-refold rebuild applied, revert path kept | `~/.local/state/zclassic23-cure/verdict.jsonl`; backups under `~/.zclassic-c23-PREINSTALL-*` and `~/.zclassic-c23-PREPROMOTE-*` |
| Tip-holding, externally confirmed | `~/.local/state/zclassic23-slo/uptime-ledger.jsonl` (`gap_vs_oracle`, `ts=`) |
| 72h hold accrual toward `HOLD_PROVEN` | `~/.local/state/zclassic23-slo/hold-ledger.jsonl`; judge: `tools/scripts/slo_hold_judge.sh`; recorder: `zclassic23-hold-certifier.timer` (`make install-hold-certifier`) |

## Open blockers

- **`sapling_tree_rebuild.fail_closed`** is a known-junk blocker, not
  corruption. When the legacy sapling-tree-checkpoint copy is discarded at
  boot for a root mismatch, the deferred rebuild replays from Sapling
  activation over bodies that do not exist below the cure anchor, so its
  first appended commitment is guaranteed to mismatch. The canonical
  incremental tree (`fold_sapling`, anchor_kv) independently verifies against
  every block header's `hashFinalSaplingRoot` and is healthy. Fix: seed the
  rebuild from the anchor_kv frontier instead of replaying from genesis, and
  reclassify skip-tainted mismatches as DEPENDENCY, not PERMANENT. Until
  fixed, this blocker keeps the escalator's empty-escape set non-empty.
- **Label-splice wedge class** — a NULL `block_hash` in a stale replay
  artifact can freeze H* during live operation, not only as wedge-era
  residue. The escalator's 600-block resnapshot refold self-cures it (crude,
  always-terminating). The merged in-place healer fixes the fault at the
  source; deploy it the next time this class fires.

## Open branches — work that is finished but did not pass review

Four branches exist because an independent verifier ran the gates and found a
specific defect, not because they ran out of time. Each names what to fix.
Re-verify before trusting any of it; all four forked before the merges above
and need a reconcile pass.

| Branch | What it does | Why it is not in `main` |
|---|---|---|
| `perf/stable-objdir-and-gold-linker` | Stops relocating all 1883 objects on every edit; per-object attestation replaces the whole-tree key. **5x less CPU per edit, independently reproduced.** | Wall time got ~11% WORSE on a 32-core box — the wasted work was hiding in spare cores, and the new per-link verifier costs ~1s. Also: editing a per-object CFLAGS line in the Makefile changes no source file, so the toolkey does not move and a stale object survives — a real regression against the guarantee it replaces. And `check-build-epoch-integrity` can now report PASS while nothing ran (its cache key omits the two new scripts; copy-proven). |
| `lane/testcache-soundness-phase0` | Fixes three live soundness bugs in the test skip-cache: `.def` files were absent from the key, the key bound the compiler but not the flags (an `-O1` pass was honoured by the `-O3` gate), and a fresh tree silently produced a header-free key. Toolkeys now differ per profile, proven by `strings` on the binaries. | One of its own new assertions passes on the unmodified code too, so it pins nothing. It also mutates `ZCL_STRESS_TESTS` without restoring it, which suppresses coverage in the sequential runner, and its Makefile-scanning helper reads only the first 256 KB of a 327 KB file. |
| `lane/module-linkgraph-enforcement` | Measures the module graph from the linker (`nm`), declares the rank order in `config/lib_module_order.def`, and adds two gates. Also fixes a real bug: the code index skipped `epochs/`, which is where 100% of live depfiles are, so its include graph was empty. | `check-no-cross-layer-extern` false-fails on a clean tree whenever the only warm object tree is a test tree — proven end-to-end with no env override. The gate docstrings also claim a full `make` arms them; it does not (a full `make` produces zero `.o` files). |
| `wf/measure` | Block-fold pipeline instrumentation. | Never reviewed: it wraps every stage, so it needs a deliberate overhead measurement before landing. Its profiling datadir copy (`~/.zclassic-c23-COPY-20260725-001046-fold-profile`, 17 GB) is intentionally left in place — delete it if this lane is abandoned. |

`archive/stash-refold-driver-wip` is older experimental work preserved from a
stash, not a live lane.

## Where the developer loop actually spends its time

Measured, recorded in [`BENCHMARKS_LOG.md`](BENCHMARKS_LOG.md). Read that before
optimising anything here — two confident hypotheses died against these numbers.

- The bare link over 1883 objects is **0.90s** (`ld.bfd`) / **0.58s** (`ld.gold`).
  It is not the bottleneck, despite `ZCL_DEV_LINKER` resolving to empty on this
  host and the gating lane never referencing it. Both true, both worth ~0.3s.
- The one-file rebuild cost is the compile-epoch churn: the object directory is
  keyed on a whole-tree content-and-stat hash, so one edit — even `touch` with
  no change — relocates every object and re-invokes every recipe.
- The remaining large lever is the ~36% of test objects that a node-header edit
  used to dirty; the header split took a real edit from 680 recompiles to 9.

## MVP status

MRS and per-criterion evidence live in [`docs/MVP.md`](MVP.md) and
`zclassic23 milestone` (REST `GET /api/v1/milestone`). Only a run-passing
`make mvp-verify` member moves a ◐ to a ✅ — never hand-bump the count.

## Operational notes

- `-import-complete-shielded` requires the source chainstate's best block to
  equal exactly the target coins-island root — a bind guard, not a bug.
- A `-bootstrapserve` zclassicd pins its on-disk chainstate at the serve
  anchor.
- `chainstate_legacy_reader` reads LevelDB SSTs only; it does not replay a
  non-empty WAL. A non-empty WAL must refuse loudly rather than silently
  drop data.
- `zclassicd invalidateblock` does not persist across restarts in this fork.

## Lanes

| Lane | Datadir | Deploy | Purpose |
|---|---|---|---|
| **live** | `$HOME/.zclassic-c23` | `make deploy` (owner-gated) | Public daily-driver node; restart only for a vetted live deploy. |
| **dev** | `$HOME/.zclassic-c23-dev` | verify/probe only | Isolated build/test lane; public tooling cannot restart or publish to it. |
| **soak** | `$HOME/.zclassic-c23-soak` | deliberate re-baseline | Long-uptime / weekly evidence lane; do not churn during development. |

`zclassic23 agentlanes` / REST `/api/v1/agent` report each lane's
`operator_lane` (`zcl.operator_lane.v1`) and restart policy; prefer that
contract over parsing systemd names. The units declare the same intent with
`-operator-lane=canonical|dev|soak`. `zclassicd` (the C++ reference) runs
co-located — never stop it.

`make deploy-dev`, `make deploy-dev-fast`, and `make agent-deploy-fast` are
Phase-0 contained: every public invocation refuses before service, datadir, or
generation mutation. Build, source verification, simulation, and hermetic
fixture probes stay available.

`make lane-health` is the read-only three-lane status check. It reports systemd
state, RPC reachability, listener state, height, lag from the live lane, peer
count, restart count, memory pressure, role readiness, soak-evidence
eligibility, any `-reindex-chainstate` flag, and the binary-owned
`bootstrapstatus.snapshot_loader` posture (snapshot seed height, active loader
path, `recovery_hint`). `role_ready` answers whether a lane serves its assigned
purpose; the dev lane is not role-ready when its lag exceeds the lane
threshold, even with RPC and listeners up. `soak_eligible=false` means the soak
lane is alive but not earning clean MVP-C6 evidence. It is an observability
check, not automatic failover.

`make lane-recover LANE=dev|soak` is a read-only bounded recovery planner
emitting `zcl.lane_recovery_plan.v1`. Public `--apply` and
`ZCL_LANE_RECOVERY_APPLY=1` refuse before any mutation; `live`, `canonical`,
and `main` are refused outright. Use it after `make lane-health` reports a
recovery hint — it is a planner, never an apply path.

Copy-prove every recovery path on a datadir COPY before live; never live
surgery. Gate on **H\* CLIMB**, never "booted without FATAL." Never weaken a
safety/operator gate. Gate every change: `make` + `make lint` +
`make test-parallel` (read the `N passed, M failed` line, not the pipe exit).
Replay any consensus-predicate tightening against real chain history first —
see `docs/CONSENSUS_PARITY_DOCTRINE.md`.

## Pointers

- [`docs/work/FORWARD_PLAN.md`](work/FORWARD_PLAN.md) — the ordered plan.
- [`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) — the sovereign-cure design.
- [`docs/work/sovereign-cutover-runbook.md`](work/sovereign-cutover-runbook.md) — owner-gated cutover + revert.
- [`docs/AGENT_TRAPS.md`](AGENT_TRAPS.md) — read before "fixing" anything.
- [`docs/MVP.md`](MVP.md) — the v1 acceptance bar (8 criteria).
- [`docs/FRAMEWORK.md`](FRAMEWORK.md) — canonical architecture reference (§9 is the open-item debt board), off the v1 path.

A map, not the territory: trust the code you read this minute over this
file.
