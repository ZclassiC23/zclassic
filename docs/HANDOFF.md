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
