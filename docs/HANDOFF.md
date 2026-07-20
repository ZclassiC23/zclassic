> **Read this file first for current live-node state.** Historical handoff
> revisions are evidence, not standing facts; recover them from Git with
> `git log --follow -- docs/HANDOFF.md` and
> `git show a22dc39265^:docs/HANDOFF.md` when incident archaeology requires it.
>
> **Durable hierarchy:** [`work/SOVEREIGN-NETWORK-ROADMAP.md`](work/SOVEREIGN-NETWORK-ROADMAP.md)
> preserves the Phase 0–6 program and promotion gates: stable sovereign sync
> first, transactional C23 hot swap second, sandboxed publishing third. It does
> not displace the immediate canonical cure below.

# HANDOFF — current state (2026-07-19)

## 0-LATEST. Current state — **AT TIP on sovereign state**

- **Height:** serve node is at network tip (H\* = peer max), 4 peers. Live
  H\*: `zclassic23 -rpcport=39072 dumpstate reducer_frontier` (**flagless CLI
  answers from the DEFAULT datadir — always pass `-rpcport=39072` for the
  serve node**).
- **Binary:** candidate `vhrepair-7d10e581d` (= main `7d10e581d`).
- **State provenance:** entirely self-verified — past the historical
  shielded-anchor wedge and the post-wedge poisoned-solution-row stall.
- **Soak:** window running from the at-tip timestamp; canonical deploy to the
  public lane is the owner's lever.
- **Kernel store:** `consensus.db` (the reducer kernel owns its own SQLite
  file; `progress.kv` holds only the address_index/txindex projections via
  `projection_store`). Migration is idempotent/crash-safe at boot. Pre-flip
  cold backup: `~/.zclassic-c23-serve1-BACKUP-preflip` (revert = old candidate
  binary + that backup; delete once soak completes).
- **Headers:** full 3.19M-header chain imported (`--importblockindex
  ~/.zclassic <datadir>/node.db` — 2nd arg is the node.db PATH).
- **Crash loop:** cured — supervisor tick-runner (no child tick on the sweep
  thread) + Sapling-persist deferral (no nested-BEGIN abort loop); backstop
  restarts stopped.
- **Consensus-bundle install:** terminal admitted-and-activated on this
  datadir (checkpoint-content authority).
- **Blockers:** the post-wedge poisoned-solution-row stall class is cured in
  code — mark failures raise ONE typed blocker and back off; a FAILED mask
  clears only if the unchanged PoW+Equihash validator passes with
  repairable-class evidence. Open follow-up: quarantine/refetch of a poisoned
  `blocks`-table row itself (`db_block_delete` is built and unwired — needs
  copy-proof); see the Wave N backlog memory.
- **Other open follow-ups:** spurious per-boot txn in
  `consensus_db_finalize_flip`; CLI wall-clock deadlines against a dead RPC
  port; graceful-stop timing for a folding node (restart flows must verify
  MainPID changed); post-soak cleanup of
  `~/.zclassic-c23-serve1-BACKUP-preflip` + `-COPY-headerfix` and old
  candidate dirs.

Mechanism detail for the cured wedge classes (the poisoned-row stall, the
tip_finalize boundary-persist pin, the header-continuation wedge) lives in
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) and
the Wave N backlog memory — not narrated here.

---

## 0. History — everything before the at-tip pass

The wedge is cured (§0-LATEST). Every earlier state this file used to narrate
in detail — the 2026-07-17 bundle-export/chain-binding-refusal night, the
mint3 from-genesis fold, the pre-cure "live node wedged at H\*=3,176,325 on
`utxo_apply.anchor_backfill_gap`" diagnosis, and the dated producer/cure
narratives before that (USS v3, legacy snapshot producers, the 2026-07-11..16
cure runs) — is archived in Git, not this file (the header above already
says so). Recover any of it with `git log --follow -- docs/HANDOFF.md` and
`git show <rev>^:docs/HANDOFF.md` for a revision before the change that
interests you; `git show a22dc39265^:docs/HANDOFF.md` is the pre-2026-07-19
sweep baseline. Root-cause mechanism for the cured wedge is preserved as
design reference (not live narrative) in
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) and
memory `project_live_wedge_anchor_frontier_rootcause_2026-07-12`. The
alternate **operational** cure design
([`docs/work/fast-sync-to-tip-plan-2026-07-16.md`](work/fast-sync-to-tip-plan-2026-07-16.md))
documents currently-shipped importer code but is **not** the path that
actually passed the wedge — the sovereign consensus-bundle install
(`docs/work/sovereign-cutover-runbook.md`) is, per §0-LATEST.

## 3. Agent interface

The native typed command registry (`zclassic23 <command>`) is the sole agent
interface. Node RPC transport lives under `app/controllers/`, and command
contracts carry native paths plus input/output schemas for discovery.

## 4. MVP status

**MRS 4/8** (do not bump without proof): C1 install, C2 Tor onion <60s, C4
receive shielded, C7 kill-9 recovery pass local operator proof. C3
cold-start, C5 file market, C6 7-day soak, C8 exact parity previously gated on
the sovereign cure landing; the cure is DONE (§0-LATEST) and the soak window
is now running, so these four are gated on accumulated clean soak time only.
Re-run `tools/mvp_gate.sh` for the current formal C6 judge before quoting
one here — do not trust a pinned verdict. Bar: [`docs/MVP.md`](MVP.md).

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
`make test_parallel` (read the `N passed, M failed` line, not the pipe
exit). Replay any consensus-predicate tightening against REAL history first
(the h=478544 lesson — `docs/CONSENSUS_PARITY_DOCTRINE.md`).

---

## 6. Pointers

- [`docs/work/FORWARD_PLAN.md`](work/FORWARD_PLAN.md) — THE plan.
- [`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) — the sovereign-cure spine.
- [`docs/work/fast-sync-to-tip-plan-2026-07-16.md`](work/fast-sync-to-tip-plan-2026-07-16.md) — the operational import-path cure design (the alternate cure noted in §0 History; not the path that passed the wedge).
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
