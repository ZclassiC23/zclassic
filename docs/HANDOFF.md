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

## 0-LATEST. AT TIP on the cured stack (uptime-ledger gap_vs_oracle=1 at ts=1784918497); 72h hold accruing, certifier armed (2026-07-24)

The canonical node reached and is holding the network tip — externally
confirmed by uptime-ledger canonical samples: served_height 3,192,736
gap_vs_oracle=1 (ts=1784917477) and 3,192,759 gap_vs_oracle=1 (ts=1784918497,
still following the tip an hour later). Every claim below carries its
external-ledger line; re-derive with `zclassic23 status` / the SLO ledger
before acting.

| Fact | Evidence (external ledger) |
|---|---|
| REBUILD cure applied + copy-proven, revert kept | `~/.local/state/zclassic23-cure/verdict.jsonl` (`zcl-anchor-refold-proof-8/-9` PASS, `zcl-revert-proof-1` PASS, `zcl-live-apply-1` APPLIED); backups `~/.zclassic-c23-PREINSTALL-20260724-101627`, `~/.zclassic-c23-PREPROMOTE-20260724-113719` |
| **AT TIP, externally confirmed** | SLO `uptime-ledger.jsonl`: canonical served 3,176,325 (ts 1784892997) → 3,192,736 gap=1 (ts 1784917477) → 3,192,759 gap=1 (ts 1784918497) |
| **72h hold certifier ARMED** (15-min timer) | `~/.local/state/zclassic23-slo/hold-ledger.jsonl` first verdict line ts=1784918144 `NOT_PROVEN reason=reachability` (honest: the trailing 72h still contains the pin era, the apply outage, 2 pages, and the 16:12Z regression below). Judge = `tools/scripts/slo_hold_judge.sh`; recorder = `--record` via `zclassic23-hold-certifier.timer` (`make install-hold-certifier`). The first `HOLD_PROVEN` line in that file IS the 72h win-proof. |
| One live incident since promotion: label-splice wedge at h=3,192,628, self-cured in ~5 min | node.log 16:12:06Z `blocker_stall_meta_detector` DEFECT CLASS (H\*=3,192,627 frozen 907s) → sticky escalator rungs retry → targeted_rederive → resnapshot: `stage_rederive [3192000,3192627]` (32 rederive commits 16:12–16:16Z, served dipped to 3,192,099 in the SLO ledger at ts 1784909617, then re-climbed to tip) |

The 16:12Z incident confirms the NULL-block_hash label-splice fault class
occurs in LIVE operation (h=3,192,628 is far above the cure walls), not just
as wedge-era residue. The running binary (started 11:37:53Z, NRestarts=0)
predates the merged in-place healer (`edc925178` + merge `944077e99`), so the
escalator's 600-block resnapshot refold is currently the live cure — crude
but always-terminating, proven by the re-climb above. **Deploy policy for the
hold window:** each escalator firing regresses served_height and resets the
72h trailing window (visible as a fresh `served_advance` violation in
hold-ledger.jsonl), so if the fault class fires again, deploy the healer
build at that moment (the hold was reset anyway — the restart costs nothing);
if it never fires again, deploy only after HOLD_PROVEN. Do not restart the
canonical node for any other reason.

Known-junk blocker, root-caused 2026-07-24 (fix in flight on a worktree lane):
`sapling_tree_rebuild.fail_closed` (`intermediate_sapling_root_mismatch
h=3155873 commitments=1`) is NOT corruption. On this cure-seeded datadir the
legacy tree copy (node_state `sapling_tree` + `sapling_tree_ckpt.dat`) was
refused at boot (`ops state --subsystem=sapling_checkpoint`:
`last_load_result=discarded, root_mismatch` at h=3,176,379), and the deferred
rebuild then replays from Sapling activation over bodies that do not exist
below the cure anchor — a structurally-doomed replay whose first appended
commitment is guaranteed to mismatch. The CANONICAL tree is healthy:
`fold_sapling` (anchor_kv, coins DB) fail-closed verifies the incremental
root against every block header's `hashFinalSaplingRoot` and climbed ~16k
blocks to tip through that check. Fix = seed the rebuild from the anchor_kv
frontier (North Star: heal by reading the canonical ledger, never by
replaying a clone) + reclassify skip-tainted mismatches as DEPENDENCY.
Side-effect until deployed: this blocker keeps the empty-escape set non-empty,
which is what let the 16:12Z freeze arm the escalator.

Next: hold-ledger.jsonl accrues toward `HOLD_PROVEN` (earliest ≈ 72h after
the last violation, i.e. ≥ 2026-07-27 ~16:13Z), then Phase-B ladder carve per
`docs/work/ladder-carve-audit.md`. The reformed pager (merge `18bd0e797`)
paged the pin incident correctly and is the failure net.

Operational discoveries recorded in the cure runbook state dir: the
`-import-complete-shielded` bind guard requires the source chainstate best
block == the target coins island root EXACTLY; a `-bootstrapserve` zclassicd
pins its on-disk chainstate at the serve anchor; `chainstate_legacy_reader`
reads LevelDB SSTs only (WAL not replayed — followup: replay or refuse loudly
on a non-empty WAL); zclassicd `invalidateblock` does not persist across
restarts in this fork.

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
