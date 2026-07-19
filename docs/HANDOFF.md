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

## 0-LATEST. Current state (2026-07-19 night) — **AT TIP on sovereign state.** The serve node is fully synced; soak window running; canonical deploy is the owner's lever

**The serve node is AT NETWORK TIP** (H\* 3,187,17x = peer max), 4 peers,
entirely on self-verified state: past the historical 3,176,325
shielded-anchor wedge AND past the post-wedge poisoned-solution-row stall at
3,179,242. Binary: candidate `vhrepair-7d10e581d` (= main `7d10e581d`).
Live H\* via `zclassic23 -rpcport=39072 dumpstate reducer_frontier`
(**flagless CLI answers from the DEFAULT datadir — always pass
-rpcport=39072 for the serve node**). The persistent monitor announces
milestones and blocker-set changes; the always-synced soak window runs from
the at-tip timestamp. Canonical deploy after a clean soak is the owner's
lever.

The post-wedge stall class is cured in code, not worked around: the
getheaders serve path had poisoned a canonical block's index entry with a
permanent FAILED mask over a torn local Equihash-solution copy, and nothing
could clear it (the storm was 18k WARN lines/5min instead of one blocker).
Now: mark failures raise ONE typed blocker and back off; a FAILED mask
clears if and only if the unchanged PoW+Equihash validator passes with
repairable-class evidence. Remaining named follow-up: quarantine/refetch of
a poisoned `blocks`-table row itself (`db_block_delete` is built and
unwired; needs copy-proof) — see the Wave N backlog memory.

The final pin before the pass — H\* stuck at 3,140,115 on ~1,015
`tip_finalize_log` rowless holes — was cured by `3b512149f`: the boundary
utxo_root persist ran an own-BEGIN inside the stage batch transaction
(nested-BEGIN, failed at 100% of 100-height boundaries post-flip = the WARN
storm) and the hash-bound hole backfill was reachable only for coin tears.
The in-tx persist + the no-tear backfill gate (bounded by the coins frontier)
deployed, self-healed the live datadir on first boot with zero persist
failures, and H\* leapt to the frontier. `tip_finalize.rewind_churn`,
`recovery_coordinator.no_applicable_rung`, and the sticky_escalator family all
cleared themselves.

What is true now (each copy-proven before live):
- **Consensus-bundle install: TERMINAL ADMITTED-AND-ACTIVATED** on this datadir
  (checkpoint-content authority). The 2026-07-17 chain-binding refusals are
  historical; recover via git if needed.
- **Kernel store = `consensus.db`** (A3+A4 flip, merged + deployed): the
  reducer kernel owns its own SQLite file; `progress.kv` holds only the
  address_index/txindex projections via `projection_store`. Migration is
  idempotent/crash-safe at boot; fold verified climbing on consensus.db live.
  Pre-flip cold backup: `~/.zclassic-c23-serve1-BACKUP-preflip` (delete after
  soak). Revert = old candidate binary + that backup.
- **Headers**: full 3.19M-header chain imported (`--importblockindex
  ~/.zclassic <datadir>/node.db` — 2nd arg is the node.db PATH). The P2P
  continuation wedge that starved headers is fixed (loud counted suppression in
  `push_getheaders_from`, `health.headers` counters) — see
  memory `header-continuation-wedge-2026-07-19`.
- **Crash loop CURED**: supervisor tick-runner (no child tick on the sweep
  thread) + E3 sapling-persist deferral (no nested BEGIN abort-loop) both
  live; backstop restarts stopped. Remaining transient wedge blockers
  (`supervisor.tick_runner_wedged`, `worker.stall.op.projection_backfill`)
  are contained signals, largely retired by the consensus.db flip.
- **Landed since (same day):** fast-restart trust-flat skip REMOVED (the
  silent best-header cap class is dead; the forward pass costs ~861ms),
  projection-store corruption quarantine gate + consensus.db downgrade boot
  refusal, loud counted suppression on every remaining silent header/net drop
  path, store-location truth sweep across docs/tools, lint critical path
  cached (17s → <1s warm), janitor timers installed (22GB reclaimed).
- Remaining follow-ups: spurious per-boot txn in `consensus_db_finalize_flip`;
  CLI wall-clock deadlines (a flagless `status` against a dead RPC port spun
  3 days at 100% CPU); graceful stop of a folding node measured ~11 min
  (restart flows must verify MainPID changed); post-soak cleanup of
  `~/.zclassic-c23-serve1-BACKUP-preflip` + `-COPY-headerfix` (28GB) and old
  candidate dirs.

---

## 0. Prior state (2026-07-17 night) — Bundle EXPORTED + VERIFIED; both copy-install routes REFUSED at chain-binding; mint3 is the reliable path; big merge wave on main

**STATE AT SHUTDOWN: the live node is still wedged; the cure is NOT yet
installed.** The complete bundle is exported + independently replay-verified
(durable, below), but BOTH attempts to `-install-consensus-bundle` onto a
copy of the live datadir **REFUSED at the chain-binding precondition**:

> `REFUSED: -install-consensus-bundle: selected-chain binding failed (the
> bundle's height/hash is not on this node's validated header chain, or the
> node is not the open singleton): chain evidence refused: chain binding:
> selected frontier changed or is not durable`
> (`config/src/boot_install_consensus_bundle.c:48` icb_refuse)

**THE OPEN PROBLEM (start here):** installing onto a `cp -a` copy of the LIVE
(actively-writing) datadir does not give the installer a *durable,
singleton-selected* header chain binding the checkpoint height 3,056,758 —
even after relinking headers with `--importblockindex ~/.zclassic`. The copy
arrives chain-detached ("selected frontier changed or is not durable").
Likely fixes to investigate: (a) install onto a datadir whose chain was built
by a normal validated sync rather than a live-copy relink — **mint3's
from-genesis datadir is exactly that** (fully validated, durable chain, no
torn-copy issue; `mint3` is `~/.zclassic-c23-mint3` on disk, a legacy
birth-order name distinct from `~/.zclassic-c23-mint` — kept literal here
because it names a real artifact, not minted fresh); or (b) make the
chain-binding evidence gate accept a
relinked-but-consistent chain. NOTE: the new **checkpoint-content install
authority** (landed on main this session) sits DOWNSTREAM of this chain-
binding gate, so rebuilding the binary alone will NOT clear this specific
refusal — the chain-binding precondition on the target datadir is the thing
to solve.

**THE RELIABLE PATH: let mint3 finish.** The `mint3` from-genesis fold
(~h=2.3M/3.06M at shutdown, PID under `curebin-7d4f346`, ~40–50 blk/s) is an
INDEPENDENT complete self-derived cure source with a validated durable chain.
When it reaches the checkpoint it produces the complete transparent+shielded
state directly (and/or is the clean target to `-install-consensus-bundle`
onto). Do not restart/relabel it. Check `mint-progress.log` in its datadir.

**Both copy-install linger units were STOPPED at shutdown** (they were on a
failed/slow path competing with mint3 for CPU): `zcl-cure-torn-repair`
(REFUSED, above) and `zcl-cure-step4b` (fresh datadir, still stuck in the
slow band-path header import after ~90 min — a separate defect: a fresh
empty datadir's `--importblockindex` should take the bulk path, not the
incremental band service). Their verdict logs are preserved:
`~/.zclassic-c23-COPY-20260717110600-cure/repair-verdict.log` and
`~/.zclassic-c23-COPY-20260717202255-freshcure/step4-verdict.log`. The
self-driving driver scripts are in `~/.local/lib/zcl-cure/` — re-launch with
`systemd-run --user --unit=<name> <script>` (linger is on) once you have a
datadir with a durable validated chain to target.

**Resilience fix banked (the night's most important lesson):** critical
long-running pipelines MUST run as systemd `--user` linger units, self-driving
to a terminal verdict written into the target datadir — NOT session-tied
background tasks. A mid-run token-limit black hole on 2026-07-17 left a cure
failure unhandled for ~8 hours; linger units make that unreachable.

**What is DONE and durable (cannot un-happen):**
- The complete consensus-state bundle is **exported and independently
  replay-verified**: `~/.zclassic-c23-COPY-20260717-fastcure/consensus-state-bundle-3056758.sqlite`
  (513 MB; coins=1,354,769 == compiled checkpoint, anchors=631,645,
  nullifiers=1,495,126; digest `eb3bb23f…cadf5d2`), proven by
  **checkpoint-content proof** (coins SHA3 reproduces the compiled checkpoint
  + Sapling tip frontier Pedersen-roots to the PoW header's
  `hashFinalSaplingRoot`). The replay receipt is written beside it
  (`consensus_state_replay_receipt.v1`).
- Preserved binary that produced + verifies it: `fastcure4-9e444c230b2840cc`
  under `~/.local/lib/zclassic23-mint-candidates/`. **Binary-binding law:
  run install/verify/copy-prove under ONE preserved binary; never rebuild a
  preserved binary.**
- The self-driving cure scripts live in `~/.local/lib/zcl-cure/`
  (`cure-torncopy-repair.sh`, `cure-step4-driver.sh`) — durable path, not the
  session scratchpad.
- **mint3** from-genesis fold (an INDEPENDENT complete cure source) continues
  under `curebin-7d4f346`, ~h=2.3M/3.06M — arrives regardless if every
  install route stalls. Do not restart/relabel it.

**The wall class is now permanently closed (landed on main this session):**
`-install-consensus-bundle` gained a second **CHECKPOINT_CONTENT ACTIVATE
authority** (`config/src/consensus_state_snapshot_install_checkpoint_authority.c`):
a bundle activates when its coins reproduce the compiled SHA3 checkpoint AND
its Sapling frontier roots to the local validated header — **receipt-free**.
Every all-day wall was a terminal verb (`-export`/`-verify`) not being
exempted from a producer-lane boot serving gate; those exemptions are all in
now. Torn-copy lesson banked: **never `cp -a` a live *writing* datadir** for
an install proof (the copy is chain-detached); relink headers first via
`--importblockindex ~/.zclassic`.

**Landed + pushed to `origin/main` this session** (all gate-verified with
`tools/scripts/gate-and-report.sh`, adversarially re-verified, several
datadir-pollution-guarded). `main` is in sync with origin, working tree clean:
- The native command registry (`zclassic23 <command>`) is the sole agent
  interface.
- **Network omniscience**: NET-1 (anchors.dat + 8-wide parallel dialer +
  feelers + one peer-floor constant), NET-2 (modal-tip truth + banked peer
  reputation + session/fork ledgers), NET-3 (range-parallel header sync),
  NET-4 (`dumpstate network`), node **census** (IP/MagicBean-UA/version, in
  `peers_projection.db`), **topology** graph (`topology.db`), and a pedantic
  wire-parser audit (`docs/NET_PARSER_AUDIT.md`).
- **Chain index**: `op_return_index` (ZNAM/ZSLP/ZANC/unknown catalog),
  opt-in `address_index` (script→appearances), `dumpstate rom` (the L0–L3
  trust-machine catalog + `docs/ROM.md`) — all rebuildable projections with
  their own cursors + SHA3 digests.
- **ROM P2P delivery (seed+policy side)**: `rom_seed` free-tier chunked
  serving of the bundle (per-peer/global rate caps, corrupt-artifact
  rejection at registration, `directory.json` advertisement),
  `rom_seed_policy` + `artifact_serve_log` ledger + `ops.debug.rom_seed.*`
  commands + `docs/ROM_DELIVERY.md`.
- **W4 integrity**: sealed-history segment sealer wired (env-gated), per-row
  stage-log **integrity tags** (MISMATCH lowers H\*, NULL trusted-but-logged),
  checkpoint content re-check + `-verify-rom` verb, `utxo_recovery.rewind_overshoot`
  typed blocker, export-heartbeat observability.
- **Sync foundation**: checkpoint-content install authority (above),
  **stall-taxonomy** (closed 4 silent-hold sites across the 8 stages incl.
  the tip_finalize class that pinned H\* for 3h on 2026-07-02), **kill-9
  resume-determinism** test harness, S8 band-hole cadence, parse-cache
  poison fix.

**IN-FLIGHT and NOT landed — session-tied, likely died on shutdown; RESUME
these** (each was building in an isolated worktree with an adversarial
verifier; re-drive from the workflow scripts under
`~/.claude/projects/<session>/workflows/scripts/` with `resumeFromRunId`, or
finish the lane branch directly):
- `lane/rom-fetch2` — the ROM **fetch** side (multi-seeder verify-by-content
  download + resume). The seed side is on main; this completes <60s assisted
  sync. NOT done.
- `lane/index-tx2` — `txindex` as a first-class projection (mirror
  `address_index`). NOT done.
- `lane/net-operator-surface2` — `net census/node/versions/graph` commands +
  `/network` explorer page over the REAL census/topology schemas (a prior
  attempt coded against imagined columns and was rejected). NOT done.

**KNOWN OPEN DEFECT (found + pinned this session, boot-path fix DEFERRED):**
an **unclean restart** (kill-9/OOM, no clean-shutdown marker) runs
`chain_restore_rebuild_active_chain` which walks tip→genesis **O(chain-height)**,
not O(delta) — a real slow-boot defect (a dirty restart re-reads the whole
chain). Proven by `test_rebuild_active_chain_is_o_chain_not_delta` and
written up in `docs/AGENT_TRAPS.md` with the fix design (engage the
trust-index fastpath around `config/src/boot.c` ~2744, mirroring the existing
`finalize_verified` guard). The fix is consensus-adjacent — copy-prove it on a
real datadir before landing. The audit infrastructure (`lib/util/boot_scan.c`
counters) is on main.

**THE plan of record + program context is unchanged below and in
`docs/work/FORWARD_PLAN.md`.** The architecture verdict + best-possible-node
plan the owner approved this session is
`~/.claude/plans/zclassic23-needs-to-be-starry-wirth.md` (the L0–L3 machine,
Ten Laws, the two-path sync spine, and the shielded-ROM keystone bake P4).

---

## 1. The live node — the wedge

The public daily-driver (`~/.zclassic-c23` : 18232) is **wedged at
H\*=3,176,325** (`coins_applied` 3,176,326), blocker
`utxo_apply.anchor_backfill_gap`: `sapling_anchors` is empty below the
reducer cursor, so unknown Sprout/Sapling roots FAIL CLOSED and hold H\*.
The network transport itself is alive: 2026-07-13 read-only audits observed
headers and block requests/receives increasing while peer count fluctuated
between three and six, and the served
hash at h=3,176,325 exactly matched zclassicd. The deployed build is old
(`3b0de63b0`) and can still promote `download_queue_starved` above the causal
history gap; current source suppresses that downstream condition and selects
the permanent anchor/nullifier blocker first, with end-to-end regression
coverage. Do not mistake the old status ranking for the cause of the freeze.
Soak (`:18242`) and dev (`:18252`) run newer builds near tip and are NOT
wedged. **Unwedging the live node via the sovereign cure is the #1 job.**
Verify with `zclassic23 status` / `zclassic23 dumpstate reducer_frontier` before
acting — this file is a snapshot, not a live read. Root cause + mechanism:
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) and
memory `project_live_wedge_anchor_frontier_rootcause_2026-07-12`. The
**sovereign cure** is the self-verified UTXO/anchor rebuild that folds real
block bodies forward from the in-binary SHA3/PoW checkpoint and deletes the
borrowed `zclassicd`-minted seed path (see `CLAUDE.md` "Tenacity &
recovery") — this is the path that actually passed the wedge, per §0-LATEST.
An alternate, faster **operational** cure (the complete-shielded-history
import, `release_assisted` trust, not sovereign,
[`docs/work/fast-sync-to-tip-plan-2026-07-16.md`](work/fast-sync-to-tip-plan-2026-07-16.md))
was proven to clear this exact wedge on a datadir copy but was never cut over
live; its owner-gated runbook was removed in the 2026-07-19 doc-rot sweep
(recover with `git log --follow -- docs/work/canonical-cutover-runbook-2026-07-16.md`
if the import path is ever revived as the active cure track).

## 2. Historical detail

Dated producer/cure narratives (USS v3, legacy snapshot producers, the
2026-07-11..16 cure runs) are removed from the tree, not archived in-repo.
Recover any of them with `git log --follow -- docs/HANDOFF.md` +
`git show <rev>:docs/HANDOFF.md`. The current live state is §0; the
current protected producer (mint3) is described there.

## 3. Agent interface

The native typed command registry (`zclassic23 <command>`) is the sole agent
interface. Node RPC transport lives under `app/controllers/`, and command
contracts carry native paths plus input/output schemas for discovery.

## 4. MVP status

**MRS 4/8** (do not bump without proof): C1 install, C2 Tor onion <60s, C4
receive shielded, C7 kill-9 recovery pass local operator proof. C3
cold-start, C5 file market, C6 7-day soak, C8 exact parity are gated on the
sovereign cure + accumulated soak time; current formal C6 judge is
`NOT_MET` (`operator_intervention_detected_x2`). Bar: [`docs/MVP.md`](MVP.md).
Live scorer: `tools/mvp_gate.sh`.

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

The live datadir runs `-load-snapshot-at-own-height` re-seeding 3,156,809 and
folding forward, so each restart takes about 13 minutes to replay back to the
same permanent shielded-history gap. This is retry convergence, not a cure or
self-healing success. `zclassicd`
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
- [`docs/work/fast-sync-to-tip-plan-2026-07-16.md`](work/fast-sync-to-tip-plan-2026-07-16.md) — the operational import-path cure design (the alternate cure in §1; not the path that passed the wedge).
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
