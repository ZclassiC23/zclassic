> **Read this file first for current live-node state.** Historical handoff
> revisions are evidence, not standing facts; recover them from Git with
> `git log --follow -- docs/HANDOFF.md` and
> `git show a22dc39265^:docs/HANDOFF.md` when incident archaeology requires it.
>
> **Durable hierarchy:** [`work/SOVEREIGN-NETWORK-ROADMAP.md`](work/SOVEREIGN-NETWORK-ROADMAP.md)
> preserves the Phase 0–6 program and promotion gates: stable sovereign sync
> first, transactional C23 hot swap second, sandboxed publishing third. It does
> not displace the immediate Q1/canonical-state priority below.

# HANDOFF — current state (2026-07-24)

## 0-LATEST. Regroup checkpoint — **Q1 is not won; protect the live lane**

Three different nodes/artifacts are in play. Do not treat one as another.

| Role | Identity | Observed state | Rule |
|---|---|---|---|
| **Canonical live lane** | default datadir, running build `3b0de63b0` | 2026-07-24: up 13 days with ZERO H\* advance the entire uptime; H\*=3,176,325, header frontier ~3,192,025, oracle 3,192,030, gap_vs_oracle ~15,705; primary blocker `catchup_stalled` (downstream symptom of the permanent fold blocker, `app/conditions/src/download_queue_starved.c`); status RPC over its 250 ms budget; restart watchdog escalation_level=3, `automation_restart_ok:false` | **Observe only.** Owner-gated; no restart, deploy, or state surgery from an agent. |
| **Stopwatch serving fixture** | `$HOME/.zclassic-c23-COPY-20260722-tipfresh3`, P2P `127.0.0.1:39070`, RPC `39071` | Typed fixture status reported H\*=3,190,536, `sync=at_tip`, 4 peers on 2026-07-22 | Immutable upstream fixture. Do not kill or mutate it while Q1 evidence depends on it. |
| **Q1 code checkpoint** | `298affaf1` (followed only by this documentation regroup) | Clean and pushed; `make arch-score` = **50/100** | Development happens in a fresh worktree/datadir COPY. Never deploy this checkpoint merely because unit tests pass. |

As of 2026-07-24 the canonical lane has been up 13 days with ZERO H\* advance
the entire uptime, so it has earned no soak time in its observed blocked state.
The wedge ROOT CAUSE is closed in code (baked `pprev` poison in
`block_index.bin`; the REBUILD recipe is copy-proven through install+climb in
the proof series under `~/.local/state/zclassic23-cure/`), but the LIVE APPLY IS
PENDING the owner gate, and the `tip_finalize` rate bug (~94% of each fold
round; fold rate collapses from ~50 to ~3 blk/s) is OPEN and gates any
catch-up. The old July 19 top-of-chain claim is historical, not current.
Re-derive all three rows before acting; heights move and fixtures can exit.

### Q1 evidence at this checkpoint

- Exact judge: `make c3-stopwatch-report` is **FAIL**, pointing at
  `build/c3-stopwatch/20260722T180227Z-3344123` with a stale/named-stall
  verdict. There is no ledger PASS, so Q1 and Q3 receive no points.
- Newest diagnostic COPY run:
  `build/c3-stopwatch/20260722T192608Z-330634`. It installed the bundle and
  climbed from H\*=3,057,014 to 3,068,904, but network tip was 3,190,504 and
  wall time was 617 seconds against the 600-second budget. This is useful
  liveness evidence, not a win-proof.
- The run crossed the earlier script-corruption height without reproducing
  that failure and staged bodies beyond the former stale-flush collision
  height. The durable H\* did not reach tip; do not overstate this as an
  end-to-end copy proof.
- Measured reducer cost per 2,000-block round was dominated by
  `body_persist` (about 52 seconds) and `script_validate` (about 24 seconds).
  Commit/fsync time was milliseconds. The next performance change must target
  one of those measured stage bodies, not transaction cadence or unrelated
  boot code.

### Landed and verified on `main`

- Batched stage transactions, commit invariants/LCC checks, bounded deferred
  event-log writes, and batched UTXO/tip-finalize paths.
- Historical duplicate-txid-safe `created_outputs` keys and bounded reads.
- `coins_ram` gather/flush lifetime protection plus monotonic stale-flush
  handling; unit tests cover both a clean stale request and dirty-overlay
  promotion.
- Bounded block-swarm pieces, intake-full retry, durability scoping, and
  loopback coverage.
- Full optimized build, all 92 lint gates (including consensus parity), and
  `make test-parallel`: `ALL TESTS PASSED — 0/720 groups failed`.

### Resume checklist — one bottleneck, one proof

1. Confirm `main` is clean and re-run the three typed status checks above.
2. Read Q1/Q3 in [`ARCH_QUEST_BOARD.md`](ARCH_QUEST_BOARD.md); do not edit the
   scorer or frontier-owner table and do not touch frozen consensus code.
3. Reproduce only on a fresh datadir COPY using the immutable fixture. Keep
   the last failing artifact until a newer artifact supersedes it.
4. Profile `body_persist` first, then `script_validate`, through existing
   typed/profile surfaces. Preserve hash, merkle, script, and shielded proof
   verification; optimize repeated representation or indexing work, not the
   predicates.
5. Gate each small change with its focused test. Then run the exact
   `make mvp-coldstart-to-tip-stopwatch` win-proof.
6. Only after a ledger PASS: confirm `make arch-score` rises, run
   `make lint && make test-parallel`, commit, and push. Q4 soak/net-disruption
   work begins only after Q1.

Mechanism detail for the historical wedge classes remains in
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md).

---

## 0. History — earlier state narratives

The wedge is root-caused and its fix copy-proven on a fixture; the live apply is
still pending the owner gate (§0-LATEST). Every earlier state this file used to narrate
in detail — the 2026-07-17 bundle-export/chain-binding-refusal night, the
mint3 from-genesis fold, the pre-cure "live node wedged at H\*=3,176,325 on
`utxo_apply.anchor_backfill_gap`" diagnosis, and the dated producer/cure
narratives before that (USS v3, legacy snapshot producers, the 2026-07-11..16
cure runs) — is archived in Git, not this file (the header above already
says so). Recover any of it with `git log --follow -- docs/HANDOFF.md` and
`git show <rev>^:docs/HANDOFF.md` for a revision before the change that
interests you; `git show a22dc39265^:docs/HANDOFF.md` is the pre-2026-07-19
sweep baseline. Root-cause mechanism for the wedge is preserved as
design reference (not live narrative) in
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) and
memory `project_live_wedge_anchor_frontier_rootcause_2026-07-12`. The
alternate **operational** cure design
([`docs/work/shielded-history-importer.md`](work/shielded-history-importer.md))
documents currently-shipped importer code but is **not** the path the July 19
narrative credited with passing the wedge — that credit went to the sovereign
consensus-bundle install (`docs/work/sovereign-cutover-runbook.md`). Neither has
passed the wedge on the live lane; both remain design references, per §0-LATEST.

## 3. Agent interface

The native typed command registry (`zclassic23 <command>`) is the sole agent
interface. Node RPC transport lives under `app/controllers/`, and command
contracts carry native paths plus input/output schemas for discovery.

## 4. MVP status

**MRS 4/8** (do not bump without proof): C1 install, C2 Tor onion <60s, C4
receive shielded, and C7 kill-9 recovery have local operator proofs. C3
cold-start, C5 full live store transfer, C6 168-hour clean soak, and C8 exact
parity remain partial. The currently observed canonical lane is blocked and is
not accruing C6 evidence. Bar: [`docs/MVP.md`](MVP.md).

---

## 5. Live ops state & lanes

| Lane | Datadir | Deploy | Purpose |
|---|---|---|---|
| **live** | `$HOME/.zclassic-c23` | `make deploy` (owner-gated) | Public daily-driver node. Restart only for vetted live deploys. |
| **dev** | `$HOME/.zclassic-c23-dev` | verify/probe only (`make deploy-dev` refuses) | Isolated build/test lane. Public tooling cannot restart it or publish a generation during containment. |
| **soak** | `$HOME/.zclassic-c23-soak` | deliberate re-baseline | Long-uptime / weekly evidence lane. Do not churn during development. |

The committed units declare the same intent to the binary with
`-operator-lane=canonical`, `-operator-lane=dev`, and `-operator-lane=soak`.
`zclassic23 agent` and REST `/api/v1/agent`
surface that as `operator_lane` (`zcl.operator_lane.v1`) with the lane's
restart policy. Prefer that native contract over parsing systemd names when a
lane's RPC is reachable.

`make deploy-dev`, `make deploy-dev-fast`, and `make agent-deploy-fast` are
Phase-0 contained: every public invocation refuses before service, datadir, or
generation mutation. Build, source verification, simulations, and hermetic
fixture probes remain available; resident `dlopen` probing is contained.
Retained activation machinery is exercised only by its inherited-FD-bound
hermetic self-test and is not a public deployment authority.

`make lane-health` is the read-only three-lane status check. It reports the
public live lane, long-uptime soak lane, and fresh-build dev lane with systemd
state, RPC reachability, listener state, height, lag from the live lane, peer
count, restart count, memory pressure, role readiness, soak-evidence
eligibility, any `-reindex-chainstate` flag, and the binary-owned
`bootstrapstatus.snapshot_loader` posture: snapshot seed height, active loader
path, and `recovery_hint`. `role_ready` answers whether a lane is serving its
assigned purpose (`canonical_ready`, `soak_evidence_ready`, or
`dev_lane_ready`); the dev lane is not role-ready when its lag exceeds the lane
threshold, even if RPC/listeners are up. `soak_eligible=false` means the soak
lane is alive but not currently earning clean MVP-C6 evidence. It is an
observability/failsafe check, not an automatic failover mechanism.

`make lane-recover LANE=dev` / `LANE=soak` is a read-only bounded recovery
planner that emits `zcl.lane_recovery_plan.v1`. Public `--apply` and
`ZCL_LANE_RECOVERY_APPLY=1` invocations refuse before unit, datadir,
snapshot-copy, drop-in, header-import, daemon-reload, or restart mutation;
`live`, `canonical`, and `main` remain refused as well. The script contains no
mutation implementation. Use the plan after `make lane-health` reports a
recovery hint, then obtain separately reviewed activation authority rather
than treating the planner as an apply path.

Restart behavior and snapshot-loader posture on the current serve datadir are
live facts — read them fresh from `make lane-health` /
`zclassic23 dumpstate boot` rather than trusting a pinned description here;
§0-LATEST's binary/datadir pointers are the current entry point. `zclassicd`
(the C++ reference) runs as a co-located service — **never stop it.**
Ports/runbook: `docs/SYNC.md`; re-pull live state (`zclassic23 status`) before acting.

**Standing method:** `make deploy` rm's the binary first (a stale binary was
a real multi-day outage; `deploy_verify.sh` confirms the exact source SHA-256
and running executable SHA-256; Git `build_commit` is display-only).
Copy-prove every recovery path on a COPY before live, never live surgery;
gate on **H\* CLIMB**, not "booted without FATAL." Never weaken a
safety/operator gate. Gate every change: `make` + `make lint` +
`make test-parallel` (read the `N passed, M failed` line, not the pipe
exit). Replay any consensus-predicate tightening against REAL history first
(the h=478544 lesson — `docs/CONSENSUS_PARITY_DOCTRINE.md`).

---

## 6. Pointers

- [`docs/work/FORWARD_PLAN.md`](work/FORWARD_PLAN.md) — THE plan.
- [`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) — the sovereign-cure spine.
- [`docs/work/shielded-history-importer.md`](work/shielded-history-importer.md) — the operational import-path cure design (the alternate cure noted in §0 History; not the path that passed the wedge).
- [`docs/work/sovereign-cutover-runbook.md`](work/sovereign-cutover-runbook.md) — owner-gated live cutover + revert for the sovereign bundle cure (the path that passed the wedge).
- [`docs/work/ROADMAPS.md`](work/ROADMAPS.md) — live vs superseded roadmaps.
- [`docs/AGENT_TRAPS.md`](AGENT_TRAPS.md) — looks-broken-but-isn't; read before "fixing" anything.
- [`docs/MVP.md`](MVP.md) — the v1 acceptance bar (8 criteria).
- Dated handoffs/audits/superseded roadmaps are removed from the tree, not
  archived in-repo — recover any of them with `git log --follow -- <old-path>`
  (see the note at the top of this file for the pattern).

## 7. Verify before you trust this file

A map, not the territory. Re-read the cited file:line before building on any
claim — trust the code you read THIS minute over this file. Architecture
reference (off the v1 path): [`docs/FRAMEWORK.md`](FRAMEWORK.md) (§9 is the
open-item debt board).
