> **Read this file first for current live-node state.** Session-by-session
> detail lives in `docs/work/archive/` (historical logs, not standing facts;
> full pre-slim history: [`HANDOFF-HISTORY-2026-07.md`](work/archive/HANDOFF-HISTORY-2026-07.md)).
>
> **⚡ Incoming developer, start here:** this file, then
> [`work/archive/HANDOFF-2026-07-12-evening.md`](work/archive/HANDOFF-2026-07-12-evening.md)
> for the original un-wedge recipe. The live facts and corrected producer
> evidence below supersede that dated recipe where they differ.
>
> **Durable hierarchy:** [`work/SOVEREIGN-NETWORK-ROADMAP.md`](work/SOVEREIGN-NETWORK-ROADMAP.md)
> preserves the Phase 0–6 program and promotion gates: stable sovereign sync
> first, transactional C23 hot swap second, sandboxed publishing third. It does
> not displace the immediate canonical cure below.

# HANDOFF — current state (2026-07-14)

## 0-NEW. Incoming-developer handoff (current state)

**`origin/main` is green and fully pushed** (`make build-only` + `make lint` clean). Foundation (log→reducer→projection→self-verify) is sound — finishing moves, not a rewrite. Physics floor (measured, Ryzen 9 7950X3D): full-verify ~60 s; today's fold sits ~30× above floor.

**THE mission: get the node ALWAYS SYNCING FAST.** One lever dominates right now — fold speed.

**Measured truth (2026-07-14, `~/.zclassic-c23-mint-receipt/mint-progress.log`).** The cure fold is **crypto-bound, not IO-bound and NOT "crypto-skipped"** (an earlier note said crypto is skipped below the anchor — that is WRONG). Per-block stage times at h~300k: `vh` (Equihash header validate) 5–33 ms + `pv` (Groth16 proof validate) 3–18 ms dominate; `sv` (ECDSA script, the only thing skipped below anchor) ~0.2 ms; `ua`/`tf` small. Net ≈ **22–35 blk/s → ~22 h ETA to the anchor (h=3,056,758)**. Physics floor is ~1000–1700 blk/s.

**A1 parallel validation is ALREADY MERGED to main** (`c6c56a3b9`, 2026-07-13 20:42 UTC) — it parallelizes script+proof (`sv`+`pv`) across cores. The running producer binary (candidate `34d78c95`, built 23:25) **predates A1 and folds serially**, so it gets none of the benefit. `wf/parallel-validation` (branch, unmerged) is the OLD drifted lane — **do NOT reuse it**; the real A1 landed via `wf/a1-parallel-validate`.

**THE next action to make sync fast (owner pre-authorized; copy-prove first):**
1. Build current `main` → `build/bin/zclassic23` (has A1).
2. Copy-prove: fold a datadir **COPY** with the A1 binary, measure blk/s vs the ~30 the producer does at the same height range; confirm H\* advances + no parity break.
3. If materially faster → restart `zclassic23-mint-receipt` on the A1 binary (fresh datadir + fresh receipt session; preserve the old datadir). Recipe: `--importblockindex $HOME/.zclassic <dir>/node.db` then start the unit. Never touch the two older producers (`~/.zclassic-c23-anchor-mint`, `-mint-fast`).
4. Then attack `vh` (header validate) — the branch `wip/proof-lookahead-header-tuning` holds unmerged fold-speed code for exactly this (`proof_validate_lookahead.{c,h}`, `validate_headers_tuning.c`).

**Other unmerged branches worth knowing** (worktrees pruned 2026-07-14, branches preserved — `git worktree add` to resume): `wf/groth16-beat-rust` (CONSENSUS-CRITICAL: partial BLS12-381 opt; do NOT merge without the differential parity oracle proving bit-identical verdicts incl the non-canonical-infinity quirk), `wf/dx-navigator-callgraph` (`code callers/callees/trace`), `wf/wallet-encryption-default`, `wf/c1-standing-exporter`.

**Live node:** public canonical remains wedged at H\*=3,176,325 on `utxo_apply.anchor_backfill_gap`; the sovereign cure (above) unwedges it. Verify live before trusting this file: `zcl_status`, then `zcl_state subsystem=reducer_frontier`.

**Repo hygiene (2026-07-14):** git worktrees pruned 44 → 2 (`main` + fuzz-service checkout), ~1.4 GB freed; all uncommitted WIP preserved on `wip/*` branches. A live `dev-watch` watcher maintains a small (~16 MB) working set under `.claude/worktrees/` — harmless.

**Traps:** the shared `core.hooksPath` flaps to absolute under worktree agents (the gate self-heals; else per-process `GIT_CONFIG` override, never `make install-hooks`); the `CODEBASE_MAP.md` `test_groups:` count conflicts on parallel lanes (resolve via `bash tools/scripts/check_doc_counts.sh`); never run two concurrent `make lint`/builds in one checkout; the pre-push hook SIGPIPEs on stdout (pre-push-ci to a file, then `--no-verify`).

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
Verify with `zcl_status` / `zcl_state subsystem=reducer_frontier` before
acting — this file is a snapshot, not a live read. Root cause + mechanism:
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) and
memory `project_live_wedge_anchor_frontier_rootcause_2026-07-12`. The
**sovereign cure** is the self-verified UTXO/anchor rebuild that folds real
block bodies forward from the in-binary SHA3/PoW checkpoint and deletes the
borrowed `zclassicd`-minted seed path (see `CLAUDE.md` "Tenacity &
recovery").

## 2. In flight — two cure producers; neither is deployable yet

2026-07-12 (`origin/main` `a7d7baf6a`) landed the fast dev loop (`make ff` /
`code map` / `code tests` / watcher `MODE=verify`), the palace legibility
layer, and an important legacy snapshot producer: one canonical
`coins_kv_snapshot_write()` (version-by-data, byte-identical, pinned SHA3
goldens) plus `snapshot_shielded_collect_from_db()` (Sapling+Sprout frontier
+ nullifiers), wired into `boot_mint_anchor.c`, and
`tools/snapshot_from_coinskv.c --shielded` (fast producer path, no fold).

**Correction:** USS v3 is not and cannot become the sovereign cure. It carries
the current Sapling/Sprout frontiers and supplied nullifiers, but omits the
historical anchor tables and does not prove complete nullifier history. USS
v1/v2/v3 are frozen legacy read-only encodings. The accelerated producer's
`progress.kv` retains the historical rows that a new
`zcl.consensus_state_bundle.v1` exporter must preserve. A 2026-07-13 schema
audit found that the preserved old producer has no `sprout_anchors`,
`sapling_anchors`, or `anchor_state` tables at all: preserve it as independent
transparent/nullifier fold evidence, but never call it a complete shielded
bundle source.

**Blocker found driving the copy-prove:** a soak-datadir copy has the
Sapling frontier but not a usable current Sprout frontier (soak was itself
seeded coins-only) — the legacy collector correctly refuses that missing
frontier rather than fabricate one, but it does not prove full anchor/nullifier
history. Only a from-genesis `-mint-anchor` fold can supply the complete
historical source, and the new exporter/proof receipt must demonstrate that it
actually did. The original producer at
`~/.zclassic-c23-anchor-mint` (PID 3329835 at the 2026-07-13 observation)
is still running and must not be stopped, rewritten, or restarted. Its old
binary predates the current export work; if it finishes, preserve the entire
datadir as immutable producer input. Its mapped executable is a deleted inode;
the read-only SHA3-256 of that actual mapping is
`0223e24712974fb5b96ca554d6139153bd632f07c75c8ccd3cc82b8e68a9f65f`.
That is forensic identity, not complete source/toolchain provenance. Do not
reduce the datadir to a USS v3 artifact.
Full background:
[`docs/work/archive/SESSION-HANDOFF-2026-07-12.md`](work/archive/SESSION-HANDOFF-2026-07-12.md).
At the latest read-only audit it was durably through h=1,050,000 at about
1.499 blocks/s. A stale main `progress.kv` mtime was a false stall signal: its
WAL was active 16 seconds before capture and the durable cursor advanced by
1,000 blocks. Never follow an `anchorstatus` restart suggestion based only on
the main DB mtime; use WAL activity and cursor/sample cadence. Current source
does exactly that and never recommends restart from offline file-age evidence;
the real producer reported `mint_in_progress_recent` with a cadence-aware
1,334-second stale deadline after the fix.

An isolated accelerated producer is also running as the low-priority transient
unit `zclassic23-mint-fast-v2.service` over
`~/.zclassic-c23-mint-fast`. It uses immutable diagnostic source identity
`882a8bcd3635bd40d4cddcc0352ff2fda03790eedd33fd40e52d1b2da6a09c88`
and binary SHA3
`69ac57adfebc3cb06a5b82a48b193c8e217fec17dba5c24be9060dbe3f1322a1`.
This is a dirty, offline development candidate, never a release or publication
artifact. The first attempt exposed a real legacy loader bug: file-0
`CDiskBlockIndex.nDataPos=1711` for h=1 was incorrectly translated to 3414,
so the reducer deserialized unrelated bytes. The loader now preserves exact
payload offsets and has a LevelDB round-trip regression test. On the real
history corpus the corrected producer durably advanced all state-applying
stages through h=67,999, with headers through h=69,999 (where the old run
failed at h=1), and retained bounded cyclic stage
skew. Its latest durable samples measured 12.048 blocks/s and an early-chain
ETA of 251,885 seconds (~2.92 days), about 8x the preserved producer's current
window; that is a provisional,
contention-limited observation, not an SLO claim. It runs with runtime CPU/IO
weight 50 (still below the preserved producer's default weight 100) and never
restarted for that adjustment. Profiling found its main thread waiting on
ext4 journal commits in 194/200 samples while the host retained CPU headroom;
the stage runner already gives each drain one outer transaction, so the next
performance pass must profile the remaining node.db/projection/journal commit
sites and batch only the measured missing boundary, without weakening SQLite
durability. Use `anchorstatus` durable cursors/rate plus WAL activity, not
process uptime or startup logs.

**2026-07-13 implementation slice:** legacy USS parsing now verifies an exact
layout and independently recomputes the transparent root/count/supply; the
manual-loader predicates bind those values plus height/hash to the compiled
checkpoint. A new `zcl.consensus_state_bundle.v1` validation core checks an
external candidate but is intentionally read-only: every valid candidate is
reported `VERIFIED_CONTAINED`, returns false, and publishes nothing. It has no
boot caller. Admission binds the declared current Sprout/Sapling roots and
their actual latest source heights, but local-header Sapling proof remains a
required protected-adapter gate. Validator, tests, and exporter use one shared
canonical codec with pinned SHA3 vectors; unfinished SQLite sidecars and
oversized rows are refused.

The contained full-history exporter now takes one locked frozen transaction
over a quiesced process-owned `progress.kv`, refuses an active RAM overlay,
requires exact computed H* plus convention-aware durable H/hash,
`coins_applied=H+1`, genesis-complete anchor/nullifier cursors, continuous
successful reducer rows, and a source receipt bound to the actual running
executable. It copies all coins, historical Sprout/Sapling anchors, and
pool-qualified nullifiers; writes a canonical eight-row proof summary and the
source receipt; independently reopens/validates; and uses FULL-durable
no-replace output. At the compiled checkpoint, hash/root/count/supply must
match exactly. Source-tree/toolchain receipt fields are honestly producer
claims, not independent rebuild proof. Admission now retains an `O_NOFOLLOW`
descriptor, opens SQLite through that identity, whole-file-hashes before/after
semantic validation, and rejects any non-canonical schema object or column.
Exporter publication is now pinned to a duplicated directory descriptor plus
one normalized filename component. It builds in an anonymous `O_TMPFILE`
through an exporter-private fd-only SQLite VFS, validates that exact inode,
then atomically hard-links the inode into the pinned directory and fsyncs the
directory. There is no mutable staging pathname to swap or clean up; parent
rename/symlink replacement and late final-name creation cannot redirect or
replace the artifact. There is still no producer-end caller or node state
publisher.
Both already-running producers predate this receipt/export contract, so neither
can honestly pass it as-is. Never inject a receipt into their live databases.
Either prove a protected offline ratification against the preserved mapped
executable plus immutable source/toolchain evidence on a completed copy, or run
a newer receipt-owning producer; absence of that proof remains a hard refusal.

The process-singleton selected-chain binder starts only from the opaque
descriptor/whole-file validation receipt and requires stable before/after durable
frontiers, exact selected H/hash and header-pass evidence, failure-free
ancestry, and a Sapling root matching both its sparse source height and bundle
H. It re-samples ancestor predicates and revalidates the exact artifact around
capture. Its lane tag is descriptive, not authority. The opaque result can
become stale after return and cannot activate anything.

Shielded completeness is now fail-closed across the audited recovery paths.
UTXO apply holds every Sapling spend/Sprout JoinSplit before coin writes while
anchor or nullifier history is incomplete. Nullifier backfill persists an exact
selected-chain target/tip receipt, verifies every body hash and Merkle root, and
performs a final progress→active-chain CAS before publishing explicit zero;
fork-bound rows are discarded and the positive marker remains. Full reindex
keeps both anchor pools and nullifiers positive through exact genesis→target
replay, then publishes all three zeros in one final transaction. General
assisted reset cannot publish zero, and legacy `-refold-staged` is contained at
preflight before any progress/reset write. The datadir PID guard is also now a
process-lifetime `O_NOFOLLOW`/`flock` exclusion rather than PID-text inference.
The reducer's shielded-history retry memo is bound to the exact selected block
hash plus Sprout, Sapling, and nullifier completeness markers. An unchanged
causal gap parks without rereading the body or inflating blocker receipts,
while a history-marker advance invalidates the park. A same-height branch
replacement stays held until both script and proof receipts bind the new hash;
missing or foreign receipt hashes are named dependencies and cannot authorize
body reads or coin writes. The canonical exporter likewise requires every
proof receipt to join the exact admitted header hash.

The C23 verify loop also became content-fingerprint-bound and fail-fast: a
controlled compiler failure fell from 185.94 seconds/18 repeated failures to
1.54 seconds/one failure, while a stable all-noop campaign took 1.07 seconds.
Source drift supersedes the campaign instead of mislabelling it. `--status`
now refuses stale pre-binding artifacts in about 0.09 seconds with an explicit
non-evaluable reason instead of replaying old failures as current.
The source-identity collector now batches metadata and hashing in bounded
128-path chunks while preserving the exact v1 preimage and a portable fallback:
on the current 219-path overlay it fell from 0.76–0.78 seconds to 0.07–0.08
seconds. A 162-path boundary fixture proves fast/portable byte identity,
supersession, deletion/mode/symlink handling, and hidden-index-bit refusal.

Canonical was not restarted, deployed, or mutated and remains at
H\*=3,176,325. Both cure producers remained alive and untouched at the latest
observation: the preserved producer PID 3329835 and low-priority fast producer
PID 2868204 (`zclassic23-mint-fast-v2.service`).

**Next job:** let both producers continue. Add producer-start/end ownership for
the durable source receipt and exporter, plus a separately reviewed offline
ratification path for the already-running immutable fast producer (or prove it
cannot be ratified and start a receipt-owning producer without deleting either
existing input). Combine contained artifact validation with selected-chain/
trust/authority evidence under one publication CAS. Then finish a boot-only,
`FULL`-durability candidate-store
publisher with strict no-quarantine reopen and atomic old/new file exchange,
projection reconciliation, a physically restorable prior generation, and an
ENOSPC/I/O/kill/reopen campaign.
Only after those pass may manual load/refold route through it. Copy-prove on a
canonical datadir clone, gate on H\* climbing past 3,176,326 plus exact parity
and warm/kill-9 restart, then seek owner authorization for live deployment.
The legacy manual loader's payload-bound marker proves retry convergence, not
all-or-nothing publication. Runtime peer-snapshot activation remains contained.
Do NOT delete the borrowed seed / flip `-refold-from-anchor` to default in
the same move — that is a separate, later, owner-gated step.

## 3. Zero-MCP program (secondary track)

Owner directive: delete the MCP server entirely — the native CLI
(`zclassic23 <command>`) becomes the ONLY agent interface. W0/W1-A/W1-B/C
(hot-swap re-targeted onto the command registry, `88b4e1030`) are DONE.
Next: W2 (~47 remaining call sites) then W3 (delete the MCP server), per
[`docs/work/MCP-REMOVAL-WORKLIST.md`](work/MCP-REMOVAL-WORKLIST.md) (the
114-site inventory). W3 is blocked on 3 prereqs — memory
`project_zero_mcp_w3_prereqs_2026-07-12`.

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
`zclassic23 agent`, REST `/api/v1/agent`, and MCP `zcl_operator_summary`
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
Ports/runbook: `docs/SYNC.md`; re-pull live state (`zcl_status`) before acting.

**Standing method:** `make deploy` rm's the binary first (a stale binary was
a real multi-day outage; `deploy_verify.sh` confirms `build_commit`).
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
- [`docs/work/ROADMAPS.md`](work/ROADMAPS.md) — live vs superseded roadmaps.
- [`docs/AGENT_TRAPS.md`](AGENT_TRAPS.md) — looks-broken-but-isn't; read before "fixing" anything.
- [`docs/MVP.md`](MVP.md) — the v1 acceptance bar (8 criteria).
- [`docs/work/archive/`](work/archive/) — dated handoffs/audits/superseded roadmaps (history only).

## 7. Verify before you trust this file

A map, not the territory. Re-read the cited file:line before building on any
claim — trust the code you read THIS minute over this file. Architecture
reference (off the v1 path): [`docs/FRAMEWORK.md`](FRAMEWORK.md) +
[`docs/REFACTOR_STATUS.md`](REFACTOR_STATUS.md).
